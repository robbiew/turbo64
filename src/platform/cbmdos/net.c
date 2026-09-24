/* C64 / U64 SwiftLink ACIA driver. Implements bbs/net.h.
 *
 * The 6551 ACIA is at $DE00-$DE03. The U64 firmware bridges incoming
 * TCP telnet connections (port 3000) through the ACIA at configurable baud
 * (default 9600), and drives DSR active-low when a TCP session is live.
 *
 * Baud rate configuration:
 *   - On U64: Setting ACIA_CTL baud bits may work if the U64 firmware supports
 *     dynamic reconfiguration. Verify with U64 documentation.
 *   - On VICE: tcpser's speed is fixed at startup (e.g., -s 9600). ACIA register
 *     writes are harmless but have no effect. Reconfigure tcpser's command line
 *     to change speed, not the BBS config.
 *
 * Under VICE with tcpser, DSR is never driven — connect/disconnect is
 * detected exclusively via AT command strings (CONNECT / NO CARRIER).
 * The driver auto-detects which mode it is in: if DSR is seen active
 * during a session, DSR-based disconnect is used (U64); otherwise AT
 * strings are authoritative (VICE/tcpser).
 *
 * Ported from drive8-bbs src/platform/c64u/acia.c.
 */
#include "bbs/net.h"
#include "bbs/cfg.h"
#include "net/telnet_iac.h"
#include "net/at_response.h"
#include <string.h>

#define ACIA_DATA    (*(volatile u8 *)0xDE00)
#define ACIA_STATUS  (*(volatile u8 *)0xDE01)
#define ACIA_CMD     (*(volatile u8 *)0xDE02)
#define ACIA_CTL     (*(volatile u8 *)0xDE03)

/* UCI register addresses on Ultimate 64 hardware.  AUTO uses the idle CMD
 * register probe to distinguish U64 from VICE before the modem state machine
 * starts; VICE builds should still use MODEM_TYPE=VICE explicitly. */
#define UCI_CMD      (*(volatile u8 *)0xDF1D)
#define UCI_IDENTIFIER ((u8)0xC9)

#define ST_TX_EMPTY  0x10
#define ST_RX_FULL   0x08

/* Baud rate mappings (6551 ACIA bits 0-3). Keep original control byte structure (0x1E base for 9600)
 * for compatibility with U64 firmware. Only bits 0-3 change per baud rate. */
#define CTL_BAUD_300    0x16    /* 0001 0110: baud bits=0110 */
#define CTL_BAUD_1200   0x18    /* 0001 1000: baud bits=1000 */
#define CTL_BAUD_2400   0x1A    /* 0001 1010: baud bits=1010 */
#define CTL_BAUD_9600   0x1E    /* 0001 1110: baud bits=1110 (original) */
#define CTL_BAUD_19200  0x1F    /* 0001 1111: baud bits=1111 */
#define CTL_BAUD_38400  0x1F    /* 0001 1111: U64 handles; uses 19200 register value */

#define CMD_DTR_ON   0x0B   /* DTR=1, RTS low, TX int disabled (ACIA IRQ OFF) */
#define CMD_DTR_OFF  0x0A   /* DTR=0 */

/* ── NMI-free interrupt-driven RX (VICE/tcpser overrun fix) ──────────────────
 * The 6551 holds only one received byte.  The WFC/session main loop is
 * preempted by multi-ms KERNAL routines (screen, disk), so polling the ACIA
 * there overruns the register and drops inbound bytes — under VICE the CONNECT
 * result code is lost and the BBS never answers a caller.
 *
 * We deliberately do NOT enable the ACIA's own RX interrupt: on SwiftLink it is
 * wired to the (unmaskable) NMI, which corrupts the timing-critical KERNAL IEC
 * disk routines and crashes the BBS.  Instead a CIA#1 Timer-B IRQ polls the
 * ACIA into a ring buffer.  Being a maskable IRQ it is held off (SEI) during
 * KERNAL disk I/O — no corruption — yet fires freely at the WFC wait loop
 * (which does no disk) to capture the CONNECT burst.  Single producer
 * (acia_irq_isr writes s_rx_tail); single consumer (net_rx reads s_rx_head). */
/* 128-byte power-of-two ring.  head/tail are full u8 counters (wrap at 256);
 * the low 7 bits index the buffer (RX_BUF_MASK).  net_rx drains continuously,
 * so 128 bytes is ample for result codes and input bursts.  The buffer lives
 * in the KERNAL cassette buffer ($033C-$03BB) — free RAM on a disk-only BBS —
 * to keep it out of the cramped oscar64 BSS segment. */
#define RX_BUF_MASK  0x7Fu
#define s_rx_buf     ((volatile u8 *)0x033Cu)   /* 128 bytes @ $033C-$03BB */
static volatile u8 s_rx_head;
static volatile u8 s_rx_tail;

/* Last value written to ACIA_CMD (DTR/RTS bits). The Timer-B ISR reads it to
 * deassert RTS at the ring high-water mark, so it must be declared before the
 * ISR. CMD_RTS_MASK is bits 3-2 (00 = RTS deasserted/high). */
#define CMD_RTS_MASK 0x0C
static u8 s_cmd_shadow = CMD_DTR_ON;

/* Timer-B reload, in phi2 cycles (~1.02 MHz).  The 6551 holds ONE received
 * byte, so two consecutive drains must never be further apart than a byte
 * time, including IRQ latency: the instruction in flight (<= 7 cycles), a VIC
 * bad line (~43), and the ~80 cycles the KERNAL IRQ entry + this handler +
 * the epilogue cost.
 *
 *   250 is fine up to 19200 (~530 cycles per byte) and was written for that.
 *   At 38400 a byte is ~265 cycles, and 250 + latency exceeds it whenever a
 *   bad line delays the tick — measured on a C64 Ultimate 2026-09-22: with
 *   the keyscan window already fixed (see acia_irq_isr), 4 of 10 unpaced
 *   15-character bursts still lost a byte at 38400; at 125 it was 10 of 10
 *   intact.  160 leaves ~50 cycles of margin at 38400 for about half the
 *   interrupt load of 125 (~80 cycles of overhead per tick either way, so
 *   ~50% of the CPU at 160 vs ~32% at 250 — the price of 38400 on a 1 MHz
 *   machine).  The latch is picked per configured rate in net_init(), so
 *   9600/19200 setups keep the cheaper tick. */
#define TIMERB_LATCH_SLOW 250   /* <= 19200 */
/* 38400. Was 160 (~50 cycles of margin), which survived short frames but a VIC
 * bad line eating that margin dropped ~1 byte in a 40+ byte Zmodem subpacket —
 * enough to fail every upload on CRC. 120 restores the margin that measured
 * 10-of-10 intact (comment above), at more interrupt load — the price of
 * reliable 38400 receive on a 1 MHz machine, paid only on 38400 setups. */
#define TIMERB_LATCH_FAST 120   /* 38400 */
static u8 s_tb_latch = TIMERB_LATCH_SLOW;

/* CIA#1 Timer-B IRQ, chained off $0314.  Reads the CIA ICR once (which clears
 * it), drains the ACIA into the ring on a Timer-B tick, and on a Timer-A tick
 * runs the KERNAL jiffy (UDTIM) + keyboard scan (SCNKEY) so the local console
 * keeps working, then exits via the KERNAL IRQ epilogue ($EA81).
 *
 * The Timer-A work takes ~1 ms, longer than a byte at any rate above 9600,
 * and used to run with IRQs masked: a caller's burst that straddled a keyscan
 * lost one byte every time.  Measured on a C64 Ultimate 2026-09-22 with a
 * 15-character paste at the HANDLE prompt — 3 of 4 bursts dropped a character
 * at 38400, 2 of 4 at 9600; 80 ms per-character pacing never lost one.  So
 * IRQs are re-enabled around UDTIM/SCNKEY: a Timer-B tick then nests through
 * this same handler, sees only its own bit (Timer-A's was consumed at entry),
 * drains the ACIA and returns into the keyscan.  The nested path touches only
 * the ring and the ACIA, so nothing SCNKEY uses is disturbed, and it cannot
 * itself CLI, so nesting depth is one.  A final drain before the epilogue
 * picks up anything that arrived after the last nested tick. */
__asm acia_irq_isr
{
    lda $dc0d           // read+clear CIA1 ICR: bit0=TimerA, bit1=TimerB
    pha
    and #$02
    beq chkta
tbloop:
    lda $de01
    and #$08            // RDRF (receive register full)?
    beq chkta
    lda s_rx_tail
    and #$7f            // low 7 bits index the 128-byte ring
    tax
    lda $de00
    sta $033c, x        // s_rx_buf @ $033C (cassette buffer)
    inc s_rx_tail
    // Hardware flow control: when the ring fills past the high-water mark,
    // deassert RTS so the (un-rate-limited) sender pauses before the 128-byte
    // ring overruns. net_rx_raw re-asserts once the consumer drains it below
    // the low-water mark. 64 leaves 64 bytes of headroom for RTS-response
    // latency. Without this, a streamed 1 KB Zmodem subpacket dropped ~a
    // ring's worth of bytes (issue #36).
    lda s_rx_tail
    sec
    sbc s_rx_head
    cmp #16
    bcc tbloop
    lda s_cmd_shadow
    and #$f3            // ~CMD_RTS_MASK: deassert RTS, keep DTR
    sta $de02          // ACIA_CMD
    jmp tbloop
chkta:
    pla
    and #$01
    beq done
    cli                 // let Timer-B nest through us during the ~1 ms below
    jsr $ffea           // UDTIM: jiffy clock + STOP key
    jsr $ff9f           // SCNKEY: keyboard scan -> buffer (for GETIN)
    sei
    lda #$02
    pha                 // fake "Timer-B only" ICR and take one more drain pass
    jmp tbloop
done:
    jmp $ea81           // KERNAL IRQ epilogue: pull Y,X,A then RTI
}

/* Full Timer-B RX-IRQ (re)arm: load the latch, force-load+start the timer in
 * continuous mode, unmask the Timer-B interrupt, and install the $0314 vector —
 * all under SEI so a stray IRQ never jumps through a half-built setup.  Safe to
 * call repeatedly: net_irq_arm() arms it once, and net_rx() re-arms whenever the
 * KERNAL boot/disk path has stopped the timer or reset the vector.  Writing
 * $DC0D with the set-mask bit ($82) only ADDS Timer-B to the enabled interrupts,
 * leaving the KERNAL's Timer-A jiffy interrupt untouched. */
static void net_irq_setup(void)
{
    __asm {
        sei
        lda s_tb_latch
        sta $dc06
        lda #0              // latch high byte: both values fit in one byte
        sta $dc07
        lda #$11            // CRB: start, continuous, force-load, count phi2
        sta $dc0f
        lda #$82            // ICR: set-bit + enable Timer-B interrupt
        sta $dc0d
    }
    *(void * volatile *)0x0314 = (void *)acia_irq_isr;
    __asm { cli }
}

/* Arm the Timer-B RX IRQ.  Call once boot disk I/O is finished: the KERNAL boot
 * path resets the $0314 RAM vector and stops Timer B, and running the timer
 * during the disk-heavy boot adds needless load.  net_rx() re-arms defensively. */
void net_irq_arm(void)
{
    /* Only reset the ring if the poll is not already running: boot-time
     * disk I/O after net_init() re-arms it through net_rx_release(), and
     * whatever a caller sent meanwhile is sitting in the ring — zeroing it
     * here would discard that (PR #32 review). */
    if (*(void * volatile *)0x0314 != (void *)acia_irq_isr ||
        (*(volatile u8 *)0xDC0F & 0x01) == 0) {
        s_rx_head = 0;
        s_rx_tail = 0;
    }
    net_irq_setup();
}

static net_state_t     s_state;
/* Last value written to ACIA_CMD (DTR/RTS state), so net_rx_release() can
 * restore exactly what the modem logic last asked for; s_hold_depth nests
 * hold/release; s_acia_up gates both until net_init() has run. */
static u8              s_hold_depth;
static bool_t          s_acia_up;


__noinline static void acia_set_cmd(u8 v)
{
    s_cmd_shadow = v;
    ACIA_CMD = s_hold_depth ? (u8)(v & ~CMD_RTS_MASK) : v;
}

__noinline void net_rx_hold(void)
{
    if (s_hold_depth++ == 0 && s_acia_up) {
        ACIA_CMD = (u8)(s_cmd_shadow & ~CMD_RTS_MASK);
    }
}

__noinline void net_rx_release(void)
{
    if (s_hold_depth == 0) return;
    if (--s_hold_depth == 0 && s_acia_up) {
        /* The KERNAL load/disk path can reset the $0314 vector and stop
         * Timer B (see net_rx()). The Ultimate releases the held bytes the
         * instant RTS comes back, so the poll ISR must be alive FIRST or the
         * first byte or two overrun the 6551 — measured: exactly "P\r" of a
         * burst lost after an overlay load, the rest intact. */
        if (*(void * volatile *)0x0314 != (void *)acia_irq_isr ||
            (*(volatile u8 *)0xDC0F & 0x01) == 0) {
            net_irq_setup();
        }
        ACIA_CMD = s_cmd_shadow;
    }
}
static at_parser_t     s_at;
/* s_iac lives in free RAM ($02A7-$02C7, the unused $02A7-$02FF block) to keep
 * its 33 bytes out of the resident region ($0880-$9700), which is at its hard
 * memory ceiling. Every use is &s_iac passed to a telnet_filter_* call, so the
 * struct-lvalue macro yields a constant address (no indirection). It is always
 * telnet_filter_init()'d before use, so it needs no zero-init. */
#define s_iac (*(telnet_filter_t *)0x02A7u)
/* s_saw_dsr_inactive: guards against spurious connect at boot if DSR happens
 * to read active.  Set TRUE the first time we observe DSR inactive; reset
 * by net_disconnect() so each call cycle requires a real inactive→active edge. */
static bool_t          s_saw_dsr_inactive;
/* s_dsr_was_active: TRUE if DSR was seen active during the current session.
 * If FALSE, we're running under VICE/tcpser (DSR never driven) and must use
 * AT string events for disconnect rather than DSR state. */
static bool_t          s_dsr_was_active;
/* s_at_mode: TRUE once an AT-string CONNECT has been seen (VICE/tcpser, or any
 * modem that reports result codes).  In that mode the DSR hardware line is
 * meaningless (raw TCP doesn't carry it; the ACIA reads a constant value), so
 * the U64 DSR connect/disconnect logic is disabled — otherwise the always-
 * "active" DSR reading spuriously re-connects the BBS right after a NO CARRIER
 * hangup, leaving it stuck "connected". */
static bool_t          s_at_mode;

/* Boot-only (called from net_init() only): lives in the boot overlay. */
#ifdef T64_BOOT_OVERLAY
#pragma code(boot_code)
#pragma data(boot_data)
#endif
static modem_type_t resolve_modem_type(void)
{
    if (bbs_cfg.modem_type != MODEM_AUTO) {
        return bbs_cfg.modem_type;
    }

    /* U64 hardware responds with $C9 on the idle UCI CMD register; VICE does
     * not expose UCI, so AUTO resolves to VICE unless that signature is present.
     */
    if (UCI_CMD == UCI_IDENTIFIER) {
        return MODEM_U64;
    }

    return MODEM_VICE;
}
#ifdef T64_BOOT_OVERLAY
#pragma code(code)
#pragma data(data)
#endif

/* Consecutive bytes the transmitter refused to take (TDRE never set within
 * the spin timeout, ~0.6 s each). On the Ultimate this is what a caller who
 * closed their TCP session without the firmware noticing looks like: DSR
 * stays asserted, the socket is dead, and TX stops draining (issue #31).
 * TX_STALL_LIMIT such bytes while connected is treated as carrier loss so
 * the session ends and net_disconnect()'s DTR drop clears the firmware —
 * seconds instead of the idle timeout's minutes. A live but slow client
 * would have to stop reading for the whole span to trip it. */
static void delay_jiffies(u8 n);   /* defined below; used by net_init() */

#define TX_STALL_LIMIT 3
static u8     s_tx_stalls;
static bool_t s_tx_dead;     /* stall threshold hit: net_rx() must drop DTR */

/* Returns FALSE when the byte was not sent: the transmitter stalled, or the
 * line is already being dropped — in which case it returns at once, so a
 * sender with hundreds of bytes queued behind a dead socket fails in one
 * timeout, not one per byte (PR #33 review). */
__noinline static bool_t acia_putc(u8 b)
{
    u16 timeout = 0;
    if (s_state == NET_DROPPING) return FALSE;
    while ((ACIA_STATUS & ST_TX_EMPTY) == 0) {
        // cppcheck-suppress knownConditionTrueFalse
        if (++timeout == 0) {
            if (s_state == NET_CONNECTED && ++s_tx_stalls >= TX_STALL_LIMIT) {
                /* Flag it for net_rx() to tear down (DTR drop) on its next
                 * call rather than calling net_disconnect() from here: that
                 * would make acia_putc -> net_disconnect -> acia_puts ->
                 * acia_putc a cycle, which oscar64's static frames cannot
                 * take. net_rx() is the pump every wait path calls, the WFC
                 * included, so the teardown never depends on a session
                 * cleanup that the WFC pump does not have. */
                s_state   = NET_DROPPING;
                s_tx_dead = TRUE;
            }
            return FALSE;
        }
    }
    s_tx_stalls = 0;
    ACIA_DATA = b;
    return TRUE;
}

/* One invisible byte to the caller (NUL: ignored by telnet clients and by
 * PETSCII/ANSI terminals alike) so that a dead socket shows up as a TX
 * stall even while the BBS is only waiting for input. sess_idle_check()
 * sends it every few idle seconds. */
void net_keepalive(void)
{
    if (s_state == NET_CONNECTED) (void)acia_putc(0);
}

static void acia_puts(const char *s)
{
    while (*s && acia_putc((u8)*s)) s++;
}

/* Boot-only: runs once from boot_sequence(), which itself runs from the
 * boot overlay, so it lives there too and costs nothing resident. Everything
 * it sets up (state, shadow registers, the Timer-B latch) is resident data.
 * acia_putc()/acia_puts() and s_at_mode deliberately stay OUTSIDE: they are
 * used by every transmit for the life of the run. */
#ifdef T64_BOOT_OVERLAY
#pragma code(boot_code)
#pragma data(boot_data)
#endif
bbs_err_t net_init(void)
{
    u8 ctl;
    modem_type_t modem_type;
    
    s_state              = NET_IDLE;
    s_saw_dsr_inactive   = FALSE;
    s_dsr_was_active     = FALSE;
    modem_type           = resolve_modem_type();
    if (bbs_cfg.modem_type == MODEM_AUTO) {
        bbs_cfg.modem_type = modem_type;
    }
    /* MODEM_VICE forces AT-string mode from boot; MODEM_U64 keeps DSR mode and
     * never auto-switches. AUTO resolves once at boot, then follows the detected
     * modem type for the remainder of the session. */
    s_at_mode            = (modem_type == MODEM_VICE);
    at_parser_init(&s_at);
    telnet_filter_init(&s_iac);

    /* Map configured baud rate to ACIA control register bits 0-3.
     * On U64: Reconfiguring ACIA_CTL may work if the U64 firmware supports it.
     * On VICE: tcpser's speed is fixed at startup and not affected by ACIA writes.
     * Supported rates: 300, 1200, 2400, 9600, 19200, 38400 (all ACIA standard). */
    switch (bbs_cfg.baud_rate) {
    case 300:    ctl = CTL_BAUD_300;    break;
    case 1200:   ctl = CTL_BAUD_1200;   break;
    case 2400:   ctl = CTL_BAUD_2400;   break;
    case 9600:   ctl = CTL_BAUD_9600;   break;
    case 19200:  ctl = CTL_BAUD_19200;  break;
    case 38400:  ctl = CTL_BAUD_38400;  break;
    default:     ctl = CTL_BAUD_9600;   break;  /* Fallback to 9600 if invalid */
    }
    s_tb_latch = (bbs_cfg.baud_rate == 38400) ? TIMERB_LATCH_FAST : TIMERB_LATCH_SLOW;
    ACIA_CTL = ctl;
    /* If the line already shows a caller before we have even raised DTR,
     * it is a session the modem kept across our reset — on the Ultimate,
     * one whose TCP peer is long gone (issue #31), with a byte stuck in the
     * transmitter and no way to answer a real caller. Drop DTR for a second
     * first: standard modem practice, and it tells the firmware to tear
     * the stale session down. Measured on the C64U: a BOOT with the modem
     * in that state used to sit at WFC unable to answer until the Ultimate
     * was rebooted. */
    if ((ACIA_STATUS & 0x40) == 0) {
        acia_set_cmd(CMD_DTR_OFF);
        delay_jiffies(60);
    }
    acia_set_cmd(CMD_DTR_ON);

    acia_puts("ATZ\r");
    acia_puts("ATS0=1\r");

    /* Prime the guard: if DSR is currently inactive (no caller), mark it
     * so the first incoming call (DSR active edge) will trigger connect.
     * If DSR is already active at boot (shouldn't happen), we skip this
     * and connect only after DSR goes inactive then active again. */
    if ((ACIA_STATUS & 0x40) != 0) {
        s_saw_dsr_inactive = TRUE;
    }

    s_acia_up = TRUE;
    return BBS_OK;
}
#ifdef T64_BOOT_OVERLAY
#pragma code(code)
#pragma data(data)
#endif

net_state_t net_state(void) { return s_state; }

static u8 process_inbound(u8 in, u8 *out)
{
    u8 b, app;
    at_event_t e = at_parser_feed(&s_at, in, &b, &app);
    switch (e) {
    case AT_EVT_RING:
        /* ATS0=1 handles auto-answer — no explicit ATA needed. */
        break;
    case AT_EVT_CONNECT:
        /* AT-string connect: used by VICE/tcpser where DSR is never driven.
         * On U64, DSR-based connect fires first (below) and this is a no-op
         * because s_state is already NET_CONNECTED. */
        /* AUTO/VICE: a result code means we're on tcpser → switch to AT-string
         * mode and ignore DSR.  U64 mode is forced to DSR and never switches. */
        if (bbs_cfg.modem_type != MODEM_U64) s_at_mode = TRUE;
        if (s_state == NET_IDLE) {
            s_state = NET_CONNECTED;
            s_at.connected = TRUE;
            telnet_filter_init(&s_iac);
        }
        break;
    case AT_EVT_NOCARRIER:
        s_state = NET_DROPPING;
        break;
    default: break;
    }

    if (!app) return 0;
    if (s_state != NET_CONNECTED) return 0;
    return telnet_filter_feed(&s_iac, in, out);
}

bbs_err_t net_rx(void *buf, u16 want, u16 *got)
{
    /* Transmitter gave up on a dead peer (see acia_putc): hang up properly. */
    if (s_tx_dead) {
        s_tx_dead = FALSE;
        net_disconnect();
    }
    /* DSR-based disconnect (U64 only): only fire if DSR was seen active
     * during this session.  Under VICE/tcpser DSR is never driven, so
     * s_dsr_was_active stays FALSE and this check is skipped — disconnect
     * is handled via AT_EVT_NOCARRIER in process_inbound() instead. */
    if (s_state == NET_CONNECTED && s_dsr_was_active &&
        (ACIA_STATUS & 0x40) != 0) {
        s_state = NET_DROPPING;
        at_parser_init(&s_at);
    }

    /* Track DSR going active while connected (marks U64 mode).  Skipped in
     * AT-string mode: under VICE the DSR bit reads a constant value that would
     * otherwise mislead the disconnect/connect logic below. */
    if (!s_at_mode && s_state == NET_CONNECTED && (ACIA_STATUS & 0x40) == 0) {
        s_dsr_was_active = TRUE;
    }

    /* NET_DROPPING → NET_IDLE.
     * U64 mode (s_dsr_was_active): wait for DSR to go inactive, confirming
     * the TCP session has been torn down, then re-assert DTR.
     * VICE mode (!s_dsr_was_active): DSR is never meaningful; transition
     * immediately — NO CARRIER already signals the call is done. */
    if (s_state == NET_DROPPING) {
        bool_t dsr_active = ((ACIA_STATUS & 0x40) == 0) ? TRUE : FALSE;
        if (!dsr_active || !s_dsr_was_active) {
            s_state = NET_IDLE;
            s_saw_dsr_inactive = TRUE;
        }
    }

    /* DSR-based connect (U64): DSR going active after having been seen inactive.
     * Skipped under VICE (DSR never goes active) — connect is via AT_EVT_CONNECT. */
    {
        bool_t dsr_active = ((ACIA_STATUS & 0x40) == 0) ? TRUE : FALSE;
        if (!dsr_active) s_saw_dsr_inactive = TRUE;
        /* !s_at_mode: never DSR-connect once result codes are in use (VICE) —
         * the constant-active DSR reading would spuriously re-connect right
         * after a NO CARRIER hangup, which is the "stuck connected" bug. */
        if (!s_at_mode && s_state == NET_IDLE && s_saw_dsr_inactive && dsr_active) {
            s_state          = NET_CONNECTED;
            s_at.connected   = TRUE;
            s_dsr_was_active = TRUE;
            telnet_filter_init(&s_iac);
        }
    }

    u8 *p = (u8 *)buf;
    *got = 0;

    /* Drain telnet filter outbound replies first. */
    {
        u8 reply[TELNET_REPLY_MAX];
        u8 r = telnet_filter_take_reply(&s_iac, reply, sizeof(reply));
        for (u8 i = 0; i < r; i++) acia_putc(reply[i]);
    }

    /* Defensively re-arm the Timer-B RX IRQ.  The KERNAL boot/disk path resets
     * the $0314 RAM vector to its default handler ($EA31) AND stops Timer B
     * (CRB $DC0F bit0 clears), so the ISR stops firing and the ring goes cold.
     * Re-arm whenever either has reverted: vector != our ISR, or Timer B is no
     * longer running.  net_irq_setup() does the full SEI-guarded rebuild. */
    if (*(void * volatile *)0x0314 != (void *)acia_irq_isr ||
        (*(volatile u8 *)0xDC0F & 0x01) == 0) {
        net_irq_setup();
    }

    /* Drain the Timer-B-filled RX ring buffer. want=0 is valid: pumps the AT
     * parser without consuming payload (used by the idle WFC poll).  The ISR
     * is the sole producer (s_rx_tail); we are the sole consumer (s_rx_head). */
    while (s_rx_head != s_rx_tail) {
        if (s_state == NET_CONNECTED && *got >= want) break;
        u8 in = s_rx_buf[s_rx_head & RX_BUF_MASK];
        s_rx_head++;
        u8 outbyte;
        if (process_inbound(in, &outbyte) && *got < want)
            p[(*got)++] = outbyte;
    }

    /* Re-assert RTS once the ring has drained below the low-water mark (the
     * ISR deasserts it at high-water). Skipped while a disk hold owns RTS. */
    if (s_hold_depth == 0 && (u8)(s_rx_tail - s_rx_head) < 8u) {
        ACIA_CMD = s_cmd_shadow;
    }

    if (s_state != NET_CONNECTED && *got == 0) return BBS_EAGAIN;
    return BBS_OK;
}

bbs_err_t net_tx(const void *buf, u16 n, u16 *sent)
{
    if (s_state != NET_CONNECTED) { *sent = 0; return BBS_EAGAIN; }
    const u8 *p = (const u8 *)buf;
    u16 i;
    for (i = 0; i < n; i++) {
        if (!acia_putc(p[i])) break;
    }
    *sent = i;
    return (i == n) ? BBS_OK : BBS_EIO;
}

/* Busy-wait n jiffies (~1/60 s each) using the KERNAL jiffy low byte ($A2),
 * which the IRQ keeps ticking.  Counts ticks (wrap-safe); bails if the jiffy
 * stops advancing so a dead IRQ can never hang the BBS. */
static void delay_jiffies(u8 n)
{
    u8  last  = *(volatile u8 *)0x00A2;
    u8  count = 0;
    u16 guard = 0;
    while (count < n) {
        u8 now = *(volatile u8 *)0x00A2;
        if (now != last) { last = now; count++; guard = 0; }
        // cppcheck-suppress knownConditionTrueFalse
        else if (++guard == 0u) break;   /* jiffy frozen → IRQ dead, don't hang */
    }
}

bbs_err_t net_rx_raw(void *buf, u16 want, u16 *got)
{
    /* Re-arm the Timer-B IRQ if KERNAL disk I/O has stopped it. */
    if (*(void * volatile *)0x0314 != (void *)acia_irq_isr ||
        (*(volatile u8 *)0xDC0F & 0x01) == 0) {
        net_irq_setup();
    }

    u8 *p = (u8 *)buf;
    *got = 0;

    while (s_rx_head != s_rx_tail) {
        if (*got >= want) break;
        u8 in = s_rx_buf[s_rx_head & RX_BUF_MASK];
        s_rx_head++;
        /* Telnet binary: 0xFF 0xFF → single 0xFF.  Lone 0xFF passes through. */
        if (in == 0xFF && s_rx_head != s_rx_tail &&
            s_rx_buf[s_rx_head & RX_BUF_MASK] == 0xFF) {
            s_rx_head++;
        }
        p[(*got)++] = in;
    }

    /* Re-assert RTS once the ring drains below the low-water mark, same as
     * net_rx(): the ISR deasserts it at high-water, and without this the raw
     * (Zmodem) path leaves RTS low after the first >16-byte frame, stalling
     * every subsequent inbound frame. Skipped while a disk hold owns RTS. */
    if (s_hold_depth == 0 && (u8)(s_rx_tail - s_rx_head) < 8u) {
        ACIA_CMD = s_cmd_shadow;
    }

    if (s_state != NET_CONNECTED && *got == 0) return BBS_EAGAIN;
    return BBS_OK;
}

bbs_err_t net_tx_raw(const void *buf, u16 n, u16 *sent)
{
    if (s_state != NET_CONNECTED) { *sent = 0; return BBS_EAGAIN; }
    const u8 *p = (const u8 *)buf;
    u16 i;
    for (i = 0; i < n; i++) {
        if (!acia_putc(p[i])) break;
        if (p[i] == 0xFF && !acia_putc(0xFF)) break;   /* telnet binary escape */
    }
    *sent = i;
    return (i == n) ? BBS_OK : BBS_EIO;
}

bbs_err_t net_disconnect(void)
{
    /* VICE/tcpser: raw TCP carries no DTR line, so the DTR drop below won't hang
     * up the caller (the U64 firmware does, via the DSR/DTR hardware lines).
     * Issue the Hayes escape + ATH so tcpser closes the TCP itself.  The guard
     * delays bracket "+++": the escape is only recognized after ~1s of silence
     * before and after it, otherwise it is passed through as data.  Only in
     * AT-string mode (VICE); U64/DSR mode hangs up via the DTR drop below. */
    if (s_at_mode) {
        delay_jiffies(72);          /* ~1.2 s silence before +++ */
        acia_puts("+++");
        delay_jiffies(72);          /* ~1.2 s after +++ → modem enters command mode */
        acia_puts("ATH\r");
        delay_jiffies(18);          /* let ATH be processed */
    }

    /* Drop DTR. U64 sees falling edge and tears down the bridged TCP session.
     * Transition to NET_DROPPING so the polling loop can observe DSR go inactive,
     * then re-assert DTR and move to NET_IDLE ready for the next caller.
     * Under VICE (s_dsr_was_active==FALSE), NET_DROPPING transitions to NET_IDLE
     * immediately since DSR is never driven. */
    /* DTR low for a measurable interval, then back up, right here. It used
     * to go low here and back up at the next net_rx() — milliseconds — and
     * the Ultimate did not act on a pulse that short: it kept its (dead)
     * session, DSR stayed asserted, and the BBS sat at WFC never seeing the
     * inactive edge it needs before it will answer, until the Ultimate was
     * rebooted (issue #31). Half a second is well past what any modem
     * needs, and short enough that a caller arriving during the hangup is
     * rarely refused. */
    acia_set_cmd(CMD_DTR_OFF);
    delay_jiffies(30);
    acia_set_cmd(CMD_DTR_ON);
    s_state              = NET_DROPPING;
    s_tx_stalls          = 0;
    s_tx_dead            = FALSE;
    s_saw_dsr_inactive   = FALSE;
    s_dsr_was_active     = FALSE;
    at_parser_init(&s_at);
    telnet_filter_init(&s_iac);
    /* Flush the RX ring: discard any leftover bytes (old call tail, +++ATH
     * echoes, a partial result code) so they can't corrupt the next call —
     * otherwise the stale CONNECT/RING leaks into the next caller's input. */
    s_rx_head = s_rx_tail;
    return BBS_OK;
}

const char *net_term_type(void)
{
    return telnet_filter_term(&s_iac);
}

u8 net_acia_status(void)
{
    return ACIA_STATUS;
}
