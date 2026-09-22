/* bbs/hal/disk.h - CBM IEC disk I/O abstraction.
 *
 * Wraps oscar64 kernalio for all BBS sequential disk access.
 * Every call takes an explicit device number — no global device state —
 * to support the multi-device layout (T64_DRIVE_SYSTEM, T64_DRIVE_MSGS, etc.)
 *
 * Channel numbers used internally:
 *   CFG_FNUM_CMD  (15)  drive command / status channel
 *   CFG_FNUM_DATA  (8)  primary data channel
 *   CFG_FNUM_AUX   (1)  secondary / aux channel
 *
 * Sequential file naming on disk always uses the literal CBM *drive* 0
 * (a 1581 only exposes drive 0 — there is no such thing as "drive 1" on
 * this hardware):
 *   "0:name,S,R"  read
 *   "0:name,S,W"  write (truncate)
 *   "0:name,S,A"  append
 *   "@0:name,S,W" write (overwrite existing)
 * The `drive` parameter below is a drive *partition*, selected separately
 * via disk_select_partition() (the "CP<n>" command channel command) before
 * the filename is opened — see disk_select_partition() for why.
 *
 * In T64_STORE_SEQ builds `drive` is instead a SECTION INDEX (0=system,
 * 1=msgs, 2=files, 3=doors, 4=gfiles) and disk_select_partition() issues an absolute
 * "CD:<path>" rather than "CP<n>". Every caller passes the same value in
 * both builds; only the resolution changes.
 *
 * REL file access is handled by bbs/rel.h, not this module.
 */
#ifndef BBS_HAL_DISK_H
#define BBS_HAL_DISK_H

#include "bbs/types.h"
#include "bbs/err.h"

typedef enum {
    DISK_READ   = 0,   /* Sequential read  ("S,R") */
    DISK_WRITE  = 1,   /* Sequential write ("S,W") — creates or truncates */
    DISK_APPEND = 2,   /* Sequential append ("S,A") */
    DISK_OVER   = 3    /* Sequential overwrite ("@x:name,S,W") */
} disk_mode_t;

/* Last drive error message (set on BBS_EIO from any disk_ call). */
extern char disk_errmsg[40];

/* -----------------------------------------------------------------------
 * Sequential file I/O
 * ----------------------------------------------------------------------- */

/* Open a sequential file on `device`, drive partition `drive`, with the
 * given CBM `name` (no prefix — drive prefix added automatically).
 * Returns BBS_OK or BBS_EIO / BBS_ENOTFOUND. */
bbs_err_t disk_open(u8 device, u8 drive, const char *name, disk_mode_t mode);

/* Close data and command channels. */
void disk_close(void);

/* Read one byte. Returns -1 on EOF/error. */
i16 disk_getc(void);

/* Read up to `len` raw bytes into buf. Returns bytes read (0 = EOF, -1 = error).
 * Uses krnio_read() — one CHKIN + N×CHRIN + one CLRCHN per call.
 * Much faster than disk_getc() for bulk sequential reads. */
i16 disk_read(u8 *buf, u8 len);

/* Read a CR-terminated line into buf[len]; strips CR; NUL-terminates.
 * Returns bytes read, or -1 on EOF/error. */
i16 disk_gets(char *buf, u8 len);

/* Write one byte. Returns BBS_OK or BBS_EIO. */
bbs_err_t disk_putc(char c);

/* Write up to `len` raw bytes from buf. Returns BBS_OK or BBS_EIO.
 * Uses krnio_write() — one CHKOUT + N×CHROUT + one CLRCHN per call.
 * Much faster than disk_putc() for bulk sequential writes. */
bbs_err_t disk_write(const u8 *buf, u8 len);

/* Write NUL-terminated string. Returns BBS_OK or BBS_EIO. */
bbs_err_t disk_puts(const char *s);

/* Write string followed by CR (CBM seq-file line terminator). */
bbs_err_t disk_putline(const char *s);

/* Returns TRUE if the current file is at EOF. */
bool_t disk_eof(void);

/* -----------------------------------------------------------------------
 * File management
 * ----------------------------------------------------------------------- */

/* Scratch (delete) a file. */
bbs_err_t disk_scratch(u8 device, u8 drive, const char *name);

/* Rename a file. */
bbs_err_t disk_rename(u8 device, u8 drive,
                      const char *old_name, const char *new_name);

/* Send a raw command string to the drive command channel (e.g. "I0"). */
bbs_err_t disk_cmd(u8 device, const char *cmd);

/* Select the storage location `partition` addresses on `device`. Partitions
 * are persistent drive state (not filename state), so this must be called
 * before any filename that assumes a given partition is current. Caches the
 * last selected (device, partition) pair and is a no-op when already
 * selected. The command sent, and what `partition == 0` means, differ by
 * build:
 *
 * REL (default) build: `partition` is a CP<n> drive partition, sent via the
 * "CP<n>" command channel command. `partition == 0` always sends nothing —
 * see disk.c for why that must stay byte-for-byte quiet.
 *
 * T64_STORE_SEQ build: `partition` is a SECTION INDEX (0=system, 1=msgs,
 * 2=files, 3=doors, 4=gfiles) and this issues an absolute "CD:<path>" to the
 * folder registered for that index via disk_set_section_path(). 0 (system)
 * is an ordinary section like any other, not a no-op case; this sends
 * nothing only when the index has no path registered yet (e.g. before
 * cfg_init() has run). */
bbs_err_t disk_select_partition(u8 device, u8 partition);

/* Load an overlay PRG (a P"OVL_..." literal) from bbs_cfg.device_system,
 * positioning the drive cursor at the system section first under
 * T64_STORE_SEQ (a no-op in the REL build). See disk.c for the full
 * rationale. Returns BBS_EIO if positioning or the load fails — callers
 * must not run into the overlay region in that case. */
bbs_err_t disk_load_overlay(const char *name);

#ifdef T64_STORE_SEQ
/* Register the absolute folder path for section `index` (0=system, 1=msgs,
 * 2=files, 3=doors, 4=gfiles). Stores the POINTER — the caller must keep the string
 * alive for the lifetime of the program (bbs_cfg does). Called once per
 * section after config load. */
void disk_set_section_path(u8 index, const char *path);
#endif

/* Restore the drive's persistent navigation cursor to "home" — wherever the
 * install's own binaries live — before the process exits. See disk.c for
 * why every disk-touching exit path needs this and what "home" resolves to
 * in each build:
 *   T64_STORE_SEQ  — the section tree root (bbs_cfg.init_system with its
 *                    last /-component stripped). No-op if init_system has no
 *                    parent component (e.g. cfg_init() hasn't run yet).
 *   REL (default)  — bbs_cfg.drive_system, via disk_select_partition(). A
 *                    correct no-op both pre-cfg_init() (drive_system is
 *                    still its zero default) and for a legitimate
 *                    partition-0 install (disk_select_partition() already
 *                    sends nothing for partition 0 — see its comment). */
void disk_reset_cursor_root(u8 device);

/* Read drive status into disk_errmsg. Returns numeric error code (0=OK). */
u8 disk_status(u8 device);

/* -----------------------------------------------------------------------
 * Filename builder helpers
 * ----------------------------------------------------------------------- */

/* Build bulletin filename: "B.<board>.<post>" into buf. */
void disk_name_bull(char *buf, u8 board, u16 post);

/* Build mail filename: "E.<msg_id>.<user_id>" into buf. */
void disk_name_mail(char *buf, u16 msg_id, u8 user_id);

/* Terminal-aware filename builder (Phase B).
 * Generates candidate filenames with a given prefix:
 *   <pfx>.<name> <mode> <width>  (most specific)
 *   <pfx>.<name> <mode>          (mode only)
 *   <pfx>.<name> <width>         (width only)
 *   <pfx>.<name>                 (generic fallback)
 * Prefix conventions: 'g'=gfile, 'm'=menu, 'p'=prompt.
 *
 * Parameters:
 *   out    - output struct receiving 4 candidate names
 *   prefix - file type prefix char ('g', 'm', 'p', etc.)
 *   name   - base name (e.g., "login", "main", "msgs")
 *   mode   - terminal mode (0=PETSCII, 1=ANSI/CP437, 2=ASCII)
 *   width  - terminal width (40 or 80)
 */
typedef struct {
  char names[4][48];  /* 4 candidates in priority order */
} term_filename_t;

void disk_build_term_filename(term_filename_t *out,
                              char prefix,
                              const char *name,
                              u8 mode, u8 width);

/* Open a sequential file with fallback chain (Phase B).
 * Tries candidates in priority order: specific → mode → width → generic.
 * Uses session terminal settings to determine candidates.
 *
 * Parameters:
 *   device    - disk device
 *   drive     - partition number
 *   base_name - base filename (e.g., "login")
 *   mode      - DISK_READ, DISK_WRITE, etc.
 *   term_mode - terminal mode from session->term_mode
 *   term_width- terminal width from session->term_width
 *
 * Returns:
 *   BBS_OK        - file opened and ready
 *   BBS_ENOTFOUND - no candidate file found
 *   BBS_EIO       - error opening file
 */
bbs_err_t disk_open_with_fallback(u8 device, u8 drive,
                                  char prefix,
                                  const char *base_name,
                                  disk_mode_t mode,
                                  u8 term_mode, u8 term_width);

/* -----------------------------------------------------------------------
 * Disk Statistics
 * ----------------------------------------------------------------------- */

/* Get disk free space in blocks (254-byte blocks, CBM convention).
 * Returns BBS_OK or BBS_EIO. */
bbs_err_t disk_free_blocks(u8 device, u16 *out_free);

/* Get disk total capacity in blocks.
 * Returns BBS_OK or BBS_EIO. */
bbs_err_t disk_total_blocks(u8 device, u16 *out_total);

#endif /* BBS_HAL_DISK_H */
