/* spy80.c - VIC-II hires bitmap engine for 80-col spy view. */
#include "bbs/spy80.h"
#include "bbs/types.h"
#include "bbs/net.h"     /* net_rx_hold/release: RTS off while IRQs are masked (issue #27) */
#include <c64/vic.h>
#include <string.h>

/* Font source table (in the WFC overlay) — copied to resident RAM by init. */
extern const u8 spy80_font[96][3];

/* Resident RAM copy of the packed font: 96*3 = 288 bytes at $C3E8, immediately
 * after the 1000-byte spy color RAM ($C000-$C3E7) and well below the stack.
 * Lives outside main code so the render path fits, yet stays resident while
 * the MSGS overlay is loaded. */
#define SPY80_FONT_RAM ((const u8 *)0xC3E8)

/* ANSI parser entry points (spy80_ansi.c) */
void spy80_ansi_feed(u8 ch);
void spy80_ansi_reset(void);

/* $01 banking register */
#define CPU_PORT ((volatile u8 *)0x01)
#define BANK_KERNAL_OUT  0x34u   /* BASIC out, KERNAL out, I/O visible */
#define BANK_RESTORE     0x36u   /* BASIC out, KERNAL in,  I/O visible */

/* ------------------------------------------------------------------ */
/* Resident render path — MAIN code.                                    */
/* These must stay resident while the MSGS overlay is loaded, because   */
/* session_emit feeds the spy during message-base browsing (the MSGS    */
/* overlay displaces the WFC overlay). Only spy80_init / spy80_puts may  */
/* live in the WFC overlay (they run only while WFC is resident).        */
/* ------------------------------------------------------------------ */

/* Write one character glyph into the bitmap at $E000.
 * Banks out KERNAL for the write (IRQs off — vector is under ROM).
 * rev != 0 inverts the glyph pixels: spaces fill solid, text becomes holes —
 * a reverse-video bar in vic_color with black text (used for the status line). */
static void spy80_render(u8 col, u8 row, u8 ch, u8 vic_color, u8 rev)
{
    u16 byte_idx;
    const u8 *glyph;
    u8  old_bank;
    u8  i;

    if (col >= SPY80_COLS || row >= SPY80_ROWS) return;
    if (ch < 0x20u || ch > 0x7Fu) ch = 0x20u;

    glyph    = SPY80_FONT_RAM + (u16)(ch - 0x20u) * 3u;  /* 3 packed bytes */
    byte_idx = (u16)(((u16)row * 40u + col / 2u) * 8u);

    /* Update screen RAM color for this VIC cell (right char color wins) */
    SPY80_SCRN[(u16)row * 40u + col / 2u] = (u8)(vic_color << 4);

    old_bank = *CPU_PORT;
    net_rx_hold();
    __asm { sei }
    *CPU_PORT = BANK_KERNAL_OUT;

    for (i = 0; i < 8u; i++) {
        /* unpack pixel row i (rows 0-5 from font; rows 6-7 blank):
         * even rows in high nibble, odd rows in low. */
        u8 gp = (i >= 6u) ? 0u
              : ((i & 1u) ? (u8)(glyph[i >> 1] & 0x0Fu)
                          : (u8)(glyph[i >> 1] >> 4));
        if (rev) gp = (u8)(~gp) & 0x0Fu;   /* invert for reverse-video bar */
        if (col & 1u) {
            SPY80_BITMAP[byte_idx + i] =
                (SPY80_BITMAP[byte_idx + i] & 0xF0u) | gp;
        } else {
            SPY80_BITMAP[byte_idx + i] =
                (SPY80_BITMAP[byte_idx + i] & 0x0Fu) | (u8)(gp << 4);
        }
    }

    *CPU_PORT = old_bank;
    __asm { cli }
    net_rx_release();
}

void spy80_render_char(u8 col, u8 row, u8 ch, u8 vic_color)
{
    spy80_render(col, row, ch, vic_color, 0u);
}

void spy80_clear(void)
{
    u16 i;
    /* Clear the caller-content area only (rows 0-23); the SysOp status line at
     * row 24 is preserved so it stays visible across the caller's ESC[2J clears
     * and wherever they navigate. Row 24: bitmap [7680,8000), color [960,1000). */
    for (i = 0u; i < 960u;  i++) SPY80_SCRN[i]   = 0x10u;   /* color rows 0-23 */
    for (i = 0u; i < 7680u; i++) SPY80_BITMAP[i] = 0x00u;   /* bitmap rows 0-23 */
}

/* Scroll the caller-content area (rows 0-23) up one text row; the status line
 * (row 24) is left untouched. One 320-byte cell-row is copied per SEI/bank
 * window so interrupts are disabled only ~2ms at a time. Bitmap reads need
 * KERNAL banked out (reads of $E000 otherwise hit ROM); writes fall through to
 * RAM, so the final row-clear needs no banking. */
void spy80_scroll(void)
{
    u16 i;
    u8  r;

    /* Color RAM ($C000, plain RAM): rows 1-23 -> 0-22, clear row 23. */
    for (i = 0u;   i < 920u; i++) SPY80_SCRN[i] = SPY80_SCRN[i + 40u];
    for (i = 920u; i < 960u; i++) SPY80_SCRN[i] = 0x10u;

    /* Bitmap: copy cell-rows 1-23 up to 0-22, one row per SEI window.
     * ~10 ms per row with IRQs off: at 38400 that is ~40 bytes the RX poll
     * cannot see, so RTS is held for the whole copy and the Ultimate holds
     * the caller's bytes instead (measured: a burst during a scroll lost
     * nearly all of it). */
    net_rx_hold();
    for (r = 0u; r < 23u; r++) {
        u16 dst = (u16)r * 320u;
        u16 src = dst + 320u;
        u16 j;
        u8  old_bank = *CPU_PORT;
        __asm { sei }
        *CPU_PORT = BANK_KERNAL_OUT;
        for (j = 0u; j < 320u; j++) SPY80_BITMAP[dst + j] = SPY80_BITMAP[src + j];
        *CPU_PORT = old_bank;
        __asm { cli }
    }
    net_rx_release();
    /* Clear the now-vacated bottom content row (row 23): pure writes. */
    for (i = 7360u; i < 7680u; i++) SPY80_BITMAP[i] = 0x00u;
}

void spy80_feed(u8 ch)
{
    spy80_ansi_feed(ch);
}

void spy80_done(void)
{
    /* Restore character mode: bank 0, screen RAM $0400, charset $1000 */
    vic_setmode(VICM_TEXT, (char *)0x0400, (char *)0x1000);
}

/* spy80_puts / spy80_puts_rev are resident (MAIN): the sysop status line at
 * row 24 must redraw wherever the caller is, including while a feature overlay
 * has displaced WFC. _rev draws a reverse-video bar (text punched out). */
void spy80_puts(u8 col, u8 row, const char *text, u8 vic_color)
{
    while (*text && col < SPY80_COLS) {
        spy80_render(col, row, (u8)*text, vic_color, 0u);
        col++;
        text++;
    }
}

void spy80_puts_rev(u8 col, u8 row, const char *text, u8 vic_color)
{
    while (*text && col < SPY80_COLS) {
        spy80_render(col, row, (u8)*text, vic_color, 1u);
        col++;
        text++;
    }
}

/* ------------------------------------------------------------------ */
/* WFC overlay — spy80_init runs only at session start (WFC resident). */
/* ------------------------------------------------------------------ */
#pragma code(wfc_code)
#pragma data(wfc_data)
#pragma bss(wfc_bss)

void spy80_init(void)
{
    /* Copy the packed font from the overlay into resident RAM at $C3E8 so the
     * render path can read it after the MSGS overlay displaces this code. */
    memcpy((void *)0xC3E8, spy80_font, 96u * 3u);
    spy80_clear();
    spy80_ansi_reset();
    /* Switch VIC to hires bitmap: bank 3, screen RAM $C000, bitmap $E000 */
    vic_setmode(VICM_HIRES, (char *)0xC000, (char *)0xE000);
}

#pragma code(code)
#pragma data(data)
#pragma bss(bss)
