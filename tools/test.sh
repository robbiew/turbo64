#!/usr/bin/env bash
# Build and run all host-side unit tests with the system C compiler.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/host"
CC="${CC:-cc}"
CFLAGS="-std=c99 -Wall -Wextra -I$ROOT/include -I$ROOT/tests"

mkdir -p "$OUT"
fails=0

run() {
    local name="$1"; shift
    # shellcheck disable=SC2086
    "$CC" $CFLAGS -o "$OUT/$name" "$@"
    if ! "$OUT/$name"; then fails=1; fi
}

run test_prompt_cursor_frame \
    "$ROOT/tests/test_prompt_cursor_frame.c" \
    "$ROOT/src/session/prompt_cursor_frame.c"

run test_telnet_iac -I"$ROOT/src/net" \
    "$ROOT/tests/test_telnet_iac.c" "$ROOT/src/net/telnet_iac.c"

run test_at_response -I"$ROOT/src/net" \
    "$ROOT/tests/test_at_response.c" "$ROOT/src/net/at_response.c"

run test_menu_garbage \
    "$ROOT/tests/test_menu_garbage.c" "$ROOT/src/features/menu.c"

run test_user_hash \
    "$ROOT/tests/test_user_hash.c" "$ROOT/src/data/user_hash.c"

run test_devspec \
    "$ROOT/tests/test_devspec.c" "$ROOT/src/data/devspec.c"

run test_devspec_seq -DT64_STORE_SEQ \
    "$ROOT/tests/test_devspec.c" "$ROOT/src/data/devspec.c"

run test_seq_region \
    "$ROOT/tests/test_seq_region.c" "$ROOT/src/hal/seq_region.c"

run test_door_abi "$ROOT/tests/test_door_abi.c"

run test_door_visible \
    "$ROOT/tests/test_door_visible.c" "$ROOT/src/data/door_visible.c"

run test_term_unxlate -I"$ROOT/src/term" \
    "$ROOT/tests/test_term_unxlate.c" \
    "$ROOT/src/term/term.c" "$ROOT/src/term/cp437_petscii.c" \
    "$ROOT/src/term/cp437_ascii.c" "$ROOT/src/term/ansi.c"

# Characterization: full CP437 translation matrix vs committed golden.
# shellcheck disable=SC2086
"$CC" $CFLAGS -I"$ROOT/src/term" -o "$OUT/test_term_xlate" \
    "$ROOT/tests/test_term_xlate.c" \
    "$ROOT/src/term/term.c" "$ROOT/src/term/cp437_petscii.c" \
    "$ROOT/src/term/cp437_ascii.c" "$ROOT/src/term/ansi.c"
if ! "$OUT/test_term_xlate" > "$OUT/term_xlate.txt"; then
    echo "term_xlate: dump binary failed"
    fails=1
elif ! diff -u "$ROOT/tests/golden/term_xlate.txt" "$OUT/term_xlate.txt"; then
    echo "term_xlate: output differs from tests/golden/term_xlate.txt"
    echo "  If the table change is intentional, regenerate and review the diff:"
    echo "    build/host/test_term_xlate > tests/golden/term_xlate.txt"
    fails=1
fi

if command -v python3 >/dev/null 2>&1; then
    python3 "$ROOT/tests/test_migrate_d81.py" || fails=1
    python3 "$ROOT/tests/test_siec_clean.py" || fails=1
else
    echo "python3 not found - skipping migrate-d81 tests"
fi

if [ "$fails" -ne 0 ]; then echo "HOST TESTS FAILED"; exit 1; fi
echo "ALL HOST TESTS PASSED"
