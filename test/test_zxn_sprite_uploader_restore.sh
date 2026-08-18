#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rift-zxn-sprite-upload.XXXXXX")
EMULATOR=${ZESARUX_BIN:-/Users/blater/src/zesarux/src/zesarux}
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

dd if=/dev/zero of="$WORK/page24.bin" bs=8192 count=1 status=none
dd if=/dev/zero of="$WORK/page25.bin" bs=8192 count=1 status=none
printf '%s\n' \
  'SECTION PAGE_24' \
  'PUBLIC _rift_assets_page_24_start' \
  'PUBLIC _rift_assets_page_24_end' \
  '_rift_assets_page_24_start:' \
  "  BINARY \"$WORK/page24.bin\"" \
  '_rift_assets_page_24_end:' \
  'SECTION PAGE_25' \
  'PUBLIC _rift_assets_page_25_start' \
  'PUBLIC _rift_assets_page_25_end' \
  '_rift_assets_page_25_start:' \
  "  BINARY \"$WORK/page25.bin\"" \
  '_rift_assets_page_25_end:' \
  >"$WORK/probe.exe.assets.asm"

zcc +zxn -m -vn -subtype=nex -startup=1 -clib=sdcc_iy -create-app \
  -pragma-include:"$ROOT/src/lib/zxn/zpragma_zxn.inc" \
  -I"$ROOT/src/lib" \
  -o "$WORK/restore.exe" \
  "$ROOT/test/zxn-probes/sprite_uploader_restore_probe.c" \
  "$ROOT/src/lib/zxn/sprite_upload.asm" \
  "$WORK/probe.exe.assets.asm"
grep -q '_sprite_pattern_upload_banked' "$WORK/restore.map"

"$ROOT/tools/rift-emu" test "$WORK/restore.nex" \
  --target zxn \
  --emulator-bin "$EMULATOR" \
  --require-emulator \
  --timeout-seconds 20 \
  --artifacts "$WORK/artifacts"

echo 'PASS: production uploader restored MMU1 and disabled/enabled IFF on pinned ZEsarUX'
