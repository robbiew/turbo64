/* CONFIG device-spec grammar: "device[;drive:[;loc]]". */
#include "bbs/devspec.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* devspec_parse is boot-only (called once from cfg_init's parse path) and
 * lives in the ovl_boot overlay, same as its predecessor in cfg.c; see the
 * placement note there. devspec_format stays resident (editor + boot).
 * Pragmas bind at the definition site, not by #ifdef branch, so the
 * T64_STORE_SEQ split below stays nested inside the existing boot/resident
 * pragma switches rather than appending a second switch pair after them. */
#ifdef T64_BOOT_OVERLAY
#pragma code(boot_code)
#pragma data(boot_data)
#endif

#ifndef T64_STORE_SEQ
bool_t devspec_parse(const char *value, u8 *device, u8 *drive,
                     char *loc, u8 loc_len) {
  const char *p;
  const char *q;

  if (!value || !device || !drive || !loc || loc_len == 0) {
    return FALSE;
  }

  p = value;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == '\0') return FALSE;

  *device = (u8)atoi(p);
  *drive = 0;
  loc[0] = '\0';

  q = p;
  while (*q && *q != ';' && *q != '\r' && *q != '\n' && *q != ' ' && *q != '\t') {
    q++;
  }
  if (*q != ';') {
    return TRUE;
  }

  p = q + 1;
  *drive = (u8)atoi(p);
  while (*p && *p != ':' && *p != '\r' && *p != '\n' && *p != ' ' && *p != '\t') {
    p++;
  }
  if (*p != ':') {
    return TRUE;
  }

  p++;
  if (*p == ';') {
    p++;
  }
  while (*p == ' ' || *p == '\t') p++;
  if (*p != '\0') {
    u8 len = (u8)strlen(p);
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t' ||
                       p[len - 1] == '\r' || p[len - 1] == '\n')) {
      len--;
    }
    if (len >= loc_len) {
      len = (u8)(loc_len - 1);
    }
    strncpy(loc, p, len);
    loc[len] = '\0';
  }

  return TRUE;
}
#else
bool_t devspec_parse(const char *value, u8 *device, u8 *drive,
                     char *loc, u8 loc_len) {
  const char *p;
  u8 len;

  (void)drive;   /* section index is owned by cfg_set_defaults() */

  if (!value || !device || !drive || !loc || loc_len == 0) {
    return FALSE;
  }

  p = value;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == '\0') return FALSE;

  *device = (u8)atoi(p);
  loc[0] = '\0';

  while (*p && *p != ';') p++;
  if (*p != ';') return TRUE;

  p++;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == '\0') return TRUE;

  len = (u8)strlen(p);
  while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t' ||
                     p[len - 1] == '\r' || p[len - 1] == '\n')) {
    len--;
  }
  if (len >= loc_len) {
    len = (u8)(loc_len - 1);
  }
  strncpy(loc, p, len);
  loc[len] = '\0';

  return TRUE;
}
#endif

#ifdef T64_BOOT_OVERLAY
#pragma code(code)
#pragma data(data)
#endif

#ifndef T64_STORE_SEQ
void devspec_format(char *buf, u8 device, u8 drive, const char *loc) {
  if (!buf) {
    return;
  }
  if (loc && loc[0] != '\0') {
    sprintf(buf, "%u;%u:;%s", (unsigned)device, (unsigned)drive, loc);
  } else if (drive != 0) {
    sprintf(buf, "%u;%u:", (unsigned)device, (unsigned)drive);
  } else {
    sprintf(buf, "%u", (unsigned)device);
  }
}
#else
void devspec_format(char *buf, u8 device, u8 drive, const char *loc) {
  (void)drive;
  if (!buf) return;
  if (loc && loc[0] != '\0') {
    sprintf(buf, "%u;%s", (unsigned)device, loc);
  } else {
    sprintf(buf, "%u", (unsigned)device);
  }
}
#endif
