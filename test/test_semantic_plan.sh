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

echo_body=$(awk '/^string rift_plan_body_0\(/ { seen++ }
                 seen == 2 { print }
                 seen == 2 && /^}$/ { exit }' "$WORK/host.c")
printf '%s\n' "$echo_body" | grep -q 'string rift_plan_slot_1 =' ||
  fail "selected body was not emitted from the ownership plan"
if printf '%s\n' "$echo_body" | grep -q '__string_retain(rift_plan_slot_0);'; then
  fail "owned-ABI adapter argument was retained again in the selected body"
fi
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

for rejected_call in native_call nonreturning_call unknown_call; do
  if "$ROOT/riftc" \
      "$ROOT/test/fixtures/semantic_plan_${rejected_call}_unsupported.rift" \
      "$WORK/$rejected_call" --target=gcc --semantic-plan \
      --component-manifest="$ROOT/src/lib/components.manifest" \
      >"$WORK/$rejected_call.log" 2>&1; then
    fail "$rejected_call unexpectedly entered the selected call ABI"
  fi
  [ ! -s "$WORK/$rejected_call.c" ] ||
    fail "$rejected_call emitted partial C instead of failing closed"
done

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

"$ROOT/riftc" "$ROOT/test/fixtures/semantic_plan_string_calls.rift" \
  "$WORK/calls" --target=gcc --semantic-plan \
  --component-manifest="$ROOT/src/lib/components.manifest"
calls_body=$(awk '/^string rift_plan_body_2\(/ { seen++ }
                  seen == 2 { print }
                  seen == 2 && /^}$/ { exit }' "$WORK/calls.c")
borrow_hold=$(printf '%s\n' "$calls_body" | grep -n '__string_retain(rift_plan_tmp_5);' | cut -d: -f1)
later_call=$(printf '%s\n' "$calls_body" | grep -n '= rift_plan_body_0(rift_plan_tmp_7);' | cut -d: -f1)
outer_call=$(printf '%s\n' "$calls_body" | grep -n '= rift_plan_body_1(rift_plan_tmp_5, rift_plan_tmp_9);' | cut -d: -f1)
[ "$borrow_hold" -lt "$later_call" ] && [ "$later_call" -lt "$outer_call" ] ||
  fail "borrowed argument was not held before evaluating the later producer"
producer_call=$(printf '%s\n' "$calls_body" | grep -n '= rift_plan_body_0(rift_plan_tmp_12);' | cut -d: -f1)
producer_move=$(printf '%s\n' "$calls_body" | grep -n 'rift_plan_tmp_14 = rift_plan_tmp_13;' | cut -d: -f1)
later_hold=$(printf '%s\n' "$calls_body" | grep -n '__string_retain(rift_plan_tmp_16);' | cut -d: -f1)
[ "$producer_call" -lt "$producer_move" ] && [ "$producer_move" -lt "$later_hold" ] ||
  fail "produced argument was not moved before evaluating the later borrower"
discard_call=$(printf '%s\n' "$calls_body" | grep -n 'rift_plan_tmp_20 = rift_plan_body_0' | cut -d: -f1)
discard_release=$(printf '%s\n' "$calls_body" | grep -n '__string_release(rift_plan_tmp_20);' | cut -d: -f1)
[ "$discard_release" -eq $((discard_call + 1)) ] ||
  fail "discarded produced call result was not released at expression end"
printf '%s\n' "$calls_body" | grep -q 'return rift_plan_tmp_23;' ||
  fail "produced call result was not transferred through return"
if printf '%s\n' "$calls_body" | grep -q 'rift_plan_fn_'; then
  fail "selected-to-selected call routed through the legacy ABI adapter"
fi
adapter=$(awk '/^string rift_plan_fn_3\(/ { seen++ }
               seen == 2 { print }
               seen == 2 && /^}$/ { exit }' "$WORK/calls.c")
printf '%s\n' "$adapter" | grep -q 'rift_plan_body_3' ||
  fail "legacy caller adapter did not forward to the selected body"
if printf '%s\n' "$adapter" | grep -q '__string_retain'; then
  fail "owned-ABI adapter retained an already-owned argument"
fi
legacy_main=$(sed -n '/^int main(/,$p' "$WORK/calls.c")
if [ "$(printf '%s\n' "$legacy_main" | grep -c 'string rift_plan_legacy_arg_.* = __strtmp_')" -ne 2 ] ||
   [ "$(printf '%s\n' "$legacy_main" | grep -c 'rift_plan_fn_3(rift_plan_legacy_arg_')" -ne 2 ]; then
  fail "legacy concat/substring producer arguments were not evaluated once"
fi
printf '%s\n' "$legacy_main" | awk '
  /string rift_plan_legacy_arg_.* = __strtmp_/ { state = 1; next }
  state == 1 && /__strtmp_.*\.data = NULL/ { state = 2; next }
  state == 2 && /rift_plan_fn_3\(rift_plan_legacy_arg_/ { transfers++; state = 0 }
  END { exit transfers == 2 ? 0 : 1 }
' || fail "legacy producer arguments were not captured then transferred"
gcc -Werror -Wall -Wextra -Wno-sign-compare -pedantic \
  -I"$ROOT/src/lib" -I"$ROOT/src/ext/lib" \
  "$WORK/calls.c" \
  "$ROOT/src/lib/arena_host.c" "$ROOT/src/lib/pools.c" \
  "$ROOT/src/lib/fundefs.c" "$ROOT/src/lib/termination.c" \
  "$ROOT/src/lib/print_bytes.c" "$ROOT/src/lib/error_sink.c" \
  "$ROOT/src/lib/host_caps.c" "$ROOT/src/lib/host/termbox2_impl.c" \
  -o "$WORK/calls.exe"
[ "$("$WORK/calls.exe")" = "left" ] ||
  fail "managed selected-call program returned the wrong string"
sed 's/rift_pools_init(NULL);/rift_arena_options rift_test_arena = { .memory_max = 1024, .memory_max_present = 1 }; rift_pools_init(\&rift_test_arena);/' \
  "$WORK/calls.c" >"$WORK/calls-constrained.c"
gcc -Werror -Wall -Wextra -Wno-sign-compare -pedantic \
  -I"$ROOT/src/lib" -I"$ROOT/src/ext/lib" \
  "$WORK/calls-constrained.c" \
  "$ROOT/src/lib/arena_host.c" "$ROOT/src/lib/pools.c" \
  "$ROOT/src/lib/fundefs.c" "$ROOT/src/lib/termination.c" \
  "$ROOT/src/lib/print_bytes.c" "$ROOT/src/lib/error_sink.c" \
  "$ROOT/src/lib/host_caps.c" "$ROOT/src/lib/host/termbox2_impl.c" \
  -o "$WORK/calls-constrained.exe"
[ "$("$WORK/calls-constrained.exe")" = "left" ] ||
  fail "managed selected-call program did not reuse a 1 KiB arena"
"$ROOT/riftc" "$ROOT/test/fixtures/semantic_plan_string_calls.rift" \
  "$WORK/calls-zxn" --target=zxn --semantic-plan \
  --component-manifest="$ROOT/src/lib/components.manifest"
zcc +zxn -vn -c -clib=sdcc_iy -I"$ROOT/src/lib" \
  "$WORK/calls-zxn.c" -o "$WORK/calls-zxn.o"

"$ROOT/riftc" "$ROOT/test/fixtures/semantic_plan_if_assignment.rift" \
  "$WORK/branches" --target=gcc --semantic-plan \
  --component-manifest="$ROOT/src/lib/components.manifest"
grep -q '^string rift_plan_body_1(int rift_plan_slot_0, string rift_plan_slot_1)' \
  "$WORK/branches.c" || fail "bool condition was not sealed into the selected ABI"
branch_body=$(awk '/^string rift_plan_body_1\(/ { seen++ }
                   seen == 2 { print }
                   seen == 2 && /^}$/ { exit }' "$WORK/branches.c")
printf '%s\n' "$branch_body" | grep -q \
  'if (rift_plan_tmp_.*) goto rift_plan_block_1; else goto rift_plan_block_2;' ||
  fail "sealed bool condition did not emit an explicit two-way branch"
printf '%s\n' "$branch_body" | awk '
  /= rift_plan_body_0/ { call = NR }
  /__string_release\(rift_plan_slot_2\)/ { if (call && call < NR) ordered++ }
  END { exit ordered >= 2 ? 0 : 1 }
' || fail "assignment released the old owner before fully evaluating its RHS"
printf '%s\n' "$branch_body" | awk '
  /__string_retain\(rift_plan_tmp_/ { held = NR }
  /= rift_plan_body_0/ { if (held && held < NR) held_call++ }
  END { exit held_call >= 2 ? 0 : 1 }
' || fail "borrowed assignment input was not held across an effectful RHS"
printf '%s\n' "$branch_body" | awk '
  /__string_release\(rift_plan_slot_3\)/ {
    if (getline > 0 && /goto rift_plan_block_3/) clean++
  }
  END { exit clean == 1 ? 0 : 1 }
' || fail "branch-local produced string was not released before its join"
maybe_body=$(awk '/^string rift_plan_body_2\(/ { seen++ }
                  seen == 2 { print }
                  seen == 2 && /^}$/ { exit }' "$WORK/branches.c")
printf '%s\n' "$maybe_body" | grep -q \
  'rift_plan_block_2: ;' || fail "if-without-else has no false path"
printf '%s\n' "$maybe_body" | awk '
  /rift_plan_block_2:/ { false_block = NR }
  /goto rift_plan_block_3/ { if (false_block && false_block < NR) found++ }
  END { exit found == 1 ? 0 : 1 }
' || fail "if-without-else false path does not reach the ownership join"
early_body=$(awk '/^string rift_plan_body_3\(/ { seen++ }
                  seen == 2 { print }
                  seen == 2 && /^}$/ { exit }' "$WORK/branches.c")
[ "$(printf '%s\n' "$early_body" | grep -c '^return rift_plan_tmp_')" -eq 2 ] ||
  fail "both conditional return paths were not emitted"
exercise_body=$(awk '/^string rift_plan_body_4\(/ { seen++ }
                     seen == 2 { print }
                     seen == 2 && /^}$/ { exit }' "$WORK/branches.c")
printf '%s\n' "$exercise_body" | grep -q 'int rift_plan_tmp_.* = 1;' ||
  fail "true bool input was not evaluated in selected code"
printf '%s\n' "$exercise_body" | grep -q 'int rift_plan_tmp_.* = 0;' ||
  fail "false bool input was not evaluated in selected code"
gcc -Werror -Wall -Wextra -Wno-sign-compare -pedantic \
  -I"$ROOT/src/lib" -I"$ROOT/src/ext/lib" \
  "$WORK/branches.c" \
  "$ROOT/src/lib/arena_host.c" "$ROOT/src/lib/pools.c" \
  "$ROOT/src/lib/fundefs.c" "$ROOT/src/lib/termination.c" \
  "$ROOT/src/lib/print_bytes.c" "$ROOT/src/lib/error_sink.c" \
  "$ROOT/src/lib/host_caps.c" "$ROOT/src/lib/host/termbox2_impl.c" \
  -o "$WORK/branches.exe"
[ "$("$WORK/branches.exe")" = "branch" ] ||
  fail "branching plan-emitted functions returned the wrong string"
sed 's/rift_pools_init(NULL);/rift_arena_options rift_test_arena = { .memory_max = 1024, .memory_max_present = 1 }; rift_pools_init(\&rift_test_arena);/' \
  "$WORK/branches.c" >"$WORK/branches-constrained.c"
gcc -Werror -Wall -Wextra -Wno-sign-compare -pedantic \
  -I"$ROOT/src/lib" -I"$ROOT/src/ext/lib" \
  "$WORK/branches-constrained.c" \
  "$ROOT/src/lib/arena_host.c" "$ROOT/src/lib/pools.c" \
  "$ROOT/src/lib/fundefs.c" "$ROOT/src/lib/termination.c" \
  "$ROOT/src/lib/print_bytes.c" "$ROOT/src/lib/error_sink.c" \
  "$ROOT/src/lib/host_caps.c" "$ROOT/src/lib/host/termbox2_impl.c" \
  -o "$WORK/branches-constrained.exe"
[ "$("$WORK/branches-constrained.exe")" = "branch" ] ||
  fail "branching ownership plan did not reuse a 1 KiB arena"
"$ROOT/riftc" "$ROOT/test/fixtures/semantic_plan_if_assignment.rift" \
  "$WORK/branches-zxn" --target=zxn --semantic-plan \
  --component-manifest="$ROOT/src/lib/components.manifest"
zcc +zxn -vn -c -clib=sdcc_iy -I"$ROOT/src/lib" \
  "$WORK/branches-zxn.c" -o "$WORK/branches-zxn.o"

"$ROOT/riftc" "$ROOT/test/fixtures/semantic_plan_string_intrinsics.rift" \
  "$WORK/intrinsics" --target=gcc --semantic-plan \
  --component-manifest="$ROOT/src/lib/components.manifest"
intrinsic_body=$(awk '/^string rift_plan_body_1\(/ { seen++ }
                      seen == 2 { print }
                      seen == 2 && /^}$/ { exit }' "$WORK/intrinsics.c")
[ "$(printf '%s\n' "$intrinsic_body" | grep -c '__concat_str(&')" -eq 6 ] ||
  fail "concat calls did not route through the sealed intrinsic ABI"
printf '%s\n' "$intrinsic_body" | grep -q '__substring_from(&' ||
  fail "two-argument substring did not route through its sealed runtime ABI"
printf '%s\n' "$intrinsic_body" | grep -q '__substring_range(&' ||
  fail "three-argument substring did not route through its sealed runtime ABI"
printf '%s\n' "$intrinsic_body" | awk '
  /__concat_str\(&/ {
    line = $0
    sub(/^.*__concat_str\(&[^,]+, /, "", line)
    split(line, args, /, |\);/)
    first = args[1]
    second = args[2]
    if (first == second) exit 1
    if (getline <= 0 || index($0, "__string_release(" second ")") == 0) exit 1
    if (getline <= 0 || index($0, "__string_release(" first ")") == 0) exit 1
    checked++
  }
  END { exit checked == 6 ? 0 : 1 }
' || fail "intrinsic operands were not held distinctly and released in reverse"
printf '%s\n' "$intrinsic_body" | awk '
  /__concat_str\(&/ { calls++ }
  calls == 4 && /__string_release\(rift_plan_tmp_/ { released_after_outer = 1 }
  END { exit released_after_outer ? 0 : 1 }
' || fail "a produced operand was not released after its consuming intrinsic"
printf '%s\n' "$intrinsic_body" | awk '
  /__concat_str\(&/ { result = $0; sub(/^.*&/, "", result); sub(/,.*/, "", result) }
  result && index($0, "__string_release(" result ")") { discarded++; result = "" }
  END { exit discarded >= 1 ? 0 : 1 }
' || fail "discarded intrinsic result was not released at expression end"
gcc -Werror -Wall -Wextra -Wno-sign-compare -pedantic \
  -I"$ROOT/src/lib" -I"$ROOT/src/ext/lib" \
  "$WORK/intrinsics.c" \
  "$ROOT/src/lib/arena_host.c" "$ROOT/src/lib/pools.c" \
  "$ROOT/src/lib/fundefs.c" "$ROOT/src/lib/termination.c" \
  "$ROOT/src/lib/print_bytes.c" "$ROOT/src/lib/error_sink.c" \
  "$ROOT/src/lib/host_caps.c" "$ROOT/src/lib/host/termbox2_impl.c" \
  -o "$WORK/intrinsics.exe"
[ "$("$WORK/intrinsics.exe")" = "bab" ] ||
  fail "plan-emitted string intrinsics returned the wrong value"
sed 's/rift_pools_init(NULL);/rift_arena_options rift_test_arena = { .memory_max = 1024, .memory_max_present = 1 }; rift_pools_init(\&rift_test_arena);/' \
  "$WORK/intrinsics.c" >"$WORK/intrinsics-constrained.c"
gcc -Werror -Wall -Wextra -Wno-sign-compare -pedantic \
  -I"$ROOT/src/lib" -I"$ROOT/src/ext/lib" \
  "$WORK/intrinsics-constrained.c" \
  "$ROOT/src/lib/arena_host.c" "$ROOT/src/lib/pools.c" \
  "$ROOT/src/lib/fundefs.c" "$ROOT/src/lib/termination.c" \
  "$ROOT/src/lib/print_bytes.c" "$ROOT/src/lib/error_sink.c" \
  "$ROOT/src/lib/host_caps.c" "$ROOT/src/lib/host/termbox2_impl.c" \
  -o "$WORK/intrinsics-constrained.exe"
[ "$("$WORK/intrinsics-constrained.exe")" = "bab" ] ||
  fail "string intrinsics did not reuse a 1 KiB arena across 200 iterations"
"$ROOT/riftc" "$ROOT/test/fixtures/semantic_plan_string_intrinsics.rift" \
  "$WORK/intrinsics-zxn" --target=zxn --semantic-plan \
  --component-manifest="$ROOT/src/lib/components.manifest"
zcc +zxn -vn -c -clib=sdcc_iy -I"$ROOT/src/lib" \
  "$WORK/intrinsics-zxn.c" -o "$WORK/intrinsics-zxn.o"

"$ROOT/riftc" "$ROOT/test/fixtures/semantic_plan_intrinsic_shadow.rift" \
  "$WORK/intrinsic-shadow" --target=gcc --semantic-plan \
  --component-manifest="$ROOT/src/lib/components.manifest"
if grep -q '__concat_str' "$WORK/intrinsic-shadow.c"; then
  fail "intrinsic lookup outranked a resolved selected user function"
fi
gcc -Werror -Wall -Wextra -Wno-sign-compare -pedantic \
  -I"$ROOT/src/lib" -I"$ROOT/src/ext/lib" \
  "$WORK/intrinsic-shadow.c" \
  "$ROOT/src/lib/arena_host.c" "$ROOT/src/lib/pools.c" \
  "$ROOT/src/lib/fundefs.c" "$ROOT/src/lib/termination.c" \
  "$ROOT/src/lib/print_bytes.c" "$ROOT/src/lib/error_sink.c" \
  "$ROOT/src/lib/host_caps.c" "$ROOT/src/lib/host/termbox2_impl.c" \
  -o "$WORK/intrinsic-shadow.exe"
[ "$("$WORK/intrinsic-shadow.exe")" = "user" ] ||
  fail "selected user shadow did not retain call precedence"

for rejected_intrinsic in arity type; do
  if "$ROOT/riftc" \
      "$ROOT/test/fixtures/semantic_plan_intrinsic_${rejected_intrinsic}_unsupported.rift" \
      "$WORK/intrinsic-$rejected_intrinsic" --target=gcc --semantic-plan \
      --component-manifest="$ROOT/src/lib/components.manifest" \
      >"$WORK/intrinsic-$rejected_intrinsic.log" 2>&1; then
    fail "unsupported intrinsic $rejected_intrinsic unexpectedly compiled"
  fi
  [ ! -s "$WORK/intrinsic-$rejected_intrinsic.c" ] ||
    fail "unsupported intrinsic $rejected_intrinsic emitted partial C"
done

echo "PASS: semantic-plan generated-C seam"
