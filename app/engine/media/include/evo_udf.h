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

#ifdef __cplusplus
}
#endif

#endif /* EVO_UDF_H */
