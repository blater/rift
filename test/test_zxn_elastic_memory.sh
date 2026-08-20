#!/bin/sh
# Exercise automatic ZX Next arena sizing, shared bump/managed growth, and
# banked-asset coexistence on the canonical emulator.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rift-zxn-elastic.XXXXXX")
EMULATOR=${ZESARUX_BIN:-/Users/blater/src/zesarux/src/zesarux}
DEBUG_DIRS=
trap 'rm -rf "$WORK" $DEBUG_DIRS' EXIT HUP INT TERM

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

capture_debug_dir() {
  log=$1
  debug_dir=$(sed -n 's/^rift: debug workspace: //p' "$log" | head -1)
  [ -n "$debug_dir" ] && [ -d "$debug_dir" ] ||
    fail "build did not retain its debug workspace"
  DEBUG_DIRS="$DEBUG_DIRS $debug_dir"
}

run_nex() {
  artifact=$1
  name=$2
  "$ROOT/tools/rift-emu" --target zxn --emulator-bin "$EMULATOR" \
    --timeout-seconds 6 --artifacts "$WORK/$name-artifacts" \
    --require-emulator test "$artifact"
}

"$ROOT/rift" --debug --target=zxn --zxn-test \
  "$ROOT/test/fixtures/zxn_elastic_large_demand.rift" \
  "$WORK/large.exe" >"$WORK/large.log" 2>&1
capture_debug_dir "$WORK/large.log"
cp "$debug_dir/large.map" "$WORK/large.map"
cp "$debug_dir/large_CODE.bin" "$WORK/large_CODE.bin"

grep -q 'ZXN profile: startup=31, memory=auto, memory-max=auto, .*bump=omitted' \
  "$WORK/large.log" || fail "default build did not select automatic memory"
grep -q 'ZXN managed arena: .*0xF750' "$WORK/large.log" ||
  fail "build did not report the protected arena boundary"
grep -q '_rift_zxn_arena_link_start' "$WORK/large.map" ||
  fail "linker-gap start shim is absent"
if grep -qE 'zxn_(bump|longlived)_pool_storage' "$WORK/large.map"; then
  fail "target retained static pool backing"
fi
grep -Eq '^CRT_STACK_SIZE[[:space:]]*=[[:space:]]*\$0800\b' \
  "$WORK/large.map" || fail "link did not retain the 2 KiB stack contract"
grep -Eq '^CLIB_MALLOC_HEAP_SIZE[[:space:]]*=[[:space:]]*\$0000\b' \
  "$WORK/large.map" || fail "z88dk malloc heap was re-enabled"
ARENA_BYTES=$(awk '/ZXN managed arena:/ { value = $5; sub(/^\(/, "", value); print value; exit }' \
  "$WORK/large.log")
[ -n "$ARENA_BYTES" ] && [ "$ARENA_BYTES" -gt 16000 ] ||
  fail "large-demand fixture has only ${ARENA_BYTES:-unknown} arena bytes"
CODE_BYTES=$(wc -c <"$WORK/large_CODE.bin" | tr -d '[:space:]')
[ "$CODE_BYTES" -lt 18000 ] ||
  fail "elastic large-demand resident image is $CODE_BYTES bytes"
run_nex "$WORK/large.nex" large

# A cap larger than the former 6,144-byte ceiling remains valid and still
# serves the fixture's 7,000-byte array plus its separately managed handle.
"$ROOT/rift" --target=zxn --zxn-test --memory-max=16384 \
  "$ROOT/test/fixtures/zxn_elastic_large_demand.rift" \
  "$WORK/limited.exe" >"$WORK/limited.log" 2>&1
grep -q 'memory=auto, memory-max=16384, .*bump=omitted' \
  "$WORK/limited.log" || fail "larger expert cap was not preserved"
run_nex "$WORK/limited.nex" limited

if "$ROOT/rift" --target=zxn --memory-min=60000 \
    "$ROOT/test/fixtures/zxn_size_general_string.rift" \
    "$WORK/impossible-min.exe" >"$WORK/impossible-min.log" 2>&1; then
  fail "build accepted an unavailable managed-memory minimum"
fi
grep -q 'below --memory-min=60000' "$WORK/impossible-min.log" ||
  fail "unavailable managed-memory minimum lacked a headroom diagnostic"

# Embedded C selects startup 1. Keep the managed operation before the raw
# block (which is terminal syntax), then prove this profile initializes the
# same linker-gap arena and executes on target.
printf '%s\n' \
  'sub main() {' \
  '  string value := concat("startup", " one");' \
  '  zxn_test_pass();' \
  '  @embed c' \
  '    (void)printf;' \
  '  @end c' \
  '}' >"$WORK/startup1.rift"
"$ROOT/rift" --target=zxn --zxn-test "$WORK/startup1.rift" \
  "$WORK/startup1.exe" >"$WORK/startup1.log" 2>&1
grep -q 'ZXN profile: startup=1, memory=auto' "$WORK/startup1.log" ||
  fail "startup 1 did not select automatic managed memory"
run_nex "$WORK/startup1.nex" startup1

# Directly exercise two-ended sharing: 4 KiB of bump storage and an 8 KiB
# managed class coexist, are released, then a 16 KiB managed class grows into
# the recovered space.
(
  cd "$WORK"
  zcc +zxn -m -vn -subtype=nex -startup=31 -clib=sdcc_iy \
    --opt-code-size -create-app -DRIFT_ZXN_TEST -DRIFT_ALLOCATOR_TEST \
    -DRIFT_MEMORY_MAX_VALUE=23392 -DRIFT_MEMORY_MAX_PRESENT=1 \
    -DRIFT_MEMORY_RESERVE_VALUE=2048 -DRIFT_MEMORY_RESERVE_PRESENT=1 \
    -pragma-include:"$ROOT/src/lib/zxn/zpragma_zxn.inc" \
    -I"$ROOT/src/lib" -o arena-probe.exe \
    "$ROOT/test/fixtures/zxn_elastic_arena_probe.c" \
    "$ROOT/src/lib/zxn/arena_zxn.c" "$ROOT/src/lib/pools.c" \
    "$ROOT/src/lib/segregated_heap.c" \
    "$ROOT/src/lib/error_sink.c" \
    "$ROOT/src/lib/print_bytes.c" "$ROOT/src/lib/zxn_test.c" \
    "$ROOT/src/lib/zxn/arena_bounds.asm"
)
if grep -qE 'zxn_(bump|longlived)_pool_storage' "$WORK/arena-probe.map"; then
  fail "direct arena probe retained static pool backing"
fi
run_nex "$WORK/arena-probe.nex" arena-probe

# A compiler-proven no-bump closure has a permanent high limit. Prove that
# this distinct capability admits the exact whole-tail escape and contracts it
# before repeating, without exposing a limit-expansion API path.
(
  cd "$WORK"
  zcc +zxn -m -vn -subtype=nex -startup=31 -clib=sdcc_iy \
    --opt-code-size -create-app -DRIFT_ZXN_TEST -DRIFT_ALLOCATOR_TEST \
    -DRIFT_ZXN_NO_BUMP_POOL \
    -DRIFT_MEMORY_MAX_VALUE=23392 -DRIFT_MEMORY_MAX_PRESENT=1 \
    -DRIFT_MEMORY_RESERVE_VALUE=2048 -DRIFT_MEMORY_RESERVE_PRESENT=1 \
    -pragma-include:"$ROOT/src/lib/zxn/zpragma_zxn.inc" \
    -I"$ROOT/src/lib" -o arena-permanent-probe.exe \
    "$ROOT/test/fixtures/zxn_elastic_arena_probe.c" \
    "$ROOT/src/lib/zxn/arena_zxn.c" "$ROOT/src/lib/pools.c" \
    "$ROOT/src/lib/segregated_heap.c" \
    "$ROOT/src/lib/error_sink.c" \
    "$ROOT/src/lib/print_bytes.c" "$ROOT/src/lib/zxn_test.c" \
    "$ROOT/src/lib/zxn/arena_bounds.asm"
)
if grep -qE 'zxn_(bump|longlived)_pool_storage' \
    "$WORK/arena-permanent-probe.map"; then
  fail "permanent arena probe retained static pool backing"
fi
run_nex "$WORK/arena-permanent-probe.nex" arena-permanent-probe

# PAGE_24 is a banked physical asset section, not resident BSS. Exercise the
# uploader while a managed string is live to guard the arena/MMU contract.
dd if=/dev/zero of="$WORK/pattern.spr" bs=128 count=1 status=none
printf '%s\n' \
  'SpritePattern pattern := SpritePattern.load("pattern.spr");' \
  'sub main() {' \
  '  string value := concat("elastic", " arena");' \
  '  Sprite sprite := Sprite(0);' \
  '  sprite.frame(pattern, 0);' \
  '  zxn_test_pass();' \
  '}' >"$WORK/asset.rift"
"$ROOT/rift" --debug --target=zxn --zxn-test --memory-reserve=4096 \
  "$WORK/asset.rift" \
  "$WORK/asset.exe" >"$WORK/asset.log" 2>&1
capture_debug_dir "$WORK/asset.log"
cp "$debug_dir/asset.map" "$WORK/asset.map"
grep -q '^__PAGE_24_size' "$WORK/asset.map" ||
  fail "asset build omitted PAGE_24"
grep -q 'ZXN managed arena: .*0xE750.*high-memory reserve=4096 bytes' \
  "$WORK/asset.log" || fail "banked asset build lost its high-memory reserve"
if grep -qE 'zxn_(bump|longlived)_pool_storage' "$WORK/asset.map"; then
  fail "banked asset build restored static pools"
fi
run_nex "$WORK/asset.nex" asset

echo "PASS: automatic arena provides $ARENA_BYTES bytes with a $CODE_BYTES-byte resident image"
echo "PASS: managed demand above 6 KiB and two-ended grow/release/reuse execute on ZX Next"
echo "PASS: 2 KiB stack protection and PAGE_24 banked assets coexist with the arena"
