# Storage diagnostics

Standalone C64 programs that exercise **the real T/64 HAL** against a device, for
questions only hardware can answer. Build with `make diag`; the PRGs land in
`build/c64/`. Each asks for a device number (8–11) and prints results on screen.

| Program | Answers |
|---|---|
| `PTEST` | Does `CP<n>` + `0:` reach the right partition, and are partitions isolated? |
| `RELTEST` | Can this device create, position and read a REL file? |
| `CPTEST` | What status code does `CP<n>` actually return, per device? |
| `DIR` | What is on a given device/partition? (non-destructive) |
| `EXISTS` | Is a specific file present? Reports the raw DOS code |
| `CLEAN` | Scratch T/64's system files from one device/partition |
| `WIPE` | Scratch **every** file on a device/partition (destructive) |
| `COPYALL` | Copy the T/64 program set between devices, through the C64 |
| `SIECPROBE` | Throwaway probe: the SoftIEC SEQ backend's four correctness gates and five cost measurements — see `docs/probe-results/PHASE0.md` |
| `SEQTEST` | Exercises `rel_seq.c`/`seq_region.c` (the `T64_STORE_SEQ` REL-over-SEQ backend) directly |
| `USRREAD` | USR LOG raw-open vs `rel_seq` read, standalone — found the boot data-loss bug (see `USRSWEEP`) |
| `USRSWEEP` | Reproduces the BOOT-SIEC boot order (cfg fields, `require_storage`, sweep) around a USR LOG read, to isolate what breaks it |
| `SEQNAME` | Does a SoftIEC content file need a host `.SEQ` extension to open? |
| `CFGREAD` | Does `cfg_init()` actually populate the section paths before the USR LOG read — the boot-order difference `USRSWEEP` couldn't isolate on its own |

`PTEST` through `SIECPROBE` link `src/hal/disk.c` and `src/hal/rel.c` directly; `SEQTEST`
through `CFGREAD` link the `T64_STORE_SEQ` backend (`rel_seq.c`, `seq_region.c`, `reu.c`)
instead — either way, a pass here is evidence about the code the BBS actually runs, not a
reimplementation that might diverge.

`EXISTS` exists because directory listings wrap at 40 columns and truncate names — it
answers "is this file here" from the DOS status code instead of parsed text. Note **64**
(FILE TYPE MISMATCH) means the file IS present: it only occurs when a REL file is opened
as SEQ. **62** is the absent case.

`CLEAN` removes only the system-file set (USR LOG, USR PROF, ACCESS, CALLERS, STATUS,
SYSCNT, USR.DAY). It deliberately leaves message-base files (BOARDS, USR.PTR, B<n>.IDX)
alone, so it is safe to run on a device whose message base you want to keep. Use it when a
config change re-points a subsystem and leaves orphans behind on the old device — stale
files in the wrong place are an easy way to misread a later test.

`COPYALL` exists because files copied onto an sd2iec card from a PC keep their FAT
extension as part of the CBM filename (`BOOT-0.3.1.PRG`), while files written through the
C64 get proper names — sd2iec generates the FAT name itself. It copies program and content
files only and deliberately skips `CONFIG`, `CALLERS` and `ACCESS`: those are per-install
data, and overwriting a SysOp's device configuration during a program update is
destructive. It needs two logical file numbers open at once, which T/64's own disk HAL
cannot do since it keeps a single data channel.

## Measured results (2026-08-01)

Commodore 64 Ultimate, firmware 1.1.0. Every result paired with a control run against
a known-good device.

| | Ultimate emulated 1581 (dev 8) | SoftIEC (dev 11) | uIEC/sd2iec (dev 10) |
|---|---|---|---|
| REL files | works | **fails** (DOS 61) | works |
| `CP<n>` partition select | **rejected** | n/a | accepted |
| Partition isolation | n/a | n/a | **verified** |
| SEQ files | works | works | works |

Two consequences worth remembering:

- The Ultimate's **emulated 1581 does not implement `CP`**, so partitions are unusable
  there. Real 1581 hardware and sd2iec both support it.
- **SoftIEC has no REL support**, which is why the record database cannot live there
  without the REU/SEQ work (Phase D). sd2iec *does* support REL, so T/64 runs on
  sd2iec hardware today.

## Two traps these programs exist to avoid

Both were hit for real while writing them, and both produced confident wrong answers
until a control run exposed them.

**1. Never read the status channel while a REL file is open.** `disk_status()` closes
and reopens logical file 15 — the command channel `rel_position()` seeks through.
Calling it between `rel_open()` and `rel_close()` makes a perfectly good 1581 report
"REL BROKEN". Collect results, print after closing. Same defect exists in `store_cd()`'s
history and is documented in `src/hal/rel.c`.

**2. `disk_open()` returning `BBS_OK` does not mean the file exists.** KERNAL OPEN on
the IEC bus succeeds on any device that answers, whatever DOS thought of the request.
Only the DOS status code tells the truth — 62 is FILE NOT FOUND. Any test that keys on
the open's return value rather than the status code is measuring nothing.

## Hazard

**Pointing these at a device number that is not present will hang the C64** and require
a reset. Reading the status channel of an absent device never returns. A
`KRNIO_NODEVICE` guard was tried and does **not** prevent this — it was measured
ineffective on both JiffyDOS and stock kernals. Confirm the device number first.

Separately, on the test machine a **JiffyDOS kernal hung on a uIEC at device 10** with
the card both inserted and removed; the stock kernal worked. Cause not established.
If a device seems absent, try a stock kernal before concluding anything.
