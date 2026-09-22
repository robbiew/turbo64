This is an early pre-release for testing and feedback. It is incomplete and
file transfers in particular need more real-world testing before they can be
considered stable. Expect bugs — please report them on GitHub.

  https://github.com/robbiew/turbo64/issues

v__VERSION__ adds a second, complete build of the BBS for the C64 Ultimate's
Software IEC storage — no disk image, records live as SEQ files in folders
on the USB stick or SD card. It also carries four fixes worth knowing about
even if you never touch SoftIEC.


The big one: a SoftIEC build
-----------------------------
T/64 now ships **two binaries**:

  BOOT-__VERSION__.prg / CONFIGURE-__VERSION__.prg
    For 1541/1571/1581, sd2iec/uIEC, or the C64 Ultimate's emulated 1581.
    Unchanged from prior releases: mount TURBO64-__VERSION__.d81, as before.

  BOOT-SIEC.prg / CONFIGURE-SIEC.prg
    For the C64 Ultimate's Software IEC (device 11 by default). No disk
    image is mounted — the BBS reads and writes SEQ files in a folder tree
    on the Ultimate's own USB/SD storage instead. These two names are
    fixed and version-independent, unlike the pair above — CBM filenames
    top out at 16 characters, and a version-suffixed CONFIGURE name
    overflows that as soon as the version string grows (this shipped
    briefly as CONFIGURE-__VERSION__-SIEC.prg internally, 20 characters,
    and could never load — see "Fixed in v__VERSION__" below). The
    version isn't lost: it's compiled in and shown on the boot screen.

SoftIEC has no REL file support, which is the file format T/64's entire
database (users, boards, messages, file areas, doors) was built on. Rather
than a REL-to-SEQ translation layer, the SIEC build reinterprets storage
from the ground up: an REU-backed SEQ file per record set, addressed
through per-section folders (SYSTEM/, MSGS/, FILES/, DOORS/) instead of
CBM DOS partitions.

**Setting it up requires one extra step.** A SoftIEC install cannot be
built by hand — `tools/migrate-d81.py` (shipped in this release) converts a
seeded .d81 into the folder tree the SIEC build expects, then you copy that
tree onto the stick. The .d81 is still needed even though nothing mounts
it: it's the data source the migration reads the user database, access
levels, and gfiles from. Full walkthrough in the README's "Setting up the
SoftIEC build" section — the short version:

    python3 tools/migrate-d81.py TURBO64-__VERSION__.d81 siec-tree \
        --base /USB1/TURBO64 --device 11

**Hard requirement: an REU enabled in the Ultimate's menu.** SIEC records
are REU-backed. Without an REU, every database operation fails outright —
this is stricter than the REL build, which merely degrades without one.

**Known gaps in the SIEC build:**
- Free-space displays read 0 — SoftIEC reports no block count.
- Each section (SYSTEM/MSGS/FILES/DOORS) is one flat 16-character
  namespace; there's no per-board or per-area subfolder.

Verified on hardware this release: BOOT, CONFIGURE, the overlays, menus,
config, user database, message base, file areas and the example door all
running from a single Ultimate over Software IEC — nothing mounted.


Fixed in v__VERSION__
---------------------

**CONFIGURE could corrupt BASIC on exit.** CONFIGURE's uninitialized data
sits inside the same memory window as the BASIC ROM, which is banked out
at startup so that space is available. Exiting the old way — a plain
return into BASIC — resumed BASIC's workspace pointers as if that window
were still ROM, and on hardware that came back as an infinite
"?FORMULA TOO COMPLEX" error storm the moment you typed anything after
GOODBYE. CONFIGURE now forces a full BASIC cold start on the way out
instead of trying to resume, which reinitializes those pointers from
scratch. Verified on hardware in both builds: clean banner, 38911 bytes
free.

**Exiting CONFIGURE could strand your drive on the wrong partition or
folder.** The admin menus (message boards, file areas, doors) leave the
drive cursor wherever the last operation parked it, and nothing moved it
back on the way out. Reported on hardware: after editing boards and
quitting CONFIGURE, a physical sd2iec was left sitting on partition 2 while
the BBS itself lived on partition 1 — the next LOAD failed until you
manually returned the drive. CONFIGURE (and the BBS itself) now reset the
drive to "home" — the system device/partition, or the SoftIEC tree root —
on every exit path.

**COPYALL could silently report success on a truncated copy.** The
diagnostic's copy loop treated any short read as a clean end-of-file, so a
real mid-transfer error (timeout, checksum, a dropped device) truncated the
destination file and still printed it as copied. It now checks the actual
drive status after each read and write instead of guessing from the byte
count, so a bad copy is reported as a failure — and retries once before
giving up.

**A SoftIEC install's DOORS folder could ship empty.** The example door
program wasn't being copied into the SoftIEC tree's DOORS/ folder during
migration, so the DOORS feature had nothing to run even on an otherwise
complete install — pressing the door key just failed. `migrate-d81.py` now
copies it in automatically, and refuses to produce an incomplete tree
silently — it fails loudly and tells you to `make door-example` first if
the PRG isn't where it expects.

**CONFIGURE-__VERSION__-SIEC.prg could not be loaded at all.** CBM
filenames cap out at 16 characters; that name was 20, so a SoftIEC SysOp
had no way to configure their BBS — LOAD always came back ?FILE NOT FOUND.
BOOT-SIEC.prg and CONFIGURE-SIEC.prg are now fixed, version-independent
names (dropping the version suffix the REL binaries still carry), so this
cannot recur when the version string grows. `migrate-d81.py` also now
copies CONFIGURE-SIEC.prg into the tree automatically, alongside BOOT-SIEC.prg.


Storage: what works on which device
-----------------------------------
Measured on hardware, each with a control run against a known-good drive:

  1541 / 1571 / 1581       REL yes,  partitions yes (real hardware)
  C64U emulated 1581       REL yes,  partitions NO — the emulation
                                     rejects CP; use partition 0
  sd2iec / uIEC / SD2IEC   REL yes,  partitions yes, isolation verified
  C64U SoftIEC             REL NO  — use the -SIEC build instead

The C64 Ultimate's built-in emulated drives do not support partitions. Set
them to partition 0. Partitions need real hardware or an sd2iec-class
device.

Copying files onto an sd2iec device: copy them **from the C64** with a
file-copy utility rather than from a PC. Files copied onto the card from a
PC keep their FAT extension as part of the name, so the C64 sees
"BOOT-__VERSION__.PRG" and LOAD"BOOT-__VERSION__" fails. Files created
through the C64 get proper CBM names. (Alternatively enable extension
hiding on the drive with XE+ then XW.) The BBS's own data files are never
affected — CONFIGURE creates them through the C64, so they are always
named correctly.

If storage behaves unexpectedly, the source tree carries a set of
standalone diagnostics that run the same disk code the BBS does — PTEST,
RELTEST, CPTEST, DIR, EXISTS, CLEAN, WIPE and COPYALL, plus several
SoftIEC-specific probes. They are not included in this archive, since some
of them delete files; build them with `make diag`. Note that pointing any
of them at a device that is not present will hang the C64 and require a
reset — that is KERNAL serial-bus behaviour, not a fault in the tools.

See the README for full details, including the SoftIEC setup walkthrough.


Known gaps
----------
- Reading the current date and time automatically still only works through
  an Ultimate's RTC on the cartridge port. It does not read the clock from
  an sd2iec or CMD drive even when one is fitted, so on a plain C64 you are
  prompted for the time at every boot.
- Free space is not reported for devices that give no block count — this
  now includes SoftIEC, which never reports one.
- SoftIEC's SYSTEM/MSGS/FILES/DOORS sections are each one flat
  16-character namespace, with no per-board or per-area subfolder.


What works in v__VERSION__:

- Login and new-user registration
- Terminal auto-detection (PETSCII, ANSI/CP437, ASCII)
- Bulletin boards (read, post)
- Door programs (run external plug-ins; dev kit for authors)
- File transfers — upload and download via Punter and Zmodem (needs testing)
- SysOp Configure editor (users, boards, doors, file areas, config, access)
- 80-column SysOp spy mode (ANSI callers, REU required)
- SwiftLink/ACIA modem support, VICE tcpser bridge
- Two storage backends: CBM DOS REL files (any supported drive) or
  SoftIEC SEQ+REU folders (C64 Ultimate only)

What is NOT yet working:

- Private mail
- SysOp chat / page
- Polls and votes
- Several smaller features (see README.txt)

Installation
------------
REL build (disk image — most hardware):
1. Mount TURBO64-__VERSION__.d81 on device 8
2. LOAD "CONFIGURE-__VERSION__",8
3. RUN — choose I (INIT FILES) for first-time setup
4. LOAD "BOOT-__VERSION__",8
5. RUN

SoftIEC build (C64 Ultimate only — no disk image):
1. Run tools/migrate-d81.py against TURBO64-__VERSION__.d81 to build a
   folder tree (see README — "Setting up the SoftIEC build")
2. Copy the tree's contents onto the Ultimate's USB/SD storage
3. LOAD "BOOT-SIEC",11
4. RUN

For full instructions see README.txt inside the archive.
