#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rift-fixed-array-ownership.XXXXXX")
debug_dir=
trap 'rm -rf "$WORK"; [ -z "$debug_dir" ] || rm -rf "$debug_dir"' EXIT HUP INT TERM

"$ROOT/rift" --debug --target=gcc \
  "$ROOT/test/fixed_aggregate_array_poison_test.rift" "$WORK/test.exe" \
  >"$WORK/build.log" 2>&1
"$WORK/test.exe" >"$WORK/run.log"
test "$(grep -c '^PASS:' "$WORK/run.log")" -eq 4
debug_dir=$(sed -n 's/^rift: debug workspace: //p' "$WORK/build.log" | head -1)
[ -n "$debug_dir" ] && [ -d "$debug_dir" ]

sed -n '/^void OwnedItem_set_elem/,/^}/p' "$debug_dir/test.exe.c" \
  >"$WORK/setter.c"
grep -q 'if (index < arr->length)' "$WORK/setter.c"
grep -q '__rift_release_OwnedItem(\*old);' "$WORK/setter.c"
grep -q '__internal_set_elem(arr, index, &elem);' "$WORK/setter.c"

echo 'PASS: generated owning setter guards release and survives poisoned capacity'
