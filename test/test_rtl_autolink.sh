#!/bin/sh
# ZXN integration check for the driver's --rtl=auto pruning.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCE="$ROOT/test/rtl_autolink_test.rift"
TMPDIR_AUDIT=$(mktemp -d "${TMPDIR:-/tmp}/rift-autolink.XXXXXX")
trap 'rm -rf "$TMPDIR_AUDIT"' EXIT HUP INT TERM

build_mode() {
  mode=$1
  output="$TMPDIR_AUDIT/$mode.exe"
  log="$TMPDIR_AUDIT/$mode.log"

  if ! "$ROOT/rift" --debug --target=zxn --zxn-test --rtl="$mode" \
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

  debug_dir=$(sed -n 's/^rift: debug workspace: //p' "$log" | head -1)
  if [ -z "$debug_dir" ] || [ ! -d "$debug_dir" ]; then
    cat "$log"
    echo "FAIL: --rtl=$mode did not report its debug workspace" >&2
    exit 1
  fi
  cp "$debug_dir/${mode}_CODE.bin" "$TMPDIR_AUDIT/${mode}_CODE.bin"
  if [ ! -f "$TMPDIR_AUDIT/$mode.nex" ] || [ ! -f "$TMPDIR_AUDIT/${mode}_CODE.bin" ]; then
    cat "$log"
    echo "FAIL: --rtl=$mode produced no code artifact" >&2
    exit 1
  fi

  MODE_CODE_BYTES=$(wc -c < "$TMPDIR_AUDIT/${mode}_CODE.bin" | tr -d '[:space:]')
  MODE_BSS_END=$(awk '/ZXN layout: BSS ends at/ { value = $6; sub(/^0x/, "", value); sub(/;.*/, "", value); print value; exit }' "$log")
  rm -rf "$debug_dir"
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

SPRITE_SOURCE="$ROOT/test/fixtures/sprite_type_methods.rift"
if ! "$ROOT/rift" --target=zxn "$SPRITE_SOURCE" "$TMPDIR_AUDIT/sprite.exe" \
    >"$TMPDIR_AUDIT/sprite.log" 2>&1; then
  cat "$TMPDIR_AUDIT/sprite.log"
  echo "FAIL: auto-selected Sprite did not link for ZXN" >&2
  exit 1
fi
grep -q '^RTL components: sprite ' "$TMPDIR_AUDIT/sprite.log"
grep -q 'ZXN profile: startup=31, .*tiny-core=1' "$TMPDIR_AUDIT/sprite.log"
grep -q '/lib/zxn/sprite.asm' "$TMPDIR_AUDIT/sprite.log"
if grep -q '/lib/sprite_host.c' "$TMPDIR_AUDIT/sprite.log"; then
  echo "FAIL: ZXN sprite build linked host-only state" >&2
  exit 1
fi
echo "PASS: byte-backed Sprite API auto-links on ZXN"
