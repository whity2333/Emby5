/*
 * evo_stream_io.c — High-Throughput Streaming I/O Engine & Read-Ahead Buffer.
 *
 * Implements high-throughput 8MB–16MB streaming buffers with sequential disk
 * prefetching (posix_fadvise) and optimized dictionary options for 100+ Mbps 4K media.
 */
#include "evo_stream_io.h"
#include "evo_iso_probe.h"
#include "evo_udf.h"
#include "evo_boot_log.h"
#include "evo_direct_mem.h"
#include <libavutil/error.h>
#include <libavutil/mathematics.h>

#ifdef EVO_APP_MODULE
extern void pp_stage_bc(const char *stage_id, const char *detail);
#  define SIO_BC(id, d) pp_stage_bc((id), (d))
#else
#  define SIO_BC(id, d) ((void)0)
#endif

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define EVO_STREAM_DEFAULT_RING_SIZE (8 * 1024 * 1024) /* 8 MiB */

/*
 * How long avformat_open_input() may spend before the interrupt callback
 * unwinds it. Generous: a 4 MB probe over USB is well inside a second, and a
 * network open has its own 5 s "timeout" option, so anything approaching this
 * is a demuxer that has stopped making progress rather than slow storage.
 */
#define EVO_STREAM_IO_OPEN_DEADLINE_SEC 20.0

/*
 * ...but a raw network stream (not a playlist) gets less. Hardware, 2026-09-30: a live channel whose
 * server answered every request with the same 1665 bytes and then closed the
 * connection sat in FFmpeg's reconnect loop until this deadline - 20 s of a
 * spinner for a channel that was never going to play. Each reconnect there
 * succeeded (it got its 1665 bytes), so no retry limit inside FFmpeg ever trips;
 * only a wall clock bounds it. A healthy IPTV open (DNS, TLS, two playlists) is
 * about three seconds, so twelve is generous and still ends a dead channel in
 * a fraction of the time.
 */
#define EVO_STREAM_IO_NET_OPEN_DEADLINE_SEC 12.0

struct evo_stream_io_ctx {
    int     is_network;
    char    media_path[512];
    /* Wall-clock deadline for blocking libavformat reads, or 0 when disarmed.
     * See evo_stream_io_set_deadline() in the header for why this exists. */
    double  deadline_at;
    int     deadline_hit;
    volatile int aborted;      /* accessed atomically, including UI-triggered abort */
    int (*external_interrupt)(void *);
    void *external_opaque;
    /* Set when the stream came out of a disc image: the window over the main
     * title's bytes, and its handle. Owned here, released by
     * evo_stream_io_close(). NULL for an ordinary stream. */
    void *udf_pb;
    void *udf_handle;
    /* What the image's playlist says about the title: length in 45 kHz ticks
     * and the language of every audio/subtitle stream. Applied to the demuxer
     * and its streams in evo_stream_io_probe(). */
    evo_udf_title udf_title;
};

static _Thread_local int (*s_open_interrupt)(void *);
static _Thread_local void *s_open_opaque;
void evo_stream_io_set_open_interrupt(int (*callback)(void *), void *opaque)
{
    s_open_interrupt = callback;
    s_open_opaque = opaque;
}

int evo_stream_io_open_cancelled(void)
{
    return s_open_interrupt && s_open_interrupt(s_open_opaque);
}

int evo_stream_io_is_aborted(evo_stream_io_ctx_t *ctx)
{
    if (!ctx) return evo_stream_io_open_cancelled();
    if (__atomic_load_n(&ctx->aborted, __ATOMIC_RELAXED)) return 1;
    if (ctx->external_interrupt && ctx->external_interrupt(ctx->external_opaque)) {
        __atomic_store_n(&ctx->aborted, 1, __ATOMIC_RELAXED);
        return 1; /* latch stop even when the opening job later changes state */
    }
    return 0;
}

int evo_stream_io_probe(evo_stream_io_ctx_t *ctx, AVFormatContext *fmt, double seconds)
{
    if (!fmt || evo_stream_io_is_aborted(ctx)) return AVERROR_EXIT;
    evo_stream_io_set_deadline(ctx, seconds);
    const int rc = avformat_find_stream_info(fmt, NULL);
    evo_stream_io_set_deadline(ctx, 0.0);
    /*
     * A disc-image window is non-seekable while avformat probes, so the probe
     * cannot wander off to the end of the image for a duration estimate. Now
     * that the probe is over, let it seek: without this every av_seek_frame()
     * is refused and the player restarts the title from 0.
     */
    if (ctx && ctx->udf_pb)
        evo_udf_window_enable_seek(ctx->udf_pb);
    /*
     * Give the demuxer the length the playlist states. mpegts has no index and
     * seeks by estimating a byte position from (target / duration) * size; with
     * duration 0 the estimate is always 0, so every seek reported success and
     * restarted the title. Only fills a duration that is still missing.
     */
    if (ctx && ctx->udf_title.duration_45k > 0 && fmt->duration <= 0) {
        fmt->duration = av_rescale(ctx->udf_title.duration_45k, AV_TIME_BASE, 45000);
        for (unsigned i = 0; i < fmt->nb_streams; i++) {
            AVStream *st = fmt->streams[i];
            if (st && st->duration <= 0 && st->time_base.num > 0)
                st->duration = av_rescale_q(fmt->duration, AV_TIME_BASE_Q, st->time_base);
        }
        evo_boot_log("ISO: demuxer duration set to %.3f s from the playlist",
                     (double)fmt->duration / AV_TIME_BASE);
    }
    /*
     * The playlist knows the language of every stream; the PMT of a Blu-ray
     * image usually does not, so FFmpeg reports none and the player labels all
     * of them "unknown language". Streams are matched by PID - for mpegts
     * AVStream.id is the PID the playlist refers to.
     */
    if (ctx && ctx->udf_title.ntracks > 0) {
        int applied = 0;

        for (int t = 0; t < ctx->udf_title.ntracks; t++) {
            const evo_udf_track *tr = &ctx->udf_title.tracks[t];
            for (unsigned i = 0; i < fmt->nb_streams; i++) {
                AVStream *st = fmt->streams[i];
                if (!st || st->id != tr->pid)
                    continue;
                if (!tr->lang[0])
                    continue;
                av_dict_set(&st->metadata, "language", tr->lang, 0);
                applied++;
                break;
            }
        }
        evo_boot_log("ISO: applied %d track language(s) from the playlist", applied);
    }
    /* find_stream_info may return success after an interrupted read. */
    return evo_stream_io_is_aborted(ctx) ? AVERROR_EXIT : rc;
}

static double sio_now_seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/*
 * avio polls this on every buffer refill. Returning non-zero makes the read
 * fail with AVERROR_EXIT, which unwinds the demuxer out of whatever loop it is
 * in - including matroska_resync()'s byte-at-a-time forward scan.
 */
static int sio_interrupt_cb(void *opaque)
{
    evo_stream_io_ctx_t *ctx = (evo_stream_io_ctx_t *)opaque;
    if (evo_stream_io_is_aborted(ctx)) return 1;
    if (!ctx || ctx->deadline_at <= 0.0)
        return 0;
    if (ctx->deadline_hit)
        return 1;
    if (sio_now_seconds() >= ctx->deadline_at) {
        ctx->deadline_hit = 1;
        return 1;
    }
    return 0;
}

/* Stop is waiting on the demux thread: make the read it is blocked in (or the
 * reconnect loop it is sitting out) return now instead of at rw_timeout. */
void evo_stream_io_abort(evo_stream_io_ctx_t *ctx)
{
    if (ctx)
        __atomic_store_n(&ctx->aborted, 1, __ATOMIC_RELAXED);
}

void evo_stream_io_set_deadline(evo_stream_io_ctx_t *ctx, double seconds)
{
    if (!ctx) return;
    if (seconds <= 0.0) {
        /* Disarm only. deadline_hit survives, because the caller reads it
         * after disarming to find out why the call it just made returned. */
        ctx->deadline_at = 0.0;
        return;
    }
    ctx->deadline_at  = sio_now_seconds() + seconds;
    ctx->deadline_hit = 0;
}

int evo_stream_io_deadline_expired(const evo_stream_io_ctx_t *ctx)
{
    return ctx ? ctx->deadline_hit : 0;
}

void evo_stream_io_hint_sequential(int fd)
{
    if (fd < 0) return;
    /* posix_fadvise is not wired on the PS5 kernel from the app-module
     * process - it faults (SIGSYS-class, same as exit()). It is only a
     * read-ahead hint, so skip it there. Hardware-confirmed 2026-09-02:
     * the crash on file-open was here, between P8_01c and P8_01d. */
#ifndef EVO_APP_MODULE
#if defined(POSIX_FADV_SEQUENTIAL)
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
#if defined(POSIX_FADV_WILLNEED)
    posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
#endif
#endif
}

static void prefetch_file_sequential(const char *path)
{
    if (!path || strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0)
        return;
#ifndef EVO_APP_MODULE
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        evo_stream_io_hint_sequential(fd);
        close(fd);
    }
#endif
}

/*
 * The frame rate a raw .obu's file name states, as an AVRational string, or 0.
 * Takes the digits before "fps" ("2397fps", "23.976fps", "24fps"). Four
 * undotted digits are hundredths (Netflix's 2397 / 2997 / 5994), and anything
 * within 0.02 of an NTSC rate snaps to its exact n*1000/1001 form.
 */
static int raw_av1_rate_from_name(const char *path, char *out, size_t out_len)
{
    const char *dot = strrchr(path, '.');
    if (!dot || strcasecmp(dot, ".obu") != 0)
        return 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    for (const char *p = strstr(base, "fps"); p; p = strstr(p + 1, "fps")) {
        const char *q = p;
        while (q > base && ((q[-1] >= '0' && q[-1] <= '9') || q[-1] == '.'))
            q--;
        if (q == p)
            continue;
        char num[16];
        size_t n = (size_t)(p - q);
        if (n >= sizeof num)
            continue;
        memcpy(num, q, n);
        num[n] = 0;
        double v = atof(num);
        if (!strchr(num, '.') && n == 4)
            v /= 100.0;
        if (v < 1.0 || v > 240.0)
            continue;
        static const int ntsc[] = { 24, 30, 48, 60, 120 };
        for (size_t i = 0; i < sizeof ntsc / sizeof ntsc[0]; i++) {
            double r = ntsc[i] * 1000.0 / 1001.0;
            if (v > r - 0.02 && v < r + 0.02) {
                snprintf(out, out_len, "%d/1001", ntsc[i] * 1000);
                return 1;
            }
        }
        snprintf(out, out_len, "%d/1000", (int)(v * 1000.0 + 0.5));
        return 1;
    }
    return 0;
}

/* Case-insensitive substring search. (libc's strcasestr is one of the calls the
 * native-app libc leaves as a NULL import, so it is not used.) */
static int sio_contains_ci(const char *hay, const char *needle)
{
    const size_t n = strlen(needle);
    if (n == 0) return 1;
    for (; *hay; hay++) {
        if (strncasecmp(hay, needle, n) == 0)
            return 1;
    }
    return 0;
}

int evo_stream_io_url_is_playlist(const char *url)
{
    if (!url) return 0;
    return sio_contains_ci(url, ".m3u8") || sio_contains_ci(url, ".mpd") ||
           sio_contains_ci(url, ".ism/");
}

/* EOF of a finite VOD file is not a broken live connection. Examine the URL
 * path, not a hostname/query that happens to contain a video extension. */
static int sio_url_is_finite_file(const char *url)
{
    if (!url) return 0;
    const char *authority = strstr(url, "://");
    const char *path = authority ? strchr(authority + 3, '/') : url;
    if (!path) return 0;
    if (sio_contains_ci(path, "?static=true") || sio_contains_ci(path, "&static=true")) return 1;
    const size_t len = strcspn(path, "?#");
    for (size_t i = 0; i < len; ++i) {
        if ((len-i >= 7 && strncasecmp(path+i, "/movie/", 7)==0) ||
            (len-i >= 8 && strncasecmp(path+i, "/series/", 8)==0)) return 1;
    }
    static const char *extensions[] = {".mp4", ".mkv", ".webm", ".avi", ".mov", ".m4v", ".flac", ".mp3", ".m4a", ".wav"};
    for (size_t i=0; i<sizeof extensions/sizeof extensions[0]; ++i) {
        const size_t n=strlen(extensions[i]);
        if (len>=n && strncasecmp(path+len-n,extensions[i],n)==0) return 1;
    }
    return 0;
}

#ifdef NUVIO_APP
/* Nuvio app: request headers the page sent with the stream (an addon's
 * Referer / User-Agent / Authorization), "Name: value\r\n" each. Set by the
 * player before it opens the URL; empty for a plain link. */
char nuvio_stream_headers[4096];
char nuvio_stream_user_agent[512];
#endif

void evo_stream_io_apply_network_options(AVDictionary **opts, const char *url)
{
#ifdef NUVIO_APP
    if (nuvio_stream_headers[0])
        av_dict_set(opts, "headers", nuvio_stream_headers, 0);
    if (nuvio_stream_user_agent[0])
        av_dict_set(opts, "user_agent", nuvio_stream_user_agent, 0);
#endif
    av_dict_set(opts, "reconnect", "1", 0);
    av_dict_set(opts, "reconnect_streamed", "1", 0);
    av_dict_set(opts, "reconnect_on_network_error", "1", 0);
    av_dict_set(opts, "reconnect_delay_max", "2", 0);
    av_dict_set(opts, "rw_timeout", "5000000", 0);
    av_dict_set(opts, "timeout", "5000000", 0);

    /*
     * Bounded retries. FFmpeg's default is to keep going for 256 s. These cap
     * one failing read at three reconnects and six seconds of waiting; the
     * network open deadline above caps the case they cannot (every reconnect
     * succeeds, and gets the same data again).
     */
    av_dict_set(opts, "reconnect_max_retries", "3", 0);
    av_dict_set(opts, "reconnect_delay_total_max", "6", 0);

    /*
     * Reconnect at EOF only for a raw stream.
     *
     * For a live MPEG-TS over HTTP an EOF is a dropped connection, and
     * reconnecting is right. For an HLS or DASH playlist it is the end of a
     * small file that the server never said the length of: the http layer
     * "reconnects", fetches the same bytes again, hits EOF again, and repeats
     * with a growing delay - a full request and TLS handshake each time, on
     * every playlist the demuxer reads. libavformat's hls demuxer hands these
     * options to every playlist and segment it opens. Hardware, 2026-09-30, from
     * the log:
     *
     *   Will reconnect at 182 in 0 second(s), error=End of file.   (x6, 2 s)
     *   ... open took 10.8 s   (a 182-byte master playlist)
     *   Will reconnect at 1023 in 0 second(s), error=End of file.  (x18, 18 s)
     *   ... open took 21.4 s and delivered no packets
     *
     * The hls demuxer already reloads a playlist and retries a segment itself.
     */
    /* Emby5: nor for Emby's direct play (static=true): a file with a known
     * length, whose end is the end. Seeking in a FLAC reads at it, and the reconnect
     * looped there until the seek failed (7 s, "Decoder seek failed"). */
    av_dict_set(opts, "reconnect_at_eof",
        !evo_stream_io_url_is_playlist(url) && !sio_url_is_finite_file(url) ? "1" : "0", 0);

    /*
     * HLS: accept every segment URL the playlist names.
     *
     * libavformat's hls demuxer refuses a segment whose URL does not end
     * in an extension on its allowlist, and the default list is short.
     * Plenty of live CDNs serve segments from extensionless, signed or
     * query-string URLs, and the refusal is not survivable: the segment
     * fetch fails, the media playlist behind it fails to parse, and the
     * open completes with nb_streams=0 - a channel that sits on a black
     * screen with no error. Hardware, 2026-09-28:
     *
     *   URL .../v1/segment/<token>/0/186944902 is not in allowed_extensions
     *   parse_playlist error Invalid data found when processing input
     *   P8_02b_FIND_INFO_RC rc=0 timeout=0 nb_streams=0
     *
     * The allowlist is a guard against a playlist naming a local path; a
     * media player asked to open a URL the user chose has already made
     * that decision, and every other player ships with this widened.
     */
    av_dict_set(opts, "allowed_extensions", "ALL", 0);

    /*
     * ...and stop checking the segment's extension against its detected format.
     *
     * "ALL" clears only the first of two gates in hls.c's test_segment(). The
     * second - `extension_picky`, on by default - compares the URL's extension
     * with what the segment turned out to be, and an extensionless URL has none
     * to compare, so a segment that is plainly MPEG-TS is refused anyway:
     *
     *   detected format mpegts extension none mismatches allowed extensions in
     *   url https://.../v1/segment/<token>/3/186971110
     *   Error when loading first segment ...
     *
     * Hardware, 2026-09-30: an Akamai channel whose variant playlist opened with
     * AVERROR_INVALIDDATA after 8 s. Same class of channel as the one above.
     */
    av_dict_set(opts, "extension_picky", "0", 0);
}

int evo_stream_io_open(const char *path,
                       AVFormatContext **out_fmt_ctx,
                       const evo_stream_io_config_t *cfg,
                       evo_stream_io_ctx_t **out_io_ctx)
{
    if (!path || !out_fmt_ctx) return -1;
    *out_fmt_ctx = NULL;
    if (out_io_ctx) *out_io_ctx = NULL;
    if (evo_stream_io_open_cancelled()) return AVERROR_EXIT;

    SIO_BC("P8_01a_SIO_ENTER", path);
    evo_stream_io_ctx_t *ctx = (evo_stream_io_ctx_t *)evo_direct_mem_calloc(1, sizeof(evo_stream_io_ctx_t));
    if (!ctx) ctx = (evo_stream_io_ctx_t *)calloc(1, sizeof(evo_stream_io_ctx_t));
    if (!ctx) return -1;
    SIO_BC("P8_01b_SIO_CTX", "ctx alloc ok");
    ctx->external_interrupt = s_open_interrupt;
    ctx->external_opaque = s_open_opaque;

    ctx->is_network = (strncmp(path, "http://", 7) == 0 ||
                       strncmp(path, "https://", 8) == 0 ||
                       strncmp(path, "ftp://", 6) == 0 ||
                       strncmp(path, "smb://", 6) == 0);
    snprintf(ctx->media_path, sizeof(ctx->media_path), "%s", path);

    size_t ring_size = (cfg && cfg->ring_buffer_size > 0) ? cfg->ring_buffer_size : EVO_STREAM_DEFAULT_RING_SIZE;
    char buf_size_str[32];
    snprintf(buf_size_str, sizeof(buf_size_str), "%zu", ring_size);

    AVDictionary *opts = NULL;

    /*
     * Fast probe & high-throughput streaming buffer options.
     *
     * analyzeduration is 4 s, not the 2 s this file used while it had no
     * callers: PlaybackController's own open (the one that actually shipped)
     * used 4 s, and #90 routes that open through here. Halving it would change
     * stream detection on awkward MPEG-TS - the files where a second audio
     * track appears late - as a side effect of a refactor, which is the kind of
     * regression that gets blamed on the codec pass rather than on this line.
     */
    av_dict_set(&opts, "probesize", "4194304", 0);
    av_dict_set(&opts, "analyzeduration", "4000000", 0);
    av_dict_set(&opts, "buffer_size", buf_size_str, 0);

    if (ctx->is_network) {
        evo_stream_io_apply_network_options(&opts, path);
    } else {
        /* Prime the kernel storage controller for sequential read-ahead */
        SIO_BC("P8_01c_PREFETCH", "open+fadvise");
        prefetch_file_sequential(path);
    }

    /* A raw AV1 .obu carries no timestamps, and the obu demuxer invents them
     * from its `framerate` option - 25 by default. 23.976 fps film then plays
     * 4% fast (#94, Chimera on hardware 2026-09-25). The sequence header's
     * timing_info is optional and Netflix's Open Content streams omit it; the
     * name is the one place the rate is written down ("...-2397fps-..."). */
    if (!ctx->is_network) {
        char rate[16];
        if (raw_av1_rate_from_name(path, rate, sizeof rate)) {
            av_dict_set(&opts, "framerate", rate, 0);
            SIO_BC("P8_01c2_OBU_RATE", rate);
        }
    }

    SIO_BC("P8_01d_PRE_OPEN", "-> avformat_open_input");

    /*
     * Allocate the context here rather than letting avformat_open_input do it,
     * so interrupt_callback is in place before the first byte is read. The
     * open itself gets the same deadline as the probe: a header parse that
     * desyncs scans just as far as a probe that does.
     */
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) {
        av_dict_free(&opts);
        evo_direct_mem_free(ctx);
        return -1;
    }
    fmt->interrupt_callback.callback = sio_interrupt_cb;
    fmt->interrupt_callback.opaque   = ctx;
    /* A playlist open is genuinely longer - the master, a variant, and the first
     * segment all load before it returns - and a slow origin can spend ten seconds
     * on them. It keeps the full budget; the reconnect loop that used to burn it
     * is gone (see evo_stream_io_apply_network_options). Only a raw stream, whose
     * failure mode is looping on the same few bytes, gets the short one. */
    evo_stream_io_set_deadline(ctx, (ctx->is_network && !evo_stream_io_url_is_playlist(path))
                                        ? EVO_STREAM_IO_NET_OPEN_DEADLINE_SEC
                                        : EVO_STREAM_IO_OPEN_DEADLINE_SEC);

    int rc = avformat_open_input(&fmt, path, NULL, &opts);
    /* avformat_open_input frees and NULLs *fmt on failure, including on an
     * interrupted open, so there is nothing left to clean up here. */
    evo_stream_io_set_deadline(ctx, 0.0);
    {
        char d[32]; snprintf(d, sizeof d, "rc=%d", rc);
        SIO_BC("P8_01e_OPEN_RC", d);
    }
    av_dict_free(&opts);

    if (rc < 0) {
        /*
         * A disc image (.iso) is a UDF filesystem, not a container, so
         * avformat rejects it with AVERROR_INVALIDDATA and the caller only
         * ever sees "unsupported". Before giving up:
         *
         *   1. probe the image so the log says what it actually contains,
         *   2. look for a BDMV/STREAM title inside it,
         *   3. reopen a window over just that title's bytes and try again.
         *
         * The demuxer then reads an ordinary mpegts stream and nothing below
         * this point knows an image was involved. Only reached when a normal
         * open already failed, so a working stream never pays for it.
         *
         * Probed once per distinct path so a retry loop cannot spam it.
         */
        evo_udf_title iso;

        {
            static char s_last_probed[512];
            if (path && strcmp(s_last_probed, path) != 0) {
                snprintf(s_last_probed, sizeof s_last_probed, "%s", path);
                evo_iso_probe(path);
            }
        }

        memset(&iso, 0, sizeof iso);

        if (path && evo_udf_main_title(path, &iso) == 0) {
            void *pb = NULL, *handle = NULL;

            if (evo_udf_open_window(path, iso.offset, iso.size, &pb, &handle) == 0) {
                AVFormatContext *fmt2 = avformat_alloc_context();

                if (fmt2) {
                    fmt2->pb = (AVIOContext *)pb;
                    fmt2->flags |= AVFMT_FLAG_CUSTOM_IO;
                    fmt2->interrupt_callback.callback = sio_interrupt_cb;
                    fmt2->interrupt_callback.opaque   = ctx;

                    evo_stream_io_set_deadline(ctx, EVO_STREAM_IO_OPEN_DEADLINE_SEC);
                    rc = avformat_open_input(&fmt2, NULL, NULL, NULL);
                    evo_stream_io_set_deadline(ctx, 0.0);

                    if (rc >= 0 && fmt2) {
                        ctx->udf_pb     = pb;
                        ctx->udf_handle = handle;
                        ctx->udf_title = iso;
                        *out_fmt_ctx = fmt2;
                        if (out_io_ctx) *out_io_ctx = ctx;
                        SIO_BC("P8_01f_ISO_TITLE", "opened main title from image");
                        return 0;
                    }
                    /* avformat_open_input freed fmt2 on failure. */
                    evo_boot_log("ISO: the title inside the image did not open (%d)", rc);
                }
                evo_udf_close_window(pb, handle);
            }
        }

        evo_direct_mem_free(ctx);
        return rc;
    }

    *out_fmt_ctx = fmt;
    if (out_io_ctx) *out_io_ctx = ctx;
    return 0;
}

void evo_stream_io_close(evo_stream_io_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->udf_pb || ctx->udf_handle) {
        evo_udf_close_window(ctx->udf_pb, ctx->udf_handle);
        ctx->udf_pb = NULL;
        ctx->udf_handle = NULL;
    }
    evo_direct_mem_free(ctx);
}
