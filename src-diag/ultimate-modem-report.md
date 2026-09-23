# C64 Ultimate: ACIA/SwiftLink modem emulation stops responding after a caller disconnects

Draft of an upstream report for the Ultimate firmware project. Everything below was
measured on the hardware described; nothing is inferred from documentation.

## Environment

- Commodore 64 Ultimate, firmware 1.1.0, FPGA 122, core 1.49
- Modem Settings: Modem Interface `ACIA / SwiftLink`, Hardware Mode `SwiftLink`,
  ACIA mapping `DE00/NMI`, Listening Port 3000, RTS Handshake (Rx) Enabled,
  DSR Behavior `Active when connected`, Drop connection on DTR low Enabled,
  Do RING sequence Enabled, Automatic Rx Pushback Disabled
- C64 side: TURBO/64 BBS (https://github.com/robbiew/turbo64) at 38400 baud,
  polling the 6551 from a CIA timer interrupt; SoftIEC storage on device 11
- Caller: a Python telnet client on a Mac on the same LAN (answers IAC DO with
  WONT), so the exact byte timing below is reproducible

## Symptom

After some caller disconnects, the emulated ACIA stops responding entirely:

- `$DE01` (status) reads `$00` **and stays there** when no byte is pending. `$00` is a
  valid transient on a live 6551 mid-transmit (TDRE momentarily clear), so the diagnostic
  is that it persists in the idle state — the delayed poll below reads it seconds after any
  traffic. Before the event the idle status reads `$50`/`$10` as expected.
- `$DE02` (command) reads `$FF`; writes to it (DTR, RTS) have no observable effect.
- The C64's transmit register is never drained; DSR (status bit 6) reads active with
  no TCP session established.
- New TCP connections to port 3000 are still accepted but nothing is bridged: the
  client receives 0 bytes.
- The REST API, FTP and the rest of the machine keep working. Toggling
  `ACIA (6551) Mapping` or other Modem Settings over `/v1/configs` does not revive it;
  a C64 reset does not either. Only `PUT /v1/machine:reboot` (firmware reboot) restores
  it. Once, the same scenario took the whole unit off the network for ~40 s until its
  watchdog rebooted it.

## Reproduction

Rate is roughly 1 in 5–6 calls with this exact pattern; 20+ calls with the same steps
typed at human speed (80 ms per character) never triggered it.

The controllable conditions (what the client does):
1. Open a TCP connection to port 3000; the BBS answers, asserts DTR, DSR goes active.
2. At the BBS's first prompt send one byte (`2`).
3. At the next prompt write **16 bytes in a single `send()` with no inter-character
   delay** (`ABCDEFGHIJKLMNO` + CR). On this LAN that left the client stack as one TCP
   segment (observed, not required — the trigger is the burst write, not a guaranteed
   packet shape).
4. At the following prompt send one more byte (`N`).
5. About 1.5 s later close the TCP socket from the client (`close()`, no further data).
6. Read `$DE01` ~2 s later. Normal: `$50`. Failure: `$00`, persisting until a firmware
   reboot.

The C64 program's behaviour during step 5 is a hangup on carrier loss: DTR is dropped
for 0.5 s and re-asserted. The same failure was also reproduced with a build that
**never touches RTS** (RTS held asserted throughout), so the RTS handshake is not
involved. It was also reproduced with the client waiting for the BBS's own hangup
(TCP EOF) before closing, so simultaneous close from both ends is not required.

## A second, milder state (for completeness)

Separately, after a client-side close the firmware sometimes keeps the session: DSR
stays active and the transmitter stops draining, but the ACIA is otherwise alive
(status `$10`). That state clears as soon as the C64 holds DTR low for ~0.5 s. It
seems the firmware does not act on a DTR drop shorter than a few milliseconds, and it
does not notice the peer's FIN on its own in this state (it did after 175 s when the
BBS's idle timeout dropped DTR). That one is handled on the C64 side now; the dead-ACIA
state above is not recoverable from the C64.

## What would help

- Any way to reset the modem emulation without a firmware reboot (a REST action, or
  having a Modem Settings change re-initialise it).
- If the `$00`/`$FF` register reads are a known "emulation halted" state, a hint in the
  docs; the boot screen of the BBS now prints "ACIA DEAD? REBOOT THE ULTIMATE" when it
  sees `$00`, but nothing else on the machine indicates the fault.
