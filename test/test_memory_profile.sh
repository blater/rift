#!/bin/sh
# Run ownership regressions with the exact 1 KiB bump / 6 KiB long-lived
# profile used by generated ZX Next programs. This is a host execution test:
# it makes constrained-pool exhaustion deterministic without an emulator.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/rift-memory-profile.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

for source in \
  test/nested_ownership_test.rift \
  test/aggregate_array_ownership_test.rift \
  test/string_array_ownership_test.rift \
  test/producer_ownership_test.rift \
  test/function_argument_ownership_test.rift \
  test/fluent_array_ownership_test.rift \
  test/module_ownership_test.rift \
  test/collect_test.rift \
  test/string_growth_test.rift \
  test/array_reassignment_soak_test.rift \
  test/mixed_memory_soak_test.rift
do
  name=$(basename "$source" .rift)
  output="$tmpdir/$name"
  "$root/rift" --target=gcc --memory-profile=zxn "$root/$source" "$output"
  "$output.exe"
done
