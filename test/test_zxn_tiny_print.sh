#!/bin/sh
# Execute the startup-31 lightweight console and verify screen behavior.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/rock-zxn-tiny-print.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
emulator=${ZESARUX_BIN:-/Users/blater/retro/retro1/zesarux/src/zesarux}

zcc +zxn -m -vn -subtype=nex -startup=31 -clib=sdcc_iy \
  --opt-code-size -create-app -DROCK_ZXN_TEST -DROCK_ZXN_TINY_CORE \
  -DROCK_ZXN_TINY_PRINT_DIRECT -DROCK_ZXN_TINY_PRINT_CONTROLS \
  -pragma-include:"$root/src/lib/zxn/zpragma_zxn.inc" \
  -I"$root/src/lib" -lm -o "$tmpdir/probe.exe" \
  "$root/test/fixtures/zxn_tiny_print_probe.c" \
  "$root/src/lib/print_bytes.c" "$root/src/lib/zxn_test.c"

"$root/tools/rock-emu" --target zxn --emulator-bin "$emulator" \
  --timeout-seconds 6 --artifacts "$tmpdir/artifacts" --require-emulator \
  test "$tmpdir/probe.nex"
