#include "semantic_ir/intrinsics.h"

#include <string.h>

static const sir_signature_parameter CONCAT_PARAMETERS[] = {
    {{SIR_TYPE_STRING, 0},
     SIR_REP_STRING_DESCRIPTOR,
     SIR_OWNERSHIP_BORROWED,
     SIR_ARGUMENT_BORROW},
    {{SIR_TYPE_STRING, 0},
     SIR_REP_STRING_DESCRIPTOR,
     SIR_OWNERSHIP_BORROWED,
     SIR_ARGUMENT_BORROW},
};

static const sir_signature_parameter SUBSTRING_FROM_PARAMETERS[] = {
    {{SIR_TYPE_STRING, 0},
     SIR_REP_STRING_DESCRIPTOR,
     SIR_OWNERSHIP_BORROWED,
     SIR_ARGUMENT_BORROW},
    {{SIR_TYPE_INT, 0},
     SIR_REP_SCALAR,
     SIR_OWNERSHIP_SCALAR,
     SIR_ARGUMENT_SCALAR},
};

static const sir_signature_parameter SUBSTRING_RANGE_PARAMETERS[] = {
    {{SIR_TYPE_STRING, 0},
     SIR_REP_STRING_DESCRIPTOR,
     SIR_OWNERSHIP_BORROWED,
     SIR_ARGUMENT_BORROW},
    {{SIR_TYPE_INT, 0},
     SIR_REP_SCALAR,
     SIR_OWNERSHIP_SCALAR,
     SIR_ARGUMENT_SCALAR},
    {{SIR_TYPE_INT, 0},
     SIR_REP_SCALAR,
     SIR_OWNERSHIP_SCALAR,
     SIR_ARGUMENT_SCALAR},
};

static const sir_intrinsic_descriptor INTRINSICS[] = {
    {{"concat", 6},
     {SIR_CALLEE_INTRINSIC,
      SIR_INTRINSIC_CONCAT_STRING,
      {"__concat_str", 12},
      CONCAT_PARAMETERS,
      2,
      {SIR_TYPE_STRING, 0},
      SIR_REP_STRING_DESCRIPTOR,
      SIR_OWNERSHIP_OWNED,
      {1,
       SIR_EFFECT_CALL | SIR_EFFECT_ALLOCATE | SIR_EFFECT_COLLECT |
           SIR_EFFECT_TERMINATE,
       SIR_MUTATION_NONE},
      SIR_CALL_ABI_OUT_STRING_FIRST}},
    {{"substring", 9},
     {SIR_CALLEE_INTRINSIC,
      SIR_INTRINSIC_SUBSTRING_FROM,
      {"__substring_from", 16},
      SUBSTRING_FROM_PARAMETERS,
      2,
      {SIR_TYPE_STRING, 0},
      SIR_REP_STRING_DESCRIPTOR,
      SIR_OWNERSHIP_OWNED,
      {1, SIR_EFFECT_CALL | SIR_EFFECT_TERMINATE, SIR_MUTATION_NONE},
      SIR_CALL_ABI_OUT_STRING_FIRST}},
    {{"substring", 9},
     {SIR_CALLEE_INTRINSIC,
      SIR_INTRINSIC_SUBSTRING_RANGE,
      {"__substring_range", 17},
      SUBSTRING_RANGE_PARAMETERS,
      3,
      {SIR_TYPE_STRING, 0},
      SIR_REP_STRING_DESCRIPTOR,
      SIR_OWNERSHIP_OWNED,
      {1, SIR_EFFECT_CALL | SIR_EFFECT_TERMINATE, SIR_MUTATION_NONE},
      SIR_CALL_ABI_OUT_STRING_FIRST}},
};

static int view_equal(string_view left, string_view right) {
  return left.length == right.length &&
         memcmp(left.data, right.data, left.length) == 0;
}

static int effects_equal(sir_effects left, sir_effects right) {
  return left.known == right.known && left.flags == right.flags &&
         left.mutation == right.mutation;
}

size_t sir_intrinsic_count(void) {
  return sizeof(INTRINSICS) / sizeof(*INTRINSICS);
}

const sir_intrinsic_descriptor *sir_intrinsic_at(size_t index) {
  return index < sir_intrinsic_count() ? &INTRINSICS[index] : NULL;
}

const sir_intrinsic_descriptor *sir_intrinsic_lookup(string_view source_name,
                                                     size_t arity) {
  size_t index;
  for (index = 0; index < sir_intrinsic_count(); index++) {
    if (INTRINSICS[index].signature.parameter_count == arity &&
        view_equal(INTRINSICS[index].source_name, source_name)) {
      return &INTRINSICS[index];
    }
  }
  return NULL;
}

int sir_intrinsic_signature_matches(const sir_signature *signature) {
  const sir_intrinsic_descriptor *canonical;
  size_t index;
  if (signature == NULL || signature->kind != SIR_CALLEE_INTRINSIC)
    return 0;
  canonical = NULL;
  for (index = 0; index < sir_intrinsic_count(); index++) {
    if (INTRINSICS[index].signature.symbol_id == signature->symbol_id) {
      canonical = &INTRINSICS[index];
      break;
    }
  }
  if (canonical == NULL ||
      !view_equal(canonical->signature.c_symbol, signature->c_symbol) ||
      canonical->signature.parameter_count != signature->parameter_count ||
      !sir_type_equal(canonical->signature.return_type,
                      signature->return_type) ||
      canonical->signature.return_representation !=
          signature->return_representation ||
      canonical->signature.return_ownership != signature->return_ownership ||
      !effects_equal(canonical->signature.effects, signature->effects) ||
      canonical->signature.call_abi != signature->call_abi) {
    return 0;
  }
  for (index = 0; index < signature->parameter_count; index++) {
    const sir_signature_parameter *expected =
        &canonical->signature.parameters[index];
    const sir_signature_parameter *actual = &signature->parameters[index];
    if (!sir_type_equal(expected->type, actual->type) ||
        expected->representation != actual->representation ||
        expected->ownership != actual->ownership ||
        expected->mode != actual->mode) {
      return 0;
    }
  }
  return 1;
}
