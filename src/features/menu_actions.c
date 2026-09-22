/**
 * TURBO/64 BBS — Menu Action Handlers
 *
 * Implementation of menu command action handlers.
 * v1: Mostly stubs and simple info displays.
 * v1.1+: Will integrate with actual feature modules.
 */
#include "bbs/sysop.h"

#include <stdio.h>
#include "bbs/menu.h"
#include "bbs/menu_actions.h"
#include "bbs/session.h"
#include "bbs/bulletin.h"
#include "bbs/users.h"
#include "bbs/cfg.h"
#include "bbs/records.h"
#include "bbs/doors.h"
#include "bbs/files.h"
#include "bbs/hal/disk.h"

/**
 * MAIN Menu Actions
 */

void action_last_callers(session_t *s) {
  /* Displayed via WFC sysop screen; remote user view is future work. */
  if (!s) return;
  session_emit(s, "\r\nLAST CALLERS: SEE WFC SCREEN\r\n");
  s->menu_displayed = FALSE;
}

void action_user_list(session_t *s) {
  if (!s) return;
  session_emit(s, "\r\nUSERS ONLINE (STUB)\r\n");
  s->menu_displayed = FALSE;
}

void action_system_info(session_t *s) {
  char info_line[64];
  if (!s) return;
  session_emit(s, "\r\nSYSTEM INFO\r\n");
  sprintf(info_line, "  ACCESS LEVEL: %u\r\n", s->user.access_level);
  session_emit(s, info_line);
  sprintf(info_line, "  CALLS: %u\r\n", s->user.calls);
  session_emit(s, info_line);
  sprintf(info_line, "  CREDIT: %u\r\n", s->user.credit_balance);
  session_emit(s, info_line);
  s->menu_displayed = FALSE;
}

void action_help(session_t *s) {
  if (!s) return;
  session_emit(s, "\r\nHELP (STUB)\r\n");
  s->menu_displayed = FALSE;
}

/**
 * MSGS (Message Areas) Menu Actions
 */

/* action_list_boards — entry for all MSG menu actions.
 * Loads OVL_MSGS overlay from system disk then runs bulletin_run.  The overlay
 * occupies $9D80-$BFEF; core code at $0880-$9D7F stays resident for callbacks.
 * Reloading on each entry is safe (same bytes, same address) and fast on CMD HD.
 *
 * bulletin_run() lives IN the overlay just loaded — on a failed load it must
 * not be called (that would run whatever stale overlay is still at $9700).
 * Skipping it and returning here is always safe: this shim is resident, so
 * control returns to the resident menu dispatcher regardless of what sits
 * in the overlay region. */
void action_list_boards(session_t *s) {
  if (disk_load_overlay(P"OVL_MSGS") != BBS_OK) {
    session_emit(s, "\r\nERROR: OVL_MSGS LOAD FAILED.\r\n");
    s->menu_displayed = FALSE;
    return;
  }
  wfc.ovl_wfc_loaded = FALSE;
  bulletin_run(s);
  wfc_reload();  /* restore OVL_WFC (spy view code) before returning to menu */
  s->menu_displayed = FALSE;
}


/**
 * EMAIL Menu Actions
 */

void action_list_mail(session_t *s) {
  if (!s) return;
  session_emit(s, "\r\nLIST MAIL (STUB)\r\n");
  s->menu_displayed = FALSE;
}

void action_send_mail(session_t *s) {
  if (!s) return;
  session_emit(s, "\r\nSEND MAIL (STUB)\r\n");
  s->menu_displayed = FALSE;
}

/**
 * PREFS (Preferences) Menu Actions
 */

void action_term_mode(session_t *s) {
  if (!s) return;
  session_emit(s, "\r\nTERMINAL MODE\r\n");
  session_emit(s, "  CURRENT: ");
  switch (s->term_mode) {
    case TERM_PETSCII:
      session_emit(s, "PETSCII\r\n");
      break;
    case TERM_ANSI_CP437:
      session_emit(s, "ANSI/CP437\r\n");
      break;
    case TERM_ASCII:
      session_emit(s, "ASCII\r\n");
      break;
    default:
      session_emit(s, "UNKNOWN\r\n");
      break;
  }
  session_emit(s, "  (CHANGE NOT YET IMPLEMENTED)\r\n");
  s->menu_displayed = FALSE;
}

void action_term_width(session_t *s) {
  char info_line[32];
  if (!s) return;
  session_emit(s, "\r\nTERMINAL WIDTH\r\n");
  sprintf(info_line, "  CURRENT: %u COLUMNS\r\n", s->term_width);
  session_emit(s, info_line);
  session_emit(s, "  (CHANGE NOT YET IMPLEMENTED)\r\n");
  s->menu_displayed = FALSE;
}

void action_colors(session_t *s) {
  if (!s) return;
  session_emit(s, "\r\nCOLOR SETTINGS (STUB)\r\n");
  s->menu_displayed = FALSE;
}

void action_paging(session_t *s) {
  if (!s) return;
  session_emit(s, "\r\nPAGING SETTINGS (STUB)\r\n");
  s->menu_displayed = FALSE;
}

void action_clear_on_msg(session_t *s) {
  if (!s || s->user_id == 0) return;
  s->user.flags ^= USER_F_CLEAR_ON_MSG;
  user_save(&s->user, bbs_cfg.device_system);
  session_emit(s, "\r\nSCREEN CLEAR: ");
  session_emit(s, (s->user.flags & USER_F_CLEAR_ON_MSG) ? "ON\r\n" : "OFF\r\n");
  s->menu_displayed = FALSE;
}

/**
 * FILES Menu Actions
 */

/* action_files — resident shim: loads OVL_FILES, runs files_run(), reloads WFC.
 * Mirrors action_doors: the four old per-command stubs (list areas, download,
 * upload, search) are replaced by a single entry that hands control to the
 * OVL_FILES dispatch loop for the duration of the files session. */
void action_files(session_t *s) {
  if (disk_load_overlay(P"OVL_FILES") != BBS_OK) {
    session_emit(s, "\r\nERROR: OVL_FILES LOAD FAILED.\r\n");
    s->menu_displayed = FALSE;
    return;
  }
  wfc.ovl_wfc_loaded = FALSE;
  files_run(s);
  wfc_reload();
  s->menu_displayed = FALSE;
}

/**
 * DOOR Menu Actions
 */

/* action_doors — resident shim that loads OVL_DOORS then calls action_doors_menu.
 * Mirrors action_list_boards: load overlay, call overlay entry, reload WFC,
 * clear menu_displayed.  door_run (called inside action_doors_menu) reloads
 * OVL_DOORS after a door runs, so action_doors_menu's code is always valid
 * when it returns here. */
void action_doors(session_t *s) {
  if (disk_load_overlay(P"OVL_DOORS") != BBS_OK) {
    session_emit(s, "\r\nERROR: OVL_DOORS LOAD FAILED.\r\n");
    s->menu_displayed = FALSE;
    return;
  }
  wfc.ovl_wfc_loaded = FALSE;
  action_doors_menu(s);
  wfc_reload();
  s->menu_displayed = FALSE;
}

