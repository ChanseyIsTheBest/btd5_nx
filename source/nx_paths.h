/* nx_paths.h -- the game's own folder, decided at runtime.
 *
 * Everything the port or the game writes goes inside ONE folder: the folder the
 * .nro was launched from. It is found the way libnx itself sets the working
 * directory (__libnx_init_cwd: argv[0] cut at its last '/'), so it is the same
 * folder libnative.so and Assets/ are loaded from. Only if the launch path is
 * unknown does it fall back to the working directory, and only then to
 * DATA_DIR_FALLBACK (config.h). The log states which one was used.
 *
 * Engine file calls are confined by libc_shim.c (shim_game_path): paths are
 * anchored and normalised, and a write that would land OUTSIDE this folder is
 * redirected to <folder>/outside/<device>/<path> instead (and read back from
 * there), so nothing is ever created elsewhere on the SD card.
 *
 * MIT license -- see LICENSE.
 */
#ifndef NX_PATHS_H
#define NX_PATHS_H

#include <stddef.h>

/* Compute the folder (idempotent). Called at the very start of userAppInit and
 * main, before any other thread exists; every accessor also calls it lazily. */
void nx_paths_init(void);

const char *nx_data_dir(void);          /* e.g. "sdmc:/switch/btd5" (no trailing '/') */
const char *nx_data_dir_source(void);   /* "launch path" / "working directory" / "built-in default" */

/* "<folder>/<name>" into buf; NULL if it does not fit. */
char *nx_data_file(const char *name, char *buf, size_t cap);
/* The same, relative to the SD card for fs-service calls ("/switch/btd5/<name>");
 * NULL if the folder is not on sdmc: or it does not fit. */
char *nx_data_fs_path(const char *name, char *buf, size_t cap);
/* Create the folder (and its parents) through the fs service: safe before
 * main(), and never hands newlib a device-root path. */
void nx_data_dir_ensure(void);

/* "dev:/a/./b//../c" -> "dev:/a/c". Returns 0, or -1 if it does not fit. A ".."
 * never climbs above the device root. */
int nx_path_normalize(const char *in, char *out, size_t cap);
/* 1 if a NORMALIZED path is the folder or inside it (case-insensitive: FAT). */
int nx_path_inside_data(const char *normalized);

#endif
