#!/usr/bin/env bash
# Assembles a .d81 disk image containing BOOT and CONFIGURE PRGs plus data files
# Usage: tools/assemble-d81.sh [options] [version]
#
# Options:
#   --seed-users <d81>  Use an existing .d81 as the base image to preserve the
#                       USR LOG REL file. New PRG files replace the old ones.
#   --fetch-users       Fetch the live .d81 from U64 (via fetch-u64.sh) and
#                       use it as the base image (implies --seed-users).
#   --extra-prg <path>  Write one additional host PRG onto the image, named
#                       from its own basename (lowercased, extension
#                       stripped) via the same cbm_name() rule as every
#                       other PRG here. Opt-in only — no caller passes this
#                       by default, so `make disk` / a bare invocation never
#                       ships it. Intended for deploy.sh's uiec target to
#                       stage src-diag/copyall.c's COPYALL.prg; do not use
#                       it to put other src-diag/ diagnostics on a release
#                       image.
#
# If neither --seed-users nor --fetch-users is provided, a fresh empty disk
# is created (no user files).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
C1541="${C1541:-c1541}"

SEED_D81=""
FETCH_USERS=0
EXTRA_PRG=""

# Parse options
while [[ $# -gt 0 ]]; do
    case "$1" in
        --seed-users)
            SEED_D81="$2"
            shift 2
            ;;
        --fetch-users)
            FETCH_USERS=1
            shift
            ;;
        --extra-prg)
            EXTRA_PRG="$2"
            shift 2
            ;;
        -h|--help)
            sed -n '2,20p' "$0"
            exit 0
            ;;
        *)
            break
            ;;
    esac
done

# Get version
VERSION_COMPACT="${1:-$(grep 'BBS_RELEASE_VERSION_COMPACT' "$ROOT/include/bbs/version.h" | cut -d'"' -f2)}"
echo "Building disk image for T/64 v${VERSION_COMPACT}"

# PRG files
BOOT_PRG="$ROOT/build/c64/BOOT-${VERSION_COMPACT}.prg"
CONFIGURE_PRG="$ROOT/build/c64/CONFIGURE-${VERSION_COMPACT}.prg"

if [ ! -f "$BOOT_PRG" ]; then
    echo "ERROR: BOOT PRG not found: $BOOT_PRG" >&2
    echo "Run 'make c64' first." >&2
    exit 1
fi

if [ ! -f "$CONFIGURE_PRG" ]; then
    echo "ERROR: CONFIGURE PRG not found: $CONFIGURE_PRG" >&2
    echo "Run 'make editor' first." >&2
    exit 1
fi

if [ -n "$EXTRA_PRG" ] && [ ! -f "$EXTRA_PRG" ]; then
    echo "ERROR: --extra-prg file not found: $EXTRA_PRG" >&2
    exit 1
fi

# Output disk
OUTPUT_DISK="$ROOT/build/c64/TURBO64-${VERSION_COMPACT}.d81"

# Data files
DATA_DIR="$ROOT/build/c64"

# A standard .d81 image is 819200 bytes (822400 with error info). Reject
# anything smaller so an empty/truncated fetch can't clobber a good disk.
file_size() { stat -f%z "$1" 2>/dev/null || stat -c%s "$1" 2>/dev/null || echo 0; }
is_valid_d81() { [ -f "$1" ] && [ "$(file_size "$1")" -ge 819200 ]; }

# If --fetch-users, download the live .d81 from U64
if [ "$FETCH_USERS" -eq 1 ]; then
    FETCH_OUT="$ROOT/build/c64/TURBO64-${VERSION_COMPACT}-live.d81"
    echo "Fetching live disk from U64..."
    bash "$ROOT/tools/fetch-u64.sh" -o "$FETCH_OUT" || true
    # Validate BEFORE adopting it as the seed: a failed/empty fetch must not
    # propagate into the output disk (it previously clobbered a good image).
    if ! is_valid_d81 "$FETCH_OUT"; then
        echo "ERROR: U64 fetch produced an empty/invalid disk ($FETCH_OUT, $(file_size "$FETCH_OUT") bytes)." >&2
        echo "       The U64 is probably unreachable or the BBS disk is busy." >&2
        echo "       Aborting WITHOUT touching $OUTPUT_DISK. Use 'make disk' or" >&2
        echo "       --seed-users <good.d81> (e.g. data/users-seed.d81) instead." >&2
        exit 1
    fi
    SEED_D81="$FETCH_OUT"
fi

# c1541 converts host filenames to PETSCII: lowercase host chars (a-z) become
# uppercase PETSCII (readable on C64); uppercase host chars (A-Z) become shifted
# PETSCII (graphics). Always pass a fully-lowercase CBM name for mixed-case hosts.
# C64 disk filenames have no extension — the file type is stored separately.
cbm_name() { basename "$1" | tr '[:upper:]' '[:lower:]' | sed 's/\.[^.]*$//'; }

if [ -n "$SEED_D81" ]; then
    # Use seed disk as base — USR LOG REL file is preserved exactly.  Validate
    # it is a real .d81 BEFORE copying over the output, so a missing/empty/
    # truncated seed can never overwrite an existing good disk.
    if ! is_valid_d81 "$SEED_D81"; then
        echo "ERROR: seed disk missing or invalid: $SEED_D81 ($(file_size "$SEED_D81") bytes; need >= 819200)." >&2
        echo "       Aborting WITHOUT touching $OUTPUT_DISK." >&2
        exit 1
    fi
    echo "Seeding from: $SEED_D81"
    cp "$SEED_D81" "$OUTPUT_DISK"

    # Patch USER_F_CLEAR_ON_MSG (bit 0 of byte 29 in every 30-byte record)
    # into the USR LOG REL file inside the output disk image.  The flag was
    # added after records may have been written so seeded images need it set.
    # Also patches data/usr_log (the flat backup) if present.
    python3 << PYPATCH
import sys, os

DISK        = "$OUTPUT_DISK"
FLAT_BACKUP = "$ROOT/data/usr_log"
RECORD_SIZE = 30
FLAG_BYTE   = 29    # offset of user_record_t.flags within packed record
FLAG_MASK   = 0x01  # USER_F_CLEAR_ON_MSG

def sector_offset(track, sec):
    return (track - 1) * 40 * 256 + sec * 256

with open(DISK, 'r+b') as f:
    img = bytearray(f.read())

    # Locate USR LOG in the D81 directory (starts at T40 S3)
    data_t, data_s, reclen = None, None, None
    t, s = 40, 3
    while t and data_t is None:
        off = sector_offset(t, s)
        sec = img[off:off+256]
        if len(sec) < 256:   # truncated/invalid image — don't index past the end
            print("  WARNING: directory chain ran past end of image — skipping flag patch", file=sys.stderr)
            break
        t, s = sec[0], sec[1]
        for eo in range(2, 256, 32):
            entry = sec[eo:eo+32]
            if len(entry) < 22: break  # 8th dir slot is a 30-byte slice; fields end at reclen (21)
            if (entry[0] & 0x07) != 4: continue   # must be REL
            # Mask to 7-bit first, which turns 0xA0 shifted-space padding into
            # 0x20 — so strip trailing spaces/nulls (not 0xA0) to get the name.
            nm = bytes(b & 0x7f for b in entry[3:19]).rstrip(b'\x00\x20')
            if nm.upper() == b'USR LOG':
                data_t, data_s, reclen = entry[1], entry[2], entry[21]
                break

    if not data_t:
        print("  WARNING: USR LOG not found in seed image — skipping flag patch", file=sys.stderr)
        sys.exit(0)
    if reclen != RECORD_SIZE:
        print(f"  WARNING: USR LOG record size is {reclen}, expected {RECORD_SIZE} — skipping", file=sys.stderr)
        sys.exit(0)

    # Walk the sector chain; track flat byte position to identify record boundaries.
    patched = 0
    flat_pos = 0
    ct, cs = data_t, data_s
    while ct:
        soff = sector_offset(ct, cs)
        sec  = img[soff:soff+256]
        nt, ns = sec[0], sec[1]
        end  = (ns + 1) if nt == 0 else 256
        for bi in range(2, end):
            if (flat_pos % RECORD_SIZE) == FLAG_BYTE:
                img[soff + bi] |= FLAG_MASK
                patched += 1
            flat_pos += 1
        ct, cs = nt, ns

    f.seek(0)
    f.write(bytes(img))
    print(f"  USR LOG patched: {patched} record(s), USER_F_CLEAR_ON_MSG=1")

# Patch the flat backup too, if it exists.
if os.path.exists(FLAT_BACKUP):
    with open(FLAT_BACKUP, 'r+b') as fb:
        data = bytearray(fb.read())
        for i in range(FLAG_BYTE, len(data), RECORD_SIZE):
            data[i] |= FLAG_MASK
        fb.seek(0)
        fb.write(bytes(data))
    print(f"  Flat backup patched: {FLAT_BACKUP}")
PYPATCH

    # Remove old PRG files so we can replace with fresh builds.
    # Scan for any version-tagged boot/configure PRG (handles seed disks built
    # with a different version number — deleting by current name would miss them).
    "$C1541" "$OUTPUT_DISK" -list 2>/dev/null | while IFS= read -r line; do
        if [[ "$line" =~ \"([^\"]+)\"[[:space:]]+prg ]]; then
            cbm="${BASH_REMATCH[1]}"
            case "$cbm" in
                boot-*|configure-*)
                    "$C1541" "$OUTPUT_DISK" -delete "$cbm" >/dev/null 2>&1 || true
                    ;;
            esac
        fi
    done
    # Fixed-name overlays and example door.
    "$C1541" "$OUTPUT_DISK" -delete "ovl_msgs"  >/dev/null 2>&1 || true
    "$C1541" "$OUTPUT_DISK" -delete "ovl_wfc"   >/dev/null 2>&1 || true
    "$C1541" "$OUTPUT_DISK" -delete "ovl_boot"  >/dev/null 2>&1 || true
    "$C1541" "$OUTPUT_DISK" -delete "ovl_doors" >/dev/null 2>&1 || true
    "$C1541" "$OUTPUT_DISK" -delete "ovl_files"  >/dev/null 2>&1 || true
    "$C1541" "$OUTPUT_DISK" -delete "ovl_zmodem" >/dev/null 2>&1 || true
    "$C1541" "$OUTPUT_DISK" -delete "ovl_auth"   >/dev/null 2>&1 || true
    "$C1541" "$OUTPUT_DISK" -delete "fortune"   >/dev/null 2>&1 || true
    # Clear all SEQ files so stale gfiles disappear when the seed is reused.
    "$C1541" "$OUTPUT_DISK" -list 2>/dev/null | while IFS= read -r line; do
        if [[ "$line" =~ \"([^\"]+)\"[[:space:]]+seq ]]; then
            cbm="${BASH_REMATCH[1]}"
            "$C1541" "$OUTPUT_DISK" -delete "$cbm" >/dev/null 2>&1 || true
        fi
    done
else
    echo "Creating fresh .d81 disk image..."
    "$C1541" -format "d8,id" d81 "$OUTPUT_DISK" >/dev/null 2>&1 || true
fi

echo "Adding boot PRG..."
"$C1541" "$OUTPUT_DISK" -write "$BOOT_PRG" "$(cbm_name "$BOOT_PRG")" >/dev/null 2>&1 || { echo "ERROR: failed to write boot PRG" >&2; exit 1; }

echo "Adding configure PRG..."
"$C1541" "$OUTPUT_DISK" -write "$CONFIGURE_PRG" "$(cbm_name "$CONFIGURE_PRG")" >/dev/null 2>&1 || { echo "ERROR: failed to write configure PRG" >&2; exit 1; }

# Opt-in only (--extra-prg) — never written unless a caller explicitly asks
# for it, so `make disk` / a bare invocation keeps release images clean.
# c1541 -write with no ",s" suffix defaults to PRG, which is what a
# diagnostic like COPYALL needs (see CLAUDE.md's c1541 file-type note).
if [ -n "$EXTRA_PRG" ]; then
    extra_cbm="$(cbm_name "$EXTRA_PRG")"
    echo "Adding extra PRG (${extra_cbm})..."
    # Delete any stale copy first — harmless no-op on a fresh disk, but
    # matters if --seed-users points at an image that already has one.
    "$C1541" "$OUTPUT_DISK" -delete "$extra_cbm" >/dev/null 2>&1 || true
    "$C1541" "$OUTPUT_DISK" -write "$EXTRA_PRG" "$extra_cbm" >/dev/null 2>&1 || \
        { echo "ERROR: failed to write extra PRG ($EXTRA_PRG)" >&2; exit 1; }
fi

# Overlays below are read explicitly from build/c64/ (the REL build's
# output dir) — never build/c64/siec/. The SIEC build (make c64-siec) writes
# its own same-named overlays there instead, compiled to different addresses;
# this disk carries the REL binary, so it must only ever pick up REL overlays.

# Add MSGS overlay (bulletin board, message base, editor, user-pointer modules)
MSGS_OVL_PRG="$ROOT/build/c64/ovl_msgs.prg"
if [ -f "$MSGS_OVL_PRG" ]; then
    echo "Adding MSGS overlay..."
    "$C1541" "$OUTPUT_DISK" -write "$MSGS_OVL_PRG" "ovl_msgs" >/dev/null 2>&1 || \
        { echo "WARNING: failed to write MSGS overlay" >&2; }
fi

# Add WFC overlay (draw functions, RTC, datetime, wfc_display/update/init)
WFC_OVL_PRG="$ROOT/build/c64/ovl_wfc.prg"
if [ -f "$WFC_OVL_PRG" ]; then
    echo "Adding WFC overlay..."
    "$C1541" "$OUTPUT_DISK" -write "$WFC_OVL_PRG" "ovl_wfc" >/dev/null 2>&1 || \
        { echo "WARNING: failed to write WFC overlay" >&2; }
fi

# Add BOOT overlay (config-load code: cfg_init + boot-only parse helpers)
BOOT_OVL_PRG="$ROOT/build/c64/ovl_boot.prg"
if [ -f "$BOOT_OVL_PRG" ]; then
    echo "Adding BOOT overlay..."
    "$C1541" "$OUTPUT_DISK" -write "$BOOT_OVL_PRG" "ovl_boot" >/dev/null 2>&1 || \
        { echo "WARNING: failed to write BOOT overlay" >&2; }
fi

# Add DOORS overlay (door menu UI: action_doors_menu)
DOORS_OVL_PRG="$ROOT/build/c64/ovl_doors.prg"
if [ -f "$DOORS_OVL_PRG" ]; then
    echo "Adding DOORS overlay..."
    "$C1541" "$OUTPUT_DISK" -write "$DOORS_OVL_PRG" "ovl_doors" >/dev/null 2>&1 || \
        { echo "WARNING: failed to write DOORS overlay" >&2; }
fi

# Add FILES overlay (file area browse, Punter upload/download)
FILES_OVL_PRG="$ROOT/build/c64/ovl_files.prg"
if [ -f "$FILES_OVL_PRG" ]; then
    echo "Adding FILES overlay..."
    "$C1541" "$OUTPUT_DISK" -write "$FILES_OVL_PRG" "ovl_files" >/dev/null 2>&1 || \
        { echo "WARNING: failed to write FILES overlay" >&2; }
fi

# Add ZMODEM overlay (Zmodem send/receive protocol)
ZMODEM_OVL_PRG="$ROOT/build/c64/ovl_zmodem.prg"
if [ -f "$ZMODEM_OVL_PRG" ]; then
    echo "Adding ZMODEM overlay..."
    "$C1541" "$OUTPUT_DISK" -write "$ZMODEM_OVL_PRG" "ovl_zmodem" >/dev/null 2>&1 || \
        { echo "WARNING: failed to write ZMODEM overlay" >&2; }
fi

# Add AUTH overlay (interactive login: auth_prompt_login)
AUTH_OVL_PRG="$ROOT/build/c64/ovl_auth.prg"
if [ -f "$AUTH_OVL_PRG" ]; then
    echo "Adding AUTH overlay..."
    "$C1541" "$OUTPUT_DISK" -write "$AUTH_OVL_PRG" "ovl_auth" >/dev/null 2>&1 || \
        { echo "WARNING: failed to write AUTH overlay" >&2; }
fi

# Add the bundled example door (built by `make all` via the door-example target).
# Doors are normally sysop-supplied; this one ships so the DOOR PROGRAMS feature
# is demonstrable out of the box (register it in CONFIGURE: device 8, key F).
EXAMPLE_DOOR_PRG="$ROOT/build/c64/FORTUNE.prg"
if [ -f "$EXAMPLE_DOOR_PRG" ]; then
    echo "Adding example door (fortune)..."
    "$C1541" "$OUTPUT_DISK" -write "$EXAMPLE_DOOR_PRG" "fortune" >/dev/null 2>&1 || \
        { echo "WARNING: failed to write example door" >&2; }
else
    # Do NOT skip silently. A missing door is invisible on the resulting disk
    # but not harmless downstream: src-diag/copyall.c's file list includes
    # FORTUNE unconditionally, so every COPYALL run to a physical drive
    # reports a failure for a file that was never built. That cost real
    # debugging time when it showed up as "DONE. 1 FAILED" with no clue why.
    echo "WARNING: $EXAMPLE_DOOR_PRG not found — the example door will be" >&2
    echo "         missing from $(basename "$OUTPUT_DISK"), the DOORS feature" >&2
    echo "         will have nothing to demonstrate, and COPYALL will report" >&2
    echo "         FORTUNE as a failure. Run 'make door-example' first." >&2
fi

# Add config data file if present
if [ -f "$DATA_DIR/config" ]; then
    echo "Adding config..."
    "$C1541" "$OUTPUT_DISK" -write "$DATA_DIR/config" "config,s" >/dev/null 2>&1 || true
fi

# Add seed CALLERS log so the WFC screen has initial entries to show.
# The live disk's callers file is preserved across builds (not replaced if
# the seed disk already has one); only fresh D81s get the sample data.
CALLERS_SEED="$ROOT/data/callers"
if [ -f "$CALLERS_SEED" ]; then
    echo "Adding seed callers log..."
    "$C1541" "$OUTPUT_DISK" -write "$CALLERS_SEED" "callers,s" >/dev/null 2>&1 || \
        echo "  WARNING: failed to write callers seed"
fi

# Add access-levels seed file if present (SEQ — note the ,s suffix).
ACCESS_SEED="$ROOT/data/access"
if [ -f "$ACCESS_SEED" ]; then
    echo "Adding access levels..."
    "$C1541" "$OUTPUT_DISK" -write "$ACCESS_SEED" "access,s" >/dev/null 2>&1 || \
        echo "  WARNING: failed to write access seed"
fi

# Add gfiles (terminal-type-specific display files)
GFILES_DIR="$ROOT/data/gfiles"
if [ -d "$GFILES_DIR" ]; then
    echo "Adding gfiles..."
    while IFS= read -r -d '' f; do
        [ -f "$f" ] || continue
        cbm="$(basename "$f")"
        "$C1541" "$OUTPUT_DISK" -write "$f" "$cbm,s" >/dev/null 2>&1 || \
            echo "  WARNING: failed to write gfile: $cbm"
        echo "  + $cbm"
    done < <(find "$GFILES_DIR" -maxdepth 1 -type f ! -name '.*' -print0 | sort -z)
fi
#   Without --seed-users/--fetch-users, the REL file is absent from this image.
#   Run CONFIGURE [I]nitialize on the C64 to create it, or use --fetch-users to
#   preserve the live copy from the U64.

# NOTE: the DOORS table (registration) is created by the real BBS via CONFIGURE
# (a correct CBM REL with valid side sectors).  A hand-built REL passed c1541
# -validate but DOS could not read its records, so there is no host-side seed.
# Register the bundled example door once in CONFIGURE > DOOR PROGRAMS; the table
# then persists across --seed-users builds (REL files are not deleted on reseed).

echo "✓ Disk image created: $OUTPUT_DISK"
"$C1541" "$OUTPUT_DISK" -list 2>/dev/null | head -20
