/* file_entries.c — per-area file entry CRUD (UD<n>,L,100 REL files).
 * Placed in the FILES overlay; only callable from OVL_FILES. */
#include "bbs/overlay.h"
#include "bbs/file_entries.h"
#include "bbs/cfg.h"
#include "bbs/rel.h"
#include "bbs/config.h"
#include <string.h>
#include <stdio.h>

#pragma code(files_code)
#pragma data(files_data)
#pragma bss(files_bss)

#define REC_MIN 4   /* minimum bytes read to treat as a valid record */

static bbs_err_t fentry_open(u8 area_id, u8 device, rel_handle_t *h)
{
    char fname[8];
    bbs_err_t err = cfg_send_drive_init(device, bbs_cfg.init_files);
    if (err != BBS_OK) return err;
    sprintf(fname, "UD%u", (unsigned)area_id);
    return rel_open(device, bbs_cfg.drive_files, fname, RECORD_SIZE_FILE_ENTRY, h);
}

static void fentry_pack(const file_entry_record_t *r, u8 *buf)
{
    u8 i;
    memset(buf, 0, RECORD_SIZE_FILE_ENTRY);
    buf[0] = r->id;
    for (i = 0; i < 15; i++) {
        char c = r->filename[i];
        buf[1 + i] = (u8)(c ? c : ' ');
    }
    buf[16] = 0;
    for (i = 0; i < 40; i++) {
        char c = r->description[i];
        buf[17 + i] = (u8)(c ? c : ' ');
    }
    buf[57] = (u8)(r->blocks & 0xFF);
    buf[58] = (u8)(r->blocks >> 8);
    for (i = 0; i < 15; i++) {
        char c = r->uploader[i];
        buf[59 + i] = (u8)(c ? c : ' ');
    }
    buf[74] = 0;
    buf[75] = (u8)(r->upload_date & 0xFF);
    buf[76] = (u8)(r->upload_date >> 8);
    buf[77] = (u8)(r->downloads & 0xFF);
    buf[78] = (u8)(r->downloads >> 8);
    buf[79] = r->access_level;
}

static void fentry_unpack(file_entry_record_t *r, const u8 *buf)
{
    u8 i;
    memset(r, 0, sizeof(*r));
    r->id = buf[0];
    for (i = 0; i < 15; i++) r->filename[i] = (char)buf[1 + i];
    r->filename[15] = 0;
    for (i = 0; i < 40; i++) r->description[i] = (char)buf[17 + i];
    r->blocks       = (u16)buf[57] | ((u16)buf[58] << 8);
    for (i = 0; i < 15; i++) r->uploader[i] = (char)buf[59 + i];
    r->uploader[15] = 0;
    r->upload_date  = (u16)buf[75] | ((u16)buf[76] << 8);
    r->downloads    = (u16)buf[77] | ((u16)buf[78] << 8);
    r->access_level = buf[79];
    if (r->access_level > CFG_ACCESS_SYSOP) r->access_level = CFG_ACCESS_SYSOP;
}

static bool_t fentry_is_deleted(const file_entry_record_t *r)
{
    u8 i;
    if (r->id == 0) return TRUE;
    for (i = 0; i < 15; i++) {
        if (r->filename[i] != ' ' && r->filename[i] != 0) return FALSE;
    }
    return TRUE;
}

bbs_err_t fentry_count(u8 area_id, u8 device, u8 *out_count)
{
    rel_handle_t h;
    u16 n;
    file_entry_record_t r;
    /* err/got/buf alias the shared rel_scratch under T64_STORE_SEQ — see the
     * comment on rel_scratch_buf/got/err in bbs/rel.h. */
#ifdef T64_STORE_SEQ
#define err rel_scratch_err
#define got rel_scratch_got
#define buf rel_scratch_buf
#else
    u8 buf[RECORD_SIZE_FILE_ENTRY], got;
    bbs_err_t err;
#endif

    *out_count = 0;
    err = fentry_open(area_id, device, &h);
    if (err != BBS_OK) return err;

    err = BBS_OK;
    for (n = 1; n <= 255; n++) {
        memset(buf, 0, RECORD_SIZE_FILE_ENTRY);
        err = rel_read(h, buf, RECORD_SIZE_FILE_ENTRY, &got);
        if (err != BBS_OK) break;
        if (got < REC_MIN) break;
        fentry_unpack(&r, buf);
        if (!fentry_is_deleted(&r)) (*out_count)++;
    }
    rel_close(h);
    return (err == BBS_OK) ? BBS_OK : err;
}
#ifdef T64_STORE_SEQ
#undef err
#undef got
#undef buf
#endif

bbs_err_t fentry_by_recnum(u8 area_id, u8 recnum, file_entry_record_t *out,
                            u8 device)
{
    rel_handle_t h;
#ifdef T64_STORE_SEQ
#define err rel_scratch_err
#define got rel_scratch_got
#define buf rel_scratch_buf
#else
    bbs_err_t err;
#endif

    if (!recnum || !out) return BBS_EBADARG;
    err = fentry_open(area_id, device, &h);
    if (err != BBS_OK) return err;

    err = rel_position(h, recnum);
    if (err == BBS_OK) {
#ifndef T64_STORE_SEQ
        u8 buf[RECORD_SIZE_FILE_ENTRY], got;
#endif
        memset(buf, 0, RECORD_SIZE_FILE_ENTRY);
        err = rel_read(h, buf, RECORD_SIZE_FILE_ENTRY, &got);
        if (err == BBS_OK && got >= REC_MIN) {
            fentry_unpack(out, buf);
            if (fentry_is_deleted(out)) err = BBS_ENOTFOUND;
        } else if (err == BBS_OK) {
            err = BBS_EIO;
        }
    }
    rel_close(h);
    return err;
}
#ifdef T64_STORE_SEQ
#undef err
#undef got
#undef buf
#endif

bbs_err_t fentry_add(u8 area_id, const file_entry_record_t *rec, u8 device)
{
    rel_handle_t h;
    u8 slot = 0;
    u16 n;
    file_entry_record_t r;
#ifdef T64_STORE_SEQ
#define err rel_scratch_err
#define got rel_scratch_got
#define buf rel_scratch_buf
#else
    u8 buf[RECORD_SIZE_FILE_ENTRY], got;
    bbs_err_t err;
#endif

    if (!rec) return BBS_EBADARG;
    err = fentry_open(area_id, device, &h);
    if (err != BBS_OK) return err;

    /* Find first empty/deleted slot or append */
    for (n = 1; n <= 255; n++) {
        memset(buf, 0, RECORD_SIZE_FILE_ENTRY);
        err = rel_read(h, buf, RECORD_SIZE_FILE_ENTRY, &got);
        /* Past the SEQ backend's high-water mark IS a free slot — there is
         * nothing there yet, which is exactly what we're scanning for. REL's
         * rel_read() never returns BBS_ENOTFOUND mid-scan (only got<REC_MIN,
         * handled below), so this case is inert on that backend. */
        if (err == BBS_ENOTFOUND) { slot = n; break; }
        if (err != BBS_OK) break;
        if (got < REC_MIN) { slot = n; break; }
        fentry_unpack(&r, buf);
        if (fentry_is_deleted(&r)) { slot = n; break; }
    }
    if (!slot) { rel_close(h); return BBS_EFULL; }

    err = rel_position(h, slot);
    if (err == BBS_OK) {
        file_entry_record_t wr = *rec;
        wr.id = slot;
        fentry_pack(&wr, buf);
        err = rel_write(h, buf, RECORD_SIZE_FILE_ENTRY);
    }
    rel_close(h);
    return err;
}
#ifdef T64_STORE_SEQ
#undef err
#undef got
#undef buf
#endif

bbs_err_t fentry_save(u8 area_id, const file_entry_record_t *rec, u8 device)
{
    rel_handle_t h;
#ifdef T64_STORE_SEQ
#define err rel_scratch_err
#define buf rel_scratch_buf
#else
    bbs_err_t err;
#endif

    if (!rec || !rec->id) return BBS_EBADARG;
    err = fentry_open(area_id, device, &h);
    if (err != BBS_OK) return err;
    err = rel_position(h, rec->id);
    if (err == BBS_OK) {
#ifndef T64_STORE_SEQ
        u8 buf[RECORD_SIZE_FILE_ENTRY];
#endif
        fentry_pack(rec, buf);
        err = rel_write(h, buf, RECORD_SIZE_FILE_ENTRY);
    }
    rel_close(h);
    return err;
}
#ifdef T64_STORE_SEQ
#undef err
#undef buf
#endif

bbs_err_t fentry_delete(u8 area_id, u8 recnum, u8 device)
{
    file_entry_record_t r;
    bbs_err_t err;

    err = fentry_by_recnum(area_id, recnum, &r, device);
    if (err != BBS_OK) return err;
    memset(r.filename, ' ', sizeof(r.filename));
    return fentry_save(area_id, &r, device);
}
