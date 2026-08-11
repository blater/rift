#!/bin/sh
set -eu

PROBE_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PROBE_DIR="$PROBE_ROOT/test/zxn-probes"
PROBE_TMP=$(mktemp -d "${TMPDIR:-/tmp}/rock-nex-pages.XXXXXX")
trap 'rm -rf "$PROBE_TMP"' EXIT HUP INT TERM

zcc +zxn -m -vn -subtype=nex -startup=1 -clib=sdcc_iy -create-app \
  -pragma-include:"$PROBE_ROOT/src/lib/zxn/zpragma_zxn.inc" \
  -o "$PROBE_TMP/probe.exe" \
  "$PROBE_DIR/nex_page_packaging_probe.c" \
  "$PROBE_DIR/nex_page_packaging_probe.asm"

perl "$PROBE_DIR/inspect_nex_pages.pl" \
  "$PROBE_TMP/probe.nex" "$PROBE_TMP/probe.map"
shasum -a 256 "$PROBE_TMP/probe.nex"

(
  cd "$PROBE_ROOT"
  ./tools/rock-emu test "$PROBE_TMP/probe.nex" \
    --target zxn \
    --emulator-bin /Users/blater/retro/retro1/zesarux/src/zesarux \
    --require-emulator \
    --keep-all \
    --timeout-seconds 20 \
    --artifacts "$PROBE_ROOT/test-artifacts/zxn/nex-page-packaging-probe"
)

zcc +zxn -m -vn -subtype=nex -startup=1 -clib=sdcc_iy -create-app \
  -DROCK_PROBE_EXPANDED \
  -pragma-include:"$PROBE_ROOT/src/lib/zxn/zpragma_zxn.inc" \
  -o "$PROBE_TMP/expanded.exe" \
  "$PROBE_DIR/nex_page_packaging_probe.c" \
  "$PROBE_DIR/nex_page_packaging_probe.asm" \
  "$PROBE_DIR/nex_page_expanded_probe.asm"

perl "$PROBE_DIR/inspect_nex_pages.pl" \
  "$PROBE_TMP/expanded.nex" "$PROBE_TMP/expanded.map" expanded
shasum -a 256 "$PROBE_TMP/expanded.nex"

(
  cd "$PROBE_ROOT"
  ./tools/rock-emu test "$PROBE_TMP/expanded.nex" \
    --target zxn \
    --emulator-bin /Users/blater/retro/retro1/zesarux/src/zesarux \
    --require-emulator \
    --keep-all \
    --timeout-seconds 20 \
    --artifacts "$PROBE_ROOT/test-artifacts/zxn/nex-page-packaging-probe"
)
