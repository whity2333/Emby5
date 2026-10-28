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
 * Stage 1: inspect and log only. The BDMV tree, the playlist names and the
 * candidate main title are reported so the next stage can pick a title with
 * evidence rather than a guess.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "evo_udf.h"
#include "evo_boot_log.h"

#include "blockinput.h"
#include "udfread.h"

#include <libavformat/avio.h>
#include <libavutil/dict.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* How many entries of each kind to log before summarising the rest. */
#define UDF_LOG_LIMIT 24

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

/* Walk one BDMV subdirectory, logging names. Returns the entry count. */
static int list_dir(udfread *udf, UDFDIR *parent, const char *name, const char *tag)
{
    UDFDIR *d = udfread_opendir_at(parent, name);
    struct udfread_dirent e;
    int n = 0;

    if (!d) {
        evo_boot_log("UDF: %s/%s not present", tag, name);
        return 0;
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
    return n;
}

/*
 * List BDMV/STREAM and report sizes. The largest .m2ts is the usual main
 * feature on a simple disc; it is a guess, and the log says so.
 */
static int scan_streams(udfread *udf)
{
    UDFDIR *bdmv = udfread_opendir(udf, "BDMV");
    UDFDIR *stream;
    struct udfread_dirent e;
    int n = 0, shown = 0;
    int64_t best_size = -1;
    uint32_t best_lba = 0;
    char best_name[256];

    best_name[0] = 0;

    if (!bdmv) {
        evo_boot_log("UDF: no BDMV directory at the image root");
        return -1;
    }

    /* Playlists first: they name the real title, and the next stage needs them. */
    list_dir(udf, bdmv, "PLAYLIST", "BDMV");

    stream = udfread_opendir_at(bdmv, "STREAM");
    if (!stream) {
        evo_boot_log("UDF: no BDMV/STREAM directory");
        udfread_closedir(bdmv);
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
        if (shown < UDF_LOG_LIMIT) {
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
        return 0;
    }

    evo_boot_log("UDF: %d stream file(s); largest is %s  size=%lld  offset=%lld  "
                 "(heuristic: main feature)",
                 n, best_name, (long long)best_size,
                 (long long)best_lba * UDF_BLOCK_SIZE);
    return n;
}

int evo_udf_inspect(const char *url)
{
    avio_input *in;
    AVDictionary *opts = NULL;
    udfread *udf;
    int rc, titles;

    if (!url || !*url)
        return -1;

    in = (avio_input *)calloc(1, sizeof *in);
    if (!in)
        return -1;

    av_dict_set(&opts, "rw_timeout", "15000000", 0);
    av_dict_set(&opts, "user_agent", "Emby5 UDF inspect", 0);
    rc = avio_open2(&in->io, url, AVIO_FLAG_READ, NULL, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        evo_boot_log("UDF: could not open the image (%d)", rc);
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

    {
        const char *vid = udfread_get_volume_id(udf);
        evo_boot_log("UDF: mounted, volume='%s', %lld blocks of %d bytes",
                     vid ? vid : "?",
                     (long long)(in->size_bytes / UDF_BLOCK_SIZE),
                     UDF_BLOCK_SIZE);
    }

    titles = scan_streams(udf);

    /* udfread_close() calls our close(), which frees `in`. */
    udfread_close(udf);
    return titles;
}
