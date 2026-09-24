/* nk_save.h -- Ninja Kiwi "DGDATA" saves: codec + in-place JSON editing.
 *
 * Pure C, no Switch dependencies (tools/savetool_test builds it on a PC and
 * runs it against real saves).
 *
 * FILE FORMAT (same engine as Bloons Supermonkey 2; see smk2_nx), verified
 * here against Bloons TD 5 4.7 saves -- every stored checksum matches and
 * re-encoding reproduces each file byte for byte:
 *
 *     offset 0   "DGDATA"
 *     offset 6   %08x checksum of the PLAINTEXT
 *     offset 14  JSON, each byte shifted by 21 + (i % 6)
 *
 * The checksum is a reflected CRC-32 (poly 0xEDB88320) with init 0, no final
 * XOR, and -- the engine's own quirk -- rounds 2-8 of each byte using an
 * ARITHMETIC shift. No stock CRC matches; nk_crc() spells it out.
 *
 * EDITING. Values are located by path and their exact byte span is replaced;
 * nothing else in the document changes. Paths:
 *
 *     Items.MonkeyMoney                      object members, '.'-separated
 *     UnlockedTowers[3]                      array element by index
 *     Levels.Towers[Type=DartMonkey].XP      array element whose "Type" is the
 *                                            string "DartMonkey"
 *
 * Setters check the existing value's JSON type (a number stays a number, a
 * bool stays a bool) and refuse anything else, so a path that means something
 * different in some other save cannot be clobbered.
 *
 * MIT license -- see LICENSE.
 */
#ifndef NK_SAVE_H
#define NK_SAVE_H

#include <stddef.h>
#include <stdint.h>

#define NK_HDR 14

uint32_t nk_crc(const unsigned char *d, size_t n);

/* raw file -> JSON text in `out` (NUL-terminated). Returns the JSON length, or
 * -1 if this is not a DGDATA file. *crc_ok says whether the stored checksum
 * matched (a mismatch is reported, not fatal). */
long nk_decode(const unsigned char *raw, size_t n, char *out, size_t cap, int *crc_ok);
/* JSON text -> raw file bytes. Returns the file length, or -1 if it does not fit. */
long nk_encode(const char *json, size_t n, unsigned char *out, size_t cap);

/* 1 if the whole buffer is exactly one well-formed JSON value. */
int nkj_valid(const char *j, size_t n);

/* Locate the value at `path`. Returns 1 and its [start,end) byte offsets. */
int nkj_find(const char *j, size_t n, const char *path, size_t *vs, size_t *ve);

/* Read the whole-number value at `path` into *out. Returns 1 if it is one. */
int nkj_get_int(const char *j, size_t n, const char *path, long long *out);

/* Edits. `n` is updated; `cap` bounds the buffer (text + NUL). Each returns the
 * number of values changed (0 if the path is absent or of the wrong type). */
int nkj_set_int(char *j, size_t *n, size_t cap, const char *path, long long v);
int nkj_set_bool(char *j, size_t *n, size_t cap, const char *path, int v);
int nkj_set_array_bools(char *j, size_t *n, size_t cap, const char *path, int v);

/* 1 if the array at `path` has the string element `str`. */
int nkj_array_has_string(const char *j, size_t n, const char *path, const char *str);
/* Append the string `str` to the array at `path` unless it is already there.
 * `str` must be a plain identifier (letters, digits, '_', '-', '.'): nothing
 * is escaped. Returns 1 if added. */
int nkj_array_add_string(char *j, size_t *n, size_t cap, const char *path, const char *str);

/* Number of elements (array) or members (object) at `path`; -1 if absent. */
int nkj_count(const char *j, size_t n, const char *path);
/* Name of the idx-th member of the object at `path` into out. Returns 1. */
int nkj_member_name(const char *j, size_t n, const char *path, int idx, char *out, size_t cap);

#endif
