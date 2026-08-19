#!/bin/bash
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIXTURES="$ROOT/test/fixtures"
WORK="$(mktemp -d /tmp/rift-component-manifest.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

fail() {
  echo "component manifest test failed: $1" >&2
  exit 1
}

compile_with() {
  local target="$1" output="$2" manifest="$3"
  "$ROOT/riftc" "$FIXTURES/component_manifest_order.rift" "$output" \
    "--target=$target" "--component-manifest=$manifest"
}

compile_with gcc "$WORK/host" "$FIXTURES/component_manifest_order.manifest"
compile_with zxn "$WORK/zxn" "$FIXTURES/component_manifest_order.manifest"

expected=$'RIFT_COMPONENTS_V1\n@profile=full\nalpha\nbeta\ngamma\ntop'
[ "$(cat "$WORK/host.components")" = "$expected" ] || fail "wrong dependency order"
cmp "$WORK/host.components" "$WORK/zxn.components" >/dev/null || \
  fail "host and ZXN closures differ"

lifecycle="$(grep -E '^(alpha|beta|gamma|top)_(init|shutdown)\(\);$' "$WORK/host.c")"
expected_lifecycle=$'alpha_init();\nbeta_init();\ngamma_init();\ntop_init();\ntop_shutdown();\ngamma_shutdown();\nbeta_shutdown();\nalpha_shutdown();'
[ "$lifecycle" = "$expected_lifecycle" ] || fail "wrong lifecycle order"
[ "$(grep -c '^alpha_init();$' "$WORK/host.c")" -eq 1 ] || fail "dependency initialized more than once"
grep -q '^goto rift_main_epilogue;$' "$WORK/host.c" || fail "early return bypasses epilogue"
grep -q '^later_native();$' "$WORK/host.c" || fail "manifest C symbol was not emitted"
grep -q '__native_arg_' "$WORK/host.c" || fail "produced native string arg was not materialized"
grep -q '__strtmp_.*data = NULL' "$WORK/host.c" || fail "produced native string arg was not transferred"

gcc -Wall -Werror -Wno-unused-variable -Wno-implicit-function-declaration \
  -I"$FIXTURES" -I"$ROOT/src/lib" -I"$ROOT/src/ext/lib" -o "$WORK/lifecycle" "$WORK/host.c" \
  "$FIXTURES/component_lifecycle.c" "$ROOT/src/lib/pools.c" \
  "$ROOT/src/lib/print_bytes.c" "$ROOT/src/lib/fundefs.c" \
  "$ROOT/src/lib/error_sink.c" \
  "$ROOT/src/lib/fundefs_internal.c" "$ROOT/src/lib/asm_interop.c" \
  "$ROOT/src/lib/host_caps.c" "$ROOT/src/lib/zxn_test.c" \
  "$ROOT/src/lib/host/termbox2_impl.c" -lm
[ "$("$WORK/lifecycle")" = "PASS: component lifecycle order" ] || \
  fail "lifecycle did not execute exactly once in dependency/reverse order"

"$ROOT/riftc" "$FIXTURES/zxn_size_empty.rift" "$WORK/missing-hook" \
  "--component-manifest=$FIXTURES/component_manifest_unknown_hook.manifest"
if gcc -Wno-implicit-function-declaration -I"$ROOT/src/lib" -I"$ROOT/src/ext/lib" \
    -o "$WORK/missing-hook" "$WORK/missing-hook.c" \
    "$ROOT/src/lib/pools.c" "$ROOT/src/lib/print_bytes.c" \
    "$ROOT/src/lib/error_sink.c" \
    "$ROOT/src/lib/fundefs.c" "$ROOT/src/lib/fundefs_internal.c" \
    "$ROOT/src/lib/asm_interop.c" "$ROOT/src/lib/host_caps.c" \
    "$ROOT/src/lib/zxn_test.c" "$ROOT/src/lib/host/termbox2_impl.c" -lm \
    >"$WORK/missing-hook.log" 2>&1; then
  fail "unknown lifecycle hook linked successfully"
fi

"$ROOT/riftc" "$FIXTURES/sprite_type_methods.rift" "$WORK/sprite" \
  "--component-manifest=$ROOT/src/lib/components.manifest"
[ "$(cat "$WORK/sprite.components")" = $'RIFT_COMPONENTS_V1\n@profile=full\ntiny_print\ntiny_test\nerror_sink\ncore\nsprite' ] || \
  fail "Sprite type-method use did not select the component exactly once"
grep -q '^byte sprite = (byte)(3);$' "$WORK/sprite.c" || \
  fail "Sprite constructor did not lower to its byte representation"
grep -q '^rift__im_L6_Sprite_L8_position_A2(sprite, 12, 34);$' \
  "$WORK/sprite.c" || fail "Sprite.position did not lower to its native ABI"
grep -q '^rift__im_L6_Sprite_L4_show_A0(sprite);$' "$WORK/sprite.c" || \
  fail "Sprite.show did not lower to its native ABI"
grep -q '^rift__im_L6_Sprite_L4_hide_A0(sprite);$' "$WORK/sprite.c" || \
  fail "Sprite.hide did not lower to its native ABI"
grep -q '^rift__tm_L6_Sprite_L7_hideall_A0();$' "$WORK/sprite.c" || \
  fail "Sprite.hideall did not lower to its native ABI"
if grep -qE 'Sprite_new|sprite_component_(init|shutdown)' "$WORK/sprite.c"; then
  fail "Sprite value emitted a constructor function or lifecycle hook"
fi

for fixture in cycle unknown duplicate_source unsafe_path bad_constructor duplicate_opaque unknown_owner; do
  if "$ROOT/riftc" "$FIXTURES/component_manifest_order.rift" "$WORK/bad" \
      "--component-manifest=$FIXTURES/component_manifest_${fixture}.manifest" \
      >"$WORK/$fixture.log" 2>&1; then
    fail "invalid $fixture manifest was accepted"
  fi
done
grep -q 'dependency cycle' "$WORK/cycle.log" || fail "cycle diagnostic missing"
grep -q 'unknown component dependency' "$WORK/unknown.log" || fail "unknown dependency diagnostic missing"
grep -q 'multiple components' "$WORK/duplicate_source.log" || fail "duplicate source diagnostic missing"
grep -q 'unsafe component path' "$WORK/unsafe_path.log" || fail "unsafe path diagnostic missing"
grep -q 'opaque constructor must be named Owner_new' "$WORK/bad_constructor.log" || \
  fail "bad opaque constructor diagnostic missing"
grep -q 'duplicate opaque owner' "$WORK/duplicate_opaque.log" || \
  fail "duplicate opaque owner diagnostic missing"
grep -q 'method owner is not a declared opaque interface or namespace' "$WORK/unknown_owner.log" || \
  fail "unknown native method owner diagnostic missing"

(
  cd /tmp
  "$ROOT/riftc" "$FIXTURES/zxn_size_empty.rift" "$WORK/direct"
)
[ "$(cat "$WORK/direct.components")" = $'RIFT_COMPONENTS_V1\n@profile=full\ntiny_print\ntiny_test\nerror_sink\ncore' ] || \
  fail "direct riftc default manifest depends on working directory"

"$ROOT/riftc" "$FIXTURES/native_name_shadow.rift" "$WORK/shadow" \
  "--component-manifest=$ROOT/src/lib/components.manifest"
[ "$(cat "$WORK/shadow.components")" = $'RIFT_COMPONENTS_V1\n@profile=full\ntiny_print\ntiny_test\nerror_sink\ncore' ] || \
  fail "user function shadow incorrectly selected a native component"
grep -q '^sleep(3);$' "$WORK/shadow.c" || fail "resolved user sleep call was lowered as native"
if grep -q '^rift_sleep(3);$' "$WORK/shadow.c"; then fail "native lowering ignored resolved target"; fi
grep -q '^rift_user_print(1);$' "$WORK/shadow.c" || \
  fail "resolved user print call was lowered as a builtin"
grep -q '^rift_user_println(2);$' "$WORK/shadow.c" || \
  fail "resolved user println call was lowered as a builtin"
grep -q '^rift_user_concat(3, 4);$' "$WORK/shadow.c" || \
  fail "resolved user concat call was lowered as a builtin"
grep -q '^rift_user_input();$' "$WORK/shadow.c" || \
  fail "resolved user input call was lowered as a builtin"
grep -q '^rift_user_putchar(5);$' "$WORK/shadow.c" || \
  fail "resolved user putchar call was lowered as a builtin"

if "$ROOT/riftc" "$FIXTURES/printf_undefined.rift" "$WORK/printf-undefined" \
    "--component-manifest=$ROOT/src/lib/components.manifest" \
    >"$WORK/printf-undefined.log" 2>&1; then
  fail "removed printf builtin was still accepted"
fi
grep -q "undefined function printf" "$WORK/printf-undefined.log" || \
  fail "removed printf builtin did not report an undefined function"

"$ROOT/riftc" "$FIXTURES/zxn_beep_requires_crt.rift" "$WORK/beep" \
  --target=zxn "--component-manifest=$ROOT/src/lib/components.manifest"
grep -q '^@profile=full$' "$WORK/beep.components" || \
  fail "ROM BEEPER was incorrectly admitted to a startup-31 profile"

for target in gcc zxn; do
  "$ROOT/riftc" "$FIXTURES/clock_trig_components.rift" \
    "$WORK/clock-trig-$target" --target="$target" \
    "--component-manifest=$ROOT/src/lib/components.manifest"
  expected_clock_trig=$'RIFT_COMPONENTS_V1\n@profile=full\ntiny_print\ntiny_test\nerror_sink\ncore\nclock\ntrig'
  [ "$(cat "$WORK/clock-trig-$target.components")" = "$expected_clock_trig" ] || \
    fail "clock/trig selected the wrong $target component closure"
  grep -q 'rift_sin(phase)' "$WORK/clock-trig-$target.c" || \
    fail "fixed sin did not lower to its collision-safe C symbol"
  grep -q 'rift_cos(phase)' "$WORK/clock-trig-$target.c" || \
    fail "fixed cos did not lower to its collision-safe C symbol"
  grep -q 'rift_clock_ticks()' "$WORK/clock-trig-$target.c" || \
    fail "clock_ticks did not lower to its runtime symbol"
  if grep -q '^fmath$' "$WORK/clock-trig-$target.components"; then
    fail "fixed trig unnecessarily selected floating-point math"
  fi
done

"$ROOT/riftc" "$FIXTURES/zxn_graphics_core_profile.rift" "$WORK/graphics-core" \
  --target=zxn "--component-manifest=$ROOT/src/lib/components.manifest"
grep -q '^@profile=core-31$' "$WORK/graphics-core.components" || \
  fail "direct core graphics calls were incorrectly accepted by the tiny profile"
grep -q '^core$' "$WORK/graphics-core.components" || \
  fail "direct core graphics calls lost their core component"

"$ROOT/riftc" "$ROOT/test/input_ownership_test.rift" "$WORK/input-ownership" \
  "--component-manifest=$ROOT/src/lib/components.manifest"
[ "$(grep -c '= input();' "$WORK/input-ownership.c")" -eq 5 ] || \
  fail "input producer contexts were not each evaluated exactly once"
grep -q '__return_string(__strtmp_' "$WORK/input-ownership.c" || \
  fail "returned input producer was not retained across return cleanup"
grep -q '__string_release(__strtmp_' "$WORK/input-ownership.c" || \
  fail "input producer temporary was not tracked for release"
grep -q '__strtmp_.*\.data = NULL' "$WORK/input-ownership.c" || \
  fail "transferred input producer temporary was not nullified"

if "$ROOT/riftc" "$FIXTURES/zxn_size_hello.rift" "$WORK/no-tiny" \
    --target=zxn "--component-manifest=$FIXTURES/component_manifest_order.manifest" \
    >"$WORK/no-tiny.log" 2>&1; then
  fail "tiny profile accepted a manifest without tiny_print"
fi
grep -q "required component 'tiny_print'" "$WORK/no-tiny.log" || \
  fail "missing tiny component diagnostic absent"

"$ROOT/riftc" "$FIXTURES/native_get_shadow.rift" "$WORK/get-shadow" \
  "--component-manifest=$FIXTURES/component_manifest_order.manifest"
grep -q '__native_recv_' "$WORK/get-shadow.c" || \
  fail "shadowed get producer was misclassified as array borrower"

echo "component manifest tests passed"
