# TURBO/64 BBS — Development Tools

Build, test, and deploy scripts for TURBO/64 BBS. All tools live in `tools/` and can be run from the project root.

---

## Quick Start

### Test in VICE

```bash
tools/build.sh vice           # build + assemble disk + launch VICE
tools/build.sh vice -f        # fullscreen
```

### Deploy to Ultimate64

```bash
tools/build.sh u64            # build + assemble fresh disk + deploy
tools/build.sh u64 --seed-users   # preserve existing user data
tools/build.sh u64 -l sd     # deploy to SD card
```

---

## Tool Reference

### `build.sh` — Master Launcher

Orchestrates build, disk assembly, and deployment in one command.

```bash
tools/build.sh <command> [options]
```

| Command | Description |
|---------|-------------|
| `vice` | Build binaries, assemble disk, launch VICE |
| `u64` | Build binaries, assemble disk, deploy to U64 |
| `disk` | Assemble disk image only (no binary rebuild) |
| `build` | Compile BOOT and CONFIGURE PRGs only |
| `clean` | Remove all build artifacts |
| `help` | Show usage |

**User database options** (for `u64` and `disk` commands):

| Option | Effect |
|--------|--------|
| `--fetch-users` | Fetch live USR LOG/PROF from U64 → `data/users-seed.d81`, then assemble |
| `--seed-users` | Assemble using existing `data/users-seed.d81` |
| `--seed-doors` | Assemble from `data/doors-seed.d81` — carries the registered DOORS table forward too (implies seeding). Create it with `capture-doors-seed.sh`. |

Neither flag → fresh disk (prompts for confirmation before wiping user data on U64).

`vice` also accepts `--fresh-users`, `--no-build`, `--jiffydos`, `--trace`,
`--no-tcpser`, `--tcpser-port`, `-f`, `-p`, `-d`, `--no-autostart`, `-c` (all
forwarded to `deploy-vice.sh`); `u64` accepts `-l/--location` and `--boards`
(forwarded to `deploy-u64.sh`). Run `tools/build.sh help` for the full list.

**Examples:**

```bash
tools/build.sh vice                   # build and launch VICE
tools/build.sh vice -f                # fullscreen
tools/build.sh vice --seed-doors      # ...and carry the registered doors forward
tools/build.sh u64                    # fresh disk (prompts)
tools/build.sh u64 -l sd             # deploy to SD card
tools/build.sh u64 --fetch-users     # preserve live user DB
tools/build.sh u64 --seed-users      # restore from data/users-seed.d81
tools/build.sh u64 --seed-doors -l usb1  # deploy with users + doors table
tools/build.sh disk --seed-users     # assemble only, with user data
tools/build.sh build                 # compile BOOT-<ver>.prg + CONFIGURE-<ver>.prg
tools/build.sh clean                 # wipe build/
```

**Environment:**
```
VICE_CMD=x64sc     # override VICE binary (default: x64sc)
T64_SD_PATH=...     # override U64 deployment path
```

---

### `deploy-vice.sh` — VICE Launcher

Launches VICE with the BBS disk image, a tcpser modem bridge, and a DS12C887 RTC cartridge at `$D700` (avoids conflict with the ACIA at `$DE00`).

```bash
tools/deploy-vice.sh [options]
```

| Option | Description |
|--------|-------------|
| `-f, --fullscreen` | Launch fullscreen |
| `-p, --paused` | Pause on startup |
| `-d, --debugger` | Enable VICE debugger window |
| `--no-autostart` | Don't auto-load the disk |
| `-c, --config <path>` | Use custom VICE config file |
| `--no-tcpser` | Skip modem bridge |
| `--tcpser-port <port>` | Telnet port for tcpser (default: 6400) |

tcpser bridges VICE's virtual SwiftLink/ACIA to a telnet port. Connect from any terminal:
```bash
telnet localhost 6400
```

**Environment:**
```
VICE_CMD=x64sc    # override VICE binary
TCPSER_CMD=tcpser # override tcpser binary
```

---

### `assemble-d81.sh` — Disk Image Assembly

Assembles `build/c64/TURBO64-<ver>.d81` from compiled PRGs, `data/config`, and `data/gfiles/`.

```bash
tools/assemble-d81.sh [options] [version]
```

| Option | Description |
|--------|-------------|
| `--seed-users <path>` | Use existing `.d81` as base, preserving USR LOG and USR PROF |
| `--fetch-users` | Fetch live disk from U64 via `fetch-u64.sh`, use as seed |
| `[version]` | Override version string (default: read from `include/bbs/version.h`) |

If `data/users-seed.d81` is present it is used as the seed automatically (no flag required).

**Output** — `build/c64/`:
```
TURBO64-<ver>.d81          bootable BBS disk image
BOOT-<ver>.prg        main BBS runtime
CONFIGURE-<ver>.prg   SysOp editor
ovl_msgs.prg          message module overlay
ovl_wfc.prg           WFC display overlay
```

---

### `deploy-u64.sh` — Ultimate64 Deployment

Uploads `TURBO64-<ver>.d81` to U64 storage via `c64u`. Optionally also restores the boards seed disk.

```bash
tools/deploy-u64.sh [options]
```

| Option | Description |
|--------|-------------|
| `-l, --location <loc>` | `usb1` (default), `usb0`, `sd`, `bbs` (→ `/BBS`), or a full path |
| `--boards` | Also upload `data/boards-seed.d81` as `BOARDS-<ver>.D81` |
| `-h, --help` | Show help |

```bash
tools/deploy-u64.sh                  # deploy to /USB1/BBS/
tools/deploy-u64.sh -l sd           # deploy to /SD/BBS/
tools/deploy-u64.sh -l bbs          # deploy to /BBS/
tools/deploy-u64.sh --boards -l bbs # also restore boards disk
```

On U64 after deploying:
```
@0 "TURBO64-0.2.0.D81"
LOAD "BOOT-0.2.0",8
RUN
```

**Requires:** `vendor/c64u/bin/c64u` — run `tools/install-c64u.sh` if missing.

**Environment:** `T64_SD_PATH` overrides default path.

---

### `migrate-d81.py` — Convert a .d81 to the SoftIEC Folder Tree

Reads a seeded `.d81` and writes the flat SEQ folder tree the SoftIEC
(`T64_STORE_SEQ`) build reads: REL sets trimmed and converted to SEQ, ACCESS/
CALLERS/gfiles/menus/prompts copied over, and the SIEC binaries (`ovl_*.prg`,
`BOOT-SIEC.prg`, `CONFIGURE-SIEC.prg`) copied in from `build/c64/siec/`.
Non-destructive — the source `.d81` is never modified. `BOOT-SIEC.prg` /
`CONFIGURE-SIEC.prg` are fixed, version-independent names — see
[the naming rationale](../README.md#which-build-do-i-want).

```bash
python3 tools/migrate-d81.py <image.d81> <outdir> [options]
```

| Option | Description |
|--------|-------------|
| `--base <path>` | SoftIEC Default Path the tree will live at (default: `/USB1/TURBO64`) |
| `--device <n>` | SoftIEC bus id written into `CONFIG` (default: `11`) |
| `--siec-build-dir <path>` | Where `make c64-siec` put its output (default: `build/c64/siec`) |
| `--allow-incomplete` | Write the tree even if a SIEC binary is missing (will not boot) |
| `--c1541 <path>` | Override the `c1541` tool (env `C1541`) |

By default the tool **fails loudly** (exit 1) if any overlay, `BOOT-SIEC.prg`,
or `CONFIGURE-SIEC.prg` is missing from `--siec-build-dir`, printing
`make c64-siec && make editor-siec` as the fix — a tree that looks complete
but is missing a boot artifact (or has no way to run the SysOp editor) fails
silently on hardware, which is the exact trap this tool exists to close.
`--allow-incomplete` is an explicit opt-out for inspecting the data layout
only, not a way to produce a deployable tree.

**Output layout:**
```
<outdir>/config.seq, ovl_boot.prg, BOOT-SIEC.prg, CONFIGURE-SIEC.prg
<outdir>/SYSTEM/  other 6 overlays, usr log.seq, usr prof.seq, access.seq,
                  callers.seq, T64.SIEC, all gfiles/menus/prompts (*.seq)
<outdir>/MSGS/    T64.SIEC (+ usr.ptr.seq, boards.seq, b<n>.idx.seq, b<n>.txt.seq)
<outdir>/FILES/   T64.SIEC (+ uds.seq, ud<n>.seq)
<outdir>/DOORS/   T64.SIEC (+ doors.seq)
```
CONFIG and ovl_boot.prg must be at the root: `main()` loads `OVL_BOOT` and
`cfg_init()` reads `CONFIG` before any section path is registered, using
whatever directory the KERNAL cursor is already in.

---

### `deploy.sh` — Build, Deploy, and Launch to Any Test Target

One command to build, deploy, and (best-effort) launch against any of the
three test targets: `d81` (device 8, emulated 1581), `siec` (device 11,
SoftIEC folder tree), `uiec` (device 10, physical uIEC/sd2iec — staged via
device 8 and driven through `COPYALL`, since its media is not network
reachable).

```bash
tools/deploy.sh <d81|siec|uiec> [options]
```

**Safe by default:** every `c64u` call is printed, not executed, unless
`--execute` is passed. Review the printed commands before pointing this at
real hardware.

| Option | Description |
|--------|-------------|
| `--execute` | Actually run the `c64u` / upload calls (default: dry run) |
| `--launch` | Best-effort launch after deploying (see script header for per-target reliability notes — `siec` in particular is not reliable; the exact manual boot commands are always printed) |
| `--no-build` | Skip the `make` step |
| `--seed <d81>` | (d81/uiec) seed disk for the user DB (default: `data/users-seed.d81`) |
| `--drive <a\|b>` | (d81/uiec) internal drive slot |
| `--device <n>` / `--base <path>` | (siec) SoftIEC bus id / Default Path (env `T64_SIEC_DEVICE` / `T64_SIEC_BASE`) |
| `--clean` | (siec only) tidy `--base` before uploading: remove old BOOT/ovl binaries, `src-diag/` diagnostics and probe scratch; **rename** data files an older migrator wrote without `.seq` to the spelling the C64 uses (they are the live data); **stop** if a file and its `.seq` twin are both present (see below) |
| `--yes` | Skip `--clean`'s interactive delete confirmation |
| `--reset-data` | (siec only) upload the seed's data files (user database, boards, message index/bodies, file areas, doors, counters) **over** ones already on the device. By default they are left alone and only binaries, overlays, gfiles and the config are refreshed, so a redeploy updates a live install without resetting it |

Run `tools/deploy.sh --help` for the full per-target breakdown, including
why `c64u runners run-prg` cannot launch the `siec` target at all (it
forces device 8 and truncates the path).

**`--clean` (siec only):** SoftIEC derives the CBM filename by stripping a
host-side type-marker extension and ignores case, so `CALLERS` beside
`callers.seq` presents the SAME CBM name. Measured on hardware (C64 Ultimate,
firmware 1.1.0): the C64 always *writes* lowercase `<name>.seq`; an
extensionless file only ever comes from the PC; with both present the C64
*reads* the extensionless one and a scratch removes both. So on a tree the
older migrator wrote, the extensionless `USR LOG` is the live user database.
`--clean` therefore never deletes a data file: it renames the old spelling to
the `.seq` one, and if both spellings of one file are present it reports a
CONFLICT and stops before uploading anything. `migrate-d81.py` itself now
writes every data file (and `config.seq`) in the `.seq` spelling.
`--clean` classifies (via `tools/siec_clean.py`)
everything currently under `--base` as safe-to-remove or must-keep, and
defaults to keeping: anything not positively matched by a remove rule is
reported unrecognized and left alone. User/message/file-area data (`USR
LOG`, `ACCESS`, `BOARDS*`, `UDS*`, ...) is never touched. A dry run makes
no network calls — it only prints the rules and the local manifest; pass
`--execute` to fetch the live listing, see exact per-file decisions, and
(after typed confirmation, or `--yes`) delete. After uploading, it re-lists
the tree and diffs it against the manifest to catch both leftovers and
upload failures.

---

### `fetch-u64.sh` — Download Live BBS Disk

Downloads the live `TURBO64-<ver>.D81` from U64 to the project root (or a specified path). Useful for inspecting the live disk without modifying it.

```bash
tools/fetch-u64.sh [options]
```

| Option | Description |
|--------|-------------|
| `-l, --location <loc>` | Source location: `usb1` (default), `sd`, or full path |
| `-o, --output <path>` | Local destination (default: `<root>/TURBO64-<ver>.D81`) |

**Environment:** `T64_SD_PATH` overrides default source path.

---

### `extract-users.sh` — Snapshot User Database from U64

Fetches the live BBS disk from U64 and extracts user database files. Saves a seed disk for future builds.

```bash
tools/extract-users.sh [options]
```

| Option | Description |
|--------|-------------|
| `-l, --location <loc>` | Source location: `usb1` (default), `sd`, or full path |
| `-d, --disk <path>` | Use a local `.d81` instead of fetching from U64 |

**Outputs to `data/`:**
- `users-seed.d81` — full disk image; auto-used by `assemble-d81.sh` when present
- `usr_log` — raw USR LOG bytes (flat, no CBM overhead)
- `usr_prof` — raw USR PROF bytes

---

### `capture-doors-seed.sh` — Snapshot the DOORS Table into a Seed

Saves a disk that has door programs registered (via CONFIGURE → DOOR PROGRAMS)
to `data/doors-seed.d81`, so `build.sh ... --seed-doors` carries the door table
forward and you never re-register. The DOORS table is a real CBM REL created by
the BBS; this snapshots the whole `.d81` (same idea as `users-seed.d81`), and
`--seed-doors` uses it as the assemble base — preserving its REL files (USR
LOG/PROF + DOORS) while rebuilding the PRGs.

```bash
tools/capture-doors-seed.sh                 # from the VICE working disk (build/c64/TURBO64-<ver>.d81)
tools/capture-doors-seed.sh path/to.d81     # from a specific local image
tools/capture-doors-seed.sh --from-u64      # fetch the live U64 disk first (default /USB1/BBS)
tools/capture-doors-seed.sh --from-u64 -l sd
```

Verifies a `doors` REL is present before writing the seed. Typical flow: register
a door once, quit VICE (or run on the U64), capture, then build with
`--seed-doors`. `data/doors-seed.d81` is gitignored by default (`*.D81`); to
commit it like `users-seed.d81`, `git add -f data/doors-seed.d81`.

---

### `install-c64u.sh` — Install c64u CLI

Downloads and installs the `c64u` Ultimate64 filesystem tool into `vendor/c64u/bin/c64u`. Idempotent — exits immediately if already installed. Auto-detects platform (Darwin/Linux, arm64/x86_64).

```bash
tools/install-c64u.sh
```

---

### `install-oscar64.sh` — Build Oscar64 Compiler

Clones Oscar64 and builds it into `vendor/oscar64/`. Idempotent.

```bash
tools/install-oscar64.sh
```

Requires: `gcc`, `make`, `git`. Clones from `https://github.com/drmortalwombat/oscar64.git` (shallow). Installs to `vendor/oscar64/bin/oscar64`.

---

### `lint.sh` — Static Analysis

Runs `cppcheck` on C source with C99, warnings, style, performance, and portability checks. Exits 1 on error.

```bash
tools/lint.sh
```

Install cppcheck: `brew install cppcheck` / `sudo apt install cppcheck`.

---

### `release.sh` — GitHub Release Pipeline

Builds all release artifacts, generates FILE_ID.DIZ and README.txt from templates, bundles them into a single ZIP, and publishes a GitHub release via `gh`.

```bash
tools/release.sh              # full release: build, tag, push, publish
tools/release.sh --dry-run    # build and stage artifacts only, no tag/push/publish
tools/release.sh --skip-tag   # publish to an existing tag (no new tag created)
tools/release.sh --force      # delete existing release and recreate from scratch
```

| Option | Description |
|--------|-------------|
| `--dry-run` | Build artifacts, create ZIP, show planned steps — no tag, push, or publish |
| `--skip-tag` | Build and publish to an existing tag — no git tag created or pushed |
| `--force` | Delete the existing GitHub release for this version and recreate it |
| `-h, --help` | Show usage |

`--force` can be combined with `--skip-tag` to replace a release on an already-pushed tag (e.g. `tools/release.sh --force --skip-tag`).

**Requires:** `gh` CLI installed and authenticated (`gh auth login`), `zip`, `oscar64` compiler, and `c1541` available.

**Release asset:** A single `TURBO64-<ver>.zip` containing:

- `TURBO64-<ver>.d81` — bootable BBS system disk
- `BOOT-<ver>.prg` — standalone BBS runtime
- `CONFIGURE-<ver>.prg` — standalone SysOp config editor
- `BOARDS-<ver>.d81` — blank disk for message bases (Device 9)
- `FILE_ID.DIZ` — classic BBS description file (45 chars/line, 10 lines)
- `README.txt` — plain-text project README

**Release notes:** If `data/release/notes.md` exists (with optional `__VERSION__` tokens), it is used as the GitHub release body. Otherwise, the script falls back to `git log` since the previous tag.

---

## Environment Variables

| Variable | Default | Used by |
|----------|---------|---------|
| `VICE_CMD` | `x64sc` | `deploy-vice.sh`, `build.sh` |
| `TCPSER_CMD` | `tcpser` | `deploy-vice.sh` |
| `T64_SD_PATH` | (none) | `deploy-u64.sh`, `fetch-u64.sh`, `extract-users.sh`, `extract-boards.sh` |
| `C1541` | `c1541` | `assemble-d81.sh`, `extract-boards.sh`, `migrate-d81.py` |
| `T64_SIEC_DEVICE` | `11` | `deploy.sh` |
| `T64_SIEC_BASE` | `/USB1/TURBO64` | `deploy.sh` |
| `T64_UIEC_DEVICE` | `10` | `deploy.sh` (informational — see script header) |

---

## Output Locations

```
build/c64/
├── TURBO64-<ver>.d81          assembled BBS disk image
├── BOOT-<ver>.prg        main BBS runtime (REL backend)
├── CONFIGURE-<ver>.prg   SysOp editor (REL backend)
├── ovl_*.prg             REL overlays (msgs, wfc, boot, doors, files, zmodem, auth)
└── siec/                 SoftIEC (T64_STORE_SEQ) build — own dir so its
    ├── BOOT-SIEC.prg          fixed, version-independent names (same-named
    ├── CONFIGURE-SIEC.prg     overlays never collide with the REL set above
    └── ovl_*.prg              — see `make c64-siec` / `make editor-siec`)

data/
├── users-seed.d81        user database snapshot (from extract-users.sh)
├── boards-seed.d81       boards disk snapshot (from extract-boards.sh)
├── usr_log               raw USR LOG binary
├── usr_prof              raw USR PROF binary
└── release/
    ├── file_id.diz.tmpl  FILE_ID.DIZ template (__VERSION__ substitution)
    ├── readme.txt.tmpl    README.txt template (__VERSION__ substitution)
    └── notes.md           release notes template (used by release.sh)

build/release/
└── TURBO64-<ver>.zip     release archive (created by release.sh)
```

---

## Script Index

| Script | Purpose |
|--------|---------|
| `build.sh` | Master launcher: vice / u64 / disk / build / clean |
| `deploy-vice.sh` | Launch VICE with modem bridge and RTC cartridge |
| `assemble-d81.sh` | Assemble bootable D8 disk image |
| `deploy-u64.sh` | Upload BBS (and optionally boards) disk to U64 |
| `deploy.sh` | Build + deploy + best-effort launch to d81 / siec / uiec |
| `migrate-d81.py` | Convert a seeded `.d81` into the SoftIEC folder tree |
| `fetch-u64.sh` | Download live BBS disk from U64 |
| `extract-users.sh` | Snapshot user database from U64 |
| `extract-boards.sh` | Snapshot boards disk from U64 |
| `install-c64u.sh` | Install c64u filesystem tool |
| `install-oscar64.sh` | Build oscar64 compiler from source |
| `lint.sh` | Run cppcheck static analysis |
| `release.sh` | Build artifacts, tag, and publish GitHub release |
