#!/bin/sh
# Run ownership regressions with the exact 1 KiB bump / 6 KiB long-lived
# profile used by generated ZX Next programs. This is a host execution test:
# it makes constrained-pool exhaustion deterministic without an emulator.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/rock-memory-profile.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

for source in \
  test/nested_ownership_test.rkr \
  test/aggregate_array_ownership_test.rkr \
  test/string_array_ownership_test.rkr \
  test/producer_ownership_test.rkr \
  test/function_argument_ownership_test.rkr \
  test/fluent_array_ownership_test.rkr \
  test/module_ownership_test.rkr \
  test/collect_test.rkr \
  test/array_reassignment_soak_test.rkr \
  test/mixed_memory_soak_test.rkr
do
  name=$(basename "$source" .rkr)
  output="$tmpdir/$name"
  "$root/rock" --memory-profile=zxn "$root/$source" "$output"
  "$output"
done
