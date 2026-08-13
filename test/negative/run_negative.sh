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
  rm -f /tmp/neg_out.exe /tmp/neg_out.exe.c /tmp/neg_out.exe.components

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
  rm -f "$exe" "$cout" "${exe}.components"

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
run_compile_neg test/negative/type_method_free_static.rkr \
                "type-level method declaration must be qualified"
run_compile_neg test/negative/type_method_array_static.rkr \
                "array type-level methods are not supported"
run_compile_neg test/negative/type_method_unknown_owner.rkr \
                "type-level method owner 'Missing' must be a user-defined"
run_compile_neg test/negative/type_method_builtin_owner.rkr \
                "must be a user-defined module, record, or union"
run_compile_neg test/negative/type_method_repeated_static.rkr \
                "Expected 'sub' after 'static'"
run_compile_neg test/negative/type_method_nested.rkr \
                "type-level method declarations are only allowed at top level"
run_compile_neg test/negative/type_method_reserved_name.rkr \
                "identifiers beginning with 'rock__tm_' are reserved"
run_compile_neg test/negative/type_method_reserved_local.rkr \
                "identifiers beginning with 'rock__tm_' are reserved"
run_compile_neg test/negative/duplicate_local.rkr \
                "duplicate variable 'value' in this scope"
run_compile_neg test/negative/duplicate_type.rkr \
                "duplicate type definition 'Pair' in this scope"
run_compile_neg test/negative/duplicate_global.rkr \
                "duplicate variable 'value' in this scope"
run_compile_neg test/negative/duplicate_free_function.rkr \
                "duplicate function 'value' with 1 argument(s) in this scope"
run_compile_neg test/negative/duplicate_parameter_local.rkr \
                "duplicate variable 'value' in this scope"
run_compile_neg test/negative/duplicate_enum_type.rkr \
                "duplicate type definition 'Pair' in this scope"
run_compile_neg test/negative/duplicate_field.rkr \
                "duplicate member 'value' in this type"
run_compile_neg test/negative/type_method_phantom_value.rkr \
                "cannot determine type of receiver for method call 'ping'"
run_compile_neg test/negative/type_method_wrong_arity.rkr \
                "has no overload taking 0 argument(s)"
run_compile_neg test/negative/type_method_duplicate.rkr \
                "duplicate method 'Tool.reset'"
run_compile_neg test/negative/type_method_instance_through_type.rkr \
                "is an instance method; call it through a value"
run_compile_neg test/negative/type_method_through_value.rkr \
                "is a type-level method; call Tool.reset()"
run_compile_neg test/negative/type_method_shadowed_type.rkr \
                "is a type-level method; call Tool.reset()"
run_compile_neg test/negative/type_method_this.rkr \
                "type-level methods have no 'this' receiver"
run_compile_neg test/negative/type_method_implicit_field.rkr \
                "type-level method cannot access instance field 'value'"
run_compile_neg test/negative/opaque_sprite_redefine.rkr \
                "standard opaque type 'Sprite' is sealed and cannot be redefined"
run_compile_neg test/negative/opaque_sprite_instance_extension.rkr \
                "standard opaque type 'Sprite' is sealed; source methods cannot extend it"
run_compile_neg test/negative/opaque_sprite_type_extension.rkr \
                "standard opaque type 'Sprite' is sealed; source methods cannot extend it"
run_compile_neg test/negative/opaque_sprite_field.rkr \
                "fields of standard opaque type cannot be accessed"
run_compile_neg test/negative/opaque_sprite_literal.rkr \
                "standard opaque type 'Sprite' cannot be constructed with an aggregate literal"
run_compile_neg test/negative/opaque_sprite_assign_literal.rkr \
                "standard opaque values cannot be assigned an aggregate literal"
run_compile_neg test/negative/opaque_sprite_constructor_collision.rkr \
                "'Sprite_new' is reserved by a standard opaque interface"
run_compile_neg test/negative/opaque_sprite_method_collision.rkr \
                "'Sprite_index' is reserved by a standard opaque interface"
run_compile_neg test/negative/opaque_sprite_instance_arity.rkr \
                "instance method 'Sprite.index' expects 1 argument(s), got 0"
run_compile_neg test/negative/opaque_sprite_type_arity.rkr \
                "type-level method 'Sprite.hideall' has no overload taking 1 argument(s)"

# Runtime-halt tests (program compiles but halts at runtime).
run_runtime_neg test/negative/setcharat_on_literal.rkr \
                "cannot mutate read-only string view"
run_runtime_neg test/negative/empty_array_pop.rkr \
                "Could not pop elem out of dynamic array: EMPTY ARRAY"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
