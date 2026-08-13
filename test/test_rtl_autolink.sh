#!/bin/sh
# ZXN integration check for the driver's --rtl=auto pruning.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCE="$ROOT/test/rtl_autolink_test.rkr"
TMPDIR_AUDIT=$(mktemp -d "${TMPDIR:-/tmp}/rock-autolink.XXXXXX")
trap 'rm -rf "$TMPDIR_AUDIT"' EXIT HUP INT TERM

build_mode() {
  mode=$1
  output="$TMPDIR_AUDIT/$mode.exe"
  log="$TMPDIR_AUDIT/$mode.log"

  if ! "$ROOT/rock" --target=zxn --zxn-test --rtl="$mode" \
      "$SOURCE" "$output" >"$log" 2>&1; then
    cat "$log"
    echo "FAIL: --rtl=$mode build failed" >&2
    exit 1
  fi

  if ! grep -q 'ZXN layout: BSS ends at' "$log"; then
    cat "$log"
    echo "FAIL: --rtl=$mode did not pass the ZXN BSS stack guard" >&2
    exit 1
  fi

  if [ ! -f "$TMPDIR_AUDIT/$mode.nex" ] || [ ! -f "$TMPDIR_AUDIT/${mode}_CODE.bin" ]; then
    cat "$log"
    echo "FAIL: --rtl=$mode produced no code artifact" >&2
    exit 1
  fi

  MODE_CODE_BYTES=$(wc -c < "$TMPDIR_AUDIT/${mode}_CODE.bin" | tr -d '[:space:]')
  MODE_BSS_END=$(awk '/ZXN layout: BSS ends at/ { value = $6; sub(/^0x/, "", value); sub(/;.*/, "", value); print value; exit }' "$log")
}

build_mode auto
AUTO_CODE_BYTES=$MODE_CODE_BYTES
AUTO_BSS_END=$MODE_BSS_END

for component in keyboard.c plot.c draw.c random.c nextreg.c helpers.c; do
  if ! grep -q "lib/$component" "$TMPDIR_AUDIT/auto.log"; then
    cat "$TMPDIR_AUDIT/auto.log"
    echo "FAIL: --rtl=auto omitted required component $component" >&2
    exit 1
  fi
done
if grep -q 'lib/fmath.c' "$TMPDIR_AUDIT/auto.log"; then
  cat "$TMPDIR_AUDIT/auto.log"
  echo "FAIL: --rtl=auto retained an unused component" >&2
  exit 1
fi

build_mode all
ALL_CODE_BYTES=$MODE_CODE_BYTES
ALL_BSS_END=$MODE_BSS_END

if [ "$AUTO_CODE_BYTES" -ge "$ALL_CODE_BYTES" ]; then
  echo "FAIL: auto-linked code ($AUTO_CODE_BYTES bytes) is not smaller than all-RTL code ($ALL_CODE_BYTES bytes)" >&2
  exit 1
fi

echo "PASS: RTL auto-linker resolved keyboard, nextreg, random, helpers, and draw/plot"
echo "PASS: --rtl=auto code is $AUTO_CODE_BYTES bytes (BSS end 0x$AUTO_BSS_END)"
echo "PASS: --rtl=all code is $ALL_CODE_BYTES bytes (BSS end 0x$ALL_BSS_END)"

SPRITE_SOURCE="$ROOT/test/fixtures/opaque_sprite_method_only.rkr"
if ! "$ROOT/rock" --target=zxn "$SPRITE_SOURCE" "$TMPDIR_AUDIT/sprite.exe" \
    >"$TMPDIR_AUDIT/sprite.log" 2>&1; then
  cat "$TMPDIR_AUDIT/sprite.log"
  echo "FAIL: auto-selected Sprite did not link for ZXN" >&2
  exit 1
fi
grep -q 'RTL components: .*core sprite' "$TMPDIR_AUDIT/sprite.log"
grep -q '/lib/sprite.c' "$TMPDIR_AUDIT/sprite.log"
echo "PASS: opaque Sprite auto-links on ZXN"
