#!/bin/bash
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIXTURES="$ROOT/test/fixtures"
WORK="$(mktemp -d /tmp/rock-component-manifest.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

fail() {
  echo "component manifest test failed: $1" >&2
  exit 1
}

compile_with() {
  local target="$1" output="$2" manifest="$3"
  "$ROOT/rockc" "$FIXTURES/component_manifest_order.rkr" "$output" \
    "--target=$target" "--component-manifest=$manifest"
}

compile_with gcc "$WORK/host" "$FIXTURES/component_manifest_order.manifest"
compile_with zxn "$WORK/zxn" "$FIXTURES/component_manifest_order.manifest"

expected=$'ROCK_COMPONENTS_V1\n@profile=full\nalpha\nbeta\ngamma\ntop'
[ "$(cat "$WORK/host.components")" = "$expected" ] || fail "wrong dependency order"
cmp "$WORK/host.components" "$WORK/zxn.components" >/dev/null || \
  fail "host and ZXN closures differ"

lifecycle="$(grep -E '^(alpha|beta|gamma|top)_(init|shutdown)\(\);$' "$WORK/host.c")"
expected_lifecycle=$'alpha_init();\nbeta_init();\ngamma_init();\ntop_init();\ntop_shutdown();\ngamma_shutdown();\nbeta_shutdown();\nalpha_shutdown();'
[ "$lifecycle" = "$expected_lifecycle" ] || fail "wrong lifecycle order"
[ "$(grep -c '^alpha_init();$' "$WORK/host.c")" -eq 1 ] || fail "dependency initialized more than once"
grep -q '^goto rock_main_epilogue;$' "$WORK/host.c" || fail "early return bypasses epilogue"
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

"$ROOT/rockc" "$FIXTURES/zxn_size_empty.rock" "$WORK/missing-hook" \
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

for use in constructor_only method_only global; do
  "$ROOT/rockc" "$FIXTURES/opaque_sprite_${use}.rkr" "$WORK/$use" \
    "--component-manifest=$ROOT/src/lib/components.manifest"
  [ "$(cat "$WORK/$use.components")" = $'ROCK_COMPONENTS_V1\n@profile=full\ntiny_print\ntiny_test\ncore\nsprite' ] || \
    fail "$use use did not select Sprite exactly once"
done
init_line="$(grep -n '^sprite_component_init();$' "$WORK/global.c" | cut -d: -f1)"
new_line="$(grep -n '^player = Sprite_new();$' "$WORK/global.c" | cut -d: -f1)"
[ "$init_line" -lt "$new_line" ] || fail "component init does not precede deferred globals"

for fixture in cycle unknown duplicate_source unsafe_path bad_constructor duplicate_opaque unknown_owner; do
  if "$ROOT/rockc" "$FIXTURES/component_manifest_order.rkr" "$WORK/bad" \
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
grep -q 'method owner is not a declared opaque interface' "$WORK/unknown_owner.log" || \
  fail "unknown native method owner diagnostic missing"

(
  cd /tmp
  "$ROOT/rockc" "$ROOT/test/opaque_sprite_type_only_test.rkr" "$WORK/direct"
)
[ "$(cat "$WORK/direct.components")" = $'ROCK_COMPONENTS_V1\n@profile=full\ntiny_print\ntiny_test\ncore' ] || \
  fail "direct rockc default manifest depends on working directory"

"$ROOT/rockc" "$FIXTURES/native_name_shadow.rkr" "$WORK/shadow" \
  "--component-manifest=$ROOT/src/lib/components.manifest"
[ "$(cat "$WORK/shadow.components")" = $'ROCK_COMPONENTS_V1\n@profile=full\ntiny_print\ntiny_test\ncore' ] || \
  fail "user function shadow incorrectly selected a native component"
grep -q '^sleep(3);$' "$WORK/shadow.c" || fail "resolved user sleep call was lowered as native"
if grep -q '^rock_sleep(3);$' "$WORK/shadow.c"; then fail "native lowering ignored resolved target"; fi

"$ROOT/rockc" "$FIXTURES/opaque_sprite_aggregate_only.rkr" "$WORK/aggregate" \
  "--component-manifest=$ROOT/src/lib/components.manifest"
[ "$(cat "$WORK/aggregate.components")" = $'ROCK_COMPONENTS_V1\n@profile=full\ntiny_print\ntiny_test\ncore' ] || \
  fail "opaque aggregate type-only use selected implementation"
grep -q '^static void __rock_release_Sprite' "$WORK/aggregate.c" || \
  fail "opaque aggregate field did not emit generic release"

"$ROOT/rockc" "$FIXTURES/opaque_sprite_core_only.rkr" "$WORK/core-only" \
  "--component-manifest=$ROOT/src/lib/components.manifest"
[ "$(cat "$WORK/core-only.components")" = $'ROCK_COMPONENTS_V1\n@profile=full\ntiny_print\ntiny_test\ncore' ] || \
  fail "opaque return/copy/array-only ABI selected implementation"
grep -q '^__internal_dynamic_array_t Sprite_make_array' "$WORK/core-only.c" || \
  fail "opaque array-only ABI omitted typed helpers"

if "$ROOT/rockc" "$FIXTURES/zxn_size_hello.rock" "$WORK/no-tiny" \
    --target=zxn "--component-manifest=$FIXTURES/component_manifest_order.manifest" \
    >"$WORK/no-tiny.log" 2>&1; then
  fail "tiny profile accepted a manifest without tiny_print"
fi
grep -q "required component 'tiny_print'" "$WORK/no-tiny.log" || \
  fail "missing tiny component diagnostic absent"

"$ROOT/rockc" "$FIXTURES/native_get_shadow.rkr" "$WORK/get-shadow" \
  "--component-manifest=$ROOT/src/lib/components.manifest"
grep -q '__native_recv_' "$WORK/get-shadow.c" || \
  fail "shadowed get producer was misclassified as array borrower"

echo "component manifest tests passed"
