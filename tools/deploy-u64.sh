#!/usr/bin/env bash
# Deploy TURBO/64 BBS disk image to Ultimate64 storage
#
# Usage: tools/deploy-u64.sh [options]
#   -l, --location <loc>  Deployment location: usb1 (default), sd, or full path
#   --boards              Also restore data/boards-seed.d81 as BOARDS.D81 on U64
#                         (drive-9 message-board disk; use after extract-boards.sh)
#
# Environment:
#   T64_SD_PATH            Override destination path (default: /USB1/BBS)
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/vendor/c64u/bin/c64u"
VERSION_COMPACT="$(grep 'BBS_RELEASE_VERSION_COMPACT' "$ROOT/include/bbs/version.h" | cut -d'"' -f2)"

DISK_IMAGE="$ROOT/build/c64/TURBO64-${VERSION_COMPACT}.d81"
BOARDS_SEED="$ROOT/data/boards-seed.d81"
LOCATION=""
DEPLOY_BOARDS=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -l|--location)
            LOCATION="$2"
            shift 2
            ;;
        --boards)
            DEPLOY_BOARDS=1
            shift
            ;;
        -h|--help)
            sed -n '2,10p' "$0"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

LOCATION="${LOCATION:-${T64_SD_PATH:-usb1}}"

case "$LOCATION" in
    bbs)  SD_PATH="/BBS"       ;;
    usb0) SD_PATH="/USB0/BBS"  ;;
    usb1) SD_PATH="/USB1/BBS"  ;;
    sd)   SD_PATH="/SD/BBS"    ;;
    *)    SD_PATH="$LOCATION"  ;;
esac

if [ ! -x "$BIN" ]; then
    echo "ERROR: c64u not found at $BIN" >&2
    echo "Run 'tools/install-c64u.sh' first." >&2
    exit 1
fi

if [ ! -f "$DISK_IMAGE" ]; then
    echo "ERROR: Disk image not found: $DISK_IMAGE" >&2
    echo "Run 'tools/assemble-d81.sh' first" >&2
    exit 1
fi

SD_PATH="${SD_PATH%/}"
DISK_NAME="$(basename "$DISK_IMAGE" | tr '[:lower:]' '[:upper:]')"

echo "╔════════════════════════════════════════════════════════════════╗"
echo "║              Deploying D8 BBS to Ultimate64                    ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""
echo "Destination:   $SD_PATH"
echo "Version:       $VERSION_COMPACT"
echo ""

echo "Deploying disk image..."
"$BIN" fs mkdir "${SD_PATH}" 2>/dev/null || true
"$BIN" fs upload "$DISK_IMAGE" "${SD_PATH}/${DISK_NAME}" || {
    echo "  ✗ Failed to upload disk image"
    exit 1
}
echo "  ✓ ${DISK_NAME} → ${SD_PATH}/"

echo ""

# -- Optional: restore boards seed disk -----------------------------------
if [ "$DEPLOY_BOARDS" -eq 1 ]; then
    if [ ! -f "$BOARDS_SEED" ]; then
        echo "ERROR: boards seed not found: $BOARDS_SEED" >&2
        echo "Run 'tools/extract-boards.sh' first." >&2
        exit 1
    fi
    BOARDS_NAME="BOARDS-${VERSION_COMPACT}.D81"
    echo "Restoring boards disk..."
    "$BIN" fs upload "$BOARDS_SEED" "${SD_PATH}/${BOARDS_NAME}" || {
        echo "  ✗ Failed to upload ${BOARDS_NAME}"
        exit 1
    }
    echo "  ✓ ${BOARDS_NAME} → ${SD_PATH}/"
    echo ""
fi

echo "✓ Deployment complete!"
echo ""
echo "On Ultimate64, load from:"
echo "  @0 \"TURBO64-${VERSION_COMPACT}.D81\""