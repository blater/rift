#!/bin/sh
# Focused contract for the startup-31, stdio-free ZXN console and input stack.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FIXTURES="$ROOT/test/fixtures"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rift-zxn-console.XXXXXX")
EMULATOR=${ZESARUX_BIN:-/Users/blater/src/zesarux/src/zesarux}
DEBUG_DIRS=
trap 'for dir in $DEBUG_DIRS; do rm -rf "$dir"; done; rm -rf "$WORK"' EXIT HUP INT TERM

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

require_line() {
  pattern=$1
  file=$2
  message=$3
  grep -Eq "$pattern" "$file" || fail "$message"
}

map_section_matches() {
  section=$1
  allowed=$2
  map=$3
  awk -v symbol="__${section}_size" -v allowed="$allowed" '
    $1 == symbol && $2 == "=" {
      found = 1
      value = substr($3, 2)
      if (value !~ allowed) exit 1
    }
    END { if (!found) exit 1 }
  ' "$map"
}

assert_no_stdio() {
  name=$1
  generated=$2
  map=$3

  if grep -En '(^|[^[:alnum:]_])(printf|fprintf|snprintf|putchar|getchar|fflush)[[:space:]]*\(' "$generated"; then
    fail "$name generated C still calls a stdio function"
  fi
  if grep -En '(^|[^[:alnum:]_])(stdin|stdout|stderr)([^[:alnum:]_]|$)' "$generated"; then
    fail "$name generated C still refers to a standard stream"
  fi

  for section in code_stdio code_driver_terminal_input \
      code_driver_terminal_output; do
    map_section_matches "$section" '^0000$' "$map" ||
      fail "$name retains non-empty Z88DK $section"
  done
  map_section_matches data_stdio '^0000$' "$map" ||
    fail "$name retains non-empty Z88DK data_stdio"
  map_section_matches bss_stdio '^0000$' "$map" ||
    fail "$name retains non-empty Z88DK bss_stdio"

  if grep -Ei '^[[:space:]]*(_?(printf|fprintf|snprintf|putchar|getchar|fflush|stdin|stdout|stderr)|asm_(v?fprintf|snprintf|putchar|getchar|fflush)[[:alnum:]_]*)[[:space:]]*=.*; addr' "$map"; then
    fail "$name map retains a linked stdio entry point"
  fi
}

build_case() {
  name=$1
  fixture=$2
  expected_profile=${3:-31}
  log="$WORK/$name.log"

  if ! "$ROOT/rift" --debug --target=zxn --zxn-test \
      "$FIXTURES/$fixture" "$WORK/$name.nex" >"$log" 2>&1; then
    cat "$log" >&2
    fail "$name did not compile; the stdio-free console contract is not implemented"
  fi

  debug_dir=$(sed -n 's/^rift: debug workspace: //p' "$log" | head -1)
  [ -n "$debug_dir" ] && [ -d "$debug_dir" ] || {
    cat "$log" >&2
    fail "$name build did not retain a debug workspace"
  }
  DEBUG_DIRS="$DEBUG_DIRS $debug_dir"

  generated="$debug_dir/$name.exe.c"
  sidecar="$debug_dir/$name.exe.components"
  map="$debug_dir/$name.map"
  code="$debug_dir/${name}_CODE.bin"
  for artifact in "$generated" "$sidecar" "$map" "$code" "$WORK/$name.nex"; do
    [ -f "$artifact" ] || fail "$name omitted build artifact $artifact"
  done

  if [ "$expected_profile" = 31 ]; then
    require_line 'ZXN profile: startup=31,' "$log" \
      "$name selected a CRT startup other than 31"
    require_line '^@profile=.*-31$' "$sidecar" \
      "$name component sidecar is not startup-31 eligible"
    assert_no_stdio "$name" "$generated" "$map"
  else
    require_line 'ZXN profile: startup=1,' "$log" \
      "$name test-only embedded seam did not select the full profile"
    require_line '^@profile=full$' "$sidecar" \
      "$name test-only embedded seam did not select the full profile"
  fi

  cp "$generated" "$WORK/$name.c"
  cp "$sidecar" "$WORK/$name.components"
  cp "$map" "$WORK/$name.map"
  cp "$code" "$WORK/${name}_CODE.bin"
  bytes=$(wc -c <"$code" | tr -d '[:space:]')
  if [ "$expected_profile" = 31 ]; then
    echo "PASS: $name links at startup 31 without stdio ($bytes resident bytes)"
  else
    echo "PASS: $name test seam links in the full profile ($bytes resident bytes)"
  fi
}

run_case() {
  name=$1
  shift
  "$ROOT/tools/rift-emu" --target zxn --emulator-bin "$EMULATOR" \
    --timeout-seconds 8 --artifacts "$WORK/$name-artifacts" \
    --require-emulator "$@" test "$WORK/$name.nex"
}

build_case typed zxn_console_typed_output.rift
build_case positioned zxn_console_positioned_output.rift
build_case scroll_cls zxn_console_scroll_cls.rift
build_case concat_order zxn_console_concat_order.rift
build_case input_profile zxn_console_input_profile.rift
build_case input_behavior zxn_console_input_behavior.rift full
build_case keyboard_matrix zxn_keyboard_matrix_input.rift
build_case tiny_loop zxn_console_tiny_loop_scroll.rift
build_case aggregate zxn_console_aggregate_helper.rift

require_line 'rift_console_' "$WORK/typed.c" \
  'typed output was not lowered through the Rift console backend'
require_line 'rift_console_(putc_at|putchar_at)' "$WORK/positioned.c" \
  'positioned output omitted the exact-cell console primitive'
require_line 'rift_console_(putc_addr|putchar_addr)' "$WORK/positioned.c" \
  'positioned output omitted the exact-address console primitive'
require_line 'rift_console_(newline|println)' "$WORK/scroll_cls.c" \
  'println omitted the common console newline primitive'
require_line '^[[:space:]]*_?rift_console_clear[[:space:]]*=.*; addr' \
  "$WORK/scroll_cls.map" \
  'cls did not link the common console clear primitive'
require_line '^@profile=tiny-console-31$' "$WORK/tiny_loop.components" \
  'looped literal output incorrectly selected the non-scrolling tiny writer'

run_case typed
run_case positioned
run_case scroll_cls
run_case concat_order
run_case input_behavior
run_case keyboard_matrix --send-keys-after-memory 16384:165 --send-keys-ascii \
  49,50,51,52,53,54,55,56,57,48,113,119,101,114,116,121,117,105,111,112,97,115,100,102,103,104,106,107,108,122,120,99,118,98,110,109,32,13
run_case tiny_loop
run_case aggregate

echo 'PASS: typed output, positioning, black cls, scrolling, concat order, keyboard matrix/input decoding, and line input passed in ZEsarUX'
