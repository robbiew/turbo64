# TURBO/64 BBS (T/64)

> **Note:** This project is under active development and is not yet feature-complete.

## What This Is: A Functional Anachronism

TURBO/64 BBS is a Commodore 64 BBS written in C for the [Oscar64 compiler](https://github.com/drmortalwombat/oscar64). It targets native `.prg` output for real hardware (including C64 Ultimate) and VICE emulation.

**Current status:** v0.4.0 — login/registration, terminal translation (PETSCII, ANSI/CP437, ASCII), bulletin boards, door programs (run external Oscar64 plug-ins), Punter and Zmodem file transfers, and the "Configure" editor are working. As of v0.4.0, T/64 ships **two builds** — see [Which Build Do I Want?](#which-build-do-i-want) below.

Not working: Private mail, SysOp chat, polls/voting, and lots more remain stubbed.

> **Not a developer?** Download the release zip from the [latest GitHub release](../../releases/latest), pick the build for your hardware below, and jump straight to First-Time Setup.

> 💬 **Join the community on [Discord](https://discord.gg/AkKC2gKKuH)** — for support, feature ideas, or just to chat C64 BBSing.

---

## Which Build Do I Want?

T/64 v0.4.0 ships two binaries built from the same source, differing only in how they
store the user/message/file/door database:

| Your hardware | Build | Media |
|---|---|---|
| C64 Ultimate, **Software IEC** | `BOOT-SIEC.prg` / `CONFIGURE-SIEC.prg` | a folder on the Ultimate's USB stick or SD card — **no disk image mounted** |
| sd2iec / uIEC / 1571 / 1581 / emulated 1581 (incl. VICE) | `BOOT-0.4.0.prg` / `CONFIGURE-0.4.0.prg` | `TURBO64-0.4.0.d81` |

The SIEC binaries deliberately have fixed, version-independent names — CBM filenames
top out at 16 characters, and `CONFIGURE-0.4.0-SIEC` (20) could never load. The version
isn't lost: it's compiled in and shown on the boot screen.

If you're not sure which one you have: if you access your Ultimate's storage as a mounted
USB/SD folder tree through its web UI or menu (not as a `.d81` you attach to a virtual
drive), you're on SoftIEC and want the `-SIEC` build. Everyone else — real 1541/1571/1581
hardware, sd2iec-class devices, or the C64 Ultimate's own **emulated** 1581 drives — wants
the plain build and the `.d81`.

**The SIEC build has two hard requirements:**

1. **An REU enabled** in the Ultimate's menu. SIEC records are REU-backed SEQ files —
   without an REU, every database operation (login, message boards, file areas, doors)
   fails outright. This is stricter than the REL build, which merely degrades without one.
2. **A folder tree produced by [`tools/migrate-d81.py`](tools/migrate-d81.py)**, not a bare
   directory of PRGs. SoftIEC has no REL file support, so the SIEC build reinterprets the
   whole storage model as SEQ files inside per-section folders (`SYSTEM/`, `MSGS/`,
   `FILES/`, `DOORS/`); `migrate-d81.py` is what builds that tree, seeded from the `.d81`'s
   user database and gfiles. See **[Setting up the SoftIEC build](#setting-up-the-softiec-build)**
   below.

**Known gaps in the SIEC build (not present in the REL build):**

- **Free-space displays read 0.** SoftIEC reports no block count, so anywhere T/64 would
  normally show free space on a device, it shows 0 instead. This is cosmetic — writes
  still succeed or fail on their own merits.
- **One flat 16-character namespace per section.** Every SIEC section folder (`SYSTEM/`,
  `MSGS/`, `FILES/`, `DOORS/`) is a single flat directory with CBM's usual 16-character
  filename limit — there's no per-board or per-area subfolder the way a `.d81` partition
  scheme might suggest. This is enough for the record and content files T/64 itself
  manages, but keep it in mind if you're naming uploaded files or door PRGs.

Both builds are otherwise the same BBS — same menus, same features, same session logic.
See [Storage Devices & Partitions](#storage-devices--partitions) for the REL build's
device/partition model, and [Setting up the SoftIEC build](#setting-up-the-softiec-build)
for the SIEC equivalent.

---

## Screenshots

<table>
  <tr>
    <td width="50%"><img src="data/git-screens/PETSCII-1.png" alt="Login screen in PETSCII mode" /><br /><sub><b>Login (PETSCII)</b> — the TURBO/64 logo and handle prompt as seen by a Commodore-mode caller.</sub></td>
    <td width="50%"><img src="data/git-screens/PETSCII-2.png" alt="Main menu in PETSCII mode" /><br /><sub><b>Main menu (PETSCII)</b> — mixed-case art and hotkey commands.</sub></td>
  </tr>
  <tr>
    <td width="50%"><img src="data/git-screens/ANSI-1.png" alt="Main menu in ANSI/CP437 mode" /><br /><sub><b>Main menu (ANSI/CP437)</b> — the same BBS served to an ANSI terminal over telnet.</sub></td>
    <td width="50%"><img src="data/git-screens/boot.png" alt="Local SysOp console" /><br /><sub><b>Local console</b> — the waiting-for-caller screen with live stats and recent callers.</sub></td>
  </tr>
  <tr>
    <td width="50%"><img src="data/git-screens/configure.png" alt="CONFIGURE editor main menu" /><br /><sub><b>CONFIGURE</b> — the on-C64 SysOp editor for boards, users, and config.</sub></td>
    <td width="50%"><img src="data/git-screens/door-config.png" alt="CONFIGURE door program editor" /><br /><sub><b>Door programs (CONFIGURE)</b> — registering an external door plug-in: title, file, device, command key, and access level.</sub></td>
  </tr>
</table>

---

## Requirements

**Hardware / emulator**
- Commodore 64 with SwiftLink/ACIA cartridge at $DE00, **or** C64 Ultimate (C64U) with built-in ACIA, **or** VICE x64sc with tcpser modem bridge
- Two .D81 disk images — one for the BBS, one blank one for message boards (device 9). On hardware that supports partitions, a second partition works instead of a second disk — see [Storage Devices & Partitions](#storage-devices--partitions)
- 16 MB REU required for the message boards and some other features. The C64 Ultimate has one built in, and VICE can be configured with one. The BBS boots and lets you log in without a REU, but reading and posting messages need it. This will probably be a hard-check in the future.

**Developer Build host (macOS / Linux)**
- `oscar64` compiler — `bash tools/install-oscar64.sh`
- `c1541` from VICE package (disk assembly)
- `c64u` deploy tool for C64U — `bash tools/install-c64u.sh`
- `tcpser` for VICE modem emulation — macOS: `brew install tcpser`

---

## Building

```bash
make all          # compile BOOT and CONFIGURE PRGs
make disk         # assemble TURBO64-<ver>.d81 from build output + data/
make disk-with-users  # same, but fetches live user database from C64U first
make c64-siec     # compile BOOT-SIEC.prg (SoftIEC: SEQ+REU records)
make editor-siec  # compile CONFIGURE-SIEC.prg
```

Output: `build/c64/BOOT-<ver>.prg`, `build/c64/CONFIGURE-<ver>.prg`, `build/c64/TURBO64-<ver>.d81`,
`build/c64/siec/BOOT-SIEC.prg`, `build/c64/siec/CONFIGURE-SIEC.prg` (fixed names — the
compiled-in version still shows on the boot screen). The SIEC build has
no disk-image target — see [Setting up the SoftIEC build](#setting-up-the-softiec-build).

See [`tools/README.md`](tools/README.md) for the full reference.

---

## First-Time Setup 

### 1. Mount and boot into CONFIGURE

On the C64, mount `TURBO64-<ver>.D81` on device 8 and load CONFIGURE:

```
LOAD "CONFIGURE-0.4.0",8
RUN
```

### 2. Initialize the files (first time only)

From the CONFIGURE main menu, choose **I — INIT FILES**.

Creates:
- `USR LOG` — user database (100 slots, 30 bytes/record)
- `USR PROF` — extended user profiles (100 slots, 86 bytes/record)
- `CALLERS` — callers log, seeded with one SYSOP entry so the WFC screen has something to display

Record 1 is always the SysOp account — handle `SYSOP`, password `PASS`, access level 5. Change it!

### 3. Configure the BBS

CONFIGURE → **C — CONFIG → 1 SETTINGS**:

| Key | Default | Notes |
|-----|---------|-------|
| `BBSNAME` | `A NEW T/64 BBS` | BBS name shown at login |
| `BBSCITY` | `SOMEWHERE, CA` | Location string |
| `SYSOPNAME` | `JOE SYSOP` | Displayed on BBS |
| `NEWUSERLVL` | `1` | Access level for new registrants (0–4) |
| `ALLOWNEW` | `1` | Allow self-registration |
| `SYSOPSTAT`| ` ` | Default message when users try and chat |
| `PROMPTCUR`| `ON` | Animated prompt cursor |

CONFIGURE → **C - CONFIG → 2 DEVICES**:
| Key | Default | Notes |
|-----|---------|-------|
| `DEV_SYSTEM` | `8` | Device for boot PRG, config, and gfiles |
| `DEV_MSGS` | `9` | Device for message boards (separate drive recommended) |
| `DEV_FILES` | `8` | Device for upload/download areas |
| `DEV_DOORS` | `8` | Device for door programs |

CONFIGURE → **C → 5 MODEM TYPE**: AUTO (DETECT), VICE, U64.

CONFIGURE → **C → 2 BAUD RATE**: 300, 1200, 2400, 9600, 19200, or 38400.

Changes are saved to the `config` SEQ file immediately.

### 4. Set up the message boards disk (device 9)

If configured in `DEVICES`, the BBS reads message boards from a separate disk on `DEV_MSGS` (default device 9). Two options:

Format a blank .D81 and mount it on C64U device 9. Then in CONFIGURE → **M — MSG AREAS → C — CREATE**, add boards. Each board needs a title, read level, and write level. The `BOARDS DIR` REL file and per-board index files are created automatically on first post.

Then mount `BOARDS-<ver>.D81` on C64U device 9.

> ⚠️ The message index format uses 63-byte REL records.

Boards can equally live on a **partition** of the same device rather than a separate
disk — e.g. `DEV_SYSTEM=8;1:` and `DEV_MSGS=8;2:` — on hardware that supports partitions.
See [Storage Devices & Partitions](#storage-devices--partitions) for which devices do.


### 5. Run the BBS

Outside CONFIGURE, on the C64:

```
LOAD "BOOT-0.4.0",8
RUN
```

Expected startup output:

```
TURBO/64 BBS V0.4.0

LOADING SETUP...
  BBS: <your bbs name>
  SYSOP: <your name>

INIT MODEM...
  ACIA STATUS: $10
  DSR: INACTIVE

CHECKING REU...
  REU: 16 MB

CHECKING USR LOG...
  USR LOG: OK

CHECKING FOR REAL TIME CLOCK...
```

If `USR LOG: NOT FOUND` or `USR LOG: EMPTY` appears, return to CONFIGURE and run Init Files.

---

## Configuration Reference

`data/config` is the template bundled into every disk build:

```
BBS_NAME=A NEW T/64 BBS
BBS_CITY=SOMEWHERE, CA
SYSOP_NAME=JOE SYSOP
NEW_USER_LEVEL=1
MIN_CALL_TIME=5
MAX_CALL_TIME=60
DEV_SYSTEM=8
DEV_MSGS=9
DEV_FILES=8
DEV_DOORS=8
MODEM_INIT=ATZ
BAUD_RATE=38400
MODEM_TIMEOUT=255
ALLOW_NEW_USERS=1
ALLOW_UPLOADS=1
```

Device values use the form `<device>;<partition>:<init>` — see
[Storage Devices & Partitions](#storage-devices--partitions) below.

---

## Storage Devices & Partitions

T/64 can put each subsystem on a different device, and on a different **partition**
within that device.

### Device spec format

```
DEV_SYSTEM=8            device 8, partition 0, no init string
DEV_MSGS=8;2:           device 8, partition 2
DEV_FILES=9;1:;CD:BBS   device 9, partition 1, sends "CD:BBS" at startup
```

| Field | Meaning |
|---|---|
| `<device>` | CBM bus device number, 8–11 |
| `<partition>` | Partition on that device. **0 = leave the drive where it is** |
| `<init>` | Optional CBM DOS command sent before disk access. Note the `:;` separator |

The four subsystems are `DEV_SYSTEM` (user database, access levels, caller log),
`DEV_MSGS` (message boards), `DEV_FILES` (file library) and `DEV_DOORS` (door PRGs).
They can share a device or be spread across several.

### How partitions work

When a partition is non-zero, T/64 selects it with the CBM DOS **`CP<n>`** command on
the command channel, then addresses files with the drive-0 form (`0:NAME`). This is the
standard CMD-drive mechanism and it is what sd2iec documents as well.

**Partition 0 sends no `CP` at all.** That keeps behaviour byte-for-byte identical to a
setup that has never touched the field, so leaving everything at 0 is always safe.

> **Note for anyone upgrading from 0.3.0 or earlier:** that field was documented as a
> partition but was being sent as a CBM *drive* number, which a 1581 rejects. Any device
> set to a non-zero value silently failed **every** disk operation on that device — not
> just the obvious one. If you hit that, no data was lost; nothing could open it. Fixed
> in 0.3.1.

### What works on which device

Measured on hardware, each with a control run against a known-good drive:

| Device | REL files (database) | `CP` partitions | Notes |
|---|---|---|---|
| 1541 / 1571 / 1581 | yes | yes (real hardware) | The baseline T/64 targets |
| **C64U emulated 1581** (Drive A/B) | yes | **no** | The emulation rejects `CP`. Use partition 0 |
| **sd2iec / uIEC / SD2IEC** | yes | **yes**, verified isolated | Partitions numbered from 1 |
| **C64U SoftIEC** | **no** | n/a | No REL support, so the database cannot live here |

Two consequences worth planning around:

- **The C64 Ultimate's built-in emulated drives do not support partitions.** Set them to
  partition 0. Partitions need real hardware or an sd2iec-class device.
- **SoftIEC cannot host the database.** T/64's user and message records are CBM relative
  (REL) files, and SoftIEC has no REL support — the open is accepted and every record
  operation then fails. Using SoftIEC for the record database is planned work, not a
  current capability.

### Getting files onto an sd2iec device

If you copy `.prg` files onto the card from a PC or Mac, the C64 will see them with the
extension as **part of the filename** — `BOOT-0.3.1.PRG` rather than `BOOT-0.3.1`. That is
not a fault: sd2iec presents the whole FAT name, and extension hiding is off by default.
It means `LOAD"BOOT-0.3.1",10` fails; you would have to type the `.PRG` too.

**Recommended: copy from the C64 rather than from the PC.** Mount the T/64 `.d81` on one
device and use any C64 file-copy utility that supports two devices (CBM-Command and the
various copiers all work) to copy `BOOT-*.PRG`, `CONFIGURE-*.PRG`, the overlays and the
gfiles across to the sd2iec device. Files created *through* the C64 get proper CBM names,
because sd2iec generates the FAT name itself.

This is also why the BBS's own data files are never affected — `USR LOG`, `BOARDS` and the
rest are created by CONFIGURE through the C64, so they are always named correctly.

**Alternative: turn on extension hiding** on the drive, which strips a `PRG`/`SEQ`/`USR`/
`REL` extension and sets the file type from it:

```
OPEN 15,10,15,"XE+":CLOSE 15     enable extension hiding
OPEN 15,10,15,"XW":CLOSE 15      save it to the drive's EEPROM
```

Under JiffyDOS use `@"XE+"` and `@"XW"` — the quotes are required. Power-cycle the drive
afterwards so the saved setting is re-read. The command is accepted on sd2iec firmware;
whether a given build hides the extension in every listing has not been verified here, so
check with a directory listing before relying on it.

### Diagnostics

If storage behaves unexpectedly, `make diag` builds standalone test programs that run
**the same disk code the BBS does**, so a result is evidence about the real thing:

| Program | Answers |
|---|---|
| `PTEST` | Does `CP<n>` reach the right partition, and are partitions isolated? |
| `RELTEST` | Can this device create, position and read a REL file? |
| `CPTEST` | What status code does `CP<n>` actually return? |
| `DIR` | What is on a given device/partition? (non-destructive) |
| `EXISTS` | Is a specific file present? Reports the raw DOS code |
| `CLEAN` | Scratch T/64's system files from a device/partition |

Start with `PTEST` — it answers in seconds whether a device supports partitions at all.

> **Warning:** pointing any of these at a device number that is **not present** will hang
> the C64 and require a reset. Reading the status channel of an absent device never
> returns; this is KERNAL serial-bus behaviour, not a fault in the tools.

### Known gaps

- **Clock on non-Ultimate hardware.** T/64's time auto-detect only queries an Ultimate's
  RTC through the cartridge port. It does not read the clock from an sd2iec or CMD drive
  even when one is fitted, so on a plain C64 you are prompted for the time at every boot.
- **Free space is not reported** for devices that do not return a block count.

---

## Setting up the SoftIEC build

The SIEC build (`BOOT-SIEC.prg` / `CONFIGURE-SIEC.prg` — fixed, version-independent
names; see [Which Build Do I Want?](#which-build-do-i-want)) never mounts a disk
image. Instead it reads and writes SEQ files inside a folder tree on the Ultimate's own
USB/SD storage, addressed through Software IEC (device 11 by default). That tree has to
exist and be laid out correctly before the SIEC build will boot — building it is what
`tools/migrate-d81.py` does.

**Requirements before you start:**

- An REU enabled in the Ultimate's menu (Settings → REU/GeoRAM). Without one, SIEC builds
  fail every database operation.
- `python3` and VICE's `c1541` tool on the machine you run the migration from (`c1541`
  ships with VICE; `brew install vice` on macOS, or use the copy already required for
  disk-image development).

**What's in the release archive for this:**

```
TURBO64-0.4.0.d81            the REL disk image — used here only as a data SOURCE
tools/migrate-d81.py          the migration tool
include/bbs/version.h         the compiled-in version (shown on the boot screen);
                               SIEC binary names are fixed and don't need this to build
build/c64/FORTUNE.prg         the example door (shared between both builds)
build/c64/siec/
  BOOT-SIEC.prg
  CONFIGURE-SIEC.prg
  ovl_boot.prg  ovl_wfc.prg  ovl_msgs.prg  ovl_doors.prg  ovl_files.prg
  ovl_zmodem.prg  ovl_auth.prg
```

Yes, the SIEC install still needs the `.d81` — not to mount it, but as the **source** of
the user database, access levels, and gfiles that `migrate-d81.py` converts into SEQ
records and copies into the tree it builds. Unzip the release archive keeping this layout
intact (it mirrors the repo's own `tools/` / `include/` / `build/c64/` paths on purpose),
`cd` into the unzipped folder, and run:

```bash
python3 tools/migrate-d81.py TURBO64-0.4.0.d81 siec-tree \
    --base /USB1/TURBO64 --device 11
```

- `siec-tree` — a new, empty output directory; the tool refuses to write into one that
  already has files in it.
- `--base` — the absolute SoftIEC path the tree will live at once it's on the stick.
  `/USB1/...` for a USB stick, `/SD/...` for the SD card.
- `--device` — the SoftIEC device number as T/64 will see it (11 is the Ultimate default).

This produces `siec-tree/` containing `CONFIG`, `ovl_boot.prg`, `BOOT-SIEC.prg`, and
`CONFIGURE-SIEC.prg` at its root, and `SYSTEM/`, `MSGS/`, `FILES/`, `DOORS/` folders
holding the remaining overlays, the migrated records and gfiles (written as lowercase
`usr log.seq`, `callers.seq`, `g.login.seq` and so on — the spelling SoftIEC itself uses,
so the C64's own saves overwrite them instead of creating a second copy), and the example
door. `migrate-d81.py` copies both SIEC binaries automatically — there is no manual copy
step for CONFIGURE.

> **Upgrading a tree made by an earlier build:** those files were written without the
> `.seq` extension, and on hardware a save from CONFIGURE or the BBS creates `callers.seq`
> beside the old `CALLERS` while reads keep opening the old one — every save is lost.
> Redeploy with `tools/deploy.sh siec --clean --execute`, which removes the stale copies.

Then copy the entire contents of `siec-tree/` onto the stick or card at the `--base` path
you gave above (Ultimate web UI, FTP, or however you move files onto its storage), so that
`siec-tree/SYSTEM` becomes `/USB1/TURBO64/SYSTEM`, and so on. **Do not mount anything** on
device 11 at boot — the whole point of SoftIEC is that there is no image to attach.

Boot with:
```
LOAD"BOOT-SIEC",11
RUN
```

To run the SysOp editor over SoftIEC instead:
```
LOAD"CONFIGURE-SIEC",11
RUN
```

If the BBS reports section-folder or REU errors at boot, re-check the REU is enabled and
that the tree landed at exactly the `--base` path you migrated for — a `CONFIG` in the
wrong place is read silently and falls back to compile-time defaults rather than erroring.

---

## Access Levels

| Level | Name | Description |
|-------|------|-------------|
| 0 | Deleted | Banned / deleted account |
| 1 | New | Unvalidated new user (default for registrants) |
| 2 | User | Standard validated user |
| 3 | Power | Power user |
| 4 | Co-SysOp | Co-SysOp |
| 5 | SysOp | Full access; cannot be deleted |

The SysOp account (ID 1) is created by Init Files at level 5. Adjust `NEW_USER_LEVEL` to control what level new registrants receive.

### Per-level limits & flags

Each level also carries daily usage limits and capability flags, edited in **CONFIGURE → Access Levels**:

- **CALLS/DAY** — maximum logins allowed per day for the level.
- **MINS/DAY** — maximum online minutes per day. Enforced live: a time-left banner and a per-second countdown show during the session (see the `%TL` MCI code), and the caller is warned and disconnected when the limit is reached. Levels with the **T** flag are exempt.
- **FLAGS** — capability bits, shown in the editor as a row of letters (a `-` means that flag is off, e.g. `APSJU---`):

| Letter | Flag | Grants |
|--------|------|--------|
| `A` | POST_ANON | post messages anonymously |
| `P` | PAGE_SYSOP | page the SysOp for chat |
| `S` | SEND_MAIL | send private mail |
| `J` | JOIN_POLLS | vote in polls |
| `U` | UPLOAD | upload files |
| `T` | NO_TIME_LIMIT | exempt from MINS/DAY |
| `C` | NO_CALL_LIMIT | exempt from CALLS/DAY |
| `Y` | SYSOP | full SysOp / Co-SysOp access |

These rows live in the `ACCESS` SEQ file on the system disk, one comma-delimited line per level:

```
level,name,calls_per_day,mins_per_day,flags
```

`flags` is the decimal sum of the bit values (`A`=1, `P`=2, `S`=4, `J`=8, `U`=16, `T`=32, `C`=64, `Y`=128). Edit it through CONFIGURE rather than by hand. Overriding the `name` field there also changes the label shown for that level throughout the BBS.

---

## CONFIGURE Menu Reference

```
CONFIGURE MAIN MENU
  I — INIT BBS        Initialize USR LOG, USR PROF, and CALLERS
  U — USER MGMT       List, delete, reset passwords
  M — MSG AREAS       Create/edit/delete message boards
  F — FILE AREAS      Create/edit upload/download areas
  V — VOTE MGMT       Create/edit polls (stub)
  D — DOOR PROGRAMS   Register/edit/delete door programs (see Door Programs below)
  C — CONFIG OPTIONS  Edit BBS name, devices, baud rate
  S — STATISTICS      Show user/area counts (basic)
  Q — QUIT
```

---

## Door Programs

Doors are external programs the BBS loads and runs during a call — games,
utilities, info screens. A door is a separate Oscar64 `.prg` built to load at
`$9700`; the BBS hands it a small SDK (print, read input, display a file, caller
info) and reloads itself when the door exits. They're configured in CONFIGURE
and run from the **`!` (DOOR PROGRAMS)** entry on the main menu, or automatically
at login.

The release disk bundles an example door, **`FORTUNE`** (`FORTUNE.PRG`), so you
can try the system without writing any code.

### Try the example door (released `.d81`)

1. **Boot CONFIGURE** and choose **`D` — DOOR PROGRAMS**, then **`C` (Create)**.
   Fill in:

   | Field | Value | Notes |
   |---|---|---|
   | TITLE | `FORTUNE` | shown in the door menu |
   | FILENAME | `FORTUNE` | the bundled `FORTUNE.PRG` on the disk |
   | DEVICE | `8` | device the door PRG lives on (the release disk is device 8) |
   | DRIVE | `0` | |
   | CMD KEY | `F` | the key that runs it from the menu |
   | MIN LEVEL | `0` | minimum access level |
   | ENABLED | `Y` | must be Y to appear |
   | RUN AT LOGIN | `N` | `Y` runs it automatically after login |

   Save. (Confirm `DEV_DOORS=8` under CONFIG OPTIONS so the BBS reads the door
   table from the same drive — on a single-drive setup that's device 8.)

2. **Run the BBS** and log in. At the main menu press **`!`** (DOOR PROGRAMS) —
   you should see `[F] FORTUNE`. Press **`F`** to run it: it greets you, shows a
   fortune, and returns to the menu on a keypress.

> The door **table** (your registrations) is created by CONFIGURE and lives on
> the disk; it persists between boots of the same disk. Re-assembling a fresh
> disk starts with no doors registered — re-register, or see
> `tools/README.md` (`--seed-doors`) to carry the table across rebuilds.

### Writing your own door

See **[`devkit/README.md`](devkit/README.md)** — the dev kit ships an SDK header
and a one-file build (`make door DOOR=<name>`). Doors get ~10 KB and a small
versioned API; the guide covers the authoring contract and constraints.

---

## Message Boards

Manage boards in CONFIGURE → **M — MSG BOARDS** (List, Create, Edit, Delete).

**Creating a board.** Create asks only for a **title**, then drops you into the
board editor where you set everything else. (Cancelling the editor right after
creating discards the new board.)

**Per-board settings (board editor).** Press the reverse-video hotkey to change
a field:

| Key | Field | Meaning |
|-----|-------|---------|
| `T` | TITLE | Board name (max 16 chars) |
| `L` | LIST ORD | Display order in the BBS (lower sorts first; ties break by id) |
| `R` | READ | Min access level to read (0–5) |
| `W` | WRITE | Min access level to post (0–5) |
| `A` | ANON | Allow anonymous posts (Y/N) |
| `N` | NET | Networked area (Y/N); when on, set `G` NET TAG |
| `O`/`X` | SUBOP | Assign / clear a SubOp by handle |
| `M` | MAX MSG | Per-board message limit (`DEFAULT` = use the system default) |
| `D` | MAX DAYS | Age-prune limit in days (`OFF` = no age pruning) |

Press `S` to save, `C` to cancel.

**Size and age limits.** Two independent limits keep a board from growing without
bound (compile-time defaults in `include/bbs/config.h`):

- **Message count.** A board's `MAX MSG` caps how many messages it keeps.
  `DEFAULT` (0) uses `CFG_MSG_LIMIT_DEFAULT` = **100**. When a board passes its
  limit, the oldest messages are auto-pruned (soft-deleted) in batches of
  `CFG_MSG_PRUNE_BATCH` = **10**. There is also a hard ceiling of
  `CFG_MSG_MAX_PER_BOARD` = **200** messages per board — posting beyond it fails.
- **Age.** `MAX DAYS` soft-deletes messages older than N days. The default is
  `CFG_MSG_AGE_DEFAULT` = **0 = OFF** (no age pruning) until a SysOp sets a day
  count on the board.

So out of the box a board keeps ~100 messages (auto-pruning the oldest above
that, hard stop at 200) with no age limit. Pruning is automatic at runtime;
there is no manual compact/prune step in CONFIGURE.

---

## File Transfers

TURBO/64 supports two transfer protocols through the **`F` (FILES)** entry on the main menu. Files are organised into areas, each with its own device, upload permission level, and listing. When you press `D` or `U`, the BBS prompts `PROTOCOL: (P)UNTER (Z)MODEM (ENTER=CANCEL):`.

### Protocols

**Punter** (P) — the native C64 BBS protocol. If you're calling with **CCGMS**, CGTerm, or another C64 terminal, use Punter. It is compiled directly into the `OVL_FILES` overlay so there is no extra disk load. The implementation sends one 256-byte block at a time with a simple checksum-and-ACK handshake.

**Zmodem** (Z) — better for PC-side clients (SyncTerm, PuTTY with Zmodem support, etc.). The BBS swaps in the `OVL_ZMODEM` overlay (~3.4 KB) for the transfer, then reloads `OVL_FILES` to continue. Uses ZHEX headers, ZCRCG streaming in 255-byte blocks, CRC-16, and the standard five-ZDLE cancel sequence.

### Caller commands (files menu)

| Key | Action |
|-----|--------|
| `L` | List files in the current area |
| `A` | List all areas |
| `+` / `-` | Next / previous area |
| `1`–`9` | Jump to area by number |
| `D` | Download a file (prompts for protocol) |
| `U` | Upload a file (prompts for protocol) |
| `Q` | Quit back to main menu |

### Setting up file areas

**CONFIGURE → F — FILE AREAS → C (Create).** Each area has:

| Field | Notes |
|-------|-------|
| NAME | Area title shown to callers |
| DEVICE | CBM device the files live on (typically 8 or 9) |
| DRIVE | CBM drive number (usually 0) |
| DL LEVEL | Minimum access level to download |
| UL LEVEL | Minimum access level to upload |

Files are stored as ordinary PRG/SEQ entries on the configured device — no special index. Uploaded files appear immediately for download.

### Access control

- **`U` flag** in CONFIGURE → Access Levels enables uploading for that level. Without it, `U` is refused even if `UL LEVEL` passes.  
- **`ALLOW_UPLOADS`** in CONFIG OPTIONS is a global on/off switch; the access flag is checked on top of it.  
- Downloads are gated only by `DL LEVEL` — no per-level flag is required.

### Limitations

- **No file size in Zmodem ZFILE header.** CBM DOS does not expose file size before reading, so the ZFILE header sends the filename only. Most Zmodem clients handle this gracefully (they show `?` for size and rely on ZEOF to terminate).
- **No Zmodem resume.** CBM DOS sequential files cannot seek; a receiver-requested ZRPOS will abort the transfer rather than rewind the file.
- **255-byte disk reads.** `disk_read` is limited to 255 bytes per call; both protocol implementations use back-to-back reads so throughput is unaffected, but each IEC operation is a separate bus transaction at 1541/1571/1581 speed.

---

## Disk File Layout

Files on `TURBO64-<ver>.d81` (device 8, system device):

| Filename | Type | Description |
|----------|------|-------------|
| `boot-<ver>` | PRG | BBS runtime |
| `configure-<ver>` | PRG | SysOp editor |
| `ovl_msgs` | PRG | Bulletin-board overlay |
| `ovl_wfc` | PRG | WFC (waiting-for-call) overlay |
| `ovl_boot` | PRG | Boot/login overlay |
| `ovl_doors` | PRG | Door programs overlay |
| `ovl_files` | PRG | File-area overlay (browse, list, D/U) |
| `ovl_zmodem` | PRG | Zmodem transfer overlay |
| `fortune` | PRG | Example door (FORTUNE) |
| `config` | SEQ | BBS configuration (key=value) |
| `access` | SEQ | Access levels (6 lines, one per level 0–5) |
| `usr log` | REL | User database (30 bytes/record, 100 slots) |
| `usr prof` | REL | Extended user profiles (86 bytes/record, 100 slots) |
| `usr day` | REL | Per-user daily counters (8 bytes/record, 100 slots) |
| `callers` | SEQ | Callers log (fixed-width lines) |
| `status` | SEQ | SysOp status line |
| `syscnt` | SEQ | System call counters |
| `g.login` | SEQ | Login art — generic fallback |
| `g.login 0` | SEQ | Login art — PETSCII |
| `g.login 1 80` | SEQ | Login art — ANSI/CP437 80-col |
| `g.login 2 80` | SEQ | Login art — ASCII 80-col |
| `g.newuser` | SEQ | New user screen — generic fallback |
| `g.term` | SEQ | Terminal selection menu |
| `m.doors` | SEQ | Door programs menu body |
| `m.files` | SEQ | File areas menu body |
| `m.main` | SEQ | Main menu body — generic fallback |
| `m.main 1 80` | SEQ | Main menu body — ANSI/CP437 80-col |
| `m.msgs` | SEQ | Messages menu body — generic fallback |
| `m.msgs 1 80` | SEQ | Messages menu body — ANSI/CP437 80-col |
| `m.read` | SEQ | Read-message display — generic fallback |
| `p.doors` | SEQ | Door programs menu prompt |
| `p.files` | SEQ | File areas menu prompt |
| `p.main` | SEQ | Main menu prompt — generic fallback |
| `p.main 1 80` | SEQ | Main menu prompt — ANSI/CP437 80-col |
| `p.msgs` | SEQ | Messages menu prompt |
| `p.read` | SEQ | Read-message prompt — generic fallback |
| `p.read 1 80` | SEQ | Read-message prompt — ANSI/CP437 80-col |

Files on `BOARDS-<ver>.d81` (device 9, message device):

| Filename | Type | Description |
|----------|------|-------------|
| `boards` | REL | Board directory (44 bytes/record; `CFG_MAX_BOARDS` boards, default 20) |
| `b<n>.idx` | REL | Message index for board N (63 bytes/record, up to 200 msgs) |
| `b<n>.txt` | SEQ | Message body text for board N |
| `usr ptr` | REL | Per-user last-read pointers (`CFG_MAX_BOARDS` × 2 bytes/record, 100 slots) |

### Gfile Naming Convention

Display files follow the pattern `<prefix>.<name> [<mode>] [<width>]`:

- **prefix** — `g` = gfile (display page), `m` = menu body, `p` = prompt
- **mode** — `0`=PETSCII, `1`=ANSI/CP437, `2`=ASCII, 
- **width** — `40` or `80`; omit to match any width

BOOT tries the most specific match first:
```
<prefix>.<name> <mode> <width>  →  <prefix>.<name> <mode>  →  <prefix>.<name> <width>  →  <prefix>.<name>
```

A `p.<name>` file replaces the built-in `COMMAND: ` prompt for that menu. If absent, the built-in text is used.

### Pipe Color Codes

Display files support inline color codes using `|XX` or `\XX` (two-digit decimal 00–15). Both forms are identical — `\` is provided as a C64-native alias because the standard C64 keyboard has no `|` key: the `£` key produces CHR$(92) = ASCII backslash. Codes are translated to the appropriate PETSCII or ANSI escape for each caller's terminal; ASCII callers receive no color output.

| Code | Color | Code | Color |
|------|-------|------|-------|
| `\|00` | Black | `\|08` | Orange |
| `\|01` | White | `\|09` | Brown |
| `\|02` | Red | `\|10` | Light Red |
| `\|03` | Cyan | `\|11` | Dark Gray |
| `\|04` | Purple | `\|12` | Medium Gray |
| `\|05` | Green | `\|13` | Light Green |
| `\|06` | Blue | `\|14` | Light Blue |
| `\|07` | Yellow | `\|15` | Light Gray |
| `\|16` | Reverse on | `\|17` | Reverse off |

Color resets to white-on-black at the end of each display file. `\|17` (reverse off) is emitted automatically at reset; you only need it mid-file to cancel an earlier `\|16`.

> **Authoring on a C64:** prefer the `\XX` form — the `£` key types it directly. The `|` form is handy when authoring on a PC. A more C64-keyboard-friendly color scheme is on the [roadmap](#future-roadmap).

### MCI Substitution Codes

| Token | Replaced with | Active in |
|-------|---------------|-----------|
| `%SN` | Configured BBS (system) name | `g.term` (connect screen) |
| `%SO` | Configured SysOp name | `g.term` (connect screen) |
| `%BN` | Current board name (trimmed) | `p.msgs`, `p.read` |
| `%MN` | Current message number | `p.read` only |
| `%TL` | Caller's time left today (e.g. `10m`; `unlim` if the level is time-exempt) | Any display/prompt file (`g.*`, `m.*`, `p.*`) |

Tokens not yet set expand to an empty string. The time-left value (`%TL`) is recomputed from the live MINS/DAY state each time a file is shown. On PETSCII it is uppercased (`10M` / `UNLIM`) so it stays readable in the graphics charset. Example `p.main`:
```
 |01(%TL LEFT) |03CMD|07?|04:
```

Example `p.read`:
```
 |05MSG#|03%MN |05IN |03%BN
 |03CMD|07?|04:
```

## SysOp "Spy" Mode

The local C64 screen mirrors the caller's session, with a status bar showing handle, access level, time online, and action keys. The view adapts to the caller's terminal type.

### 40-column spy (PETSCII callers)

The caller's output is rendered directly on the C64's 40-column text screen using the C64 character ROM. A five-row panel (rows 20–24) shows user details and quick-action keys.

The spy is **locked to the uppercase/graphics charset** for the entire session — it does not follow callers who switch to lowercase/text. This keeps PETSCII art and the status panel rendering correctly, but means mixed-case text (e.g. a message body) appears as graphics glyphs on the spy. ANSI/ASCII callers are unaffected since they use the 80-column view below.

### 80-column "soft" view (ANSI / ASCII callers)

The VIC-II has no hardware 80-column mode, so TURBO/64 renders a **software 80-column display** into a hi-res bitmap using a custom 4×6-pixel font — 80 columns on a stock C64. This mode is **automatic** when an REU is detected at boot and the caller is 80-column; without an REU it falls back to the 40-column spy.

Internals: VIC switches to hi-res bitmap mode (bank 3, bitmap at `$E000` under KERNAL ROM, colour RAM at `$C000`). A small ANSI parser in the overlay interprets printable text, CR/LF, cursor positioning (`ESC[H`/`ESC[r;cH`/`ESC[A`–`D`), screen clear (`ESC[2J`), and SGR colours (mapped to the VIC palette), rendering into the bitmap. Caller content occupies rows 0–23; row 24 is a reverse-video status bar (handle · access level · time · action keys) that stays visible and ticks once per second.

### SysOp action keys

| Key | Action |
|-----|--------|
| **F2** | Drop the current caller (works from anywhere) |
| **F1** | SysOp chat (scaffolded, not yet wired) |
| **F3** | Change access level (scaffolded, not yet wired) |

### 80-column limitations

- **REU required.** No REU → 80-column callers shown on 40-column spy (clipped to 40 columns).
- **ANSI/ASCII only.** PETSCII callers always use the 40-column spy — the bitmap parser doesn't speak PETSCII.
- **Approximate font.** 4×6 font covers ASCII `0x20`–`0x7E`. CP437 box-drawing chars map to `+ - | #`; fine graphics are lost.
- **Two colours per cell.** One foreground colour per 8×8 cell (two chars), so adjacent differently-coloured characters share the rightmost colour. Background is always black.
- **Scroll pacing.** Shifting ~7 KB of bitmap per scroll; noticeably slower on a stock 1 MHz C64, snappy on C64U. Caller output is not delayed — only the spy rendering slows.
- **Bottom row.** Status bar occupies row 24; the caller's row 25 is not shown.

> **Note:** The 80-column soft view is functional but not extensively tested with complex ANSI art. Help appreciated!

---

## Troubleshooting

**`USR LOG: NOT FOUND` on boot**
Run CONFIGURE → I Init Files. The REL file was never created.

**`USR LOG: EMPTY` on boot**
Record 1 (SysOp) is missing. Run Init Files again.

**`ERROR: MODEM INIT FAILED`**
ACIA not detected at $DE00. On C64U, verify ACIA/SwiftLink is enabled. In VICE, verify `tools/deploy-vice.sh` launched with `-acia1` flags.

**VICE: telnet connection refused**
`tcpser` may not have started. Check that `deploy-vice.sh` output shows `tcpser launched`.

**Double-echo characters in terminal**
The BBS sends `IAC WILL ECHO`; your terminal client should suppress local echo. Use a proper telnet client (not raw TCP).

**Garbled characters (PETSCII/ANSI)**
BOOT auto-detects terminal type via backspace probe. Use a terminal that responds to backspace (PETSCII: C64 terminal; ANSI: SyncTerm, PuTTY). If detection fails, BBS falls back to ASCII.

**`M` from main menu says NO BOARDS**
Device 9 is not mounted or has no boards configured. Mount `BOARDS-<ver>.D81` on device 9 and verify `DEV_MSGS=9` in config. If the disk is blank, use CONFIGURE → M — MSG AREAS to create at least one board.

---

## What Is Not Yet Implemented (v0.4.0)

The following features return a placeholder message:

- Private mail
- SysOp chat / page
- Polls and votes
- Last caller display
- System info
- Grafitti wall 
- Preferences (can't edit)
- Help
- Local chat
- 80 column (ANSI) view message viewer
- WFC F Keys (local login, log viewer, etc.)
- Spy F Keys (Chat, Change Access Level)
- Adapt message area to 80 cols if user selects that mode

Data structures and CONFIGURE admin modules for all of the above are in place.

## Future Roadmap

Things I'd love for TURBO/64 to do:

- Message networking (like Image3 - should D8 be compatible? Not Fidonet)
- Embedded scriping / modding support - let sysops customize the hell out of it
- External strings to easy editing
- A more C64-keyboard-friendly markup scheme for gfiles. Colors currently use `|XX`/`\XX` (the `\` form is already C64-typeable via the `£` key), but the `|` prefix isn't on the C64 keyboard. A dedicated single-key or `%`-style color scheme would make on-C64 authoring easier and keep color markup consistent with the `%`-prefixed MCI codes.

---

## How to Contribute

TURBO/64 is an active project and contributions of all kinds are welcome — you do not need to be a C programmer to help.

### Developers

Can you wrangle a C codebase targeting a 1MHz 6502 while having genuine opinions about which C64 BBS software had the best message base? Do you understand why fitting a feature into a 10 KB overlay segment is a satisfying constraint rather than an annoying one? We could use you.

The stubbed features listed above are the priority. Each one has its data layer and CONFIGURE admin UI in place; what is missing is the caller-facing session logic inside `src/features/`. If you want to pick up a feature, open an issue first to avoid duplication.

Good entry points: `src/features/mail.c`, `src/features/vote.c`, `src/features/callers.c` — all are thin stubs waiting to be wired up on the same pattern as `src/features/bulletin.c`.


**Technical chops:**
- Solid C experience — not "I got it to compile with enough casts"
- Understands the 6502 memory model: zero page, bank switching, why you care about what goes where
- Knows what IEC bus timing means in practice and why CBM DOS REL files are both brilliant and painful
- Can read a linker map and understand why the overlay boundary moved four times in a week
- Bonus: familiarity with oscar64, PETSCII terminal emulation, or CBM DOS internals
- Bonus: experience with modem/ACIA programming or serial protocol implementation

**Cultural fit:**
- Either lived through the C64 scene OR has become genuinely and unironically obsessed with it
- Understands why PETSCII is not just "ASCII but weird" and has feelings about it
- Gets that the 40-column display is a design constraint worth respecting, not working around
- Has opinions about which 1581 partition scheme was least terrible
- Will not suggest porting it to a Raspberry Pi

### C64 / Oscar64 Specialists

The BBS runs on a constrained 6502 with a 37 KB core region and a 10.5 KB overlay zone for the message module. Contributions that reduce code size, tighten the overlay boundary, or improve IEC bus throughput are extremely valuable. Familiarity with oscar64 pragmas (`#pragma code`, `#pragma overlay`, `#pragma region`) and CBM DOS REL files is a plus.

### Artists and SysOps

The BBS ships with bare-bones gfiles. What it needs:

- **PETSCII art** (`g.login 0`, `g.newuser 0`, `m.main 0`, `m.msgs 0`, `p.msgs 0`, etc.) — 40-column PETSCII using pipe/backslash color codes (`|NN` or `\NN`)
- **ANSI/CP437 art** (mode `1`, optionally `1 80` for 80-column) — classic BBS ANSI with the same color code system
- **ASCII variants** (mode `3`) — plain text for terminals that strip color
- **Menu prompt files** (`p.*`) — the one-or-two-line prompts shown at each menu; small but high-impact

Files live in `data/gfiles/` and are assembled directly onto the disk image by `make disk`. No build tools required — edit, copy to the D81, and test live.

#### Period-Correct ANSI Artists

Are you an old-school ANSi artist? Younger and inexplicably drawn to this era and aesthetic? Do you need one more goddamn thing to do? Consider spending some of your valuable free time on this — compensated by nothing more than the unyielding appreciation of the people who enjoy this kind of thing. Gotta be a few of us around, right?

**Style guide:**
- Authentic C64/Compunet-era aesthetic — PETSCII character graphics, the C64's native 40-column canvas, and the distinct visual language that came out of the early European and North American C64 scene
- Work within the C64's 16-color palette; the classic cyan-on-blue, or white-on-black with colored highlights are right at home here — lean into the limitations, don't fight them
- Logo screens and headers built from PETSCII block and graphic characters — the kind of thing that looked like it took three days on a real machine, because it did
- ANSI/CP437 variants (for mode `1`) should feel like they belong on a 1990 warez dist site, not a modern retro tribute — if it could have shipped on a C64 BBS in 1991, you're in the right place
- Group shoutouts, greetz, and handle tags are not only acceptable, they are strongly encouraged

### Testers

Real-hardware and emulator coverage is critical. Specifically needed:

- **SD2IEC** — the BBS uses CBM DOS REL files; SD2IEC REL support has known edge cases. Reports of what works and what does not are very helpful.
- **VICE** — regression testing after each feature addition; `tools/deploy-vice.sh` makes this fast.
- **Real 1541/1571/1581** — timing and IEC bus behaviour differs from emulation and the C64U's built-in drive.
- **Terminal clients** — SyncTerm, PuTTY, netcat, and real C64 terminals over a nullmodem or SwiftLink.

File issues on GitHub with hardware/firmware versions, a description of what you did, and the exact error or unexpected behaviour.

### Chat

Prefer to talk it through? Join the [TURBO/64 Discord](https://discord.gg/AkKC2gKKuH) for support, feature ideas, and general C64 BBS chatter.
