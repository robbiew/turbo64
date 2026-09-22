/* bbs/devspec.h - CONFIG device-spec grammar. Pure parse/format, no I/O. */
#ifndef BBS_DEVSPEC_H
#define BBS_DEVSPEC_H

#include "bbs/types.h"

/* Minimum size for devspec_format()'s output buffer. */
#define DEVSPEC_BUF_MAX 40

/* Parse a device spec into its parts.
 *   REL grammar:   "8;2:;I0"            -> device=8, drive=2, loc="I0" (DOS init command)
 *   SIEC grammar:  "11;/USB1/BBS/SYSTEM" -> device=11, loc="/USB1/BBS/SYSTEM" (T64_STORE_SEQ)
 * Under T64_STORE_SEQ, `*drive` is never written — section indices are owned
 * by cfg_set_defaults(), not by this grammar.
 * `loc` is always NUL-terminated and never written past `loc_len`.
 * Returns FALSE on a NULL argument, a zero-length buffer, or an empty spec. */
bool_t devspec_parse(const char *value, u8 *device, u8 *drive,
                     char *loc, u8 loc_len);

/* Inverse of devspec_parse(). `buf` must hold at least DEVSPEC_BUF_MAX bytes. */
void devspec_format(char *buf, u8 device, u8 drive, const char *loc);

#endif /* BBS_DEVSPEC_H */
