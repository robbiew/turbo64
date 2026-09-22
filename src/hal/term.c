/**
 * TURBO/64 BBS — Terminal Detection & Capability Negotiation
 *
 * Presents a single menu at connect time. Width and ANSI caps are set
 * automatically by mode — no follow-up questions asked:
 *   PETSCII  → 40 col, no ANSI
 *   ANSI     → 80 col, CP437, color=YES
 *   ASCII    → 80 col
 */

#include "bbs/hal/term.h"
#include "bbs/hal/disk.h"
#include "bbs/session.h"
#include "bbs/term.h"
#include "bbs/net.h"
#include "bbs/cfg.h"
#include "bbs/hal/clock.h"
#include <string.h>
#include <conio.h>   /* getchx() — raw GETIN for the local console */

/* C64 keyboard buffer count (non-blocking check) */
#define KBD_COUNT (*(volatile u8 *)0xC6)

static void _term_puts(const session_t *s, const char *text);
static bool_t s_term_last_was_cr = FALSE;

static bool_t _term_accept_input(u8 ch, u8 *out)
{
    if (s_term_last_was_cr) {
        s_term_last_was_cr = FALSE;
        if (ch == '\n') return FALSE;
    }
    if (ch == '\r') s_term_last_was_cr = TRUE;
    *out = ch;
    return TRUE;
}

/**
 * _term_getc_blocking()
 * Read one byte from modem or keyboard into *out.
 * Blocks until a byte is available (used during terminal detection).
 *
 * For remote callers this enforces bbs_cfg.idle_timeout_mins and carrier
 * state — the session idle watchdog isn't armed until after detection, so
 * without this a caller who connects and never picks a terminal would sit
 * here forever. Returns FALSE (and drops carrier on idle) when the caller
 * should be disconnected; TRUE when a byte was read.
 */
static bool_t _term_getc_blocking(const session_t *s, u8 *out)
{
    if (s->is_local) {
        /* Local console: wait for keyboard (no idle timeout for the SysOp).
         * getchx() (GETIN) returns the pending key directly; getchar() (CHRIN)
         * would invoke the KERNAL line editor and wait for RETURN. */
        while (KBD_COUNT == 0) {
            /* busy-wait */
        }
        return _term_accept_input((u8)getchx(), out);
    } else {
        /* Remote modem: pump ACIA until byte available, idle, or hangup */
        u16 got;
        clock_tod_t mark, now;
        u16 limit = (u16)bbs_cfg.idle_timeout_mins * 60u;
        clock_read(&mark);
        while (1) {
            if (net_rx(out, 1, &got) == BBS_OK && got == 1) {
                if (_term_accept_input(*out, out)) {
                    return TRUE;
                }
            }
            if (net_state() != NET_CONNECTED) {
                return FALSE;   /* carrier dropped */
            }
            if (limit != 0) {
                clock_read(&now);
                if (clock_elapsed(&mark, &now) >= limit) {
                    _term_puts(s, "\r\nIDLE TOO LONG - BYE!\r\n");
                    net_disconnect();
                    return FALSE;
                }
            }
        }
    }
}

/**
 * _term_puts()
 * Output text (bypass session_emit for detection prompts).
 */
static void _term_puts(const session_t *s, const char *text)
{
    if (s->is_local) {
        printf("%s", text);
    } else {
        u16 sent;
        net_tx(text, (u16)strlen(text), &sent);
    }
}

/**
 * _term_drain()
 * Discard any bytes already waiting in the receive buffer.
 * Called before sending a detection prompt to ensure we read
 * the user's actual keypress, not leftover telnet/handshake bytes.
 */
static void _term_drain(const session_t *s)
{
    if (s->is_local) return;
    u8 ch;
    u16 got;
    while (net_rx(&ch, 1, &got) == BBS_OK && got == 1) {
        /* discard */
    }
}

/**
 * _term_mci()
 * Copy `in` to `out`, expanding %SN (BBS name) and %SO (SysOp name) to their
 * configured values, forced UPPERCASE. Used for the raw, pre-detection G.TERM
 * menu, sent before the terminal/charset is known. `out` must hold the result:
 * callers pass the 48-byte prompt buffer, and G.TERM lines + the longest name
 * (bbs_name 23 / sysop_name 19) stay well under that.
 */
static void _term_mci(const char *in, char *out)
{
    while (*in) {
        if (in[0] == '%' && in[1] == 'S' && (in[2] == 'N' || in[2] == 'O')) {
            const char *r = (in[2] == 'N') ? bbs_cfg.bbs_name : bbs_cfg.sysop_name;
            while (*r) {
                char c = *r++;
                *out++ = (c >= 'a' && c <= 'z') ? (char)(c - 0x20) : c;
            }
            in += 3;
        } else {
            *out++ = *in++;
        }
    }
    *out = '\0';
}

/**
 * _term_show_menu()
 *
 * Display the terminal selection menu from G.TERM on disk, then return the
 * prompt text so the caller can print it without a trailing newline.
 *
 * File format (SEQ):
 *   - Each line is displayed followed by \r\n.
 *   - The last line is the prompt — returned in prompt_buf (not printed here).
 *
 * If G.TERM is not found or unreadable, fills prompt_buf with the built-in
 * fallback and prints the built-in menu lines.
 *
 * Returns TRUE if file was used, FALSE if fallback was used.
 */
static bool_t _term_show_menu(const session_t *s, char *prompt_buf, u8 prompt_len)
{
    char line_a[80], line_b[80];
    char *prev = line_a, *curr = line_b, *tmp;
    i16 got;
    bbs_err_t err;
    bool_t prev_had_cr;

    err = cfg_send_drive_init(bbs_cfg.device_gfiles, bbs_cfg.init_gfiles);
    if (err != BBS_OK) goto fallback;

    err = disk_open(bbs_cfg.device_gfiles, bbs_cfg.drive_gfiles, "G.TERM", DISK_READ);
    if (err != BBS_OK) goto fallback;

    got = disk_gets(prev, sizeof(line_a));
    if (got <= 0) { disk_close(); goto fallback; }

    /* Strip trailing CR/LF; track if line ended with CR (CRLF file artifact). */
    /* got>0 is redundant after the guard above; kept as defensive style. */
    // cppcheck-suppress knownConditionTrueFalse
    prev_had_cr = (got > 0 && (u8)prev[got - 1] == '\r');
    while (got > 0 && ((u8)prev[got-1] == '\r' || (u8)prev[got-1] == '\n'))
        prev[--got] = '\0';

    while (1) {
        got = disk_gets(curr, sizeof(line_b));
        if (got <= 0) break;

        /* Skip orphaned LF that is the second byte of a CRLF pair. */
        if (prev_had_cr && got == 1 && (u8)curr[0] == '\n') {
            prev_had_cr = FALSE;
            continue;
        }

        /* prev is not last — display it with \r\n. Reuse prompt_buf as MCI
         * scratch here; the prompt (last line) overwrites it before return. */
        _term_mci(prev, prompt_buf);
        _term_puts(s, prompt_buf);
        _term_puts(s, "\r\n");

        /* swap buffers; strip terminator from new prev */
        tmp = prev; prev = curr; curr = tmp;
        prev_had_cr = ((u8)prev[got - 1] == '\r');   /* got > 0: guaranteed by break above */
        while (got > 0 && ((u8)prev[got-1] == '\r' || (u8)prev[got-1] == '\n'))
            prev[--got] = '\0';
    }

    disk_close();
    /* prev is the last line — the prompt */
    _term_mci(prev, prompt_buf);
    return TRUE;

fallback:
    /* G.TERM unreadable (disk error): show a minimal built-in prompt. */
    strncpy(prompt_buf, "GRAPHICS 1-3 (1): ", (int)prompt_len - 1);
    prompt_buf[prompt_len - 1] = '\0';
    return FALSE;
}

/**
 * term_detect_backspace()
 *
 * Present a graphics selection menu and set all terminal fields based on
 * user choice. Width and ANSI caps are fixed per mode — no follow-up prompts.
 *
 *   [1] PETSCII  → 40 col, no ANSI color/graphics
 *   [2] ANSI     → 80 col, CP437, color + IBM graphics
 *   [3] ASCII    → 80 col, no color/graphics
 *
 * Default (RETURN or invalid): PETSCII
 */
term_mode_t term_detect_backspace(session_t *s)
{
    // cppcheck-suppress variableScope
    u8 ch;
    u8 tries;
    char prompt[48];

    if (s->is_local) {
        s->term_mode = TERM_PETSCII;
        s->term_width = 40;
        s->linefeed_mode = FALSE;
        s->ansi_color = FALSE;
        s->ansi_graphics = FALSE;
        return TERM_PETSCII;
    }

    /* Drain any connect-banner or telnet-negotiation bytes that arrived
     * before we started reading.  At high baud (38400) the "[CONNECTED]"
     * banner and IAC exchange can arrive in < 5ms; spin briefly then
     * drain a second time so we don't mistake those bytes for user input. */
    _term_drain(s);
    { volatile u16 i; for (i = 0; i < 8000; i++); }
    _term_drain(s);

    for (tries = 0; tries < 3; tries++) {
        _term_puts(s, "\r\n");
        _term_show_menu(s, prompt, sizeof(prompt));
        _term_puts(s, prompt);

        /* Drain any bytes that arrived while we were printing the prompt
         * (e.g. trailing telnet negotiation or echoed control codes). */
        _term_drain(s);
        if (!_term_getc_blocking(s, &ch)) {
            /* Carrier dropped or caller idled out — bail (carrier already
             * released). Fall back to a safe ASCII default. */
            s->term_mode = TERM_ASCII;
            s->term_width = 80;
            s->linefeed_mode = TRUE;
            s->ansi_color = FALSE;
            s->ansi_graphics = FALSE;
            return s->term_mode;
        }
        _term_puts(s, "\r\n");

        if (ch == '1' || ch == '\r' || ch == '\n') {
            s->term_mode = TERM_PETSCII;
            s->term_width = 40;
            s->linefeed_mode = FALSE;
            s->ansi_color = FALSE;
            s->ansi_graphics = FALSE;
            return s->term_mode;
        } else if (ch == '2') {
            s->term_mode = TERM_ANSI_CP437;
            s->term_width = 80;
            s->linefeed_mode = TRUE;
            s->ansi_color = TRUE;
            s->ansi_graphics = TRUE;
            return s->term_mode;
        } else if (ch == '3') {
            s->term_mode = TERM_ASCII;
            s->term_width = 80;
            s->linefeed_mode = TRUE;
            s->ansi_color = FALSE;
            s->ansi_graphics = FALSE;
            return s->term_mode;
        }

        _term_puts(s, "INVALID SELECTION.\r\n");
    }

    /* 3 failed attempts — likely a bot or scanner */
    _term_puts(s, "\r\nCOME BACK WHEN YOU'RE NOT A BOT...\r\n\r\n");
    net_disconnect();
    s->term_mode = TERM_ASCII;
    s->term_width = 80;
    s->linefeed_mode = TRUE;
    s->ansi_color = FALSE;
    s->ansi_graphics = FALSE;
    return s->term_mode;
}

/**
 * term_detect_display_file()
 *
 * After terminal detection, display the login welcome file.
 * Uses session_display_file() with fallback chain:
 *   G.LOGIN <mode> <width> → G.LOGIN <mode> → G.LOGIN <width> → G.LOGIN
 */
void term_detect_display_file(const session_t *s)
{
    if (!s) return;
    if (session_display_file(s, 'g', "login") != BBS_OK) {
        _term_puts(s, "\r\nTURBO/64 BBS\r\n\r\n");
    }
}

/**
 * term_detect_all()
 *
 * Orchestrate the full terminal detection sequence:
 *   1. Display graphics menu — user picks mode; width/caps set automatically
 *   2. Display login welcome file
 *
 * Populates all session terminal fields:
 *   - term_mode, term_width, ansi_color, ansi_graphics, linefeed_mode
 *
 * For local console sessions, skips all prompts and forces:
 *   - term_mode = TERM_PETSCII, term_width = 40, linefeed_mode = FALSE
 */
void term_detect_all(session_t *s)
{
    if (!s) return;
    s_term_last_was_cr = FALSE;

    /* Local console: force PETSCII/40-column, skip detection */
    if (s->is_local) {
        s->term_mode = TERM_PETSCII;
        s->petscii_lower = TRUE;   /* local console renders mixed-case (text charset) */
        s->term_width = 40;
        s->linefeed_mode = FALSE;
        s->ansi_color = FALSE;
        s->ansi_graphics = FALSE;
        return;
    }

    term_detect_backspace(s);    /* Menu: sets mode, width, and caps */
    term_detect_display_file(s); /* Show g.login <mode> <width> */
    s->petscii_lower = (s->term_mode == TERM_PETSCII);
}
