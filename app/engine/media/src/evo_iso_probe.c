/*
 * evo_iso_probe.c — structural probe for disc images (.iso).
 *
 * Read-only. Reached from the failed-open path in evo_stream_io_open(), so a
 * stream that opens normally never pays for this. Everything goes to
 * evo_boot_log() (UDP when the build defines EMBY5_LOG_HOST).
 *
 * What it reports
 *   - image size
 *   - ISO9660 primary volume descriptor (DVD images, and the bridge on hybrids)
 *   - UDF anchor volume descriptor pointer at LBA 256 (Blu-ray images)
 *   - the tag sequence of the UDF main volume descriptor sequence, which says
 *     whether a partition and a logical volume are actually present
 *
 * What it does NOT do yet
 *   - walk UDF to BDMV/STREAM, pick a title, or feed the demuxer. That is the
 *     next step; this probe exists so we can see the real layout first.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "evo_iso_probe.h"
#include "evo_boot_log.h"

#include <libavformat/avio.h>
#include <libavutil/dict.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define ISO_SECTOR      2048
#define ISO_PVD_LBA     16      /* ECMA-119 primary volume descriptor */
#define UDF_ANCHOR_LBA  256     /* UDF 2.01+ anchor volume descriptor pointer */

/* UDF descriptor tag identifiers (ECMA-167 / UDF 2.60 part 3) */
#define UDF_TAG_PVD     1
#define UDF_TAG_ANCHOR  2
#define UDF_TAG_VDP     3
#define UDF_TAG_PART    5
#define UDF_TAG_LVD     6
#define UDF_TAG_TERM    8

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static int read_at(AVIOContext *io, int64_t offset, unsigned char *buf, int len)
{
    if (offset < 0)
        return -1;
    if (avio_seek(io, offset, SEEK_SET) < 0)
        return -1;
    return avio_read(io, buf, len) == len ? 0 : -1;
}

/* ISO9660: report the volume id and the root directory extent. */
static int probe_iso9660(AVIOContext *io)
{
    unsigned char s[ISO_SECTOR];

    if (read_at(io, (int64_t)ISO_PVD_LBA * ISO_SECTOR, s, ISO_SECTOR) != 0)
        return -1;
    /* bytes 1..5 are "CD001"; byte 0 is the descriptor type (1 = primary) */
    if (s[0] != 1 || memcmp(s + 1, "CD001", 5) != 0)
        return -1;

    {
        char vol[33];
        memcpy(vol, s + 40, 32);
        vol[32] = 0;
        /* the field is space padded */
        for (int i = 31; i >= 0 && vol[i] == ' '; i--)
            vol[i] = 0;
        evo_boot_log("ISO probe: ISO9660 PVD ok, volume='%s'", vol);
    }

    /* root directory record lives at offset 156, 34 bytes */
    {
        const unsigned char *rr = s + 156;
        unsigned rec_len = rr[0];
        if (rec_len >= 34) {
            evo_boot_log("ISO probe: ISO9660 root extent lba=%u size=%u",
                         rd32(rr + 2), rd32(rr + 10));
        }
    }
    return 0;
}

/* UDF: anchor pointer, then the tag sequence of the main descriptor sequence. */
static int probe_udf(AVIOContext *io)
{
    unsigned char s[ISO_SECTOR];

    if (read_at(io, (int64_t)UDF_ANCHOR_LBA * ISO_SECTOR, s, ISO_SECTOR) != 0)
        return -1;

    {
        unsigned tag = rd16(s);
        if (tag != UDF_TAG_ANCHOR) {
            evo_boot_log("ISO probe: no UDF anchor at LBA%d (tag=%u)", UDF_ANCHOR_LBA, tag);
            return -1;
        }
    }

    {
        uint32_t main_len = rd32(s + 16);
        uint32_t main_loc = rd32(s + 20);
        evo_boot_log("ISO probe: UDF anchor ok, main VDS len=%u loc=%u", main_len, main_loc);

        if (main_len < ISO_SECTOR || main_loc == 0 || main_len > 16u * ISO_SECTOR) {
            evo_boot_log("ISO probe: UDF main VDS extent looks wrong, stopping");
            return 0;
        }

        /* Walk the main volume descriptor sequence: every 2048-byte sector
         * starts with a tag. Stop at the terminating descriptor or when the
         * declared extent runs out. */
        {
            uint32_t sectors = main_len / ISO_SECTOR;
            uint32_t part_loc = 0, lvd_block = 0;
            char vol[129];

            vol[0] = 0;
            if (sectors > 16)
                sectors = 16;

            for (uint32_t i = 0; i < sectors; i++) {
                if (read_at(io, ((int64_t)main_loc + i) * ISO_SECTOR, s, ISO_SECTOR) != 0)
                    break;
                {
                    unsigned tag = rd16(s);
                    evo_boot_log("ISO probe:   VDS[%u] tag=%u", i, tag);

                    if (tag == UDF_TAG_PART)
                        part_loc = rd32(s + 188);   /* partition starting location */

                    if (tag == UDF_TAG_LVD) {
                        lvd_block = rd32(s + 212);  /* logical block size */
                        /* logical volume identifier: 128-byte dstring at 84,
                         * stored as compressed unicode: first byte = charset,
                         * then bytes, possibly with a trailing length byte */
                        memcpy(vol, s + 84, 128);
                        vol[128] = 0;
                    }

                    if (tag == UDF_TAG_TERM)
                        break;
                }
            }

            evo_boot_log("ISO probe: UDF partition start=%u, logical block size=%u",
                         part_loc, lvd_block);
        }
    }
    return 0;
}

int evo_iso_probe(const char *path)
{
    AVIOContext *io = NULL;
    AVDictionary *opts = NULL;
    int is_iso = 0;
    int has_udf = 0;

    if (!path || !*path)
        return -1;

    av_dict_set(&opts, "rw_timeout", "8000000", 0);
    av_dict_set(&opts, "user_agent", "Emby5 ISO probe", 0);

    if (avio_open2(&io, path, AVIO_FLAG_READ, NULL, &opts) < 0) {
        av_dict_free(&opts);
        evo_boot_log("ISO probe: could not open the stream");
        return -1;
    }
    av_dict_free(&opts);

    {
        int64_t size = avio_size(io);
        evo_boot_log("ISO probe: opened, size=%lld bytes", (long long)size);
        /* a Blu-ray image is tens of GB; a few MB is not a disc image */
        if (size > 0 && size < (int64_t)64 * 1024 * 1024)
            evo_boot_log("ISO probe: small for a disc image, probing anyway");
    }

    if (probe_iso9660(io) == 0)
        is_iso = 1;
    if (probe_udf(io) == 0) {
        is_iso = 1;
        has_udf = 1;
    }

    if (!is_iso)
        evo_boot_log("ISO probe: no ISO9660 or UDF signature found");

    avio_closep(&io);

    /*
     * The BDMV walk is not done here: evo_udf_main_title() runs it once, on the
     * playback path, and does it with logging. Walking here as well cost a
     * second connection and a second full pass over the descriptor tree, which
     * on a cloud-backed image is seconds of latency for no new information.
     */
    (void)has_udf;

    return is_iso ? 0 : -1;
}
