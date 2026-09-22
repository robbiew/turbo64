/**
 * TURBO/64 BBS — User Module (Implementation)
 *
 * Manages user record access via REL files.
 */

#include "bbs/users.h"
#include "bbs/config.h"
#include "bbs/cfg.h"
#include "bbs/rel.h"
#include "bbs/hal/reu.h"
#include <string.h>
#include <stdio.h>

/* Minimum bytes from a REL record read to be considered usable.
 * CBM DOS 1581 (U64 emulation) signals EOI at the first trailing zero byte,
 * so a 23-byte record with zeros from offset 17 onwards arrives as 19 bytes.
 * Fields 0-16 (id + handle + password hash + access level) are all we need
 * for login.  We zero-fill the rest after reading. */
#define RECORD_READ_MIN 12

/* This whole cache is redundant under T64_STORE_SEQ: rel_open("USR LOG")
 * already loads the full user table into REU (the USERS region at bank 2
 * offset 0x4000 — rel_seq.c's own region map, distinct from REU_REGION_USERS
 * at offset 0x0000 below) and rel_read() is itself a DMA read from there.
 * Keeping this second REU-resident copy would cost resident bytes for no
 * benefit, so the whole mechanism compiles out of the SIEC build. */
#ifndef T64_STORE_SEQ
/* TRUE once user_cache_load() has populated the REU data tier (Bank 2). When
 * set, reads serve from REU; writes are write-through. Disk stays authoritative. */
static bool_t s_user_cache_valid = FALSE;

/* Fixed scratch for all REU user-record DMA. MUST be file-static, not a stack
 * local: oscar64 can overlay a stack slot with another live variable depending
 * on code layout, which would feed the DMA garbage (the frame-overlay bug). A
 * fixed BSS address is immune. */
static u8 s_reu_scratch[RECORD_SIZE_USER];

bool_t user_cache_active(void) { return s_user_cache_valid; }
#endif

static void user_pack(const user_record_t *rec, u8 *buf) {
  u8 i;
  memset(buf, 0, RECORD_SIZE_USER);
  buf[0] = rec->id;
  for (i = 0; i < 15; i++) {
    char c = rec->handle[i];
    if (c == 0) c = ' ';
    buf[1 + i] = (u8)c;
  }
  for (i = 0; i < 4; i++) {
    buf[16 + i] = (u8)rec->password[i];
  }
  buf[20] = rec->access_level;
  buf[21] = rec->credit_balance;
  buf[22] = (u8)(rec->calls & 0xFF);
  buf[23] = (u8)((rec->calls >> 8) & 0xFF);
  buf[24] = rec->downloads;
  buf[25] = rec->uploads;
  buf[26] = rec->term_mode;
  buf[27] = rec->term_width;
  buf[28] = rec->term_rows;
  buf[29] = rec->flags;
}

static void user_unpack(user_record_t *rec, const u8 *buf) {
  u8 i;
  memset(rec, 0, sizeof(*rec));
  rec->id = buf[0];
  for (i = 0; i < 15; i++) {
    rec->handle[i] = (char)buf[1 + i];
  }
  for (i = 0; i < 4; i++) {
    rec->password[i] = (char)buf[16 + i];
  }
  rec->access_level = buf[20];
  /* A corrupt/tampered record must never grant privileges: out-of-range
   * levels demote to DELETED rather than clamping up toward SYSOP. */
  if (rec->access_level > CFG_ACCESS_SYSOP) rec->access_level = CFG_ACCESS_DELETED;
  rec->credit_balance = buf[21];
  rec->calls = (u16)buf[22] | ((u16)buf[23] << 8);
  rec->downloads = buf[24];
  rec->uploads = buf[25];
  /* New fields added in Phase A; default to PETSCII/40-col for existing records */
  rec->term_mode = buf[26];       /* PETSCII by default */
  rec->term_width = buf[27];      /* 40 columns by default */
  rec->term_rows = buf[28];       /* 24 rows by default */
  rec->flags = buf[29];           /* 0 on old records = all flags off */
}

static u8 handle_eq(const char *a, const char *b) {
  u8 i;
  for (i = 0; i < 15; i++) {
    char ca = a[i];
    char cb = b[i];
    if (ca == ' ') ca = 0;
    if (cb == ' ') cb = 0;
    if (ca >= 'a' && ca <= 'z') ca -= 0x20;
    if (cb >= 'a' && cb <= 'z') cb -= 0x20;
    if (ca != cb) return FALSE;
    if (ca == 0) return TRUE;
  }
  return TRUE;
}

static bbs_err_t user_open_rel(u8 device, const char *name, u8 record_size,
                               rel_handle_t *h)
{
  bbs_err_t err;

  err = cfg_send_drive_init(device, bbs_cfg.init_system);
  if (err != BBS_OK) {
    return err;
  }

  return rel_open(device, bbs_cfg.drive_system, name, record_size, h);
}

/**
 * user_by_id()
 *
 * Load a user record by user ID.
 */
bbs_err_t user_by_id(u8 id, user_record_t *out_rec, u8 device) {
  rel_handle_t h;
  /* err/got/buf alias the shared rel_scratch under T64_STORE_SEQ rather than
   * being plain locals — see the comment on rel_scratch_buf/got/err in bbs/rel.h. */
#ifdef T64_STORE_SEQ
#define err rel_scratch_err
#define got rel_scratch_got
#define buf rel_scratch_buf
#else
  bbs_err_t err;
  u8 got;
  u8 buf[RECORD_SIZE_USER];
#endif

  if (id == 0) {
    return BBS_EBADARG;
  }

#ifndef T64_STORE_SEQ
  if (s_user_cache_valid && id <= USERS_MAX) {
    reu_data_get(REU_REGION_USERS + (u16)(id - 1) * RECORD_SIZE_USER, s_reu_scratch, RECORD_SIZE_USER);
    user_unpack(out_rec, s_reu_scratch);
    return (out_rec->id == 0) ? BBS_ENOTFOUND : BBS_OK;
  }
#endif

  /* Open "USR LOG" REL file */
  err = user_open_rel(device, "USR LOG", RECORD_SIZE_USER, &h);
  if (err != BBS_OK) {
    return err;
  }

  /* Position to user record (id is 1-based; REL is 1-based) */
  err = rel_position(h, id);
  if (err != BBS_OK) {
    rel_close(h);
    return err;
  }

  /* Read record — zero-fill buf first so trailing fields default to 0
   * if CBM DOS signals EOI early (U64 1581 emulation early-EOF quirk). */
  memset(buf, 0, RECORD_SIZE_USER);
  err = rel_read(h, (void *)buf, RECORD_SIZE_USER, &got);
  rel_close(h);

  if (err != BBS_OK) {
    return err;
  }

  if (got < RECORD_READ_MIN) {
    return BBS_EIO;  /* Too short to unpack anything useful */
  }

  user_unpack(out_rec, buf);

  if (out_rec->id == 0) {
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
 * user_by_handle()
 *
 * Search for a user by handle (username).
 * Scans user REL file sequentially, comparing handles.
 *
 * Returns user ID if found, or 0 if not found.
 */
u8 user_by_handle(const char *handle, u8 device) {
  bbs_err_t err;
  rel_handle_t h;
  user_record_t rec;
  u8 buf[RECORD_SIZE_USER];
  u8 rec_num;
  u8 got;

  if (!handle || handle[0] == 0) {
    return 0;
  }

#ifndef T64_STORE_SEQ
  if (s_user_cache_valid) {
    for (rec_num = 1; rec_num <= USERS_MAX; rec_num++) {
      reu_data_get(REU_REGION_USERS + (u16)(rec_num - 1) * RECORD_SIZE_USER, s_reu_scratch, RECORD_SIZE_USER);
      user_unpack(&rec, s_reu_scratch);
      if (rec.id != 0 && handle_eq(rec.handle, handle)) {
        return rec.id;
      }
    }
    return 0;
  }
#endif

  /* Open "USR LOG" REL file */
  err = user_open_rel(device, "USR LOG", RECORD_SIZE_USER, &h);
  if (err != BBS_OK) {
    return 0;
  }

  /* Sequential scan — NO P command per record.
   *
   * After rel_open, CBM DOS positions at record 1 byte 1.  Reading exactly
   * RECORD_SIZE_USER bytes advances the pointer to the next record automatically.
   * Because USR LOG is pre-allocated (USERS_MAX records written at init time by
   * CONFIGURE), every read returns a full record; RECORD NOT PRESENT is never
   * triggered during a normal scan.
   *
   * This approach removes P command dependency from the login path entirely.
   * (P commands are still used in user_save for targeted writes.) */
  /* RECORD_READ_MIN: minimum bytes for a usable record (see top of file). */
  for (rec_num = 1; rec_num <= USERS_MAX; rec_num++) {
    memset(buf, 0, RECORD_SIZE_USER);
    err = rel_read(h, (void *)buf, RECORD_SIZE_USER, &got);
    if (err != BBS_OK || got < RECORD_READ_MIN) {
      break;  /* I/O error or truly short read */
    }
    /* Zero-fill any bytes CBM DOS didn't deliver (trailing-zero early-EOF). */
    if (got < RECORD_SIZE_USER) {
      memset(buf + got, 0, RECORD_SIZE_USER - got);
    }

    user_unpack(&rec, buf);
    if (rec.id != 0 && handle_eq(rec.handle, handle)) {
      rel_close(h);
      return rec.id;
    }
  }

  rel_close(h);
  return 0;
}

/**
 * user_save()
 *
 * Write a user record back to disk.
 */
bbs_err_t user_save(const user_record_t *rec, u8 device) {
  rel_handle_t h;
#ifdef T64_STORE_SEQ
#define err rel_scratch_err
#define buf rel_scratch_buf
#else
  bbs_err_t err;
  u8 buf[RECORD_SIZE_USER];
#endif

  if (!rec || rec->id == 0) {
    return BBS_EBADARG;
  }

  /* Open "USR LOG" REL file for writing */
  err = user_open_rel(device, "USR LOG", RECORD_SIZE_USER, &h);
  if (err != BBS_OK) {
    return err;
  }

  /* Position to user record */
  err = rel_position(h, rec->id);
  if (err != BBS_OK) {
    rel_close(h);
    return err;
  }

  /* Write record */
  user_pack(rec, buf);
  err = rel_write(h, (const void *)buf, RECORD_SIZE_USER);
  err = rel_close_keep(h, err);

#ifndef T64_STORE_SEQ
  if (err == BBS_OK && s_user_cache_valid && rec->id <= USERS_MAX) {
    /* write-through: disk already authoritative above; keep REU cache coherent.
     * Copy via the fixed scratch so the DMA source can't be frame-overlaid. */
    memcpy(s_reu_scratch, buf, RECORD_SIZE_USER);
    reu_data_put(REU_REGION_USERS + (u16)(rec->id - 1) * RECORD_SIZE_USER, s_reu_scratch, RECORD_SIZE_USER);
  }
#endif

  return err;
}
#ifdef T64_STORE_SEQ
#undef err
#undef buf
#endif

/**
 * user_next_id()
 *
 * Find the next available user ID.
 * Returns 0 if table is full.
 */
u8 user_next_id(u8 device) {
  bbs_err_t err;
  rel_handle_t h;
  user_record_t rec;
  u8 buf[RECORD_SIZE_USER];
  u8 rec_num, highest_id = 0;
  u8 got;

  /* Open "USR LOG" REL file */
  err = user_open_rel(device, "USR LOG", RECORD_SIZE_USER, &h);
  if (err != BBS_OK) {
    return 1;  /* File doesn't exist yet — start from ID 1 */
  }

  /* Sequential scan — same early-EOF tolerance as user_by_handle.
   * We zero-fill buf before each read so trailing zeros don't matter. */
  for (rec_num = 1; rec_num <= USERS_MAX; rec_num++) {
    memset(buf, 0, RECORD_SIZE_USER);
    err = rel_read(h, (void *)buf, RECORD_SIZE_USER, &got);
    if (err != BBS_OK || got < RECORD_READ_MIN) {
      break;
    }
    if (got < RECORD_SIZE_USER) {
      memset(buf + got, 0, RECORD_SIZE_USER - got);
    }
    user_unpack(&rec, buf);
    if (rec.id != 0 && rec.id > highest_id) {
      highest_id = rec.id;
    }
  }

  rel_close(h);

  if (highest_id < USERS_MAX) {
    return highest_id + 1;
  }

  return 0;  /* Table full */
}

/* A slot holds a real user only if it has an id AND a non-blank handle.
 * user_delete() soft-deletes by blanking the handle (id is kept); never-used
 * slots are all-zero. Mirrors boards' title_is_deleted() convention. */
static bool_t user_is_live(const user_record_t *r) {
  return (bool_t)(r->id != 0 && r->handle[0] != ' ' && r->handle[0] != '\0');
}

/**
 * user_count()
 *
 * Count total non-deleted users.
 */
u8 user_count(u8 device) {
  bbs_err_t err;
  rel_handle_t h;
  user_record_t rec;
  u8 buf[RECORD_SIZE_USER];
  u8 rec_num, count = 0;
  u8 got;

  /* Open "USR LOG" REL file */
  err = user_open_rel(device, "USR LOG", RECORD_SIZE_USER, &h);
  if (err != BBS_OK) {
    return 0;
  }

  /* Sequential scan, counting non-deleted records */
  for (rec_num = 1; rec_num <= USERS_MAX; rec_num++) {
    memset(buf, 0, RECORD_SIZE_USER);
    err = rel_read(h, (void *)buf, RECORD_SIZE_USER, &got);
    if (err != BBS_OK || got < RECORD_READ_MIN) {
      break;
    }
    if (got < RECORD_SIZE_USER) {
      memset(buf + got, 0, RECORD_SIZE_USER - got);
    }
    user_unpack(&rec, buf);
    if (user_is_live(&rec)) {
      count++;
    }
  }

  rel_close(h);
  return count;
}

void user_cache_load(u8 device) {
#ifndef T64_STORE_SEQ
  rel_handle_t h;
  u8 rec_num, got;
  bbs_err_t err;

  s_user_cache_valid = FALSE;
  if (!reu_data_available()) return;

  err = user_open_rel(device, "USR LOG", RECORD_SIZE_USER, &h);
  if (err != BBS_OK) return;

  for (rec_num = 1; rec_num <= USERS_MAX; rec_num++) {
    memset(s_reu_scratch, 0, RECORD_SIZE_USER);
    err = rel_read(h, (void *)s_reu_scratch, RECORD_SIZE_USER, &got);
    if (err != BBS_OK || got < RECORD_READ_MIN) break;
    if (got < RECORD_SIZE_USER) memset(s_reu_scratch + got, 0, RECORD_SIZE_USER - got);
    reu_data_put(REU_REGION_USERS + (u16)(rec_num - 1) * RECORD_SIZE_USER, s_reu_scratch, RECORD_SIZE_USER);
  }
  rel_close(h);
  s_user_cache_valid = TRUE;
#else
  /* Redundant under T64_STORE_SEQ — see the comment above s_user_cache_valid's
   * (now compiled-out) declaration. Not called from main.c in this build, so
   * this whole function dead-strips; kept as a no-op body (rather than an
   * #ifdef'd-out declaration) so the bbs/users.h prototype stays valid for
   * both builds without touching that header. */
  (void)device;
#endif
}

/**
 * user_by_index()
 *
 * Get the Nth non-deleted user (1-based index).
 */
bbs_err_t user_by_index(u8 n, user_record_t *out_rec, u8 device) {
  bbs_err_t err;
  rel_handle_t h;
  u8 buf[RECORD_SIZE_USER];
  u8 rec_num, count = 0;
  u8 got;

  if (n == 0 || !out_rec) {
    return BBS_EBADARG;
  }

  /* Open "USR LOG" REL file */
  err = user_open_rel(device, "USR LOG", RECORD_SIZE_USER, &h);
  if (err != BBS_OK) {
    return err;
  }

  /* Sequential scan, skip deleted records until we reach the nth one */
  for (rec_num = 1; rec_num <= USERS_MAX; rec_num++) {
    memset(buf, 0, RECORD_SIZE_USER);
    err = rel_read(h, (void *)buf, RECORD_SIZE_USER, &got);
    if (err != BBS_OK || got < RECORD_READ_MIN) {
      break;
    }
    if (got < RECORD_SIZE_USER) {
      memset(buf + got, 0, RECORD_SIZE_USER - got);
    }
    user_unpack(out_rec, buf);
    if (user_is_live(out_rec)) {
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
 * user_delete()
 *
 * Soft-delete a user by clearing the handle.
 */
bbs_err_t user_delete(u8 user_id, u8 device) {
  bbs_err_t err;
  user_record_t rec;

  if (user_id == 0) {
    return BBS_EBADARG;
  }

  /* Load user record */
  err = user_by_id(user_id, &rec, device);
  if (err != BBS_OK) {
    return err;
  }

  /* Clear handle to mark as deleted */
  memset(rec.handle, ' ', sizeof(rec.handle));
  rec.handle[10] = 0;

  /* Write back */
  return user_save(&rec, device);
}

/**
 * user_reset_password()
 *
 * Reset a user's password (sysop operation).
 */
bbs_err_t user_reset_password(u8 user_id, const char *new_password, u8 device) {
  bbs_err_t err;
  user_record_t rec;
  char hash[4];

  if (user_id == 0 || !new_password) {
    return BBS_EBADARG;
  }

  /* Load user record */
  err = user_by_id(user_id, &rec, device);
  if (err != BBS_OK) {
    return err;
  }

  /* Hash new password */
  user_hash_password(user_id, new_password, hash);

  /* Update password field */
  memcpy(rec.password, hash, 4);

  /* Write back */
  return user_save(&rec, device);
}

/* ---- User Profile (USR PROF) ---- */

static void user_profile_pack(const user_profile_record_t *rec, u8 *buf) {
  memset(buf, 0, RECORD_SIZE_USER_PROFILE);
  buf[0] = rec->id;
  memcpy(buf + 1,  rec->email,     32);
  memcpy(buf + 33, rec->firstname, 16);
  memcpy(buf + 49, rec->lastname,  16);
  memcpy(buf + 65, rec->location,  21);
}

static void user_profile_unpack(user_profile_record_t *rec, const u8 *buf) {
  memset(rec, 0, sizeof(*rec));
  rec->id = buf[0];
  memcpy(rec->email,     buf + 1,  32);
  memcpy(rec->firstname, buf + 33, 16);
  memcpy(rec->lastname,  buf + 49, 16);
  memcpy(rec->location,  buf + 65, 21);
  /* Ensure null termination */
  rec->email[31]     = 0;
  rec->firstname[15] = 0;
  rec->lastname[15]  = 0;
  rec->location[20]  = 0;
}

bbs_err_t user_profile_save(const user_profile_record_t *rec, u8 device) {
  rel_handle_t h;
#ifdef T64_STORE_SEQ
#define err rel_scratch_err
#define buf rel_scratch_buf
#else
  bbs_err_t err;
  u8 buf[RECORD_SIZE_USER_PROFILE];
#endif

  if (!rec || rec->id == 0) return BBS_EBADARG;

  err = user_open_rel(device, "USR PROF", RECORD_SIZE_USER_PROFILE, &h);
  if (err != BBS_OK) return err;

  err = rel_position(h, rec->id);
  if (err != BBS_OK) { rel_close(h); return err; }

  user_profile_pack(rec, buf);
  err = rel_write(h, (const void *)buf, RECORD_SIZE_USER_PROFILE);
  err = rel_close_keep(h, err);
  return err;
}
#ifdef T64_STORE_SEQ
#undef err
#undef buf
#endif

bbs_err_t user_profile_by_id(u8 id, user_profile_record_t *out_rec, u8 device) {
  rel_handle_t h;
#ifdef T64_STORE_SEQ
#define err rel_scratch_err
#define got rel_scratch_got
#define buf rel_scratch_buf
#else
  bbs_err_t err;
  u8 buf[RECORD_SIZE_USER_PROFILE];
  u8 got;
#endif

  if (id == 0 || !out_rec) return BBS_EBADARG;

  err = user_open_rel(device, "USR PROF", RECORD_SIZE_USER_PROFILE, &h);
  if (err != BBS_OK) return err;

  err = rel_position(h, id);
  if (err != BBS_OK) { rel_close(h); return err; }

  memset(buf, 0, RECORD_SIZE_USER_PROFILE);
  err = rel_read(h, (void *)buf, RECORD_SIZE_USER_PROFILE, &got);
  rel_close(h);

  if (err != BBS_OK) return err;
  if (got < 1) return BBS_ENOTFOUND;

  user_profile_unpack(out_rec, buf);
  if (out_rec->id == 0) return BBS_ENOTFOUND;

  return BBS_OK;
}
#ifdef T64_STORE_SEQ
#undef err
#undef got
#undef buf
#endif
