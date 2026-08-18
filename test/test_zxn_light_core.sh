#!/bin/sh
# Execute allocation, dynamic-string ownership, and Rift console output under
# the core-only startup-31 profile.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/rift-zxn-light-core.XXXXXX")
debug_dir=
trap 'rm -rf "$tmpdir"; [ -z "$debug_dir" ] || rm -rf "$debug_dir"' EXIT HUP INT TERM
emulator=${ZESARUX_BIN:-/Users/blater/src/zesarux/src/zesarux}

"$root/rift" --debug --target=zxn --zxn-test \
  "$root/test/fixtures/zxn_light_core_probe.rift" "$tmpdir/probe.exe" \
  >"$tmpdir/build.log" 2>&1

grep -q 'ZXN profile: startup=31, .*tiny-core=0, light-core=1' \
  "$tmpdir/build.log"
grep -q -- '-DRIFT_ZXN_LIGHT_CORE' "$tmpdir/build.log"
debug_dir=$(sed -n 's/^rift: debug workspace: //p' "$tmpdir/build.log" | head -1)
[ -n "$debug_dir" ] && [ -d "$debug_dir" ]

"$root/tools/rift-emu" --target zxn --emulator-bin "$emulator" \
  --timeout-seconds 6 --artifacts "$tmpdir/artifacts" --require-emulator \
  test "$tmpdir/probe.zxn"
