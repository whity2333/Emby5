/*
 * config.h — hand-written replacement for the meson-generated config.h.
 *
 * libudfread's build normally generates this. Emby5 compiles the library
 * straight into the app (no meson, no cross-build step), so the few switches
 * the sources actually test are set here.
 *
 * The PS5 (Orbis) sysroot provides all three POSIX headers. Atomic pointer
 * updates do not need a switch: udf_atomic.h takes its first branch on
 * clang/x86-64, which is what the PS5 toolchain is.
 *
 * Keep in sync with upstream libudfread 1.2.0 (commit b0bc695).
 */
#ifndef UDFREAD_CONFIG_H_
#define UDFREAD_CONFIG_H_

#define HAVE_UDFREAD_VERSION_H 1
#define HAVE_FCNTL_H           1
#define HAVE_PTHREAD_H         1
#define HAVE_UNISTD_H          1

#endif /* UDFREAD_CONFIG_H_ */
