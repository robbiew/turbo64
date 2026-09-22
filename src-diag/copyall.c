/* COPYALL - copy the T/64 disk set from one device to another device/partition.
 *
 * Uses two distinct KERNAL logical file numbers so source and destination are
 * open at once - T/64's disk HAL keeps a single data channel, so it cannot do
 * this itself. Files are copied through the C64, which is the point: sd2iec
 * then generates the FAT name, so names come out as proper CBM names instead
 * of carrying a ".PRG" the way a PC-side copy does. */
#include <stdio.h>
#include <string.h>
#include <conio.h>
#include <c64/kernalio.h>
#include "bbs/types.h"
#include "bbs/err.h"
#include "bbs/config.h"
#include "bbs/hal/disk.h"
#include "bbs/version.h"

#define LF_SRC 2
#define LF_DST 3

/* Each entry carries its own PRG/SEQ flag instead of a parallel "first N are
   PRG" count - adding or reordering an entry can't desync a hand-maintained
   number from the array again. BOOT/CONFIGURE names are built from
   BBS_RELEASE_VERSION_COMPACT (C99 adjacent string-literal concatenation) so
   this list can never drift from the version the Makefile actually names the
   PRGs after.

   Program and content files only. CONFIG, CALLERS and ACCESS are deliberately
   NOT copied: they are per-install data, and overwriting a SysOp's device
   configuration during a program update is destructive. */
typedef struct {
    const char *name;
    bool_t is_prg;
} copy_file_t;

static const copy_file_t files[] = {
    { "BOOT-" BBS_RELEASE_VERSION_COMPACT,      TRUE },
    { "CONFIGURE-" BBS_RELEASE_VERSION_COMPACT, TRUE },
    { "OVL_MSGS",    TRUE },
    { "OVL_WFC",     TRUE },
    { "OVL_BOOT",    TRUE },
    { "OVL_DOORS",   TRUE },
    { "OVL_FILES",   TRUE },
    { "OVL_ZMODEM",  TRUE },
    { "OVL_AUTH",    TRUE },
    { "FORTUNE",     TRUE },
    { "G.LOGIN",      FALSE }, { "G.LOGIN 0",   FALSE },
    { "G.LOGIN 1 80", FALSE }, { "G.LOGIN 2 80", FALSE },
    { "G.NEWUSER",    FALSE }, { "G.TERM",       FALSE },
    { "M.DOORS",      FALSE }, { "M.FILES",      FALSE },
    { "M.MAIN",       FALSE }, { "M.MAIN 1 80",  FALSE },
    { "M.MSGS",       FALSE }, { "M.MSGS 1 80",  FALSE },
    { "M.READ",       FALSE },
    { "P.DOORS",      FALSE }, { "P.FILES",      FALSE },
    { "P.MAIN",       FALSE }, { "P.MAIN 1 80",  FALSE },
    { "P.MSGS",       FALSE }, { "P.READ",       FALSE },
    { "P.READ 1 80",  FALSE }
};
#define NFILE (sizeof(files)/sizeof(files[0]))

/* Row budget for the final summary: the C64 screen is 25 rows, and the "." /
   "!" progress lines (one per NFILE) already scroll the early ones off long
   before the run ends - the exact problem this fix addresses. Re-printing
   every failed name would just move the same problem down: with all NFILE
   files failing, "DONE. N FAILED." itself would scroll off the top before
   the last name printed, leaving the operator exactly as blind as before.
   Cap the re-printed list well under the full screen and say "AND N MORE"
   for the rest - the count from "DONE." is always exact even when the list
   is truncated. */
#define MAX_FAIL_SHOW 15

/* Total attempts per file (1 initial + 2 retries). The IEC bus between the
   emulated 1581 and the physical uIEC has been measured to drop 1-6 files
   out of 30 on a run, intermittently and unrelated to file size - exactly
   the shape of a transient bus fault, which a retry is meant to absorb.
   3 gives every file two chances to recover from a glitch without turning
   a single bad handshake into a third or fourth full-file re-read; going
   higher buys little (a fault that survives two retries is far more likely
   a real problem than bad luck) while extending the worst-case run time. */
#define MAX_ATTEMPTS 3

static u8 buf[128];

/* Failed-file indices, not copied name strings: files[] already holds the
   names, so recording the index costs 1 byte per failure instead of
   duplicating up to 39 bytes (sizeof sn/dn) per name. Sized to the worst
   case (every file fails) so the loop below never needs a bounds check
   against a smaller buffer. File-static, matching s_read_status below and
   buf above: main()'s loop writes this across repeated calls into copy_one()
   (which itself calls krnio_ and disk_ functions every iteration), and this
   file already treats "must stay correct across such a call" as reason
   enough to use a fixed BSS slot rather than lean on a stack local - see
   src/data/users.c:25-30 for the underlying oscar64 hazard (no hardware
   stack; per-function frames can overlap a callee's frame across a call). */
static u8 s_fail_idx[NFILE];
static u8 s_fail_n;

/* Per-chunk source read status (krnio_pstatus[LF_SRC], captured right after
   krnio_read). Held file-static, not a copy_one() local: it must stay valid
   across the krnio_write()/krnio_status() calls later in the same loop
   iteration, and oscar64 gives functions static per-function frames that can
   overlap with a callee's frame across a call (see src/data/users.c:25-30).
   A stack local surviving a call is exactly the shape of that hazard; a
   fixed BSS slot is immune. */
static krnioerr s_read_status;

/* Attempt number for the file currently in copy_with_retry()'s retry loop,
   1-based. File-static for the same reason as s_fail_idx/s_fail_n above:
   copy_with_retry() re-enters copy_one() on each retry, and copy_one() calls
   krnio_/disk_ functions throughout, so a plain local here would be exactly
   the frame-overlay hazard documented at src/data/users.c:25-30 - a value
   that must outlive such a call needs a fixed BSS slot, not a stack frame.
   main() reads it once copy_with_retry() returns, to report whether the
   file needed more than one attempt. */
static u8 s_attempt;

static bool_t copy_one(u8 sdev, u8 ddev, const char *name, bool_t is_prg)
{
    char sn[40], dn[40];
    bool_t ok = FALSE;

    sprintf(sn, "0:%s,%c,R", name, is_prg ? 'P' : 'S');
    sprintf(dn, "0:%s,%c,W", name, is_prg ? 'P' : 'S');

    krnio_setnam(dn + 2);            /* scratch any old copy first */
    disk_scratch(ddev, 1, name);
    (void)disk_status(ddev);

    krnio_setnam(sn);
    if (!krnio_open(LF_SRC, sdev, LF_SRC)) return FALSE;
    krnio_setnam(dn);
    if (!krnio_open(LF_DST, ddev, LF_DST)) { krnio_close(LF_SRC); return FALSE; }

    /* Loop until krnio_read reports clean end-of-file. Do NOT treat every
       n <= 0 as end-of-file: krnio_read() returns -1 only when CHKIN on the
       source channel fails outright, but a mid-stream drive error (timeout,
       checksum, device dropped) can also come back as a SHORT read - even
       n == 0 - without going negative. The only reliable signal is the
       per-channel status krnio_read() itself latches into krnio_pstatus[]
       (see vendor/oscar64/include/c64/kernalio.c): KRNIO_OK/KRNIO_EOF mean
       the bytes actually in `buf` are good, anything else is a real error.
       Reading krnio_pstatus[LF_SRC] directly (not krnio_status()) matters
       because krnio_status() reflects the KERNAL's single shared ST
       register, which the krnio_write() call below clobbers with the
       destination channel's status - see disk_write()'s comment in
       src/hal/disk.c for the same caveat on the write side.

       A truncated PRG still LOADs successfully and then crashes on RUN,
       which is a miserable thing to debug - so on any doubt we fail the
       file rather than print a false '.'. */
    for (;;) {
        int n = krnio_read(LF_SRC, (char *)buf, (int)sizeof(buf));
        if (n < 0) break;                      /* source CHKIN failed outright */

        s_read_status = krnio_pstatus[LF_SRC];
        if (s_read_status != KRNIO_OK && s_read_status != KRNIO_EOF) break; /* mid-stream read error */

        if (n == 0) { ok = TRUE; break; }        /* clean EOF, nothing left to flush */

        if (krnio_write(LF_DST, (const char *)buf, n) != n) break;
        /* Mirrors disk_write(): krnio_write() returns `n` once CHKOUT
           succeeds, before the bytes are confirmed on the drive. */
        if (krnio_status() != KRNIO_OK) break;

        if (s_read_status == KRNIO_EOF) { ok = TRUE; break; }
    }

    krnio_clrchn();
    krnio_close(LF_SRC);
    krnio_close(LF_DST);
    return ok;
}

/* No timer is available on this HAL, so this is a counted busy-loop, not a
   real delay - same idiom as the connect-banner drain wait in
   src/hal/term.c:248. ~20000 iterations is a rough tens-of-milliseconds
   pause: enough for a wedged IEC handshake to time out and both devices to
   settle before the next attempt hits them again, but small enough that
   even a file that burns every retry only adds a fraction of a second to a
   run that already takes ~10 minutes on real hardware. */
static void retry_delay(void)
{
    volatile u16 i;
    for (i = 0; i < 20000; i++) ;
}

/* Retries a single file up to MAX_ATTEMPTS times. Every call into
   copy_one() - including retries - starts with copy_one() scratching the
   destination before it opens anything, so a failed attempt can never leave
   bytes on disk for the next attempt to build on top of: attempt N+1 always
   sees the same clean (absent) destination a first attempt would. This
   holds even though a failed attempt N still closes LF_DST (copy_one()
   closes both channels on every exit path, success or failure) - that
   close is what finalizes the truncated file as a normal directory entry,
   which is exactly what makes it scratchable on the next attempt instead of
   leaving the drive's directory in a half-open, inconsistent state. */
static bool_t copy_with_retry(u8 sdev, u8 ddev, const char *name, bool_t is_prg)
{
    for (s_attempt = 1; s_attempt <= MAX_ATTEMPTS; s_attempt++) {
        if (copy_one(sdev, ddev, name, is_prg)) return TRUE;
        if (s_attempt < MAX_ATTEMPTS) retry_delay();
    }
    return FALSE;
}

int main(void)
{
    u8 sdev = 8, ddev = 10, i;

    printf("\x93\x8e");
    printf("COPY T/64 SET  8 -> 10 PART 1\n");
    printf("PRESS C TO START\n");
    for (;;) { int c = getch(); if (c == 'C' || c == 'c') break; }
    printf("\n");

    if (disk_select_partition(ddev, 1) != BBS_OK) {
        printf("CP1 FAILED %02u\n", disk_status(ddev));
        getch(); return 0;
    }

    for (i = 0; i < (u8)NFILE; i++) {
        bool_t ok = copy_with_retry(sdev, ddev, files[i].name, files[i].is_prg);
        /* s_attempt is only worth reporting when the file needed more than
           one try to succeed - a file that used every attempt and still
           failed always shows the same MAX_ATTEMPTS count, which the '!'
           marker and the FAILED: summary below already communicate. */
        if (ok && s_attempt > 1) {
            printf(". %s R%u\n", files[i].name, (unsigned)s_attempt);
        } else {
            printf("%c %s\n", ok ? '.' : '!', files[i].name);
        }
        if (!ok) s_fail_idx[s_fail_n++] = i;
    }
    printf("\nDONE. %u FAILED.\n", (unsigned)s_fail_n);

    /* Re-print the failed NAMES, not just the count: with NFILE=30 on a
       25-row screen, the "." / "!" lines above have long since scrolled the
       failing ones out of view by the time DONE prints. Capped at
       MAX_FAIL_SHOW (see above) so a bad run (many failures) can't scroll
       DONE itself back off the top while this list is still printing. */
    if (s_fail_n) {
        bool_t truncated = s_fail_n > MAX_FAIL_SHOW;
        u8 show = truncated ? MAX_FAIL_SHOW : s_fail_n;
        printf("FAILED:\n");
        for (i = 0; i < show; i++) {
            printf("%s\n", files[s_fail_idx[i]].name);
        }
        if (truncated) {
            printf("...AND %u MORE\n", (unsigned)(s_fail_n - show));
        }
    }
    getch();
    return 0;
}
