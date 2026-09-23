/* bbs/net.h - single-line modem driver interface.
 *
 * On C64/U64: backed by the 6551 ACIA at $DE00-$DE03 (SwiftLink cartridge).
 * The U64 firmware bridges a TCP telnet connection on port 3000 through the
 * ACIA. With ATS0=1, the firmware auto-answers and asserts DSR when a TCP
 * connection arrives.
 *
 * The driver handles:
 *   - ACIA initialisation (9600 8N1)
 *   - DSR-based carrier detection (more reliable than AT RING parsing at 9600)
 *   - AT response parsing pre-connect (RING/CONNECT/NO CARRIER)
 *   - Telnet IAC negotiation (strips IAC from the byte stream; negotiates
 *     TERMINAL-TYPE so the session can pick the right term_mode_t)
 *   - DTR-drop disconnect (no +++ escape needed)
 */
#ifndef BBS_NET_H
#define BBS_NET_H

#include "bbs/types.h"
#include "bbs/err.h"

typedef enum {
    NET_IDLE      = 0,   /* waiting for RING / DSR assertion */
    NET_RINGING   = 1,   /* RING seen, not yet answered */
    NET_CONNECTED = 2,   /* caller online */
    NET_DROPPING  = 3    /* NO CARRIER / DTR drop, cleaning up */
} net_state_t;

/* Initialise the ACIA and send ATZ + ATS0=1. */
bbs_err_t net_init(void);

/* Arm the CIA Timer-B RX interrupt that polls the ACIA into a ring buffer.
 * Call once after boot disk I/O completes (e.g. at the start of the WFC loop);
 * captures inbound bytes (notably the CONNECT result code) that the main loop
 * would otherwise miss to single-byte-register overrun. */
void net_irq_arm(void);

/* Current modem/connection state. */
net_state_t net_state(void);

/* Non-blocking receive. Reads up to `want` application bytes (after
 * stripping IAC sequences) into `buf`; writes count to `*got`.
 * Also drives state transitions (RING/CONNECT/NO CARRIER/DSR).
 * Returns BBS_EAGAIN if not CONNECTED and nothing was buffered. */
bbs_err_t net_rx(void *buf, u16 want, u16 *got);

/* Non-blocking send. Writes up to `n` bytes to the ACIA TX FIFO.
 * Returns BBS_EAGAIN if not CONNECTED. */
bbs_err_t net_tx(const void *buf, u16 n, u16 *sent);

/* Raw receive for binary protocol transfers (Zmodem).
 * Skips telnet IAC command processing; only undoubles 0xFF 0xFF → 0xFF.
 * Use instead of net_rx() during binary transfers to avoid data corruption. */
bbs_err_t net_rx_raw(void *buf, u16 want, u16 *got);

/* Raw send for binary protocol transfers (Zmodem).
 * Doubles outbound 0xFF bytes as required by telnet binary mode.
 * Use instead of net_tx() during binary transfers. */
bbs_err_t net_tx_raw(const void *buf, u16 n, u16 *sent);

/* Drop the current call by de-asserting DTR. */
bbs_err_t net_disconnect(void);

/* Hardware flow control around IRQ-masked disk I/O. The KERNAL's IEC
 * routines run with interrupts off, so the Timer-B RX poll cannot run and
 * the 6551 (one byte of buffer) overruns on any burst that arrives during a
 * disk operation. net_rx_hold() deasserts RTS so the Ultimate (which honours
 * RTS on its emulated ACIA) holds the caller's bytes; net_rx_release() puts
 * RTS back. Nestable: only the outermost pair touches the line. Both are
 * no-ops until net_init() has brought the ACIA up. The disk HAL calls them
 * around every KERNAL I/O; see issue #27. */
void net_keepalive(void);
void net_rx_hold(void);
void net_rx_release(void);

/* Returns the TERMINAL-TYPE string the remote sent during telnet
 * negotiation, e.g. "ansi", "petscii", "syncterm", or "" if not
 * yet received. Caller must not free or modify. */
const char *net_term_type(void);

/* Returns raw ACIA status byte for diagnostics. */
u8 net_acia_status(void);

#endif /* BBS_NET_H */
