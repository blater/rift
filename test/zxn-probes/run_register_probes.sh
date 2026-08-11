#!/bin/sh
set -eu

PROBE_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
EMULATOR=/Users/blater/retro/retro1/zesarux/src/zesarux

cd "$PROBE_ROOT"
tools/test-zxn --emulator-bin "$EMULATOR" --timeout-seconds 20 \
  test/zxn-probes/palette_autoincrement_probe.rkr \
  test/zxn-probes/bank_interrupt_restore_probe.rkr \
  test/zxn-probes/sprite_attribute_index_observability_probe.rkr
