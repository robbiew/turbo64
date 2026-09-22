/* CBM relative (REL) file access using oscar64 kernalio.
 *
 * REL files use the command channel's "P" (position) command to seek
 * to a record:  PRINT#15,"P";CHR$(sa);CHR$(rec_lo);CHR$(rec_hi);CHR$(0)
 * where `sa` is the secondary address (logical file number) of the data
 * channel.  We use CFG_FNUM_DATA (8) for data and CFG_FNUM_CMD (15) for
 * the command channel.
 *
 * Only one REL file may be open at a time (single data channel).
 */
#include "bbs/rel.h"
#include "bbs/config.h"
#include "bbs/hal/disk.h"
#include <c64/kernalio.h>
#include <string.h>
#include <stdio.h>

static u8 s_rec_size = 0;
static u8 s_device   = 0;
static u8 s_open     = 0;

bbs_err_t rel_open(u8 device, u8 partition, const char *name, u8 record_size,
                   rel_handle_t *out)
{
    if (s_open) return BBS_EFULL;   /* only one open at a time */

    char fname[40];
    bbs_err_t perr = disk_select_partition(device, partition);
    if (perr != BBS_OK) return perr;

    sprintf(fname, "0:%s,L,%c", name, (char)record_size);
    krnio_setnam(fname);
    /* OPEN with ",L,size" format. CBM DOS creates the file on first write
     * if it doesn't exist. If the open fails here, treat it as "file not found"
     * (most common) rather than I/O error — the caller will decide whether to
     * create it or treat it as a real error. */
    if (!krnio_open(CFG_FNUM_DATA, device, CFG_FNUM_DATA)) {
        return BBS_ENOTFOUND;
    }

    /* Open command channel. */
    krnio_setnam("");
    krnio_open(CFG_FNUM_CMD, device, 15);

    /* WHY the status check: krnio_open() succeeds on ANY device that answers
     * the bus, whatever DOS thought of the ",L," open string. Without this,
     * a drive that rejects the REL open still returns BBS_OK here, and every
     * later rel_write() also returns BBS_OK while the data goes nowhere —
     * silent record loss, not a visible error. Measured on a Commodore 64
     * Ultimate: its SoftIEC drive has no REL support, answers 61 FILE NOT
     * OPEN, and never creates the file, yet the whole API reported success.
     *
     * Read the status IN PLACE. disk_status() closes and reopens logical
     * file 15, which is the command channel rel_position() drives the seek
     * through, so calling it here would break positioning on a working drive.
     * krnio_gets() does its own CHKIN/CLRCHN and latches KRNIO_EOF at the end
     * of the line, hence the reset. Buffer is full-line sized so no unread
     * bytes are left queued on the channel.
     *
     * 50 (RECORD NOT PRESENT) is accepted: CBM DOS reports it for a REL file
     * that does not exist yet, which is the normal create-on-first-write path
     * this module documents. */
    {
        char st[40];
        int  n;
        u8   code;

        krnio_pstatus[CFG_FNUM_CMD] = KRNIO_OK;
        n = krnio_gets(CFG_FNUM_CMD, st, (int)sizeof(st));
        code = (n < 2) ? 99 : (u8)((st[0] - '0') * 10 + (st[1] - '0'));
        if (code >= 20 && code != 50) {
            krnio_close(CFG_FNUM_CMD);
            krnio_close(CFG_FNUM_DATA);
            return BBS_EIO;
        }
    }

    s_rec_size = record_size;
    s_device   = device;
    s_open     = 1;
    *out = (rel_handle_t)CFG_FNUM_DATA;
    return BBS_OK;
}

bbs_err_t rel_position(rel_handle_t h, u16 rec)
{
    if (!s_open) return BBS_ENOTFOUND;
    /* Send "P" command via PRINT#15 (CHKOUT style) — equivalent to BASIC:
     *   PRINT#15,"P";CHR$(sa);CHR$(rec_lo);CHR$(rec_hi);CHR$(0)
     * Using OPEN/CLOSE to deliver the P command as a "filename" is unreliable
     * on 1581/U64; the PRINT# approach is the canonical CBM DOS method. */
    if (!krnio_chkout(CFG_FNUM_CMD)) return BBS_EIO;
    krnio_chrout('P');
    krnio_chrout((char)h);                    /* secondary address of data file */
    krnio_chrout((char)(rec & 0xFF));         /* record number low byte */
    krnio_chrout((char)((rec >> 8) & 0xFF));  /* record number high byte */
    krnio_chrout(0);                           /* byte offset within record */
    krnio_clrchn();
    return BBS_OK;
}

bbs_err_t rel_read(rel_handle_t h, void *buf, u8 record_size, u8 *got)
{
    if (!s_open) return BBS_ENOTFOUND;
    /* krnio_read handles CHKIN internally (same pattern as krnio_write/CHKOUT).
     * Calling krnio_chkin here would send TALK+SECOND twice on the IEC bus
     * without an intervening UNTALK — same double-CHKIN bug as rel_write had.
     *
     * krnio_pstatus[fnum] is set to KRNIO_EOF by krnio_read when it reads the
     * last byte of a REL record (CBM DOS signals end-of-record via KERNAL ST).
     * krnio_read checks this cache at entry and returns 0 immediately if set.
     * We must reset it before each record read so the scan advances past
     * record 1.  krnio_pstatus is declared extern in kernalio.h.
     *
     * DO NOT call krnio_clrchn() here — that closes ALL channels including
     * the data channel, breaking sequential reads. Only reset the status cache. */
    krnio_pstatus[(char)h] = KRNIO_OK;
    int r = krnio_read((char)h, (char *)buf, (int)record_size);
    if (r < 0) { *got = 0; return BBS_EIO; }
    *got = (u8)r;
    return BBS_OK;
}

bbs_err_t rel_write(rel_handle_t h, const void *buf, u8 record_size)
{
    if (!s_open) return BBS_ENOTFOUND;
    /* krnio_write handles CHKOUT + CHROUT loop + CLRCHN internally.
     * Calling krnio_chkout here before krnio_write would send LISTEN+SECOND
     * twice on the IEC bus without an intervening UNLISTEN — avoid that.
     *
     * DO NOT call krnio_clrchn() before krnio_write — krnio_write already
     * calls it internally as part of the close sequence. Calling it here
     * would close the data channel prematurely. */
    int r = krnio_write((char)h, (const char *)buf, (int)record_size);
    return (r == (int)record_size) ? BBS_OK : BBS_EIO;
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
    if (!s_open) return BBS_OK;
    krnio_clrchn();
    krnio_close((char)h);
    krnio_close(CFG_FNUM_CMD);
    s_open = 0;
    return BBS_OK;
}

void rel_reset(void)
{
    /* Force-clear the single-open guard. Use only when the underlying
     * KERNAL channels were closed by external code (e.g. setup scratch). */
    s_open = 0;
}
