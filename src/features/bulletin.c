/* TURBO/64 BBS — Bulletin board feature layer (C*BASE-style unified subs). */
#include "bbs/bulletin.h"
#include "bbs/boards.h"
#include "bbs/messages.h"
#include "bbs/usrptr.h"
#include "bbs/users.h"
#include "bbs/cfg.h"
#include "bbs/config.h"
#include "bbs/records.h"
#include "bbs/session.h"
#include "bbs/hal/reu.h"
#include "bbs/editor.h"
#include "bbs/chat.h"
#include "bbs/sysop.h"
#include <string.h>
#include <stdio.h>
#include "bbs/overlay.h"
#pragma code(msgs_code)
#pragma data(msgs_data)
#pragma bss(msgs_bss)

/* Module state lives in main bss — the paged-listing code growth left the
 * msgs overlay region too full to hold it (oscar64 error 3034 otherwise). */
#pragma bss(bss)
static session_t          *s_sess;
static board_dir_record_t  s_board;
static u8                  s_board_idx;
static u16                 s_cur_msg;
static struct { u16 author_id; u8 flags; char subj[31]; } s_cur_info;
static usr_ptr_record_t   *s_ptr;
static char                s_msg_body[48]; /* layout: [0-15]=to [17-47]=subj (compose scratchpad only) */
static char                s_cmd[5];
#pragma bss(msgs_bss)

/* One shared 39-char rule, resident in main data — bull_sep (overlay) and
 * bull_sep_named (main) previously each carried their own copy; the overlay
 * copy is dead weight in the full msgs region. */
#pragma data(data)
static const char s_dashes[] = "---------------------------------------";
#pragma data(msgs_data)

static void bull_tx(const char *s)   { session_emit(s_sess, s); }
static void bull_nl(void)            { session_emit(s_sess, "\n"); }
static void bull_line(const char *s) { session_emit(s_sess, s); bull_nl(); }
static void bc(u8 n) { sess_pipe_color(s_sess, n); }
static void bull_sep(void) {
  bc(3); sess_tx(s_dashes); bc(1); bull_nl();
}
/* Emit labeled header: green label, yellow for following data value. */
static void bull_hdr(const char *label) { bc(13); bull_tx(label); bc(7); }

/* bull_getkey — single-keypress input for the reading loop.
 * CR/LF fires immediately as an empty command (ENTER).  All other keys
 * fire on the first character.  'R' is special: digits may follow before
 * CR so that R# (jump to msg) still works; any other stray character after
 * 'R' is discarded and CR closes the command. */
static void bull_getkey(char *buf) {
  u8 ch; char e[2];
  buf[0] = '\0';
  for (;;) {
    if (!sess_getc(&ch)) {
      if (!sess_carrier_ok(s_sess)) return;
      continue;
    }
    break;
  }
  if (ch == 10 || ch == 13) { bull_nl(); return; }   /* ENTER → empty cmd */
  if (ch >= 'a' && ch <= 'z') ch = (u8)(ch - 32);
  e[0] = (char)ch; e[1] = '\0'; bull_tx(e);
  buf[0] = (char)ch; buf[1] = '\0';
  /* 'R' or a leading digit may carry a numeric suffix (R#, or board #); the
   * rest are immediate hotkeys.  Numeric commands collect digits until CR. */
  if (ch == 'R' || (ch >= '0' && ch <= '9')) {
    u8 len;
    for (len = 1; len < 4;) {
      if (!sess_getc(&ch)) {
        if (!sess_carrier_ok(s_sess)) break;
        continue;
      }
      if (ch == 13 || ch == 10) break;
      if (ch >= '0' && ch <= '9') {
        e[0] = (char)ch; e[1] = '\0'; bull_tx(e);
        buf[len++] = (char)ch; buf[len] = '\0';
      }
    }
  }
  bull_nl();
}

static u8 is_subop_or_sysop(void) {
  if (s_sess->user.access_level >= CFG_ACCESS_SYSOP) return TRUE;
  if (s_board.subop_id != 0 && s_sess->user_id == (u8)s_board.subop_id) return TRUE;
  return FALSE;
}

/* body_display_cb — called per line during message display.
 * Attribution ([HANDLE] SAID:) and quoted ("> ") lines render in dark grey;
 * the author's own text stays white. */
static void body_display_cb(const char *line, void *ctx) {
  (void)ctx;
  if (line[0] == '>' || line[0] == '[') {
    bc(11); bull_tx(line); bc(1); bull_nl();
  } else {
    bull_line(line);
  }
}

/* ── Message-header helpers (main region; run only while msgs overlay loaded) ── */
#pragma code(code)
#pragma data(data)
#pragma bss(bss)

/* Field label in label-color, ':' in dark grey, then switch to value-color. */
static void bull_field(const char *label, u8 label_color, u8 value_color) {
  bc(label_color); bull_tx(label);
  bc(11);          bull_tx(":");
  bc(value_color); bull_tx(" ");
}

/* Pad spaces from the current column until reaching target. */
static void bull_pad(u8 col, u8 target) { while (col < target) { sess_tx(" "); col++; } }

/* Top rule: reverse-video cyan board name + cyan dashed fill (39-col safe width). */
static void bull_sep_named(void) {
  char ttl[17]; u8 len, k;
  for (k = 0; k < 16; k++) ttl[k] = s_board.title[k] ? s_board.title[k] : ' ';
  for (len = 16; len != 0 && ttl[len - 1] == ' '; len--) {}
  ttl[len] = '\0';
  bc(3); bc(16); bull_tx(ttl); bc(17);   /* reverse-video name in cyan */
  bull_tx(s_dashes + len);                  /* fill the rest of the 39-col rule */
  bc(1); bull_nl();
}

/* Blank the line directly above the cursor — the just-used reading prompt —
 * leaving the cursor on its current row so a clean blank line sits between
 * messages.  Called after bull_getkey() (which advanced one row past the
 * prompt).  No-op for clear-screen users (moot) and terminals without cursor
 * addressing (plain ASCII). */
static void bull_blank_prev_line(const session_t *s) {
  if (s->user.flags & USER_F_CLEAR_ON_MSG) return;
  if (s->term_mode == TERM_PETSCII) {
    sess_tx("\x91"                                          /* cursor up      */
            "                                       "       /* 39 spaces      */
            "\r");                                          /* down to col 0  */
  } else if (s->term_mode == TERM_ANSI_CP437) {
    sess_tx("\x1b[A\x1b[2K\x1b[B");                          /* up, clear, down */
  }
}

#pragma code(msgs_code)
#pragma data(msgs_data)
#pragma bss(msgs_bss)

static void display_msg(const session_t *s, u16 msg_id) {
  msg_index_record_t rec;
  char buf[16];   /* 15-char handle or 7-char M# format */
  bbs_err_t err;
  u8 is_new;

  err = msg_index_get(s_board.id, msg_id, &rec, bbs_cfg.device_msgs);
  if (err != BBS_OK || rec.msg_id == 0) { bull_line("NOT FOUND."); return; }
  if (rec.flags & MSG_F_DELETED) { bull_line("DELETED."); return; }
  s_cur_msg = msg_id;
  s_cur_info.author_id = rec.author_id;
  s_cur_info.flags     = rec.flags;
  memcpy(s_cur_info.subj, rec.subj, 31);
  if (s->user.flags & USER_F_CLEAR_ON_MSG) session_clear_screen(s);

  is_new = (u8)(msg_id > s_ptr->hwm[s_board.id - 1]);
  bull_sep_named();

  /* ── FROM (lt-green/yellow) + msg # / NEW (cyan, right-justified; NEW reserved) ── */
  bull_field("FROM", 13, 7);
  if (!rec.author_id || (rec.flags & MSG_F_ANON)) {
    strcpy(buf, "ALL");
  } else {
    user_record_t uname;
    buf[0] = '?'; buf[1] = '\0';
    if (user_by_id(rec.author_id, &uname, bbs_cfg.device_system) == BBS_OK) {
      u8 j;
      memcpy(buf, uname.handle, 15); buf[15] = '\0';
      for (j = 14; j > 0 && (buf[j] == ' ' || buf[j] == '\0'); j--) buf[j] = '\0';
    }
  }
  bull_tx(buf);
  bull_pad((u8)(6 + strlen(buf)), 28);     /* "#: 0000" ends col 34, " NEW" 35-38 */
  bull_field("#", 3, 3);
  sprintf(buf, "%04u", (unsigned)msg_id); bull_tx(buf);
  if (is_new) { bc(11); bull_tx(" NEW"); }
  bull_nl();

  /* ── TO (lt-green/yellow) + DATE (cyan, right-justified to col 38) ── */
  bull_field("TO  ", 13, 7);
  if (!rec.to_id) {
    strcpy(buf, "ALL");
  } else {
    user_record_t uname;
    buf[0] = '?'; buf[1] = '\0';
    if (user_by_id(rec.to_id, &uname, bbs_cfg.device_system) == BBS_OK) {
      u8 j;
      memcpy(buf, uname.handle, 15); buf[15] = '\0';
      for (j = 14; j > 0 && (buf[j] == ' ' || buf[j] == '\0'); j--) buf[j] = '\0';
    }
  }
  bull_tx(buf);
  if (rec.month) {
    bull_pad((u8)(6 + strlen(buf)), 25);   /* "DATE: MM/DD/YY" ends col 38 */
    bull_field("DATE", 3, 3);
    buf[0]='0'+(u8)(rec.month>>4);   buf[1]='0'+(rec.month&0x0F);   buf[2]='/';
    buf[3]='0'+(u8)(rec.day>>4);     buf[4]='0'+(rec.day&0x0F);     buf[5]='/';
    buf[6]='0'+(u8)(rec.year_yy>>4); buf[7]='0'+(rec.year_yy&0x0F); buf[8]='\0';
    bull_tx(buf);
  }
  bull_nl();

  /* ── SUBJ (lt-green/yellow) ── */
  bull_field("SUBJ", 13, 7); bull_tx(rec.subj); bull_nl();

  bull_sep();

  /* ── Body: stream every line via callback ── */
  msg_body_each_line(s_board.id, &rec, body_display_cb, (void *)0,
                     bbs_cfg.device_msgs);

  bull_sep();
}

static void bull_list_boards(const session_t *s) {
  u8 i, total, hw; board_dir_record_t rec; char num[4];
  total = board_count(bbs_cfg.device_msgs);
  bull_sep(); bull_line("BOARDS"); bull_sep();
  for (i = 1; i <= total; i++) {
    if (board_by_index(i, &rec, bbs_cfg.device_msgs) != BBS_OK) continue;
    if (rec.read_level > s->user.access_level) continue;
    hw = (s_ptr->hwm[rec.id - 1] < rec.msg_count) ? 1 : 0;
    { char ttl[17]; u8 k;
      for (k = 0; k < 16; k++) ttl[k] = (rec.title[k] == 0) ? ' ' : rec.title[k];
      ttl[16] = '\0';
      sprintf(num, "%2u.", (unsigned)i); bull_tx(num); bull_tx(ttl); }
    if (rec.flags & BOARD_F_NET) bull_tx("[N]"); else bull_tx(" ");
    if (hw) bull_tx("*"); bull_nl();
  }
  bull_sep();
}

static u8 bull_load_board(u8 idx) {
  if (board_by_index(idx, &s_board, bbs_cfg.device_msgs) != BBS_OK) return 0;
  s_board_idx = idx;
  s_cur_msg = 0;
#ifndef T64_STORE_SEQ
  /* Under T64_STORE_SEQ this bank-0 pre-warm is redundant: msg_index_get()
   * lazily loads B<n>.IDX into REU (WINDOW region, bank 2) on first access
   * via rel_open(), which is the same disk read this would otherwise force
   * up front. */
  reu_index_load(s_board.id, bbs_cfg.device_msgs);
#endif
  return msg_count_new(s_board.id, s_ptr->hwm[s_board.id - 1], 0, bbs_cfg.device_msgs);
}

/* bull_shift_board — advance to the next/previous board the user can read.
 * Returns the new board's unread count, or 0xFF if every board is denied. */
static u8 bull_shift_board(const session_t *s, u8 forward, u8 total) {
  u8 idx = s_board_idx, tries; board_dir_record_t rec;
  for (tries = 0; tries < total; tries++) {
    idx = forward ? (idx >= total ? 1 : (u8)(idx + 1))
                  : (idx <= 1    ? total : (u8)(idx - 1));
    if (board_by_index(idx, &rec, bbs_cfg.device_msgs) == BBS_OK &&
        rec.read_level <= s->user.access_level) {
      return bull_load_board(idx);
    }
  }
  bull_line("ALL DENIED.");
  return 0xFF;
}

/* ── Quote helpers ──────────────────────────────────────────────────────────
 * These live in the always-resident main region (not the tight 'msgs' overlay
 * at 0x9700-0xC000) so their code and the large record frames below don't
 * consume overlay space.  They still read the msgs_bss state globals, which is
 * valid because quoting only runs while the msgs overlay is loaded. */
#pragma code(code)
#pragma data(data)
#pragma bss(bss)

static void body_quote_cb(const char *line, void *ctx) {
  u8 *plines = (u8 *)ctx;
  reu_compose_puts("> ");
  reu_compose_puts(line);
  reu_compose_putc('\n');
  bull_tx("> "); bull_line(line);   /* echo quoted line (grey set by caller) */
  (*plines)++;
}

/* bull_quote — append the current message as an attributed quote block.
 * Stored body carries no color; the terminal echo is dark grey (bc 11).
 * Returns line count appended (0 on error). */
static u8 bull_quote(void) {
  msg_index_record_t tmp_rec;
  char attrib[24];
  u8 n = 0;
  u8 lines;

  if (msg_index_get(s_board.id, s_cur_msg, &tmp_rec, bbs_cfg.device_msgs) != BBS_OK ||
      tmp_rec.msg_id == 0) return 0;

  /* Build the attribution line once, then store and echo it */
  attrib[n++] = '[';
  if (s_cur_info.author_id && !(s_cur_info.flags & MSG_F_ANON)) {
    user_record_t uname;
    if (user_by_id(s_cur_info.author_id, &uname, bbs_cfg.device_system) == BBS_OK) {
      u8 j;
      for (j = 0; j < 15 && uname.handle[j] && uname.handle[j] != ' '; j++)
        attrib[n++] = uname.handle[j];
    }
  } else {
    attrib[n++] = 'A'; attrib[n++] = 'N'; attrib[n++] = 'O'; attrib[n++] = 'N';
  }
  attrib[n++] = ']'; attrib[n++] = ' '; attrib[n++] = 'S';
  attrib[n++] = 'A'; attrib[n++] = 'I'; attrib[n++] = 'D'; attrib[n++] = ':';
  attrib[n] = '\0';

  bc(11);                                   /* dark grey for the quote block */
  reu_compose_puts(attrib); reu_compose_putc('\n');
  bull_tx(attrib); bull_nl();
  lines = 1;

  msg_body_each_line(s_board.id, &tmp_rec, body_quote_cb, &lines,
                     bbs_cfg.device_msgs);
  bc(1);                                    /* restore normal text color */
  return lines;
}

#pragma code(msgs_code)
#pragma data(msgs_data)
#pragma bss(msgs_bss)

static void bull_do_post(u16 parent) {
  u16 new_id;
  u8  j;
  u16 to_id_val = 0;
  char area[17];   /* board title for the AREA: header (display only) */

  if (parent > 0) {
    /* ── Reply: SUBJ from s_cur_info.subj (saved by display_msg); TO from author ── */
    strcpy(s_msg_body + 17, "RE: ");
    for (j = 0; j < 26 && s_cur_info.subj[j]; j++)
      s_msg_body[21 + j] = s_cur_info.subj[j];
    s_msg_body[21 + j] = '\0';
    to_id_val = (u16)s_cur_info.author_id;
    if (to_id_val) {
      user_record_t uname;
      if (user_by_id(to_id_val, &uname, bbs_cfg.device_system) == BBS_OK) {
        strncpy(s_msg_body, uname.handle, 15); s_msg_body[15] = '\0';
        for (j = 14; j > 0 && s_msg_body[j] == ' '; j--) s_msg_body[j] = '\0';
      } else { strcpy(s_msg_body, "ALL"); to_id_val = 0; }
    } else { strcpy(s_msg_body, "ALL"); }

  } else {
    /* ── New post ── */
    bull_tx("TO  : ");   sess_read_line(s_sess, s_msg_body,      15, TRUE);
    if (!s_msg_body[0]) strcpy(s_msg_body, "ALL");
    /* Resolve handle to user_id so TO is stored in index (0 = ALL) */
    if (strcmp(s_msg_body, "ALL") != 0) {
      u8 uid = user_by_handle(s_msg_body, bbs_cfg.device_system);
      if (uid) to_id_val = (u16)uid;
    }
    do {
      if (!sess_carrier_ok(s_sess)) return;
      bull_tx("SUBJ: "); sess_read_line(s_sess, s_msg_body + 17, 30, FALSE);
    } while (!s_msg_body[17]);
  }

  /* AREA */
  memcpy(area, s_board.title, 16); area[16] = '\0';
  for (j = 15; j > 0 && (area[j] == ' ' || area[j] == '\0'); j--)
    area[j] = '\0';

  /* ── Editor screen ── */
  session_clear_screen(s_sess);
  bull_sep();
  bull_hdr("TO  : "); bull_tx(s_msg_body);      bull_nl();
  bull_hdr("SUBJ: "); bull_tx(s_msg_body + 17); bull_nl();
  bull_hdr("AREA: "); bull_tx(area);            bull_nl();
  bull_sep();
  bc(15);
  if (parent > 0) bull_tx("/S SAVE, /A ABORT, /Q QUOTE");
  else            bull_tx("/S SAVE, /A ABORT");
  bc(1); bull_nl();
  bull_sep();

  /* Save SUBJ before editor loop (bull_quote re-reads body, not s_msg_body) */
  { char subj_sv[32]; u8 k;
    for (k = 0; k < 31 && s_msg_body[17 + k]; k++) subj_sv[k] = s_msg_body[17 + k];
    subj_sv[k] = '\0';

  reu_compose_init();   /* body = user-typed text only — no TO/SUBJ header lines */

  { editor_result_t res;
    for (;;) {
      res = editor_run(s_sess, (void *)0);
      if (res == EDITOR_QUOTE) {
        if (!(parent > 0 && bull_quote() > 0)) bull_line("NOTHING TO QUOTE.");
        continue;
      }
      break;
    }
    if (res != EDITOR_ABORT) {
      if (msg_post(s_board.id, parent, (u16)s_sess->user_id, to_id_val, FALSE,
                   bbs_cfg.device_msgs, &new_id, subj_sv, wfc.date) == BBS_OK) {
        char buf[24];
        sprintf(buf, parent ? "REPLY #%u POSTED." : "MSG #%u POSTED.", (unsigned)new_id);
        bull_line(buf); reu_compose_init();
        board_by_id(s_board.id, &s_board, bbs_cfg.device_msgs);
#ifndef T64_STORE_SEQ
        /* Redundant under T64_STORE_SEQ: msg_index_put() already wrote the
         * new record straight into the REU-resident WINDOW region. */
        reu_index_load(s_board.id, bbs_cfg.device_msgs);
#endif
      } else bull_line("FAILED.");
    } else bull_line("ABORTED.");
  } /* close { editor_result_t res } */
  } /* close { char subj_sv } */
}   /* bull_do_post */

#define BULL_PAGE_ROWS   3
#define BULL_MEMO_SLOTS  3

/* Listing page buffer + author memo in msgs_bss (overlay BSS, valid only
 * while OVL_MSGS is loaded).  bull_author7 is resident code but only ever
 * called from overlay context, so accessing these from it is safe. */
static msg_list_row_t s_list_rows[BULL_PAGE_ROWS];

/* Author-id -> 7-char padded handle memo for one listing pass.  A screenful
 * rarely has more than a handful of distinct authors; without the REU user
 * cache each miss costs a full USR LOG open. */
static struct { u16 id; char handle[8]; } s_author_memo[BULL_MEMO_SLOTS];
static u8 s_author_memo_n;

/* Resident code, not overlay: msgs_code is as tight as msgs_bss, and this
 * helper only calls resident users.c anyway. */
#pragma code(code)
static const char *bull_author7(u16 author_id) {
  u8 i;
  user_record_t u;
  for (i = 0; i < s_author_memo_n; i++)
    if (s_author_memo[i].id == author_id) return s_author_memo[i].handle;
  i = (s_author_memo_n < BULL_MEMO_SLOTS) ? s_author_memo_n
                                          : (u8)(BULL_MEMO_SLOTS - 1);  /* full: reuse last */
  memset(s_author_memo[i].handle, ' ', 7);
  s_author_memo[i].handle[7] = '\0';
  /* User ids are u8 today; the u16 field exists for future net import,
   * which must add a guard here before ids can exceed 255. */
  if (user_by_id((u8)author_id, &u, bbs_cfg.device_system) == BBS_OK) {
    u8 j;
    memcpy(s_author_memo[i].handle, u.handle, 7);
    for (j = 0; j < 7; j++)
      if (!s_author_memo[i].handle[j]) s_author_memo[i].handle[j] = ' ';
  }
  s_author_memo[i].id = author_id;
  if (s_author_memo_n < BULL_MEMO_SLOTS) s_author_memo_n++;
  return s_author_memo[i].handle;
}
#pragma code(msgs_code)

/* bull_list_messages — scan listing for the current board.
 * Format (39 col; col 40 left blank to avoid line-wrap):
 *   #    = right-justified msg id (4 chars)
 *   *    = yellow asterisk if unread by this user, space otherwise
 *   MM/DD= post date (5 chars)
 *   FROM = handle, 7 chars space-padded (longer handles truncated)
 *   SUBJ = subj preview (20 chars)
 * Layout: 4 + 1 + 5 + 1 + 7 + 1 + 20 = 39.
 * msg_index_page fetches one page per REL pass; bull_author7 memoizes handles. */
static void bull_list_messages(const session_t *s) {
  u16 next_id; u8 nrows, i, row; char numstr[8]; char d[6]; bool_t is_new;
  u8 pg = (s->term_rows > 6u) ? (u8)(s->term_rows - 4u) : 20u;
  if (!s_board.msg_high_id && !s_board.msg_count) { bull_line("NO MSGS."); return; }
  s_author_memo_n = 0;
  bull_sep(); bull_line("   # MM/DD FROM    SUBJ"); bull_sep();
  row = 3;
  next_id = 1;
  for (;;) {
    if (!sess_carrier_ok(s)) break;
    nrows = msg_index_page(s_board.id, next_id, BULL_PAGE_ROWS,
                           s_list_rows, bbs_cfg.device_msgs);
    if (nrows == 0) break;
    /* Advance by slot count, not stored id — a corrupt record with a low
     * msg_id must not regress next_id into an infinite loop. */
    next_id = (u16)(next_id + nrows);
    for (i = 0; i < nrows; i++) {
      const msg_list_row_t *r = &s_list_rows[i];
      if (r->flags & MSG_F_DELETED) continue;
      is_new = (bool_t)(r->msg_id > s_ptr->hwm[s_board.id - 1]);
      /* msg# right-justified */
      sprintf(numstr, "%4u", (unsigned)r->msg_id);
      bull_tx(numstr);
      /* NEW indicator: yellow asterisk or space */
      if (is_new) { bc(7); bull_tx("*"); bc(1); } else bull_tx(" ");
      /* Date MM/DD — BCD-encoded nibbles, same as display_msg */
      if (r->month) {
        d[0]='0'+(u8)(r->month>>4); d[1]='0'+(r->month&0x0F); d[2]='/';
        d[3]='0'+(u8)(r->day>>4);   d[4]='0'+(r->day&0x0F);   d[5]='\0';
      } else { d[0]='-'; d[1]='-'; d[2]='/'; d[3]='-'; d[4]='-'; d[5]='\0'; }
      bull_tx(d); bull_tx(" ");
      if (!r->author_id || (r->flags & MSG_F_ANON))
        bull_tx("ANON   ");
      else
        bull_tx(bull_author7(r->author_id));
      bull_tx(" ");
      bull_line(r->subj);
      /* Paginate */
      if (++row >= pg) {
        u8 ch = 0;
        bull_tx("-- MORE --");
        while (!sess_getc(&ch)) {
          if (!sess_carrier_ok(s)) { bull_nl(); bull_sep(); return; }
        }
        bull_nl();
        if (ch == 'Q' || ch == 'q' || ch == 3 || ch == 27) goto done;
        row = 0;
      }
    }
    if (nrows < BULL_PAGE_ROWS) break;   /* short page = end of index */
  }
done:
  bull_sep();
}

/* bull_reading_loop — entered after display_msg(); handles per-message commands.
 * Returns when user types Q (back to boards) or no-carrier, or after auto-advance
 * hits the end of available messages. */
static void bull_reading_loop(const session_t *s) {
  for (;;) {
    if (!sess_carrier_ok(s)) break;

    /* Reading prompt: board name + message number. */
    session_set_mci_board(s_board.title);
    session_set_mci_msg(s_cur_msg);
    if (session_display_file(s, 'p', "read") == BBS_ENOTFOUND) {
      char fb[20]; sprintf(fb, "MSG#%u CMD?: ", (unsigned)s_cur_msg); bull_tx(fb);
    }

    bull_getkey(s_cmd);

    /* ENTER: advance to next message; auto-exit when exhausted */
    if (s_cmd[0] == '\0') {
      u16 target = (u16)(s_cur_msg + 1); msg_index_record_t chk;
      if ((s_board.msg_high_id > 0 && target > s_board.msg_high_id) ||
          msg_index_get(s_board.id, target, &chk, bbs_cfg.device_msgs) != BBS_OK ||
          chk.msg_id == 0) {
        bull_line("NO MORE."); break;
      }
      bull_blank_prev_line(s);
      display_msg(s, target);
      if (!(chk.flags & MSG_F_DELETED)) usrptr_advance(s_ptr, s_board.id, s_cur_msg);
      continue; }

    /* N: next new; stay in reading if none */
    if (strcmp(s_cmd, "N") == 0) {
      u16 next = msg_next_unread_any(s_board.id, s_ptr->hwm[s_board.id - 1], 0, bbs_cfg.device_msgs);
      if (next == 0) { bull_line("NO NEW."); continue; }
      bull_blank_prev_line(s);
      display_msg(s, next); usrptr_advance(s_ptr, s_board.id, s_cur_msg);
      continue; }

    /* R or R#: re-read current or jump to specific */
    if (s_cmd[0] == 'R') {
      if (s_cmd[1] >= '0' && s_cmd[1] <= '9') {
        u16 target = 0; u8 j;
        for (j = 1; s_cmd[j] >= '0' && s_cmd[j] <= '9'; j++)
          target = (u16)(target * 10 + (s_cmd[j] - '0'));
        if (target > 0) {
          u16 prev = s_cur_msg; bull_blank_prev_line(s); display_msg(s, target);
          if (s_cur_msg != prev) usrptr_advance(s_ptr, s_board.id, s_cur_msg);
        }
      } else { bull_blank_prev_line(s); display_msg(s, s_cur_msg); }
      continue; }

    /* *: reply to current */
    if (strcmp(s_cmd, "*") == 0) {
      if (!s_cur_msg) { bull_line("NO MSG."); continue; }
      if (s_board.write_level > s->user.access_level) { bull_line("DENIED."); continue; }
      bull_do_post(s_cur_msg); continue; }

    /* K: kill current; exit reading on success (cur_msg now invalid) */
    if (strcmp(s_cmd, "K") == 0) {
      if (!s_cur_msg) { bull_line("NO MSG."); continue; }
      if (s_cur_info.author_id != (u16)s->user_id && !is_subop_or_sysop()) { bull_line("DENIED."); continue; }
      if (msg_delete(s_board.id, s_cur_msg, bbs_cfg.device_msgs) == BBS_OK) {
        bull_line("KILLED."); s_cur_msg = 0; break;
      } else bull_line("FAILED.");
      continue; }

    /* ?: re-display reading menu */
    if (strcmp(s_cmd, "?") == 0) {
      session_clear_screen(s);
      if (session_display_file(s, 'm', "read") == BBS_ENOTFOUND) {
        sess_tx("---------------------------------------\r\n"
                " ENTER/N  NEXT/NEXT NEW\r\n"
                " R[#]     READ MSG #\r\n"
                " *        REPLY\r\n"
                " K        KILL MSG\r\n"
                " Q        BACK TO BOARDS\r\n"
                "---------------------------------------\r\n");
      }
      continue; }

    /* Q: back to boards */
    if (strcmp(s_cmd, "Q") == 0) break;

    bull_line("?");
  }
}

bbs_err_t bulletin_run(session_t *s) {
  usr_ptr_record_t ptr;
  u8 total;
  s_sess = s; s_ptr = &ptr;
  s->menu_skip_pause = TRUE;   /* full-screen feature: return straight to menu, no pause */
  usrptr_load((u16)s->user_id, s_ptr, bbs_cfg.device_msgs);
  total = board_count(bbs_cfg.device_msgs);
  if (total > 0) {
    /* Enter board 1, then display boards menu. */
    bull_load_board(1);
    session_clear_screen(s);
    if (session_display_file(s, 'm', "msgs") == BBS_ENOTFOUND) {
      sess_reset_color(s);
      session_emit(s, "MESSAGE AREAS\r\n");
      session_emit(s, "======================================\r\n\r\n");
    }
    for (;;) {
      if (!sess_carrier_ok(s)) break;

      /* Boards prompt — p.msgs with %BN for current board name. */
      session_set_mci_board(s_board.title);
      if (session_display_file(s, 'p', "msgs") == BBS_ENOTFOUND) {
        { char t[17]; u8 j;
          for (j = 0; j < 16; j++) t[j] = s_board.title[j] ? s_board.title[j] : ' ';
          t[16] = '\0';
          for (j = 16; j > 0 && t[j-1] == ' '; j--) t[j-1] = '\0';
          bc(3); bull_tx("BOARD: "); bc(1); bull_line(t); }
        bull_tx("CMD?: ");
      }

      bull_getkey(s_cmd);   /* hotkeys; 'R'/digit commands collect a number until ENTER */
      total = board_count(bbs_cfg.device_msgs);

      /* ENTER: read from first available (non-deleted) message in board */
      if (s_cmd[0] == '\0') {
        u16 first = msg_next_unread_any(s_board.id, 0, 0, bbs_cfg.device_msgs);
        if (first == 0) { bull_line("NO MSGS."); continue; }
        display_msg(s, first); usrptr_advance(s_ptr, s_board.id, s_cur_msg);
        bull_reading_loop(s); continue; }

      /* N: read from next unread (NEW) message for this user */
      if (strcmp(s_cmd, "N") == 0) {
        u16 next = msg_next_unread_any(s_board.id, s_ptr->hwm[s_board.id - 1], 0, bbs_cfg.device_msgs);
        if (next == 0) { bull_line("NO NEW."); continue; }
        display_msg(s, next); usrptr_advance(s_ptr, s_board.id, s_cur_msg);
        bull_reading_loop(s); continue; }

      /* R or R#: read specific or first new; enter reading loop */
      if (s_cmd[0] == 'R') {
        u16 target = 0;
        if (s_cmd[1] >= '0' && s_cmd[1] <= '9') {
          u8 j;
          for (j = 1; s_cmd[j] >= '0' && s_cmd[j] <= '9'; j++)
            target = (u16)(target * 10 + (s_cmd[j] - '0'));
        }
        if (target == 0) {
          target = msg_next_unread_any(s_board.id, s_ptr->hwm[s_board.id - 1], 0, bbs_cfg.device_msgs);
          if (target == 0) { bull_line("NO NEW."); continue; }
        }
        { u16 prev = s_cur_msg; display_msg(s, target);
          if (s_cur_msg != prev) usrptr_advance(s_ptr, s_board.id, s_cur_msg); }
        if (s_cur_msg > 0) bull_reading_loop(s);
        continue; }

      /* P / W: post new message to current board */
      if (strcmp(s_cmd, "P") == 0 || strcmp(s_cmd, "W") == 0) {
        if (s_board.write_level > s->user.access_level) { bull_line("DENIED."); continue; }
        bull_do_post(0); continue; }

      /* Y: quick user status */
      if (strcmp(s_cmd, "Y") == 0) {
        char buf[32];
        sprintf(buf, "USER: %s", s->handle); bull_line(buf);
        sprintf(buf, "LEVEL: %u  CALLS: %u", (unsigned)s->user.access_level, (unsigned)s->user.calls);
        bull_line(buf); continue; }

      /* S: scan — list message headers for current board */
      if (strcmp(s_cmd, "S") == 0) {
        bull_list_messages(s); continue; }

      /* L / V: list boards */
      if (strcmp(s_cmd, "L") == 0 || strcmp(s_cmd, "V") == 0) {
        bull_list_boards(s); continue; }

      /* + / >: next board */
      if (strcmp(s_cmd, "+") == 0 || strcmp(s_cmd, ">") == 0) {
        { u8 nc = bull_shift_board(s, TRUE, total);
          if (nc != 0xFF && nc > 0) { char b[8]; sprintf(b, "%u NEW", (unsigned)nc); bull_line(b); }
        }
        continue; }

      /* - / <: previous board */
      if (strcmp(s_cmd, "-") == 0 || strcmp(s_cmd, "<") == 0) {
        { u8 nc = bull_shift_board(s, FALSE, total);
          if (nc != 0xFF && nc > 0) { char b[8]; sprintf(b, "%u NEW", (unsigned)nc); bull_line(b); }
        }
        continue; }

      /* ?: re-display boards menu */
      if (strcmp(s_cmd, "?") == 0) {
        session_clear_screen(s);
        if (session_display_file(s, 'm', "msgs") == BBS_ENOTFOUND) {
          sess_tx("---------------------------------------\r\n"
                  "R[#]/N/ENTER  READ MSGS\r\n"
                  "S             SCAN MSG HEADERS\r\n"
                  "P             POST MSG\r\n"
                  "+/>  -/<  #   BOARD NAV\r\n"
                  "L             LIST BOARDS\r\n"
                  "Q             QUIT TO MAIN\r\n"
                  "---------------------------------------\r\n");
        }
        continue; }

      /* Q: quit to main */
      if (strcmp(s_cmd, "Q") == 0) break;

      /* #: change board by number */
      if (s_cmd[0] >= '1' && s_cmd[0] <= '9') {
        u8 idx = 0, j;
        for (j = 0; s_cmd[j] >= '0' && s_cmd[j] <= '9'; j++)
          idx = (u8)(idx * 10 + (s_cmd[j] - '0'));
        if (idx >= 1 && idx <= total) {
          u8 nc = bull_load_board(idx);
          if (nc > 0) { char b[8]; sprintf(b, "%u NEW", (unsigned)nc); bull_line(b); }
        } else bull_line("INVALID.");
        continue; }

      bull_line("?");
    }
  } else bull_line("NO BOARDS.");
  usrptr_save((u16)s->user_id, s_ptr, bbs_cfg.device_msgs);
  return BBS_OK;
}
#pragma code(code)
#pragma data(data)
#pragma bss(bss)
