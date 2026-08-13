#!/bin/sh
# Verify the conservative ZXN tiny-core profile and configurable pool sizes.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FIXTURES="$ROOT/test/fixtures"
TMPDIR_SIZE=$(mktemp -d "${TMPDIR:-/tmp}/rock-zxn-size.XXXXXX")
trap 'rm -rf "$TMPDIR_SIZE"' EXIT HUP INT TERM

build_debug() {
  name=$1
  source=$2
  shift 2
  "$ROOT/rock" --debug --target=zxn "$@" \
    "$source" "$TMPDIR_SIZE/$name.exe" >"$TMPDIR_SIZE/$name.log" 2>&1
}

build_debug empty "$FIXTURES/zxn_size_empty.rock"
build_debug hello "$FIXTURES/zxn_size_hello.rock"
build_debug control "$FIXTURES/zxn_size_control.rock"
build_debug marker_collision "$FIXTURES/zxn_size_marker_collision.rock"
build_debug general "$FIXTURES/zxn_size_general_string.rock"
build_debug embedded "$FIXTURES/zxn_size_embedded_stdio.rock"
build_debug hello_all "$FIXTURES/zxn_size_hello.rock" --rtl=all
build_debug custom "$FIXTURES/zxn_size_general_string.rock" \
  --zxn-bump-pool=256 --zxn-longlived-pool=1024

grep -q 'ZXN profile: startup=31, .*tiny-core=1' "$TMPDIR_SIZE/empty.log"
grep -q 'ZXN profile: startup=31, .*tiny-core=1' "$TMPDIR_SIZE/hello.log"
grep -q 'ZXN profile: startup=31, .*tiny-core=1' "$TMPDIR_SIZE/control.log"
grep -q 'ZXN profile: startup=31, .*tiny-core=1' "$TMPDIR_SIZE/marker_collision.log"
grep -q 'ZXN profile: startup=31, .*tiny-core=0, light-core=1' "$TMPDIR_SIZE/general.log"
grep -q 'ZXN profile: startup=1, .*tiny-core=0, light-core=0' "$TMPDIR_SIZE/embedded.log"
grep -q 'ZXN profile: startup=1, .*tiny-core=0, light-core=0' "$TMPDIR_SIZE/hello_all.log"
if grep -q '^#define ROCK_ZXN_TINY_CORE' "$TMPDIR_SIZE/hello_all.exe.c"; then
  echo "FAIL: --rtl=all retained an active generated tiny-core macro" >&2
  exit 1
fi
grep -q 'rock_pools_init(ROCK_ZXN_BUMP_POOL_CAPACITY' "$TMPDIR_SIZE/hello_all.exe.c"
grep -q 'bump-pool=256, longlived-pool=1024' "$TMPDIR_SIZE/custom.log"

grep -q 'rock_print_bytes("hello world", 11)' "$TMPDIR_SIZE/hello.exe.c"
grep -q 'ROCK_PROFILE:ZXN_TINY_CORE:STARTUP=31' "$TMPDIR_SIZE/hello.exe.c"
grep -q -- '-DROCK_ZXN_TINY_PRINT_DIRECT' "$TMPDIR_SIZE/hello.log"
if grep -q -- '-DROCK_ZXN_TINY_PRINT_CONTROLS' "$TMPDIR_SIZE/hello.log"; then
  echo "FAIL: plain literal selected the larger control writer" >&2
  exit 1
fi
grep -q 'ROCK_PROFILE:ZXN_TINY_CORE:STARTUP=31:LIGHT_CONSOLE' \
  "$TMPDIR_SIZE/control.exe.c"
grep -q -- '-DROCK_ZXN_TINY_PRINT_DIRECT' "$TMPDIR_SIZE/control.log"
grep -q -- '-DROCK_ZXN_TINY_PRINT_CONTROLS' "$TMPDIR_SIZE/control.log"
grep -q 'ROCK_PROFILE:ZXN_LIGHT_CORE:STARTUP=31' "$TMPDIR_SIZE/general.exe.c"
grep -q -- '-DROCK_ZXN_LIGHT_CORE' "$TMPDIR_SIZE/general.log"
if grep -q -- '-DROCK_ZXN_LIGHT_CORE' "$TMPDIR_SIZE/embedded.log"; then
  echo "FAIL: embedded code selected the lightweight core" >&2
  exit 1
fi
if grep -q '__rock_make_string' "$TMPDIR_SIZE/hello.exe.c"; then
  echo "FAIL: literal print still constructs a Rock string" >&2
  exit 1
fi
grep -q '/lib/print_bytes.c' "$TMPDIR_SIZE/hello.log"
if grep -q '/lib/pools.c' "$TMPDIR_SIZE/hello.log"; then
  echo "FAIL: tiny literal print retained the pool runtime" >&2
  exit 1
fi
grep -q '/lib/pools.c' "$TMPDIR_SIZE/general.log"
grep -q '/lib/fundefs.c' "$TMPDIR_SIZE/general.log"

EMPTY_BYTES=$(wc -c <"$TMPDIR_SIZE/empty_CODE.bin" | tr -d '[:space:]')
HELLO_BYTES=$(wc -c <"$TMPDIR_SIZE/hello_CODE.bin" | tr -d '[:space:]')
CONTROL_BYTES=$(wc -c <"$TMPDIR_SIZE/control_CODE.bin" | tr -d '[:space:]')
GENERAL_BYTES=$(wc -c <"$TMPDIR_SIZE/general_CODE.bin" | tr -d '[:space:]')
CUSTOM_BYTES=$(wc -c <"$TMPDIR_SIZE/custom_CODE.bin" | tr -d '[:space:]')

if [ "$HELLO_BYTES" -ge "$GENERAL_BYTES" ]; then
  echo "FAIL: tiny hello ($HELLO_BYTES) is not smaller than full string build ($GENERAL_BYTES)" >&2
  exit 1
fi
if [ $((HELLO_BYTES - EMPTY_BYTES)) -gt 600 ]; then
  echo "FAIL: tiny literal print overhead exceeds 600 bytes ($HELLO_BYTES vs $EMPTY_BYTES)" >&2
  exit 1
fi
if [ $((CONTROL_BYTES - EMPTY_BYTES)) -gt 1400 ]; then
  echo "FAIL: lightweight console overhead exceeds 1400 bytes ($CONTROL_BYTES vs $EMPTY_BYTES)" >&2
  exit 1
fi
if [ $((GENERAL_BYTES - CUSTOM_BYTES)) -lt 5000 ]; then
  echo "FAIL: custom pools did not reclaim the expected static footprint" >&2
  exit 1
fi

if "$ROOT/rock" --target=zxn --zxn-bump-pool=invalid \
    "$FIXTURES/zxn_size_empty.rock" "$TMPDIR_SIZE/invalid.exe" \
    >"$TMPDIR_SIZE/invalid.log" 2>&1; then
  echo "FAIL: invalid pool capacity was accepted" >&2
  exit 1
fi
grep -q -- '--zxn-bump-pool requires a decimal byte count' "$TMPDIR_SIZE/invalid.log"

if "$ROOT/rock" --target=zxn --zxn-bump-pool=0016 \
    "$FIXTURES/zxn_size_empty.rock" "$TMPDIR_SIZE/leading-zero.exe" \
    >"$TMPDIR_SIZE/leading-zero.log" 2>&1; then
  echo "FAIL: leading-zero pool capacity was accepted" >&2
  exit 1
fi
grep -q 'canonical decimal values without leading zeros' "$TMPDIR_SIZE/leading-zero.log"

if "$ROOT/rock" --target=zxn --zxn-bump-pool=99999999999999999999 \
    "$FIXTURES/zxn_size_empty.rock" "$TMPDIR_SIZE/overlong.exe" \
    >"$TMPDIR_SIZE/overlong.log" 2>&1; then
  echo "FAIL: overlong pool capacity was accepted" >&2
  exit 1
fi
grep -q "fit the target's 16-bit pool offsets" "$TMPDIR_SIZE/overlong.log"

if "$ROOT/rock" --target=zxn --zxn-longlived-pool=5008 \
    "$FIXTURES/zxn_size_general_string.rock" "$TMPDIR_SIZE/invalid-roots.exe" \
    >"$TMPDIR_SIZE/invalid-roots.log" 2>&1; then
  echo "FAIL: unsupported buddy-root capacity was accepted" >&2
  exit 1
fi
grep -q 'at most two power-of-two buddy roots' "$TMPDIR_SIZE/invalid-roots.log"

if "$ROOT/rock" --target=zxn --zxn-longlived-pool=8192 \
    "$FIXTURES/zxn_size_general_string.rock" "$TMPDIR_SIZE/invalid-order.exe" \
    >"$TMPDIR_SIZE/invalid-order.log" 2>&1; then
  echo "FAIL: unsupported buddy order was accepted" >&2
  exit 1
fi
grep -q 'multiple of 16 in the range 16..6144' "$TMPDIR_SIZE/invalid-order.log"

"$ROOT/rock" --target=gcc "$FIXTURES/zxn_size_hello.rock" \
  "$TMPDIR_SIZE/hello-host" >"$TMPDIR_SIZE/host-build.log" 2>&1
HOST_OUTPUT=$($TMPDIR_SIZE/hello-host)
if [ "$HOST_OUTPUT" != "hello world" ]; then
  echo "FAIL: literal fast path changed host output: $HOST_OUTPUT" >&2
  exit 1
fi

echo "PASS: empty tiny-core resident size is $EMPTY_BYTES bytes"
echo "PASS: literal hello resident size is $HELLO_BYTES bytes"
echo "PASS: escaped literal resident size is $CONTROL_BYTES bytes with the lightweight console"
echo "PASS: dynamic-string lightweight core is $GENERAL_BYTES bytes; custom pools reduce it to $CUSTOM_BYTES bytes"
