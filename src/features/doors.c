/* Door runtime: SDK wrappers, DCB handshake, loader/validator. RESIDENT. */
#include "bbs/doors.h"
#include "bbs/door_abi.h"
#include "bbs/net.h"
#include "bbs/sysop.h"
#include "bbs/cfg.h"
#include "bbs/hal/disk.h"
#include "bbs/overlay.h"
#include <c64/kernalio.h>
#include <string.h>
#include <stdio.h>

#define DCB ((volatile u8 *)BBS_DCB_ADDR)
#define DOOR_ENTRY 0x9700u

/* Spike-confirmed (Task 0): a door is a separate native Oscar64 build that
 * clobbers the shared Oscar64 ZP runtime window ($02-$8F) while it runs, so
 * door_run saves/restores that range around the call. zp_buf MUST live outside
 * $02-$8F (the linker places this global well above ZP). The wrappers and the
 * entry helper MUST be #pragma native so the native door can call them and so
 * the save/restore compiles to a clean A/X-indexed copy that cannot
 * self-corrupt. Verify both in the build step. */
#define ZP_SAVE_LO  0x02u
#define ZP_SAVE_LEN 0x8Eu               /* $02..$8F inclusive = 142 bytes */
static u8 zp_buf[ZP_SAVE_LEN];

static session_t *g_door_sess;   /* active session for the wrappers */

static void w_print(const char *s)            { session_emit(g_door_sess, s); }
static void w_print_n(const char *b, u8 n)    { char t[81]; if(n>80)n=80; memcpy(t,b,n); t[n]=0; session_emit(g_door_sess, t); }
static void w_display(u8 cat, const char *nm) { session_display_file(g_door_sess, (char)cat, nm); }
static void w_clear(void)                     { session_clear_screen(g_door_sess); }
static u8   w_getkey(void)                    { u8 c; return sess_read_key(g_door_sess, &c) ? c : 0; }
static i8   w_read_line(char *b, u8 max)      {
  if (!sess_carrier_ok(g_door_sess)) return -1;          /* carrier absent at entry */
  sess_read_line(g_door_sess, b, max, FALSE);
  /* sess_read_line returns on a mid-read carrier drop leaving partial bytes in b;
   * surface that as 0 (empty) so the door treats it as "carrier lost, exit now". */
  if (!sess_carrier_ok(g_door_sess)) { b[0] = 0; return 0; }
  return (i8)strlen(b);
}
static void w_get_caller(bbs_caller_t *o) {
  memset(o, 0, sizeof(*o));
  strncpy(o->handle,    g_door_sess->handle,        sizeof(o->handle)    - 1);
  o->access_level = g_door_sess->user.access_level;
  strncpy(o->firstname, g_door_sess->reg_firstname, sizeof(o->firstname) - 1);
  strncpy(o->lastname,  g_door_sess->reg_lastname,  sizeof(o->lastname)  - 1);
  strncpy(o->location,  g_door_sess->reg_location,  sizeof(o->location)  - 1);
  o->term_width = g_door_sess->term_width;
  o->term_mode  = (u8)g_door_sess->term_mode;
}

static const bbs_api_t g_api = {
  BBS_ABI_VERSION, w_print, w_print_n, w_display, w_clear,
  w_getkey, w_read_line, w_get_caller
};

/* The native door calls these via the API pointers — force them native. */
#pragma native(w_print)
#pragma native(w_print_n)
#pragma native(w_display)
#pragma native(w_clear)
#pragma native(w_getkey)
#pragma native(w_read_line)
#pragma native(w_get_caller)

/* enter_door: save the Oscar64 ZP runtime window, JSR the door at $9700 (a
 * direct ((void(*)(void))0x9700)() cast CRASHES oscar64 — use inline asm),
 * then restore ZP so the suspended BBS resumes cleanly. Hand __asm X-indexed
 * copy uses only A and X (no ZP scratch) — avoids self-corruption that a C
 * loop would cause by clobbering its own bytecode runtime state mid-loop. */
static void enter_door(void) {
  __asm {
    ldx #0
  ed_save:
    lda $02,x
    sta zp_buf,x
    inx
    cpx #ZP_SAVE_LEN
    bne ed_save
  }
  __asm { jsr $9700 }
  __asm {
    ldx #0
  ed_restore:
    lda zp_buf,x
    sta $02,x
    inx
    cpx #ZP_SAVE_LEN
    bne ed_restore
  }
}
#pragma native(enter_door)

/* door_run must NOT be inlined: action_doors_menu lives in OVL_DOORS ($9700),
 * and krnio_load inside door_run overwrites that region.  If door_run were
 * inlined into action_doors_menu the load would clobber its own running code.
 * After enter_door() returns (door exited), we reload OVL_DOORS so that the
 * return address back into action_doors_menu is valid.  All early-exit paths
 * (load failure, ABI mismatch) also reload because the door file load may have
 * already partially overwritten the overlay. */
__noinline void door_run(session_t *s, const door_record_t *rec) {
  /* "0:<filename>" — rec->drive is a partition (persistent drive state,
   * selected below via disk_select_partition()), not a drive number; a
   * 1581 only exposes drive 0. Up to 16 filename chars + "0:" + NUL = 19
   * worst case; 24 leaves margin. */
  char name[24];
  const volatile u8 *hdr = (const volatile u8 *)DOOR_ENTRY;

  g_door_sess = s;
  DCB[BBS_DCB_MAGIC0] = BBS_DOOR_MAGIC0;
  DCB[BBS_DCB_MAGIC1] = BBS_DOOR_MAGIC1;
  DCB[BBS_DCB_VER]    = BBS_ABI_VERSION;
  DCB[BBS_DCB_PTR_LO] = (u8)((u16)&g_api & 0xFF);
  DCB[BBS_DCB_PTR_HI] = (u8)((u16)&g_api >> 8);

  /* A wrong-partition load is unrecoverable: it either fails silently (the
   * exact defect this fix addresses) or, worse, silently loads whatever is
   * on the currently-selected partition instead of the intended door. Bail
   * out through the same failure path as a genuine load failure rather than
   * attempting a load that cannot be trusted. */
  if (disk_select_partition(rec->device, rec->drive) != BBS_OK) {
    session_emit(s, "\r\nDOOR LOAD FAILED.\r\n");
    goto reload_ovl;
  }

  sprintf(name, "0:%s", rec->filename);
  krnio_setnam(name);
  if (!krnio_load(1, rec->device, 1)) {
    session_emit(s, "\r\nDOOR LOAD FAILED.\r\n");
    goto reload_ovl;
  }
  if (!door_abi_check(hdr[BBS_DOOR_HDR_MAGIC], hdr[BBS_DOOR_HDR_MAGIC+1],
                      hdr[BBS_DOOR_HDR_VER])) {
    session_emit(s, "\r\nDOOR ABI MISMATCH.\r\n");
    goto reload_ovl;
  }
  enter_door();                      /* save ZP, JSR $9700, restore ZP */

reload_ovl:
  /* Reload OVL_DOORS so action_doors_menu's code is back at its load address
   * before we return into it.  The door load (and the failed-load path above)
   * clobbered $9700-$BFFF.  Mark WFC displaced too — it needs reload later.
   *
   * Unlike the "skip the overlay call" recovery used elsewhere (e.g.
   * action_files/action_doors when their initial OVL_* load fails), this
   * reload has no safe skip: door_run's caller (action_doors_menu or
   * login_doors_iter) lives IN OVL_DOORS, and the C-level `return` below is
   * an implicit jump back into that region — there is no way to redirect
   * it. If the reload fails, $9700 holds whatever the door (or the failed
   * load itself) left there, and returning would execute it as if it were
   * doors_code. Halting with a legible message is the only safe option;
   * the alternative is the exact silent-garbage crash this whole fix
   * exists to close. */
  if (disk_load_overlay(P"OVL_DOORS") != BBS_OK) {
    session_emit(s, "\r\nERROR: OVL_DOORS RESTORE FAILED. HALTED.\r\n");
    for (;;) { }
  }
  wfc.ovl_wfc_loaded = FALSE;
}

/* ── OVL_DOORS overlay: door menu UI + login iterator ────────────────────────
 * Both action_doors_menu and login_doors_iter live in the doors overlay so
 * they don't consume resident space.  door_run (above) reloads OVL_DOORS
 * before returning here, so this code is valid at the return address despite
 * the door having overwritten $9700-$BFFF during its execution. */
#pragma code(doors_code)
#pragma data(doors_data)
#pragma bss(doors_bss)

/* doors_scan — ONE rel_open, sequentially read up to `max` door records into
 * out[], return the count read.  Lives in the overlay and calls the resident
 * primitives door_open_rel/door_unpack (already linked — door_count uses them),
 * so it adds no resident code and lets door_by_id/index/key DCE away.  A single
 * open (vs one per slot) is why login no longer chatters the drive for minutes,
 * and reading distinct records (never re-positioning into absent slots) is why a
 * one-door table no longer lists DOORS_MAX times. */
static u8 doors_scan(door_record_t *out, u8 max) {
  rel_handle_t h;
  u8 n = 0, got;
  u8 buf[RECORD_SIZE_DOOR];
  if (door_open_rel(bbs_cfg.device_doors, &h) != BBS_OK) return 0;
  rel_position(h, 1);
  while (n < max) {
    /* Zero before each read: CBM REL rel_read returns a short count, leaving
     * the tail bytes (min_level/login_order) unread — without this they'd hold
     * stack garbage (door_by_id/door_count memset for the same reason). */
    memset(buf, 0, RECORD_SIZE_DOOR);
    if (rel_read(h, (void *)buf, RECORD_SIZE_DOOR, &got) != BBS_OK || got < 8) break;
    door_unpack(&out[n], buf);
    n++;
  }
  rel_close(h);
  return n;
}

void action_doors_menu(session_t *s) {
  door_record_t arr[DOORS_MAX];   /* on the BBS stack ($C000+); survives door_run reloads */
  u8 n, i, key;

  n = doors_scan(arr, DOORS_MAX);

  session_clear_screen(s);
  /* m.doors — sysop-maintained door menu.  Fallback: dynamic list from DOORS REL. */
  if (session_display_file(s, 'm', "doors") == BBS_ENOTFOUND) {
    sess_reset_color(s);
    session_emit(s, "\r\nDOOR PROGRAMS\r\n");
    session_emit(s, "---------------------------------------\r\n\r\n");
    for (i = 0; i < n; i++) {
      if (!door_visible(&arr[i], s->user.access_level)) continue;
      { char line[40]; sprintf(line, " [%c] %s\r\n", arr[i].cmd_key, arr[i].title);
        session_emit(s, line); }
    }
  }

  /* p.doors prompt — fallback matches action_doors_menu's original inline prompt. */
  session_emit(s, "\r\n");
  if (session_display_file(s, 'p', "doors") == BBS_ENOTFOUND)
    session_emit(s, "SELECT (RETURN=BACK): ");

  { u8 c; if (!sess_read_key(s, &c)) return; key = c; }
  if (key == '\r' || key == '\n') { s->menu_displayed = FALSE; return; }
  if (key >= 'a' && key <= 'z') key = (u8)(key - 0x20);
  { const char e[2] = { (char)key, 0 }; session_emit(s, e); }
  for (i = 0; i < n; i++) {                                 /* select from memory — no extra open */
    if (door_visible(&arr[i], s->user.access_level) && arr[i].cmd_key == (char)key) {
      door_run(s, &arr[i]);
      break;
    }
  }
  s->menu_displayed = FALSE;
}

void login_doors_iter(session_t *s) {
  /* One scan, then run login-flagged visible doors in slot order.  The old
   * per-slot door_by_id(1..MAX) loop re-opened the DOORS file 16 times even
   * with no login doors (a ~2-minute login on a real-speed drive).  door_run
   * reloads OVL_DOORS before returning, so this overlay code and arr (on the
   * BBS stack) survive each call. */
  door_record_t arr[DOORS_MAX];
  u8 n, i;

  n = doors_scan(arr, DOORS_MAX);
  for (i = 0; i < n; i++) {
    if (!(arr[i].flags & DOOR_F_LOGIN)) continue;
    if (!door_visible(&arr[i], s->user.access_level)) continue;
    door_run(s, &arr[i]);
  }
}

#pragma code(code)
#pragma data(data)
#pragma bss(bss)

/* (resident) Load OVL_DOORS and run any doors flagged DOOR_F_LOGIN in
 * ascending login_order.  The iterator itself lives in OVL_DOORS (overlay)
 * so it survives the door_run reloads; this shim just bootstraps it.
 * With no login doors registered the iterator's flag/visibility checks are
 * all skipped, so login proceeds unchanged. */
void session_run_login_doors(session_t *s) {
  /* login_doors_iter lives IN the overlay just requested — skip it on a
   * failed load rather than run into whatever is still at $9700.  Safe to
   * just fall through to wfc_reload(): this shim is resident, so login
   * proceeds normally (minus any login-flagged doors this call). */
  if (disk_load_overlay(P"OVL_DOORS") != BBS_OK) {
    session_emit(s, "\r\nERROR: OVL_DOORS LOAD FAILED.\r\n");
  } else {
    login_doors_iter(s);
  }
  wfc.ovl_wfc_loaded = FALSE;  /* iterator + door loads displaced wfc */
  wfc_reload();                /* restore 40-col spy footer before the menu */
}
