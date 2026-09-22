#!/usr/bin/env bash
# Full release pipeline for TURBO/64 BBS: build, tag, push, publish GitHub release
#
# Usage: tools/release.sh              # full release
#        tools/release.sh --dry-run    # build artifacts, show plan, no tag/push/publish
#        tools/release.sh --skip-tag   # build + publish to existing tag
#        tools/release.sh --force      # delete existing release + recreate from scratch
#        tools/release.sh --force --skip-tag  # recreate release on existing tag
#        tools/release.sh --prerelease # mark the GitHub release as a pre-release
#        tools/release.sh --no-prerelease  # force a full (non-pre) release
#
# Pre-release: by default any 0.x version is published as a GitHub pre-release
# (the project is pre-1.0); --prerelease/--no-prerelease override that.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

DRY_RUN=0
SKIP_TAG=0
FORCE=0
PRERELEASE=auto   # auto = pre-release iff version is 0.x; or 1 / 0 to force

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run)  DRY_RUN=1; shift ;;
        --skip-tag) SKIP_TAG=1; shift ;;
        --force)    FORCE=1; shift ;;
        --prerelease)    PRERELEASE=1; shift ;;
        --no-prerelease) PRERELEASE=0; shift ;;
        -h|--help)
            sed -n '2,12p' "$0"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Usage: tools/release.sh [--dry-run|--skip-tag|--force|--prerelease|--no-prerelease]" >&2
            exit 1
            ;;
    esac
done

# --- Prerequisites ---

command -v gh >/dev/null 2>&1 || {
    echo -e "${RED}ERROR: gh CLI is not installed.${NC}" >&2
    echo "Install from: https://cli.github.com/" >&2
    exit 1
}

command -v zip >/dev/null 2>&1 || {
    echo -e "${RED}ERROR: zip is not installed.${NC}" >&2
    exit 1
}

gh auth status >/dev/null 2>&1 || {
    echo -e "${RED}ERROR: gh CLI is not authenticated.${NC}" >&2
    echo "Run: gh auth login" >&2
    exit 1
}

if ! git diff --quiet || ! git diff --cached --quiet; then
    echo -e "${RED}ERROR: Working tree is dirty. Commit or stash changes first.${NC}" >&2
    git status --short >&2
    exit 1
fi

# Refuse to release if the local branch is behind its upstream — tagging
# here would publish stale code.
BRANCH="$(git rev-parse --abbrev-ref HEAD)"
if git ls-remote --exit-code origin "refs/heads/$BRANCH" >/dev/null 2>&1; then
    if ! git fetch --quiet origin "$BRANCH"; then
        echo -e "${RED}ERROR: Could not fetch origin/$BRANCH. Check network/auth.${NC}" >&2
        exit 1
    fi
    if ! git merge-base --is-ancestor "origin/$BRANCH" HEAD; then
        echo -e "${RED}ERROR: Local $BRANCH is behind origin/$BRANCH. Pull/rebase first.${NC}" >&2
        exit 1
    fi
fi

# --- Version ---

VERSION="$(grep 'BBS_RELEASE_VERSION_COMPACT' "$ROOT/include/bbs/version.h" | cut -d'"' -f2)"
if [ -z "$VERSION" ]; then
    echo -e "${RED}ERROR: Could not read version from include/bbs/version.h${NC}" >&2
    exit 1
fi
TAG="v$VERSION"

# Resolve pre-release: default to pre-release for any pre-1.0 (0.x) version.
if [ "$PRERELEASE" = "auto" ]; then
    case "$VERSION" in
        0.*) PRERELEASE=1 ;;
        *)   PRERELEASE=0 ;;
    esac
fi

echo -e "${BLUE}TURBO/64 BBS release pipeline${NC}"
echo -e "  Version: ${GREEN}$VERSION${NC}"
echo -e "  Tag:     ${GREEN}$TAG${NC}"
echo -e "  Pre-release: ${GREEN}$([ "$PRERELEASE" = 1 ] && echo yes || echo no)${NC}"
echo ""

# --- Tag check ---

if [ "$SKIP_TAG" -eq 0 ]; then
    if git rev-parse "$TAG" >/dev/null 2>&1; then
        echo -e "${RED}ERROR: Tag $TAG already exists.${NC}" >&2
        echo "Use --skip-tag to publish to an existing tag, or bump the version." >&2
        exit 1
    fi
fi

# --- Delete existing release if --force ---

if [ "$FORCE" -eq 1 ]; then
    if gh release view "$TAG" >/dev/null 2>&1; then
        echo -e "${YELLOW}--force: deleting existing release $TAG...${NC}"
        gh release delete "$TAG" --yes
        echo ""
    else
        echo -e "${YELLOW}--force: no existing release $TAG to delete, proceeding.${NC}"
    fi
fi

# --- Build ---

echo -e "${BLUE}Cleaning and building...${NC}"
make -C "$ROOT" clean && make -C "$ROOT" all && make -C "$ROOT" disk
echo -e "${BLUE}Building the SoftIEC (T64_STORE_SEQ) half...${NC}"
make -C "$ROOT" c64-siec && make -C "$ROOT" editor-siec
echo ""

# --- Verify build outputs ---

BOOT_PRG="$ROOT/build/c64/BOOT-${VERSION}.prg"
CONFIGURE_PRG="$ROOT/build/c64/CONFIGURE-${VERSION}.prg"
DISK_IMG="$ROOT/build/c64/TURBO64-${VERSION}.d81"
BLANK_DISK="$ROOT/data/blank-disk.d81"
DOOR_PRG="$ROOT/build/c64/FORTUNE.prg"

SIEC_DIR="$ROOT/build/c64/siec"
# BOOT-SIEC / CONFIGURE-SIEC are fixed, version-independent names (see
# Makefile) — unlike BOOT_PRG/CONFIGURE_PRG above, they do not carry
# $VERSION. The version is still compiled in and shown on the boot screen.
BOOT_SIEC_PRG="$SIEC_DIR/BOOT-SIEC.prg"
CONFIGURE_SIEC_PRG="$SIEC_DIR/CONFIGURE-SIEC.prg"
SIEC_OVERLAYS=(ovl_boot.prg ovl_wfc.prg ovl_msgs.prg ovl_doors.prg ovl_files.prg ovl_zmodem.prg ovl_auth.prg)

MIGRATE_TOOL="$ROOT/tools/migrate-d81.py"
VERSION_HEADER="$ROOT/include/bbs/version.h"

for f in "$BOOT_PRG" "$CONFIGURE_PRG" "$DISK_IMG" "$BLANK_DISK" "$DOOR_PRG" \
         "$BOOT_SIEC_PRG" "$CONFIGURE_SIEC_PRG" "$MIGRATE_TOOL" "$VERSION_HEADER"; do
    if [ ! -f "$f" ]; then
        echo -e "${RED}ERROR: Missing build output: $f${NC}" >&2
        exit 1
    fi
done
for ovl in "${SIEC_OVERLAYS[@]}"; do
    if [ ! -f "$SIEC_DIR/$ovl" ]; then
        echo -e "${RED}ERROR: Missing SIEC overlay: $SIEC_DIR/$ovl${NC}" >&2
        exit 1
    fi
done

# --- Stage release assets ---
#
# The archive mirrors the repo's own relative layout for the SoftIEC half
# (tools/migrate-d81.py, include/bbs/version.h, build/c64/siec/*,
# build/c64/FORTUNE.prg) rather than flattening it. That is what lets a
# SysOp run the exact command tools/deploy.sh uses on real hardware,
# unzip-and-cd, with zero extra flags:
#
#   python3 tools/migrate-d81.py TURBO64-<ver>.d81 siec-tree \
#       --base /USB1/TURBO64 --device 11
#
# migrate-d81.py's own defaults (--siec-build-dir build/c64/siec,
# --door-build-dir build/c64) then resolve correctly with no path surgery,
# and there is only one documented invocation to keep in sync instead of a
# second "for the zip" variant. migrate-d81.py copies both BOOT-SIEC.prg and
# CONFIGURE-SIEC.prg into the tree automatically — no manual copy step.
# include/bbs/version.h ships alongside for reference (the compiled-in
# version shown on the boot screen); migrate-d81.py no longer reads it,
# since the SIEC binary names are fixed and version-independent.

RELEASE_DIR="$ROOT/build/release"
rm -rf "$RELEASE_DIR"
mkdir -p "$RELEASE_DIR"

cp "$DISK_IMG"      "$RELEASE_DIR/TURBO64-${VERSION}.d81"
cp "$BOOT_PRG"      "$RELEASE_DIR/BOOT-${VERSION}.prg"
cp "$CONFIGURE_PRG" "$RELEASE_DIR/CONFIGURE-${VERSION}.prg"
cp "$BLANK_DISK"    "$RELEASE_DIR/BOARDS-${VERSION}.d81"

# Storage diagnostics are deliberately NOT shipped. They are developer tools,
# two of them (WIPE, CLEAN) delete files, and dropping extra PRGs beside
# BOOT and CONFIGURE only obscures what a SysOp is meant to run. Build them
# from source with `make diag` when they are actually needed.

mkdir -p "$RELEASE_DIR/tools" "$RELEASE_DIR/include/bbs" \
         "$RELEASE_DIR/build/c64/siec"
cp "$MIGRATE_TOOL"    "$RELEASE_DIR/tools/migrate-d81.py"
cp "$VERSION_HEADER"  "$RELEASE_DIR/include/bbs/version.h"
cp "$DOOR_PRG"        "$RELEASE_DIR/build/c64/FORTUNE.prg"
cp "$BOOT_SIEC_PRG"       "$RELEASE_DIR/build/c64/siec/"
cp "$CONFIGURE_SIEC_PRG"  "$RELEASE_DIR/build/c64/siec/"
for ovl in "${SIEC_OVERLAYS[@]}"; do
    cp "$SIEC_DIR/$ovl" "$RELEASE_DIR/build/c64/siec/"
done

sed "s/__VERSION__/$VERSION/g" "$ROOT/data/release/file_id.diz.tmpl" > "$RELEASE_DIR/FILE_ID.DIZ"
sed "s/__VERSION__/$VERSION/g" "$ROOT/data/release/readme.txt.tmpl" > "$RELEASE_DIR/README.txt"

echo -e "${GREEN}Release assets staged:${NC}"
(cd "$RELEASE_DIR" && find . -type f | LC_ALL=C sort)
echo ""

# --- Create ZIP archive ---

ZIP_FILE="$RELEASE_DIR/TURBO64-${VERSION}.zip"
echo -e "${BLUE}Creating ZIP archive...${NC}"
(
    cd "$RELEASE_DIR"
    zip -q -r "TURBO64-${VERSION}.zip" . -x "TURBO64-${VERSION}.zip"
)

# --- Release notes ---

NOTES_FILE="$ROOT/data/release/notes.md"
NOTES_TMP="$RELEASE_DIR/notes.md"
if [ -f "$NOTES_FILE" ]; then
    sed "s/__VERSION__/$VERSION/g" "$NOTES_FILE" > "$NOTES_TMP"
    RELEASE_NOTES="$(cat "$NOTES_TMP")"
    echo -e "${GREEN}Using release notes from data/release/notes.md${NC}"
else
    PREV_TAG="$(git tag --sort=-version:refname 2>/dev/null | head -2 | tail -1)" || true
    if [ -n "$PREV_TAG" ] && [ "$PREV_TAG" != "$TAG" ]; then
        RELEASE_NOTES="$(git log "$PREV_TAG..HEAD" --oneline)"
    else
        RELEASE_NOTES="$(git log --oneline)"
    fi
    echo -e "${YELLOW}No data/release/notes.md found, using git log for release notes.${NC}"
fi
echo ""

# --- Dry run exit ---

if [ "$DRY_RUN" -eq 1 ]; then
    echo -e "${YELLOW}DRY RUN — no tag, push, or publish will be performed.${NC}"
    echo ""
    echo "Planned steps:"
    if [ "$FORCE" -eq 1 ]; then
        echo "  0. gh release delete \"$TAG\" --yes  (if release exists)"
    fi
    if [ "$SKIP_TAG" -eq 0 ]; then
        echo "  1. git tag -a \"$TAG\" -m \"TURBO/64 BBS v$VERSION\""
        echo "  2. git push origin \"$TAG\""
    fi
    echo "  3. gh release create \"$TAG\" \\"
    echo "       TURBO64-${VERSION}.zip \\"
    echo "       --title \"TURBO/64 BBS v$VERSION\" \\"
    echo "       --notes \"<from notes.md or git log>\" \\"
    echo "       $([ "$PRERELEASE" = 1 ] && echo --prerelease)"
    echo ""
    echo -e "Release notes preview:"
    echo "---"
    echo "$RELEASE_NOTES"
    echo "---"
    echo ""
    echo -e "${GREEN}Dry run complete.${NC}"
    exit 0
fi

# --- Tag and push ---

if [ "$SKIP_TAG" -eq 0 ]; then
    echo -e "${BLUE}Creating tag $TAG...${NC}"
    git tag -a "$TAG" -m "TURBO/64 BBS v$VERSION"
    echo -e "${BLUE}Pushing tag $TAG...${NC}"
    git push origin "$TAG"
    echo ""
fi

# --- GitHub release ---

echo -e "${BLUE}Creating GitHub release $TAG...${NC}"
gh release create "$TAG" \
    "$ZIP_FILE" \
    --title "TURBO/64 BBS v$VERSION" \
    --notes "$RELEASE_NOTES" \
    $([ "$PRERELEASE" = 1 ] && echo --prerelease)

echo ""
echo -e "${GREEN}Release v$VERSION published!${NC}"