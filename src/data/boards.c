/**
 * TURBO/64 BBS — Board Module (Implementation)
 *
 * Manages board directory records via REL files.
 */

#include "bbs/boards.h"
#include "bbs/config.h"
#include "bbs/cfg.h"
#include "bbs/rel.h"
#include <string.h>
#include <stdio.h>

#define RECORD_READ_MIN 12

static void board_pack(const board_dir_record_t *rec, u8 *buf) {
  u8 i;
  memset(buf, 0, RECORD_SIZE_BOARD_DIR);
  buf[0] = rec->id;
  buf[1] = rec->flags;
  for (i = 0; i < 16; i++) {
    char c = rec->title[i];
    if (c == 0) c = ' ';
    buf[2 + i] = (u8)c;
  }
  buf[18] = (u8)(rec->subop_id & 0xFF);
  buf[19] = (u8)((rec->subop_id >> 8) & 0xFF);
  buf[20] = rec->read_level;
  buf[21] = rec->write_level;
  buf[22] = (u8)(rec->msg_count & 0xFF);
  buf[23] = (u8)((rec->msg_count >> 8) & 0xFF);
  for (i = 0; i < 8; i++) buf[24 + i] = (u8)rec->net_area_tag[i];
  /* bytes 32-35: reserved (was board password), left zeroed */
  buf[36] = rec->max_msgs;
  buf[37] = rec->max_age_days;
  buf[38] = (u8)(rec->msg_high_id & 0xFF);
  buf[39] = (u8)((rec->msg_high_id >> 8) & 0xFF);
  buf[40] = (u8)(rec->body_eof & 0xFF);
  buf[41] = (u8)((rec->body_eof >> 8) & 0xFF);
  buf[42] = rec->display_order;
  /* byte 43: reserved, already zeroed */
}

static void board_unpack(board_dir_record_t *rec, const u8 *buf) {
  u8 i;
  memset(rec, 0, sizeof(*rec));
  rec->id          = buf[0];
  rec->flags       = buf[1];
  for (i = 0; i < 16; i++) rec->title[i] = (char)buf[2 + i];
  rec->subop_id    = (u16)buf[18] | ((u16)buf[19] << 8);
  rec->read_level  = buf[20];
  rec->write_level = buf[21];
  /* Clamping to SYSOP (not 0) keeps a corrupt board visible to the one
   * account that can repair it, instead of hiding it from everyone. */
  if (rec->read_level  > CFG_ACCESS_SYSOP) rec->read_level  = CFG_ACCESS_SYSOP;
  if (rec->write_level > CFG_ACCESS_SYSOP) rec->write_level = CFG_ACCESS_SYSOP;
  rec->msg_count   = (u16)buf[22] | ((u16)buf[23] << 8);
  for (i = 0; i < 8; i++) rec->net_area_tag[i] = (char)buf[24 + i];
  /* bytes 32-35: reserved (was board password), ignored */
  rec->max_msgs     = buf[36];
  rec->max_age_days = buf[37];
  rec->msg_high_id  = (u16)buf[38] | ((u16)buf[39] << 8);
  rec->body_eof     = (u16)buf[40] | ((u16)buf[41] << 8);
  /* Legacy records have byte 42 = 0; treat that as "ordered by id". */
  rec->display_order = buf[42] ? buf[42] : rec->id;
}

static u8 title_is_deleted(const char *title) {
  u8 i;
  for (i = 0; i < 16; i++) {
    if (title[i] != ' ' && title[i] != 0) {
      return FALSE;
    }
  }
  return TRUE;
}

static bbs_err_t board_open_rel(u8 device, rel_handle_t *h)
{
  bbs_err_t err;

  err = cfg_send_drive_init(device, bbs_cfg.init_msgs);
  if (err != BBS_OK) {
    return err;
  }

  return rel_open(device, bbs_cfg.drive_msgs, "BOARDS", RECORD_SIZE_BOARD_DIR, h);
}

u8 board_count(u8 device) {
  bbs_err_t err;
  rel_handle_t h;
  board_dir_record_t rec;
  u8 buf[RECORD_SIZE_BOARD_DIR];
  u8 rec_num, count = 0;
  u8 got;

  err = board_open_rel(device, &h);
  if (err != BBS_OK) {
    return 0;
  }

  rel_position(h, 1);
  for (rec_num = 1; rec_num <= CFG_MAX_BOARDS; rec_num++) {
    memset(buf, 0, RECORD_SIZE_BOARD_DIR);
    err = rel_read(h, (void *)buf, RECORD_SIZE_BOARD_DIR, &got);
    if (err != BBS_OK || got < RECORD_READ_MIN) {
      break;
    }
    if (got < RECORD_SIZE_BOARD_DIR) {
      memset(buf + got, 0, RECORD_SIZE_BOARD_DIR - got);
    }
    board_unpack(&rec, buf);
    if (rec.id != 0 && rec.id <= CFG_MAX_BOARDS && !title_is_deleted(rec.title)) {
      count++;
    }
  }

  rel_close(h);
  return count;
}

bbs_err_t board_by_index(u8 n, board_dir_record_t *out_rec, u8 device) {
  bbs_err_t err;
  rel_handle_t h;
  u8 buf[RECORD_SIZE_BOARD_DIR];
  u8 ids[CFG_MAX_BOARDS], orders[CFG_MAX_BOARDS];
  u8 rec_num, count = 0, got;
  u8 c, o, target;
  board_dir_record_t tmp;

  if (n == 0 || !out_rec) {
    return BBS_EBADARG;
  }

  err = board_open_rel(device, &h);
  if (err != BBS_OK) {
    return err;
  }

  /* Pass 1: collect (id, display_order) of every valid board. */
  rel_position(h, 1);
  for (rec_num = 1; rec_num <= CFG_MAX_BOARDS; rec_num++) {
    memset(buf, 0, RECORD_SIZE_BOARD_DIR);
    err = rel_read(h, (void *)buf, RECORD_SIZE_BOARD_DIR, &got);
    if (err != BBS_OK || got < RECORD_READ_MIN) {
      break;
    }
    if (got < RECORD_SIZE_BOARD_DIR) {
      memset(buf + got, 0, RECORD_SIZE_BOARD_DIR - got);
    }
    board_unpack(&tmp, buf);
    if (tmp.id != 0 && tmp.id <= CFG_MAX_BOARDS && !title_is_deleted(tmp.title)) {
      ids[count]    = tmp.id;
      orders[count] = tmp.display_order;
      count++;
    }
  }
  rel_close(h);

  if (n > count) {
    return BBS_ENOTFOUND;
  }

  /* Select the board at rank n by ascending (display_order, id). */
  target = 0xFF;
  for (c = 0; c < count; c++) {
    u8 rank = 1;
    for (o = 0; o < count; o++) {
      if (orders[o] < orders[c] ||
          (orders[o] == orders[c] && ids[o] < ids[c])) {
        rank++;
      }
    }
    if (rank == n) { target = c; break; }
  }
  if (target == 0xFF) {
    return BBS_ENOTFOUND;
  }

  return board_by_id(ids[target], out_rec, device);
}

bbs_err_t board_by_id(u8 id, board_dir_record_t *out_rec, u8 device) {
  rel_handle_t h;
  /* err/got/buf alias the shared rel_scratch under T64_STORE_SEQ — see the
   * comment on rel_scratch_buf/got/err in bbs/rel.h. */
#ifdef T64_STORE_SEQ
#define err rel_scratch_err
#define got rel_scratch_got
#define buf rel_scratch_buf
#else
  bbs_err_t err;
  u8 got;
  u8 buf[RECORD_SIZE_BOARD_DIR];
#endif

  if (id == 0 || id > CFG_MAX_BOARDS) {
    return BBS_EBADARG;
  }

  if (!out_rec) {
    return BBS_EBADARG;
  }

  err = board_open_rel(device, &h);
  if (err != BBS_OK) {
    return err;
  }

  err = rel_position(h, id);
  if (err != BBS_OK) {
    rel_close(h);
    return err;
  }

  memset(buf, 0, RECORD_SIZE_BOARD_DIR);
  err = rel_read(h, (void *)buf, RECORD_SIZE_BOARD_DIR, &got);
  rel_close(h);

  if (err != BBS_OK) {
    return err;
  }

  if (got < RECORD_READ_MIN) {
    return BBS_EIO;
  }

  board_unpack(out_rec, buf);

  if (out_rec->id == 0 || title_is_deleted(out_rec->title)) {
    return BBS_ENOTFOUND;
  }

  return BBS_OK;
}
#ifdef T64_STORE_SEQ
#undef err
#undef got
#undef buf
#endif

bbs_err_t board_save(const board_dir_record_t *rec, u8 device) {
  rel_handle_t h;
#ifdef T64_STORE_SEQ
#define err rel_scratch_err
#define buf rel_scratch_buf
#else
  bbs_err_t err;
  u8 buf[RECORD_SIZE_BOARD_DIR];
#endif

  if (!rec || rec->id == 0 || rec->id > CFG_MAX_BOARDS) {
    return BBS_EBADARG;
  }

  err = board_open_rel(device, &h);
  if (err != BBS_OK) {
    return err;
  }

  err = rel_position(h, rec->id);
  if (err != BBS_OK) {
    rel_close(h);
    return err;
  }

  board_pack(rec, buf);
  err = rel_write(h, (const void *)buf, RECORD_SIZE_BOARD_DIR);
  err = rel_close_keep(h, err);

  return err;
}
#ifdef T64_STORE_SEQ
#undef err
#undef buf
#endif

bbs_err_t board_create(const char *title, u8 read_level, u8 write_level, u8 device, u8 *out_id) {
  bbs_err_t err;
  rel_handle_t h;
  board_dir_record_t rec;
  u8 buf[RECORD_SIZE_BOARD_DIR];
  u8 rec_num, highest_id = 0;
  u8 got;

  if (!title || !out_id) {
    return BBS_EBADARG;
  }

  err = board_open_rel(device, &h);
  if (err != BBS_OK) {
    return err;
  }

  for (rec_num = 1; rec_num <= CFG_MAX_BOARDS; rec_num++) {
    memset(buf, 0, RECORD_SIZE_BOARD_DIR);
    err = rel_read(h, (void *)buf, RECORD_SIZE_BOARD_DIR, &got);
    if (err != BBS_OK || got < RECORD_READ_MIN) {
      break;
    }
    if (got < RECORD_SIZE_BOARD_DIR) {
      memset(buf + got, 0, RECORD_SIZE_BOARD_DIR - got);
    }
    board_unpack(&rec, buf);
    if (rec.id != 0 && rec.id <= CFG_MAX_BOARDS && rec.id > highest_id) {
      highest_id = rec.id;
    }
  }

  rel_close(h);

  if (highest_id >= CFG_MAX_BOARDS) {
    return BBS_EFULL;
  }

  *out_id = highest_id + 1;
  memset(&rec, 0, sizeof(rec));
  rec.id = *out_id;
  rec.display_order = *out_id;   /* default new boards to id order */
  strncpy(rec.title, title, 15);
  rec.title[15] = 0;
  rec.read_level  = read_level;
  rec.write_level = write_level;

  return board_save(&rec, device);
}

bbs_err_t board_delete(u8 board_id, u8 device) {
  bbs_err_t err;
  board_dir_record_t rec;

  if (board_id == 0 || board_id > CFG_MAX_BOARDS) {
    return BBS_EBADARG;
  }

  err = board_by_id(board_id, &rec, device);
  if (err != BBS_OK) {
    return err;
  }

  memset(rec.title, ' ', sizeof(rec.title));

  return board_save(&rec, device);
}

bbs_err_t board_set_subop(u8 board_id, u16 user_id, u8 device)
{
  bbs_err_t err;
  board_dir_record_t rec;
  err = board_by_id(board_id, &rec, device);
  if (err != BBS_OK) return err;
  rec.subop_id = user_id;
  return board_save(&rec, device);
}

bbs_err_t board_set_net_area(u8 board_id, const char *area_tag, u8 device)
{
  bbs_err_t err;
  board_dir_record_t rec;
  err = board_by_id(board_id, &rec, device);
  if (err != BBS_OK) return err;
  if (area_tag == NULL) {
    memset(rec.net_area_tag, 0, 8);
    rec.flags &= (u8)~BOARD_F_NET;
  } else {
    u8 i;
    memset(rec.net_area_tag, ' ', 8);
    for (i = 0; i < 8 && area_tag[i]; i++)
      rec.net_area_tag[i] = area_tag[i];
    rec.flags |= BOARD_F_NET;
  }
  return board_save(&rec, device);
}
