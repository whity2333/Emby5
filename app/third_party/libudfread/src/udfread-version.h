/*
 * udfread-version.h — hand-written replacement for the meson-generated header.
 *
 * Generated upstream from src/udfread-version.h.in; only the three version
 * numbers change. libudfread 1.2.0 (commit b0bc695).
 *
 * This file is part of libudfread
 * Copyright (C) 2014-2026 VLC authors and VideoLAN
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef UDFREAD_VERSION_H_
#define UDFREAD_VERSION_H_

#ifdef __cplusplus
extern "C" {
#endif

/** @cond */
#ifdef UDFREAD_API_EXPORT
#include "attributes.h"
#elif !defined(UDF_PUBLIC)
#define UDF_PUBLIC
#endif
/** @endcond */

#define UDFREAD_VERSION_CODE(major, minor, micro) \
    (((major) * 10000) +                         \
     ((minor) *   100) +                         \
     ((micro) *     1))

/** Library version major number */
#define UDFREAD_VERSION_MAJOR 1
/** Library version minor number */
#define UDFREAD_VERSION_MINOR 2
/** Library version micro number */
#define UDFREAD_VERSION_MICRO 0

/** Library version as a string */
#define UDFREAD_VERSION_STRING "1.2.0"

/** Library version as a single number */
#define UDFREAD_VERSION \
    UDFREAD_VERSION_CODE(UDFREAD_VERSION_MAJOR, UDFREAD_VERSION_MINOR, UDFREAD_VERSION_MICRO)

UDF_PUBLIC void udfread_get_version(int *major, int *minor, int *micro);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* UDFREAD_VERSION_H_ */
