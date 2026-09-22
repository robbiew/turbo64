/* SEQ + REU implementation of bbs/rel.h for the Ultimate Software IEC.
 *
 * WHY this exists: REL files do not work over SoftIEC — the open succeeds at
 * KERNAL level and every record operation then returns DOS 61. Measured, with
 * a 1581 control run, in docs/probe-results/FINDINGS.md. */
#include "bbs/rel.h"
#include "bbs/seq_region.h"
#include "bbs/config.h"
#include "bbs/cfg.h"
#include "bbs/hal/disk.h"
#include "bbs/hal/reu.h"
#include <string.h>
#include <stdio.h>

#define REL_SEQ_HANDLE 0u

static bool_t s_open      = FALSE;
static u8     s_region    = SEQ_REGION_NONE;
static u8     s_recsize   = 0;
static u16    s_pos       = 0;      /* 1-based current record */
static u16    s_base      = 0;      /* seq_region_offset(s_region), cached at open */
static u16    s_max_recs  = 0;      /* seq_region_capacity(s_region) / s_recsize */
static u16    s_off       = 0;      /* byte offset of s_pos; set by rel_position, advanced by rel_read */
static bool_t s_dirty     = FALSE;
static u8     s_device    = 0;
static u8     s_partition = 0;
static char   s_name[SEQ_NAME_MAX];

/* Which region is resident, and whether it came off disk this session.
 * The fixed sets stay loaded once read; WINDOW is reloaded per name. */
static u8   s_loaded[REGION_COUNT_MAX];
static char s_window_name[SEQ_NAME_MAX];

/* Records present per region (high-water mark) — MUST be per-region, not a
 * single scalar: region_load() is skipped whenever a fixed region is
 * already s_loaded[], or a WINDOW name repeats, so a single shared counter
 * leaks the previous region's count into the next one. Concretely: BOARDS
 * loads (count=5), B1.IDX loads (count=150), BOARDS reopens — load skipped,
 * a single scalar would still read 150, and a flush would stream 150x44
 * bytes from an 880-byte region straight past its neighbours (reu_data_get
 * has no bounds check). This array is 2*REGION_COUNT_MAX = 18 bytes; the
 * single scalar it replaces was 2, so the net BSS cost is +16 bytes. */
static u16 s_count_by_region[REGION_COUNT_MAX];

static u8 s_io[64];

/* Definitions for the shared scratch declared in bbs/rel.h — see that
 * comment for why one process-wide instance (and three flat globals rather
 * than a struct) is safe and necessary. Declared here, alongside the
 * backend they protect, ahead of the boot_code pragma switch below so they
 * land in resident bss regardless of which overlay bank last loaded (every
 * data-layer caller, in any bank, must see the same addresses). */
u8        rel_scratch_buf[REL_SCRATCH_SIZE];
u8        rel_scratch_got;
bbs_err_t rel_scratch_err;

static bbs_err_t region_load(void)
{
    u16 cap = seq_region_capacity(s_region);
    u16 total = 0;
    // cppcheck-suppress variableScope
    i16 got;

    if (disk_open(s_device, s_partition, s_name, DISK_READ) != BBS_OK) {
        /* A real I/O failure (device didn't answer at all), NOT "file
         * absent" — disk_open() succeeds on the plain not-found case (see
         * DOS 62 below); it only fails this way for a genuine fault. Do
         * not cache this as an empty region: rel_open() only sets
         * s_loaded[] on BBS_OK, so returning an error here leaves the
         * region retryable on the next open instead of permanently empty
         * for the rest of the run. */
        return BBS_EIO;
    }
    if (disk_status(s_device) == 62) {
        disk_close();
        s_count_by_region[s_region] = 0;
        return BBS_OK;      /* genuinely absent == empty set, as rel_open creates */
    }

    for (;;) {
        got = disk_read(s_io, sizeof(s_io));
        if (got <= 0) break;
        if (total + (u16)got > cap) {
            got = (i16)(cap - total);
            if (got <= 0) break;
        }
        reu_data_put((u16)(s_base + total), s_io, (u16)got);
        total += (u16)got;
    }
    disk_close();

    s_count_by_region[s_region] = (u16)(total / s_recsize);
    return BBS_OK;
}

static bbs_err_t region_flush(void)
{
    char tmp[SEQ_NAME_MAX];
    u16 total;
    u16 done = 0;
    // cppcheck-suppress variableScope
    u16 chunk;

    /* Must come before any s_count_by_region[s_region] access: rel_seq_flush()
     * is a public entry point with no caller yet (Task 8), and s_region is
     * SEQ_REGION_NONE (0xFF) until the first rel_open() — indexing the
     * 9-element array with that before confirming a region is actually
     * open/dirty would read out of bounds. */
    if (!s_dirty) return BBS_OK;
    total = (u16)(s_count_by_region[s_region] * s_recsize);

    if (!seq_tmp_name(tmp, s_name)) return BBS_EIO;

    /* SoftIEC ignores the "@" replace prefix (returns 63), so scratch first. */
    disk_scratch(s_device, s_partition, tmp);

    if (disk_open(s_device, s_partition, tmp, DISK_WRITE) != BBS_OK) {
        return BBS_EIO;
    }
    while (done < total) {
        chunk = (u16)(total - done);
        if (chunk > sizeof(s_io)) chunk = sizeof(s_io);
        reu_data_get((u16)(s_base + done), s_io, chunk);
        if (disk_write(s_io, (u8)chunk) != BBS_OK) {
            disk_close();
            return BBS_EIO;
        }
        done += chunk;
    }
    disk_close();

    if (disk_scratch(s_device, s_partition, s_name) != BBS_OK) {
        /* Absent is fine — this is the first write of a new set. */
        if (disk_status(s_device) != 62) return BBS_EIO;
    }
    if (disk_rename(s_device, s_partition, tmp, s_name) != BBS_OK) {
        return BBS_EIO;
    }

    s_dirty = FALSE;
    return BBS_OK;
}

bbs_err_t rel_seq_flush(void) { return region_flush(); }

/* rel_seq_recover() and seq_name_exists() run only from the Task 8 boot
 * recovery sweep — once per name, at startup, never during a session — so
 * they are boot-overlay-only, same as devspec_parse() (src/data/devspec.c).
 * Pragmas bind at the definition site and are not scoped, so the switch back
 * to resident placement below MUST land before rel_open(): everything
 * between the two switches ends up in ovl_boot. */
#ifdef T64_BOOT_OVERLAY
#pragma code(boot_code)
#pragma data(boot_data)
#endif

/* disk_open() succeeding is not evidence a file exists (KERNAL OPEN answers
 * for any device that responds); only DOS 62 is trustworthy. A disk_open()
 * failure is treated as absent, matching region_load()'s convention. */
static bool_t seq_name_exists(u8 device, u8 partition, const char *name)
{
    bool_t exists;
    if (disk_open(device, partition, name, DISK_READ) != BBS_OK) return FALSE;
    exists = (disk_status(device) != 62) ? TRUE : FALSE;
    disk_close();
    return exists;
}

bbs_err_t rel_seq_recover(u8 device, u8 partition, const char *name)
{
    char tmp[SEQ_NAME_MAX];
    bool_t name_exists, tmp_exists;

    if (!name || !seq_tmp_name(tmp, name)) return BBS_EIO;
    if (disk_select_partition(device, partition) != BBS_OK) return BBS_EIO;

    /* .NEW absent is the common case (every non-crash boot) — one probe,
     * not two, matching the measured 0.25s/10-probe boot-time budget. */
    tmp_exists = seq_name_exists(device, partition, tmp);
    if (!tmp_exists) return BBS_OK;
    name_exists = seq_name_exists(device, partition, name);

    switch (seq_recover_action(name_exists, tmp_exists)) {
    case SEQ_RECOVER_DROP_TMP: return disk_scratch(device, partition, tmp);
    case SEQ_RECOVER_PROMOTE:  return disk_rename(device, partition, tmp, name);
    default:                   return BBS_OK;
    }
}

/* Directory listings do not work over SoftIEC (LOAD"$",11 returns only the
 * header and free-blocks line, no entries — measured on hardware), so this
 * probes known names instead of scanning: the eight fixed sets, then UD<n>/
 * B<n>.IDX up to the configured maxima, whether or not those areas/boards
 * are actually in use. bbs_cfg must already be populated by cfg_init().
 *
 * drive_* is a section-folder index under T64_STORE_SEQ, not an alias —
 * system/msgs/files/doors are genuinely different directories. Each fixed
 * name is swept against the same (device, drive) pair its own rel_open()
 * call site uses, verified directly rather than assumed:
 *   USR LOG/USR PROF (users.c via user_open_rel), USR.DAY (usrday.c),
 *   VOTE1 (votes.c)                                        -> drive_system
 *   USR.PTR (usrptr.c), BOARDS (boards.c)                  -> drive_msgs
 *   UDS (file_areas.c)                                     -> drive_files
 *   DOORS (doors.c)                                        -> drive_doors
 * Sweeping any of these against the wrong folder silently no-ops instead
 * of finding a real crash-orphaned .NEW. */
void rel_seq_sweep(void)
{
    static const char * const sys_set[] = {
        "USR LOG", "USR PROF", "USR.DAY", "VOTE1"
    };
    static const char * const msg_set[] = {
        "USR.PTR", "BOARDS"
    };
    char name[SEQ_NAME_MAX];
    u8 i;

    for (i = 0; i < 4; i++) {
        (void)rel_seq_recover(bbs_cfg.device_system, bbs_cfg.drive_system, sys_set[i]);
    }
    for (i = 0; i < 2; i++) {
        (void)rel_seq_recover(bbs_cfg.device_msgs, bbs_cfg.drive_msgs, msg_set[i]);
    }
    (void)rel_seq_recover(bbs_cfg.device_files, bbs_cfg.drive_files, "UDS");
    (void)rel_seq_recover(bbs_cfg.device_doors, bbs_cfg.drive_doors, "DOORS");

    for (i = 1; i <= CFG_MAX_FILE_AREAS; i++) {
        sprintf(name, "UD%u", (unsigned)i);
        (void)rel_seq_recover(bbs_cfg.device_files, bbs_cfg.drive_files, name);
    }
    for (i = 1; i <= CFG_MAX_BOARDS; i++) {
        sprintf(name, "B%u.IDX", (unsigned)i);
        (void)rel_seq_recover(bbs_cfg.device_msgs, bbs_cfg.drive_msgs, name);
    }
}

/* rel_seq_require_storage() — boot-time hard gate, called once from
 * boot_sequence() (now itself boot_code — see main.c) before rel_seq_sweep()
 * above. Two things turn silent data loss into a legible error:
 *   1. No REU means rel_open() (below, once resident) returns BBS_EIO for
 *      every record set — the BBS would otherwise run with no database.
 *   2. No T64.SIEC marker (first line "T64SEQ1", written by the migration
 *      tool) means the folder tree was copied but never migrated — booting
 *      straight into that would look like a fresh, empty install rather
 *      than the sysop's real one.
 * Order matters: REU first, since the marker probe goes through disk_open()
 * and a missing REU makes the whole storage layer unusable regardless of
 * what the marker says.
 *
 * printf() is used directly rather than main.c's main_print() wrapper
 * (printf("%s",...)) since main_print is static to main.c and not visible
 * here; the boot console takes plain printf() output identically. */
void rel_seq_require_storage(void)
{
    (void)reu_detect();
    if (!reu_data_available()) {
        printf("\rREU REQUIRED - ENABLE IN ULTIMATE MENU\r");
        for (;;) { }
    }
    {
        bool_t ok = FALSE;
        /* disk_open() returning BBS_OK only proves the device answered, not
         * that the file exists (KERNAL OPEN succeeds regardless) — DOS 62 is
         * the only trustworthy "not found" signal. */
        if (disk_open(bbs_cfg.device_system, bbs_cfg.drive_system,
                      "T64.SIEC", DISK_READ) == BBS_OK &&
            disk_status(bbs_cfg.device_system) != 62) {
            /* disk_gets() keeps the CR terminator, so this is strncmp(7),
             * not strcmp — a documented past defect (probe-results/
             * FINDINGS.md): comparing against a string that arrives with a
             * trailing CR. */
            char line[16];
            if (disk_gets(line, sizeof(line)) > 0 &&
                strncmp(line, "T64SEQ1", 7) == 0) {
                ok = TRUE;
            }
        }
        disk_close();
        if (!ok) {
            printf("\rUNMIGRATED INSTALL\r");
            printf("RUN: TOOLS/MIGRATE-D81.PY\r");
            /* The marker open above CD'd SoftIEC into the section, and that
             * cursor survives a reset: put it back at the tree root so the
             * operator's next LOAD"BOOT-SIEC" after fixing the tree works. */
            disk_reset_cursor_root(bbs_cfg.device_system);
            for (;;) { }
        }
    }
}

#ifdef T64_BOOT_OVERLAY
#pragma code(code)
#pragma data(data)
#endif

bbs_err_t rel_open(u8 device, u8 partition, const char *name,
                   u8 record_size, rel_handle_t *out)
{
    u8 region;

    if (!name || !out || record_size == 0) return BBS_EIO;
    if (s_open) return BBS_EIO;
    if (!reu_data_available()) return BBS_EIO;

    if (s_dirty) {
        /* A previous rel_close()'s flush failed and left live REU data
         * unpersisted for whichever region was open at the time — s_region/
         * s_name/s_base/s_recsize below still hold that region's values,
         * nothing has overwritten them yet. Retry the flush now, before
         * they are replaced: for the shared WINDOW region in particular,
         * region_load() below would otherwise overwrite the still-pending
         * data with a different name's bytes read from disk, losing it
         * permanently rather than just leaving it unflushed one more time. */
        bbs_err_t ferr = region_flush();
        if (ferr != BBS_OK) return ferr;
    }

    region = seq_region_for_name(name);
    if (region == SEQ_REGION_NONE) return BBS_EIO;
    if (strlen(name) >= SEQ_NAME_MAX) return BBS_EIO;

    if (disk_select_partition(device, partition) != BBS_OK) return BBS_EIO;

    s_region = region;
    s_recsize = record_size;
    s_base = seq_region_offset(region);
    s_max_recs = (u16)(seq_region_capacity(region) / record_size);
    s_device = device;
    s_partition = partition;
    /* rel.c's contract: rel_open() itself leaves the file positioned at
     * record 1 (CBM DOS convention — see src/data/users.c's login-path
     * comment), so a caller that never calls rel_position() can still
     * read/write straight away. Must come after s_base/s_recsize above. */
    s_pos = 1;
    s_off = s_base;
    s_dirty = FALSE;
    strcpy(s_name, name);

    if (region == SEQ_REGION_WINDOW) {
        if (strcmp(s_window_name, name) != 0) {
            bbs_err_t err = region_load();
            if (err != BBS_OK) return err;
            strcpy(s_window_name, name);
        }
    } else if (!s_loaded[region]) {
        bbs_err_t err = region_load();
        if (err != BBS_OK) return err;
        s_loaded[region] = 1;
    }

    s_open = TRUE;
    *out = REL_SEQ_HANDLE;
    return BBS_OK;
}

bbs_err_t rel_position(rel_handle_t h, u16 rec)
{
    (void)h;
    if (!s_open || rec == 0) return BBS_EIO;
    s_pos = rec;
    s_off = (u16)(s_base + (u16)(rec - 1) * s_recsize);
    return BBS_OK;
}

/* CBM REL semantics (src/hal/rel.c reads/writes straight off the open KERNAL
 * data channel): a successful read OR write advances the position to the
 * next record, and rel_open() itself leaves the file positioned at record 1
 * (see the comment there). So a caller can loop rel_read()/rel_write() with
 * no rel_position() call at all — src/data/users.c's login-path comment
 * documents the read side; src-editor/setup.c's user/profile database
 * initializers write USERS_MAX records in a `for` loop with zero
 * rel_position() calls anywhere in the file, relying entirely on
 * rel_open()'s implicit record 1 plus this advance. rel_write() gets the
 * same treatment as rel_read() below. */
bbs_err_t rel_read(rel_handle_t h, void *buf, u8 record_size, u8 *got)
{
    (void)h;
    if (!s_open || !buf || s_pos == 0) return BBS_EIO;
    if (got) *got = 0;
    if (s_pos > s_count_by_region[s_region]) return BBS_ENOTFOUND;

    reu_data_get(s_off, buf, record_size);
    if (got) *got = record_size;

    s_pos++;
    s_off = (u16)(s_off + s_recsize);
    return BBS_OK;
}

bbs_err_t rel_write(rel_handle_t h, const void *buf, u8 record_size)
{
    (void)h;
    if (!s_open || !buf || s_pos == 0) return BBS_EIO;
    if (s_pos > s_max_recs) return BBS_EFULL;

    /* Extending past the high-water mark zero-fills the gap, so a sparse
     * write behaves the way it does against a preallocated REL file.
     * s_io is only 64 bytes, but s_recsize can be up to 100 (RECORD_SIZE_
     * FILE_ENTRY) — a fixed per-record memset/DMA of s_recsize bytes (the
     * previous shape here) overran s_io by up to 36 bytes on every sparse
     * USR PROF/UD<n> write, corrupting whatever follows it in bss (as of
     * this file, rel_scratch_buf). Chunk against sizeof(s_io) instead,
     * the way region_flush() already does for its own DMA loop — since
     * every byte written is zero, record boundaries don't need to line up
     * with chunk boundaries, only the total byte count does. */
    if (s_count_by_region[s_region] + 1 < s_pos) {
        u16 fill_off = (u16)(s_base + (u16)s_count_by_region[s_region] * s_recsize);
        u16 remaining = (u16)((u16)((s_pos - 1) - s_count_by_region[s_region]) * s_recsize);
        // cppcheck-suppress variableScope
        u16 chunk;
        memset(s_io, 0, sizeof(s_io));
        while (remaining) {
            chunk = (remaining > sizeof(s_io)) ? (u16)sizeof(s_io) : remaining;
            reu_data_put(fill_off, s_io, chunk);
            fill_off = (u16)(fill_off + chunk);
            remaining = (u16)(remaining - chunk);
        }
        s_count_by_region[s_region] = (u16)(s_pos - 1);
    }
    if (s_pos > s_count_by_region[s_region]) s_count_by_region[s_region] = s_pos;

    reu_data_put(s_off, buf, record_size);
    s_dirty = TRUE;

    s_pos++;
    s_off = (u16)(s_off + s_recsize);
    return BBS_OK;
}

/* Close after a write and keep the FIRST error: on the SEQ backend the disk
 * write happens in rel_close(), so a save path that returned rel_write()'s
 * result alone reported success even when the flush failed (PR #25 review). */
bbs_err_t rel_close_keep(rel_handle_t h, bbs_err_t err)
{
    bbs_err_t ce = rel_close(h);
    return (err != BBS_OK) ? err : ce;
}

bbs_err_t rel_close(rel_handle_t h)
{
    bbs_err_t err;
    (void)h;
    if (!s_open) return BBS_OK;
    err = region_flush();
    s_open = FALSE;
    s_pos = 0;
    return err;
}

void rel_reset(void)
{
    u8 i;
    s_open = FALSE;
    s_pos = 0;
    /* s_dirty is deliberately left untouched: if the last rel_close()'s
     * flush failed, the pending write is still live in REU only, and
     * clearing the flag here would let it be silently discarded instead of
     * retried by the next rel_open() (see the retry block there). */
    for (i = 0; i < REGION_COUNT_MAX; i++) s_loaded[i] = 0;
    s_window_name[0] = '\0';
}
