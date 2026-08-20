#!/bin/sh
# Verify the stable managed identity store and exact compact ABI on ZX Next.
# The large probe owns behavior and the resident ceiling. The small pair owns
# linked attribution: both allocate one two-byte payload, access/write it, keep
# it alive across a scoped hold, access one static payload, and tear down. The
# raw side represents those lifetimes directly with pointers; the managed side
# adds precisely the retain/pin/static-reference facade being measured.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rift-zxn-managed-handles.XXXXXX")
EMULATOR=${ZESARUX_BIN:-/Users/blater/src/zesarux/src/zesarux}
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

(
  cd "$WORK"
  zcc +zxn -m -vn -subtype=nex -startup=31 -clib=sdcc_iy \
    --opt-code-size -create-app -DRIFT_ZXN_TEST -DRIFT_ZXN_NO_BUMP_POOL \
    -DRIFT_MEMORY_MAX_VALUE=23392 -DRIFT_MEMORY_MAX_PRESENT=1 \
    -DRIFT_MEMORY_RESERVE_VALUE=2048 -DRIFT_MEMORY_RESERVE_PRESENT=1 \
    -pragma-include:"$ROOT/src/lib/zxn/zpragma_zxn.inc" \
    -I"$ROOT/src/lib" -o managed-handles.exe \
    "$ROOT/test/fixtures/zxn_managed_handles_probe.c" \
    "$ROOT/src/lib/managed_heap.c" "$ROOT/src/lib/zxn/arena_zxn.c" \
    "$ROOT/src/lib/pools.c" "$ROOT/src/lib/segregated_heap.c" \
    "$ROOT/src/lib/error_sink.c" "$ROOT/src/lib/print_bytes.c" \
    "$ROOT/src/lib/zxn_test.c" "$ROOT/src/lib/zxn/arena_bounds.asm"

  zcc +zxn -m -vn -subtype=nex -startup=31 -clib=sdcc_iy \
    --opt-code-size -create-app -DRIFT_ZXN_TEST -DRIFT_ZXN_NO_BUMP_POOL \
    -DRIFT_MEMORY_MAX_VALUE=23392 -DRIFT_MEMORY_MAX_PRESENT=1 \
    -DRIFT_MEMORY_RESERVE_VALUE=2048 -DRIFT_MEMORY_RESERVE_PRESENT=1 \
    -pragma-include:"$ROOT/src/lib/zxn/zpragma_zxn.inc" \
    -I"$ROOT/src/lib" -o control.exe \
    "$ROOT/test/fixtures/zxn_managed_handles_control.c" \
    "$ROOT/src/lib/zxn/arena_zxn.c" "$ROOT/src/lib/pools.c" \
    "$ROOT/src/lib/segregated_heap.c" "$ROOT/src/lib/error_sink.c" \
    "$ROOT/src/lib/print_bytes.c" "$ROOT/src/lib/zxn_test.c" \
    "$ROOT/src/lib/zxn/arena_bounds.asm"

  zcc +zxn -m -vn -subtype=nex -startup=31 -clib=sdcc_iy \
    --opt-code-size -create-app -DRIFT_ZXN_TEST -DRIFT_ZXN_NO_BUMP_POOL \
    -DRIFT_MEMORY_MAX_VALUE=23392 -DRIFT_MEMORY_MAX_PRESENT=1 \
    -DRIFT_MEMORY_RESERVE_VALUE=2048 -DRIFT_MEMORY_RESERVE_PRESENT=1 \
    -pragma-include:"$ROOT/src/lib/zxn/zpragma_zxn.inc" \
    -I"$ROOT/src/lib" -o size-managed.exe \
    "$ROOT/test/fixtures/zxn_managed_handles_size_probe.c" \
    "$ROOT/src/lib/managed_heap.c" "$ROOT/src/lib/zxn/arena_zxn.c" \
    "$ROOT/src/lib/pools.c" "$ROOT/src/lib/segregated_heap.c" \
    "$ROOT/src/lib/error_sink.c" "$ROOT/src/lib/print_bytes.c" \
    "$ROOT/src/lib/zxn_test.c" "$ROOT/src/lib/zxn/arena_bounds.asm"

  zcc +zxn -m -vn -subtype=nex -startup=31 -clib=sdcc_iy \
    --opt-code-size -create-app -DRIFT_ZXN_TEST -DRIFT_ZXN_NO_BUMP_POOL \
    -DRIFT_MEMORY_MAX_VALUE=23392 -DRIFT_MEMORY_MAX_PRESENT=1 \
    -DRIFT_MEMORY_RESERVE_VALUE=2048 -DRIFT_MEMORY_RESERVE_PRESENT=1 \
    -pragma-include:"$ROOT/src/lib/zxn/zpragma_zxn.inc" \
    -I"$ROOT/src/lib" -o size-control.exe \
    "$ROOT/test/fixtures/zxn_managed_handles_size_control.c" \
    "$ROOT/src/lib/zxn/arena_zxn.c" "$ROOT/src/lib/pools.c" \
    "$ROOT/src/lib/segregated_heap.c" "$ROOT/src/lib/error_sink.c" \
    "$ROOT/src/lib/print_bytes.c" "$ROOT/src/lib/zxn_test.c" \
    "$ROOT/src/lib/zxn/arena_bounds.asm"
)

if grep -qE 'zxn_(bump|longlived)_pool_storage' "$WORK/managed-handles.map"; then
  fail "stable identity probe retained fixed pool storage"
fi
grep -q '_managed_heap_alloc' "$WORK/managed-handles.map" ||
  fail "stable identity store was not linked"
grep -q '_managed_heap_pin_typed' "$WORK/managed-handles.map" ||
  fail "pin facade was not linked"

CODE_BYTES=$(wc -c <"$WORK/managed-handles_CODE.bin" | tr -d '[:space:]')
SIZE_MANAGED_BYTES=$(wc -c <"$WORK/size-managed_CODE.bin" | tr -d '[:space:]')
CONTROL_BYTES=$(wc -c <"$WORK/size-control_CODE.bin" | tr -d '[:space:]')
MODULE_DELTA=$((SIZE_MANAGED_BYTES - CONTROL_BYTES))
[ "$CODE_BYTES" -lt 22000 ] ||
  fail "stable identity behavioral probe is $CODE_BYTES resident bytes"
[ "$MODULE_DELTA" -le 3500 ] ||
  fail "stable identity checkpoint adds $MODULE_DELTA resident bytes (budget 3500)"

"$ROOT/tools/rift-emu" --target zxn --emulator-bin "$EMULATOR" \
  --timeout-seconds 8 --artifacts "$WORK/artifacts" --require-emulator \
  test "$WORK/managed-handles.nex"

echo "PASS: stable identity/access/pin probe executes in $CODE_BYTES resident bytes"
echo "PASS: stable identity linked delta is $MODULE_DELTA bytes over the $CONTROL_BYTES-byte raw control"
