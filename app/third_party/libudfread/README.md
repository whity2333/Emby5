# libudfread (vendored)

UDF 2.01/2.60 reader, used to open Blu-ray disc images (`.iso`) that Emby5's
demuxer cannot parse. LGPL-2.1-or-later, see `COPYING`.

## Provenance

| | |
|---|---|
| Upstream | https://code.videolan.org/videolan/libudfread |
| Version | 1.2.0 |
| Commit | `b0bc695` |
| Vendored | 2026-10-28 |
| Files | `src/*.c`, `src/*.h` (unmodified) |

## What was changed

Nothing in the upstream sources. Two files that upstream generates at build
time are written by hand here, because Emby5 compiles the library straight into
the app instead of running meson:

- `src/config.h` — the `HAVE_*` switches the sources test
- `src/udfread-version.h` — the version numbers, normally filled in from
  `udfread-version.h.in`

Both carry a header saying so. `src/meson.build` and
`src/udfread-version.h.in` were dropped because nothing reads them here.

## Why vendored and not linked

The PS5 sysroot has no libudfread, and adding a meson cross-build to the
toolchain setup would be a second build system for one small C library. The
sources are plain C99 with no dependency beyond libc and pthread.

## Licensing

LGPL-2.1-or-later is compatible with this project's GPL-3.0-or-later: the
licence permits use under the GPL, and the library stays a separate,
replaceable set of source files under `third_party/libudfread/`.

## Usage here

`app/engine/media/src/evo_udf.c` supplies a `udfread_block_input` backed by
FFmpeg's avio, so a disc image can be read over HTTP range requests instead of
a local file.
