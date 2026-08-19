#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rift-semantic-plan.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

if grep -Eq 'ast\.h|generator/|name_table|internal\.h' \
    "$ROOT/src/plan_c_emitter/plan_c_emitter.h"; then
  fail "public plan emitter depends on AST or generator internals"
fi
grep -q 'const ownership_plan \*plan' \
  "$ROOT/src/plan_c_emitter/plan_c_emitter.h" ||
  fail "public plan emitter does not consume a const sealed plan"
grep -q 'const plan_c_abi \*abi' \
  "$ROOT/src/plan_c_emitter/plan_c_emitter.h" ||
  fail "public plan emitter does not consume a const ABI view"
grep -q 'selected body entered legacy lowering' \
  "$ROOT/src/generator/generator.c" ||
  fail "selected-body routing has no legacy-lowering poison guard"
if grep -R 'semantic-plan' "$ROOT/src/driver" >/dev/null 2>&1; then
  fail "internal semantic-plan gate leaked into the public rift driver"
fi

cat >"$WORK/forge.c" <<'EOF'
#include "ownership_plan/ownership_plan.h"
int main(void) {
  ownership_plan forged;
  return (int)sizeof(forged);
}
EOF
if gcc -Werror -Wall -Wextra -pedantic -I"$ROOT/src" \
    -c "$WORK/forge.c" -o "$WORK/forge.o" 2>"$WORK/forge.log"; then
  fail "public API allowed construction of an unsealed ownership plan"
fi

"$ROOT/riftc" "$ROOT/test/fixtures/semantic_plan_string_copy.rift" \
  "$WORK/host" --target=gcc --semantic-plan \
  --component-manifest="$ROOT/src/lib/components.manifest"

echo_body=$(sed -n '/^string rift_plan_fn_0(/,/^}$/p' "$WORK/host.c")
printf '%s\n' "$echo_body" | grep -q '__string_retain(rift_plan_slot_0);' ||
  fail "selected body was not emitted from the ownership plan"
if printf '%s\n' "$echo_body" | grep -Eq 'rift_bump|__bm_|__strtmp_|__rift_plan_t_'; then
  fail "selected body entered legacy body/expression lowering"
fi
copy_line=$(printf '%s\n' "$echo_body" | grep -n '__string_release(rift_plan_slot_1);' | cut -d: -f1)
value_line=$(printf '%s\n' "$echo_body" | grep -n '__string_release(rift_plan_slot_0);' | cut -d: -f1)
[ "$copy_line" -lt "$value_line" ] || fail "cleanup was not emitted in reverse order"

gcc -Werror -Wall -Wextra -Wno-sign-compare -pedantic \
  -I"$ROOT/src/lib" -I"$ROOT/src/ext/lib" \
  "$WORK/host.c" \
  "$ROOT/src/lib/arena_host.c" "$ROOT/src/lib/pools.c" \
  "$ROOT/src/lib/fundefs.c" "$ROOT/src/lib/termination.c" \
  "$ROOT/src/lib/print_bytes.c" "$ROOT/src/lib/error_sink.c" \
  "$ROOT/src/lib/host_caps.c" "$ROOT/src/lib/host/termbox2_impl.c" \
  -o "$WORK/host.exe"
[ "$("$WORK/host.exe")" = "hello" ] || fail "plan-emitted host function returned the wrong string"

if "$ROOT/riftc" "$ROOT/test/fixtures/semantic_plan_unsupported.rift" \
    "$WORK/unsupported" --target=gcc --semantic-plan \
    --component-manifest="$ROOT/src/lib/components.manifest" \
    >"$WORK/unsupported.log" 2>&1; then
  fail "unsupported selected AST fell back to legacy lowering"
fi
grep -q 'semantic-plan preflight failed' "$WORK/unsupported.log" ||
  fail "unsupported selected AST did not report preflight failure"
[ ! -s "$WORK/unsupported.c" ] ||
  fail "unsupported selected AST emitted partial C before failing"

for encoded_name in c_keyword asm_keyword temp_collision; do
  "$ROOT/riftc" \
    "$ROOT/test/fixtures/semantic_plan_${encoded_name}.rift" \
    "$WORK/$encoded_name" --target=gcc --semantic-plan \
    --component-manifest="$ROOT/src/lib/components.manifest"
  grep -q 'string rift_plan_slot_0' "$WORK/$encoded_name.c" ||
    fail "$encoded_name parameter was not emitted through its slot ID"
  if grep -Eq 'string (restrict|asm|__rift_plan_t_2)([,;) =])' \
      "$WORK/$encoded_name.c"; then
    fail "$encoded_name source slot leaked into generated C"
  fi
  gcc -Werror -Wall -Wextra -Wno-sign-compare -pedantic \
    -I"$ROOT/src/lib" -I"$ROOT/src/ext/lib" \
    -c "$WORK/$encoded_name.c" -o "$WORK/$encoded_name-host.o"

  "$ROOT/riftc" \
    "$ROOT/test/fixtures/semantic_plan_${encoded_name}.rift" \
    "$WORK/$encoded_name-zxn" --target=zxn --semantic-plan \
    --component-manifest="$ROOT/src/lib/components.manifest"
  zcc +zxn -vn -c -clib=sdcc_iy -I"$ROOT/src/lib" \
    "$WORK/$encoded_name-zxn.c" -o "$WORK/$encoded_name-zxn.o"
done

for routed_name in print_shadow overload; do
  "$ROOT/riftc" \
    "$ROOT/test/fixtures/semantic_plan_${routed_name}.rift" \
    "$WORK/$routed_name" --target=gcc --semantic-plan \
    --component-manifest="$ROOT/src/lib/components.manifest"
  grep -q '^string rift_plan_fn_0(' "$WORK/$routed_name.c" ||
    fail "$routed_name first function did not use its sealed symbol"
  if [ "$routed_name" = overload ]; then
    grep -q '^string rift_plan_fn_1(' "$WORK/$routed_name.c" ||
      fail "overloaded function did not receive a distinct sealed symbol"
  fi
  gcc -Werror -Wall -Wextra -Wno-sign-compare -pedantic \
    -I"$ROOT/src/lib" -I"$ROOT/src/ext/lib" \
    "$WORK/$routed_name.c" \
    "$ROOT/src/lib/arena_host.c" "$ROOT/src/lib/pools.c" \
    "$ROOT/src/lib/fundefs.c" "$ROOT/src/lib/termination.c" \
    "$ROOT/src/lib/print_bytes.c" "$ROOT/src/lib/error_sink.c" \
    "$ROOT/src/lib/host_caps.c" "$ROOT/src/lib/host/termbox2_impl.c" \
    -o "$WORK/$routed_name.exe"
  if [ "$routed_name" = print_shadow ]; then
    expected_output=shadow
  else
    expected_output=onetwo
  fi
  [ "$("$WORK/$routed_name.exe")" = "$expected_output" ] ||
    fail "$routed_name call did not route through its sealed symbol"

  "$ROOT/riftc" \
    "$ROOT/test/fixtures/semantic_plan_${routed_name}.rift" \
    "$WORK/$routed_name-zxn" --target=zxn --semantic-plan \
    --component-manifest="$ROOT/src/lib/components.manifest"
  zcc +zxn -vn -c -clib=sdcc_iy -I"$ROOT/src/lib" \
    "$WORK/$routed_name-zxn.c" -o "$WORK/$routed_name-zxn.o"
done

"$ROOT/riftc" "$ROOT/test/fixtures/semantic_plan_string_copy.rift" \
  "$WORK/zxn" --target=zxn --semantic-plan \
  --component-manifest="$ROOT/src/lib/components.manifest"
zcc +zxn -vn -c -clib=sdcc_iy -I"$ROOT/src/lib" \
  "$WORK/zxn.c" -o "$WORK/zxn.o"

echo "PASS: semantic-plan generated-C seam"
