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
    "$ROOT/src/lib/fundefs.c" "$ROOT/src/lib/fundefs_internal.c" \
    "$ROOT/src/lib/asm_interop.c" "$ROOT/src/lib/host_caps.c" \
    "$ROOT/src/lib/zxn_test.c" "$ROOT/src/lib/host/termbox2_impl.c" -lm \
    >"$WORK/missing-hook.log" 2>&1; then
  fail "unknown lifecycle hook linked successfully"
fi

"$ROOT/riftc" "$FIXTURES/sprite_type_methods.rift" "$WORK/sprite" \
  "--component-manifest=$ROOT/src/lib/components.manifest"
[ "$(cat "$WORK/sprite.components")" = $'RIFT_COMPONENTS_V1\n@profile=full\ntiny_print\ntiny_test\ncore\nsprite' ] || \
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
[ "$(cat "$WORK/direct.components")" = $'RIFT_COMPONENTS_V1\n@profile=full\ntiny_print\ntiny_test\ncore' ] || \
  fail "direct riftc default manifest depends on working directory"

"$ROOT/riftc" "$FIXTURES/native_name_shadow.rift" "$WORK/shadow" \
  "--component-manifest=$ROOT/src/lib/components.manifest"
[ "$(cat "$WORK/shadow.components")" = $'RIFT_COMPONENTS_V1\n@profile=full\ntiny_print\ntiny_test\ncore' ] || \
  fail "user function shadow incorrectly selected a native component"
grep -q '^sleep(3);$' "$WORK/shadow.c" || fail "resolved user sleep call was lowered as native"
if grep -q '^rift_sleep(3);$' "$WORK/shadow.c"; then fail "native lowering ignored resolved target"; fi

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
