/**
 * TURBO/64 BBS — File Area Module (Implementation)
 *
 * Manages file upload/download area records via REL files.
 */

#include "bbs/file_areas.h"
#include "bbs/config.h"
#include "bbs/cfg.h"
#include "bbs/rel.h"
#include <string.h>
#include <stdio.h>

#define RECORD_READ_MIN 12

static void file_area_pack(const ud_area_record_t *rec, u8 *buf) {
  u8 i;
  memset(buf, 0, RECORD_SIZE_UD_AREA);
  buf[0] = rec->id;
  for (i = 0; i < 20; i++) {
    char c = rec->title[i];
    if (c == 0) c = ' ';
    buf[1 + i] = (u8)c;
  }
  buf[21] = rec->access_level;
  buf[22] = rec->upload_level;
  buf[23] = rec->device;
  buf[24] = rec->flags;
  buf[25] = (u8)(rec->free_blocks & 0xFF);
  buf[26] = (u8)((rec->free_blocks >> 8) & 0xFF);
  buf[27] = (u8)(rec->total_files & 0xFF);
  buf[28] = (u8)((rec->total_files >> 8) & 0xFF);
}

static void file_area_unpack(ud_area_record_t *rec, const u8 *buf) {
  u8 i;
  memset(rec, 0, sizeof(*rec));
  rec->id = buf[0];
  for (i = 0; i < 20; i++) {
    rec->title[i] = (char)buf[1 + i];
  }
  rec->access_level = buf[21];
  rec->upload_level = buf[22];
  /* access_level/upload_level are minimum-access thresholds; a corrupt
   * out-of-range byte clamps to the highest valid level (SYSOP-only) so a
   * damaged record fails safe by hiding the area rather than exposing it. */
  if (rec->access_level > CFG_ACCESS_SYSOP) rec->access_level = CFG_ACCESS_SYSOP;
  if (rec->upload_level > CFG_ACCESS_SYSOP) rec->upload_level = CFG_ACCESS_SYSOP;
  rec->device = buf[23];
  rec->flags = buf[24];
  rec->free_blocks = (u16)buf[25] | ((u16)buf[26] << 8);
  rec->total_files = (u16)buf[27] | ((u16)buf[28] << 8);
}

static u8 title_is_deleted(const char *title) {
  u8 i;
  for (i = 0; i < 20; i++) {
    if (title[i] != ' ' && title[i] != 0) {
      return FALSE;
    }
  }
  return TRUE;
}

static bbs_err_t file_area_open_rel(u8 device, rel_handle_t *h)
{
  bbs_err_t err;

  err = cfg_send_drive_init(device, bbs_cfg.init_files);
  if (err != BBS_OK) {
    return err;
  }

  return rel_open(device, bbs_cfg.drive_files, "UDS", RECORD_SIZE_UD_AREA, h);
}

/**
 * file_area_count()
 *
 * Count total non-deleted file areas.
 */
u8 file_area_count(u8 device) {
  bbs_err_t err;
  rel_handle_t h;
  ud_area_record_t rec;
  u8 buf[RECORD_SIZE_UD_AREA];
  u8 rec_num, count = 0;
  u8 got;

  /* Open "UDS" REL file */
  err = file_area_open_rel(device, &h);
  if (err != BBS_OK) {
    return 0;
  }

  /* Sequential scan, counting non-deleted records */
  for (rec_num = 1; rec_num <= CFG_MAX_FILE_AREAS; rec_num++) {
    memset(buf, 0, RECORD_SIZE_UD_AREA);
    err = rel_read(h, (void *)buf, RECORD_SIZE_UD_AREA, &got);
    if (err != BBS_OK || got < RECORD_READ_MIN) {
      break;
    }
    if (got < RECORD_SIZE_UD_AREA) {
      memset(buf + got, 0, RECORD_SIZE_UD_AREA - got);
    }
    file_area_unpack(&rec, buf);
    if (rec.id != 0 && !title_is_deleted(rec.title)) {
      count++;
    }
  }

  rel_close(h);
  return count;
}

/**
 * file_area_by_index()
 *
 * Get the Nth non-deleted file area (1-based index).
 */
bbs_err_t file_area_by_index(u8 n, ud_area_record_t *out_rec, u8 device) {
  bbs_err_t err;
  rel_handle_t h;
  u8 buf[RECORD_SIZE_UD_AREA];
  u8 rec_num, count = 0;
  u8 got;

  if (n == 0 || !out_rec) {
    return BBS_EBADARG;
  }

  /* Open "UDS" REL file */
  err = file_area_open_rel(device, &h);
  if (err != BBS_OK) {
    return err;
  }

  /* Sequential scan, skip deleted records until we reach the nth one */
  for (rec_num = 1; rec_num <= CFG_MAX_FILE_AREAS; rec_num++) {
    memset(buf, 0, RECORD_SIZE_UD_AREA);
    err = rel_read(h, (void *)buf, RECORD_SIZE_UD_AREA, &got);
    if (err != BBS_OK || got < RECORD_READ_MIN) {
      break;
    }
    if (got < RECORD_SIZE_UD_AREA) {
      memset(buf + got, 0, RECORD_SIZE_UD_AREA - got);
    }
    file_area_unpack(out_rec, buf);
    if (out_rec->id != 0 && !title_is_deleted(out_rec->title)) {
      count++;
      if (count == n) {
        rel_close(h);
        return BBS_OK;
      }
    }
  }

  rel_close(h);
  return BBS_ENOTFOUND;
}

/**
 * file_area_by_id()
 *
 * Load a file area record by area ID.
 */
bbs_err_t file_area_by_id(u8 id, ud_area_record_t *out_rec, u8 device) {
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
  u8 buf[RECORD_SIZE_UD_AREA];
#endif

  if (id == 0 || id > CFG_MAX_FILE_AREAS) {
    return BBS_EBADARG;
  }

  if (!out_rec) {
    return BBS_EBADARG;
  }

  /* Open "UDS" REL file */
  err = file_area_open_rel(device, &h);
  if (err != BBS_OK) {
    return err;
  }

  /* Position to area record */
  err = rel_position(h, id);
  if (err != BBS_OK) {
    rel_close(h);
    return err;
  }

  /* Read record */
  memset(buf, 0, RECORD_SIZE_UD_AREA);
  err = rel_read(h, (void *)buf, RECORD_SIZE_UD_AREA, &got);
  rel_close(h);

  if (err != BBS_OK) {
    return err;
  }

  if (got < RECORD_READ_MIN) {
    return BBS_EIO;
  }

  file_area_unpack(out_rec, buf);

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

/**
 * file_area_save()
 *
 * Write a file area record back to disk.
 */
bbs_err_t file_area_save(const ud_area_record_t *rec, u8 device) {
  rel_handle_t h;
#ifdef T64_STORE_SEQ
#define err rel_scratch_err
#define buf rel_scratch_buf
#else
  bbs_err_t err;
  u8 buf[RECORD_SIZE_UD_AREA];
#endif

  if (!rec || rec->id == 0 || rec->id > CFG_MAX_FILE_AREAS) {
    return BBS_EBADARG;
  }

  /* Open "UDS" REL file */
  err = file_area_open_rel(device, &h);
  if (err != BBS_OK) {
    return err;
  }

  /* Position to area record */
  err = rel_position(h, rec->id);
  if (err != BBS_OK) {
    rel_close(h);
    return err;
  }

  /* Write record */
  file_area_pack(rec, buf);
  err = rel_write(h, (const void *)buf, RECORD_SIZE_UD_AREA);
  err = rel_close_keep(h, err);

  return err;
}
#ifdef T64_STORE_SEQ
#undef err
#undef buf
#endif

/**
 * file_area_create()
 *
 * Create a new file upload/download area.
 */
bbs_err_t file_area_create(const char *title, u8 access_level, u8 upload_level,
                           u8 device, u8 *out_id) {
  bbs_err_t err;
  rel_handle_t h;
  ud_area_record_t rec;
  u8 buf[RECORD_SIZE_UD_AREA];
  u8 rec_num, highest_id = 0;
  u8 got;

  if (!title || !out_id) {
    return BBS_EBADARG;
  }

  /* Open "UDS" REL file */
  err = file_area_open_rel(device, &h);
  if (err != BBS_OK) {
    return err;
  }

  /* Find highest area ID to assign next ID */
  for (rec_num = 1; rec_num <= CFG_MAX_FILE_AREAS; rec_num++) {
    memset(buf, 0, RECORD_SIZE_UD_AREA);
    err = rel_read(h, (void *)buf, RECORD_SIZE_UD_AREA, &got);
    if (err != BBS_OK || got < RECORD_READ_MIN) {
      break;
    }
    if (got < RECORD_SIZE_UD_AREA) {
      memset(buf + got, 0, RECORD_SIZE_UD_AREA - got);
    }
    file_area_unpack(&rec, buf);
    if (rec.id != 0 && rec.id > highest_id) {
      highest_id = rec.id;
    }
  }

  rel_close(h);

  /* Check if we can create a new area */
  if (highest_id >= CFG_MAX_FILE_AREAS) {
    return BBS_EFULL;
  }

  /* Create new area record */
  *out_id = highest_id + 1;
  memset(&rec, 0, sizeof(rec));
  rec.id = *out_id;
  strncpy(rec.title, title, 19);
  rec.title[19] = 0;
  rec.access_level = access_level;
  rec.upload_level = upload_level;
  rec.device = 8;  /* Default to device 8 */

  /* Save new area */
  return file_area_save(&rec, device);
}

/**
 * file_area_delete()
 *
 * Soft-delete a file area by clearing the title.
 */
bbs_err_t file_area_delete(u8 area_id, u8 device) {
  bbs_err_t err;
  ud_area_record_t rec;

  if (area_id == 0 || area_id > CFG_MAX_FILE_AREAS) {
    return BBS_EBADARG;
  }

  /* Load area record */
  err = file_area_by_id(area_id, &rec, device);
  if (err != BBS_OK) {
    return err;
  }

  /* Clear title to mark as deleted */
  memset(rec.title, ' ', sizeof(rec.title));

  /* Write back */
  return file_area_save(&rec, device);
}
