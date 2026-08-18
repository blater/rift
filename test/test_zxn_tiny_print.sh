#!/bin/sh
# Execute the startup-31 lightweight console and verify screen behavior.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/rift-zxn-tiny-print.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
emulator=${ZESARUX_BIN:-/Users/blater/src/zesarux/src/zesarux}

zcc +zxn -m -vn -subtype=nex -startup=31 -clib=sdcc_iy \
  --opt-code-size -create-app -DRIFT_ZXN_TEST -DRIFT_ZXN_TINY_CORE \
  -DRIFT_ZXN_TINY_PRINT_DIRECT -DRIFT_ZXN_TINY_PRINT_CONTROLS \
  -pragma-include:"$root/src/lib/zxn/zpragma_zxn.inc" \
  -I"$root/src/lib" -lm -o "$tmpdir/probe.exe" \
  "$root/test/fixtures/zxn_tiny_print_probe.c" \
  "$root/src/lib/print_bytes.c" "$root/src/lib/zxn_test.c"

"$root/tools/rift-emu" --target zxn --emulator-bin "$emulator" \
  --timeout-seconds 6 --artifacts "$tmpdir/artifacts" --require-emulator \
  test "$tmpdir/probe.nex"
