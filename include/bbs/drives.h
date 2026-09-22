/* bbs/drives.h - compile-time drive assignments.
 *
 * Override at build time:  make c64 T64_DRIVE_MSGS=9
 * Default: all areas on drive 8 (single-drive, backward-compatible). */
#ifndef BBS_DRIVES_H
#define BBS_DRIVES_H

#ifndef T64_DRIVE_SYSTEM
#define T64_DRIVE_SYSTEM  8   /* accounts, config, log, bulletins */
#endif
#ifndef T64_DRIVE_MSGS
#define T64_DRIVE_MSGS    8   /* message base */
#endif
#ifndef T64_DRIVE_FILES
#define T64_DRIVE_FILES   8   /* file library (uploads/downloads) */
#endif
#ifndef T64_DRIVE_DOORS
#define T64_DRIVE_DOORS   10  /* door PRG files */
#endif
#ifndef T64_DRIVE_GFILES
#define T64_DRIVE_GFILES  T64_DRIVE_SYSTEM  /* default: alongside system files */
#endif

#ifndef T64_DRIVE_SOFTIEC
#define T64_DRIVE_SOFTIEC 11  /* Ultimate Software IEC virtual drive */
#endif

#endif /* BBS_DRIVES_H */
