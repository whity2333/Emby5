/*
 * evo_udf.c — Blu-ray disc image (.iso) access through libudfread.
 *
 * The player hands us an avio URL for an .iso. avformat rejects it
 * (AVERROR_INVALIDDATA) because the file is a UDF filesystem image, not a
 * container. libudfread can read that filesystem, but its own file API wants a
 * path. `udfread_open_input()` takes block callbacks instead, so this file
 * supplies them on top of the same avio connection: a seek becomes an HTTP
 * range request, which is exactly what a disc image needs.
 *
 * Two things are built here:
 *
 *   udf_walk()        mount the image, find BDMV/STREAM, pick a title
 *   evo_udf_open_window() expose that title as a small seekable AVIOContext
 *
 * The demuxer then sees an ordinary mpegts stream and none of the playback
 * path changes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "evo_udf.h"
#include "evo_boot_log.h"

#include "blockinput.h"
#include "udfread.h"

#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* How many entries of each kind to log before summarising the rest. */
#define UDF_LOG_LIMIT 24

/* Read-ahead for the windowed stream. A disc title is read sequentially at
 * 60-100 Mbit/s; 256 KiB keeps the range requests large enough to amortise. */
#define WIN_BUFFER (256 * 1024)

/* ------------------------------------------------------------------ */
/* udfread block input backed by avio                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    udfread_block_input bi;      /* must be first: the callbacks cast back */
    AVIOContext        *io;
    int64_t             size_bytes;
    pthread_mutex_t     lock;    /* avio is not thread-safe; reads may race */
} avio_input;

static int ai_read(udfread_block_input *in, uint32_t lba, void *buf,
                   uint32_t nblocks, int flags)
{
    avio_input *self = (avio_input *)in;
    const int64_t off  = (int64_t)lba * UDF_BLOCK_SIZE;
    const size_t  want = (size_t)nblocks * UDF_BLOCK_SIZE;
    size_t got = 0;

    (void)flags;

    if (!self->io || want == 0)
        return 0;

    pthread_mutex_lock(&self->lock);
    if (avio_seek(self->io, off, SEEK_SET) >= 0) {
        while (got < want) {
            int n = avio_read(self->io, (unsigned char *)buf + got, (int)(want - got));
            if (n <= 0)
                break;
            got += (size_t)n;
        }
    }
    pthread_mutex_unlock(&self->lock);

    return (int)(got / UDF_BLOCK_SIZE);
}

static uint32_t ai_size(udfread_block_input *in)
{
    avio_input *self = (avio_input *)in;
    int64_t blocks = self->size_bytes / UDF_BLOCK_SIZE;
    return (blocks > 0 && blocks < 0xFFFFFFFFll) ? (uint32_t)blocks : 0;
}

/* Only ever called by udfread_close() after a successful open, or by us when
 * the open failed (open_input does not take ownership on failure). */
static int ai_close(udfread_block_input *in)
{
    avio_input *self = (avio_input *)in;

    if (self->io)
        avio_closep(&self->io);
    pthread_mutex_destroy(&self->lock);
    free(self);
    return 0;
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls)
        return 0;
    for (size_t i = 0; i < lf; i++) {
        char a = s[ls - lf + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return 1;
}

static AVIOContext *open_avio(const char *url, const char *agent, int timeout_ms)
{
    AVDictionary *opts = NULL;
    AVIOContext *io = NULL;
    char t[32];

    /*
     * FFmpeg's rw_timeout is in MICROSECONDS, not milliseconds. Passing the
     * millisecond value straight through makes it a 15 ms timeout, which the
     * proxy fails instantly with "Operation timed out" - the rest of this
     * codebase already writes the microsecond value ("5000000" in
     * evo_stream_io.c), so convert here and keep the caller in ms.
     */
    snprintf(t, sizeof t, "%d", timeout_ms > 0 ? timeout_ms * 1000 : 0);
    av_dict_set(&opts, "rw_timeout", t, 0);
    av_dict_set(&opts, "user_agent", agent, 0);

    if (avio_open2(&io, url, AVIO_FLAG_READ, NULL, &opts) < 0)
        io = NULL;
    av_dict_free(&opts);
    return io;
}

/* Walk one BDMV subdirectory, logging names. */
static void list_dir(udfread *udf, UDFDIR *parent, const char *name, const char *tag)
{
    UDFDIR *d = udfread_opendir_at(parent, name);
    struct udfread_dirent e;
    int n = 0;

    if (!d) {
        evo_boot_log("UDF: %s/%s not present", tag, name);
        return;
    }
    while (udfread_readdir(d, &e)) {
        if (!e.d_name || !strcmp(e.d_name, ".") || !strcmp(e.d_name, ".."))
            continue;
        if (n < UDF_LOG_LIMIT)
            evo_boot_log("UDF: %s/%s/%s", tag, name, e.d_name);
        n++;
    }
    udfread_closedir(d);
    if (n > UDF_LOG_LIMIT)
        evo_boot_log("UDF: %s/%s ... %d entries total", tag, name, n);
}

/*
 * Mount the image, pick the main title. `log_tree` controls how much goes to
 * the log: the playback path asks for the numbers only.
 */
static void title_from_playlists(udfread *udf, const char *clip, evo_udf_title *out);

static int udf_walk(const char *url, int log_tree, evo_udf_title *out_title)
{
    avio_input *in;
    udfread *udf;
    UDFDIR *bdmv, *stream;
    struct udfread_dirent e;
    int n = 0, shown = 0, rc;
    int64_t best_size = -1;
    uint32_t best_lba = 0;
    char best_name[256];

    best_name[0] = 0;

    in = (avio_input *)calloc(1, sizeof *in);
    if (!in)
        return -1;

    in->io = open_avio(url, "Emby5 UDF", 15000);
    if (!in->io) {
        evo_boot_log("UDF: could not open the image");
        free(in);
        return -1;
    }

    in->size_bytes = avio_size(in->io);
    pthread_mutex_init(&in->lock, NULL);
    in->bi.read  = ai_read;
    in->bi.size  = ai_size;
    in->bi.close = ai_close;

    udf = udfread_init();
    if (!udf) {
        ai_close(&in->bi);
        return -1;
    }
    udfread_set_log_level(udf, UDFREAD_LOG_ERROR);

    rc = udfread_open_input(udf, &in->bi);
    if (rc < 0) {
        /* open_input does not take ownership when it fails; udfread_close
         * would not call our close either, so release it here. */
        evo_boot_log("UDF: libudfread could not open the image");
        ai_close(&in->bi);
        udfread_close(udf);
        return -1;
    }

    if (log_tree) {
        const char *vid = udfread_get_volume_id(udf);
        evo_boot_log("UDF: mounted, volume='%s', %lld blocks of %d bytes",
                     vid ? vid : "?",
                     (long long)(in->size_bytes / UDF_BLOCK_SIZE),
                     UDF_BLOCK_SIZE);
    }

    bdmv = udfread_opendir(udf, "BDMV");
    if (!bdmv) {
        evo_boot_log("UDF: no BDMV directory at the image root");
        udfread_close(udf);
        return -1;
    }
    if (log_tree)
        list_dir(udf, bdmv, "PLAYLIST", "BDMV");

    stream = udfread_opendir_at(bdmv, "STREAM");
    if (!stream) {
        evo_boot_log("UDF: no BDMV/STREAM directory");
        udfread_closedir(bdmv);
        udfread_close(udf);
        return -1;
    }

    while (udfread_readdir(stream, &e)) {
        char path[320];
        UDFFILE *f;
        int64_t size;
        uint32_t lba;

        if (!e.d_name || !ends_with_ci(e.d_name, ".m2ts"))
            continue;

        snprintf(path, sizeof path, "BDMV/STREAM/%s", e.d_name);
        f = udfread_file_open(udf, path);
        if (!f) {
            evo_boot_log("UDF: stream %s: could not open", e.d_name);
            continue;
        }
        size = udfread_file_size(f);
        lba  = udfread_file_lba(f, 0);
        udfread_file_close(f);

        n++;
        if (log_tree && shown < UDF_LOG_LIMIT) {
            evo_boot_log("UDF: stream %s  size=%lld  lba=%u  offset=%lld",
                         e.d_name, (long long)size, lba,
                         (long long)lba * UDF_BLOCK_SIZE);
            shown++;
        }

        if (size > best_size) {
            best_size = size;
            best_lba  = lba;
            snprintf(best_name, sizeof best_name, "%s", e.d_name);
        }
    }

    udfread_closedir(stream);
    udfread_closedir(bdmv);

    if (n == 0) {
        evo_boot_log("UDF: BDMV/STREAM has no .m2ts files");
        udfread_close(udf);
        return 0;
    }

    if (log_tree) {
        evo_boot_log("UDF: %d stream file(s); largest is %s  size=%lld  offset=%lld  "
                     "(heuristic: main feature)",
                     n, best_name, (long long)best_size,
                     (long long)best_lba * UDF_BLOCK_SIZE);
    }

    /* The playlist has to be read before the volume is closed. */
    if (out_title) {
        char clip[8];

        memset(out_title, 0, sizeof *out_title);
        out_title->offset = (int64_t)best_lba * UDF_BLOCK_SIZE;
        out_title->size   = best_size;
        out_title->duration_45k = -1;

        snprintf(clip, sizeof clip, "%.5s", best_name);
        title_from_playlists(udf, clip, out_title);

        if (log_tree) {
            evo_boot_log("UDF: title duration %.3f s, %d stream language(s) from the playlist",
                         out_title->duration_45k > 0
                             ? (double)out_title->duration_45k / 45000.0 : 0.0,
                         out_title->ntracks);
            for (int i = 0; i < out_title->ntracks; i++)
                evo_boot_log("UDF:   playlist stream pid=%u lang=%s",
                             out_title->tracks[i].pid, out_title->tracks[i].lang);
        }
    }

    /* udfread_close() calls our close(), which frees `in`. */
    udfread_close(udf);
    return n;
}

/* ------------------------------------------------------------------ */
/* windowed AVIO: [offset, offset+size) of the image, as a plain file   */
/* ------------------------------------------------------------------ */

typedef struct {
    AVIOContext    *under;
    int64_t         base;
    int64_t         size;
    int64_t         pos;
    pthread_mutex_t lock;
} win_io;

static int win_read(void *opaque, uint8_t *buf, int n)
{
    win_io *w = (win_io *)opaque;
    int64_t left = w->size - w->pos;
    int r;

    if (left <= 0)
        return AVERROR_EOF;
    if ((int64_t)n > left)
        n = (int)left;

    pthread_mutex_lock(&w->lock);
    r = avio_read(w->under, buf, n);
    if (r > 0)
        w->pos += r;
    pthread_mutex_unlock(&w->lock);

    return r;
}

static int64_t win_seek(void *opaque, int64_t off, int whence)
{
    win_io *w = (win_io *)opaque;
    int64_t target, r;

    switch (whence) {
    case AVSEEK_SIZE:
        return w->size;
    case SEEK_SET:
        target = off;
        break;
    case SEEK_CUR:
        target = w->pos + off;
        break;
    case SEEK_END:
        target = w->size + off;
        break;
    default:
        return AVERROR(EINVAL);
    }

    if (target < 0)
        target = 0;
    if (target > w->size)
        target = w->size;

    pthread_mutex_lock(&w->lock);
    r = avio_seek(w->under, w->base + target, SEEK_SET);
    if (r >= 0) {
        w->pos = target;
        r = target;
    }
    pthread_mutex_unlock(&w->lock);

    /*
     * The player's seek reported success while the picture never moved, so
     * either this is not being called or the reposition underneath fails.
     * Both are visible here.
     */
    evo_boot_log("UDF: win_seek(whence=%d target=%lld) -> %lld (underlying seekable=%d)",
                 whence, (long long)target, (long long)r,
                 w->under ? w->under->seekable : -1);

    return r;
}

/* ------------------------------------------------------------------ */
/* playlist (.mpls): the title's real duration                         */
/* ------------------------------------------------------------------ */

/*
 * MPEG-TS carries no index, so av_seek_frame() on it estimates a byte
 * position as (target / duration) * size. With no duration the estimate is
 * always 0: the seek reports success and playback restarts at the top.
 *
 * The duration a Blu-ray image does have is in its playlist: every playitem
 * has IN_time and OUT_time in 45 kHz ticks. That is what this reads.
 */

#define MPLS_MAX_BYTES (1024 * 1024)

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static unsigned be16(const unsigned char *p)
{
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

/*
 * Read one .mpls. Returns its total duration in 45 kHz ticks, or -1 when the
 * playlist does not reference `clip` (a 5-character clip id, "00008").
 *
 * When `out` is given, the audio and subtitle stream languages from the
 * STN_table are copied into it. That table is the only reliable source: a
 * Blu-ray PMT frequently carries no ISO-639 descriptor, so FFmpeg reports no
 * language and the player shows "unknown language" for every track.
 */
static int64_t mpls_read(udfread *udf, const char *path, const char *clip,
                         evo_udf_title *out)
{
    UDFFILE *f;
    int64_t size, total = -1;
    unsigned char *buf;
    uint32_t pl_start;
    size_t got = 0;

    f = udfread_file_open(udf, path);
    if (!f)
        return -1;
    size = udfread_file_size(f);
    if (size < 40 || size > MPLS_MAX_BYTES) {
        udfread_file_close(f);
        return -1;
    }

    buf = (unsigned char *)malloc((size_t)size);
    if (!buf) {
        udfread_file_close(f);
        return -1;
    }
    while (got < (size_t)size) {
        ssize_t n = udfread_file_read(f, buf + got, (size_t)size - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    udfread_file_close(f);

    if (got < 40 || memcmp(buf, "MPLS", 4) != 0) {
        free(buf);
        return -1;
    }

    pl_start = be32(buf + 8);
    if ((size_t)pl_start + 10 > got) {
        free(buf);
        return -1;
    }

    {
        /* length(4) reserved(2) number_of_PlayItems(2) number_of_SubPaths(2) */
        unsigned n_items = be16(buf + pl_start + 6);
        size_t p = (size_t)pl_start + 10;
        int64_t sum = 0;
        int references_clip = 0;

        for (unsigned i = 0; i < n_items && p + 22 <= got; i++) {
            unsigned plen = be16(buf + p);
            if (plen < 20 || p + 2 + plen > got)
                break;

            /* playitem: length(2) clip_name(5) codec(4) reserved(1)
             *           connection_condition(1) stc_id(1) in(4) out(4) */
            if (!memcmp(buf + p + 2, clip, 5))
                references_clip = 1;

            {
                uint32_t in_time  = be32(buf + p + 14);
                uint32_t out_time = be32(buf + p + 18);
                if (out_time > in_time)
                    sum += (int64_t)(out_time - in_time);
            }

            /*
             * STN_table follows the playitem's fixed part:
             *   length(2) clip(5) codec(4) reserved(1) flags(1) stc(1)
             *   in(4) out(4) UO_mask_table(8) random_access+still_mode(1)
             * = 31 bytes. Missing the 9 bytes of UO mask and flags puts the
             * table 9 bytes early, where the entry counts read as zero and no
             * language is ever found.
             */
            if (out) {
                size_t q = p + 31;
                size_t p_end = p + 2 + plen;

                if (q + 12 <= p_end) {
                    unsigned n_video = buf[q + 4];
                    unsigned n_audio = buf[q + 5];
                    unsigned n_pg    = buf[q + 6];
                    unsigned n_ig    = buf[q + 7];
                    unsigned total_e = n_video + n_audio + n_pg + n_ig;

                    q += 16;   /* length(2) reserved(2) 7 counts(7) reserved(5) */

                    for (unsigned k = 0; k < total_e && q + 4 <= p_end; k++) {
                        unsigned elen = buf[q];
                        unsigned type = buf[q + 1];
                        unsigned pid  = be16(buf + q + 2);
                        size_t   qa   = q + 3;   /* attributes start */
                        size_t   qn   = q + 1 + elen;
                        int      lang_at = -1;

                        if (qn > p_end)
                            break;

                        if (type >= 0x80 && type <= 0x87)
                            lang_at = 2;   /* audio_format(1) + reserved(1) */
                        else if (type == 0x90)
                            lang_at = 1;   /* reserved(1) */

                        if (lang_at >= 0 && qa + lang_at + 3 <= qn &&
                            out->ntracks < EVO_UDF_MAX_TRACKS) {
                            evo_udf_track *t = &out->tracks[out->ntracks];
                            t->pid = (uint16_t)pid;
                            memcpy(t->lang, buf + qa + lang_at, 3);
                            t->lang[3] = 0;
                            /* drop padding */
                            for (int c = 2; c >= 0 && (t->lang[c] == 0 || t->lang[c] == ' '); c--)
                                t->lang[c] = 0;
                            if (t->lang[0])
                                out->ntracks++;
                        }

                        q = qn;
                    }
                }
            }

            p += 2 + plen;
        }

        if (references_clip)
            total = sum;
    }

    free(buf);
    return total;
}

/*
 * Pick the playlist that references `clip` and runs longest - on a disc with
 * several cuts of the same film that is the feature, not the trailer - and
 * copy its duration and stream languages into `out`.
 */
static void title_from_playlists(udfread *udf, const char *clip, evo_udf_title *out)
{
    UDFDIR *bdmv = udfread_opendir(udf, "BDMV");
    UDFDIR *pl;
    struct udfread_dirent e;
    int64_t best = -1;
    evo_udf_title best_title;

    memset(&best_title, 0, sizeof best_title);

    if (!bdmv)
        return;
    pl = udfread_opendir_at(bdmv, "PLAYLIST");
    if (!pl) {
        udfread_closedir(bdmv);
        return;
    }

    while (udfread_readdir(pl, &e)) {
        char path[320];
        evo_udf_title cand;
        int64_t d;

        if (!e.d_name || !ends_with_ci(e.d_name, ".mpls"))
            continue;
        snprintf(path, sizeof path, "BDMV/PLAYLIST/%s", e.d_name);
        memset(&cand, 0, sizeof cand);
        d = mpls_read(udf, path, clip, &cand);
        if (d > best) {
            best = d;
            best_title = cand;
        }
    }

    udfread_closedir(pl);
    udfread_closedir(bdmv);

    out->duration_45k = best;
    out->ntracks      = best_title.ntracks;
    memcpy(out->tracks, best_title.tracks, sizeof out->tracks);
}

/* ------------------------------------------------------------------ */
/* public                                                              */
/* ------------------------------------------------------------------ */

int evo_udf_inspect(const char *url)
{
    if (!url || !*url)
        return -1;
    return udf_walk(url, 1, NULL);
}

int evo_udf_main_title(const char *url, evo_udf_title *out)
{
    if (!url || !*url || !out)
        return -1;

    /* Build marker: the fastest way to tell from a log which binary is
     * running when several local builds are floating around. */
    evo_boot_log("ISO: looking for a BDMV title (iso-play/2)");

    if (udf_walk(url, 1, out) <= 0 || out->size <= 0)
        return -1;
    return 0;
}

int evo_udf_open_window(const char *url, int64_t offset, int64_t size,
                        void **out_pb, void **out_handle)
{
    win_io *w;
    AVIOContext *pb;
    unsigned char *buf;

    if (!url || !out_pb || !out_handle || size <= 0)
        return -1;

    w = (win_io *)calloc(1, sizeof *w);
    if (!w)
        return -1;

    w->under = open_avio(url, "Emby5 ISO", 15000);
    if (!w->under) {
        evo_boot_log("UDF: window: could not reopen the image");
        free(w);
        return -1;
    }

    w->base = offset;
    w->size = size;
    w->pos  = 0;
    pthread_mutex_init(&w->lock, NULL);

    /* Start at the title's first byte; the demuxer will seek from here. */
    if (avio_seek(w->under, offset, SEEK_SET) < 0) {
        evo_boot_log("UDF: window: seek to %lld failed", (long long)offset);
        avio_closep(&w->under);
        pthread_mutex_destroy(&w->lock);
        free(w);
        return -1;
    }

    buf = (unsigned char *)av_malloc(WIN_BUFFER);
    if (!buf) {
        avio_closep(&w->under);
        pthread_mutex_destroy(&w->lock);
        free(w);
        return -1;
    }

    pb = avio_alloc_context(buf, WIN_BUFFER, 0, w, win_read, NULL, win_seek);
    if (!pb) {
        av_free(buf);
        avio_closep(&w->under);
        pthread_mutex_destroy(&w->lock);
        free(w);
        return -1;
    }

    /*
     * Deliberately NOT seekable yet.
     *
     * Setting AVIO_SEEKABLE_NORMAL here lets avformat probe by seeking - to the
     * end for a duration estimate, and back - and every seek is a fresh range
     * request through whatever proxy fronts the image. On a cloud-backed
     * 87 GB image that turned an 11 s open into a 46 s one.
     *
     * evo_udf_window_enable_seek() turns it on once find_stream_info has
     * finished, which is when the player needs it and the demuxer no longer
     * probes.
     */
    *out_pb     = pb;
    *out_handle = w;
    evo_boot_log("UDF: window ready, %lld bytes at offset %lld",
                 (long long)size, (long long)offset);
    return 0;
}

void evo_udf_window_enable_seek(void *pb_v)
{
    AVIOContext *pb = (AVIOContext *)pb_v;

    if (!pb)
        return;
    if (pb->seekable & AVIO_SEEKABLE_NORMAL)
        return;
    /*
     * The demuxer decides whether byte seeking is allowed at all from this
     * flag. Without it av_seek_frame() on mpegts is refused, and the player
     * treats a refused seek as "start the title again from 0".
     */
    pb->seekable = AVIO_SEEKABLE_NORMAL;
    evo_boot_log("UDF: window is seekable now");
}

void evo_udf_close_window(void *pb_v, void *handle)
{
    AVIOContext *pb = (AVIOContext *)pb_v;
    win_io *w = (win_io *)handle;

    if (pb) {
        av_freep(&pb->buffer);
        avio_context_free(&pb);
    }
    if (w) {
        if (w->under)
            avio_closep(&w->under);
        pthread_mutex_destroy(&w->lock);
        free(w);
    }
}
