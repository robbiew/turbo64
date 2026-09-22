/* src/data/usrday.c - Per-user daily-limit state (USR.DAY REL file). */

#include "bbs/usrday.h"
#include "bbs/cfg.h"
#include "bbs/rel.h"
#include "bbs/config.h"
#include <string.h>
#include <stdio.h>

/* No msgs_code overlay (cf. usrptr.c): USR.DAY is used on the core session
 * login/logoff/tick path, so it stays resident in main code. */

#define USRDAY_READ_MIN 6

static bbs_err_t usrday_open(u8 device, rel_handle_t *h)
{
    bbs_err_t err;
    err = cfg_send_drive_init(device, bbs_cfg.init_system);
    if (err != BBS_OK) return err;
    return rel_open(device, bbs_cfg.drive_system, "USR.DAY", RECORD_SIZE_USR_DAY, h);
}

bbs_err_t usrday_load(u16 user_id, usr_day_record_t *out, u8 device)
{
    rel_handle_t h;
    /* err/got/buf alias the shared rel_scratch under T64_STORE_SEQ — see the
     * comment on rel_scratch_buf/got/err in bbs/rel.h. */
#ifdef T64_STORE_SEQ
#define err rel_scratch_err
#define got rel_scratch_got
#define buf rel_scratch_buf
#else
    bbs_err_t err;
    u8 buf[RECORD_SIZE_USR_DAY];
    u8 got;
#endif

    if (!out || user_id == 0) return BBS_EBADARG;

    memset(out, 0, sizeof(*out));   /* default: fresh / no usage */

    err = usrday_open(device, &h);
    if (err != BBS_OK) return err;  /* BBS_ENOTFOUND if file absent */

    err = rel_position(h, (u8)user_id);
    if (err != BBS_OK) { rel_close(h); return err; }

    memset(buf, 0, RECORD_SIZE_USR_DAY);
    err = rel_read(h, buf, RECORD_SIZE_USR_DAY, &got);
    rel_close(h);

    /* Missing/short record = user has no daily state yet. Not an error. */
    if (err != BBS_OK || got < USRDAY_READ_MIN) return BBS_OK;

    out->last_mm     = buf[0];
    out->last_dd     = buf[1];
    out->last_yy     = buf[2];
    out->calls_today = buf[3];
    out->mins_today  = (u16)buf[4] | ((u16)buf[5] << 8);
    return BBS_OK;
}
#ifdef T64_STORE_SEQ
#undef err
#undef got
#undef buf
#endif

bbs_err_t usrday_save(u16 user_id, const usr_day_record_t *rec, u8 device)
{
    rel_handle_t h;
#ifdef T64_STORE_SEQ
#define err rel_scratch_err
#define buf rel_scratch_buf
#else
    bbs_err_t err;
    u8 buf[RECORD_SIZE_USR_DAY];
#endif

    if (!rec || user_id == 0) return BBS_EBADARG;

    memset(buf, 0, RECORD_SIZE_USR_DAY);
    buf[0] = rec->last_mm;
    buf[1] = rec->last_dd;
    buf[2] = rec->last_yy;
    buf[3] = rec->calls_today;
    buf[4] = (u8)(rec->mins_today & 0xFF);
    buf[5] = (u8)(rec->mins_today >> 8);

    err = usrday_open(device, &h);
    if (err != BBS_OK) return err;

    err = rel_position(h, (u8)user_id);
    if (err != BBS_OK) { rel_close(h); return err; }

    err = rel_write(h, buf, RECORD_SIZE_USR_DAY);
    err = rel_close_keep(h, err);
    return err;
}
#ifdef T64_STORE_SEQ
#undef err
#undef buf
#endif
