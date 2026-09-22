#!/usr/bin/env bash
# Snapshot the live message-board disk from Ultimate64 storage.
#
# Downloads BOARDS-<version>.D81 (the drive-9 disk) from the U64 to
# data/boards-seed.d81, so `deploy-u64.sh --boards` can put it back after a
# rebuild. The counterpart of extract-users.sh for the boards disk.
#
# Usage: tools/extract-boards.sh [options]
#   -l, --location <loc>  Source location: usb1 (default), sd, or full path
#   -o, --output <path>   Local destination (default: data/boards-seed.d81)
#   -h, --help
#
# Environment:
#   T64_SD_PATH            Override source path (default: /USB1/BBS)
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/vendor/c64u/bin/c64u"
VERSION_COMPACT="$(grep 'BBS_RELEASE_VERSION_COMPACT' "$ROOT/include/bbs/version.h" | cut -d'"' -f2)"

LOCATION=""
OUTPUT="$ROOT/data/boards-seed.d81"

while [[ $# -gt 0 ]]; do
    case "$1" in
        -l|--location) LOCATION="$2"; shift 2 ;;
        -o|--output)   OUTPUT="$2";   shift 2 ;;
        -h|--help)     sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

LOCATION="${LOCATION:-${T64_SD_PATH:-usb1}}"
case "$LOCATION" in
    usb0) SD_PATH="/USB0/BBS" ;;
    usb1) SD_PATH="/USB1/BBS" ;;
    sd)   SD_PATH="/SD/BBS"   ;;
    *)    SD_PATH="$LOCATION" ;;
esac
SD_PATH="${SD_PATH%/}"

if [ ! -x "$BIN" ]; then
    echo "ERROR: c64u not found at $BIN" >&2
    echo "Run 'tools/install-c64u.sh' first." >&2
    exit 1
fi

REMOTE="${SD_PATH}/BOARDS-${VERSION_COMPACT}.D81"
echo "Fetching ${REMOTE} -> ${OUTPUT}"
"$BIN" fs download "$REMOTE" "$OUTPUT" || {
    echo "  ✗ Failed to download BOARDS-${VERSION_COMPACT}.D81 from ${SD_PATH}" >&2
    exit 1
}
echo "  ✓ boards seed saved: ${OUTPUT}"
