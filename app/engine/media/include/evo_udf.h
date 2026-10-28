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

#define EVO_UDF_MAX_TRACKS 32

/* One audio or subtitle stream, as the playlist describes it. */
typedef struct {
    uint16_t pid;      /* matches AVStream.id for a mpegts stream */
    char     lang[4];  /* ISO-639, may be empty */
} evo_udf_track;

/* What the playlist says about the title we picked. */
typedef struct {
    int64_t offset;        /* byte offset of the .m2ts inside the image */
    int64_t size;          /* its length */
    int64_t duration_45k;  /* title length in 45 kHz ticks, -1 when unknown */
    int     ntracks;
    evo_udf_track tracks[EVO_UDF_MAX_TRACKS];
} evo_udf_title;

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
 * Locate the main title inside `url` and describe it in `out`.
 *
 * The title is the largest .m2ts. That is a heuristic: right on a plain disc,
 * wrong on a seamless-branching one, where the feature is split across several
 * .m2ts and only the .mpls knows the order.
 *
 * `out` gets the title's byte range, its duration, and the language of every
 * audio and subtitle stream the playlist lists. Those languages are the point:
 * a Blu-ray PMT often carries no ISO-639 descriptor, so FFmpeg reports none and
 * the player shows "unknown language" for every track. The playlist always has
 * them.
 *
 * Returns 0 on success, -1 when the image has no usable BDMV/STREAM entry.
 */
int evo_udf_main_title(const char *url, evo_udf_title *out);

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

/*
 * Allow byte seeking in a window.
 *
 * A window starts non-seekable on purpose: while avformat probes, a seekable
 * stream invites duration estimates that seek to the end of the image, and each
 * of those is a range request through the proxy. Call this once the probe is
 * done - then seeking works and playback can jump.
 */
void evo_udf_window_enable_seek(void *pb);

/*
 * Size of the window behind `pb`, or 0 when it is not one.
 *
 * The demuxer asks so it can tell a disc-image stream from an ordinary one and
 * seek it by byte position: mpegts has no index and its timestamp seek returns
 * success here without moving the byte stream.
 */
int64_t evo_udf_window_size_of(void *pb);

#ifdef __cplusplus
}
#endif

#endif /* EVO_UDF_H */
