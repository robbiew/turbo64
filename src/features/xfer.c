/* xfer.c — Resident Zmodem transfer shim.  RESIDENT (no overlay pragma).
 *
 * Loads OVL_ZMODEM over the overlay zone, delegates to zmodem_send/recv,
 * then reloads OVL_FILES so the caller (fl_download / fl_upload in
 * OVL_FILES) can safely return.
 *
 * BSS note: oscar64's -Oo optimizer spills 76 bytes of activation records
 * into main BSS per krnio_load call when a zmodem function is also called
 * in the same scope.  Two mitigations here prevent overflow:
 *   1. z_load_zmodem / z_load_files are __noinline helpers so krnio_load
 *      runs in a separate scope from the zmodem_send/recv call.
 *   2. They take NO parameters — any parameter spilled across the krnio_load
 *      would add to main BSS.
 *
 * Safety: __noinline prevents -Oo from folding xfer_zmodem_send/recv into
 * OVL_FILES.  If inlined, krnio_load(OVL_ZMODEM) would execute from within
 * OVL_FILES at $9700, overwriting itself mid-call. */
#include "bbs/xfer.h"
#include "bbs/session.h"
#include "bbs/cfg.h"
#include "bbs/sysop.h"
#include "bbs/hal/disk.h"
#include "net/zmodem.h"

/* Both return bbs_err_t rather than void so the two callers below can tell
 * a failed load from a completed one — a return value doesn't add to the
 * BSS-spill risk documented above (only *parameters* spilled across the
 * krnio_load call did); it costs nothing extra here. */
__noinline static bbs_err_t z_load_zmodem(void)
{
    bbs_err_t err = disk_load_overlay(P"OVL_ZMODEM");
    if (err == BBS_OK) wfc.ovl_wfc_loaded = FALSE;
    return err;
}

__noinline static bbs_err_t z_load_files(void)
{
    bbs_err_t err = disk_load_overlay(P"OVL_FILES");
    if (err == BBS_OK) wfc.ovl_wfc_loaded = FALSE;
    return err;
}

__noinline xfer_result_t xfer_zmodem_send(const session_t *s, u8 device, u8 drive,
                                            const char *filename)
{
    u8 r;
    /* z_load_zmodem() runs before anything overwrites $9700 — OVL_FILES
     * (the caller's own overlay, fl_download) is still intact on failure,
     * so skipping zmodem_send and returning is safe. */
    if (z_load_zmodem() != BBS_OK) {
        session_emit(s, "\r\nERROR: OVL_ZMODEM LOAD FAILED.\r\n");
        return XFER_ERR;
    }
    r = (u8)zmodem_send(s, device, drive, filename);
    /* z_load_files() restores OVL_FILES before this function returns into
     * fl_download, which lives there. zmodem_send() above has already run
     * from OVL_ZMODEM, so $9700 is no longer valid OVL_FILES — unlike the
     * z_load_zmodem() check above, there is no safe skip here. Halt rather
     * than return into it; see door_run's reload_ovl for the same reasoning. */
    if (z_load_files() != BBS_OK) {
        session_emit(s, "\r\nERROR: OVL_FILES RESTORE FAILED. HALTED.\r\n");
        for (;;) { }
    }
    if (r == ZMODEM_OK) return XFER_OK;
    if (r == ZMODEM_CANCEL) return XFER_CANCEL;
    return XFER_ERR;
}

__noinline xfer_result_t xfer_zmodem_recv(const session_t *s, u8 device, u8 drive,
                                            const char *filename)
{
    u8 r;
    if (z_load_zmodem() != BBS_OK) {
        session_emit(s, "\r\nERROR: OVL_ZMODEM LOAD FAILED.\r\n");
        return XFER_ERR;
    }
    r = (u8)zmodem_recv(s, device, drive, filename);
    if (z_load_files() != BBS_OK) {
        session_emit(s, "\r\nERROR: OVL_FILES RESTORE FAILED. HALTED.\r\n");
        for (;;) { }
    }
    if (r == ZMODEM_OK) return XFER_OK;
    if (r == ZMODEM_CANCEL) return XFER_CANCEL;
    return XFER_ERR;
}
