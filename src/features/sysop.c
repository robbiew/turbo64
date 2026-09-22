/**
 * src/features/sysop.c — WFC (Waiting For Caller) sysop screen
 *
 * Implements a C*BASE v3.3.8-inspired WFC display for the TURBO/64 BBS.
 *
 * Screen layout during waiting-for-caller (C64, 40×25 PETSCII):
 *
 *   Row  0: "HH:MM AM  [RVON BBS NAME          RVOF]"
 *   Row  1: "========================================"
 *   Row  2: " Users Total :  0   Msg Areas  :  0"
 *   Row  3: " Calls Today :  0   File Areas :  0"
 *   Row  4: " Posts Today :  0   Active Poll:  0"
 *   Row  5: " Guest Access: ON   Chat       : ON"
 *   Row  6: "========================================"
 *   Row  7: "          SYSOP IS <status msg>"
 *   Row  8: "========================================"
 *   Row  9: "[HH:MM AM] [RVON username     RVOF] ( 9600)"
 *   ...   (10 most-recent callers)
 *   Row 18: "[HH:MM AM] [RVON username     RVOF] ( 9600)"
 *   Row 19: "========================================"  (footer, bottom)
 *   Row 20: " F1 - ---            F5 - ---"
 *   Row 21: " F2 - ---            F6 - CHAT TOGGLE"
 *   Row 22: " F3 - ---            F7 - SET STATUS"
 *   Row 23: " F4 - REFRESH SCREEN F8 - QUIT"
 *   Row 24: (status alert line)
 *
 * During active session (F1 toggles footer visibility):
 *   Row  0: "HH:MM AM  [RVON BBS NAME          RVOF]"
 *   Row  1: "========================================"
 *   Row  2: "[HH:MM AM] [RVON CURRENT USER    RVOF] (NOW)"
 *   Rows 3-17: [HH:MM AM] [RVON prev users     RVOF] (XXXm/s)"
 *   Row 18: "[HH:MM AM] [RVON CURRENT USER    RVOF] (9600) (opt footer)"
 *   Row 19: "========================================"
 *   Row 20: " F1 - TOGGLE FOOTER F2 - VIEW LOG"
 *   Row 21: " F3 - TOGGLE GUEST  F4 - LOCAL LOGON"
 *   Row 22: " F5 - SET TIME/DATE F6 - CHAT TOGGLE"
 *   Row 23: " F7 - REFRESH       F8 - CARRIER"
 *   Row 24: " CR - SYSOP LOGON   Q - QUIT/RESTART"
 */

#include <stdio.h>
#include <string.h>
#include <conio.h>

#include "bbs/sysop.h"
#include "bbs/sstatus.h"
#include "bbs/syscnt.h"
#include "bbs/callers.h"
#include "bbs/cfg.h"
#include "bbs/config.h"
#include "bbs/net.h"
#include "bbs/users.h"
#include "bbs/boards.h"
#include "bbs/file_areas.h"
#include "bbs/votes.h"
#include "bbs/hal/disk.h"
#include "bbs/hal/clock.h"
#include "bbs/version.h"
#include "bbs/spy80.h"
#include "bbs/overlay.h"


/* ── PETSCII color/control codes ───────────────────────────── */
#define P_WHITE   "\x05"   /* white text */
#define P_LGREEN  "\x99"   /* light green */
#define P_YELLOW  "\x9e"   /* yellow */
#define P_CYAN    "\x9f"   /* cyan */
#define P_LBLUE   "\x9a"   /* light blue */
#define P_LGRAY   "\x9b"   /* light gray */
#define P_GREEN   "\x1e"   /* green */
#define P_RVON    "\x12"   /* reverse video on */
#define P_RVOF    "\x92"   /* reverse video off */
#define P_CLR     "\x93"   /* clear screen */
#define P_HOME    "\x13"   /* cursor home (top-left) */
#define P_GFX     "\x8e"   /* uppercase/graphics charset */

/* C64 keyboard buffer count — non-blocking key check */
#define C64_KBD_COUNT (*(volatile u8 *)0xC6)

/* F-key PETSCII codes */
#define KEY_F1  ((char)0x85)
#define KEY_F2  ((char)0x89)
#define KEY_F3  ((char)0x86)
#define KEY_F4  ((char)0x8A)
#define KEY_F5  ((char)0x87)
#define KEY_F6  ((char)0x8B)
#define KEY_F7  ((char)0x88)
#define KEY_F8  ((char)0x8C)

/* ── Module globals ─────────────────────────────────────────── */
wfc_state_t wfc;

/* ── Internal helpers ───────────────────────────────────────── */

/* ── WFC overlay load helper + core stubs ──────────────────────────────────
 * OVL_WFC (bank 2) shares $9D80-$BFFF with OVL_MSGS (bank 1).
 * load_ovl_wfc() is a no-op when the overlay is already resident.
 *
 * On failure, ovl_wfc_loaded is left FALSE (not forced TRUE — that was the
 * bug: a discarded load error let every caller run into whatever stale
 * overlay still sat at $9700) so the next WFC call retries the load. The
 * four thin wrappers below all skip their _impl() call when the load
 * fails, since that call would otherwise execute into the unloaded/wrong
 * overlay region. This is the local sysop console (no session to report
 * to), so the error goes straight to the screen via printf — WFC's own
 * draw code does the same (see wfc_display_impl). */
static bbs_err_t load_ovl_wfc(void)
{
    if (!wfc.ovl_wfc_loaded) {
        if (disk_load_overlay(P"OVL_WFC") != BBS_OK) {
            printf("\nERROR: OVL_WFC LOAD FAILED.\n");
            return BBS_EIO;
        }
        wfc.ovl_wfc_loaded = TRUE;
    }
    return BBS_OK;
}
void       wfc_init(void)      { if (load_ovl_wfc() == BBS_OK) wfc_init_impl();    }
void       wfc_display(void)   { if (load_ovl_wfc() == BBS_OK) wfc_display_impl(); }
bbs_err_t  wfc_update(void)    { bbs_err_t e = load_ovl_wfc(); return (e == BBS_OK) ? wfc_update_impl() : e; }
void       wfc_reload(void)    { load_ovl_wfc();                          }

/* ── WFC overlay: draw/render/RTC/datetime — loaded on demand ─────────── */
#pragma code(wfc_code)
#pragma data(wfc_data)
#pragma bss(wfc_bss)

#if WFC_LOG_SIZE > 0
/**
 * fmt_duration()
 * Format seconds as compact string: "XXs", "XXm", or "XXh". buf >= 5 bytes.
 * Only used by draw_log(), which is also gated on WFC_LOG_SIZE.
 */
static void fmt_duration(u16 secs, char *buf)
{
    if (secs < 3600) {
        u16 m = secs / 60;
        u16 s = secs % 60;
        if (m > 0)
            sprintf(buf, "%uM", (unsigned)m);
        else if (s > 0)
            sprintf(buf, "%uS", (unsigned)s);
        else
            sprintf(buf, "0");
    } else {
        sprintf(buf, "%uH", (unsigned)(secs / 3600));
    }
}
#endif

/**
 * fmt_time()
 * Format CIA1 BCD TOD as "HH:MM AM" (8 chars + NUL).
 * buf must be >= 9 bytes.
 */
static void fmt_time(const clock_tod_t *t, char *buf)
{
    u8 h  = (u8)(((t->hours >> 4) & 0x01) * 10 + (t->hours & 0x0F));
    u8 m  = (u8)(((t->mins  >> 4) & 0x07) * 10 + (t->mins  & 0x0F));
    u8 pm = (t->hours & 0x80) ? 1 : 0;
    if (h == 0) h = 12;   /* CIA1: midnight/noon stored as 12 */
    buf[0] = (h >= 10) ? (char)('0' + h / 10) : ' ';
    buf[1] = (char)('0' + h % 10);
    buf[2] = ':';
    buf[3] = (char)('0' + m / 10);
    buf[4] = (char)('0' + m % 10);
    buf[5] = ' ';
    buf[6] = pm ? 'P' : 'A';   /* uppercase for PETSCII graphics mode */
    buf[7] = 'M';
    buf[8] = '\0';
}

/**
 * fmt_uptime()
 * Format elapsed seconds since boot as "HHhMMm" (6 chars + NUL).
 * buf must be >= 7 bytes.
 */
static void fmt_uptime(char *buf)
{
    clock_tod_t now;
    u16 elapsed, h, m;
    clock_read(&now);
    elapsed = clock_elapsed(&wfc.boot_time, &now);
    h = elapsed / 3600;
    m = (elapsed % 3600) / 60;
    buf[0] = (char)('0' + (h / 10) % 10);
    buf[1] = (char)('0' + h % 10);
    buf[2] = 'H';
    buf[3] = (char)('0' + m / 10);
    buf[4] = (char)('0' + m % 10);
    buf[5] = 'M';
    buf[6] = '\0';
}

/* ── Screen sections ────────────────────────────────────────── */

/** Rows 0–1: blank row then time | TURBO/64 BBS (green reverse) | date */
static void draw_header(const clock_tod_t *t)
{
    char tbuf[9];
    const char *tstr;
    fmt_time(t, tbuf);
    tstr = (tbuf[0] == ' ') ? tbuf + 1 : tbuf;
    /* Both fields are padded to 8 so the row is always 39 columns.  It is
     * redrawn in place (P_HOME, no clear), so a narrower row leaves the
     * previous frame's trailing character stranded: at the 12:59 -> 1:00
     * rollover the hour loses a digit and column 38 kept the year's last
     * digit, rendering "07/30/26" as "07/30/266". */
    printf("\n" P_YELLOW " %-8s "
           P_LGREEN P_RVON "    TURBO/64 BBS    " P_RVOF
           P_YELLOW " %-8s\n",
           tstr, wfc.date[0] ? wfc.date : "");
}


/** One row of the two-column stats grid (both numeric) */
static void draw_stat2(const char *l1, u16 v1,
                       const char *l2, u16 v2)
{
    printf(P_LGREEN " %-12s" P_WHITE ":%3u  "
           P_LGREEN "%-11s" P_WHITE ":%3u\n",
           l1, v1, l2, v2);
}

/** One row with a string value on each column */
static void draw_stat2s(const char *l1, const char *v1,
                        const char *l2, const char *v2)
{
    printf(P_LGREEN " %-12s" P_WHITE ":%3s  "
           P_LGREEN "%-11s" P_WHITE ":%3s\n",
           l1, v1, l2, v2);
}

/** Rows 2–4: statistics grid — 3 rows */
static void draw_stats(void)
{
    u8 users = user_count(bbs_cfg.device_system);
    u8 areas = board_count(bbs_cfg.device_msgs);
    u8 files = file_area_count(bbs_cfg.device_files);
    draw_stat2("USERS TOTAL", (u16)users,      "MSG AREAS",  (u16)areas);
    draw_stat2("CALLS TODAY", wfc.calls_today, "FILE AREAS", (u16)files);
    printf(P_LGREEN " %-12s" P_WHITE ":%3u  "
           P_LGREEN "%-11s" P_WHITE ":%3s\n",
           "POSTS TODAY", (unsigned)wfc.posts_today,
           "SYSOP CHAT",  wfc.chat_enabled ? "Y" : "N");
}


/** Rows 11–17: caller activity log */
static void draw_log(void)
{
#if WFC_LOG_SIZE > 0
    u8 i, idx;
    char tbuf[9];

    for (i = 0; i < WFC_LOG_SIZE; i++) {
        /* Map display row → circular buffer index (oldest first) */
        if (wfc.log_count == WFC_LOG_SIZE) {
            idx = (wfc.log_head + i) % WFC_LOG_SIZE;
        } else {
            idx = i;
        }

        if (i < wfc.log_count) {
            wfc_log_entry_t *e = &wfc.log[idx];
            char dbuf[5];
            fmt_time(&e->time, tbuf);
            fmt_duration(e->duration, dbuf);
            printf(P_YELLOW "[%-8s]" P_WHITE " "
                   P_RVON "%-15s" P_RVOF
                   P_LGRAY " %4u %3s\n" P_WHITE,
                   tbuf, e->handle, (unsigned)e->baud, dbuf);
        } else {
            /* Empty slot — just a blank line */
            printf("\n");
        }
    }
#endif
}

/** Rows 6–8: cyan $C0 top + centered YELLOW"SYSOP IS " CYAN<msg> + cyan $C0 bottom */
static void draw_sysop_msg(void)
{
    const char *msg = wfc.chat_msg[0] ? wfc.chat_msg : "PLAYING ATARI 2600";
    char lspc[20];
    char umsg[21];           /* uppercased copy: local console has no lowercase glyphs */
    u8 mlen = 0, total, lpad;
    const char *p = msg;
    while (*p && mlen < 20) {
        umsg[mlen] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 0x20) : *p;
        mlen++; p++;
    }
    umsg[mlen] = '\0';
    total = (u8)(9 + mlen);           /* "SYSOP IS " is 9 chars */
    lpad  = (total < 39) ? (u8)((39 - total) / 2) : 0;
    if (lpad) { memset(lspc, ' ', lpad); lspc[lpad] = '\0'; } else lspc[0] = '\0';
    printf(P_CYAN
           "\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0"
           "\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0"
           "\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0"
           "\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\n");
    printf(P_WHITE "%s" P_YELLOW "SYSOP IS " P_CYAN "%.20s\n", lspc, umsg);
    printf(P_CYAN
           "\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0"
           "\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0"
           "\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0"
           "\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\n"
           P_WHITE);
}

/* Most-recent callers shown on the WFC screen (rows 9–18). */
#define WFC_CALLERS_SHOWN 10

/** Rows 9–18: ten most-recent callers, read raw from disk (no struct parsing).
 * Line layout: [0-4]=date [6-10]=HH:MM [12]=A/P [14-28]=handle [30-34]=baud */
static void draw_callers_log(void)
{
    char lns[WFC_CALLERS_SHOWN][44];   /* raw lines on STACK — 440 bytes, not BSS */
    u8 head = 0, count = 0, i;

    if (cfg_send_drive_init(bbs_cfg.device_system, bbs_cfg.init_system) == BBS_OK &&
        disk_open(bbs_cfg.device_system, bbs_cfg.drive_system,
                  CALLERS_FILE, DISK_READ) == BBS_OK) {
        while (!disk_eof()) {
            i16 len = disk_gets(lns[head], 43);
            if (len >= CALLERS_LINE_MIN) {
                head = (u8)((head + 1) % WFC_CALLERS_SHOWN);
                if (count < WFC_CALLERS_SHOWN) count++;
            }
        }
        disk_close();
    }

    for (i = 0; i < WFC_CALLERS_SHOWN; i++) {
        if (i < count) {
            char dur[4];
            u8 idx = (count < WFC_CALLERS_SHOWN) ? i : (u8)((head + i) % WFC_CALLERS_SHOWN);
            /* Parse duration if line is new (41-char) format */
            if (lns[idx][40] && lns[idx][35] == ' ') {
                u16 secs = 0, m;
                u8 k;
                for (k = 36; k <= 40; k++)
                    secs = (u16)(secs * 10 + (u8)(lns[idx][k] - '0'));
                m = (u16)(secs / 60u);
                if (m < 100u) {
                    dur[0] = (m >= 10u) ? (char)('0' + m / 10u) : ' ';
                    dur[1] = (char)('0' + m % 10u);
                    dur[2] = 'M';
                } else {
                    u16 h = m / 60u;
                    dur[0] = (h >= 10u) ? (char)('0' + (h/10u)%10u) : ' ';
                    dur[1] = (char)('0' + h % 10u);
                    dur[2] = 'H';
                }
                dur[3] = '\0';
            } else {
                dur[0] = ' '; dur[1] = '-'; dur[2] = '-'; dur[3] = '\0';
            }
            /* Explicit NUL-terminated copies — avoids %.Ns truncation unreliability */
            char baud5[6], hdl[16];
            memcpy(baud5, lns[idx] + 30, 5); baud5[5] = '\0';
            memcpy(hdl, lns[idx] + 14, 15);  hdl[15]  = '\0';
            printf(P_LGREEN "[%c%c:%c%c %cM]" P_WHITE " %-15s"
                   P_LGREEN " %s %s\n" P_WHITE,
                   lns[idx][6], lns[idx][7], lns[idx][9],
                   lns[idx][10], lns[idx][12],
                   hdl, baud5, dur);
        } else {
            printf("\n");
        }
    }
}

/** Rows 19–23: F-key footer — PETSCII $C0 separator, green reverse-video labels */
static void draw_footer(void)
{
    printf(P_LGREEN
           "\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0"
           "\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0"
           "\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0"
           "\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0"
           "\n" P_WHITE);
    printf(" " P_LGREEN P_RVON "F1" P_RVOF P_WHITE " ---             "
           " " P_LGREEN P_RVON "F5" P_RVOF P_WHITE " ---\n");
    printf(" " P_LGREEN P_RVON "F2" P_RVOF P_WHITE " ---             "
           " " P_LGREEN P_RVON "F6" P_RVOF P_WHITE " CHAT TOGGLE\n");
    printf(" " P_LGREEN P_RVON "F3" P_RVOF P_WHITE " ---             "
            " " P_LGREEN P_RVON "F7" P_RVOF P_WHITE " SET STATUS\n");
    printf(" " P_LGREEN P_RVON "F4" P_RVOF P_WHITE " REFRESH SCREEN  "
             " " P_LGREEN P_RVON "F8" P_RVOF P_WHITE " QUIT\n");
}

static void clear_user_footer(void)
{
    /* Direct screen-RAM clear of rows 20-24 (offsets 800-999). Using KERNAL
     * printf here scrolls the whole screen: writing 40 chars to row 24 wraps the
     * cursor past the bottom row, and this runs once/second during login — which
     * scrolled the spy login zone (rows 0-19) off the screen. */
    volatile u8 *scrn = (volatile u8 *)0x0400u;
    u16 i;
    for (i = 800u; i < 1000u; i++) scrn[i] = 0x20u;
    gotoxy(0, 20);
}
static void prompt_chat_msg(void)
{
    u8 len = 0;
    memset(wfc.chat_msg, 0, sizeof(wfc.chat_msg));
    /* Clear row 7 then re-home cursor: '\r' (CHR$13) is a NEWLINE on the C64,
     * not a column-0 return — relying on it dropped the prompt to row 8. */
    gotoxy(0, 7);
    printf(P_WHITE "                                       ");
    gotoxy(0, 7);
    printf("SYSOP IS: ");
    for (;;) {
        char ch;
        while ((ch = (char)getchx()) == 0) {}
        if (ch == '\r' || ch == '\n') break;
        if ((ch == 8 || ch == 20) && len > 0) {   /* BS / DEL */
            len--;
            wfc.chat_msg[len] = '\0';
            printf("\x14");   /* PETSCII: delete character to left */
        } else if (ch >= 0x20 && ch < 0x80 && len < 20) {
            wfc.chat_msg[len++] = ch;
            wfc.chat_msg[len] = '\0';
            printf("%c", ch);
        }
    }
    sstatus_save(wfc.chat_msg);   /* persist across reboots */
}

/** Row 24: status alert — positions cursor first to avoid landing on wrong row */
static void draw_alert(const char *msg)
{
    u8 i;
    printf(P_HOME);
    for (i = 0; i < 24; i++) printf("\n");
    printf(P_CYAN "%-39s" P_WHITE, msg);
}

/* ── Public API ─────────────────────────────────────────────── */

/**
 * rtc_try_read()
 * Attempt to read date/time from an Ultimate II+ / Ultimate 64 RTC
 * via the UCI (Ultimate Command Interface).
 *
 * UCI registers (cartridge I/O, always at $DF1C-$DF1F):
 *   $DF1C = CTRL  (W: push/accept/abort  R: state bits + data/status flags)
 *   $DF1D = CMD   (W: write command bytes  R: $C9 when UCI idle = present)
 *   $DF1E = DATA  (R: response data bytes)
 *   $DF1F = STAT  (R: status string bytes)
 *
 * Protocol:
 *   1. Write target byte to $DF1D, then command byte to $DF1D
 *   2. Write $01 to $DF1C to push/execute the command
 *   3. Spin on $DF1C bits[5:4] == $10 (BUSY)
 *   4. While bit7 of $DF1C is set, read data bytes from $DF1E
 *   5. Write $02 to $DF1C to acknowledge / return to idle
 *
 * DOS target (id=1), GET_TIME command ($26), format 0:
 *   Response: "YYYY/MM/DD HH:MM:SS"  (19 ASCII bytes, 24h)
 *
 * Returns 1 on success, 0 if UCI not present or parse failed.
 * On success: *out_yr (2-digit), *out_mo, *out_dy, *out_hr (12h),
 *             *out_mn, *out_pm (0=AM 1=PM) are set.
 */

/* UCI register addresses in C64 memory space */
#define UCI_CTRL  (*(volatile u8 *)0xDF1C)
#define UCI_CMD   (*(volatile u8 *)0xDF1D)
#define UCI_DATA  (*(volatile u8 *)0xDF1E)
#define UCI_STAT  (*(volatile u8 *)0xDF1F)

/* UCI control bits */
#define UCI_PUSH_CMD     0x01  /* write: execute queued command */
#define UCI_ACCEPT_DATA  0x02  /* write: accept/ack, return to idle */
#define UCI_STATE_MASK   0x30  /* read: command state */
#define UCI_STATE_BUSY   0x10  /* read: processing */

/* UCI targets and commands */
#define UCI_TARGET_DOS   0x01  /* DOS (Ultimate filesystem) target */
#define DOS_CMD_GET_TIME 0x26  /* GET_TIME command */
#define DOS_TIME_FMT0    0x00  /* format 0: "YYYY/MM/DD HH:MM:SS" */

/* UCI identifier — reading $DF1D returns this when UCI is idle */
#define UCI_IDENTIFIER   ((u8)0xC9)

/* Brief settle (~hundreds of cycles) between UCI register accesses. Back-to-back
 * writes to the command register are layout-fragile: depending on code
 * placement they arrive too fast for the Ultimate to register and the command
 * silently fails to execute, leaving the RTC undetected. */
static void uci_settle(void)
{
    volatile u8 s;
    for (s = 0; s < 90u; s++) { }
}

/* DS12C887 RTC register layout */
#define RTC_REG_SECONDS  0x00
#define RTC_REG_MINUTES  0x02
#define RTC_REG_HOURS    0x04
#define RTC_REG_DAY      0x07
#define RTC_REG_MONTH    0x08
#define RTC_REG_YEAR     0x09
#define RTC_REG_STATUS_B 0x0B
#define RTC_REG_STATUS_D 0x0D
#define RTC_STATUS_B_DM  0x04
#define RTC_STATUS_B_24H 0x02

/* $DE00 = ACIA (SwiftLink), $DF00 = U64 expansion — skip both */
static const u16 rtc_bases[] = { 0xD500, 0xD600, 0xD700 };

static u8 bcd_to_bin(u8 bcd)
{
    return (u8)((bcd >> 4) * 10 + (bcd & 0x0F));
}

static u8 rtc_read_reg(u16 base, u8 reg)
{
    (*(volatile u8 *)base) = reg;
    return *(volatile u8 *)(base + 1);
}

static u8 rtc_read_ds12c887(u8 *out_yr, u8 *out_mo, u8 *out_dy,
                            u8 *out_hr, u8 *out_mn, u8 *out_pm)
{
    u8 i;

    for (i = 0; i < (u8)(sizeof(rtc_bases) / sizeof(rtc_bases[0])); ++i) {
        u16 base = rtc_bases[i];
        u8 status_b = rtc_read_reg(base, RTC_REG_STATUS_B);
        u8 status_d = rtc_read_reg(base, RTC_REG_STATUS_D);
        u8 raw_sec  = rtc_read_reg(base, RTC_REG_SECONDS);
        u8 raw_min  = rtc_read_reg(base, RTC_REG_MINUTES);
        u8 raw_hour = rtc_read_reg(base, RTC_REG_HOURS);
        u8 raw_day  = rtc_read_reg(base, RTC_REG_DAY);
        u8 raw_mo   = rtc_read_reg(base, RTC_REG_MONTH);
        u8 raw_yr   = rtc_read_reg(base, RTC_REG_YEAR);
        u8 hour_24;
        u8 sec, mn, mo, dy, yr;

        if ((status_d & 0x80) == 0) {
            continue;
        }

        if (status_b & RTC_STATUS_B_DM) {
            sec = raw_sec;
            mn  = raw_min;
            dy  = raw_day;
            mo  = raw_mo;
            yr  = raw_yr;
        } else {
            sec = bcd_to_bin(raw_sec);
            mn  = bcd_to_bin(raw_min);
            dy  = bcd_to_bin(raw_day);
            mo  = bcd_to_bin(raw_mo);
            yr  = bcd_to_bin(raw_yr);
        }

        if (status_b & RTC_STATUS_B_24H) {
            if (status_b & RTC_STATUS_B_DM) {
                hour_24 = raw_hour;
            } else {
                hour_24 = bcd_to_bin((u8)(raw_hour & 0x7F));
            }
            if (hour_24 > 23) {
                continue;
            }
            if (hour_24 == 0) {
                *out_hr = 12;
                *out_pm = 0;
            } else if (hour_24 < 12) {
                *out_hr = hour_24;
                *out_pm = 0;
            } else if (hour_24 == 12) {
                *out_hr = 12;
                *out_pm = 1;
            } else {
                *out_hr = (u8)(hour_24 - 12);
                *out_pm = 1;
            }
        } else {
            u8 pm;
            if (status_b & RTC_STATUS_B_DM) {
                hour_24 = (u8)(raw_hour & 0x7F);
                pm = (u8)((raw_hour & 0x80) ? 1 : 0);
            } else {
                hour_24 = bcd_to_bin((u8)(raw_hour & 0x7F));
                pm = (u8)((raw_hour & 0x80) ? 1 : 0);
            }
            /* VICE's DS12C887 reports 12-hour mode (status B 24H bit clear) but
             * stores 24-hour values (e.g. 0x13 = 1 PM), so a strict 1..12 check
             * rejects every afternoon/evening hour.  Accept a 24-hour value and
             * fold it into 12-hour + AM/PM. */
            if (hour_24 == 0) {                 /* 24h midnight */
                *out_hr = 12; *out_pm = 0;
            } else if (hour_24 <= 12) {         /* normal 1..12 (use PM flag) */
                *out_hr = hour_24; *out_pm = pm;
            } else if (hour_24 <= 23) {         /* 24h 13..23 → 1..11 PM */
                *out_hr = (u8)(hour_24 - 12); *out_pm = 1;
            } else {
                continue;                       /* truly invalid */
            }
        }

        if (sec > 59 || mn > 59 || mo < 1 || mo > 12 || dy < 1 || dy > 31 ||
            yr > 99 || yr < 20) {
            continue;
        }

        *out_yr = yr;
        *out_mo = mo;
        *out_dy = dy;
        *out_mn = mn;
        return 1;
    }

    return 0;
}

static u8 rtc_try_read(u8 *out_yr, u8 *out_mo, u8 *out_dy,
                       u8 *out_hr, u8 *out_mn, u8 *out_pm)
{
    u8   ok = 0;

    /* Settle delay before commanding the UCI. After the REU activity at boot and
     * the WFC overlay load, the Ultimate command engine needs a brief moment;
     * issuing GET_TIME immediately produces no response and the RTC read falls
     * through to manual entry. A debug printf here incidentally fixed it — this
     * is the deterministic equivalent. */
    { volatile u16 d; for (d = 0; d < 8000u; d++) { } }

    /* Detect UCI: reading CMD register returns $C9 when idle.
     * The UCI ($DF1C-$DF1F) is real hardware (U64). Under VICE there is no UCI,
     * but a REU sits at $DF00, and probing $DF1x there reads/writes the REU's
     * I/O-2 space — which crashes the BBS during boot. The hazard is specific
     * to VICE, so probe the UCI on every platform EXCEPT VICE: AUTO and U64
     * (real hardware) get the U64 RTC; only MODEM_TYPE=VICE falls straight
     * through to the emulated DS12C887 RTC ($D500/$D600/$D700). Gating on the
     * modem type is correct because that is how the build distinguishes VICE
     * (data/config.vice forces MODEM_TYPE=VICE) from hardware (AUTO/U64). */
    if (bbs_cfg.modem_type != MODEM_VICE && UCI_CMD == UCI_IDENTIFIER) {
        u8  buf[22];
        u8  i = 0;
        u16 dr;

        /* Queue command bytes (target=DOS, cmd=GET_TIME, param=format0), then
         * push. Settle between each write so they register regardless of code
         * layout/CPU timing. */
        UCI_CMD = UCI_TARGET_DOS;    uci_settle();
        UCI_CMD = DOS_CMD_GET_TIME;  uci_settle();
        UCI_CMD = DOS_TIME_FMT0;     uci_settle();
        UCI_CTRL = UCI_PUSH_CMD;     uci_settle();

        /* Wait for the command to COMPLETE (leave the BUSY state) before reading.
         * "Not busy" means the full response is ready, so the read below cannot
         * outrun the data. Polling the data-available flag (bit7) instead let the
         * read start when only the first byte was ready and misread later bytes
         * (notably the year). The settle after PUSH above ensures the command has
         * entered BUSY before we poll, so we don't exit prematurely. */
        dr = 50000u;
        while ((UCI_CTRL & UCI_STATE_MASK) == UCI_STATE_BUSY) {
            if (--dr == 0) break;
        }

        /* Read response bytes while data remains available. */
        while ((UCI_CTRL & 0x80) && i < (u8)(sizeof(buf) - 1)) {
            buf[i++] = UCI_DATA;
        }
        buf[i] = '\0';

        /* Acknowledge / return UCI to idle */
        UCI_CTRL = UCI_ACCEPT_DATA;

        /* Parse "YYYY/MM/DD HH:MM:SS" */
        /* Format: buf[0..3]=year, [5..6]=month, [8..9]=day,
                   [11..12]=hour, [14..15]=min, [17..18]=sec */
        if (i >= 19) {
            /* 2-digit year straight from the last two digits of YYYY — avoids a
             * 16-bit *1000 multiply that mis-generated the year under oscar64. */
            u8  yr2  = (u8)((buf[2]-'0')*10 + (buf[3]-'0'));
            u8  mo   = (u8)((buf[5]-'0')*10 + (buf[6]-'0'));
            u8  dy   = (u8)((buf[8]-'0')*10 + (buf[9]-'0'));
            u8  hh   = (u8)((buf[11]-'0')*10 + (buf[12]-'0'));
            u8  mn   = (u8)((buf[14]-'0')*10 + (buf[15]-'0'));

            /* Sanity check raw values; yr < 20 = unset/factory-default clock */
            if (mo >= 1 && mo <= 12 &&
                dy >= 1 && dy <= 31 &&
                hh <= 23 && mn <= 59 &&
                yr2 >= 20) {

                *out_yr = yr2;   /* 2-digit year */
                *out_mo = mo;
                *out_dy = dy;
                *out_mn = mn;

                /* Convert 24h → 12h + AM/PM */
                if (hh == 0)       { *out_hr = 12; *out_pm = 0; }
                else if (hh < 12)  { *out_hr = hh; *out_pm = 0; }
                else if (hh == 12) { *out_hr = 12; *out_pm = 1; }
                else               { *out_hr = (u8)(hh - 12); *out_pm = 1; }
                ok = 1;
            }
        }
    }

    if (!ok) {
        ok = rtc_read_ds12c887(out_yr, out_mo, out_dy, out_hr, out_mn, out_pm);
    }

    return ok;
}

/**
 * read_uint()
 * Read a decimal number from keyboard (1–3 digits), blocking.
 * Returns the value, or 255 on empty/invalid input.
 */
static u8 read_uint(void)
{
    u8 val = 0;
    u8 n = 0;
    /* getch() (GETIN), not getchar() (CHRIN): CHRIN's early return on the 3rd
     * digit left the rest of the line + RETURN in its readback state, feeding
     * stray input into the next prompt (e.g. typing 2026 at YEAR). */
    for (;;) {
        char c = (char)getch();
        if (c == '\r' || c == '\n') {
            printf("\n");
            return (n > 0) ? val : 255;
        }
        if (c == '\x14' || c == 0x08 || c == 0x7f) {   /* PETSCII DEL / backspace */
            if (n > 0) { n--; val /= 10; printf("\x14"); }
            continue;
        }
        if (c >= '0' && c <= '9') {
            val = (u8)(val * 10 + (c - '0'));
            n++;
            printf("%c", c);
            if (n >= 3) { printf("\n"); return val; }
            continue;
        }
        /* non-digit: ignore */
    }
}

/* Low 16 bits of the KERNAL jiffy clock ($A1 mid, $A2 low), incremented 60x/sec
 * by the default KERNAL IRQ. (Read via a helper so two calls aren't folded into
 * one value — the count advances between reads.) */
static u16 jiffy16(void)
{
    return (u16)(((u16)(*(volatile u8 *)0x00A1) << 8) | *(volatile u8 *)0x00A2);
}

/* Wait up to ~2 seconds for a keypress (non-blocking GETIN poll); returns the
 * key, or 0 on timeout.  Valid here because this runs during boot, before
 * main_loop arms the Timer-B RX IRQ, so the KERNAL jiffy is still ticking. */
static char wait_key_2s(void)
{
    u16 start = jiffy16();
    for (;;) {
        char ch = (char)getchx();
        if (ch != 0) return ch;
        if ((u16)(jiffy16() - start) >= 120u) return 0;   /* ~2 s at 60 Hz */
    }
}

/* ── TOD rate calibration ─────────────────────────────────────────────────
 * The CIA must be told what frequency its TOD pin is fed at (CRA bit 7:
 * 1 = 50 Hz, 0 = 60 Hz); a wrong setting runs the clock 20% off — +30 min
 * over 2.5 h.  The feed is NOT implied by the video standard: VICE's x64sc
 * is PAL video with a 60 Hz TOD feed, xscpu64 the reverse, and on real
 * hardware it follows the mains.  So measure it.
 *
 * CIA2 Timer A divides O2 by CAL_UNIT and Timer B counts those underflows,
 * giving a 16-bit tick count over CAL_TENTHS TOD tenths, probed in 50 Hz mode:
 *
 *   PAL O2 (985248 Hz) .. NTSC O2 (1022727 Hz)
 *   feed really is 50 Hz  -> 0.400 s -> 3941 .. 4091
 *   feed is 60 Hz         -> 0.333 s -> 3284 .. 3409   (the 1.2x-fast case)
 *
 * The bands sit ~15% apart, so the 3.8% PAL/NTSC spread never straddles
 * CAL_SPLIT and the video standard needs no detecting.  This times hardware
 * against hardware, so CPU turbo (U64, SuperCPU) cannot skew it; a reading
 * outside [CAL_MIN,CAL_MAX] is reported as unknown rather than guessed at.
 * CIA2's interrupt mask ($DD0D) is never touched — a CIA2 IRQ is an NMI. */
#define CIA2_TA_LO   (*(volatile u8 *)0xDD04)
#define CIA2_TA_HI   (*(volatile u8 *)0xDD05)
#define CIA2_TB_LO   (*(volatile u8 *)0xDD06)
#define CIA2_TB_HI   (*(volatile u8 *)0xDD07)
#define CIA2_CRA     (*(volatile u8 *)0xDD0E)
#define CIA2_CRB     (*(volatile u8 *)0xDD0F)
#define TOD_10THS    (*(volatile u8 *)0xDC08)

#define CAL_TENTHS   4
#define CAL_UNIT     100
#define CAL_SPLIT    3675    /* below -> 60 Hz feed, above -> 50 Hz feed */
#define CAL_MIN      3000    /* outside [MIN,MAX] -> unrecognised */
#define CAL_MAX      4400

static u8 s_tod_hz = 0;      /* 50, 60, or 0 if the measurement was inconclusive */

/* Read Timer B, retrying across a high-byte carry. */
static u16 cal_tb_read(void)
{
    u8 hi, lo;
    do { hi = CIA2_TB_HI; lo = CIA2_TB_LO; } while (hi != CIA2_TB_HI);
    return (u16)(((u16)hi << 8) | lo);
}

/* Wait for the TOD tenths register to change.  FALSE if it never does, so a
 * stopped TOD reports "unknown" instead of hanging the boot.  Polling $DC08
 * alone is a live read — only reading $DC0B latches the TOD registers. */
static bool_t cal_wait_tenth(void)
{
    u8  t0 = TOD_10THS;
    u16 guard = 0;
    while (TOD_10THS == t0)
        /* The u16 wrap back to 0 after 65536 iterations IS the escape: a
         * stopped TOD would otherwise spin here forever and hang the boot.
         * cppcheck reports this as always-false because its value analysis
         * does not model unsigned wraparound (confirmed: the same finding
         * appears under --platform=avr8, so it is not an int-width artefact). */
        /* cppcheck-suppress knownConditionTrueFalse */
        if (++guard == 0u) return FALSE;
    return TRUE;
}

/* Measure the TOD feed. Returns 50, 60, or 0 if it could not be identified. */
static u8 tod_calibrate(void)
{
    u16 ticks;

    CIA2_CRA   = 0x00;
    CIA2_CRB   = 0x00;
    CIA2_TA_LO = (u8)(CAL_UNIT - 1);   /* period is latch+1 O2 cycles */
    CIA2_TA_HI = 0x00;
    CIA2_TB_LO = 0xFF;
    CIA2_TB_HI = 0xFF;
    CIA2_CRB   = 0x41;   /* start, count Timer A underflows */
    CIA2_CRA   = 0x01;   /* start, count O2 */

    ticks = 0;
    if (cal_wait_tenth()) {
        u16 a;
        u8  i;
        a = cal_tb_read();
        for (i = 0; i < CAL_TENTHS; i++)
            if (!cal_wait_tenth()) break;
        if (i == CAL_TENTHS) {
            u16 b = cal_tb_read();
            ticks = (u16)(a - b);   /* counts down; u16 wrap makes this right */
        }
    }

    CIA2_CRA = 0x00;
    CIA2_CRB = 0x00;

    if (ticks < CAL_MIN || ticks > CAL_MAX) return 0;
    return (u8)((ticks < CAL_SPLIT) ? 60 : 50);
}

/* Report the measured TOD feed.  Worth showing: a wrong rate is invisible
 * until the clock has drifted for hours. */
static void print_tod_rate(void)
{
    if (s_tod_hz) printf("TOD CLOCK: %uHZ\n", (unsigned)s_tod_hz);
    else          printf("TOD CLOCK: UNKNOWN, ASSUMING 50HZ\n");
}

void wfc_set_datetime(void)
{
    clock_tod_t t;
    u8 h = 0, m = 0, pm = 0, mo = 0, dy = 0, yr = 0;
    char confirm;

    printf(P_CLR P_GFX P_WHITE);
    printf(P_LGREEN "*** SET TIME/DATE ***\n" P_WHITE);
    print_tod_rate();

    /* ── Try RTC auto-detect ─────────────────────────────────── */
    if (rtc_try_read(&yr, &mo, &dy, &h, &m, &pm)) {
        printf("\n" P_LGREEN "RTC DETECTED!\n" P_WHITE);
        printf("\nTIME: " P_YELLOW "%02u:%02u %s" P_WHITE
               "  DATE: " P_YELLOW "%02u/%02u/%02u\n",
               h, m, pm ? "PM" : "AM", mo, dy, yr);
        /* Auto-accept the detected RTC time after a brief pause so unattended
         * boots (and headless VICE testing) reach WFC without a keypress.  The
         * sysop can still press N during the window to set the time manually.
         * Drain any stray keystrokes first (e.g. keys buffered while navigating
         * the U64 menu to launch the BBS) so they don't spuriously select N. */
        while (getchx() != 0) { }
        printf("\n" P_WHITE "USING RTC TIME... (N=MANUAL)");
        confirm = wait_key_2s();
        printf("\n");
        if (confirm != 'N' && confirm != 'n') goto commit;
    }

    do {
        printf(P_CLR P_GFX P_WHITE);
        printf(P_LGREEN "*** SET TIME/DATE ***\n" P_WHITE);
        print_tod_rate();

        /* ── Time entry ──────────────────────────────────────── */
        for (;;) {
            printf("\n" P_YELLOW "HOUR" P_WHITE " (1-12): ");
            h = read_uint();
            if (h >= 1 && h <= 12) break;
            printf("\n" P_LGREEN "  MUST BE 1-12" P_WHITE);
        }
        for (;;) {
            printf("\n" P_YELLOW "MINUTE" P_WHITE " (0-59): ");
            m = read_uint();
            if (m <= 59) break;
            printf("\n" P_LGREEN "  MUST BE 0-59" P_WHITE);
        }
        for (;;) {
            printf("\n" P_YELLOW "AM OR PM" P_WHITE " (1=AM 2=PM): ");
            pm = read_uint();
            if (pm == 1 || pm == 2) break;
            printf("\n" P_LGREEN "  ENTER 1 OR 2" P_WHITE);
        }

        /* ── Date entry ──────────────────────────────────────── */
        for (;;) {
            printf("\n" P_YELLOW "MONTH" P_WHITE " (1-12): ");
            mo = read_uint();
            if (mo >= 1 && mo <= 12) break;
            printf("\n" P_LGREEN "  MUST BE 1-12" P_WHITE);
        }
        for (;;) {
            printf("\n" P_YELLOW "DAY" P_WHITE " (1-31): ");
            dy = read_uint();
            if (dy >= 1 && dy <= 31) break;
            printf("\n" P_LGREEN "  MUST BE 1-31" P_WHITE);
        }
        for (;;) {
            printf("\n" P_YELLOW "YEAR" P_WHITE " (0-99): ");
            yr = read_uint();
            if (yr <= 99) break;
            printf("\n" P_LGREEN "  MUST BE 0-99" P_WHITE);
        }

        /* ── Confirm ─────────────────────────────────────────── */
        printf("\n\n" P_WHITE "TIME: " P_YELLOW "%02u:%02u %s" P_WHITE
               "  DATE: " P_YELLOW "%02u/%02u/%02u\n",
               h, m, (pm == 2) ? "PM" : "AM", mo, dy, yr);
        printf("\n" P_WHITE "CORRECT? (Y/N): ");
        confirm = getch();   /* getch() uses GETIN loop — no line buffer, no RETURN needed */
        printf("\n");

    } while (confirm != 'Y' && confirm != 'y');

    pm = (pm == 2) ? 1 : 0;   /* manual path: convert 1/2 input to 0/1 */

commit:
    t.tenths = 0x00;
    t.secs   = 0x00;
    t.mins   = (u8)(((m / 10) << 4) | (m % 10));
    t.hours  = (u8)(((h / 10) << 4) | (h % 10));
    if (pm) t.hours = (u8)(t.hours | 0x80);
    clock_set(&t);

    /* Build "MM/DD/YY" date string */
    wfc.date[0] = (char)('0' + mo / 10);
    wfc.date[1] = (char)('0' + mo % 10);
    wfc.date[2] = '/';
    wfc.date[3] = (char)('0' + dy / 10);
    wfc.date[4] = (char)('0' + dy % 10);
    wfc.date[5] = '/';
    wfc.date[6] = (char)('0' + yr / 10);
    wfc.date[7] = (char)('0' + yr % 10);
    wfc.date[8] = '\0';
}

void wfc_init_impl(void)
{
    /* Start the TOD in 50 Hz mode, measure what it is actually fed, then
     * restart it with the right rate.  Costs ~0.5 s, once, at boot. */
    clock_init(50);
    s_tod_hz = tod_calibrate();
    clock_init(s_tod_hz ? s_tod_hz : 50);

    wfc_set_datetime();   /* prompt sysop for time + date at boot */
    clock_read(&wfc.boot_time);
    wfc.last_secs = 0xFF;
    /* Restore persisted "SYSOP IS" status line (set via F7 or CONFIGURE) */
    sstatus_load(wfc.chat_msg, (u8)sizeof(wfc.chat_msg));
    /* Restore calls/posts counters (survive reboots within the same day) */
    syscnt_load();
    /* Restore caller log from disk (survives reboots) */
#if WFC_LOG_SIZE > 0
    {
        u8 loaded;
        callers_load(wfc.log, WFC_LOG_SIZE, &loaded);
        wfc.log_count = loaded;
        wfc.log_head  = (u8)(loaded % WFC_LOG_SIZE);
    }
#endif
}

void wfc_display_impl(void)
{
    clock_tod_t now;
    clock_read(&now);
    wfc.last_secs = now.secs;

    printf(P_CLR P_GFX);    /* clear screen, uppercase charset */

    draw_header(&now);       /* rows 0–1  (blank + header) */
    printf(P_LGREEN
           "\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0"
           "\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0"
           "\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0"
           "\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0\xC0"
           "\n" P_WHITE);   /* row 2  PETSCII $C0 horizontal bar × 39 */
    draw_stats();            /* rows 3–5  stats grid */
    draw_sysop_msg();        /* rows 6–8  sysop status */
    draw_callers_log();      /* rows 9–18 callers */
    draw_footer();           /* rows 19–23 footer (bottom) */
}

bbs_err_t wfc_update_impl(void)
{
    clock_tod_t now;
    u16  got;
    // cppcheck-suppress variableScope
    char key;

    /* Advance modem state machine */
    net_rx((void *)0, 0, &got);

    /* Carrier detect → signal caller arrival */
    if (net_state() == NET_CONNECTED) {
        draw_alert("INCOMING CALL! CONNECTING...");
        return BBS_EAGAIN;
    }

    /* Update time field (row 0) once per second */
    clock_read(&now);
    if (now.secs != wfc.last_secs) {
        wfc.last_secs = now.secs;
        printf(P_HOME);             /* cursor to top-left */
        draw_header(&now);          /* overwrite row 0 only */
    }

    /* Non-blocking keyboard check — use getchx() (GETIN, $FFE4) not
     * getchar() (CHRIN, $FFCF).  CHRIN uses the KERNAL line editor and
     * waits for RETURN before returning; GETIN returns single keycodes
     * (including F-key PETSCII codes) immediately without echoing. */
    if (C64_KBD_COUNT > 0) {
        key = getchx();
        switch (key) {
            case KEY_F1:
                break;   /* F1 reserved for session use */
            case KEY_F2:
                break;   /* F2 removed */
            case KEY_F3:
                break;   /* F3 removed */
            case KEY_F4:
                /* REFRESH SCREEN */
                wfc_display_impl();
                break;
            case KEY_F5:
                break;   /* F5 unused */
            case KEY_F6:
                /* CHAT TOGGLE — flip SYSOP CHAT Y/N */
                wfc.chat_enabled = wfc.chat_enabled ? FALSE : TRUE;
                wfc_display_impl();
                break;
            case KEY_F7:
                /* SET STATS — edit sysop status message */
                prompt_chat_msg();
                wfc_display_impl();
                break;
            case KEY_F8:
                /* QUIT — graceful shutdown */
                draw_alert("[ SHUTTING DOWN... ]");
                return BBS_EQUIT;
            case '\r':
                wfc.local_logon = TRUE;
                draw_alert("[SYSOP LOCAL LOGON...]");
                return BBS_EAGAIN;
            default:
                printf(P_HOME);
                clock_read(&now);
                draw_header(&now);
                break;
        }
    }

    return BBS_OK;
}

/* Print a string for the current local-screen charset. In text/lowercase charset,
 * remap ASCII letters so they render correct-case there (A-Z -> 0xC1-0xDA uppercase,
 * a-z -> 0x41-0x5A lowercase); pass digits, punctuation, and raw PETSCII color/
 * control codes through unchanged. In uppercase/graphics charset, print verbatim. */
static void wfc_puts_cs(const char *str)
{
    bool_t lower = session_screen_lower();
    char c;
    while ((c = *str++) != 0) {
        if (lower) {
            if (c >= 'A' && c <= 'Z') { putchar((char)((u8)c + 0x80u)); continue; }
            if (c >= 'a' && c <= 'z') { putchar((char)((u8)c - 0x20u)); continue; }
        }
        putchar(c);
    }
}

/**
 * wfc_draw_user_footer()
 * Draw 5-row user info panel (rows 20–24) matching spy_footer.seq layout.
 * Ends with gotoxy(0,20) so cursor never sits on row 24 (prevents KERNAL scroll).
 */
static void wfc_draw_user_footer(const session_t *s, u16 elapsed_secs)
{
    char name[25];
    char line[72];
    // cppcheck-suppress variableScope
    u8 j, k;

    /* 80-col spy uses the resident single-row status line (session_spy_status),
     * drawn by the wrappers below — this PETSCII 5-row panel handles 40-col. */

    name[0] = '\0';
    if (s->reg_firstname[0]) {
        j = 0;
        while (j < 15u && s->reg_firstname[j]) { name[j] = s->reg_firstname[j]; j++; }
        if (s->reg_lastname[0]) {
            name[j++] = ' ';
            k = 0;
            while (j < 23u && s->reg_lastname[k]) { name[j++] = s->reg_lastname[k++]; }
        }
        name[j] = '\0';
    }

    gotoxy(0, 20);
    sprintf(line, P_LBLUE P_RVON "    F1=CHAT     F2=KICK     F3=ACCS    " P_RVOF);
    wfc_puts_cs(line);

    gotoxy(0, 21);
    sprintf(line, P_YELLOW "USER:" P_WHITE " %-20s" P_YELLOW "ACCS:" P_WHITE " %-7u ",
            s->handle, (unsigned)s->user.access_level);
    wfc_puts_cs(line);

    gotoxy(0, 22);
    sprintf(line, P_YELLOW "REAL:" P_WHITE " %-20s" P_YELLOW "CALL:" P_WHITE " %-7u ",
            name[0] ? name : "-", (unsigned)s->user.calls);
    wfc_puts_cs(line);

    gotoxy(0, 23);
    sprintf(name, "%uM", (unsigned)(elapsed_secs / 60u));
    sprintf(line, P_YELLOW "FROM:" P_WHITE " %-20s" P_YELLOW "TIME:" P_WHITE " %-7s ",
            s->reg_location[0] ? s->reg_location : "-", name);
    wfc_puts_cs(line);

    gotoxy(0, 24);
    sprintf(line, P_YELLOW "LAST:" P_WHITE " %-20s" P_YELLOW "MSGS:" P_WHITE " %-6u",
            "----", 0u);
    wfc_puts_cs(line);

    /* Park cursor away from row 24 — prevents KERNAL scroll on next output */
    gotoxy(0, 20);
}

#pragma code(code)
#pragma data(data)
#pragma bss(bss)

/* wfc_display_session() and wfc_update_session() live in MAIN code, not the
 * WFC overlay, because main.c calls them directly during a session — and the
 * MSGS overlay (message bases) displaces the WFC overlay at $9700-$BFFF.
 *
 * 80-col spy: the status line (session_spy_status) is resident in main, so it
 * draws under any overlay — handled BEFORE the wfc.ovl_wfc_loaded guard.
 * 40-col PETSCII spy: the 5-row panel (wfc_draw_user_footer) is in the WFC
 * overlay, so it only draws while WFC is resident. */

/** wfc_display_session() — called once when session starts; draws spy footer. */
void wfc_display_session(const session_t *s)
{
    if (!s) return;
    if (s->screen_capture.spy_mode == SPY_MODE_80COL) { session_spy_status(s, 0); return; }
    if (!wfc.ovl_wfc_loaded) return;
    if (s->user_id == 0) { clear_user_footer(); return; }
    wfc_draw_user_footer(s, 0);
}

/**
 * wfc_update_session()
 * Update the spy footer once per second.  The 80-col status line redraws
 * everywhere; the 40-col panel skips while the WFC overlay is displaced
 * (e.g. caller is in the message bases) and resumes on return.
 */
void wfc_update_session(const session_t *s, u16 elapsed_secs)
{
    static u8 last_secs = 255;
    clock_tod_t now;
    if (!s) return;
    /* 80-col status line + SysOp keys are handled by session_spy_poll() (resident,
     * called from the main loop and feature loops). This path draws only the
     * 40-col PETSCII panel, which lives in the WFC overlay. */
    if (s->screen_capture.spy_mode == SPY_MODE_80COL) return;
    clock_read(&now);
    if (now.secs == last_secs) return;
    last_secs = now.secs;
    if (!wfc.ovl_wfc_loaded) return;
    if (s->user_id == 0) { clear_user_footer(); return; }
    wfc_draw_user_footer(s, elapsed_secs);
}

void wfc_log_session(const char *handle, u16 baud, u16 duration)
{
#if WFC_LOG_SIZE > 0
    wfc_log_entry_t *e = &wfc.log[wfc.log_head];
    clock_read(&e->time);
    strncpy(e->handle, handle, 15);
    e->handle[15] = '\0';
    e->baud = baud;
    e->duration = duration;
    wfc.log_head = (u8)((wfc.log_head + 1) % WFC_LOG_SIZE);
    if (wfc.log_count < WFC_LOG_SIZE) {
        wfc.log_count++;
    }
#else
    (void)handle; (void)baud; (void)duration;
#endif
}
