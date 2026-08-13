#!/bin/sh
# Execute allocation, dynamic-string ownership, and Rock console output under
# the core-only startup-31 profile.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/rock-zxn-light-core.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
emulator=${ZESARUX_BIN:-/Users/blater/retro/retro1/zesarux/src/zesarux}

"$root/rock" --debug --target=zxn --zxn-test \
  "$root/test/fixtures/zxn_light_core_probe.rkr" "$tmpdir/probe.exe" \
  >"$tmpdir/build.log" 2>&1

grep -q 'ZXN profile: startup=31, .*tiny-core=0, light-core=1' \
  "$tmpdir/build.log"
grep -q -- '-DROCK_ZXN_LIGHT_CORE' "$tmpdir/build.log"

"$root/tools/rock-emu" --target zxn --emulator-bin "$emulator" \
  --timeout-seconds 6 --artifacts "$tmpdir/artifacts" --require-emulator \
  test "$tmpdir/probe.nex"
