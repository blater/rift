#!/bin/bash
# Negative test runner.
#
# Two test modes:
#   compile — Rock program MUST fail compilation with the expected diagnostic.
#   runtime — Rock program MUST compile, then exit non-zero at runtime with
#             the expected diagnostic.

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ROCK="$ROOT/rock"
ROCKC="$ROOT/rockc"

pass=0
fail=0

run_compile_neg() {
  local file="$1" expect="$2"
  local out exit_code
  out="$($ROCKC "$file" /tmp/neg_out.exe 2>&1)"
  exit_code=$?
  rm -f /tmp/neg_out.exe /tmp/neg_out.exe.c

  if [ "$exit_code" -eq 0 ]; then
    echo "  FAIL $file — compiled successfully (expected compile failure)"
    fail=$((fail+1))
    return
  fi
  if ! echo "$out" | grep -qF "$expect"; then
    echo "  FAIL $file — exit non-zero but missing expected diagnostic"
    echo "    expected substring: $expect"
    echo "    actual output:"
    echo "$out" | sed 's/^/      /'
    fail=$((fail+1))
    return
  fi
  echo "  ok (compile-fail) $(basename "$file")"
  pass=$((pass+1))
}

run_runtime_neg() {
  local file="$1" expect="$2"
  local exe="${file%.rkr}.exe"
  local cout="${exe}.c"
  local compile_out runtime_out exit_code

  compile_out="$($ROCK "$file" 2>&1)"
  if [ ! -x "$exe" ]; then
    echo "  FAIL $file — did not compile (expected compile success)"
    echo "$compile_out" | sed 's/^/      /'
    fail=$((fail+1))
    return
  fi

  runtime_out="$("$exe" 2>&1)"
  exit_code=$?
  rm -f "$exe" "$cout"

  if [ "$exit_code" -eq 0 ]; then
    echo "  FAIL $file — runtime exit 0 (expected non-zero)"
    fail=$((fail+1))
    return
  fi
  if ! echo "$runtime_out" | grep -qF "$expect"; then
    echo "  FAIL $file — runtime exit non-zero but missing expected diagnostic"
    echo "    expected substring: $expect"
    echo "    actual output:"
    echo "$runtime_out" | sed 's/^/      /'
    fail=$((fail+1))
    return
  fi
  echo "  ok (runtime-halt) $(basename "$file")"
  pass=$((pass+1))
}

cd "$ROOT"

# Compile-failure tests (typechecker rejects the program).
run_compile_neg test/negative/recursive_record_via_array.rkr \
                "recursive type definition 'Node' forbidden"
run_compile_neg test/negative/recursive_record_direct.rkr \
                "recursive type definition 'Loop' forbidden"
run_compile_neg test/negative/mutual_recursion.rkr \
                "recursive type definition 'A' forbidden"
# case is no longer a selector keyword.
run_compile_neg test/negative/legacy_case_selector.rkr \
                "After identifier 'case', expected '(' for function call"
run_compile_neg test/negative/match_keyword.rkr \
                "Expected ':=' in assignment, got match"
run_compile_neg test/negative/collect_identifier.rkr \
                "Expected ':=' in assignment, got collect"

# Runtime-halt tests (program compiles but halts at runtime).
run_runtime_neg test/negative/setcharat_on_literal.rkr \
                "cannot mutate read-only string view"
run_runtime_neg test/negative/empty_array_pop.rkr \
                "Could not pop elem out of dynamic array: EMPTY ARRAY"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
