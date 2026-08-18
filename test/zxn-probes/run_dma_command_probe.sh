#!/bin/sh
set -eu

PROBE_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PROBE_DIR="$PROBE_ROOT/test/zxn-probes"
PROBE_TMP=$(mktemp -d "${TMPDIR:-/tmp}/rift-dma-probe.XXXXXX")
EMULATOR=${ZESARUX_BIN:-/Users/blater/src/zesarux/src/zesarux}
trap 'rm -rf "$PROBE_TMP"' EXIT HUP INT TERM

zcc +zxn -m -vn -subtype=nex -startup=1 -clib=sdcc_iy -create-app \
  -pragma-include:"$PROBE_ROOT/src/lib/zxn/zpragma_zxn.inc" \
  -o "$PROBE_TMP/probe.exe" \
  "$PROBE_DIR/dma_command_probe.c" \
  "$PROBE_DIR/dma_interrupt_probe.asm"

shasum -a 256 "$PROBE_TMP/probe.nex"
(
  cd "$PROBE_ROOT"
  ./tools/rift-emu test "$PROBE_TMP/probe.nex" \
    --target zxn \
    --emulator-bin "$EMULATOR" \
    --require-emulator \
    --keep-all \
    --timeout-seconds 20 \
    --artifacts "$PROBE_ROOT/test-artifacts/zxn/dma-command-probe"
)
