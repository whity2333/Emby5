/*
 * evo_iso_probe.h — read-only structural probe for disc images (.iso).
 *
 * Emby5's demuxer cannot open an ISO: the file is a filesystem image, not a
 * video stream. Blu-ray images carry UDF 2.50; DVDs carry ISO9660 (+UDF
 * bridge). Before adding any playback path we need to know which of those a
 * given image actually has, and where its BDMV tree lives.
 *
 * This probe only reads and logs. It never changes what the caller does, and
 * it is called from the failed-open path, so a working stream is never
 * affected by it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef EVO_ISO_PROBE_H
#define EVO_ISO_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Open `path` (a local file or an http(s) URL) read-only, walk the ISO9660
 * primary volume descriptor and the UDF anchor, and log what is found.
 * Safe to call on anything: a non-ISO simply logs that no signature matched.
 * Returns 0 when the image looks like a disc image, -1 otherwise.
 */
int evo_iso_probe(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* EVO_ISO_PROBE_H */
