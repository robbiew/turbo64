/* bbs/rel.h - CBM relative (record) file abstraction.
 *
 * GST BBS v6.0 uses REL files for all structured data:
 *   usr log  record=23  user records
 *   boards   record=40  bulletin board directory
 *   board<n> record=40  per-board message index
 *   uds      record=40  upload/download area directory
 *   ud<n>    record=100 per-area file entries
 *   text     record=25  help/text page index
 *   vote1    record=40  vote question/answer records
 *
 * On SD2IEC / 1571 / 1581: native CBM REL file support via drive DOS.
 * Positioning uses the "P" command on the command channel:
 *   PRINT#15, "P"; CHR$(data_sa); CHR$(rec_lo); CHR$(rec_hi); CHR$(0)
 *
 * Records are 1-based (CBM convention; record 0 is unused).
 */
#ifndef BBS_REL_H
#define BBS_REL_H

#include "bbs/types.h"
#include "bbs/err.h"
#include "bbs/records.h"

/* Opaque handle: encodes the logical file number used for this REL file. */
typedef u8 rel_handle_t;
#define REL_INVALID ((rel_handle_t)0xFF)

/* Open or create a REL file on `device`, drive partition `partition`, with
 * the given bare CBM `name` (no drive prefix — added internally; a 1581
 * only exposes drive 0, so the partition is selected separately via
 * disk_select_partition() and the filename always uses the literal "0:").
 * `record_size`: 1-254 bytes.
 * If the file does not exist it is created; if it exists the record
 * size must match. Returns BBS_OK on success. */
bbs_err_t rel_open(u8 device, u8 partition, const char *name, u8 record_size,
                   rel_handle_t *out);

/* Position to record `rec` (1-based). Must be called before rel_read
 * or rel_write. */
bbs_err_t rel_position(rel_handle_t h, u16 rec);

/* Read one record (record_size bytes) from the current position into
 * `buf`. `*got` is set to the number of bytes actually read. */
bbs_err_t rel_read(rel_handle_t h, void *buf, u8 record_size, u8 *got);

/* Write one record (record_size bytes) at the current position. */
bbs_err_t rel_write(rel_handle_t h, const void *buf, u8 record_size);

/* Close the REL file handle. */
bbs_err_t rel_close(rel_handle_t h);
bbs_err_t rel_close_keep(rel_handle_t h, bbs_err_t err);

/* Force-clear the single-open guard when the underlying KERNAL channels
 * were closed by external code (e.g. after a disk_scratch + raw krnio_close). */
void rel_reset(void);

#ifdef T64_STORE_SEQ
/* Boot-time crash recovery for the SEQ backend. Not part of the REL API. */
bbs_err_t rel_seq_recover(u8 device, u8 partition, const char *name);
void      rel_seq_sweep(void);
bbs_err_t rel_seq_flush(void);

/* Boot-time hard gate: halts (does not return) if no REU is present or the
 * folder tree has no valid T64.SIEC format marker. Call once, before
 * rel_seq_sweep(), from a point where OVL_BOOT is still resident. */
void      rel_seq_require_storage(void);

/* Shared single-record scratch: rel_seq.c's rel_read()/rel_write() DMA
 * straight into/out of caller memory (reu_data_get/reu_data_put), and
 * oscar64's frame-overlay allocator can alias a caller's stack locals —
 * buffer AND status alike — with another live variable across an
 * intervening call. Proven on hardware for boot_sequence()'s USR LOG check
 * (src/main.c): making only the buffer file-static was not enough: `check`
 * and `got` were the corrupted values, not the DMA target. Every data-layer
 * accessor with the same "read/write, close, then read the result" shape
 * needs the same protection.
 *
 * One process-wide instance, not one per file/function — but "single open
 * file at a time" is not by itself why that's safe. What actually makes it
 * safe: every function that populates this scratch before calling
 * rel_open() (usrday_save, usrptr_save — pack into the shared buf, then
 * open) is safe only because rel_open()/region_load()/region_flush() never
 * touch rel_scratch_buf/got/err themselves, only their own s_io. If a
 * future rel_open()-path change ever borrows this scratch, that ordering
 * breaks silently.
 *
 * Gated on T64_STORE_SEQ for budget reasons, not because REL is immune to
 * the underlying hazard. The mechanism proven on hardware in
 * boot_sequence()'s USR LOG check (src/main.c, commit c9a7701) was oscar64's
 * frame-overlay allocator reusing a live stack slot across a rel_close()
 * call — `check` (a status variable, not a DMA target) came back corrupt
 * with the read itself having succeeded (`got = 30, buf[0] = 1`). That is a
 * property of oscar64's stack-slot allocator, not of which rel_* backend is
 * linked, so it is latent and unproven under REL, exactly as reasoned there
 * — not disproven. This fix is scoped to T64_STORE_SEQ anyway because REL
 * builds (BOOT, CONFIGURE) already have ample headroom and are the shipping
 * backend with no field reports, while CONFIGURE-SIEC has 39 bytes free and
 * a shared region is cheaper than 23 separate per-function frames; extending
 * the same treatment to REL is a pure budget call, revisit if REL ever shows
 * the symptom. Sized to the largest record type in use
 * (RECORD_SIZE_FILE_ENTRY, 100 bytes; see include/bbs/records.h) — the
 * static_assert-style check below ties that to the record constants it must
 * stay >= to, rather than leaving the relationship implicit.
 *
 * Three flat globals, not one struct: oscar64's preprocessor cannot expand
 * an object-like macro whose body is a struct-member expression (confirmed
 * with a minimal reproduction — `#define x s.field` followed by `x = 1;`
 * fails to parse with "Struct member identifier not found", even though
 * `s.field = 1;` written out directly compiles fine) — and call sites alias
 * these through `#define err rel_scratch_err` (etc.) so the function bodies
 * below don't need to change at all. */
#define REL_SCRATCH_SIZE RECORD_SIZE_FILE_ENTRY
typedef char rel_scratch_size_check[
    (REL_SCRATCH_SIZE >= RECORD_SIZE_USER_PROFILE &&
     REL_SCRATCH_SIZE >= RECORD_SIZE_MSG_IDX) ? 1 : -1];
extern u8        rel_scratch_buf[REL_SCRATCH_SIZE];
extern u8        rel_scratch_got;
extern bbs_err_t rel_scratch_err;
#endif

#endif /* BBS_REL_H */
