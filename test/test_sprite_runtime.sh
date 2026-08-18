#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rift-sprite-runtime.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

gcc -std=c11 -Wall -Wextra -Werror -I"$ROOT/src/lib" \
  -o "$WORK/sprite-runtime" "$ROOT/test/sprite_runtime_test.c" \
  "$ROOT/src/lib/sprite_host.c"

"$WORK/sprite-runtime"

expect_failure() {
  case_name=$1
  diagnostic=$2
  if "$WORK/sprite-runtime" "$case_name" >"$WORK/$case_name.log" 2>&1; then
    echo "FAIL: Sprite.$case_name accepted an invalid dynamic value" >&2
    exit 1
  fi
  grep -q "rift: Sprite.$diagnostic" "$WORK/$case_name.log"
}

expect_failure position-slot 'position slot must be 0..127'
expect_failure frame-slot 'frame slot must be 0..127'
expect_failure show-slot 'show slot must be 0..127'
expect_failure hide-slot 'hide slot must be 0..127'
expect_failure x 'position x must be 0..319'
expect_failure y 'position y must be 0..255'
expect_failure frame4 'frame is outside uploaded pattern data'
expect_failure frame8 'frame is outside uploaded pattern data'

zcc +zxn -vn -c -clib=sdcc_iy -I"$ROOT/src/lib" \
  -o "$WORK/sprite.o" "$ROOT/src/lib/zxn/sprite.asm"
zcc +zxn -vn -c -clib=sdcc_iy -I"$ROOT/src/lib" \
  -o "$WORK/sprite-upload.o" "$ROOT/src/lib/zxn/sprite_upload.asm"
"/Users/blater/bin/z88dk/bin/z88dk-z80nm" -a "$WORK/sprite.o" \
  >"$WORK/sprite.nm"
"/Users/blater/bin/z88dk/bin/z88dk-z80nm" -a "$WORK/sprite-upload.o" \
  >"$WORK/sprite-upload.nm"
grep -q 'Section code_user: 162 bytes' "$WORK/sprite.nm"
grep -q 'Section bss_user: 128 bytes' "$WORK/sprite.nm"
grep -q 'Section "": 0 bytes' "$WORK/sprite.nm"
test "$(grep -c '^  Section ' "$WORK/sprite.nm")" -eq 3
grep -q '_rift_sprite_attr3_shadow (section bss_user)' "$WORK/sprite.nm"
if grep -q '_sprite_pattern_upload_banked' "$WORK/sprite.nm"; then
  echo 'FAIL: assetless Sprite presentation object retains the uploader' >&2
  exit 1
fi
if grep -Eq 'Sprite_new|rift_longlived|coordinates|frame_table|asset_table|format_table|descriptor|residen|dirty' \
    "$WORK/sprite.nm"; then
  echo 'FAIL: ZXN sprite object retains forbidden state or allocation symbols' >&2
  exit 1
fi
grep -q 'Section code_user: 92 bytes' "$WORK/sprite-upload.nm"
grep -q 'Section "": 0 bytes' "$WORK/sprite-upload.nm"
test "$(grep -c '^  Section ' "$WORK/sprite-upload.nm")" -eq 2
grep -q '_sprite_pattern_upload_banked (section code_user)' \
  "$WORK/sprite-upload.nm"

echo 'PASS: assetless ZXN Sprite is 162 code/128 BSS; optional uploader is 92 code/0 BSS'
