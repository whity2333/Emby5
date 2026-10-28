/*
 * evo_udf.h — Blu-ray disc image (.iso) access.
 *
 * An ISO is a filesystem image, not a stream: avformat cannot open it. This
 * module reads the UDF filesystem inside the image (through libudfread) over
 * the same avio connection the player would have used, finds the BDMV
 * structure, and reports where the main title's .m2ts actually lives inside
 * the image.
 *
 * Stage 1 (this file today) only inspects and logs. Nothing here changes what
 * the caller does yet.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef EVO_UDF_H
#define EVO_UDF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Open `url` (local path or http(s)), read its UDF filesystem, and log the
 * BDMV tree: playlists, stream files, sizes, and which .m2ts looks like the
 * main title. Read-only.
 *
 * Returns the number of stream files found, or -1 if the image could not be
 * opened or has no BDMV directory.
 */
int evo_udf_inspect(const char *url);

/*
 * Locate the main title inside `url` without logging the whole tree.
 * On success fills *offset (byte offset of the .m2ts inside the image) and
 * *size (its length in bytes) and returns 0. Returns -1 when the image has no
 * usable BDMV/STREAM entry.
 *
 * The title is the largest .m2ts. That is a heuristic: it is right on a plain
 * disc and wrong on a seamless-branching one, where the feature is split
 * across several .m2ts and only the .mpls knows the order. Reported as such.
 */
int evo_udf_main_title(const char *url, int64_t *offset, int64_t *size);

/*
 * Build an AVIOContext that exposes only [offset, offset+size) of `url`.
 *
 * This is what makes an .iso playable: the demuxer sees a normal, seekable
 * file of the title's length, while every read is a range request into the
 * image. Returns 0 and fills *out_pb on success; the caller owns the pair and
 * releases it with evo_udf_close_window().
 */
int evo_udf_open_window(const char *url, int64_t offset, int64_t size,
                        void **out_pb, void **out_handle);
void evo_udf_close_window(void *pb, void *handle);

#ifdef __cplusplus
}
#endif

#endif /* EVO_UDF_H */
