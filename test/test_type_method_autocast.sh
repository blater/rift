#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
output=/tmp/rock_type_method_autocast
shadow_output=/tmp/rock_type_method_global_shadow

"$root/rockc" "$root/test/type_method_autocast_test.rkr" "$output" --auto-cast

grep -Fq 'rock__tm_L5_Casts_L6_narrow_A1((byte)(1 + 2))' "$output.c"
grep -Fq 'Casts_instance_narrow(casts, (byte)(3 + 4))' "$output.c"

"$root/rockc" "$root/test/fixtures/type_method_global_shadow.rkr" \
  "$shadow_output"
grep -Fq 'FieldTool_kind(this->ParamTool)' "$shadow_output.c"

rm -f "$output.c" "$shadow_output.c"
echo "type-method generated-C invariants: pass"
