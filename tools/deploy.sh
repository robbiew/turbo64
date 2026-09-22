#!/usr/bin/env bash
# Build, deploy, and (best-effort) launch T/64 on one of three test targets.
#
# Usage: tools/deploy.sh <d81|siec|uiec> [options]
#
# Targets:
#   d81   Device 8, the emulated 1581. Assembles a SEEDED disk image (the
#         seed is required — an unseeded disk has no USR LOG and
#         boot_sequence() correctly refuses to start) and mounts it.
#   siec  Device 11, SoftIEC. Runs tools/migrate-d81.py to build the folder
#         tree, then uploads it to the device's Default Path over FTP.
#   uiec  Device 10, a physical uIEC/sd2iec. Not reachable over the network:
#         stages device 8 (same as the d81 target, plus COPYALL written onto
#         that same image) and drives COPYALL through the C64 to copy the
#         program set across. See NOTES below.
#
# Options (all targets):
#   --execute            Actually call c64u / upload. Default: DRY RUN —
#                         every c64u invocation is printed, not run. This is
#                         deliberate: review the printed commands before
#                         pointing this at real hardware.
#   --launch             After deploying, attempt a best-effort launch (see
#                         per-target notes below). The exact manual fallback
#                         is always printed, whether or not this is passed.
#   --no-build           Skip the `make` step; deploy what's already built.
#   -h, --help           Show this message.
#
# d81 options:
#   --seed <d81>          Seed disk for the user DB (default: data/users-seed.d81)
#   --drive <a|b>         Internal drive slot to mount into (default: a)
#
# siec options:
#   --device <n>          SoftIEC bus id (default: 11; env T64_SIEC_DEVICE)
#   --base <path>         Default Path on the Ultimate (default: /USB1/TURBO64;
#                          env T64_SIEC_BASE)
#   --clean               Before uploading, remove stale files under --base
#                          that are not part of this deploy — old BOOT/ovl
#                          binaries, src-diag/ diagnostics, probe scratch
#                          (PERF.DAT*, SP1*, STRAND/), and any *.seq whose
#                          stripped name collides with a file this deploy
#                          writes (see NOTES below for why that collision is
#                          dangerous on SoftIEC). User/message/file-area data
#                          is never touched — see tools/siec_clean.py for the
#                          exact classification rules. A dry run (no
#                          --execute) prints the rules and the local deploy
#                          manifest only, making NO network calls, same as
#                          every other c64u invocation in this script. With
#                          --execute, it lists the live tree, prints exactly
#                          what would be removed vs. kept (recognized and
#                          unrecognized), and asks for interactive
#                          confirmation before deleting anything (skip with
#                          --yes). After uploading, it re-lists the tree and
#                          diffs it against the manifest, catching both
#                          leftovers and upload failures.
#   --yes                  Skip --clean's interactive delete confirmation.
#                           Ignored without --clean.
#   --keep-data            Do not upload a data file (user database, boards,
#                          message index/bodies, file areas, doors, counters)
#                          that already exists on the device. Binaries,
#                          overlays, gfiles and CONFIG are still refreshed.
#                          This is the "update a live install" mode: without
#                          it a redeploy resets users and boards to the seed.
#                          Needs --execute to look at the device; a dry run
#                          just says so. Which names count as data is
#                          siec_clean.py's protected set.
#
# uiec options:
#   --uiec-device <n>     Physical drive's device number (default: 10; env
#                          T64_UIEC_DEVICE) — informational only: this is
#                          src-diag/copyall.c's hardcoded destination, not
#                          something this script can change without touching
#                          C source.
#
# NOTES — read before using --launch:
#
#   `c64u runners run-prg` issues LOAD"<path>",8,1: it FORCES device 8 and
#   TRUNCATES the path to 16 characters. It works for the d81 target (device
#   8 is already correct there) but cannot launch the siec target at all —
#   $BA ends up 8 and the BBS reports OVL_BOOT LOAD FAILED. For siec the
#   launch is: machine reset, `drives softiec root <base>` (the SoftIEC
#   working directory persists across resets and the BBS leaves it inside
#   SYSTEM/, so a bare LOAD fails with ?FILE NOT FOUND), then LOAD and RUN
#   through `machine sendkey` with the text and the '\n' as separate calls.
#   Verified on hardware; if it does not take, type the two printed lines
#   by hand.
#
#   uiec automation (--launch): COPYALL.prg is itself loaded via run-prg
#   (device 8, so run-prg's forced device is correct), then a "C" keystroke
#   starts it (COPYALL waits on getch(), not a BASIC line, so the sendkey
#   chunking bug above does not apply). VERIFIED on hardware: the first
#   sendkey "C" after run-prg is reliably dropped — COPYALL was still
#   sitting at its "PRESS C TO START" prompt until a second, identical
#   sendkey landed. But COPYALL's RESULT screen ("DONE. N FAILED.") also
#   ends on a getch(), so blindly sending "C" twice risks the opposite
#   failure: if the first keystroke actually landed, the second sits in the
#   buffer and silently dismisses the results before anyone reads them.
#   uiec_start_copyall sends "C" once, then polls the keyboard-buffer depth
#   at $00C6 via `machine read-mem` (0 = consumed, i.e. COPYALL read it and
#   moved on) and only resends if it never drains within a short timeout —
#   see that function for the full reasoning. This has NOT been re-verified
#   against hardware since the polling logic was added.
#   COPYALL itself is a diagnostic under src-diag/; its file list derives
#   BOOT/CONFIGURE names from BBS_RELEASE_VERSION_COMPACT and includes
#   OVL_AUTH, so it cannot drift the way earlier builds did. This script
#   still verifies the built PRG's embedded strings before staging (see
#   deploy_uiec) rather than trusting that source claim blind. deploy_uiec
#   also writes COPYALL.prg onto the staged device-8 image itself (via
#   assemble-d81.sh --extra-prg) so the printed manual LOAD"COPYALL",8 is
#   actually true — it used to name a file that was never on the disk.
#
#   Mount assertions: a C64 reset (which `runners run-prg` performs as part
#   of launching a PRG) can silently drop a mounted disk image from an
#   internal drive. Driving COPYALL against an empty device 8 either copies
#   nothing or deadlocks the IEC bus with interrupts disabled — a full
#   machine hang requiring a physical reset — and has been observed to
#   truncate the destination files instead of failing loudly. assert_drive_
#   mounted (see below) checks `c64u drives list`'s human-readable "Mounted
#   Image:" line (NOT --json: that call doesn't crash, but its success-path
#   schema was never observed against live hardware in the pass that added
#   this, whereas the human-readable line format is directly observed
#   ground truth) at three points: right after deploy_d81 mounts, right
#   before deploy_uiec drives COPYALL, and again right after run-prg's
#   reset but before the start keystroke. All three abort the whole deploy
#   on failure. A fourth, non-fatal check runs after the copy completes —
#   it can't inspect the destination (uiec has no network path) but a
#   dropped device-8 mount there is strong evidence of a mid-run unmount
#   that would explain a partial copy. UNVERIFIED ON HARDWARE.
#
# Environment:
#   T64_SIEC_DEVICE   default SoftIEC bus id (11)
#   T64_SIEC_BASE     default SoftIEC Default Path (/USB1/TURBO64)
#   T64_UIEC_DEVICE   default physical drive device number (10, informational)
#   C1541             path to the VICE c1541 tool (used by migrate-d81.py)
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/vendor/c64u/bin/c64u"
VERSION_COMPACT="$(grep 'BBS_RELEASE_VERSION_COMPACT' "$ROOT/include/bbs/version.h" | cut -d'"' -f2)"

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

show_help() {
    sed -n '2,131p' "$0"
}

TARGET="${1:-}"
shift || true

EXECUTE=0
LAUNCH=0
BUILD=1
CLEAN=0
CLEAN_YES=0
KEEP_DATA=0
VERIFY_FAILED=0
D81_SEED="$ROOT/data/users-seed.d81"
D81_DRIVE="a"
SIEC_DEVICE="${T64_SIEC_DEVICE:-11}"
SIEC_BASE="${T64_SIEC_BASE:-/USB1/TURBO64}"
UIEC_DEVICE="${T64_UIEC_DEVICE:-10}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --execute)      EXECUTE=1; shift ;;
        --launch)       LAUNCH=1; shift ;;
        --no-build)     BUILD=0; shift ;;
        --clean)        CLEAN=1; shift ;;
        --yes)          CLEAN_YES=1; shift ;;
        --keep-data)    KEEP_DATA=1; shift ;;
        --seed)         D81_SEED="$2"; shift 2 ;;
        --drive)        D81_DRIVE="$2"; shift 2 ;;
        --device)       SIEC_DEVICE="$2"; shift 2 ;;
        --base)         SIEC_BASE="$2"; shift 2 ;;
        --uiec-device)  UIEC_DEVICE="$2"; shift 2 ;;
        -h|--help)      show_help; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

case "$TARGET" in
    d81|siec|uiec) ;;
    -h|--help|"") show_help; exit 0 ;;
    *) echo "Unknown target: $TARGET (expected d81, siec, or uiec)" >&2; exit 1 ;;
esac

if [ "$KEEP_DATA" -eq 1 ] && [ "$TARGET" != "siec" ]; then
    echo "ERROR: --keep-data is only supported for the siec target." >&2
    exit 1
fi

if [ "$CLEAN" -eq 1 ] && [ "$TARGET" != "siec" ]; then
    echo "ERROR: --clean is only supported for the siec target (d81 replaces the" >&2
    echo "whole disk image, so staleness cannot accumulate; uiec has no network" >&2
    echo "filesystem access — see the deploy_uiec notes)." >&2
    exit 1
fi

if [ ! -x "$BIN" ]; then
    echo "ERROR: c64u not found at $BIN" >&2
    echo "Run 'tools/install-c64u.sh' first." >&2
    exit 1
fi

# Every c64u call goes through here. Dry-run (the default) only prints what
# WOULD run — nothing touches hardware until --execute is passed.
run_c64u() {
    if [ "$EXECUTE" -eq 1 ]; then
        "$BIN" "$@"
    else
        echo "[dry-run] c64u $*"
    fi
}

# Internal helper for assert_drive_mounted — prints the failure and, in
# fatal mode, aborts the whole deploy. Split out so the same messaging
# serves both the pre-copy (fatal) and post-copy (warn-only) call sites.
_drive_mount_fail() {
    local mode="$1" detail="$2" upper="$3" context="$4"
    echo -e "${RED}${detail}${NC}" >&2
    echo -e "${RED}A C64 reset drops the mounted image — run-prg and other resets can" \
             "silently unmount it, and that is almost certainly what happened here.${NC}" >&2
    if [ "$mode" = "fatal" ]; then
        echo -e "${RED}Aborting before ${context}: driving COPYALL against an empty" \
                 "device 8 is how the machine hangs. Re-run the staging step" \
                 "(deploy_d81 / drives mount-upload) and try again.${NC}" >&2
        exit 1
    else
        echo -e "${YELLOW}Continuing, but treat the copy that just ran as suspect:" \
                 "re-stage and re-run, then verify block counts on the destination" \
                 "before trusting it.${NC}" >&2
    fi
}

# Confirms internal drive $1 (a|b) currently shows a mounted image, by
# parsing `c64u drives list`'s human-readable output. $2 is a short
# description of what we're about to do, used in messages. $3 is "fatal"
# (default — abort the whole deploy) or "warn" (print and continue; used
# only for the post-copy sanity check, where the copy already happened and
# aborting can't undo it).
#
# Why parse the human-readable form instead of --json: `c64u drives list
# --json` was checked and does NOT crash — it returns a well-formed JSON
# error envelope when the host is unreachable — so it isn't unsafe to call.
# But its success-path schema (field names for "which drive", "is media
# mounted") was never observed against live hardware in the pass that added
# this check, and this vendored tool has already been caught failing badly
# elsewhere (`fs ls --json` panics with a Go stack trace on any
# extensionless filename). Guessing at an unverified JSON key for a check
# whose entire job is to prevent a machine hang is the wrong trade. The
# human-readable "Mounted Image:" line, by contrast, is directly observed
# ground truth — it shows "─ (no disk)" when empty and "● /path" when
# mounted — so that's what's parsed here.
#
# Dry run: makes no network call and always passes, matching run_c64u's
# contract that nothing touches hardware without --execute.
assert_drive_mounted() {
    local drive="$1" context="$2" mode="${3:-fatal}"
    local upper lower
    upper="$(printf '%s' "$drive" | tr '[:lower:]' '[:upper:]')"
    lower="$(printf '%s' "$drive" | tr '[:upper:]' '[:lower:]')"

    if [ "$EXECUTE" -eq 0 ]; then
        echo -e "${YELLOW}[dry-run] would verify: drive ${upper} shows a mounted image" \
                 "(c64u drives list) before ${context}.${NC}"
        return
    fi

    echo -e "${BLUE}Verifying drive ${upper} has a mounted image (${context})...${NC}"
    local out
    if ! out="$("$BIN" drives list 2>&1)"; then
        _drive_mount_fail "$mode" "'c64u drives list' failed:
${out}" "$upper" "$context"
        return
    fi

    # `c64u drives list` groups each drive under an UNINDENTED header line and
    # indents that drive's fields beneath it:
    #
    #     ● a (Enabled)
    #       Bus ID:            #8
    #       Mounted Image:     ● /Temp/temp0000
    #
    #     ● b (Enabled)
    #       Mounted Image:     ─ (no disk)
    #
    # So the block boundary is "line does not start with whitespace", and the
    # drive is identified by " <letter> (" within that header — the leading
    # glyph is a multi-byte bullet that varies with enabled/disabled state, so
    # do not anchor on it. Scoping to one drive's block is load-bearing: a
    # naive grep for "Mounted Image" across the whole output also sees drive b,
    # the IEC drive and the printer, all of which normally report "(no disk)",
    # and would report every deploy as unmounted.
    local block
    block="$(printf '%s\n' "$out" | awk -v want=" ${lower} (" '
        /^[^[:space:]]/ { active = (index($0, want) > 0) }
        active { print }
    ')"

    if [ -z "$block" ] || ! printf '%s\n' "$block" | grep -q "Mounted Image"; then
        _drive_mount_fail "$mode" "could not find a 'Mounted Image' line for drive ${upper} in:
${out}" "$upper" "$context"
        return
    fi

    if printf '%s\n' "$block" | grep -q "Mounted Image.*(no disk)"; then
        _drive_mount_fail "$mode" "drive ${upper} has NO disk mounted" "$upper" "$context"
        return
    fi

    echo -e "${GREEN}Drive ${upper}: image mounted — OK.${NC}"
}

# Reads the C64 keyboard-buffer depth at $00C6 ("number of keys queued and
# not yet consumed by the kernal") via a real DMA memory read, for
# uiec_start_copyall's consumption check below. Echoes the byte value
# (0-255) on success, or the literal string "unknown" if the read failed or
# produced something unparsable. Only ever called under EXECUTE=1.
#
# Uses --json. Plain `machine read-mem` writes a FORMATTED HEX DUMP to
# stdout, never raw bytes, so redirecting it to a file and expecting one
# byte back always fails — verified on hardware, where this reported
# "unknown" every time and fell through to the blind extra keystroke.
# --json returns {"address":"$00c6","data":"01","length":1}; the value
# arrives as hex TEXT, which also sidesteps the NUL-byte problem that makes
# a raw-byte capture unsafe for depth 0 — the single value this most needs
# to report correctly.
uiec_kbd_buffer_depth() {
    local out hex
    if ! out="$("$BIN" machine read-mem 00c6 --length 1 --json 2>/dev/null)"; then
        echo "unknown"
        return
    fi
    hex="$(printf '%s' "$out" \
        | sed -n 's/.*"data"[[:space:]]*:[[:space:]]*"\([0-9a-fA-F][0-9a-fA-F]*\)".*/\1/p')"
    if [ -z "$hex" ]; then
        echo "unknown"
        return
    fi
    printf '%d\n' "0x${hex}"
}

# Sends the "C" keystroke that starts COPYALL after run-prg has loaded it,
# handling the reliably-dropped-first-keystroke problem without risking a
# surplus keystroke that would dismiss COPYALL's result screen.
#
# Background: COPYALL blocks on getch() at "PRESS C TO START". Verified on
# hardware in an earlier pass: the first `machine sendkey "C"` after
# `runners run-prg` is reliably dropped — COPYALL sits at the prompt until
# an identical second keystroke lands. The script used to paper over this
# by always sending "C" twice, unconditionally. That is unsafe here
# specifically because COPYALL's RESULT screen ("DONE. N FAILED.") also
# ends on a getch(): if the first "C" actually landed this time, the blind
# second send sits in the keyboard buffer and silently dismisses the
# results before anyone can read them — trading a dropped start for a lost
# result, which is strictly worse for a script that already can't read the
# result back over the API.
#
# So: send once, then poll the keyboard-buffer depth at $00C6 instead of
# guessing. 0 means the byte was consumed (COPYALL read it and is now
# copying); nonzero means it's still sitting there unread. Only resend if
# polling shows it was never consumed inside a short timeout, and never
# send more than two keystrokes total, matching the one-extra-key behavior
# already verified reliable on hardware. If the depth read ever comes back
# "unknown" (see uiec_kbd_buffer_depth), this falls back immediately to
# that same one extra send rather than looping on a signal it can't trust.
#
# UNVERIFIED ON HARDWARE: the polling logic itself (as opposed to the
# plain double-send it replaces) has not been run against a real C64U.
uiec_start_copyall() {
    if [ "$EXECUTE" -eq 0 ]; then
        echo "[dry-run] c64u machine sendkey C"
        echo "[dry-run] would poll \$00C6 (keyboard buffer depth) for consumption," \
             "resending \"C\" at most once more, only if it never drains to 0."
        return
    fi

    echo -e "${BLUE}Sending start keystroke (C)...${NC}"
    "$BIN" machine sendkey "C"

    local tries=0 depth="unknown" consumed=0
    while [ "$tries" -lt 6 ]; do
        sleep 0.3
        depth="$(uiec_kbd_buffer_depth)"
        if [ "$depth" = "0" ]; then
            consumed=1
            break
        fi
        if [ "$depth" = "unknown" ]; then
            break
        fi
        tries=$((tries + 1))
    done

    if [ "$consumed" -eq 1 ]; then
        echo -e "${GREEN}Keystroke consumed (\$00C6 == 0) — COPYALL is running.${NC}"
        return
    fi

    if [ "$depth" = "unknown" ]; then
        echo -e "${YELLOW}Could not read \$00C6 — falling back to the one extra keystroke" \
                 "verified reliable on hardware.${NC}"
    else
        echo -e "${YELLOW}Keystroke not yet consumed after ${tries} checks — resending" \
                 "once (known-dropped-first-key case).${NC}"
    fi

    "$BIN" machine sendkey "C"
    sleep 0.3
    depth="$(uiec_kbd_buffer_depth)"
    if [ "$depth" = "0" ]; then
        echo -e "${GREEN}Keystroke consumed after resend.${NC}"
    else
        echo -e "${YELLOW}Still unconfirmed (\$00C6=${depth}) — watch the console; not" \
                 "sending a third keystroke (risk of dismissing the results screen).${NC}"
    fi
}

echo -e "${BLUE}T/64 deploy: target=${TARGET} version=${VERSION_COMPACT} execute=${EXECUTE} launch=${LAUNCH}${NC}"
if [ "$EXECUTE" -eq 0 ]; then
    echo -e "${YELLOW}DRY RUN — no c64u command below will actually execute. Pass --execute to run for real.${NC}"
fi
echo ""

deploy_d81() {
    # Optional: absolute path to one extra host PRG to stage onto the image
    # (forwarded to assemble-d81.sh --extra-prg). Only deploy_uiec passes
    # this (COPYALL); called with no argument here it stays empty, so the
    # plain d81 target and every release build remain diagnostic-free.
    local extra_prg="${1:-}"

    if [ "$BUILD" -eq 1 ]; then
        echo -e "${BLUE}Building REL binaries...${NC}"
        # door-example is NOT optional here, despite assemble-d81.sh guarding
        # its write with a file-existence test. tools/build.sh has always built
        # it; this path did not, so every disk it produced silently lacked
        # `fortune` — leaving the DOORS feature with nothing to demonstrate and
        # making COPYALL report FORTUNE as a failure on every single run.
        (cd "$ROOT" && make c64 && make editor && make door-example)
    fi

    if [ ! -f "$D81_SEED" ]; then
        echo "ERROR: seed disk not found: $D81_SEED" >&2
        echo "The seed is required — a plain 'make disk' produces a disk with no" >&2
        echo "user database, and boot_sequence() correctly refuses to start." >&2
        exit 1
    fi

    echo -e "${BLUE}Assembling seeded disk image...${NC}"
    if [ -n "$extra_prg" ]; then
        bash "$ROOT/tools/assemble-d81.sh" --seed-users "$D81_SEED" --extra-prg "$extra_prg"
    else
        bash "$ROOT/tools/assemble-d81.sh" --seed-users "$D81_SEED"
    fi

    local image="$ROOT/build/c64/TURBO64-${VERSION_COMPACT}.d81"
    echo -e "${BLUE}Mounting to internal drive ${D81_DRIVE} (device 8)...${NC}"
    run_c64u drives mount-upload "$D81_DRIVE" "$image" --type d81 --mode readwrite

    # Confirm the mount actually took, rather than trusting the upload
    # call's success message — see assert_drive_mounted for why this
    # matters.
    assert_drive_mounted "$D81_DRIVE" "trusting this mount for the deploy"

    if [ "$LAUNCH" -eq 1 ]; then
        echo -e "${BLUE}Launching (run-prg forces device 8, which is correct here)...${NC}"
        local boot_prg="$ROOT/build/c64/BOOT-${VERSION_COMPACT}.prg"
        local remote="/USB1/T64RUN.PRG"
        run_c64u fs upload "$boot_prg" "$remote"
        run_c64u runners run-prg "$remote"
    fi

    echo ""
    echo -e "${GREEN}d81 deploy done.${NC} Manual boot on the C64, if not using --launch:"
    echo "    LOAD\"BOOT-${VERSION_COMPACT}\",8"
    echo "    RUN"
}

# --clean helpers. Bash 3.2 on macOS has no associative arrays, hence the
# case statement instead of a section->path map.
siec_section_path() {
    case "$1" in
        ROOT) echo "$SIEC_BASE" ;;
        *)    echo "$SIEC_BASE/$1" ;;
    esac
}

# Fetches `c64u fs ls <path> --json` for ROOT + the four sections into
# "$work"/<SECTION>.json and prints a matching --listing SECTION=path
# argument list on stdout (one per line, for the caller to collect). Always
# a real network call — only invoked from inside an EXECUTE=1 branch, never
# from a dry run. A directory that does not exist yet (e.g. first-ever
# deploy) is treated as empty rather than failing the whole pass.
#
# --json is used here because it is the stable, parseable form. Older
# vendor/c64u builds also panicked in plain `fs ls` on any entry with no
# extension (output.GetFileIcon sliced on LastIndex(name, ".") == -1) —
# c64u v1.0.0 lists CONFIG and USR LOG fine, but the JSON form is still
# what siec_clean.py parses, so do not "simplify" this to human-readable
# output. This is the only call site in tools/ that shells out to `fs ls`;
# siec_clean.py never talks to c64u itself, it only parses the JSON this
# function already captured.
siec_fetch_listings() {
    local work="$1"
    local section path out
    for section in ROOT SYSTEM MSGS FILES DOORS; do
        path="$(siec_section_path "$section")"
        out="$work/$section.json"
        if ! "$BIN" fs ls "$path" --json >"$out" 2>"$work/$section.err"; then
            echo "[]" >"$out"
            echo -e "${YELLOW}  (could not list ${path} — treating as empty; see ${work}/${section}.err)${NC}" >&2
        fi
        echo "--listing"
        echo "$section=$out"
    done
}

# Pre-upload: classify what is currently under $SIEC_BASE and remove what's
# safe to remove. Dry run makes NO network calls (same contract as every
# other c64u invocation here) — it can only show the rules and the local
# manifest, not real per-file decisions. --execute fetches the live tree,
# prints the classification, and requires typed confirmation before
# deleting anything unless --yes was passed.
siec_clean_pass() {
    local manifest="$1"
    local boot_name
    boot_name="$(grep -m1 '^BOOT-.*\.prg$' "$manifest" || true)"

    echo -e "${BLUE}--clean: evaluating ${SIEC_BASE} for stale files...${NC}"

    if [ "$EXECUTE" -eq 0 ]; then
        echo -e "${YELLOW}[dry-run] would run: c64u fs ls --json against ${SIEC_BASE} and its${NC}"
        echo -e "${YELLOW}SYSTEM/MSGS/FILES/DOORS subfolders. Nothing is fetched from the device${NC}"
        echo -e "${YELLOW}in a dry run. Rules applied once real listings are available (--execute):${NC}"
        echo "  REMOVE  BOOT-*.prg / BOOT*.PRG other than ${boot_name:-<the BOOT binary for this deploy>}"
        echo "  REMOVE  CONFIGURE-*.prg / CONFIGURE*.PRG other than CONFIGURE-SIEC.prg"
        echo "  REMOVE  ovl_*.prg outside this deploy's root/SYSTEM locations"
        echo "  REMOVE  known src-diag/ diagnostic PRGs: SIECPROBE SEQTEST SEQNAME USRREAD"
        echo "          USRSWEEP CFGREAD PTEST RELTEST CPTEST DIR EXISTS CLEAN WIPE COPYALL"
        echo "  REMOVE  probe scratch: PERF.DAT*, SP1*, and the STRAND/ fixture directory"
        echo "  REMOVE  any file that answers to the same CBM name as one this deploy writes"
        echo "          (case-insensitive, .seq marker ignored: a stale CALLERS beside callers.seq)"
        echo "  KEEP    USR LOG, USR PROF, ACCESS, CALLERS, syscnt*, USR.PTR*, USR.DAY*,"
        echo "          BOARDS*, B<n>.IDX*, B<n>.TXT*, UDS*, UD<n>*, VOTE1*, DOORS*, T64.SIEC"
        echo "  KEEP    every file this deploy is about to write ($(wc -l <"$manifest" | tr -d ' ') files)"
        echo "  KEEP    anything else, reported unrecognized for manual review"
        echo -e "${YELLOW}Pass --execute to fetch the live listing, see exact per-file decisions,${NC}"
        echo -e "${YELLOW}and (after typed confirmation, or --yes) delete.${NC}"
        echo ""
        return
    fi

    local work removals n confirm
    work="$(mktemp -d)"
    local listing_args=()
    while IFS= read -r line; do
        listing_args+=("$line")
    done < <(siec_fetch_listings "$work")

    removals="$work/removals.txt"
    if ! python3 "$ROOT/tools/siec_clean.py" classify \
            --manifest "$manifest" --base "$SIEC_BASE" \
            "${listing_args[@]}" --emit-removals "$removals"; then
        echo -e "${RED}ERROR: --clean classification failed (see above) — aborting" \
                 "without deleting anything.${NC}" >&2
        exit 1
    fi
    echo ""

    if [ ! -s "$removals" ]; then
        echo -e "${GREEN}--clean: nothing to remove.${NC}"
        echo ""
        return
    fi

    n=$(wc -l <"$removals" | tr -d ' ')
    if [ "$CLEAN_YES" -ne 1 ]; then
        echo -e "${YELLOW}About to delete ${n} file(s) listed above from ${SIEC_BASE}.${NC}"
        confirm=""
        read -r -p "Type YES to confirm deletion: " confirm || true
        if [ "$confirm" != "YES" ]; then
            echo "Aborted — nothing deleted."
            exit 1
        fi
    fi

    while IFS= read -r path; do
        run_c64u fs rm "$path"
    done <"$removals"
    echo ""
}

# Post-upload: re-list the tree and diff it against the manifest, catching
# both leftovers (a REMOVE-classified entry still present) and upload
# failures (a manifest entry that never arrived). Only runs under
# --execute — a dry run uploaded nothing, so there is nothing to verify.
siec_verify_pass() {
    local manifest="$1"

    if [ "$EXECUTE" -eq 0 ]; then
        echo -e "${YELLOW}[dry-run] --clean: post-upload verification skipped — nothing" \
                 "was uploaded in a dry run (requires --execute).${NC}"
        return
    fi

    echo -e "${BLUE}--clean: verifying uploaded tree against the manifest...${NC}"
    local work
    work="$(mktemp -d)"
    local listing_args=()
    while IFS= read -r line; do
        listing_args+=("$line")
    done < <(siec_fetch_listings "$work")

    if python3 "$ROOT/tools/siec_clean.py" verify \
            --manifest "$manifest" --base "$SIEC_BASE" "${listing_args[@]}"; then
        echo -e "${GREEN}--clean: post-upload verification OK — remote tree matches" \
                 "the manifest.${NC}"
    else
        echo -e "${RED}--clean: post-upload verification found problems (see above).${NC}" >&2
        VERIFY_FAILED=1
    fi
    echo ""
}

mkdir_quiet() {
    if [ "$EXECUTE" -eq 1 ]; then
        "$BIN" fs mkdir "$1" >/dev/null 2>&1 || true
    else
        echo "[dry-run] c64u fs mkdir $1"
    fi
}

# --keep-data: which manifest entries to skip. Lists the live tree and asks
# siec_clean.py which protected data files are already there (matched on
# the CBM name, so the C64's "BOARDS.seq" counts for the manifest's
# "boards.seq"). Prints one manifest-relative path per line. A dry run
# cannot look at the device, so it prints nothing and says why.
siec_keep_data_list() {
    local manifest="$1"
    if [ "$EXECUTE" -eq 0 ]; then
        echo -e "${YELLOW}[dry-run] --keep-data: would list ${SIEC_BASE} and skip uploading" \
                 "any protected data file already present (needs --execute).${NC}" >&2
        return
    fi
    local work
    work="$(mktemp -d)"
    local listing_args=()
    while IFS= read -r line; do
        listing_args+=("$line")
    done < <(siec_fetch_listings "$work")
    python3 "$ROOT/tools/siec_clean.py" existing-data \
        --manifest "$manifest" --base "$SIEC_BASE" "${listing_args[@]}"
}

deploy_siec() {
    if [ "$BUILD" -eq 1 ]; then
        echo -e "${BLUE}Building SIEC binaries...${NC}"
        # door-example is not siec-flavored (the door PRG is shared with the
        # REL build — see Makefile's `door` target, always $(OUTDIR)) but
        # migrate-d81.py needs it in build/c64/ to populate DOORS/. Without
        # it here the tree ships with an empty DOORS/ and COPYALL reports
        # FORTUNE as a failure on every run — the same trap deploy_d81 was
        # already fixed for above.
        # The REL binaries are built too: assemble-d81.sh below refuses to
        # run without BOOT-<ver>.prg / CONFIGURE-<ver>.prg, and that image is
        # the migration's data source even though nothing mounts it. On a
        # clean checkout this target used to die with "BOOT PRG not found";
        # it only ever worked when build/c64/ still held REL output from an
        # earlier build.
        (cd "$ROOT" && make c64 && make editor \
                    && make c64-siec && make editor-siec && make door-example)
    fi

    if [ ! -f "$D81_SEED" ]; then
        echo "ERROR: seed disk not found: $D81_SEED" >&2
        echo "migrate-d81.py needs a seeded .d81 as its data source." >&2
        exit 1
    fi

    echo -e "${BLUE}Assembling seeded disk image (data source for migration)...${NC}"
    bash "$ROOT/tools/assemble-d81.sh" --seed-users "$D81_SEED"
    local image="$ROOT/build/c64/TURBO64-${VERSION_COMPACT}.d81"

    local tree="$ROOT/build/c64/siec-tree"
    echo -e "${BLUE}Building SoftIEC folder tree (tools/migrate-d81.py)...${NC}"
    rm -rf "$tree"
    python3 "$ROOT/tools/migrate-d81.py" "$image" "$tree" \
        --base "$SIEC_BASE" --device "$SIEC_DEVICE"

    local manifest="$ROOT/build/c64/siec-tree.manifest"
    (cd "$tree" && find . -type f | sed 's#^\./##') | LC_ALL=C sort >"$manifest"

    echo -e "${BLUE}Uploading tree to ${SIEC_BASE} (device ${SIEC_DEVICE})...${NC}"
    # `fs mkdir` on a folder that already exists prints "Failed to create
    # directory" — every redeploy hit five of those. Silence it; a folder
    # that genuinely could not be made fails loudly at the first upload.
    mkdir_quiet "$SIEC_BASE"
    for section in SYSTEM MSGS FILES DOORS; do
        mkdir_quiet "$SIEC_BASE/$section"
    done

    if [ "$CLEAN" -eq 1 ]; then
        siec_clean_pass "$manifest"
    fi

    local skip=""
    if [ "$KEEP_DATA" -eq 1 ]; then
        skip="$(siec_keep_data_list "$manifest")"
    fi

    while IFS= read -r -d '' f; do
        local rel="${f#"$tree"/}"
        if [ -n "$skip" ] && printf '%s\n' "$skip" | grep -qxF "$rel"; then
            echo "  keep-data: not uploading $rel (exists on device)"
            continue
        fi
        run_c64u fs upload "$f" "$SIEC_BASE/$rel"
    done < <(find "$tree" -type f -print0)

    if [ "$CLEAN" -eq 1 ]; then
        siec_verify_pass "$manifest"
    fi

    # BOOT-SIEC is a fixed, version-independent name (see Makefile) — unlike
    # the REL BOOT binary above, it does not carry $VERSION_COMPACT.
    local boot_name="BOOT-SIEC"
    if [ "$LAUNCH" -eq 1 ]; then
        # Verified on hardware (firmware 1.1.0, c64u v1.0.0), and the only
        # sequence that has worked every time:
        #  1. reset — the C64 must be at the BASIC prompt for step 2, and
        #     whatever was running (usually the BBS itself) has to go anyway.
        #  2. `drives softiec root <base>` — the SoftIEC working directory
        #     PERSISTS across a reset, and the BBS leaves it in SYSTEM/ (or
        #     wherever the last overlay load was). LOAD"BOOT-SIEC" from there
        #     is a plain ?FILE NOT FOUND. This call puts the cursor back at
        #     the tree root; it refuses (harmlessly) if the C64 is not at
        #     BASIC, which is why the reset comes first.
        #  3. LOAD / RUN through the keyboard buffer, text and RETURN as
        #     separate calls. The load takes ~10 s over SoftIEC, so RUN is
        #     sent after a generous pause rather than immediately.
        echo -e "${BLUE}Launching: reset, SoftIEC root -> ${SIEC_BASE}, then LOAD/RUN via sendkey...${NC}"
        run_c64u machine reset
        [ "$EXECUTE" -eq 1 ] && sleep 4
        run_c64u drives softiec root "$SIEC_BASE"
        run_c64u machine sendkey "LOAD\"${boot_name}\",${SIEC_DEVICE}"
        run_c64u machine sendkey '\n'
        [ "$EXECUTE" -eq 1 ] && sleep 14
        run_c64u machine sendkey 'RUN'
        run_c64u machine sendkey '\n'
    fi

    echo ""
    echo -e "${GREEN}siec deploy done.${NC} Manual boot on the C64 (reliable path):"
    echo "    LOAD\"${boot_name}\",${SIEC_DEVICE}"
    echo "    RUN"
    echo "(if sendkey drops the RETURN, type these by hand — send the text and"
    echo " '\\n' as separate keystrokes if scripting it again)"
    echo ""
    echo "CONFIGURE-SIEC is at the tree root alongside BOOT-SIEC (migrate-d81.py"
    echo "copies it automatically). To run it instead:"
    echo "    LOAD\"CONFIGURE-SIEC\",${SIEC_DEVICE}"
    echo "    RUN"
    echo ""
    echo -e "${YELLOW}Reminder: the SoftIEC current directory persists across a reset.${NC}"
    echo "This script only uploads over FTP (does not touch IEC bus DOS state), so"
    echo "it never leaves the drive cursor positioned anywhere — only a program run"
    echo "ON the C64 (the BBS itself, COPYALL, diagnostics) can do that. If you run"
    echo "anything else on the C64 against device ${SIEC_DEVICE} afterwards, follow"
    echo "the commit e57b968 precedent: CD back to ${SIEC_BASE} before it exits."
}

deploy_uiec() {
    echo -e "${YELLOW}uiec (device ${UIEC_DEVICE}) has no network path — its media is not${NC}"
    echo -e "${YELLOW}reachable from this script. COPYALL is written onto the same staged${NC}"
    echo -e "${YELLOW}device-8 image (assemble-d81.sh --extra-prg), then driven through the${NC}"
    echo -e "${YELLOW}C64 to copy the program set across (src-diag/copyall.c).${NC}"
    echo ""

    if [ "$BUILD" -eq 1 ]; then
        echo -e "${BLUE}Building diagnostics (COPYALL)...${NC}"
        (cd "$ROOT" && make diag)
    fi

    # Verify the BUILT PRG, not the source text: copyall.c derives its
    # BOOT/CONFIGURE names from BBS_RELEASE_VERSION_COMPACT via C preprocessor
    # string concatenation, so grepping the source for a literal version
    # string would no longer find one. Checking the compiled binary's
    # embedded strings instead confirms what COPYALL will actually try to
    # open, not just what the source claims to do.
    local copyall_prg="$ROOT/build/c64/COPYALL.prg"
    local missing=""
    for want in "BOOT-${VERSION_COMPACT}" "CONFIGURE-${VERSION_COMPACT}" "OVL_AUTH"; do
        if ! strings "$copyall_prg" | grep -qx "$want"; then
            missing="${missing}${missing:+, }${want}"
        fi
    done
    if [ -n "$missing" ]; then
        echo -e "${RED}WARNING: build/c64/COPYALL.prg does not contain expected string(s):" \
                 "${missing}.${NC}"
        echo -e "${RED}COPYALL would silently skip these files. Check src-diag/copyall.c's" \
                 "file list against include/bbs/version.h before relying on it.${NC}"
    else
        echo -e "${GREEN}Verified: COPYALL.prg's embedded file list includes" \
                 "BOOT-${VERSION_COMPACT}, CONFIGURE-${VERSION_COMPACT}, and OVL_AUTH.${NC}"
    fi
    echo ""

    # COPYALL is staged onto THIS deploy's own device-8 image via
    # assemble-d81.sh --extra-prg (see deploy_d81) so the manual LOAD"COPYALL"
    # instruction below is actually true. Never passed for the plain d81
    # target or `make disk` — release images stay diagnostic-free.
    deploy_d81 "$copyall_prg"

    if [ "$LAUNCH" -eq 1 ]; then
        echo -e "${BLUE}Attempting best-effort launch of COPYALL via run-prg + sendkey...${NC}"

        # Critical check: staging (deploy_d81, above) may have happened
        # minutes ago by the time we actually get here. Don't trust that
        # earlier check — verify again, right before driving COPYALL.
        assert_drive_mounted "$D81_DRIVE" "driving COPYALL"

        local remote="/USB1/T64COPYALL.PRG"
        run_c64u fs upload "$copyall_prg" "$remote"
        run_c64u runners run-prg "$remote"
        sleep 1

        # run-prg just reset the machine to launch COPYALL — this is
        # precisely where the mount can vanish (a mounted image does not
        # survive a reset), so check again before sending the start
        # keystroke. This is the check that matters most: staging succeeds,
        # the reset drops the mount, and everything downstream goes quiet.
        assert_drive_mounted "$D81_DRIVE" "sending the COPYALL start keystroke"

        uiec_start_copyall

        # Non-fatal: the copy already ran, so aborting now can't undo
        # anything. But if device 8 also lost its mount somewhere along the
        # way, that's strong evidence of a mid-run unmount that would
        # explain a partial/truncated copy — worth flagging even though
        # this script has no network path to device 10 to check the
        # destination directly.
        assert_drive_mounted "$D81_DRIVE" "the copy that just ran" "warn"

        echo "Watch the console for 'DONE. N FAILED.' — this script cannot read it back."
    fi

    echo ""
    echo -e "${GREEN}uiec staging done.${NC} COPYALL is on device 8's image. On the C64"
    echo "(device 8 is now the seeded d81):"
    echo "    LOAD\"COPYALL\",8"
    echo "    RUN"
    echo "    (press C to start; it copies device 8 -> device ${UIEC_DEVICE} partition 1)"
}

case "$TARGET" in
    d81)  deploy_d81 ;;
    siec) deploy_siec ;;
    uiec) deploy_uiec ;;
esac

if [ "${VERIFY_FAILED:-0}" -eq 1 ]; then
    echo -e "${RED}Deploy finished but --clean's post-upload verification found" \
             "problems (see above). Exiting non-zero.${NC}" >&2
    exit 1
fi
