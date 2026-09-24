/* zmodem.c — Zmodem file transfer (OVL_ZMODEM, $9700-$BFFF).
 *
 * Minimal Zmodem SEND and RECEIVE:
 *   ZHEX headers; ZCRCG streaming with ZCRCE final block.
 *   CRC-16 bitwise (no table — saves 512 bytes of BSS).
 *   255-byte disk reads (disk_read limit is u8).
 *   No crash-recovery; always start at position 0.
 *
 * All called functions (net_rx_raw, net_tx_raw, disk_*, clock_*,
 * session_emit, sess_carrier_ok) are RESIDENT. */

#include "bbs/overlay.h"
#include "bbs/session.h"
#include "bbs/net.h"
#include "bbs/hal/disk.h"
#include "bbs/hal/clock.h"
#include "net/zmodem.h"

#pragma code(zmodem_code)
#pragma data(zmodem_data)
#pragma bss(zmodem_bss)

#pragma native(zmodem_send)
#pragma native(zmodem_recv)

/* -----------------------------------------------------------------------
 * BSS
 * ----------------------------------------------------------------------- */
static u8  z_rxbuf[255]; /* disk-read / data-packet accumulation buffer */

/* What our ZRINIT advertises. The low 16 bits are the receive buffer size:
 * a sender that sees it (lrzsz, SyncTerm) limits each data subpacket to it
 * and waits for our ZACK after every block. Without it a PC sender streams
 * 1 KB subpackets at line rate into a 128-byte RX ring drained one byte at
 * a time; the ring overran, the CRC failed, and after the retries the
 * transfer died (measured: 2 KB upload from sz, file created, 0 bytes).
 * 255 = sizeof(z_rxbuf), the most one subpacket can hold here anyway. */
/* Flags in ZF0 (the last header byte); receive-buffer length in the first
 * two. CANOVIO is deliberately NOT set: with it the sender streams 1 KB
 * ZCRCG subpackets, but z_recv_data_pkt accumulates into a 255-byte buffer
 * and the RX ring is 128 bytes, so anything over the buffer was dropped
 * (and skipped from the CRC), the subpacket CRC failed, and the sender
 * resent the same oversized block forever. Advertising a 64-byte buffer and
 * no overlap asks a sender to send <=64-byte subpackets and wait for our
 * ZACK — flow the 128-byte ring and the byte-at-a-time drain keep up with.
 * A sender that honours rxbuflen (per the ZMODEM spec) does this; lrzsz sz
 * 0.12.20 does NOT (it uses rxbuflen only for window/ACK cadence and still
 * streams 1 KB subpackets), so an lrzsz upload is not fixed by this alone —
 * the receiver must accept a large streamed subpacket, which is issue #36. */
#define Z_RXBUF 64u
/* CANFDX is required: SyncTerm aborts the handshake on a flagless ZRINIT. A
 * sender that honours the advertised rxbuflen would window (<=64-byte blocks,
 * ZACK-paced) and completely avoid the Ultimate ACIA RTS-resume bug (#36) —
 * proven with a purpose-built windowed sender: 256 B uploaded byte-perfect.
 * But neither lrzsz sz nor SyncTerm honour it; both stream 512-1024 B blocks
 * and rely on RTS, so large uploads still stall on the firmware bug. */
#define ZRINIT_INFO (((u32)CANFDX << 24) | Z_RXBUF)
#define Z_FLUSH 32u   /* data-subpacket flush granularity — see z_recv_data_to_disk */
static u8  z_tx[160];    /* tx staging; flushed via net_tx_raw */
static u8  z_txlen;
static u8  z_cancel_cnt; /* consecutive ZDLE bytes seen (5 = abort) */
static u8  z_rx_tmax = 10; /* z_rx_byte inter-byte timeout, seconds (tunable per phase) */

/* -----------------------------------------------------------------------
 * CRC-16/CCITT — bitwise, no table
 * ----------------------------------------------------------------------- */
static u16 z_crc16_byte(u16 crc, u8 b)
{
    u8 i;
    crc ^= (u16)b << 8;
    for (i = 0; i < 8; i++)
        crc = (crc & 0x8000u) ? (u16)((crc << 1) ^ 0x1021u) : (u16)(crc << 1);
    return crc;
}

/* -----------------------------------------------------------------------
 * TX primitives
 * ----------------------------------------------------------------------- */
static void z_tx_flush(void)
{
    u16 sent;
    if (z_txlen) { net_tx_raw(z_tx, z_txlen, &sent); z_txlen = 0; }
}

static void z_tx_put(u8 b)
{
    z_tx[z_txlen++] = b;
    if (z_txlen >= sizeof(z_tx)) z_tx_flush();
}

/* ZDLE-escape one byte (for data subpackets) */
static void z_tx_esc(u8 b)
{
    if (b == ZDLE || b == 0x11 || b == 0x13 || b == 0x91 || b == 0x93) {
        z_tx_put(ZDLE); z_tx_put(b ^ 0x40);
    } else {
        z_tx_put(b);
    }
}

/* Two lower-case hex digits */
static void z_tx_hex2(u8 b)
{
    static const char hx[] = "0123456789abcdef";
    z_tx_put((u8)hx[b >> 4]);
    z_tx_put((u8)hx[b & 0x0F]);
}

/* ZHEX frame:  ** ZDLE B <hex-type> <8 hex data> <4 hex crc> CR LF XON */
static void z_send_hex_hdr(u8 type, u32 data)
{
    u8  h[5];
    u16 crc;
    h[0] = type;
    h[1] = (u8)( data        & 0xFF);
    h[2] = (u8)((data >>  8) & 0xFF);
    h[3] = (u8)((data >> 16) & 0xFF);
    h[4] = (u8)((data >> 24) & 0xFF);
    crc  = z_crc16_byte(z_crc16_byte(z_crc16_byte(
             z_crc16_byte(z_crc16_byte(0, h[0]), h[1]), h[2]), h[3]), h[4]);
    z_tx_put(ZPAD); z_tx_put(ZPAD); z_tx_put(ZDLE); z_tx_put(ZHEX);
    z_tx_hex2(h[0]);
    z_tx_hex2(h[1]); z_tx_hex2(h[2]);
    z_tx_hex2(h[3]); z_tx_hex2(h[4]);
    z_tx_hex2((u8)(crc >> 8)); z_tx_hex2((u8)(crc & 0xFF));
    z_tx_put(0x0D); z_tx_put(0x0A); z_tx_put(0x11); /* CR LF XON */
    z_tx_flush();
}

/* Data subpacket:  [escaped data] ZDLE <marker> [escaped crc-hi crc-lo] */
static void z_send_data_pkt(const u8 *buf, u8 len, u8 marker)
{
    u8  i;
    u16 crc = 0;
    u8  ch, cl;
    for (i = 0; i < len; i++) {
        crc = z_crc16_byte(crc, buf[i]);
        z_tx_esc(buf[i]);
    }
    crc = z_crc16_byte(crc, marker);
    z_tx_put(ZDLE); z_tx_put(marker);
    ch = (u8)(crc >> 8); cl = (u8)(crc & 0xFF);
    z_tx_esc(ch); z_tx_esc(cl);
    z_tx_flush();
}

/* -----------------------------------------------------------------------
 * RX primitives
 * ----------------------------------------------------------------------- */

/* Returns raw byte (0-255), -1 on timeout/loss, -2 on cancel sequence */
static i16 z_rx_byte(const session_t *s)
{
    clock_tod_t t0; clock_read(&t0);
    for (;;) {
        u16 got; u8 b;
        net_rx_raw(&b, 1, &got);
        if (got) {
            if (b == ZDLE) {
                if (++z_cancel_cnt >= 5) return -2;
            } else {
                z_cancel_cnt = 0;
            }
            return (i16)(u16)b;
        }
        if (!sess_carrier_ok(s)) return -1;
        { clock_tod_t tn; clock_read(&tn);
          if (clock_elapsed(&t0, &tn) >= (u16)z_rx_tmax) return -1; }
    }
}

/* One hex-encoded byte (two nibble chars) */
static i16 z_rx_hex_byte(const session_t *s)
{
    i16 c, hi, lo;
    c = z_rx_byte(s); if (c < 0) return c;
    if      (c >= '0' && c <= '9') hi = c - '0';
    else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
    else return -1;
    c = z_rx_byte(s); if (c < 0) return c;
    if      (c >= '0' && c <= '9') lo = c - '0';
    else if (c >= 'a' && c <= 'f') lo = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') lo = c - 'A' + 10;
    else return -1;
    return (i16)((hi << 4) | lo);
}

/* -----------------------------------------------------------------------
 * z_recv_header — hunt for and parse one Zmodem frame header.
 * Returns frame type (0-19), -1 on timeout/error, -2 on cancel.
 * *pos receives the 4-byte position/info value (little-endian u32).
 * ----------------------------------------------------------------------- */
static i16 z_recv_header(const session_t *s, u32 *pos)
{
    i16 c;
    u8  h[5], i;
    u16 crc_got, crc_calc;

hunt:
    do {
        c = z_rx_byte(s);
        if (c == -2) return -2;
        if (c <   0) return -1;
    } while ((u8)c != ZPAD);
    do { c = z_rx_byte(s); if (c < 0) return c; } while ((u8)c == ZPAD);
    if ((u8)c != ZDLE) goto hunt;

    c = z_rx_byte(s); if (c < 0) return c;

    if ((u8)c == ZHEX) {
        for (i = 0; i < 5; i++) {
            i16 b = z_rx_hex_byte(s); if (b < 0) return -1;
            h[i] = (u8)b;
        }
        { i16 hi2 = z_rx_hex_byte(s); if (hi2 < 0) return -1;
          i16 lo  = z_rx_hex_byte(s); if (lo  < 0) return -1;
          crc_got = (u16)(((u16)(u8)hi2 << 8) | (u8)lo); }
        /* drain trailing CR LF (and optional XON — tolerate missing) */
        { i16 d; d = z_rx_byte(s); (void)d; d = z_rx_byte(s); (void)d; }
    } else if ((u8)c == ZBIN) {
        /* ZBIN: 5 raw bytes (ZDLE-escaped), then 2 CRC bytes (ZDLE-escaped) */
        for (i = 0; i < 5; i++) {
            c = z_rx_byte(s); if (c < 0) return c;
            if ((u8)c == ZDLE) {
                c = z_rx_byte(s); if (c < 0) return c;
                h[i] = (u8)c ^ 0x40;
            } else { h[i] = (u8)c; }
        }
        { u8 ch, cl;
          c = z_rx_byte(s); if (c < 0) return c;
          ch = ((u8)c == ZDLE) ? (c = z_rx_byte(s), (u8)c ^ 0x40) : (u8)c;
          c = z_rx_byte(s); if (c < 0) return c;
          cl = ((u8)c == ZDLE) ? (c = z_rx_byte(s), (u8)c ^ 0x40) : (u8)c;
          crc_got = (u16)(((u16)ch << 8) | cl); }
    } else {
        goto hunt;
    }

    crc_calc = z_crc16_byte(z_crc16_byte(z_crc16_byte(
                 z_crc16_byte(z_crc16_byte(0, h[0]), h[1]), h[2]), h[3]), h[4]);
    if (crc_calc != crc_got) goto hunt;

    *pos = (u32)h[1] | ((u32)h[2] << 8) | ((u32)h[3] << 16) | ((u32)h[4] << 24);
    return (i16)(u16)h[0];
}

/* -----------------------------------------------------------------------
 * z_recv_data_pkt — receive one Zmodem data subpacket.
 * Accumulates unescaped bytes into buf[0..bufsize-1].
 * Returns byte count on success (0-bufsize).
 * Sets *marker to ZCRCE/ZCRCG/ZCRCQ/ZCRCW.
 * Returns -1 timeout, -2 cancel, -3 CRC error.
 * ----------------------------------------------------------------------- */
static i16 z_recv_data_pkt(const session_t *s, u8 *buf, u8 bufsize, u8 *marker)
{
    u16 crc = 0, len = 0;
    for (;;) {
        i16 c;
        c = z_rx_byte(s); if (c < 0) return c;
        if ((u8)c != ZDLE) {
            if (len < bufsize) { crc = z_crc16_byte(crc, (u8)c); buf[len++] = (u8)c; }
            continue;
        }
        /* ZDLE: next byte is escaped or a subpacket end marker */
        c = z_rx_byte(s); if (c < 0) return c;
        {
            u8 eb = (u8)c;
            if (eb == ZCRCE || eb == ZCRCG || eb == ZCRCQ || eb == ZCRCW) {
                u8 ch2, cl2;
                crc = z_crc16_byte(crc, eb);
                *marker = eb;
                /* Receive CRC-16 (2 bytes, possibly escaped) */
                c = z_rx_byte(s); if (c < 0) return c;
                ch2 = ((u8)c == ZDLE) ? (c = z_rx_byte(s), (u8)c ^ 0x40) : (u8)c;
                c = z_rx_byte(s); if (c < 0) return c;
                cl2 = ((u8)c == ZDLE) ? (c = z_rx_byte(s), (u8)c ^ 0x40) : (u8)c;
                if (crc != (u16)(((u16)ch2 << 8) | cl2)) return -3;
                return (i16)len;
            }
            /* Escaped data byte */
            {
                u8 raw = eb ^ 0x40;
                if (len < bufsize) { crc = z_crc16_byte(crc, raw); buf[len++] = raw; }
            }
        }
    }
}

/* Write-through subpacket receiver: each data byte goes straight to the open
 * disk file instead of a RAM buffer, so a streamed 1 KB subpacket cannot
 * overflow anything (z_rxbuf is 255, the RX ring 128) — the per-byte
 * disk_putc() drops RTS while it writes, which pauses the sender and keeps
 * the ring drained. CRC is accumulated on the fly. *nbytes gets the byte
 * count (for fpos), *marker the closing marker. Returns 0 ok, -1 timeout,
 * -2 cancel, -3 CRC error (bytes already on disk — caller must truncate and
 * restart), -4 disk write error. */
static i16 z_recv_data_to_disk(const session_t *s, u16 *nbytes, u8 *marker)
{
    u16 crc = 0, n = 0;
    u8  blk = 0;                 /* bytes buffered in z_rxbuf awaiting a flush */
    for (;;) {
        i16 c = z_rx_byte(s);
        u8  raw;
        if (c < 0) { *nbytes = n; return (i16)c; }
        if ((u8)c != ZDLE) {
            raw = (u8)c;
        } else {
            c = z_rx_byte(s); if (c < 0) { *nbytes = n; return (i16)c; }
            {
                u8 eb = (u8)c;
                if (eb == ZCRCE || eb == ZCRCG || eb == ZCRCQ || eb == ZCRCW) {
                    u8 ch2, cl2;
                    crc = z_crc16_byte(crc, eb);
                    *marker = eb;
                    c = z_rx_byte(s); if (c < 0) { *nbytes = n; return (i16)c; }
                    ch2 = ((u8)c == ZDLE) ? (c = z_rx_byte(s), (u8)c ^ 0x40) : (u8)c;
                    c = z_rx_byte(s); if (c < 0) { *nbytes = n; return (i16)c; }
                    cl2 = ((u8)c == ZDLE) ? (c = z_rx_byte(s), (u8)c ^ 0x40) : (u8)c;
                    *nbytes = n;
                    if (crc != (u16)(((u16)ch2 << 8) | cl2)) return -3;  /* leave blk unwritten */
                    if (blk && disk_write(z_rxbuf, blk) != BBS_OK) return -4;
                    return 0;
                }
                raw = eb ^ 0x40;   /* escaped data byte */
            }
        }
        crc = z_crc16_byte(crc, raw);
        z_rxbuf[blk++] = raw; n++;
        /* Flush well before the 128-byte RX ring can fill. During accumulation
         * RTS is high and the C64 (barely keeping up with 38400 while doing
         * CRC per byte) falls behind; measured, the ring overran ~120 bytes
         * into a streamed subpacket. disk_write holds RTS for its duration,
         * pausing the sender so the ring drains, so flushing every 32 bytes
         * keeps the backlog well under the ring. */
        if (blk == Z_FLUSH) {
            if (disk_write(z_rxbuf, blk) != BBS_OK) { *nbytes = n; return -4; }
            blk = 0;
        }
    }
}

/* Send 8 × CAN to abort */
static void z_send_cancel(void)
{
    u8 i;
    for (i = 0; i < 8; i++) z_tx_put(ZDLE);
    for (i = 0; i < 8; i++) z_tx_put(0x08);
    z_tx_flush();
}

/* -----------------------------------------------------------------------
 * zmodem_send — BBS → caller
 *
 *  ZRQINIT → ZRINIT → ZFILE+data → ZRPOS → ZDATA+data-pkts → ZEOF
 *  → ZRINIT → ZFIN
 * ----------------------------------------------------------------------- */
__noinline zmodem_result_t zmodem_send(const session_t *s, u8 device, u8 drive,
                                        const char *filename)
{
    i16  frame;
    u32  pos;
    u32  fpos = 0;
    u8   retries;
    u8   fnamebuf[30];
    u8   fi;

    z_cancel_cnt = 0; z_txlen = 0;

    if (disk_open(device, drive, filename, DISK_READ) != BBS_OK) {
        session_emit(s, "\r\nFILE NOT FOUND.\r\n");
        return ZMODEM_ERR;
    }

    /* No capital C in either banner: lrzsz (sz/rz, and the clients built on
     * it) treat a 'C' arriving before the first Zmodem frame as the
     * XMODEM/YMODEM "send in CRC mode" request and switch protocol on the
     * spot. Measured: with "ZMODEM RECV - CTRL-X TO CANCEL" printed first,
     * sz answered with YMODEM blocks and the transfer never started. */
    session_emit(s, "\r\nZMODEM SEND - START YOUR DOWNLOAD (X TO STOP)\r\n");

    /* Prompt receiver with ZRQINIT, wait for ZRINIT */
    frame = 0;
    for (retries = 0; retries < 4; retries++) {
        z_send_hex_hdr(ZRQINIT, 0);
        frame = z_recv_header(s, &pos);
        if (frame == ZRINIT) break;
        if (frame == -2) { disk_close(); z_send_cancel(); return ZMODEM_CANCEL; }
        if (frame == ZABORT) { disk_close(); return ZMODEM_CANCEL; }
    }
    if (frame != ZRINIT) {
        disk_close();
        session_emit(s, "\r\nNO RESPONSE FROM RECEIVER.\r\n");
        return ZMODEM_ERR;
    }

    /* ZFILE: header + data subpacket with "filename\0\0" */
    fi = 0;
    while (fi < 28 && filename[fi]) { fnamebuf[fi] = (u8)filename[fi]; fi++; }
    fnamebuf[fi++] = 0; fnamebuf[fi++] = 0; /* NUL-terminate + empty size info */
    z_send_hex_hdr(ZFILE, 0);
    z_send_data_pkt(fnamebuf, fi, ZCRCW);

    /* Wait for ZRPOS */
    frame = 0;
    for (retries = 0; retries < 5; retries++) {
        frame = z_recv_header(s, &pos);
        if (frame == ZRPOS) break;
        if (frame == ZSKIP) { disk_close(); return ZMODEM_OK; }
        if (frame == ZABORT || frame == -2) { disk_close(); z_send_cancel(); return ZMODEM_CANCEL; }
    }
    if (frame != ZRPOS) {
        disk_close();
        session_emit(s, "\r\nNO ZRPOS FROM RECEIVER.\r\n");
        return ZMODEM_ERR;
    }

    /* Send ZDATA then stream file in 255-byte chunks with ZCRCG.
     * Last chunk uses ZCRCE.  On CRC error: receiver sends ZRPOS;
     * since we can't seek CBM sequential files, any ZRPOS aborts. */
    z_send_hex_hdr(ZDATA, fpos);

    for (;;) {
        i16 nread = disk_read(z_rxbuf, 255);
        if (nread <= 0) nread = 0;

        {
            bool_t is_last = ((u8)nread < 255) || disk_eof();
            u8     marker  = is_last ? ZCRCE : ZCRCG;

            if (nread > 0) {
                z_send_data_pkt(z_rxbuf, (u8)nread, marker);
                fpos += (u32)(u8)nread;
            }

            if (is_last) break;
        }
    }

    /* ZEOF */
    z_send_hex_hdr(ZEOF, fpos);
    disk_close();

    /* Wait for ZRINIT (receiver ack'd EOF) */
    for (retries = 0; retries < 10; retries++) {
        frame = z_recv_header(s, &pos);
        if (frame == ZRINIT) break;
        if (frame == -2) { z_send_cancel(); return ZMODEM_CANCEL; }
        if (frame == ZABORT) return ZMODEM_CANCEL;
        if (frame == ZRPOS) {
            /* Receiver wants retry — we already closed, just re-ZEOF */
            z_send_hex_hdr(ZEOF, fpos);
        }
    }

    /* ZFIN + wait for "OO" */
    z_send_hex_hdr(ZFIN, 0);
    { i16 a = z_rx_byte(s); i16 b = z_rx_byte(s); (void)a; (void)b; }

    session_emit(s, "\r\nTRANSFER COMPLETE.\r\n");
    return ZMODEM_OK;
}

/* -----------------------------------------------------------------------
 * zmodem_recv — caller → BBS
 *
 *  ZRINIT → ZFILE → ZRPOS=0 → ZDATA → data-pkts → (ZCRCE) → ZRINIT → ZFIN → OO
 * ----------------------------------------------------------------------- */
__noinline zmodem_result_t zmodem_recv(const session_t *s, u8 device, u8 drive,
                                        const char *filename)
{
    i16    frame;
    u32    pos;
    u8     marker;
    i16    pkt_len;
    u32    fpos = 0;
    u8     retries;
    clock_tod_t t0, tlast, tnow;
    const char *dest;

    z_cancel_cnt = 0; z_txlen = 0;

    session_emit(s, "\r\nZMODEM - START YOUR UPLOAD NOW (X TO STOP)\r\n");

    /* Wait for ZFILE.  A Zmodem receiver drives the handshake by *repeatedly*
       announcing ZRINIT — the sender's ZRQINIT is only a prompt, and a sender
       proceeds to ZFILE the moment it sees any ZRINIT.  Emitting it once (and
       only re-emitting on a cleanly-parsed ZRQINIT) loses the race: our lone
       ZRINIT fires before the sender is listening, and if its ZRQINIT drops a
       byte we never answer.
       BUT the re-announce must be gated on a WALL CLOCK, never sent once per
       loop iteration: z_recv_header can return immediately (spurious/echoed RX
       bytes with nothing from the client), and an unconditional per-iteration
       send then spins at line rate — a flood that crashes the Ultimate ACIA.
       So announce at most once every Z_ZRINIT_GAP seconds, give up after
       Z_HELLO_SECS total. */
    clock_read(&t0); tlast = t0;
    z_send_hex_hdr(ZRINIT, ZRINIT_INFO);
    z_rx_tmax = 3;
    frame = 0;
    for (;;) {
        frame = z_recv_header(s, &pos);
        if (frame == ZFILE) break;
        if (frame == ZFIN) { z_rx_tmax = 10; z_send_hex_hdr(ZRINIT, 0); return ZMODEM_OK; }
        if (frame == ZABORT || frame == -2) { z_rx_tmax = 10; z_send_cancel(); return ZMODEM_CANCEL; }
        if (!sess_carrier_ok(s)) { frame = -1; break; }
        clock_read(&tnow);
        if (clock_elapsed(&t0, &tnow) >= 60u) { frame = -1; break; }
        if (clock_elapsed(&tlast, &tnow) >= 3u) {
            z_send_hex_hdr(ZRINIT, ZRINIT_INFO);
            tlast = tnow;
        }
    }
    z_rx_tmax = 10;
    if (frame != ZFILE) {
        session_emit(s, "\r\nNO FILE FROM SENDER.\r\n");
        return ZMODEM_ERR;
    }

    /* Receive ZFILE data subpacket (filename in z_rxbuf) */
    pkt_len = z_recv_data_pkt(s, z_rxbuf, sizeof(z_rxbuf), &marker);
    if (pkt_len < 0) { z_send_cancel(); return ZMODEM_ERR; }

    /* Open destination file: use sysop-supplied name or sender's name */
    dest = (filename && filename[0]) ? filename : (const char *)z_rxbuf;
    if (disk_open(device, drive, dest, DISK_OVER) != BBS_OK) {
        session_emit(s, "\r\nCANNOT OPEN FILE.\r\n");
        z_send_cancel(); return ZMODEM_ERR;
    }

    /* Data phase. The sender streams ZCRCG subpackets which we write straight
     * to disk (z_recv_data_to_disk) — no buffer big enough to hold a 1 KB
     * subpacket is needed, and the per-byte disk write's RTS hold paces the
     * sender. A ZRPOS(fpos) restarts the file from a known offset; since a
     * SEQ file cannot be rewound, a CRC error truncates and restarts from 0.
     * Structured as: position ONCE, read a ZDATA header, stream subpackets to
     * the end of the frame, then read the next header (another ZDATA, or ZEOF
     * to finish). ZRPOS is sent only to (re)position — once at the start, and
     * again after a CRC error resets fpos to 0. It must NOT be re-sent on every
     * data frame: ZRPOS(fpos) at end-of-data means "resend from fpos", and a
     * sender that has just sent ZEOF (fpos == EOF) rejects it as an invalid
     * position and aborts (SyncTerm: "Received INVALID ZRPOS offset"). */
    z_send_hex_hdr(ZRPOS, fpos);                 /* initial position (0) */
    for (;;) {                                   /* data frames */
        for (retries = 0; retries < 8; retries++) {   /* wait for ZDATA/ZEOF */
            frame = z_recv_header(s, &pos);
            if (frame == ZDATA || frame == ZEOF) break;
            if (frame == ZABORT || frame == -2) { disk_close(); z_send_cancel(); return ZMODEM_CANCEL; }
            if (frame == ZFIN) { frame = ZEOF; break; }
        }
        if (frame == ZEOF) break;                /* no (more) data — done */
        if (frame != ZDATA) {
            disk_close();
            session_emit(s, "\r\nNO DATA FROM SENDER.\r\n");
            return ZMODEM_ERR;
        }

        /* Duplicate frame: ZDATA whose position is behind what we already have
         * on disk. A SEQ file cannot be rewound, so we cannot overwrite — but
         * appending it would duplicate the data (measured: when the ACIA RTS
         * bug delayed our ZEOF ack, SyncTerm resent the whole 31-byte file ~8x
         * and the receiver appended each copy → a 248-byte file). Read the
         * frame to the RAM scratch buffer and DISCARD it so fpos does not move;
         * the sender keeps retrying until its ZEOF finally gets our ZRINIT. */
        if (pos < fpos) {
            for (;;) {
                u8 dm;
                i16 dr = z_recv_data_pkt(s, z_rxbuf, sizeof(z_rxbuf), &dm);
                if (dr == -2) { disk_close(); z_send_cancel(); return ZMODEM_CANCEL; }
                if (dr < 0) break;                       /* timeout/CRC: drop it */
                if (dm == ZCRCW || dm == ZCRCE) break;   /* end of this frame */
            }
            continue;                                    /* wait for next header */
        }

        for (;;) {                               /* subpackets in this frame */
            u16 n = 0;
            i16 r = z_recv_data_to_disk(s, &n, &marker);
            fpos += (u32)n;

            if (r == -2) { disk_close(); z_send_cancel(); return ZMODEM_CANCEL; }
            if (r == -4) {
                disk_close(); z_send_cancel();
                session_emit(s, "\r\nDISK WRITE ERROR.\r\n");
                return ZMODEM_ERR;
            }
            if (r == -3) {
                /* Bad subpacket — its bytes are already on disk and a SEQ file
                 * cannot be rewound, so discard the whole file and restart at
                 * 0. On a clean TCP path this does not happen. */
                if (disk_open(device, drive, dest, DISK_OVER) != BBS_OK) {
                    disk_close(); z_send_cancel();
                    session_emit(s, "\r\nRECEIVE ERROR.\r\n");
                    return ZMODEM_ERR;
                }
                fpos = 0;
                z_send_hex_hdr(ZRPOS, 0);        /* reposition sender to 0 */
                break;                           /* -> outer loop waits for ZDATA */
            }
            if (r < 0) {
                char msg[40];
                disk_close();
                sprintf(msg, "\r\nRECEIVE ERROR AT %lu.\r\n", (unsigned long)fpos);
                session_emit(s, msg);
                return ZMODEM_ERR;
            }

            if (marker == ZCRCQ || marker == ZCRCW) z_send_hex_hdr(ZACK, fpos);
            if (marker == ZCRCW || marker == ZCRCE) break;   /* frame end -> next header */
            /* ZCRCG / ZCRCQ: more subpackets follow with no header */
        }
        if (marker == ZCRCE || marker == ZCRCW) {
            /* end of a data frame; loop reads the next header (ZDATA or ZEOF).
             * fpos is where we are, so the ZRPOS at the top just confirms it. */
        }
    }

    disk_close();

    /* Ready for next file (or ZFIN) */
    z_send_hex_hdr(ZRINIT, ZRINIT_INFO);

    /* Wait for ZFIN */
    for (retries = 0; retries < 10; retries++) {
        frame = z_recv_header(s, &pos);
        if (frame == ZFIN) break;
        if (frame == ZFILE) {
            /* Batch mode: skip extra files — we only handle one */
            { u8 m2; z_recv_data_pkt(s, z_rxbuf, sizeof(z_rxbuf), &m2); }
            z_send_hex_hdr(ZSKIP, 0);
            continue;
        }
        if (frame == ZABORT || frame == -2) { z_send_cancel(); return ZMODEM_CANCEL; }
    }

    z_tx_put('O'); z_tx_put('O'); z_tx_flush();

    {
        char msg[40];
        sprintf(msg, "\r\nTRANSFER COMPLETE (%lu BYTES).\r\n", (unsigned long)fpos);
        session_emit(s, msg);
    }
    return ZMODEM_OK;
}
