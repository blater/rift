#!/bin/sh
set -eu

PROBE_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
EMULATOR=${ZESARUX_BIN:-/Users/blater/src/zesarux/src/zesarux}

cd "$PROBE_ROOT"
tools/test-zxn --emulator-bin "$EMULATOR" --timeout-seconds 20 \
  test/zxn-probes/palette_autoincrement_probe.rift \
  test/zxn-probes/bank_interrupt_restore_probe.rift \
  test/zxn-probes/sprite_attribute_index_observability_probe.rift
