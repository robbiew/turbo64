/**
 * TURBO/64 BBS — Session State Machine (Implementation)
 *
 * Single-line session management: dial-in → login → menu → logoff.
 */

#include "bbs/session.h"
#include "bbs/menu.h"
#include "bbs/net.h"
#include "bbs/term.h"
#include "bbs/cfg.h"
#include "bbs/users.h"
#include "bbs/auth.h"
#include "bbs/rel.h"
#include "bbs/io.h"
#include "bbs/hal/disk.h"
#include "bbs/hal/clock.h"
#include "bbs/newuser.h"
#include "bbs/spy80.h"
#include "bbs/sysop.h"
#include "bbs/doors.h"
#include "bbs/prompt_cursor.h"
#include "bbs/usrday.h"
#include "bbs/access.h"
#include <string.h>
#include <stdio.h>
#include <conio.h>   /* getchx() — raw GETIN, safe while VIC is in bitmap mode */

/* C64 keyboard buffer count (non-blocking check) */
#define KBD_COUNT (*(volatile u8 *)0xC6)

/* Direct screen and color RAM access — bypasses KERNAL */
#define SCRN ((volatile u8 *)0x0400u)
#define COLR ((volatile u8 *)0xD800u)

/* Set to TRUE when running a local sysop session (no modem) */
static bool_t s_local = FALSE;
static bool_t s_rx_last_was_cr = FALSE;

static bool_t sess_accept_input(u8 ch, u8 *out)
{
    if (s_rx_last_was_cr) {
        s_rx_last_was_cr = FALSE;
        if (ch == '\n') return FALSE;
    }
    if (ch == '\r') s_rx_last_was_cr = TRUE;
    *out = ch;
    return TRUE;
}

/* Active session pointer — allows sess_tx to feed the spy terminal */
static session_t *s_active = NULL;

/* Spy status-line timing: start-of-call TOD + last second drawn, so the
 * resident session_spy_poll() can tick the status line from any context
 * (main loop or a feature's nested loop) without needing elapsed passed in. */
static clock_tod_t s_spy_start;
static u8          s_spy_last_sec = 255;

/* Keyboard idle watchdog: TOD of the last caller keystroke. If a remote
 * caller sends nothing for bbs_cfg.idle_timeout_mins, we drop carrier.
 * s_idle_fired guards against re-sending the drop message while the
 * NET_DROPPING state settles. Reset per session in session_init(). */
static clock_tod_t s_idle_mark;
static bool_t      s_idle_fired = FALSE;

/* MINS/DAY enforcement state (per session). Armed by session_time_begin();
 * checked once/second from sess_getc() via session_time_check(). */
static bool_t      s_online_begun  = FALSE;
static bool_t      s_time_active   = FALSE;
static bool_t      s_time_warned   = FALSE;
static u8          s_time_last_sec = 255;
static u16         s_day_mins_used;
static u16         s_day_mins_limit;
static clock_tod_t s_session_start;

/* MCI substitution context — set by callers before session_display_file().
 * %BN → s_mci_board    (board name, trimmed)
 * %MN → s_mci_msgnum   (current message number as decimal string)
 * %TL → s_mci_timeleft (MINS/DAY time left, e.g. "10m"; "unlim" if exempt) —
 *       refreshed automatically at the top of session_display_file().
 * Cleared to empty string at module load (BSS zero-init). */
static char s_mci_board[17];
static char s_mci_msgnum[7];   /* u16 max = 65535 → 5 chars + NUL */
static char s_mci_timeleft[8]; /* "65535m" = 6 / "unlim" = 5, + NUL */

/**
 * session_set_mci_board()
 * Set the board-name MCI substitution string for %BN in display files.
 * title16 is the 16-byte space-padded title from board_dir_record_t.
 */
void session_set_mci_board(const char *title16)
{
    u8 j;
    if (!title16) { s_mci_board[0] = '\0'; return; }
    for (j = 0; j < 16; j++)
        s_mci_board[j] = title16[j] ? title16[j] : ' ';
    s_mci_board[16] = '\0';
    for (j = 16; j > 0 && s_mci_board[j - 1] == ' '; j--)
        s_mci_board[j - 1] = '\0';
}

/**
 * session_set_mci_msg()
 * Set the message-number MCI substitution string for %MN in display files.
 */
void session_set_mci_msg(u16 msg_num)
{
    sprintf(s_mci_msgnum, "%u", (unsigned)msg_num);
}

/* session_refresh_timeleft()
 * Recompute the %TL string from the live MINS/DAY state. Mirrors
 * session_time_check()'s remaining-minutes math. Exempt sessions (local sysop
 * or NO_TIME_LIMIT level, i.e. !s_time_active) render "unlim". */
static void session_refresh_timeleft(const session_t *s)
{
    clock_tod_t now;
    u16 used, rem;
    if (!s_time_active || !s || s->is_local) {
        strcpy(s_mci_timeleft, "unlim");
        return;
    }
    clock_read(&now);
    used = s_day_mins_used + (u16)(clock_elapsed(&s_session_start, &now) / 60u);
    rem  = (used < s_day_mins_limit) ? (u16)(s_day_mins_limit - used) : 0u;
    sprintf(s_mci_timeleft, "%um", (unsigned)rem);
}

/* PETSCII spy virtual terminal state */
static u8 s_vcol  = 0;   /* cursor column (0–39) */
static u8 s_vrow  = 0;   /* cursor row (0–19) */
static u8 s_color = 1;   /* current VIC color index (1=white) */
static bool_t s_reverse = FALSE; /* reverse-video active */
static bool_t s_screen_lower = FALSE;  /* local screen charset: TRUE = text/lowercase */
/* Caller's CURRENT logical charset, tracked from 0x0E/0x8E in the stream. Used
 * only to interpret incoming bytes (text-mode uppercase A-Z arrive as 0xC1-0xDA);
 * the spy's hardware charset stays graphics regardless (s_screen_lower). */
static bool_t s_caller_lower = FALSE;

/* Raw transmit helper — sends text to modem (or local screen), and feeds spy. */
void sess_tx(const char *text)
{
    if (s_local) {
        printf("%s", text);
    } else {
        u16 sent;
        net_tx(text, (u16)strlen(text), &sent);
        if (s_active) {
            u16 i;
            for (i = 0; text[i]; i++) session_spy_feed((u8)text[i]);
        }
    }
}

/* Send raw bytes without translation — used for native PETSCII gfiles. */
static void session_emit_raw(const session_t *s, const u8 *buf, u16 len)
{
    if (!s || !buf || len == 0) return;
    if (s->is_local) {
        u16 i;
        for (i = 0; i < len; i++) {
            u8 b = buf[i];
            if (b == 0x0Eu) s_screen_lower = TRUE;
            else if (b == 0x8Eu) s_screen_lower = FALSE;
            putchar(b);
        }
    } else {
        u16 sent;
        net_tx((const char *)buf, len, &sent);
        if (s_active) {
            u16 i;
            for (i = 0; i < len; i++) session_spy_feed(buf[i]);
        }
    }
}

void session_emit_charset(const session_t *s, u8 code)
{
    session_emit_raw(s, &code, 1);
}

/* Flip the LOCAL C64 screen charset via the KERNAL and record the state.  The
 * spy writes screen codes directly, so this only changes which glyphs show. */
static void screen_set_charset(bool_t lower)
{
    /* 0x8E is a valid PETSCII byte; host signed-char makes it look negative. */
    // cppcheck-suppress invalidFunctionArg
    putchar(lower ? (char)0x0E : (char)0x8E);
    s_screen_lower = lower;
}

bool_t session_screen_lower(void) { return s_screen_lower; }

/* Erase one character: BS+space+BS for ANSI/ASCII; cursor-left+space+cursor-left for PETSCII. */
void sess_erase_char(const session_t *s)
{
    if (s->term_mode == TERM_PETSCII) {
        sess_tx("\x9d \x9d");  /* PETSCII cursor-left, space, cursor-left */
    } else {
        sess_tx("\x08 \x08");  /* BS, space, BS */
    }
}

/* Emit a color/attribute: PETSCII byte for PETSCII mode, ANSI escape for ANSI mode.
 * No-op for ASCII. Common PETSCII codes: 0x9f=cyan 0x05=white 0x9a=lt.blue
 * 0x9e=yellow 0x12=rvrs-on 0x92=rvrs-off */
void sess_color(const session_t *s, u8 petscii_code, const char *ansi_code)
{
    if (s->term_mode == TERM_PETSCII) {
        char b[2]; b[0] = (char)petscii_code; b[1] = 0;
        sess_tx(b);
    } else if (s->term_mode == TERM_ANSI_CP437) {
        sess_tx(ansi_code);
    }
}

void sess_reset_color(const session_t *s)
{
    /* PETSCII: reverse-off (0x92) + white (0x05). ANSI: reset + white fg + black bg. */
    if (s->term_mode == TERM_PETSCII) {
        sess_tx("\x92\x05");
    } else if (s->term_mode == TERM_ANSI_CP437) {
        sess_tx("\x1b[0;37;40m");
    }
}

/* PETSCII byte → CBM screen code for the uppercase/graphics charset. */
static u8 petscii_to_sc(u8 ch)
{
    if (ch >= 0x40u && ch <= 0x5Fu) return (u8)(ch - 0x40u);
    if (ch >= 0x20u && ch <= 0x3Fu) return ch;
    if (ch >= 0xA0u && ch <= 0xBFu) return (u8)(ch - 0x40u);
    if (ch >= 0xC0u && ch <= 0xFEu) return (u8)(ch - 0x80u);
    if (ch == 0xFFu) return 0x5Eu;
    if (ch >= 0x60u && ch <= 0x7Fu) return (u8)(ch - 0x20u);
    return 0x20u;
}

/* Scroll spy zone (rows 0–19) up one line — linear indexing, no multiply. */
static __noinline void spy_scroll(void)
{
    u16 i;
    for (i = 0u; i < 760u; i++) {
        SCRN[i] = SCRN[i + 40u];
        COLR[i] = COLR[i + 40u];
    }
    for (i = 760u; i < 800u; i++) {
        SCRN[i] = 0x20u;
        COLR[i] = 0x01u;
    }
}

/* Clear spy zone rows 0–19 to spaces/white — shared by CLR and spy_init. */
static __noinline void spy_clear(void)
{
    u16 i;
    for (i = 0u; i < 800u; i++) { SCRN[i] = 0x20u; COLR[i] = 0x01u; }
}

/* Clear rows 20-24 in place without scrolling. */
static __noinline void spy_clear_footer(void)
{
    u16 i;
    for (i = 800u; i < 1000u; i++) { SCRN[i] = 0x20u; COLR[i] = 0x01u; }
}

/* Advance spy cursor row; scroll if at bottom — shared by CR, cursor-down, char-write. */
static __noinline void spy_advance(void)
{
    if (s_vrow < 19u) s_vrow++; else spy_scroll();
}

/**
 * session_spy_init()
 * Prepare spy terminal for a new session.
 * Selects the 80-col bitmap path for any 80-col, ASCII-byte-stream terminal
 * (ANSI/CP437, ASCII) when REU is present; otherwise the 40-col PETSCII path.
 * PETSCII (40-col, non-ASCII byte stream) never uses it.
 */
void session_spy_init(session_t *s)
{
    if (!s) return;

    s->screen_capture.wfc_show_user_view = TRUE;
    s_active = s;

    /* Mark call start for the resident status-line clock. */
    clock_read(&s_spy_start);
    s_spy_last_sec = 255;

    /* Arm the keyboard idle watchdog from the moment of connect. */
    s_idle_mark = s_spy_start;
    s_idle_fired = FALSE;

    if ((s->term_mode == TERM_ANSI_CP437 || s->term_mode == TERM_ASCII)
            && bbs_cfg.reu_enabled) {
        s->screen_capture.spy_mode = SPY_MODE_80COL;
        spy80_init();
        return;
    }

    s->screen_capture.spy_mode = SPY_MODE_PETSCII;
    s_vcol    = 0;
    s_vrow    = 0;
    s_color   = 1;
    s_reverse = FALSE;
    spy_clear();
    /* The spy local screen always stays in uppercase/graphics charset: PETSCII
     * gfiles/menus are graphics-charset art (uppercase) and the footer chrome is
     * authored uppercase. The caller's own text/lowercase charset (petscii_lower)
     * is handled on the wire, not mirrored here — see session_spy_feed(). */
    screen_set_charset(FALSE);
    s_caller_lower = s->petscii_lower;  /* caller's starting logical charset */
}

/**
 * session_spy_feed()
 * Interpret one PETSCII byte and write to screen RAM (rows 0–19 only).
 * Handles: CLR, HOME, cursor movement, all 16 colors, RVS ON/OFF, DEL, INST, printable chars.
 */
void session_spy_feed(u8 ch)
{
    u16 addr;

    if (!s_active || !s_active->screen_capture.wfc_show_user_view) return;

    if (s_active->screen_capture.spy_mode == SPY_MODE_80COL) {
        /* spy80 render path lives in MAIN code — always resident, so the spy
         * keeps rendering even while the MSGS overlay is loaded. */
        spy80_feed(ch);
        return;
    }

    /* Track the caller's charset switches but DON'T flip the spy's hardware
     * charset: the spy stays uppercase/graphics so menus (graphics-charset art)
     * and footer chrome render uppercase and never flip case. We track the
     * logical mode only to interpret incoming letters (see the 0xC1-0xDA fold
     * below). The bytes still reach the caller's terminal via net_tx. */
    if (ch == 0x0Eu) { s_caller_lower = TRUE;  return; }
    if (ch == 0x8Eu) { s_caller_lower = FALSE; return; }

    /* existing PETSCII path below — unchanged */
    if (ch == 0x93u) {  /* CLR */
        spy_clear();
        s_vcol = 0; s_vrow = 0; s_color = 1; s_reverse = FALSE;
        return;
    }
    if (ch == 0x0Du || ch == 0x8Du) {  /* CR/shifted-return (0x0A dropped below) */
        s_vcol = 0; spy_advance(); return;
    }
    if (ch == 0x13u) { s_vcol = 0; s_vrow = 0; return; } /* HOME */
    if (ch == 0x11u) { spy_advance(); return; }           /* cursor down */
    if (ch == 0x91u) { if (s_vrow > 0u) s_vrow--; return; } /* cursor up */
    if (ch == 0x1Du) {                          /* cursor right */
        if (++s_vcol >= 40u) { s_vcol = 0; if (s_vrow < 19u) s_vrow++; }
        return;
    }
    if (ch == 0x9Du) {                          /* cursor left */
        if (s_vcol > 0u) s_vcol--;
        else if (s_vrow > 0u) { s_vrow--; s_vcol = 39u; }
        return;
    }
    if (ch == 0x90u) { s_color =  0; return; } /* black      */
    if (ch == 0x05u) { s_color =  1; return; } /* white      */
    if (ch == 0x1Cu) { s_color =  2; return; } /* red        */
    if (ch == 0x9Fu) { s_color =  3; return; } /* cyan       */
    if (ch == 0x9Cu) { s_color =  4; return; } /* purple     */
    if (ch == 0x1Eu) { s_color =  5; return; } /* green      */
    if (ch == 0x1Fu) { s_color =  6; return; } /* blue       */
    if (ch == 0x9Eu) { s_color =  7; return; } /* yellow     */
    if (ch == 0x81u) { s_color =  8; return; } /* orange     */
    if (ch >= 0x95u && ch <= 0x9Bu) { s_color = (u8)(ch - 0x8Cu); return; } /* brown–lt.gray */
    if (ch == 0x12u) { s_reverse = TRUE;  return; } /* RVS ON  */
    if (ch == 0x92u) { s_reverse = FALSE; return; } /* RVS OFF */
    if (ch == 0x14u) {                          /* DEL — shift line left */
        u16 base = (u16)s_vrow * 40u;
        u8 c;
        for (c = s_vcol; c < 39u; c++) {
            SCRN[base + c] = SCRN[base + c + 1u];
            COLR[base + c] = COLR[base + c + 1u];
        }
        SCRN[base + 39u] = 0x20u;
        COLR[base + 39u] = s_color;
        return;
    }
    if (ch == 0x94u) {                          /* INST — shift line right */
        u16 base = (u16)s_vrow * 40u;
        u8 c;
        for (c = 39u; c > s_vcol; c--) {
            SCRN[base + c] = SCRN[base + c - 1u];
            COLR[base + c] = COLR[base + c - 1u];
        }
        SCRN[base + s_vcol] = 0x20u;
        COLR[base + s_vcol] = s_color;
        return;
    }
    if (ch >= 0x80u && ch < 0xA0u) return;  /* drop unhandled control codes */
    if (ch < 0x20u)  return;

    /* Only while the caller is in TEXT charset: uppercase A-Z arrive as PETSCII
     * 0xC1-0xDA, which petscii_to_sc would turn into block glyphs on the spy's
     * graphics charset. Fold them to 0x41-0x5A so they render as uppercase
     * letters. In GRAPHICS mode (gfiles/menus, after 0x8E) 0xC1-0xDA are real
     * block-graphics art and must pass through untouched. */
    if (s_caller_lower && ch >= 0xC1u && ch <= 0xDAu) ch = (u8)(ch - 0x80u);

    addr = (u16)s_vrow * 40u + s_vcol;
    { u8 sc = petscii_to_sc(ch); SCRN[addr] = s_reverse ? (u8)(sc | 0x80u) : sc; }
    COLR[addr] = s_color;

    if (++s_vcol >= 40u) { s_vcol = 0; spy_advance(); }
}

/**
 * session_spy_status()
 * Draw the SysOp status line (row 24) for the 80-col bitmap spy view: caller
 * identity + action-key hints, padded across the row in a distinct color.
 * Resident in main code so it redraws wherever the caller is — including while
 * a feature overlay (e.g. message bases) has displaced the WFC overlay.
 * Row 24 is reserved by the ANSI parser (caller content uses rows 0-23).
 */
void session_spy_status(const session_t *s, u16 elapsed_secs)
{
    char buf[96];
    u8 n;

    if (!s || s->screen_capture.spy_mode != SPY_MODE_80COL) return;

    if (s->user_id == 0) {
        char blank[81];
        memset(blank, ' ', 80);
        blank[80] = '\0';
        spy80_puts_rev(0, (u8)(SPY80_ROWS - 1), blank, 3);
        return;
    }

    sprintf(buf, " %-12s  LVL %u   ON %u:%02u:%02u",
        s->handle[0] ? s->handle : "(LOGGED IN)",
        (unsigned)s->user.access_level,
        (unsigned)(elapsed_secs / 3600u),
        (unsigned)((elapsed_secs / 60u) % 60u),
        (unsigned)(elapsed_secs % 60u));

    n = (u8)strlen(buf);
    while (n < 44u) buf[n++] = ' ';
    buf[n] = '\0';
    strcat(buf, "F1:CHAT  F2:DROP  F3:LEVEL");

    /* pad the whole line to 80 cols so any stale cells are overwritten */
    n = (u8)strlen(buf);
    while (n < SPY80_COLS) buf[n++] = ' ';
    buf[SPY80_COLS] = '\0';

    spy80_puts_rev(0, (u8)(SPY80_ROWS - 1), buf, 3);  /* row 24, reverse cyan bar */
}

/**
 * session_spy_poll()
 * Resident SysOp tick for an active remote session. Reads the local C64
 * keyboard for SysOp action keys and (in 80-col mode) refreshes the status
 * line once per second using a self-computed elapsed time. Safe to call from
 * any context — the main session loop AND a feature's nested loop (e.g. the
 * message reader) — so the status keeps ticking and the action keys stay live
 * wherever the caller is. Cheap: throttled to one redraw per second.
 */
void session_spy_poll(void)
{
    clock_tod_t now;

    if (!s_active || s_active->is_local) return;

    /* SysOp action keys from the local keyboard (PETSCII F-key codes).
     * Convention matches the PETSCII footer: F1=CHAT, F2=DROP, F3=ACCESS.
     * getchx() = raw GETIN (returns 0 if no key); must NOT use getchar() here,
     * which routes through the KERNAL screen editor and hangs while the VIC is
     * in hires bitmap mode for the 80-col spy. */
    {
        u8 k = (u8)getchx();
        switch (k) {
            /* case 133: ... F1 — toggle SysOp chat   (TBD) */
            /* case 134: ... F3 — change access level (TBD) */
            case 137:  /* F2 — drop/kick the caller */
                /* Brief feedback on the status bar (80-col only), then drop
                 * carrier. net_state() leaves NET_CONNECTED, which the
                 * net_state checks in every feature loop (and the main loop)
                 * detect to unwind the session from wherever the caller is. */
                if (s_active->screen_capture.spy_mode == SPY_MODE_80COL) {
                    spy80_puts_rev(0, (u8)(SPY80_ROWS - 1),
                        " DROPPING CALLER...                                                             ",
                        2);  /* red reverse bar */
                }
                net_disconnect();
                return;
            default: break;
        }
    }

    if (s_active->screen_capture.spy_mode != SPY_MODE_80COL) return;

    clock_read(&now);
    if (now.secs == s_spy_last_sec) return;   /* once per second */
    s_spy_last_sec = now.secs;
    session_spy_status(s_active, clock_elapsed(&s_spy_start, &now));
}



/* Drop a remote caller who has been idle past bbs_cfg.idle_timeout_mins.
 * No-op when the timeout is disabled (0) or already fired this session. */
static void sess_idle_check(void)
{
    clock_tod_t now;
    u16 limit;

    if (s_idle_fired || bbs_cfg.idle_timeout_mins == 0) return;

    limit = (u16)bbs_cfg.idle_timeout_mins * 60u;
    clock_read(&now);
    if (clock_elapsed(&s_idle_mark, &now) < limit) return;

    s_idle_fired = TRUE;
    sess_color(s_active, 0x9f, "\x1b[36m");
    sess_tx("\r\nIDLE TOO LONG - BYE!\r\n");
    sess_color(s_active, 0x05, "\x1b[37m");
    net_disconnect();
}

/* Translate one accepted inbound byte from the caller's wire encoding to
 * canonical CP437, per the active session's mode.  Identity for ANSI/ASCII,
 * control bytes, digits and punctuation; only PETSCII text-mode letters move. */
static u8 sess_unxlate(u8 wire)
{
    term_mode_t em;
    if (!s_active) return wire;
    em = s_active->term_mode;
    if (em == TERM_PETSCII && s_active->petscii_lower) em = TERM_PETSCII_LOWER;
    return term_unxlate_byte(em, wire);
}

/* Read one byte from modem (or keyboard in local mode).
 * Returns 0 if nothing available yet. */
static u8 sess_rx_byte(u8 *out)
{
    if (s_local) {
        if (KBD_COUNT == 0) return 0;
        /* getchx() (GETIN), not getchar(): CHRIN would enter the KERNAL line
         * editor — blocking this non-blocking pump until RETURN and echoing
         * the keystrokes (including the password) before the session sees
         * them. KBD_COUNT > 0 guarantees getchx() returns a real key. */
        if (sess_accept_input((u8)getchx(), out)) {
            *out = sess_unxlate(*out);
            return 1;
        }
        return 0;
    } else {
        u16 got;
        if (net_rx(out, 1, &got) == BBS_OK && got == 1) {
            if (sess_accept_input(*out, out)) {
                *out = sess_unxlate(*out);
                clock_read(&s_idle_mark);   /* activity — restart watchdog */
                return 1;
            }
        }
        sess_idle_check();
        return 0;
    }
}

/* Reset a USR.DAY record's counters if its date stamp != today (wfc.date,
 * "MM/DD/YY"), then stamp today. */
static void day_apply_reset(usr_day_record_t *d)
{
  u8 mm = (u8)((wfc.date[0] - '0') * 10 + (wfc.date[1] - '0'));
  u8 dd = (u8)((wfc.date[3] - '0') * 10 + (wfc.date[4] - '0'));
  u8 yy = (u8)((wfc.date[6] - '0') * 10 + (wfc.date[7] - '0'));
  if (d->last_mm != mm || d->last_dd != dd || d->last_yy != yy) {
    d->calls_today = 0;
    d->mins_today  = 0;
    d->last_mm = mm; d->last_dd = dd; d->last_yy = yy;
  }
}

/* Once, when a logged-in caller first reaches the menu: arm the MINS/DAY check
 * and show the time-left banner. Local sysop + NO_TIME_LIMIT levels exempt;
 * disk errors fail open. (CALLS/DAY deferred to a later step.) */
static void session_time_begin(const session_t *s)
{
  u16 mins  = 0;   /* set by access_limits_runtime before use; init quiets oscar64 */
  u8  flags = 0;
  usr_day_record_t day;
  bbs_err_t err;

  s_time_active = FALSE;
  s_time_warned = FALSE;

  if (s->is_local || s->user_id == 0) return;

  if (access_limits_runtime(s->user.access_level, &mins, &flags,
                            bbs_cfg.device_system) != BBS_OK)
    return;
  if (flags & ACCESS_F_NO_TIME_LIMIT) return;

  err = usrday_load(s->user_id, &day, bbs_cfg.device_system);
  if (err != BBS_OK && err != BBS_ENOTFOUND) return;
  day_apply_reset(&day);

  s_day_mins_used  = day.mins_today;
  s_day_mins_limit = mins;
  s_time_active    = TRUE;
  clock_read(&s_session_start);

  {
    u16 rem = (mins > day.mins_today) ? (mins - day.mins_today) : 0;
    char buf[32];
    sprintf(buf, "\r\nTIME LEFT TODAY: %u MIN\r\n", (unsigned)rem);
    sess_color(s, 0x9f, "\x1b[36m");
    session_emit(s, buf);
    sess_color(s, 0x05, "\x1b[37m");
  }
}

/* Once-per-second MINS/DAY check for the active remote session. */
static void session_time_check(void)
{
  clock_tod_t now;
  const session_t *s = s_active;
  u16 used, rem;

  if (!s_time_active || !s || s->is_local) return;

  clock_read(&now);
  if (now.secs == s_time_last_sec) return;
  s_time_last_sec = now.secs;

  used = s_day_mins_used + (u16)(clock_elapsed(&s_session_start, &now) / 60u);

  if (used >= s_day_mins_limit) {
    sess_color(s, 0x9f, "\x1b[36m");
    session_emit(s, "\r\nDAILY TIME LIMIT REACHED. GOODBYE.\r\n");
    sess_color(s, 0x05, "\x1b[37m");
    net_disconnect();
    s_time_active = FALSE;
    return;
  }

  rem = s_day_mins_limit - used;
  if (!s_time_warned && rem <= 2) {
    s_time_warned = TRUE;
    sess_color(s, 0x9f, "\x1b[36m");
    session_emit(s, "\r\n2 MINUTES LEFT.\r\n");
    sess_color(s, 0x05, "\x1b[37m");
  }
}

bool_t sess_carrier_ok(const session_t *s)
{
  return (bool_t)(s->is_local || net_state() == NET_CONNECTED);
}

bool_t sess_getc(u8 *out)
{
  if (sess_rx_byte(out)) return TRUE;
  /* No caller byte ready — use the idle moment to tick the resident SysOp
   * spy (status line + action keys). Because every feature loop waits on
   * sess_getc, this keeps the spy live everywhere with no per-feature code. */
  session_spy_poll();
  session_time_check();
  return FALSE;
}

/* Blocking single-key read; pumps modem/spy/idle, honors carrier.
 * Returns FALSE immediately when carrier drops so callers avoid spinning. */
bool_t sess_read_key(const session_t *s, u8 *out)
{
  u8 ch;
  for (;;) {
    if (sess_getc(&ch)) { *out = ch; return TRUE; }
    if (!sess_carrier_ok(s)) return FALSE;
  }
}

/* Blocking line reader: collect a CR-terminated line into buf (up to max
 * chars, always NUL-terminated), echoing each character as it is typed.
 * Handles BS/DEL erase and skips the bare LF of a CRLF pair.  When uppercase
 * is set, a-z fold to A-Z on entry.  Returns early with whatever was collected
 * if carrier drops, so a full-buffer wait never outlives the caller.  Resident
 * (costs no overlay space) and session-wide; promoted from the bulletin reader. */
void sess_read_line(const session_t *s, char *buf, u8 max, bool_t uppercase)
{
  u8 len = 0, ch;
  char e[2];
  memset(buf, 0, (u8)(max + 1));
  while (len < max) {
    if (!sess_read_key(s, &ch)) return;
    if (ch == 10) continue;            /* skip LF — CRLF terminals send CR+LF */
    if (ch == 13) { session_emit(s, "\n"); return; }
    if ((ch == 8 || ch == 20) && len > 0) {
      len--; buf[len] = '\0'; sess_erase_char(s); continue;
    }
    if (ch >= 0x20 && ch < 0x7f) {
      if (uppercase && ch >= 'a' && ch <= 'z') ch = (u8)(ch - 32);
      buf[len++] = (char)ch; buf[len] = '\0';
      e[0] = (char)ch; e[1] = '\0'; session_emit(s, e);
    }
  }
  for (;;) {                           /* buffer full: drain to CR for a clean line end */
    if (!sess_read_key(s, &ch)) return;
    if (ch == 10) continue;
    if (ch == 13) { session_emit(s, "\n"); return; }
  }
}

bbs_err_t session_init(session_t *s, bool_t is_local) {
  if (!s) return BBS_EBADARG;

  memset(s, 0, sizeof(*s));
  s->state      = SESS_CONNECTED;
  s->user_id    = 0;
  s->is_local   = is_local;
  s->term_mode  = TERM_PETSCII;
  s->petscii_lower = FALSE;  /* local spy stays uppercase/graphics; remote is set later */
  s->term_width = 40;  /* PETSCII default; overridden by term_detect_all or user prefs */
  s->term_rows  = 24;
  s->online_limit = bbs_cfg.max_call_time;
  s->auth_step  = 0;  /* 0=handle, 1=password, 2=confirm new user, 3=confirm password again */

  /* MINS/DAY enforcement: reset per-session timing state. */
  s_online_begun  = FALSE;
  s_time_active   = FALSE;
  s_time_warned   = FALSE;
  s_time_last_sec = 255;
  s_session_start.hours = 0xFF;   /* sentinel: time_begin not run yet */

  /* Initialize menu state to MAIN menu at depth 0 */
  strncpy(s->menu_state.current_menu, "main", 15);
  s->menu_state.current_menu[15] = '\0';
  s->menu_state.depth = 0;
  memset(s->menu_state.parent_menus, 0, sizeof(s->menu_state.parent_menus));

  s_local = is_local;   /* set module-level flag for sess_tx / sess_rx_byte */
  s_rx_last_was_cr = FALSE;

  return BBS_OK;
}

bbs_err_t session_step(session_t *s) {
  u8 ch;
  // cppcheck-suppress variableScope
  bbs_err_t err;

  if (!s) return BBS_EBADARG;

  switch (s->state) {

  case SESS_CONNECTED:
    /* Send banner to caller */
    sess_color(s, 0x9f, "\x1b[36m");  /* cyan */
    session_emit(s, "HANDLE");
    sess_color(s, 0x05, "\x1b[37m");  /* white */
    session_emit(s, ": ");
    s->state = SESS_AUTHENTICATING;
    s->auth_step = 0;  /* Collecting handle */
    s->login_attempts = 0;
    s->empty_handle_attempts = 0;
    memset(&s->handle, 0, sizeof(s->handle));
    memset(&s->password, 0, sizeof(s->password));
    break;

  case SESS_AUTHENTICATING:
    /* Multi-phase auth: 0=handle, 1=password, 2=new user confirm, 3=password confirm */
    if (!sess_rx_byte(&ch)) break;

    /* ===== Phase 0: Collect handle ===== */
    if (s->auth_step == 0) {
      if (ch == '\r' || ch == '\n') {
        if (s->handle[0] == 0) {
          s->empty_handle_attempts++;
          if (s->empty_handle_attempts >= 3) {
            sess_color(s, 0x9f, "\x1b[36m");
            session_emit(s, "\r\nARE YOU A BOT? BYE!\r\n");
            sess_color(s, 0x05, "\x1b[37m");
            net_disconnect();
            s->state = SESS_IDLE;
            break;
          }
          sess_color(s, 0x9f, "\x1b[36m");
          session_emit(s, "\r\nHANDLE ");
          sess_color(s, 0x05, "\x1b[37m");
          session_emit(s, "(2-15 CHARS): ");
          break;
        }
        /* "NEW" shortcut: skip directly to registration */
        if (strcmp(s->handle, "NEW") == 0) {
          memset(s->handle,   0, sizeof(s->handle));
          memset(s->password, 0, sizeof(s->password));
          session_clear_screen(s);
          session_display_file(s, 'g', "newuser");
          sess_color(s, 0x9f, "\x1b[36m");
          sess_tx("\r\nCREATE AN ACCOUNT? Y/N ");
          sess_color(s, 0x05, "\x1b[37m");
          s->reg_step    = 0;
          s->reg_editing = 0;
          s->state = SESS_REGISTERING;
          break;
        }
        /* Handle complete; ask for password */
        sess_color(s, 0x9f, "\x1b[36m");
        session_emit(s, "\r\nPASSWORD");
        sess_color(s, 0x05, "\x1b[37m");
        session_emit(s, ": ");
        s->auth_step = 1;
        memset(&s->password, 0, sizeof(s->password));
      } else if (ch == 0x08 || ch == 0x7F || ch == 0x14) {
        /* Backspace (0x08=BS, 0x7F=DEL, 0x14=PETSCII DEL) */
        u8 len = (u8)strlen(s->handle);
        if (len > 0) {
          s->handle[len - 1] = 0;
          sess_erase_char(s);
        }
      } else if (ch >= 0x20 && ch < 0x7F && strlen(s->handle) < 15) {
        /* Uppercase and echo handle char visibly */
        if (ch >= 'a' && ch <= 'z') ch -= 0x20;
        u8 pos = (u8)strlen(s->handle);
        s->handle[pos] = ch;
        s->handle[pos + 1] = 0;  /* Ensure null-terminated */
        const char echo[2] = { ch, 0 };
        session_emit(s, echo);
      }
    }

    /* ===== Phase 1: Collect password ===== */
    else if (s->auth_step == 1) {
      if (ch == '\r' || ch == '\n') {
        if (s->password[0] == 0) {
          s->empty_handle_attempts++;
          if (s->empty_handle_attempts >= 3) {
            sess_color(s, 0x9f, "\x1b[36m");
            session_emit(s, "\r\nTOO MANY TRIES, HACKER!\r\n");
            sess_color(s, 0x05, "\x1b[37m");
            net_disconnect();
            s->state = SESS_IDLE;
            break;
          }
          sess_color(s, 0x9f, "\x1b[36m");
          session_emit(s, "\r\nPASSWORD");
          sess_color(s, 0x05, "\x1b[37m");
          session_emit(s, ": ");
          break;
        }
        /* Both handle and password collected; attempt login.
         * auth_prompt_login lives in OVL_AUTH (bank 7) — load it over the
         * overlay zone, then reload OVL_WFC (spy view code) before doing
         * anything else, same pattern as action_list_boards/OVL_MSGS.
         *
         * auth_prompt_login() lives IN the overlay just requested — on a
         * failed load it must not run (that would execute whatever is
         * still at $9700). Falling through with err=BBS_EIO reuses the
         * existing "bad login" branch below: it already rate-limits and
         * never distinguishes handle vs password, so folding a load
         * failure into it needs no new disconnect logic. */
        if (disk_load_overlay(P"OVL_AUTH") == BBS_OK) {
          wfc.ovl_wfc_loaded = FALSE;
          err = auth_prompt_login(s);
        } else {
          session_emit(s, "\r\nERROR: OVL_AUTH LOAD FAILED.\r\n");
          err = BBS_EIO;
        }
        wfc_reload();
        if (err == BBS_OK) {
          /* Login successful */
          sess_color(s, 0x9f, "\x1b[36m");
          session_emit(s, "\r\n\r\nWELCOME, ");
          sess_color(s, 0x05, "\x1b[37m");
          session_emit(s, s->handle);
          sess_color(s, 0x9f, "\x1b[36m");
          session_emit(s, "!\r\n\r\n");
          session_run_login_doors(s);  /* run DOOR_F_LOGIN doors in login_order before menu */
          s->menu_needs_pause = TRUE;  /* "[PRESS ANY KEY]" before first menu */
          s->state = SESS_IN_MENU;
        } else if (err == BBS_ENOTFOUND) {
          /* User not found; offer registration */
          sess_color(s, 0x9f, "\x1b[36m");
          session_emit(s, "\r\n\"");
          sess_color(s, 0x05, "\x1b[37m");
          session_emit(s, s->handle);
          sess_color(s, 0x9f, "\x1b[36m");
          session_emit(s, "\" NOT FOUND!\r\nAPPLY FOR AN ACCOUNT? Y/N ");
          sess_color(s, 0x05, "\x1b[37m");
          s->auth_step = 2;
        } else {
          /* Bad password — never reveal whether handle or password was wrong */
          s->login_attempts++;
          if (s->login_attempts >= 3) {
            sess_color(s, 0x9f, "\x1b[36m");
            session_emit(s, "\r\nTOO MANY TRIES, HACKER!\r\n");
            sess_color(s, 0x05, "\x1b[37m");
            net_disconnect();
            s->state = SESS_IDLE;
            break;
          }
          sess_color(s, 0x9f, "\x1b[36m");
          session_emit(s, "\r\nINVALID LOGIN.\r\n");
          session_emit(s, "HANDLE");
          sess_color(s, 0x05, "\x1b[37m");
          session_emit(s, ": ");
          s->auth_step = 0;
          memset(&s->handle, 0, sizeof(s->handle));
          memset(&s->password, 0, sizeof(s->password));
        }
      } else if (ch == 0x08 || ch == 0x7F || ch == 0x14) {
        /* Backspace (0x08=BS, 0x7F=DEL, 0x14=PETSCII DEL) */
        u8 len = strlen(s->password);
        if (len > 0) {
          s->password[len - 1] = 0;
          sess_erase_char(s);
        }
      } else if (ch >= 0x20 && ch < 0x7F && strlen(s->password) < USER_PASSWORD_MAX) {
        /* Uppercase password to match stored hash (sysop stores "PASS" uppercase). */
        if (ch >= 'a' && ch <= 'z') ch -= 0x20;
        u8 pos = (u8)strlen(s->password);
        s->password[pos] = ch;
        s->password[pos + 1] = 0;  /* Ensure null-terminated */
        session_emit(s, "*");
      }
    }

    /* ===== Phase 2: Confirm new user account creation ===== */
    else if (s->auth_step == 2) {
      if (ch == 'Y' || ch == 'y') {
        session_emit(s, "Y");
        memset(s->handle,   0, sizeof(s->handle));
        memset(s->password, 0, sizeof(s->password));
        session_clear_screen(s);
        session_display_file(s, 'g', "newuser");
        sess_color(s, 0x9f, "\x1b[36m");
        sess_tx("\r\nCREATE AN ACCOUNT? Y/N ");
        sess_color(s, 0x05, "\x1b[37m");
        s->reg_step    = 0;
        s->reg_editing = 0;
        s->state = SESS_REGISTERING;
      } else if (ch == 'N' || ch == 'n') {
        sess_color(s, 0x9f, "\x1b[36m");
        session_emit(s, "N");
        s->state = SESS_LOGOFF;
      }
    }
    break;

  case SESS_REGISTERING: {
    if (!sess_rx_byte(&ch)) break;
    if (ch >= 'a' && ch <= 'z') ch -= 0x20;
    session_reg_step(s, ch);
    break;
  }

  case SESS_IN_MENU:
    /* One-time MINS/DAY arm on first menu entry (login + new-user reg both
     * land here). */
    if (!s_online_begun) {
      s_online_begun = TRUE;
      session_time_begin(s);
    }

    /* Menu system: display and dispatch single-char commands */

    /* Display menu once per session when entering menu state */
    if (!s->menu_displayed) {
      if (s->menu_needs_pause) {
        u8 pause_ch;
        s->menu_needs_pause = FALSE;
        sess_color(s, 0x9f, "\x1b[36m");
        session_emit(s, "\r\n[PRESS ANY KEY]");
        sess_color(s, 0x05, "\x1b[37m");
        /* sess_read_key keeps sysop spy/idle checks live while paused. */
        sess_read_key(s, &pause_ch);
      }
      s->menu_displayed = TRUE;
      menu_display(s);
      prompt_cursor_arm(s);
    }
    
    if (!sess_rx_byte(&ch)) { prompt_cursor_tick(s); break; }

    /* Ignore CR, LF, and other control chars (cursor stays armed -> keeps
     * animating until a real command key arrives). */
    if (ch < 0x20 || ch == 0x7F) break;

    prompt_cursor_clear(s);   /* about to echo: reset color/reverse, drop the block */

    ch = (ch >= 'a' && ch <= 'z') ? (ch - 0x20) : ch;  /* Uppercase */

    /* Echo key in-place (no newline; each command's output begins with \r\n) */
    { const char e[2] = { ch, 0 }; sess_tx(e); }

    /* Dispatch to menu system */
    menu_dispatch(s, ch);

    /* Check if user requested logoff (menu_back at depth 0) */
    if (s->state == SESS_LOGOFF) {
      sess_tx("\r\nLOGGING OFF...");
    }
    break;

  case SESS_LOGOFF:
    sess_tx("\r\n\r\nGOODBYE!\r\n\r\n");
    s->state = SESS_IDLE;
    break;

  case SESS_IDLE:
  case SESS_ERROR:
  default:
    break;
  }

  return BBS_OK;
}

void session_emit(const session_t *s, const char *text) {
  if (!s || !text) return;
  
  u8 xlat_buf[4];  /* max bytes per character (PETSCII escape sequences) */
  u8 xlat_len;
  
  for (u16 i = 0; text[i] != '\0'; i++) {
    u8 ch = (u8)text[i];

    /* Skip bare CR — line breaks are driven by \n below */
    if (ch == '\r') continue;

    /* Special handling for newlines per terminal type */
    if (ch == '\n') {
      if (s->linefeed_mode) {
        /* ASCII/ANSI terminals need CRLF */
        if (s->is_local) {
          printf("\r\n");
        } else {
          u16 sent;
          net_tx("\r\n", 2, &sent);
        }
      } else {
        /* PETSCII needs just CR */
        char lf_char = '\r';
        if (s->is_local) {
          putchar(lf_char);
        } else {
          u16 sent;
          net_tx(&lf_char, 1, &sent);
        }
      }
      /* Feed spy: the 80-col bitmap parser treats CR and LF separately and
       * needs both to start a fresh line; the PETSCII spy's CR also resets
       * the column. Routed by spy_mode so ASCII 80-col is covered too. */
      if (!s->is_local && s_active) {
          if (s->screen_capture.spy_mode == SPY_MODE_80COL) {
              session_spy_feed(0x0Du);  /* CR — reset column to 0 */
              session_spy_feed(0x0Au);  /* LF — advance row */
          } else {
              session_spy_feed(0x0Du);
          }
      }
      continue;
    }

    /* Translate each byte through the appropriate character set */
    term_mode_t em = s->term_mode;
    if (em == TERM_PETSCII && s->petscii_lower) em = TERM_PETSCII_LOWER;
    xlat_len = term_xlate_byte(em, ch, xlat_buf, sizeof(xlat_buf));
    
    if (xlat_len > 0) {
      /* Send translated byte(s) to output */
      u16 sent;
      if (s->is_local) {
        /* Local console: write directly */
        for (u8 j = 0; j < xlat_len; j++) {
          putchar(xlat_buf[j]);
        }
      } else {
        /* Remote: send via network */
        net_tx((const char *)xlat_buf, xlat_len, &sent);
        if (s_active) {
            if (s->screen_capture.spy_mode == SPY_MODE_80COL) {
                /* Feed pre-translation CP437/ASCII byte; spy80_ansi remaps it.
                 * Covers ANSI/CP437 and ASCII 80-col callers. */
                session_spy_feed(ch);
            } else if (s->term_mode == TERM_PETSCII) {
                u8 kk;
                for (kk = 0; kk < xlat_len; kk++) session_spy_feed(xlat_buf[kk]);
            }
        }
      }
    }
  }
}

void session_clear_screen(const session_t *s) {
  u16 sent;
  // cppcheck-suppress variableScope
  u8 i;

  if (!s) return;

  if (s->term_mode == TERM_PETSCII) {
    u8 clr = 0x93;  /* PETSCII CLR/HOME */
    if (s->is_local) {
      putchar(clr);
    } else {
      net_tx((const char *)&clr, 1, &sent);
      session_spy_feed(0x93u);
    }
  } else if (s->term_mode == TERM_ANSI_CP437) {
    if (s->is_local) {
      printf("\x1b[2J\x1b[H");
    } else {
      net_tx("\x1b[2J\x1b[H", 7, &sent);
    }
  } else {
    /* ASCII: scroll off with blank lines */
    for (i = 0; i < 24; i++) {
      if (s->is_local) {
        printf("\r\n");
      } else {
        net_tx("\r\n", 2, &sent);
      }
    }
  }

  /* 80-col bitmap spy: clear the spy directly regardless of how the wire was
   * cleared (ANSI ESC[2J vs ASCII blank lines) so the bitmap resets crisply
   * instead of drawing each new screen over the previous one. */
  if (!s->is_local && s_active &&
      s_active->screen_capture.spy_mode == SPY_MODE_80COL) {
    session_spy_feed(0x1Bu); session_spy_feed((u8)'['); session_spy_feed((u8)'2'); session_spy_feed((u8)'J');
    session_spy_feed(0x1Bu); session_spy_feed((u8)'['); session_spy_feed((u8)'H');
  }
}

/* Emit a terminal color change for a pipe code like |03. */
void sess_pipe_color(const session_t *s, u8 color)
{
  switch (color) {
    case 0:  sess_color(s, 0x90, "\x1b[30m"); break;
    case 1:  sess_color(s, 0x05, "\x1b[37m"); break;
    case 2:  sess_color(s, 0x1C, "\x1b[31m"); break;
    case 3:  sess_color(s, 0x9F, "\x1b[36m"); break;
    case 4:  sess_color(s, 0x9C, "\x1b[35m"); break;
    case 5:  sess_color(s, 0x1E, "\x1b[32m"); break;
    case 6:  sess_color(s, 0x1F, "\x1b[34m"); break;
    case 7:  sess_color(s, 0x9E, "\x1b[33m"); break;
    case 8:  sess_color(s, 0x81, "\x1b[91m"); break;
    case 9:  sess_color(s, 0x95, "\x1b[33m"); break;
    case 10: sess_color(s, 0x96, "\x1b[91m"); break;
    case 11: sess_color(s, 0x97, "\x1b[90m"); break;
    case 12: sess_color(s, 0x98, "\x1b[37m"); break;
    case 13: sess_color(s, 0x99, "\x1b[92m"); break;
    case 14: sess_color(s, 0x9A, "\x1b[94m"); break;
    case 15: sess_color(s, 0x9B, "\x1b[97m"); break;
    case 16: sess_color(s, 0x12, "\x1b[7m");  break;  /* reverse on  */
    case 17: sess_color(s, 0x92, "\x1b[27m"); break;  /* reverse off */
    default: break;
  }
}

/* Flush a literal text chunk through the normal terminal translator. */
static void session_emit_text_chunk(const session_t *s, char *chunk, u8 *len)
{
  if (*len == 0) return;
  chunk[*len] = '\0';
  session_emit(s, chunk);
  *len = 0;
}


/**
 * session_display_file()
 *
 * Phase B: Display terminal-aware file from disk.
 *
 * CBM DOS OPEN always succeeds (carry clear) even for missing files;
 * "FILE NOT FOUND" only surfaces as 0 bytes on the first read (immediate
 * EOI with no data).  We probe with the first disk_gets: if ≤ 0 bytes
 * come back the file is absent and we continue down the fallback chain.
 *
 * Fallback chain (most-specific first):
 *   <pfx>.<name> <mode> <width>  →  <pfx>.<name> <mode>  →  <pfx>.<name> <width>  →  <pfx>.<name>
 *
 * Color codes in gfiles — both forms are equivalent:
 *   |NN   — standard pipe notation (N = 0-9 digit, two digits 00-17)
 *   \NN   — C64-native alias; the £ key (next to Return) produces CHR$(92)=backslash,
 *            so sysops authoring files directly on C64 hardware can use \ instead of |
 *            (| is ASCII 0x7C and has no key on the standard C64 keyboard)
 *
 * MCI substitution tokens (set before calling this function):
 *   %BN  — current board name (session_set_mci_board)
 *   %MN  — current message number (session_set_mci_msg)
 */
bbs_err_t session_display_file(const session_t *s, char prefix, const char *base_name) {
  term_filename_t names;
  char line[80];
  char chunk[80];
  i16 got;
  u8 i;
  u8 chunk_len;
  bool_t restore_petscii_lower = FALSE;
  bool_t restore_footer = FALSE;
  bbs_err_t err;

  if (!s || !base_name) {
    return BBS_EBADARG;
  }

  session_refresh_timeleft(s);   /* keep %TL current for this render */

  disk_build_term_filename(&names, prefix, base_name, s->term_mode, s->term_width);
  err = cfg_send_drive_init(bbs_cfg.device_gfiles, bbs_cfg.init_gfiles);
  if (err != BBS_OK) {
    return err;
  }

  for (i = 0; i < 4; i++) {
    err = disk_open(bbs_cfg.device_gfiles, bbs_cfg.drive_gfiles, names.names[i], DISK_READ);
    if (err != BBS_OK) {
      continue;
    }

    if (s->term_mode == TERM_PETSCII) {
      /* PETSCII raw path: bulk disk reads to avoid per-byte IEC bus negotiation.
       * krnio_getch() calls CHKIN+CLRCHN per byte (~2ms IEC overhead each).
       * disk_read() uses krnio_read(): ONE CHKIN + N bytes + ONE CLRCHN per call.
       * Probe: missing file returns 0 or 1 bytes before EOF. */
      if (s->is_local) {
        spy_clear_footer();
        restore_footer = (s->user_id != 0);
        if (session_screen_lower()) {
          u8 uc = 0x8Eu;
          session_emit_raw(s, &uc, 1);
          restore_petscii_lower = TRUE;
        }
      }
      u8 buf[64];
      u8 raw[64];
      u8 raw_len;
      u8 ps;
      u8 pd;
      u8 ps_pfx;  /* which escape char started the current |/\ sequence */
      i16 n = disk_read(buf, sizeof(buf));
      if (n < 2) {
        disk_close();
        continue;
      }
      /* Select the gfile charset. Default art is uppercase/graphics, so emit
       * CHR$(142) (0x8E). When the sysop sets PETSCII_LOWER_ART, the gfiles are
       * authored in the text charset (mixed-case letters + the text-mode
       * graphics subset — the RetroCampus look), so emit CHR$(14) (0x0E) to
       * force text mode for the caller. Scoped to remote callers; the local
       * sysop console stays graphics (its WFC chrome is graphics-charset). */
      bool_t lower_art = (!s->is_local && bbs_cfg.petscii_lower_art);
      if (!restore_petscii_lower) {
        u8 uc = lower_art ? (u8)0x0Eu : (u8)0x8Eu;
        session_emit_raw(s, &uc, 1);
      }
      raw_len = 0;
      ps = 0;
      pd = 0;
      ps_pfx = (u8)'|';
      do {
        i16 j;
        for (j = 0; j < n; j++) {
          u8 b = (buf[j] == 0x0A) ? (u8)0x0D : buf[j];
          if (ps == 0) {
            if (b == (u8)'|' || b == (u8)'\\') {
              ps_pfx = b; ps = 1;
            } else if (b == (u8)'%') {
              ps = 3;
            } else {
              if (raw_len >= (u8)(sizeof(raw) - 1u)) { session_emit_raw(s, raw, raw_len); raw_len = 0; }
              raw[raw_len++] = b;
            }
          } else if (ps == 1) {
            if (b >= (u8)'0' && b <= (u8)'9') {
              pd = (u8)(b - '0');
              ps = 2;
            } else {
              if (raw_len >= (u8)(sizeof(raw) - 1u)) { session_emit_raw(s, raw, raw_len); raw_len = 0; }
              raw[raw_len++] = ps_pfx;  /* emit the | or \ that started this sequence */
              if (b == (u8)'|' || b == (u8)'\\') {
                ps_pfx = b; ps = 1;
              } else {
                ps = 0;
                if (raw_len >= (u8)(sizeof(raw) - 1u)) { session_emit_raw(s, raw, raw_len); raw_len = 0; }
                raw[raw_len++] = b;
              }
            }
          } else if (ps == 2) {
            if (b >= (u8)'0' && b <= (u8)'9') {
              if (raw_len > 0) { session_emit_raw(s, raw, raw_len); raw_len = 0; }
              sess_pipe_color(s, (u8)(pd * 10u + (b - '0')));
              ps = 0;
            } else {
              if (raw_len >= (u8)(sizeof(raw) - 1u)) { session_emit_raw(s, raw, raw_len); raw_len = 0; }
              raw[raw_len++] = ps_pfx;  /* emit the | or \ that started this sequence */
              if (raw_len >= (u8)(sizeof(raw) - 1u)) { session_emit_raw(s, raw, raw_len); raw_len = 0; }
              raw[raw_len++] = (u8)('0' + pd);
              if (b == (u8)'|' || b == (u8)'\\') {
                ps_pfx = b; ps = 1;
              } else {
                ps = 0;
                if (raw_len >= (u8)(sizeof(raw) - 1u)) { session_emit_raw(s, raw, raw_len); raw_len = 0; }
                raw[raw_len++] = b;
              }
            }
          } else if (ps == 3) {
            /* Saw '%': dispatch on next letter to known MCI prefix */
            if (b == (u8)'B') {
              ps = 4;
            } else if (b == (u8)'M') {
              ps = 5;
            } else if (b == (u8)'T') {
              ps = 6;
            } else {
              if (raw_len >= (u8)(sizeof(raw) - 1u)) { session_emit_raw(s, raw, raw_len); raw_len = 0; }
              raw[raw_len++] = (u8)'%';
              if (b == (u8)'|' || b == (u8)'\\') { ps_pfx = b; ps = 1; }
              else if (b == (u8)'%') { ps = 3; }
              else { ps = 0; if (raw_len >= (u8)(sizeof(raw) - 1u)) { session_emit_raw(s, raw, raw_len); raw_len = 0; } raw[raw_len++] = b; }
            }
          } else if (ps == 4) {
            /* Saw '%B'; 'N' completes %BN = board name */
            if (b == (u8)'N') {
              if (raw_len > 0) { session_emit_raw(s, raw, raw_len); raw_len = 0; }
              { u8 ml = 0; u8 ub[17];
                while (ml < 16u && s_mci_board[ml]) {
                  char c = s_mci_board[ml];
                  ub[ml] = (c >= 'a' && c <= 'z') ? (u8)(c - 0x20) : (u8)c;
                  ml++;
                }
                if (ml > 0) session_emit_raw(s, ub, ml); }
              ps = 0;
            } else {
              if (raw_len >= (u8)(sizeof(raw) - 1u)) { session_emit_raw(s, raw, raw_len); raw_len = 0; }
              raw[raw_len++] = (u8)'%';
              if (raw_len >= (u8)(sizeof(raw) - 1u)) { session_emit_raw(s, raw, raw_len); raw_len = 0; }
              raw[raw_len++] = (u8)'B';
              if (b == (u8)'|' || b == (u8)'\\') { ps_pfx = b; ps = 1; }
              else if (b == (u8)'%') { ps = 3; }
              else { ps = 0; if (raw_len >= (u8)(sizeof(raw) - 1u)) { session_emit_raw(s, raw, raw_len); raw_len = 0; } raw[raw_len++] = b; }
            }
          } else if (ps == 5) {
            /* ps == 5: saw '%M'; 'N' completes %MN = message number */
            if (b == (u8)'N') {
              if (raw_len > 0) { session_emit_raw(s, raw, raw_len); raw_len = 0; }
              { u8 ml = 0; while (s_mci_msgnum[ml]) ml++;
                if (ml > 0) session_emit_raw(s, (const u8 *)s_mci_msgnum, ml); }
              ps = 0;
            } else {
              if (raw_len >= (u8)(sizeof(raw) - 1u)) { session_emit_raw(s, raw, raw_len); raw_len = 0; }
              raw[raw_len++] = (u8)'%';
              if (raw_len >= (u8)(sizeof(raw) - 1u)) { session_emit_raw(s, raw, raw_len); raw_len = 0; }
              raw[raw_len++] = (u8)'M';
              if (b == (u8)'|' || b == (u8)'\\') { ps_pfx = b; ps = 1; }
              else if (b == (u8)'%') { ps = 3; }
              else { ps = 0; if (raw_len >= (u8)(sizeof(raw) - 1u)) { session_emit_raw(s, raw, raw_len); raw_len = 0; } raw[raw_len++] = b; }
            }
          } else {
            /* ps == 6: saw '%T'; 'L' completes %TL = time left. Letters are
             * uppercased so "10m"/"unlim" stay readable in the PETSCII
             * uppercase/graphics charset (mirrors the %BN path). */
            if (b == (u8)'L') {
              if (raw_len > 0) { session_emit_raw(s, raw, raw_len); raw_len = 0; }
              { u8 ml = 0; u8 ub[8];
                while (ml < 7u && s_mci_timeleft[ml]) {
                  char c = s_mci_timeleft[ml];
                  ub[ml] = (c >= 'a' && c <= 'z') ? (u8)(c - 0x20) : (u8)c;
                  ml++;
                }
                if (ml > 0) session_emit_raw(s, ub, ml); }
              ps = 0;
            } else {
              if (raw_len >= (u8)(sizeof(raw) - 1u)) { session_emit_raw(s, raw, raw_len); raw_len = 0; }
              raw[raw_len++] = (u8)'%';
              if (raw_len >= (u8)(sizeof(raw) - 1u)) { session_emit_raw(s, raw, raw_len); raw_len = 0; }
              raw[raw_len++] = (u8)'T';
              if (b == (u8)'|' || b == (u8)'\\') { ps_pfx = b; ps = 1; }
              else if (b == (u8)'%') { ps = 3; }
              else { ps = 0; if (raw_len >= (u8)(sizeof(raw) - 1u)) { session_emit_raw(s, raw, raw_len); raw_len = 0; } raw[raw_len++] = b; }
            }
          }
        }
        n = disk_read(buf, sizeof(buf));
      } while (n > 0);
      if (raw_len > 0) session_emit_raw(s, raw, raw_len);
    } else {
      /* Non-PETSCII: line-based read through CP437 translation layer.
       * Probe: CBM DOS OPEN succeeds even for missing files; missing file
       * returns exactly 1 garbage byte + immediate EOI. */
      bool_t prev_had_cr = FALSE;
      got = disk_gets(line, sizeof(line));
      if (got <= 1) {
        disk_close();
        continue;
      }
      do {
        /* Skip orphaned LF that is the second byte of a CRLF pair. */
        if (prev_had_cr && got == 1 && (u8)line[0] == '\n') {
          prev_had_cr = FALSE;
          got = disk_gets(line, sizeof(line));
          continue;
        }
        prev_had_cr = (got > 0 && (u8)line[got - 1] == '\r');
        /* Detect terminator before stripping: prompt files have no terminator
         * and must not get a trailing newline (cursor must stay on same line). */
        { bool_t line_terminated = (got > 0 &&
              ((u8)line[got-1] == '\r' || (u8)line[got-1] == '\n'));
          while (got > 0 && (line[got-1] == '\r' || line[got-1] == '\n')) {
            line[--got] = '\0';
          }
          chunk_len = 0;
          for (i = 0; i < (u8)got; i++) {
            if ((line[i] == '|' || line[i] == '\\') && (i + 2u) < (u8)got &&
                line[i + 1u] >= '0' && line[i + 1u] <= '9' &&
                line[i + 2u] >= '0' && line[i + 2u] <= '9') {
              session_emit_text_chunk(s, chunk, &chunk_len);
              sess_pipe_color(s, (u8)((line[i + 1u] - '0') * 10u + (line[i + 2u] - '0')));
              i += 2u;
            } else if (line[i] == '%' && (i + 2u) < (u8)got &&
                       line[i + 1u] == 'B' && line[i + 2u] == 'N') {
              session_emit_text_chunk(s, chunk, &chunk_len);
              session_emit(s, s_mci_board);
              i += 2u;
            } else if (line[i] == '%' && (i + 2u) < (u8)got &&
                       line[i + 1u] == 'M' && line[i + 2u] == 'N') {
              session_emit_text_chunk(s, chunk, &chunk_len);
              session_emit(s, s_mci_msgnum);
              i += 2u;
            } else if (line[i] == '%' && (i + 2u) < (u8)got &&
                       line[i + 1u] == 'T' && line[i + 2u] == 'L') {
              session_emit_text_chunk(s, chunk, &chunk_len);
              session_emit(s, s_mci_timeleft);
              i += 2u;
            } else {
              if (chunk_len >= (u8)(sizeof(chunk) - 1u)) {
                session_emit_text_chunk(s, chunk, &chunk_len);
              }
              chunk[chunk_len++] = line[i];
            }
          }
          session_emit_text_chunk(s, chunk, &chunk_len);
          /* Only terminated lines get a newline. Prompt files (p.*) have no
           * terminator on their last line so the input cursor stays put. */
          if (line_terminated) session_emit(s, "\n");
        }
        got = disk_gets(line, sizeof(line));
      } while (got > 0);
    }

    disk_close();
    if (restore_petscii_lower) { u8 lo = 0x0Eu; session_emit_raw(s, &lo, 1); }
    /* Remote PETSCII text-mode callers: the gfile was shown in the graphics
     * charset (0x8E emitted above), but their base charset is text. Restore it
     * (0x0E) so the next session_emit() menu renders as text, not graphics
     * glyphs. Without this, every system-generated menu after a gfile garbles. */
    if (!s->is_local && s->term_mode == TERM_PETSCII && s->petscii_lower) {
      u8 lo = 0x0Eu; session_emit_raw(s, &lo, 1);
    }
    if (restore_footer) { wfc_display_session(s); }
    sess_reset_color(s);
    return BBS_OK;
  }

  if (restore_petscii_lower) {
    u8 lo = 0x0Eu;
    session_emit_raw(s, &lo, 1);
  }
  return BBS_ENOTFOUND;
}

bbs_err_t session_done(session_t *s) {
  if (!s) return BBS_EBADARG;

  if (s->screen_capture.spy_mode == SPY_MODE_80COL) {
    spy80_done();
    s->screen_capture.spy_mode = SPY_MODE_PETSCII;
  }

  s_active = NULL;  /* stop spy terminal feed */

  /* If user is logged in, update and save their record */
  if (s->user_id != 0) {
    s->user.calls++;
    /* Accrue this session's online minutes into USR.DAY (fail open). Skipped
     * for local sysop and when MINS/DAY was never armed (sentinel hours). */
    if (!s->is_local && s_session_start.hours != 0xFF) {
      usr_day_record_t day;
      bbs_err_t de = usrday_load(s->user_id, &day, bbs_cfg.device_system);
      if (de == BBS_OK || de == BBS_ENOTFOUND) {
        clock_tod_t now;
        u16 add;
        clock_read(&now);
        add = (u16)(clock_elapsed(&s_session_start, &now) / 60u);
        day_apply_reset(&day);
        if ((u16)(day.mins_today + add) < day.mins_today ||
            (u16)(day.mins_today + add) > 1440u)
          day.mins_today = 1440;
        else
          day.mins_today = (u16)(day.mins_today + add);
        (void)usrday_save(s->user_id, &day, bbs_cfg.device_system);
      }
    }
    /* Persist terminal settings */
    s->user.term_mode  = s->term_mode;
    s->user.term_width = s->term_width;
    s->user.term_rows  = s->term_rows;
    if (user_save(&s->user, bbs_cfg.device_system) != BBS_OK) {
      return BBS_EIO;
    }
  }

  return BBS_OK;
}
