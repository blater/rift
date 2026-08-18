#!/bin/bash
# Negative test runner.
#
# Two test modes:
#   compile — Rift program MUST fail compilation with the expected diagnostic.
#   runtime — Rift program MUST compile, then exit non-zero at runtime with
#             the expected diagnostic.

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RIFT="$ROOT/rift"
RIFTC="$ROOT/riftc"

pass=0
fail=0

run_compile_neg() {
  local file="$1" expect="$2"
  local out exit_code
  out="$($RIFTC "$file" /tmp/neg_out.exe 2>&1)"
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
  local source_base="${file%.rift}"
  source_base="${source_base%.rft}"
  local exe="${source_base}.exe"
  local cout="${exe}.c"
  local compile_out runtime_out exit_code

  compile_out="$($RIFT --target=gcc "$file" 2>&1)"
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
run_compile_neg test/negative/recursive_record_via_array.rift \
                "recursive type definition 'Node' forbidden"
run_compile_neg test/negative/recursive_record_direct.rift \
                "recursive type definition 'Loop' forbidden"
run_compile_neg test/negative/mutual_recursion.rift \
                "recursive type definition 'A' forbidden"
# case is no longer a selector keyword.
run_compile_neg test/negative/legacy_case_selector.rift \
                "After identifier 'case', expected '(' for function call"
run_compile_neg test/negative/match_keyword.rift \
                "Expected ':=' in assignment, got match"
run_compile_neg test/negative/collect_identifier.rift \
                "Expected ':=' in assignment, got collect"
run_compile_neg test/negative/type_method_free_static.rift \
                "type-level method declaration must be qualified"
run_compile_neg test/negative/type_method_array_static.rift \
                "array type-level methods are not supported"
run_compile_neg test/negative/type_method_unknown_owner.rift \
                "type-level method owner 'Missing' must be a user-defined"
run_compile_neg test/negative/type_method_builtin_owner.rift \
                "must be a user-defined module, record, or union"
run_compile_neg test/negative/type_method_repeated_static.rift \
                "Expected 'sub' after 'static'"
run_compile_neg test/negative/type_method_nested.rift \
                "type-level method declarations are only allowed at top level"
run_compile_neg test/negative/type_method_reserved_name.rift \
                "identifiers beginning with 'rift__tm_' are reserved"
run_compile_neg test/negative/type_method_reserved_local.rift \
                "identifiers beginning with 'rift__tm_' are reserved"
run_compile_neg test/negative/duplicate_local.rift \
                "duplicate variable 'value' in this scope"
run_compile_neg test/negative/duplicate_type.rift \
                "duplicate type definition 'Pair' in this scope"
run_compile_neg test/negative/duplicate_global.rift \
                "duplicate variable 'value' in this scope"
run_compile_neg test/negative/duplicate_free_function.rift \
                "duplicate function 'value' with 1 argument(s) in this scope"
run_compile_neg test/negative/duplicate_parameter_local.rift \
                "duplicate variable 'value' in this scope"
run_compile_neg test/negative/duplicate_enum_type.rift \
                "duplicate type definition 'Pair' in this scope"
run_compile_neg test/negative/duplicate_field.rift \
                "duplicate member 'value' in this type"
run_compile_neg test/negative/type_method_phantom_value.rift \
                "cannot determine type of receiver for method call 'ping'"
run_compile_neg test/negative/type_method_wrong_arity.rift \
                "has no overload taking 0 argument(s)"
run_compile_neg test/negative/type_method_duplicate.rift \
                "duplicate method 'Tool.reset'"
run_compile_neg test/negative/type_method_instance_through_type.rift \
                "is an instance method; call it through a value"
run_compile_neg test/negative/type_method_through_value.rift \
                "is a type-level method; call Tool.reset()"
run_compile_neg test/negative/type_method_shadowed_type.rift \
                "is a type-level method; call Tool.reset()"
run_compile_neg test/negative/type_method_this.rift \
                "type-level methods have no 'this' receiver"
run_compile_neg test/negative/type_method_implicit_field.rift \
                "type-level method cannot access instance field 'value'"
run_compile_neg test/negative/sprite_namespace_redefine.rift \
                "standard type namespace 'Sprite' is sealed and cannot be redefined"
run_compile_neg test/negative/sprite_namespace_instance_extension.rift \
                "standard type namespace 'Sprite' is sealed; source methods cannot extend it"
run_compile_neg test/negative/sprite_namespace_type_extension.rift \
                "standard type namespace 'Sprite' is sealed; source methods cannot extend it"
run_compile_neg test/negative/sprite_namespace_type_arity.rift \
                "type-level method 'Sprite.hideall' has no overload taking 1 argument(s)"
run_compile_neg test/negative/sprite_constructor_literal_range.rift \
                "sprite slot literal 128 is outside 0..127"
run_compile_neg test/negative/sprite_constructor_argument_type.rift \
                "Sprite() slot argument must be a byte-compatible integer expression"
run_compile_neg test/negative/sprite_constructor_wide_dynamic.rift \
                "Sprite() slot argument must be a byte-compatible integer expression"
run_compile_neg test/negative/sprite_constructor_fractional_literal.rift \
                "Sprite() slot argument must be a byte-compatible integer expression"
run_compile_neg test/negative/sprite_constructor_negative_fractional_literal.rift \
                "Sprite() slot argument must be a byte-compatible integer expression"
run_compile_neg test/negative/sprite_position_fractional_x.rift \
                "sprite x must be an integer literal"
run_compile_neg test/negative/sprite_position_fractional_y.rift \
                "sprite y must be an integer literal"
run_compile_neg test/negative/sprite_direct_numeric_init.rift \
                "Sprite declaration requires a Sprite value"
run_compile_neg test/negative/sprite_numeric_assignment.rift \
                "Sprite assignment requires a Sprite value"
run_compile_neg test/negative/sprite_parameter_numeric.rift \
                "function argument requires a Sprite value"
run_compile_neg test/negative/sprite_parameter_omitted.rift \
                "function argument count for Sprite-related call must be 1, got 0"
run_compile_neg test/negative/sprite_parameter_extra.rift \
                "function argument count for Sprite-related call must be 1, got 2"
run_compile_neg test/negative/sprite_method_parameter_numeric.rift \
                "method argument requires a Sprite value"
run_compile_neg test/negative/sprite_return_numeric.rift \
                "return expression requires a Sprite value"
run_compile_neg test/negative/sprite_return_bare.rift \
                "Sprite-returning function requires an explicit return value"
run_compile_neg test/negative/sprite_return_missing.rift \
                "Sprite-returning function may fall through without returning a value"
run_compile_neg test/negative/sprite_return_partial_branch.rift \
                "Sprite-returning function may fall through without returning a value"
run_compile_neg test/negative/sprite_return_match_no_default.rift \
                "Sprite-returning function may fall through without returning a value"
run_compile_neg test/negative/sprite_return_match_partial.rift \
                "Sprite-returning function may fall through without returning a value"
run_compile_neg test/negative/sprite_return_function_extra_argument.rift \
                "function argument count for Sprite-related call must be 0, got 1"
run_compile_neg test/negative/sprite_return_method_extra_argument.rift \
                "method argument count for Sprite-related call must be 0, got 1"
run_compile_neg test/negative/sprite_array_return_missing.rift \
                "Sprite-returning function may fall through without returning a value"
run_compile_neg test/negative/sprite_default_global.rift \
                "Sprite has no default slot; use Sprite(byteSlot)"
run_compile_neg test/negative/sprite_default_module_field.rift \
                "Sprite has no default slot; use Sprite(byteSlot)"
run_compile_neg test/negative/sprite_record_init_numeric.rift \
                "Sprite record field initializer requires a Sprite value"
run_compile_neg test/negative/sprite_record_omitted_field.rift \
                "Sprite record field 'sprite' requires explicit initialization"
run_compile_neg test/negative/sprite_record_omitted_array_field.rift \
                "Sprite record field 'sprites' requires explicit initialization"
run_compile_neg test/negative/sprite_record_array_field_from_byte_array.rift \
                "Sprite array assignment requires a Sprite array value"
run_compile_neg test/negative/sprite_field_assignment_numeric.rift \
                "Sprite assignment requires a Sprite value"
run_compile_neg test/negative/sprite_indexed_record_field_assignment.rift \
                "assignment to a field through an array element is not supported"
run_compile_neg test/negative/sprite_indexed_record_field_numeric.rift \
                "assignment to a field through an array element is not supported"
run_compile_neg test/negative/sprite_indexed_record_array_field_byte_array.rift \
                "assignment to a field through an array element is not supported"
run_compile_neg test/negative/sprite_union_argument_numeric.rift \
                "union constructor argument requires a Sprite value"
run_compile_neg test/negative/sprite_union_argument_omitted.rift \
                "value constructor 'SomeSprite' expects exactly 1 argument(s), got 0"
run_compile_neg test/negative/sprite_union_void_variant_extra.rift \
                "value constructor 'None' expects exactly 0 argument(s), got 1"
run_compile_neg test/negative/sprite_union_byte_variant_missing.rift \
                "value constructor 'Number' expects exactly 1 argument(s), got 0"
run_compile_neg test/negative/enum_variant_extra_argument.rift \
                "enum item 'On' is a value; use On without parentheses"
run_compile_neg test/negative/enum_variant_zero_call.rift \
                "enum item 'On' is a value; use On without parentheses"
run_compile_neg test/negative/sprite_array_append_numeric.rift \
                "Sprite array write requires a Sprite value"
run_compile_neg test/negative/sprite_array_set_numeric.rift \
                "Sprite array write requires a Sprite value"
run_compile_neg test/negative/sprite_array_index_numeric.rift \
                "Sprite assignment requires a Sprite value"
run_compile_neg test/negative/sprite_array_from_byte_array.rift \
                "Sprite declaration requires a Sprite array value"
run_compile_neg test/negative/sprite_iterator_numeric_assignment.rift \
                "Sprite assignment requires a Sprite value"
run_compile_neg test/negative/sprite_union_variant_collision.rift \
                "variant name 'Sprite' is reserved by the built-in value constructor"
run_compile_neg test/negative/sprite_enum_variant_collision.rift \
                "variant name 'Sprite' is reserved by the built-in value constructor"
run_compile_neg test/negative/sprite_constructor_collision_byte_first.rift \
                "value constructor 'PickByte' conflicts with an earlier enum or union value constructor"
run_compile_neg test/negative/sprite_constructor_collision_sprite_first.rift \
                "value constructor 'PickByte' conflicts with an earlier enum or union value constructor"
run_compile_neg test/negative/sprite_constructor_function_collision_union_first.rift \
                "function 'Pick' conflicts with an enum or union value constructor"
run_compile_neg test/negative/sprite_constructor_function_collision_function_first.rift \
                "function 'Pick' conflicts with an enum or union value constructor"
run_compile_neg test/negative/sprite_pattern_legacy_syntax.rift \
                "'asset' declarations were removed"
run_compile_neg test/negative/sprite_pattern_nested.rift \
                "SpritePattern bindings are only allowed at file scope"

# Runtime-halt tests (program compiles but halts at runtime).
run_runtime_neg test/negative/setcharat_on_literal.rift \
                "cannot mutate read-only string view"
run_runtime_neg test/negative/empty_array_pop.rift \
                "Could not pop elem out of dynamic array: EMPTY ARRAY"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
