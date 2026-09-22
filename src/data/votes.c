/**
 * TURBO/64 BBS — Vote Module (Implementation)
 *
 * Manages poll/vote records via REL files.
 */

#include "bbs/votes.h"
#include "bbs/config.h"
#include "bbs/cfg.h"
#include "bbs/rel.h"
#include <string.h>
#include <stdio.h>

#define RECORD_READ_MIN 12

static void vote_pack(const vote_record_t *rec, u8 *buf) {
  u8 i;
  memset(buf, 0, RECORD_SIZE_VOTE);
  buf[0] = rec->id;
  for (i = 0; i < 24; i++) {
    char c = rec->question[i];
    if (c == 0) c = ' ';
    buf[1 + i] = (u8)c;
  }
  buf[25] = rec->option_count;
  for (i = 0; i < 5; i++) {
    buf[26 + i] = rec->options[i];
  }
  buf[31] = rec->active;
}

static void vote_unpack(vote_record_t *rec, const u8 *buf) {
  u8 i;
  memset(rec, 0, sizeof(*rec));
  rec->id = buf[0];
  for (i = 0; i < 24; i++) {
    rec->question[i] = (char)buf[1 + i];
  }
  rec->option_count = buf[25];
  for (i = 0; i < 5; i++) {
    rec->options[i] = buf[26 + i];
  }
  rec->active = buf[31];
}

static u8 question_is_deleted(const char *question) {
  u8 i;
  for (i = 0; i < 24; i++) {
    if (question[i] != ' ' && question[i] != 0) {
      return FALSE;
    }
  }
  return TRUE;
}

static bbs_err_t vote_open_rel(u8 device, rel_handle_t *h)
{
  bbs_err_t err;

  err = cfg_send_drive_init(device, bbs_cfg.init_system);
  if (err != BBS_OK) {
    return err;
  }

  return rel_open(device, bbs_cfg.drive_system, "VOTE1", RECORD_SIZE_VOTE, h);
}

/**
 * vote_count()
 *
 * Count total non-deleted votes.
 */
u8 vote_count(u8 device) {
  bbs_err_t err;
  rel_handle_t h;
  vote_record_t rec;
  u8 buf[RECORD_SIZE_VOTE];
  u8 rec_num, count = 0;
  u8 got;

  /* Open "VOTE1" REL file */
  err = vote_open_rel(device, &h);
  if (err != BBS_OK) {
    return 0;
  }

  /* Sequential scan, counting non-deleted records */
  for (rec_num = 1; rec_num <= CFG_MAX_VOTES; rec_num++) {
    memset(buf, 0, RECORD_SIZE_VOTE);
    err = rel_read(h, (void *)buf, RECORD_SIZE_VOTE, &got);
    if (err != BBS_OK || got < RECORD_READ_MIN) {
      break;
    }
    if (got < RECORD_SIZE_VOTE) {
      memset(buf + got, 0, RECORD_SIZE_VOTE - got);
    }
    vote_unpack(&rec, buf);
    if (rec.id != 0 && !question_is_deleted(rec.question)) {
      count++;
    }
  }

  rel_close(h);
  return count;
}

/**
 * vote_by_index()
 *
 * Get the Nth non-deleted vote (1-based index).
 */
bbs_err_t vote_by_index(u8 n, vote_record_t *out_rec, u8 device) {
  bbs_err_t err;
  rel_handle_t h;
  u8 buf[RECORD_SIZE_VOTE];
  u8 rec_num, count = 0;
  u8 got;

  if (n == 0 || !out_rec) {
    return BBS_EBADARG;
  }

  /* Open "VOTE1" REL file */
  err = vote_open_rel(device, &h);
  if (err != BBS_OK) {
    return err;
  }

  /* Sequential scan, skip deleted records until we reach the nth one */
  for (rec_num = 1; rec_num <= CFG_MAX_VOTES; rec_num++) {
    memset(buf, 0, RECORD_SIZE_VOTE);
    err = rel_read(h, (void *)buf, RECORD_SIZE_VOTE, &got);
    if (err != BBS_OK || got < RECORD_READ_MIN) {
      break;
    }
    if (got < RECORD_SIZE_VOTE) {
      memset(buf + got, 0, RECORD_SIZE_VOTE - got);
    }
    vote_unpack(out_rec, buf);
    if (out_rec->id != 0 && !question_is_deleted(out_rec->question)) {
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
 * vote_by_id()
 *
 * Load a vote record by vote ID.
 */
bbs_err_t vote_by_id(u8 id, vote_record_t *out_rec, u8 device) {
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
  u8 buf[RECORD_SIZE_VOTE];
#endif

  if (id == 0 || id > CFG_MAX_VOTES) {
    return BBS_EBADARG;
  }

  if (!out_rec) {
    return BBS_EBADARG;
  }

  /* Open "VOTE1" REL file */
  err = vote_open_rel(device, &h);
  if (err != BBS_OK) {
    return err;
  }

  /* Position to vote record */
  err = rel_position(h, id);
  if (err != BBS_OK) {
    rel_close(h);
    return err;
  }

  /* Read record */
  memset(buf, 0, RECORD_SIZE_VOTE);
  err = rel_read(h, (void *)buf, RECORD_SIZE_VOTE, &got);
  rel_close(h);

  if (err != BBS_OK) {
    return err;
  }

  if (got < RECORD_READ_MIN) {
    return BBS_EIO;
  }

  vote_unpack(out_rec, buf);

  if (out_rec->id == 0 || question_is_deleted(out_rec->question)) {
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
 * vote_save()
 *
 * Write a vote record back to disk.
 */
bbs_err_t vote_save(const vote_record_t *rec, u8 device) {
  rel_handle_t h;
#ifdef T64_STORE_SEQ
#define err rel_scratch_err
#define buf rel_scratch_buf
#else
  bbs_err_t err;
  u8 buf[RECORD_SIZE_VOTE];
#endif

  if (!rec || rec->id == 0 || rec->id > CFG_MAX_VOTES) {
    return BBS_EBADARG;
  }

  /* Open "VOTE1" REL file */
  err = vote_open_rel(device, &h);
  if (err != BBS_OK) {
    return err;
  }

  /* Position to vote record */
  err = rel_position(h, rec->id);
  if (err != BBS_OK) {
    rel_close(h);
    return err;
  }

  /* Write record */
  vote_pack(rec, buf);
  err = rel_write(h, (const void *)buf, RECORD_SIZE_VOTE);
  err = rel_close_keep(h, err);

  return err;
}
#ifdef T64_STORE_SEQ
#undef err
#undef buf
#endif

/**
 * vote_create()
 *
 * Create a new poll/vote question.
 */
bbs_err_t vote_create(const char *question, u8 device, u8 *out_id) {
  bbs_err_t err;
  rel_handle_t h;
  vote_record_t rec;
  u8 buf[RECORD_SIZE_VOTE];
  u8 rec_num, highest_id = 0;
  u8 got;

  if (!question || !out_id) {
    return BBS_EBADARG;
  }

  /* Open "VOTE1" REL file */
  err = vote_open_rel(device, &h);
  if (err != BBS_OK) {
    return err;
  }

  /* Find highest vote ID to assign next ID */
  for (rec_num = 1; rec_num <= CFG_MAX_VOTES; rec_num++) {
    memset(buf, 0, RECORD_SIZE_VOTE);
    err = rel_read(h, (void *)buf, RECORD_SIZE_VOTE, &got);
    if (err != BBS_OK || got < RECORD_READ_MIN) {
      break;
    }
    if (got < RECORD_SIZE_VOTE) {
      memset(buf + got, 0, RECORD_SIZE_VOTE - got);
    }
    vote_unpack(&rec, buf);
    if (rec.id != 0 && rec.id > highest_id) {
      highest_id = rec.id;
    }
  }

  rel_close(h);

  /* Check if we can create a new vote */
  if (highest_id >= CFG_MAX_VOTES) {
    return BBS_EFULL;
  }

  /* Create new vote record */
  *out_id = highest_id + 1;
  memset(&rec, 0, sizeof(rec));
  rec.id = *out_id;
  strncpy(rec.question, question, 23);
  rec.question[23] = 0;
  rec.option_count = 2;  /* Default to yes/no */
  rec.active = 1;        /* Active by default */

  /* Save new vote */
  return vote_save(&rec, device);
}

/**
 * vote_delete()
 *
 * Soft-delete a vote by clearing the question.
 */
bbs_err_t vote_delete(u8 vote_id, u8 device) {
  bbs_err_t err;
  vote_record_t rec;

  if (vote_id == 0 || vote_id > CFG_MAX_VOTES) {
    return BBS_EBADARG;
  }

  /* Load vote record */
  err = vote_by_id(vote_id, &rec, device);
  if (err != BBS_OK) {
    return err;
  }

  /* Clear question to mark as deleted */
  memset(rec.question, ' ', sizeof(rec.question));

  /* Write back */
  return vote_save(&rec, device);
}
