/* src/data/messages.c - Message base data layer. */

#include "bbs/messages.h"
#include "bbs/sysop.h"
#include "bbs/syscnt.h"
#include "bbs/boards.h"
#include "bbs/cfg.h"
#include "bbs/rel.h"
#include "bbs/hal/reu.h"
#include "bbs/hal/disk.h"
#include "bbs/hal/clock.h"
#include "bbs/config.h"
#include <string.h>
#include <stdio.h>
#define RECORD_READ_MIN 18
#include "bbs/overlay.h"
#pragma code(msgs_code)
#pragma data(msgs_data)
#pragma bss(msgs_bss)


/* ---- Filename helpers --------------------------------------------- */

static void msg_idx_fname(u8 board_id, char *buf)
{
    sprintf(buf, "B%u.IDX", (unsigned)board_id);
}

static void msg_txt_fname(u8 board_id, char *buf)
{
    sprintf(buf, "B%u.TXT", (unsigned)board_id);
}

/* ---- Pack / unpack (32-byte layout) ------------------------------- */

/*
 * buf[0-1]   msg_id (LE)
 * buf[2-3]   parent_id (LE)
 * buf[4-5]   thread_root_id (LE)
 * buf[6-7]   author_id (LE)
 * buf[8-9]   date (LE)
 * buf[10-11] to_id (LE)
 * buf[12]    flags
 * buf[13]    reply_count
 * buf[14-15] body_offset (LE)
 * buf[16-17] body_len (LE)
 * buf[18-25] net_origin_bbs (8 bytes)
 * buf[26-27] net_origin_id (LE)
 * buf[28-31] reserved (zero)
 */

static void msg_pack(const msg_index_record_t *rec, u8 *buf)
{
    u8 i;
    memset(buf, 0, RECORD_SIZE_MSG_IDX);
    buf[0]  = (u8)(rec->msg_id & 0xFF);
    buf[1]  = (u8)((rec->msg_id >> 8) & 0xFF);
    buf[2]  = (u8)(rec->parent_id & 0xFF);
    buf[3]  = (u8)((rec->parent_id >> 8) & 0xFF);
    buf[4]  = (u8)(rec->thread_root_id & 0xFF);
    buf[5]  = (u8)((rec->thread_root_id >> 8) & 0xFF);
    buf[6]  = (u8)(rec->author_id & 0xFF);
    buf[7]  = (u8)((rec->author_id >> 8) & 0xFF);
    buf[8]  = (u8)(rec->date & 0xFF);
    buf[9]  = (u8)((rec->date >> 8) & 0xFF);
    buf[10] = (u8)(rec->to_id & 0xFF);
    buf[11] = (u8)((rec->to_id >> 8) & 0xFF);
    buf[12] = rec->flags;
    buf[13] = rec->reply_count;
    buf[14] = (u8)(rec->body_offset & 0xFF);
    buf[15] = (u8)((rec->body_offset >> 8) & 0xFF);
    buf[16] = (u8)(rec->body_len & 0xFF);
    buf[17] = (u8)((rec->body_len >> 8) & 0xFF);
    for (i = 0; i < 8; i++) buf[18 + i] = (u8)rec->net_origin_bbs[i];
    buf[26] = (u8)(rec->net_origin_id & 0xFF);
    buf[27] = (u8)((rec->net_origin_id >> 8) & 0xFF);
    buf[28] = rec->month;
    buf[29] = rec->day;
    buf[30] = rec->year_yy;
    for (i = 0; i < 31; i++) buf[32 + i] = (u8)rec->subj[i];
}

static void msg_unpack(msg_index_record_t *rec, const u8 *buf)
{
    u8 i;
    memset(rec, 0, sizeof(*rec));
    rec->msg_id         = (u16)buf[0]  | ((u16)buf[1]  << 8);
    rec->parent_id      = (u16)buf[2]  | ((u16)buf[3]  << 8);
    rec->thread_root_id = (u16)buf[4]  | ((u16)buf[5]  << 8);
    rec->author_id      = (u16)buf[6]  | ((u16)buf[7]  << 8);
    rec->date           = (u16)buf[8]  | ((u16)buf[9]  << 8);
    rec->to_id          = (u16)buf[10] | ((u16)buf[11] << 8);
    rec->flags          = buf[12];
    rec->reply_count    = buf[13];
    rec->body_offset    = (u16)buf[14] | ((u16)buf[15] << 8);
    rec->body_len       = (u16)buf[16] | ((u16)buf[17] << 8);
    for (i = 0; i < 8; i++) rec->net_origin_bbs[i] = (char)buf[18 + i];
    rec->net_origin_id  = (u16)buf[26] | ((u16)buf[27] << 8);
    rec->month    = buf[28];
    rec->day      = buf[29];
    rec->year_yy  = buf[30];
    for (i = 0; i < 30; i++) rec->subj[i] = (char)buf[32 + i];
    rec->subj[30] = '\0';
}

/* ---- Open index REL file ------------------------------------------ */

static bbs_err_t msg_open_idx(u8 board_id, u8 device, rel_handle_t *h)
{
    char fname[24];
    bbs_err_t err;

    err = cfg_send_drive_init(device, bbs_cfg.init_msgs);
    if (err != BBS_OK) return err;

    msg_idx_fname(board_id, fname);
    return rel_open(device, bbs_cfg.drive_msgs, fname, RECORD_SIZE_MSG_IDX, h);
}

/* ---- Index CRUD --------------------------------------------------- */

bbs_err_t msg_index_get(u8 board_id, u16 msg_id,
                         msg_index_record_t *out, u8 device)
{
    rel_handle_t h;
    /* err/got/buf alias the shared rel_scratch under T64_STORE_SEQ — see the
     * comment on rel_scratch_buf/got/err in bbs/rel.h. (Renamed from
     * s_msg_buf, which read as a static but never was one — same rename
     * applied to msg_index_put/msg_index_stats below, unconditionally,
     * since it was misleading regardless of backend.) */
#ifdef T64_STORE_SEQ
#define err rel_scratch_err
#define got rel_scratch_got
#define buf rel_scratch_buf
#else
    bbs_err_t err;
    u8 got;
    u8 buf[RECORD_SIZE_MSG_IDX];
#endif

    if (!out || msg_id == 0 || board_id == 0) return BBS_EBADARG;

#ifndef T64_STORE_SEQ
    /* Under T64_STORE_SEQ this cache is redundant: rel_open() already loads
     * B<n>.IDX into REU (the WINDOW region in bank 2) and rel_read() is
     * itself a DMA read, so the fallback below costs nothing extra there. */
    if (bbs_cfg.reu_enabled) {
        reu_index_get(msg_id, out);
        return BBS_OK;
    }
#endif

    err = msg_open_idx(board_id, device, &h);
    if (err != BBS_OK) return err;

    err = rel_position(h, (u16)msg_id);
    if (err != BBS_OK) { rel_close(h); return err; }

    memset(buf, 0, RECORD_SIZE_MSG_IDX);
    err = rel_read(h, (void *)buf, RECORD_SIZE_MSG_IDX, &got);
    rel_close(h);

    if (err != BBS_OK) return err;
    if (got < RECORD_READ_MIN) return BBS_EIO;
    if (got < RECORD_SIZE_MSG_IDX)
        memset(buf + got, 0, RECORD_SIZE_MSG_IDX - got);

    msg_unpack(out, buf);
    return BBS_OK;
}
#ifdef T64_STORE_SEQ
#undef err
#undef got
#undef buf
#endif

static void msg_row_from_rec(msg_list_row_t *row, const msg_index_record_t *rec)
{
    row->msg_id    = rec->msg_id;
    row->author_id = rec->author_id;
    row->flags     = rec->flags;
    row->month     = rec->month;
    row->day       = rec->day;
    memcpy(row->subj, rec->subj, 20);
    row->subj[20] = '\0';
}

/* Page fetch for the board listing: REU serves rows directly when loaded;
 * otherwise ONE open/position/sequential-read/close pass replaces the
 * per-message open/close cycles of repeated msg_index_get calls. */
u8 msg_index_page(u8 board_id, u16 first_id, u8 max_rows,
                  msg_list_row_t *out, u8 device)
{
    rel_handle_t h;
    bbs_err_t err;
    msg_index_record_t rec;
    u8 buf[RECORD_SIZE_MSG_IDX];
    u8 filled = 0;
    u8 got;

    if (!out || board_id == 0 || first_id == 0 || max_rows == 0) return 0;

#ifndef T64_STORE_SEQ
    if (bbs_cfg.reu_enabled) {
        while (filled < max_rows &&
               (u16)(first_id + filled) <= CFG_MSG_MAX_PER_BOARD) {
            reu_index_get((u16)(first_id + filled), &rec);
            if (rec.msg_id == 0) break;
            msg_row_from_rec(&out[filled], &rec);
            filled++;
        }
        return filled;
    }
#endif

    err = msg_open_idx(board_id, device, &h);
    if (err != BBS_OK) return 0;
    if (rel_position(h, first_id) != BBS_OK) { rel_close(h); return 0; }

    while (filled < max_rows &&
           (u16)(first_id + filled) <= CFG_MSG_MAX_PER_BOARD) {
        memset(buf, 0, RECORD_SIZE_MSG_IDX);
        err = rel_read(h, (void *)buf, RECORD_SIZE_MSG_IDX, &got);
        if (err != BBS_OK || got < RECORD_READ_MIN) break;
        if (got < RECORD_SIZE_MSG_IDX)
            memset(buf + got, 0, RECORD_SIZE_MSG_IDX - got);
        msg_unpack(&rec, buf);
        if (rec.msg_id == 0) break;
        msg_row_from_rec(&out[filled], &rec);
        filled++;
    }
    rel_close(h);
    return filled;
}

/* Count total and deleted index records in ONE pass: open the index REL once,
 * read sequentially, close once. (msg_index_get reopens/closes per call, which
 * is O(messages) drive open/close cycles — far too slow for the maint screen.)
 * A missing index file = no messages. */
bbs_err_t msg_index_stats(u8 board_id, u8 device, u16 *out_total, u16 *out_deleted)
{
    rel_handle_t h;
    bbs_err_t err;
    u8 got;
    u8 buf[RECORD_SIZE_MSG_IDX];
    msg_index_record_t rec;
    u16 idx, total = 0, deleted = 0;

    if (!out_total || !out_deleted || board_id == 0) return BBS_EBADARG;

    err = msg_open_idx(board_id, device, &h);
    if (err != BBS_OK) { *out_total = 0; *out_deleted = 0; return BBS_OK; }

    rel_position(h, 1);
    for (idx = 1; idx <= CFG_MSG_MAX_PER_BOARD; idx++) {
        memset(buf, 0, RECORD_SIZE_MSG_IDX);
        err = rel_read(h, (void *)buf, RECORD_SIZE_MSG_IDX, &got);
        if (err != BBS_OK || got < RECORD_READ_MIN) break;   /* past last record */
        if (got < RECORD_SIZE_MSG_IDX)
            memset(buf + got, 0, RECORD_SIZE_MSG_IDX - got);
        msg_unpack(&rec, buf);
        total++;
        if (rec.flags & MSG_F_DELETED) deleted++;
    }
    rel_close(h);

    *out_total = total;
    *out_deleted = deleted;
    return BBS_OK;
}

bbs_err_t msg_index_put(u8 board_id, const msg_index_record_t *rec, u8 device)
{
    rel_handle_t h;
#ifdef T64_STORE_SEQ
#define err rel_scratch_err
#define buf rel_scratch_buf
#else
    bbs_err_t err;
    u8 buf[RECORD_SIZE_MSG_IDX];
#endif

    if (!rec || rec->msg_id == 0 || board_id == 0) return BBS_EBADARG;

    /* Always write to disk */
    err = msg_open_idx(board_id, device, &h);
    if (err != BBS_OK) return err;

    err = rel_position(h, (u16)rec->msg_id);
    if (err != BBS_OK) { rel_close(h); return err; }

    msg_pack(rec, buf);
    err = rel_write(h, (const void *)buf, RECORD_SIZE_MSG_IDX);
    err = rel_close_keep(h, err);

    if (err != BBS_OK) return err;

#ifndef T64_STORE_SEQ
    /* Also update REU if enabled. Under T64_STORE_SEQ this is redundant:
     * the rel_write() above already updated the WINDOW region in REU bank 2,
     * which is the authoritative cache there — bank 0 would just be a
     * second copy of the same data. */
    if (bbs_cfg.reu_enabled) {
        reu_index_put(rec->msg_id, rec);
    }
#endif

    return BBS_OK;
}
#ifdef T64_STORE_SEQ
#undef err
#undef buf
#endif

/* ---- Scan visitor pattern ----------------------------------------- */

typedef u8 (*msg_visitor_t)(const msg_index_record_t *rec, void *ctx);

static u16 msg_scan_all(u8 board_id, u8 device, msg_visitor_t visit, void *ctx)
{
    rel_handle_t h;
    bbs_err_t err;
    msg_index_record_t rec;
    u8 buf[RECORD_SIZE_MSG_IDX];
    u16 rec_num;
    u16 visited = 0;
    u8 got;
    // cppcheck-suppress variableScope
    u8 stop;

    err = msg_open_idx(board_id, device, &h);
    if (err != BBS_OK) return 0;

    for (rec_num = 1; rec_num <= CFG_MSG_MAX_PER_BOARD; rec_num++) {
        memset(buf, 0, RECORD_SIZE_MSG_IDX);
        err = rel_read(h, (void *)buf, RECORD_SIZE_MSG_IDX, &got);
        if (err != BBS_OK || got < RECORD_READ_MIN) break;
        if (got < RECORD_SIZE_MSG_IDX)
            memset(buf + got, 0, RECORD_SIZE_MSG_IDX - got);

        msg_unpack(&rec, buf);
        if (rec.msg_id != 0) {
            visited++;
            stop = visit(&rec, ctx);
            if (stop) break;
        }
    }

    rel_close(h);
    return visited;
}

/* ---- Scan contexts ------------------------------------------------ */

typedef struct {
    u16 hwm;
    u16 last_call_date;
    u16 *out_ids;
    u8  max_ids;
    u8  found;
} scan_new_ctx_t;

static u8 scan_new_visitor(const msg_index_record_t *rec, void *ctx)
{
    scan_new_ctx_t *c = (scan_new_ctx_t *)ctx;
    if ((rec->flags & MSG_F_DELETED)) return 0;
    if (rec->msg_id > c->hwm && (c->last_call_date == 0 || rec->date > c->last_call_date)) {
        if (c->out_ids && c->found < c->max_ids) {
            c->out_ids[c->found] = rec->msg_id;
        }
        c->found++;
        if (c->out_ids && c->found >= c->max_ids) return 1;
    }
    return 0;
}

typedef struct {
    u16 root_id;
    u16 *out_ids;
    u8  max_ids;
    u8  found;
} thread_ctx_t;

static u8 thread_visitor(const msg_index_record_t *rec, void *ctx)
{
    thread_ctx_t *c = (thread_ctx_t *)ctx;
    if ((rec->flags & MSG_F_DELETED)) return 0;
    if (rec->thread_root_id == c->root_id) {
        if (c->out_ids && c->found < c->max_ids) {
            c->out_ids[c->found] = rec->msg_id;
        }
        c->found++;
        if (c->out_ids && c->found >= c->max_ids) return 1;
    }
    return 0;
}

typedef struct {
    u16 highest_id;
} max_id_ctx_t;

static u8 max_id_visitor(const msg_index_record_t *rec, void *ctx)
{
    max_id_ctx_t *c = (max_id_ctx_t *)ctx;
    if (rec->msg_id > c->highest_id) c->highest_id = rec->msg_id;
    return 0;
}

typedef struct {
    u16 highest_end;
} body_eof_ctx_t;

static u8 body_eof_visitor(const msg_index_record_t *rec, void *ctx)
{
    body_eof_ctx_t *c = (body_eof_ctx_t *)ctx;
    u16 end;
    if (rec->msg_id == 0) return 0;
    end = rec->body_offset + rec->body_len;
    if (end > c->highest_end) c->highest_end = end;
    return 0;
}

/* ---- msg_count_new ------------------------------------------------ */

u8 msg_count_new(u8 board_id, u16 hwm, u16 last_call_date, u8 device)
{
    scan_new_ctx_t ctx;

    ctx.hwm = hwm;
    ctx.last_call_date = last_call_date;
    ctx.out_ids = NULL;
    ctx.max_ids = 0;
    ctx.found = 0;

    /* REU bank 0 holds only the currently-entered board's index; scanning
     * all boards for new-message counts requires the on-disk path. */
    msg_scan_all(board_id, device, scan_new_visitor, &ctx);
    return ctx.found;
}

/* ---- msg_scan_new ------------------------------------------------- */

u8 msg_scan_new(u8 board_id, u16 hwm, u16 last_call_date,
                 u16 *out_ids, u8 max_ids, u8 device)
{
    scan_new_ctx_t ctx;
#ifndef T64_STORE_SEQ
    // cppcheck-suppress variableScope
    u16 rec_num;
    msg_index_record_t rec;
#endif

    ctx.hwm = hwm;
    ctx.last_call_date = last_call_date;
    ctx.out_ids = out_ids;
    ctx.max_ids = max_ids;
    ctx.found = 0;

#ifndef T64_STORE_SEQ
    if (bbs_cfg.reu_enabled) {
        for (rec_num = 1; rec_num <= CFG_MSG_MAX_PER_BOARD; rec_num++) {
            reu_index_get(rec_num, &rec);
            if (rec.msg_id == 0) continue;
            if (!(rec.flags & MSG_F_DELETED) &&
                rec.msg_id > hwm &&
                rec.date > last_call_date) {
                if (out_ids && ctx.found < max_ids)
                    out_ids[ctx.found] = rec.msg_id;
                ctx.found++;
                if (out_ids && ctx.found >= max_ids) break;
            }
        }
        return ctx.found;
    }
#endif

    msg_scan_all(board_id, device, scan_new_visitor, &ctx);
    return ctx.found;
}

/* ---- Thread traversal --------------------------------------------- */

u8 msg_thread_list(u8 board_id, u16 root_id,
                    u16 *out_ids, u8 max_ids, u8 device)
{
    thread_ctx_t ctx;
    ctx.root_id = root_id;
    ctx.out_ids = out_ids;
    ctx.max_ids = max_ids;
    ctx.found = 0;
    msg_scan_all(board_id, device, thread_visitor, &ctx);
    return ctx.found;
}

u16 msg_thread_next_unread(u8 board_id, u16 thread_root_id,
                            u16 hwm, u8 device)
{
    u16 ids[32];
    u8 count, i;

    count = msg_thread_list(board_id, thread_root_id, ids, 32, device);
    for (i = 0; i < count; i++) {
        if (ids[i] > hwm) return ids[i];
    }
    return 0;
}

u16 msg_next_unread_any(u8 board_id, u16 hwm, u16 last_call_date, u8 device)
{
    u16 ids[1];
    scan_new_ctx_t ctx;

    ctx.hwm = hwm;
    ctx.last_call_date = last_call_date;
    ctx.out_ids = ids;
    ctx.max_ids = 1;
    ctx.found = 0;

    msg_scan_all(board_id, device, scan_new_visitor, &ctx);
    if (ctx.found > 0) return ids[0];
    return 0;
}

/* ---- Internal helpers --------------------------------------------- */

static u16 msg_body_eof(u8 board_id, u8 device)
{
    body_eof_ctx_t ctx;
    ctx.highest_end = 0;
    msg_scan_all(board_id, device, body_eof_visitor, &ctx);
    return ctx.highest_end;
}

static u16 msg_next_id(u8 board_id, u8 device)
{
    max_id_ctx_t ctx;
    ctx.highest_id = 0;
    msg_scan_all(board_id, device, max_id_visitor, &ctx);
    return ctx.highest_id + 1;
}

/* ---- Flag operations --------------------------------------------- */

static bbs_err_t msg_set_flag(u8 board_id, u16 msg_id, u8 flag,
                               bool_t set, u8 device)
{
    bbs_err_t err;
    msg_index_record_t rec;

    if (board_id == 0 || msg_id == 0) return BBS_EBADARG;

    err = msg_index_get(board_id, msg_id, &rec, device);
    if (err != BBS_OK) return err;
    if (rec.msg_id == 0) return BBS_ENOTFOUND;

    if (set)
        rec.flags |= flag;
    else
        rec.flags &= (u8)~flag;

    return msg_index_put(board_id, &rec, device);
}

bbs_err_t msg_delete(u8 board_id, u16 msg_id, u8 device)
{
    bbs_err_t err;
    board_dir_record_t board;
    msg_index_record_t rec;

    err = msg_index_get(board_id, msg_id, &rec, device);
    if (err != BBS_OK) return err;
    if (rec.flags & MSG_F_DELETED) return BBS_OK;  /* already deleted */

    rec.flags |= MSG_F_DELETED;
    err = msg_index_put(board_id, &rec, device);
    if (err != BBS_OK) return err;

    err = board_by_id(board_id, &board, device);
    if (err != BBS_OK) return err;
    if (board.msg_count > 0) board.msg_count--;
    return board_save(&board, device);
}

bbs_err_t msg_freeze(u8 board_id, u16 msg_id, bool_t frozen, u8 device)
{
    return msg_set_flag(board_id, msg_id, MSG_F_FROZEN, frozen, device);
}

bbs_err_t msg_set_sticky(u8 board_id, u16 msg_id, bool_t sticky, u8 device)
{
    return msg_set_flag(board_id, msg_id, MSG_F_STICKY, sticky, device);
}

/* ---- Post --------------------------------------------------------- */

bbs_err_t msg_post(u8 board_id, u16 parent_id, u16 author_id, u16 to_id,
                   bool_t anonymous, u8 device, u16 *out_msg_id,
                   const char *subj, const char *date_mmddyy)
{
    bbs_err_t err;
    board_dir_record_t board;
    msg_index_record_t rec;
    msg_index_record_t parent_rec;
    u16 body_offset;
    u16 body_len = 0;
    u16 effective_limit;
    // cppcheck-suppress variableScope
    u8 i;

    if (board_id == 0 || !out_msg_id) return BBS_EBADARG;

    err = board_by_id(board_id, &board, device);
    if (err != BBS_OK) return err;

    /* Determine effective message limit */
    effective_limit = (board.max_msgs > 0)
        ? (u16)board.max_msgs
        : CFG_MSG_LIMIT_DEFAULT;

    /* Prune if at limit */
    if (effective_limit > 0 && board.msg_count >= effective_limit) {
        err = msg_prune_quantity(board_id, device);
        if (err != BBS_OK) return err;
        /* Reload board after prune */
        err = board_by_id(board_id, &board, device);
        if (err != BBS_OK) return err;
    }

    /* Hard ceiling */
    if (board.msg_count >= CFG_MSG_MAX_PER_BOARD) return BBS_EFULL;

    /* Determine body offset (EOF of TXT file) */
    body_offset = msg_body_eof(board_id, device);

    /* Commit compose buffer to B<n>.TXT */
    err = reu_compose_commit(board_id, device, &body_offset, &body_len);
    if (err != BBS_OK) return err;

    /* Build index record */
    memset(&rec, 0, sizeof(rec));
    rec.msg_id    = msg_next_id(board_id, device);
    rec.parent_id = parent_id;
    rec.author_id = anonymous ? 0 : author_id;
    rec.date      = 0;  /* u16 packed date unused; display uses BCD fields below */
    if (date_mmddyy && date_mmddyy[0]) {
        rec.month   = (u8)(((date_mmddyy[0] - '0') << 4) | (date_mmddyy[1] - '0'));
        rec.day     = (u8)(((date_mmddyy[3] - '0') << 4) | (date_mmddyy[4] - '0'));
        rec.year_yy = (u8)(((date_mmddyy[6] - '0') << 4) | (date_mmddyy[7] - '0'));
    }
    rec.to_id     = to_id;
    rec.body_offset = body_offset;
    rec.body_len    = body_len;
    if (anonymous) rec.flags |= MSG_F_ANON;

    /* Thread linkage */
    if (parent_id > 0) {
        err = msg_index_get(board_id, parent_id, &parent_rec, device);
        if (err == BBS_OK && parent_rec.msg_id != 0) {
            rec.thread_root_id = parent_rec.thread_root_id;
            parent_rec.reply_count++;
            msg_index_put(board_id, &parent_rec, device);
        } else {
            /* Orphan: parent not found */
            rec.thread_root_id = rec.msg_id;
            rec.flags |= MSG_F_ORPHAN;
        }
    } else {
        rec.thread_root_id = rec.msg_id;
    }

    /* Network board handling */
    if (board.flags & BOARD_F_NET) {
        for (i = 0; i < 8; i++)
            rec.net_origin_bbs[i] = bbs_cfg.bbs_id[i];
        rec.flags |= MSG_F_NET;
    }

    /* Subject preview */
    if (subj) {
        u8 k;
        for (k = 0; k < 30 && subj[k]; k++) rec.subj[k] = subj[k];
        rec.subj[k] = '\0';
    }

    err = msg_index_put(board_id, &rec, device);
    if (err != BBS_OK) return err;

    if (rec.msg_id > board.msg_high_id) board.msg_high_id = rec.msg_id;
    board.body_eof = (u16)(body_offset + body_len);
    board.msg_count++;
    err = board_save(&board, device);
    if (err != BBS_OK) return err;

    *out_msg_id = rec.msg_id;
    wfc.posts_today++;
    syscnt_save();
    return BBS_OK;
}

/* ---- Prune -------------------------------------------------------- */

bbs_err_t msg_prune_quantity(u8 board_id, u8 device)
{
    bbs_err_t err;
    board_dir_record_t board;
    msg_index_record_t rec;
    u16 effective_limit;
    u16 rec_num;
    u8  pruned;

    if (board_id == 0) return BBS_EBADARG;

    err = board_by_id(board_id, &board, device);
    if (err != BBS_OK) return err;

    effective_limit = (board.max_msgs > 0)
        ? (u16)board.max_msgs
        : CFG_MSG_LIMIT_DEFAULT;

    if (effective_limit == 0) return BBS_OK;
    if (board.msg_count < effective_limit) return BBS_OK;

    pruned = 0;
    for (rec_num = 1;
         rec_num <= CFG_MSG_MAX_PER_BOARD && pruned < CFG_MSG_PRUNE_BATCH;
         rec_num++) {
        err = msg_index_get(board_id, rec_num, &rec, device);
        if (err != BBS_OK) continue;
        if (rec.msg_id == 0) continue;
        if (rec.flags & MSG_F_DELETED) continue;
        if (rec.flags & MSG_F_STICKY)  continue;
        if (rec.flags & MSG_F_FROZEN)  continue;

        rec.flags |= MSG_F_DELETED;
        msg_index_put(board_id, &rec, device);
        if (board.msg_count > 0) board.msg_count--;
        pruned++;
    }

    return board_save(&board, device);
}

bbs_err_t msg_prune_age(u8 board_id, u16 today, u8 device)
{
    bbs_err_t err;
    board_dir_record_t board;
    msg_index_record_t rec;
    u16 rec_num;
    u16 cutoff;
    bool_t changed;

    if (board_id == 0) return BBS_EBADARG;

    err = board_by_id(board_id, &board, device);
    if (err != BBS_OK) return err;

    if (board.max_age_days == 0) return BBS_OK;

    /* Avoid underflow when today < max_age_days */
    if (today > (u16)board.max_age_days)
        cutoff = today - (u16)board.max_age_days;
    else
        cutoff = 0;

    changed = FALSE;
    for (rec_num = 1; rec_num <= CFG_MSG_MAX_PER_BOARD; rec_num++) {
        err = msg_index_get(board_id, rec_num, &rec, device);
        if (err != BBS_OK) continue;
        if (rec.msg_id == 0) continue;
        if (rec.flags & MSG_F_DELETED) continue;
        if (rec.flags & MSG_F_STICKY)  continue;
        if (rec.flags & MSG_F_FROZEN)  continue;
        if (rec.date == 0) continue;  /* skip; calendar date not set */
        if (rec.date < cutoff) {
            rec.flags |= MSG_F_DELETED;
            msg_index_put(board_id, &rec, device);
            if (board.msg_count > 0) board.msg_count--;
            changed = TRUE;
        }
    }

    if (changed) return board_save(&board, device);
    return BBS_OK;
}

/* ---- Body read ---------------------------------------------------- */

/* Disk scratch — msgs_bss is full (<2 bytes free); lives in main bss. */
#pragma bss(bss)
static u8 s_skip_buf[24];
#pragma bss(msgs_bss)

bbs_err_t msg_body_read(u8 board_id, const msg_index_record_t *rec,
                         char *buf, u16 buf_len, u8 device)
{
    char fname[16];
    bbs_err_t err;
    u16 to_read;
    u16 n;

    if (!rec || !buf || buf_len == 0 || board_id == 0) return BBS_EBADARG;

    /* Check body cache first */
    if (bbs_cfg.reu_enabled && reu_body_cached(board_id, rec->msg_id)) {
        return reu_body_fetch(board_id, rec->msg_id, buf, buf_len);
    }

    err = cfg_send_drive_init(device, bbs_cfg.init_msgs);
    if (err != BBS_OK) return err;

    msg_txt_fname(board_id, fname);
    err = disk_open(device, bbs_cfg.drive_msgs, fname, DISK_READ);
    if (err != BBS_OK) return err;

    /* Skip to body_offset in chunks — disk_read does one CHKIN/CLRCHN per
     * chunk instead of per byte. */
    {
        u16 left = rec->body_offset;
        while (left) {
            u8 step = (left > (u16)sizeof(s_skip_buf)) ? (u8)sizeof(s_skip_buf) : (u8)left;
            i16 r = disk_read(s_skip_buf, step);
            if (r <= 0) { disk_close(); return BBS_EIO; }
            left -= (u16)r;
        }
    }

    to_read = rec->body_len;
    if (to_read > buf_len - 1) to_read = buf_len - 1;

    n = 0;
    while (n < to_read) {
        u16 want = (u16)(to_read - n);
        u8 step = (want > 254u) ? 254u : (u8)want;
        i16 r = disk_read((u8 *)buf + n, step);
        if (r <= 0) break;
        n += (u16)r;
    }
    buf[n] = '\0';
    disk_close();

    /* Cache in REU if available */
    if (bbs_cfg.reu_enabled) {
        reu_body_store(board_id, rec->msg_id, buf, n);
    }

    return BBS_OK;
}
/* msg_body_each_line lives in the main code section (not the msgs overlay)
 * so its local frame arrays (linebuf, fname) do not consume msgs_bss.
 * Its string literals stay in msgs_data, so callers must hold the msgs
 * overlay loaded — true for all message-feature callers today. */
#pragma code(code)
#pragma bss(bss)
bbs_err_t msg_body_each_line(u8 board_id, const msg_index_record_t *rec,
                              msg_line_cb_t cb, void *ctx, u8 device)
{
    char linebuf[41];
    u8 linelen = 0;
    u16 pos;
    u16 body_len;

    if (!rec || !cb || board_id == 0) return BBS_EBADARG;
    body_len = rec->body_len;
    if (body_len == 0) return BBS_OK;

    /* REU path: stream byte-by-byte from body cache */
    if (bbs_cfg.reu_enabled && reu_body_cached(board_id, rec->msg_id)) {
        char c;
        for (pos = 0; pos < body_len; pos++) {
            reu_body_fetch_at(board_id, rec->msg_id, &c, 1, pos);
            if (c == '\n') {
                linebuf[linelen] = '\0'; cb(linebuf, ctx); linelen = 0;
            } else if (linelen < 40u) {
                linebuf[linelen++] = c;
            }
        }
        if (linelen) { linebuf[linelen] = '\0'; cb(linebuf, ctx); }
        return BBS_OK;
    }

    /* Disk path: char-by-char */
    {
        char fname[16];
        bbs_err_t err;
        u16 skip;

        err = cfg_send_drive_init(device, bbs_cfg.init_msgs);
        if (err != BBS_OK) return err;
        sprintf(fname, "B%u.TXT", (unsigned)board_id);  /* inline msg_txt_fname */
        err = disk_open(device, bbs_cfg.drive_msgs, fname, DISK_READ);
        if (err != BBS_OK) return err;
        skip = rec->body_offset;
        while (skip) {
            u8 step = (skip > (u16)sizeof(s_skip_buf)) ? (u8)sizeof(s_skip_buf) : (u8)skip;
            i16 r = disk_read(s_skip_buf, step);
            if (r <= 0) { disk_close(); return BBS_EIO; }
            skip -= (u16)r;
        }
        pos = 0;
        while (pos < body_len) {
            u16 want = (u16)(body_len - pos);
            u8 step = (want > (u16)sizeof(s_skip_buf)) ? (u8)sizeof(s_skip_buf) : (u8)want;
            i16 r = disk_read(s_skip_buf, step);
            i16 k;
            if (r <= 0) break;
            for (k = 0; k < r; k++) {
                char c = (char)s_skip_buf[k];
                if (c == '\n') {
                    linebuf[linelen] = '\0'; cb(linebuf, ctx); linelen = 0;
                } else if (linelen < 40u) {
                    linebuf[linelen++] = c;
                }
            }
            pos += (u16)r;
        }
        if (linelen) { linebuf[linelen] = '\0'; cb(linebuf, ctx); }
        disk_close();
    }
    return BBS_OK;
}
#pragma code(msgs_code)
#pragma bss(msgs_bss)

/* ---- Stubs -------------------------------------------------------- */

/* 64-byte chunk keeps BSS within editor binary limits; compact is a sysop
 * maintenance op so the extra open/close cycles per chunk are acceptable. */
#define COMPACT_CHUNK 64

bbs_err_t msg_compact(u8 board_id, u8 device)
{
    static char s_cbody[COMPACT_CHUNK];
    char fname_txt[16], fname_tmp[16];
    u16 msg_id, new_offset;
    bbs_err_t err;
    board_dir_record_t board;
    u8 any_active;

    if (board_id == 0 || board_id > CFG_MAX_BOARDS) return BBS_EBADARG;

    err = board_by_id(board_id, &board, device);
    if (err != BBS_OK) return err;

    err = cfg_send_drive_init(device, bbs_cfg.init_msgs);
    if (err != BBS_OK) return err;

    sprintf(fname_txt, "B%u.TXT", (unsigned)board_id);
    sprintf(fname_tmp, "B%u.TMP", (unsigned)board_id);

    /* Create empty TMP */
    err = disk_open(device, bbs_cfg.drive_msgs, fname_tmp, DISK_WRITE);
    if (err != BBS_OK) return err;
    disk_close();

    new_offset = 0;
    any_active = FALSE;

    for (msg_id = 1; msg_id <= CFG_MSG_MAX_PER_BOARD; msg_id++) {
        msg_index_record_t rec;
        u16 copied;

        err = msg_index_get(board_id, msg_id, &rec, device);
        if (err == BBS_ENOTFOUND) break;
        if (err != BBS_OK) continue;
        if (rec.body_len == 0) continue;
        if (rec.flags & MSG_F_DELETED) continue;

        /* Copy body in COMPACT_CHUNK-byte passes; CBM IEC allows only one
         * open file at a time so we re-open TXT and TMP each chunk. */
        copied = 0;
        while (copied < rec.body_len) {
            u16 want = rec.body_len - copied;
            u16 got = 0;
            u16 sk;
            u16 gi;

            if (want > COMPACT_CHUNK) want = COMPACT_CHUNK;

            err = disk_open(device, bbs_cfg.drive_msgs, fname_txt, DISK_READ);
            if (err != BBS_OK) return err;
            for (sk = rec.body_offset + copied; sk > 0; sk--) disk_getc();
            for (gi = 0; gi < want; gi++) {
                i16 b = disk_getc();
                if (b < 0) break;
                s_cbody[gi] = (char)(u8)b;
                got++;
            }
            disk_close();

            if (got == 0) break;

            err = disk_open(device, bbs_cfg.drive_msgs, fname_tmp, DISK_APPEND);
            if (err != BBS_OK) return err;
            for (gi = 0; gi < got; gi++) {
                err = disk_putc(s_cbody[gi]);
                if (err != BBS_OK) { disk_close(); return err; }
            }
            disk_close();
            copied += got;
        }

        rec.body_offset = new_offset;
        new_offset = (u16)(new_offset + copied);
        err = msg_index_put(board_id, &rec, device);
        if (err != BBS_OK) return err;
        any_active = TRUE;
    }

    err = disk_scratch(device, bbs_cfg.drive_msgs, fname_txt);
    if (err != BBS_OK && err != BBS_ENOTFOUND) return err;

    if (any_active) {
        err = disk_rename(device, bbs_cfg.drive_msgs, fname_tmp, fname_txt);
        if (err != BBS_OK) return err;
    } else {
        disk_scratch(device, bbs_cfg.drive_msgs, fname_tmp);
    }

    (void)board;
    return BBS_OK;
}

bbs_err_t msg_net_export(u8 board_id, u16 since_msg_id, u8 device)
{
    (void)board_id;
    (void)since_msg_id;
    (void)device;
    return BBS_ENOTIMPL;
}

bbs_err_t msg_net_import(u8 board_id, u8 device)
{
    (void)board_id;
    (void)device;
    return BBS_ENOTIMPL;
}
#pragma code(code)
#pragma data(data)
#pragma bss(bss)
