#include "semantic_ir/lower.h"

#include "semantic_ir/intrinsics.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct lower_binding {
  string_view name;
  sir_id slot;
  int visible;
} lower_binding;

typedef struct lower_state {
  sir_function function;
  lower_binding *bindings;
  size_t binding_count;
  size_t binding_capacity;
  sir_id block;
  const sir_lower_options *options;
  sir_diagnostic *diagnostic;
} lower_state;

static void lower_error(lower_state *state, sir_diagnostic_code code,
                        const char *message) {
  if (state->diagnostic == NULL) {
    return;
  }
  state->diagnostic->code = code;
  state->diagnostic->block_index =
      state->block == SIR_INVALID_ID ? 0 : (size_t)state->block;
  state->diagnostic->operation_index = 0;
  state->diagnostic->message = message;
}

static int view_is(string_view view, const char *text) {
  size_t length = strlen(text);
  return view.length == length && memcmp(view.data, text, length) == 0;
}

static int metadata_for_type(ast_t type_ast, sir_type *output_type,
                             sir_representation *representation,
                             sir_ownership *ownership) {
  string_view name;
  if (type_ast == NULL || type_ast->tag != type) {
    return 0;
  }
  name = type_ast->data.type.name.lexeme;
  if (type_ast->data.type.is_array) {
    if (!view_is(name, "string") || type_ast->data.type.array_capacity != 0)
      return 0;
    output_type->kind = SIR_TYPE_ARRAY;
    output_type->nominal_id = SIR_TYPE_STRING;
    *representation = SIR_REP_MANAGED_HANDLE;
    *ownership = SIR_OWNERSHIP_OWNED;
    return 1;
  }
  if (view_is(name, "void")) {
    *output_type = sir_builtin_type(SIR_TYPE_VOID);
    *representation = SIR_REP_NONE;
    *ownership = SIR_OWNERSHIP_NONE;
  } else if (view_is(name, "bool")) {
    *output_type = sir_builtin_type(SIR_TYPE_BOOL);
    *representation = SIR_REP_SCALAR;
    *ownership = SIR_OWNERSHIP_SCALAR;
  } else if (view_is(name, "byte")) {
    *output_type = sir_builtin_type(SIR_TYPE_BYTE);
    *representation = SIR_REP_SCALAR;
    *ownership = SIR_OWNERSHIP_SCALAR;
  } else if (view_is(name, "word")) {
    *output_type = sir_builtin_type(SIR_TYPE_WORD);
    *representation = SIR_REP_SCALAR;
    *ownership = SIR_OWNERSHIP_SCALAR;
  } else if (view_is(name, "dword")) {
    *output_type = sir_builtin_type(SIR_TYPE_DWORD);
    *representation = SIR_REP_SCALAR;
    *ownership = SIR_OWNERSHIP_SCALAR;
  } else if (view_is(name, "int")) {
    *output_type = sir_builtin_type(SIR_TYPE_INT);
    *representation = SIR_REP_SCALAR;
    *ownership = SIR_OWNERSHIP_SCALAR;
  } else if (view_is(name, "float")) {
    *output_type = sir_builtin_type(SIR_TYPE_FLOAT);
    *representation = SIR_REP_SCALAR;
    *ownership = SIR_OWNERSHIP_SCALAR;
  } else if (view_is(name, "char")) {
    *output_type = sir_builtin_type(SIR_TYPE_CHAR);
    *representation = SIR_REP_SCALAR;
    *ownership = SIR_OWNERSHIP_SCALAR;
  } else if (view_is(name, "string")) {
    *output_type = sir_builtin_type(SIR_TYPE_STRING);
    *representation = SIR_REP_STRING_DESCRIPTOR;
    *ownership = SIR_OWNERSHIP_OWNED;
  } else {
    return 0;
  }
  return 1;
}

static int reserve_bindings(lower_state *state, size_t required) {
  size_t capacity;
  lower_binding *bindings;
  if (state->binding_capacity >= required) {
    return 1;
  }
  capacity = state->binding_capacity == 0 ? 4 : state->binding_capacity;
  while (capacity < required) {
    if (capacity > SIZE_MAX / 2) {
      return 0;
    }
    capacity *= 2;
  }
  bindings = realloc(state->bindings, capacity * sizeof(*bindings));
  if (bindings == NULL) {
    return 0;
  }
  state->bindings = bindings;
  state->binding_capacity = capacity;
  return 1;
}

static int binding_index(const lower_state *state, string_view name,
                         size_t *index) {
  size_t candidate;
  for (candidate = state->binding_count; candidate != 0; candidate--) {
    if (!state->bindings[candidate - 1].visible)
      continue;
    string_view binding_name = state->bindings[candidate - 1].name;
    if (binding_name.length == name.length &&
        memcmp(binding_name.data, name.data, name.length) == 0) {
      *index = candidate - 1;
      return 1;
    }
  }
  return 0;
}

static int add_binding(lower_state *state, string_view name, sir_id slot) {
  size_t duplicate;
  if (binding_index(state, name, &duplicate)) {
    lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                "representative lowerer rejects duplicate local names");
    return 0;
  }
  if (!reserve_bindings(state, state->binding_count + 1)) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not allocate semantic bindings");
    return 0;
  }
  state->bindings[state->binding_count].name = name;
  state->bindings[state->binding_count].slot = slot;
  state->bindings[state->binding_count].visible = 1;
  state->binding_count++;
  return 1;
}

static int add_private_binding(lower_state *state, string_view name,
                               sir_id slot) {
  if (!reserve_bindings(state, state->binding_count + 1)) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not allocate private semantic binding");
    return 0;
  }
  state->bindings[state->binding_count].name = name;
  state->bindings[state->binding_count].slot = slot;
  state->bindings[state->binding_count].visible = 0;
  state->binding_count++;
  return 1;
}

static sir_operation empty_operation(sir_op_kind kind) {
  sir_operation operation;
  memset(&operation, 0, sizeof(operation));
  operation.kind = kind;
  operation.primitive = SIR_PRIMITIVE_UNKNOWN;
  operation.effects = sir_effects_none();
  operation.result = SIR_INVALID_ID;
  operation.slot = SIR_INVALID_ID;
  operation.callee = SIR_INVALID_ID;
  operation.parameter_index = SIZE_MAX;
  operation.array_input_mode = SIR_ARRAY_INPUT_UNKNOWN;
  operation.targets[0] = SIR_INVALID_ID;
  operation.targets[1] = SIR_INVALID_ID;
  return operation;
}

static sir_id lower_identifier(lower_state *state, ast_t expression) {
  size_t binding;
  sir_slot *slot;
  sir_value value;
  sir_operation operation;
  sir_id value_id;

  if (expression == NULL || expression->tag != identifier ||
      !binding_index(state, expression->data.identifier.id.lexeme, &binding)) {
    lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                "representative lowerer requires a resolved identifier");
    return SIR_INVALID_ID;
  }
  slot = &state->function.slots[state->bindings[binding].slot];
  value.type = slot->type;
  value.representation = slot->representation;
  value.ownership = slot->ownership == SIR_OWNERSHIP_SCALAR
                        ? SIR_OWNERSHIP_SCALAR
                        : SIR_OWNERSHIP_BORROWED;
  value_id = sir_function_add_value(&state->function, value);
  if (value_id == SIR_INVALID_ID) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not allocate semantic value");
    return SIR_INVALID_ID;
  }
  operation = empty_operation(slot->ownership == SIR_OWNERSHIP_SCALAR
                                  ? SIR_OP_LOAD_SLOT
                                  : SIR_OP_BORROW_SLOT);
  operation.result = value_id;
  operation.slot = state->bindings[binding].slot;
  if (!sir_block_add_operation(&state->function, state->block, &operation)) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not append semantic operation");
    return SIR_INVALID_ID;
  }
  return value_id;
}

static sir_id lower_bool_constant(lower_state *state, ast_t expression) {
  sir_value value;
  sir_operation operation;
  sir_id value_id;
  int bool_value;
  if (expression == NULL || expression->tag != identifier ||
      (!view_is(expression->data.identifier.id.lexeme, "true") &&
       !view_is(expression->data.identifier.id.lexeme, "false")))
    return SIR_INVALID_ID;
  bool_value = view_is(expression->data.identifier.id.lexeme, "true");
  value.type = sir_builtin_type(SIR_TYPE_BOOL);
  value.representation = SIR_REP_SCALAR;
  value.ownership = SIR_OWNERSHIP_SCALAR;
  value_id = sir_function_add_value(&state->function, value);
  if (value_id == SIR_INVALID_ID) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not allocate boolean constant");
    return SIR_INVALID_ID;
  }
  operation = empty_operation(SIR_OP_CONSTANT);
  operation.result = value_id;
  operation.bool_value = bool_value;
  if (!sir_block_add_operation(&state->function, state->block, &operation)) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not append boolean constant");
    return SIR_INVALID_ID;
  }
  return value_id;
}

static sir_id lower_int_constant(lower_state *state, ast_t expression) {
  string_view text;
  char *copy;
  char *end;
  long parsed;
  sir_value value;
  sir_operation operation;
  sir_id value_id;
  if (expression == NULL || expression->tag != literal ||
      expression->data.literal.lit.type != TOK_NUM_LIT)
    return SIR_INVALID_ID;
  text = expression->data.literal.lit.lexeme;
  if (text.data == NULL || text.length == 0 || text.length == SIZE_MAX) {
    lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                "substring indices require bounded integer literals");
    return SIR_INVALID_ID;
  }
  copy = malloc(text.length + 1);
  if (copy == NULL) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not allocate integer literal text");
    return SIR_INVALID_ID;
  }
  memcpy(copy, text.data, text.length);
  copy[text.length] = '\0';
  errno = 0;
  parsed = strtol(copy, &end, 10);
  if (errno != 0 || end != copy + text.length || parsed < 0 ||
      parsed > INT16_MAX) {
    free(copy);
    lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                "substring indices require bounded integer literals");
    return SIR_INVALID_ID;
  }
  free(copy);
  value.type = sir_builtin_type(SIR_TYPE_INT);
  value.representation = SIR_REP_SCALAR;
  value.ownership = SIR_OWNERSHIP_SCALAR;
  value_id = sir_function_add_value(&state->function, value);
  if (value_id == SIR_INVALID_ID) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not allocate integer constant");
    return SIR_INVALID_ID;
  }
  operation = empty_operation(SIR_OP_CONSTANT);
  operation.result = value_id;
  operation.int_value = (int)parsed;
  if (!sir_block_add_operation(&state->function, state->block, &operation)) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not append integer constant");
    return SIR_INVALID_ID;
  }
  return value_id;
}

static sir_id lower_expression(lower_state *state, ast_t expression);

static int string_array_value(const sir_value *value) {
  return value->type.kind == SIR_TYPE_ARRAY &&
         value->type.nominal_id == SIR_TYPE_STRING &&
         value->representation == SIR_REP_MANAGED_HANDLE &&
         (value->ownership == SIR_OWNERSHIP_BORROWED ||
          value->ownership == SIR_OWNERSHIP_OWNED);
}

static sir_effects array_effects(sir_op_kind kind) {
  sir_effects effects = sir_effects_none();
  effects.flags = SIR_EFFECT_CALL | SIR_EFFECT_TERMINATE;
  if (kind == SIR_OP_ARRAY_NEW_STRING ||
      kind == SIR_OP_ARRAY_APPEND_STRING ||
      kind == SIR_OP_ARRAY_GET_STRING) {
    effects.flags |= SIR_EFFECT_ALLOCATE | SIR_EFFECT_COLLECT;
  }
  if (kind == SIR_OP_ARRAY_APPEND_STRING ||
      kind == SIR_OP_ARRAY_REPLACE_STRING) {
    effects.mutation = SIR_MUTATION_ARGUMENT;
  }
  return effects;
}

static sir_id prepare_owned(lower_state *state, sir_id source,
                            sir_array_input_mode *mode) {
  sir_value value;
  sir_operation operation;
  sir_id result;
  if (source >= state->function.value_count ||
      (state->function.values[source].ownership != SIR_OWNERSHIP_BORROWED &&
       state->function.values[source].ownership != SIR_OWNERSHIP_OWNED)) {
    lower_error(state, SIR_DIAGNOSTIC_TYPE_MISMATCH,
                "managed array operand is not ownable");
    return SIR_INVALID_ID;
  }
  value = state->function.values[source];
  *mode = value.ownership == SIR_OWNERSHIP_BORROWED
              ? SIR_ARRAY_INPUT_COPY_BORROWED
              : SIR_ARRAY_INPUT_TRANSFER_OWNED;
  value.ownership = SIR_OWNERSHIP_OWNED;
  result = sir_function_add_value(&state->function, value);
  if (result == SIR_INVALID_ID) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not allocate prepared managed operand");
    return SIR_INVALID_ID;
  }
  operation = empty_operation(SIR_OP_PREPARE_OWNED);
  operation.result = result;
  operation.array_input_mode = *mode;
  operation.operands = &source;
  operation.operand_count = 1;
  if (!sir_block_add_operation(&state->function, state->block, &operation)) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not append prepared managed operand");
    return SIR_INVALID_ID;
  }
  return result;
}

static sir_id lower_array_new(lower_state *state, ast_t expression) {
  sir_value value;
  sir_operation operation;
  sir_id result;
  if (expression == NULL || expression->tag != literal ||
      expression->data.literal.lit.type != TOK_ARR_DECL)
    return SIR_INVALID_ID;
  value.type.kind = SIR_TYPE_ARRAY;
  value.type.nominal_id = SIR_TYPE_STRING;
  value.representation = SIR_REP_MANAGED_HANDLE;
  value.ownership = SIR_OWNERSHIP_OWNED;
  result = sir_function_add_value(&state->function, value);
  if (result == SIR_INVALID_ID) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not allocate string-array value");
    return SIR_INVALID_ID;
  }
  operation = empty_operation(SIR_OP_ARRAY_NEW_STRING);
  operation.effects = array_effects(operation.kind);
  operation.result = result;
  if (!sir_block_add_operation(&state->function, state->block, &operation)) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not append string-array construction");
    return SIR_INVALID_ID;
  }
  return result;
}

static int unresolved_array_builtin(ast_funcall *call, const char *name,
                                    size_t arity) {
  return call->resolved_target == NULL && view_is(call->name.lexeme, name) &&
         call->args.length >= 0 && (size_t)call->args.length == arity;
}

static sir_id lower_array_get(lower_state *state, ast_t expression) {
  ast_funcall *call = &expression->data.funcall;
  sir_id receiver;
  sir_id held_receiver;
  sir_id index;
  sir_id result;
  sir_id operands[2];
  sir_value value;
  sir_operation operation;
  sir_array_input_mode ignored;
  if (!unresolved_array_builtin(call, "get", 2))
    return SIR_INVALID_ID;
  receiver = lower_expression(state, call->args.data[0]);
  if (receiver == SIR_INVALID_ID ||
      !string_array_value(&state->function.values[receiver])) {
    lower_error(state, SIR_DIAGNOSTIC_TYPE_MISMATCH,
                "get requires a dynamic string array");
    return SIR_INVALID_ID;
  }
  held_receiver = prepare_owned(state, receiver, &ignored);
  index = lower_expression(state, call->args.data[1]);
  if (held_receiver == SIR_INVALID_ID || index == SIR_INVALID_ID ||
      state->function.values[index].type.kind != SIR_TYPE_INT ||
      state->function.values[index].representation != SIR_REP_SCALAR) {
    lower_error(state, SIR_DIAGNOSTIC_TYPE_MISMATCH,
                "get index must be an int");
    return SIR_INVALID_ID;
  }
  value.type = sir_builtin_type(SIR_TYPE_STRING);
  value.representation = SIR_REP_STRING_DESCRIPTOR;
  value.ownership = SIR_OWNERSHIP_OWNED;
  result = sir_function_add_value(&state->function, value);
  if (result == SIR_INVALID_ID)
    return SIR_INVALID_ID;
  operands[0] = held_receiver;
  operands[1] = index;
  operation = empty_operation(SIR_OP_ARRAY_GET_STRING);
  operation.effects = array_effects(operation.kind);
  operation.result = result;
  operation.operands = operands;
  operation.operand_count = 2;
  if (!sir_block_add_operation(&state->function, state->block, &operation)) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not append string-array get");
    return SIR_INVALID_ID;
  }
  return result;
}

static int lower_array_append(lower_state *state, ast_t expression) {
  ast_funcall *call = &expression->data.funcall;
  sir_id receiver;
  sir_id held_receiver;
  sir_id source;
  sir_id prepared;
  sir_id operands[2];
  sir_operation operation;
  sir_array_input_mode ignored;
  sir_array_input_mode mode;
  if (!unresolved_array_builtin(call, "append", 2))
    return 0;
  receiver = lower_expression(state, call->args.data[0]);
  if (receiver == SIR_INVALID_ID ||
      !string_array_value(&state->function.values[receiver])) {
    lower_error(state, SIR_DIAGNOSTIC_TYPE_MISMATCH,
                "append requires a dynamic string array");
    return -1;
  }
  held_receiver = prepare_owned(state, receiver, &ignored);
  source = lower_expression(state, call->args.data[1]);
  if (held_receiver == SIR_INVALID_ID || source == SIR_INVALID_ID ||
      state->function.values[source].type.kind != SIR_TYPE_STRING ||
      state->function.values[source].representation !=
          SIR_REP_STRING_DESCRIPTOR) {
    lower_error(state, SIR_DIAGNOSTIC_TYPE_MISMATCH,
                "append value must be a string");
    return -1;
  }
  prepared = prepare_owned(state, source, &mode);
  if (prepared == SIR_INVALID_ID)
    return -1;
  operands[0] = held_receiver;
  operands[1] = prepared;
  operation = empty_operation(SIR_OP_ARRAY_APPEND_STRING);
  operation.effects = array_effects(operation.kind);
  operation.array_input_mode = mode;
  operation.operands = operands;
  operation.operand_count = 2;
  if (!sir_block_add_operation(&state->function, state->block, &operation)) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not append string-array append");
    return -1;
  }
  return 1;
}

static int resolved_callee(const lower_state *state, ast_t definition,
                           sir_id *signature_id) {
  size_t index;
  if (state->options == NULL || state->options->environment == NULL ||
      definition == NULL) {
    return 0;
  }
  for (index = 0; index < state->options->callee_count; index++) {
    if (state->options->callees[index].definition == definition) {
      *signature_id = state->options->callees[index].signature;
      return *signature_id < state->options->environment->signature_count;
    }
  }
  return 0;
}

static int intrinsic_callee(const lower_state *state, string_view name,
                            size_t arity, sir_id *signature_id) {
  const sir_intrinsic_descriptor *intrinsic;
  size_t index;
  if (state->options == NULL || state->options->environment == NULL)
    return 0;
  intrinsic = sir_intrinsic_lookup(name, arity);
  if (intrinsic == NULL)
    return 0;
  for (index = 0; index < state->options->environment->signature_count;
       index++) {
    const sir_signature *candidate =
        &state->options->environment->signatures[index];
    if (candidate->kind == SIR_CALLEE_INTRINSIC &&
        candidate->symbol_id == intrinsic->signature.symbol_id &&
        sir_intrinsic_signature_matches(candidate)) {
      *signature_id = (sir_id)index;
      return 1;
    }
  }
  return 0;
}

static int resolved_user_definition(ast_t definition) {
  return definition != NULL && definition->tag == fundef &&
         definition->data.fundef.body != NULL;
}

static sir_id lower_call(lower_state *state, ast_t expression) {
  ast_funcall *call;
  const sir_signature *signature;
  sir_id signature_id;
  sir_id *arguments = NULL;
  sir_id result_id = SIR_INVALID_ID;
  size_t index;
  sir_operation operation;
  sir_value value;

  if (expression == NULL || expression->tag != funcall) {
    lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                "semantic call lowering requires a resolved function call");
    return SIR_INVALID_ID;
  }
  call = &expression->data.funcall;
  if (call->args.length < 0) {
    lower_error(state, SIR_DIAGNOSTIC_CALL_ABI_MISMATCH,
                "selected call has an invalid argument list");
    return SIR_INVALID_ID;
  }
  if (!resolved_callee(state, call->resolved_target, &signature_id) &&
      (resolved_user_definition(call->resolved_target) ||
       !intrinsic_callee(state, call->name.lexeme, (size_t)call->args.length,
                         &signature_id))) {
    lower_error(state, SIR_DIAGNOSTIC_UNRESOLVED_CALLEE,
                "semantic call target is outside the selected user program");
    return SIR_INVALID_ID;
  }
  signature = &state->options->environment->signatures[signature_id];
  if ((signature->kind != SIR_CALLEE_USER &&
       signature->kind != SIR_CALLEE_INTRINSIC) ||
      (signature->effects.flags & SIR_EFFECT_NONRETURNING) != 0 ||
      signature->return_type.kind != SIR_TYPE_STRING ||
      signature->return_ownership != SIR_OWNERSHIP_OWNED ||
      (size_t)call->args.length != signature->parameter_count) {
    lower_error(state, SIR_DIAGNOSTIC_CALL_ABI_MISMATCH,
                "selected call does not match a returning string user ABI");
    return SIR_INVALID_ID;
  }
  if (signature->parameter_count != 0) {
    arguments = malloc(signature->parameter_count * sizeof(*arguments));
    if (arguments == NULL) {
      lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                  "could not allocate semantic call arguments");
      return SIR_INVALID_ID;
    }
  }
  for (index = 0; index < signature->parameter_count; index++) {
    const sir_signature_parameter *parameter = &signature->parameters[index];
    sir_id source = lower_expression(state, call->args.data[index]);
    if (source == SIR_INVALID_ID ||
        !sir_type_equal(state->function.values[source].type, parameter->type) ||
        state->function.values[source].representation !=
            parameter->representation) {
      lower_error(state, SIR_DIAGNOSTIC_CALL_ABI_MISMATCH,
                  "selected call argument does not match its ABI");
      goto failure;
    }
    if (parameter->mode == SIR_ARGUMENT_SCALAR &&
        state->function.values[source].ownership == SIR_OWNERSHIP_SCALAR) {
      arguments[index] = source;
      continue;
    }
    if (!((signature->kind == SIR_CALLEE_USER &&
           parameter->mode == SIR_ARGUMENT_CONSUME) ||
          (signature->kind == SIR_CALLEE_INTRINSIC &&
           parameter->mode == SIR_ARGUMENT_BORROW)) ||
        (state->function.values[source].ownership != SIR_OWNERSHIP_BORROWED &&
         state->function.values[source].ownership != SIR_OWNERSHIP_OWNED)) {
      lower_error(state, SIR_DIAGNOSTIC_CALL_ABI_MISMATCH,
                  "selected managed call argument is not consumable");
      goto failure;
    }
    value.type = parameter->type;
    value.representation = parameter->representation;
    value.ownership = SIR_OWNERSHIP_OWNED;
    arguments[index] = sir_function_add_value(&state->function, value);
    if (arguments[index] == SIR_INVALID_ID) {
      lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                  "could not allocate prepared call argument");
      goto failure;
    }
    operation = empty_operation(SIR_OP_PREPARE_ARGUMENT);
    operation.callee = signature_id;
    operation.parameter_index = index;
    operation.result = arguments[index];
    operation.operands = &source;
    operation.operand_count = 1;
    if (!sir_block_add_operation(&state->function, state->block, &operation)) {
      lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                  "could not append prepared call argument");
      goto failure;
    }
  }
  value.type = signature->return_type;
  value.representation = signature->return_representation;
  value.ownership = signature->return_ownership;
  result_id = sir_function_add_value(&state->function, value);
  if (result_id == SIR_INVALID_ID) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not allocate semantic call result");
    goto failure;
  }
  operation = empty_operation(SIR_OP_CALL);
  operation.effects = signature->effects;
  operation.callee = signature_id;
  operation.result = result_id;
  operation.operands = arguments;
  operation.operand_count = signature->parameter_count;
  if (!sir_block_add_operation(&state->function, state->block, &operation)) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not append semantic call");
    result_id = SIR_INVALID_ID;
  }

failure:
  free(arguments);
  return result_id;
}

static sir_id lower_expression(lower_state *state, ast_t expression) {
  if (expression == NULL) {
    lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                "semantic expression is missing");
    return SIR_INVALID_ID;
  }
  if (expression->tag == identifier &&
      (view_is(expression->data.identifier.id.lexeme, "true") ||
       view_is(expression->data.identifier.id.lexeme, "false")))
    return lower_bool_constant(state, expression);
  if (expression->tag == identifier)
    return lower_identifier(state, expression);
  if (expression->tag == literal &&
      expression->data.literal.lit.type == TOK_NUM_LIT)
    return lower_int_constant(state, expression);
  if (expression->tag == literal &&
      expression->data.literal.lit.type == TOK_ARR_DECL)
    return lower_array_new(state, expression);
  if (expression->tag == funcall &&
      unresolved_array_builtin(&expression->data.funcall, "get", 2))
    return lower_array_get(state, expression);
  if (expression->tag == funcall)
    return lower_call(state, expression);
  lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
              "expression has no primitive semantic lowering yet");
  return SIR_INVALID_ID;
}

static int lower_vardef(lower_state *state, ast_t statement) {
  sir_slot slot;
  sir_operation operation;
  sir_id source;
  sir_id slot_id;

  if (statement->data.vardef.is_rec ||
      !metadata_for_type(statement->data.vardef.type, &slot.type,
                         &slot.representation, &slot.ownership) ||
      (slot.type.kind != SIR_TYPE_STRING && slot.type.kind != SIR_TYPE_BOOL &&
       slot.type.kind != SIR_TYPE_ARRAY)) {
    lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                "representative lowerer requires an explicit builtin type");
    return 0;
  }
  if (slot.type.kind == SIR_TYPE_ARRAY &&
      (statement->data.vardef.expr == NULL ||
       statement->data.vardef.expr->tag != literal ||
       statement->data.vardef.expr->data.literal.lit.type != TOK_ARR_DECL)) {
    lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                "checkpoint arrays require direct dynamic construction");
    return 0;
  }
  source = lower_expression(state, statement->data.vardef.expr);
  if (source == SIR_INVALID_ID) {
    return 0;
  }
  if (!sir_type_equal(state->function.values[source].type, slot.type) ||
      state->function.values[source].representation != slot.representation) {
    lower_error(state, SIR_DIAGNOSTIC_TYPE_MISMATCH,
                "initializer type does not match its local slot");
    return 0;
  }
  slot.is_parameter = 0;
  slot.name = statement->data.vardef.name.lexeme;
  slot_id = sir_function_add_slot(&state->function, slot);
  if (slot_id == SIR_INVALID_ID ||
      !add_binding(state, statement->data.vardef.name.lexeme, slot_id)) {
    if (slot_id == SIR_INVALID_ID) {
      lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                  "could not allocate semantic slot");
    }
    return 0;
  }
  operation = empty_operation(SIR_OP_INIT_SLOT);
  operation.slot = slot_id;
  operation.operands = &source;
  operation.operand_count = 1;
  if (!sir_block_add_operation(&state->function, state->block, &operation)) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not append local initialization");
    return 0;
  }
  return 1;
}

static int lower_assignment(lower_state *state, ast_t statement) {
  size_t binding;
  sir_id source;
  sir_slot *slot;
  sir_operation operation;
  if (statement->data.assign.target != NULL &&
      statement->data.assign.target->tag == arr_index) {
    ast_arr_index *target = &statement->data.assign.target->data.arr_index;
    sir_id receiver;
    sir_id held_receiver;
    sir_id index;
    sir_id source;
    sir_id prepared;
    sir_id operands[3];
    sir_array_input_mode ignored;
    sir_array_input_mode mode;
    if (target->array == NULL || target->array->tag != identifier ||
        target->has_field || target->field_path.length != 0 ||
        target->field_expr != NULL) {
      lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                  "checkpoint indexed assignment requires a simple array");
      return 0;
    }
    receiver = lower_expression(state, target->array);
    if (receiver == SIR_INVALID_ID ||
        !string_array_value(&state->function.values[receiver])) {
      lower_error(state, SIR_DIAGNOSTIC_TYPE_MISMATCH,
                  "indexed assignment requires a dynamic string array");
      return 0;
    }
    held_receiver = prepare_owned(state, receiver, &ignored);
    index = lower_expression(state, target->index);
    if (held_receiver == SIR_INVALID_ID || index == SIR_INVALID_ID ||
        state->function.values[index].type.kind != SIR_TYPE_INT ||
        state->function.values[index].representation != SIR_REP_SCALAR) {
      lower_error(state, SIR_DIAGNOSTIC_TYPE_MISMATCH,
                  "indexed assignment index must be an int");
      return 0;
    }
    source = lower_expression(state, statement->data.assign.expr);
    if (source == SIR_INVALID_ID ||
        state->function.values[source].type.kind != SIR_TYPE_STRING ||
        state->function.values[source].representation !=
            SIR_REP_STRING_DESCRIPTOR) {
      lower_error(state, SIR_DIAGNOSTIC_TYPE_MISMATCH,
                  "indexed assignment value must be a string");
      return 0;
    }
    prepared = prepare_owned(state, source, &mode);
    if (prepared == SIR_INVALID_ID)
      return 0;
    operands[0] = held_receiver;
    operands[1] = index;
    operands[2] = prepared;
    operation = empty_operation(SIR_OP_ARRAY_REPLACE_STRING);
    operation.effects = array_effects(operation.kind);
    operation.array_input_mode = mode;
    operation.operands = operands;
    operation.operand_count = 3;
    if (!sir_block_add_operation(&state->function, state->block, &operation)) {
      lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                  "could not append indexed string-array assignment");
      return 0;
    }
    return 1;
  }
  if (statement->data.assign.target == NULL ||
      statement->data.assign.target->tag != identifier ||
      !binding_index(state,
                     statement->data.assign.target->data.identifier.id.lexeme,
                     &binding)) {
    lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                "checkpoint assignment requires a local identifier");
    return 0;
  }
  slot = &state->function.slots[state->bindings[binding].slot];
  source = lower_expression(state, statement->data.assign.expr);
  if (source == SIR_INVALID_ID)
    return 0;
  if (!sir_type_equal(state->function.values[source].type, slot->type) ||
      state->function.values[source].representation != slot->representation) {
    lower_error(state, SIR_DIAGNOSTIC_TYPE_MISMATCH,
                "assignment value does not match its slot");
    return 0;
  }
  operation = empty_operation(SIR_OP_ASSIGN_SLOT);
  operation.slot = state->bindings[binding].slot;
  operation.operands = &source;
  operation.operand_count = 1;
  if (!sir_block_add_operation(&state->function, state->block, &operation)) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not append slot assignment");
    return 0;
  }
  return 1;
}

static int append_end_slots(lower_state *state, size_t binding_base) {
  size_t index;
  for (index = state->binding_count; index > binding_base; index--) {
    sir_operation operation = empty_operation(SIR_OP_END_SLOT);
    operation.slot = state->bindings[index - 1].slot;
    if (!sir_block_add_operation(&state->function, state->block, &operation)) {
      lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                  "could not append lexical slot cleanup");
      return 0;
    }
  }
  return 1;
}

static int lower_return(lower_state *state, ast_t statement) {
  sir_operation operation;
  sir_id value;
  sir_id *cleanup_slots = NULL;
  size_t index;

  if (state->function.return_type.kind == SIR_TYPE_VOID) {
    if (statement->data.ret.expr != NULL) {
      lower_error(state, SIR_DIAGNOSTIC_TYPE_MISMATCH,
                  "void function cannot return a value");
      return 0;
    }
    operation = empty_operation(SIR_OP_RETURN);
  } else {
    value = lower_expression(state, statement->data.ret.expr);
    if (value == SIR_INVALID_ID) {
      return 0;
    }
    if (!sir_type_equal(state->function.values[value].type,
                        state->function.return_type) ||
        state->function.values[value].representation !=
            state->function.return_representation) {
      lower_error(state, SIR_DIAGNOSTIC_TYPE_MISMATCH,
                  "return value does not match the function result");
      return 0;
    }
    operation = empty_operation(SIR_OP_RETURN);
    operation.operands = &value;
    operation.operand_count = 1;
  }
  if (state->binding_count != 0) {
    cleanup_slots = malloc(state->binding_count * sizeof(*cleanup_slots));
    if (cleanup_slots == NULL) {
      lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                  "could not allocate return unwind list");
      return 0;
    }
    for (index = 0; index < state->binding_count; index++)
      cleanup_slots[index] =
          state->bindings[state->binding_count - index - 1].slot;
    operation.cleanup_slots = cleanup_slots;
    operation.cleanup_slot_count = state->binding_count;
  }
  if (!sir_block_add_operation(&state->function, state->block, &operation)) {
    free(cleanup_slots);
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not append return operation");
    return 0;
  }
  free(cleanup_slots);
  return 1;
}

static int lower_statement(lower_state *state, ast_t statement,
                           int *terminated);

static int lower_scoped_body(lower_state *state, ast_t body, int *terminated) {
  size_t binding_base = state->binding_count;
  size_t index;
  *terminated = 0;
  if (body == NULL) {
    lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                "if branch body is missing");
    return 0;
  }
  if (body->tag == compound) {
    for (index = 0; index < (size_t)body->data.compound.stmts.length; index++) {
      if (*terminated) {
        lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                    "statement after terminal return is not reachable");
        return 0;
      }
      if (!lower_statement(state, body->data.compound.stmts.data[index],
                           terminated))
        return 0;
    }
  } else if (!lower_statement(state, body, terminated)) {
    return 0;
  }
  if (!*terminated && !append_end_slots(state, binding_base))
    return 0;
  state->binding_count = binding_base;
  return 1;
}

static int append_jump(lower_state *state, sir_id block, sir_id target) {
  sir_operation jump = empty_operation(SIR_OP_JUMP);
  jump.targets[0] = target;
  jump.target_count = 1;
  if (!sir_block_add_operation(&state->function, block, &jump)) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not append semantic jump");
    return 0;
  }
  return 1;
}

static int lower_if(lower_state *state, ast_t statement, int *terminated) {
  sir_id condition;
  sir_id condition_block = state->block;
  sir_id true_block;
  sir_id false_block;
  sir_id true_end;
  sir_id false_end;
  sir_id join_block;
  sir_operation branch;
  int true_terminated;
  int false_terminated = 0;

  condition = lower_expression(state, statement->data.ifstmt.expression);
  if (condition == SIR_INVALID_ID ||
      state->function.values[condition].type.kind != SIR_TYPE_BOOL) {
    lower_error(state, SIR_DIAGNOSTIC_TYPE_MISMATCH,
                "if condition must be a bool value");
    return 0;
  }
  true_block = sir_function_add_block(&state->function);
  false_block = sir_function_add_block(&state->function);
  if (true_block == SIR_INVALID_ID || false_block == SIR_INVALID_ID) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not allocate if blocks");
    return 0;
  }
  branch = empty_operation(SIR_OP_BRANCH);
  branch.operands = &condition;
  branch.operand_count = 1;
  branch.targets[0] = true_block;
  branch.targets[1] = false_block;
  branch.target_count = 2;
  if (!sir_block_add_operation(&state->function, condition_block, &branch)) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not append semantic branch");
    return 0;
  }

  state->block = true_block;
  if (!lower_scoped_body(state, statement->data.ifstmt.body, &true_terminated))
    return 0;
  true_end = state->block;

  state->block = false_block;
  if (statement->data.ifstmt.elsestmt != NULL &&
      !lower_scoped_body(state, statement->data.ifstmt.elsestmt,
                         &false_terminated))
    return 0;
  false_end = state->block;

  if (true_terminated && false_terminated) {
    state->block = SIR_INVALID_ID;
    *terminated = 1;
    return 1;
  }
  join_block = sir_function_add_block(&state->function);
  if (join_block == SIR_INVALID_ID ||
      (!true_terminated && !append_jump(state, true_end, join_block)) ||
      (!false_terminated && !append_jump(state, false_end, join_block))) {
    if (join_block == SIR_INVALID_ID)
      lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                  "could not allocate if join block");
    return 0;
  }
  state->block = join_block;
  *terminated = 0;
  return 1;
}

static int lower_while(lower_state *state, ast_t statement, int *terminated) {
  sir_id header_block;
  sir_id body_block;
  sir_id exit_block;
  sir_id condition;
  sir_operation branch;
  int body_terminated;

  header_block = sir_function_add_block(&state->function);
  body_block = sir_function_add_block(&state->function);
  exit_block = sir_function_add_block(&state->function);
  if (header_block == SIR_INVALID_ID || body_block == SIR_INVALID_ID ||
      exit_block == SIR_INVALID_ID) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not allocate while blocks");
    return 0;
  }
  if (!append_jump(state, state->block, header_block))
    return 0;

  state->block = header_block;
  condition = lower_expression(state, statement->data.while_loop.condition);
  if (condition == SIR_INVALID_ID ||
      state->function.values[condition].type.kind != SIR_TYPE_BOOL ||
      state->function.values[condition].representation != SIR_REP_SCALAR ||
      state->function.values[condition].ownership != SIR_OWNERSHIP_SCALAR) {
    lower_error(state, SIR_DIAGNOSTIC_TYPE_MISMATCH,
                "while condition must be a bool scalar value");
    return 0;
  }
  branch = empty_operation(SIR_OP_BRANCH);
  branch.operands = &condition;
  branch.operand_count = 1;
  branch.targets[0] = body_block;
  branch.targets[1] = exit_block;
  branch.target_count = 2;
  if (!sir_block_add_operation(&state->function, header_block, &branch)) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not append while branch");
    return 0;
  }

  state->block = body_block;
  if (!lower_scoped_body(state, statement->data.while_loop.statement,
                         &body_terminated))
    return 0;
  if (!body_terminated && !append_jump(state, state->block, header_block))
    return 0;

  state->block = exit_block;
  *terminated = 0;
  return 1;
}

static int match_pattern_bool(ast_t pattern, int *value) {
  if (pattern == NULL || pattern->tag != identifier)
    return 0;
  if (view_is(pattern->data.identifier.id.lexeme, "true")) {
    *value = 1;
    return 1;
  }
  if (view_is(pattern->data.identifier.id.lexeme, "false")) {
    *value = 0;
    return 1;
  }
  return 0;
}

static int match_pattern_default(ast_t pattern) {
  return pattern != NULL && pattern->tag == literal &&
         pattern->data.literal.lit.type == TOK_WILDCARD;
}

static int append_match_slot_load(lower_state *state, sir_id slot,
                                  sir_id *result) {
  sir_value value;
  sir_operation operation;
  value.type = state->function.slots[slot].type;
  value.representation = state->function.slots[slot].representation;
  value.ownership = SIR_OWNERSHIP_SCALAR;
  *result = sir_function_add_value(&state->function, value);
  if (*result == SIR_INVALID_ID) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not allocate match test value");
    return 0;
  }
  operation = empty_operation(SIR_OP_LOAD_SLOT);
  operation.result = *result;
  operation.slot = slot;
  if (!sir_block_add_operation(&state->function, state->block, &operation)) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not append match test load");
    return 0;
  }
  return 1;
}

static int lower_match(lower_state *state, ast_t statement, int *terminated) {
  static char private_name[] = "@match-scrutinee";
  ast_match *selector = &statement->data.match;
  size_t binding_base = state->binding_count;
  size_t case_count;
  size_t index;
  sir_id scrutinee;
  sir_id scrutinee_slot;
  sir_id *continuing_blocks = NULL;
  size_t continuing_count = 0;
  int has_default = 0;
  sir_slot slot;
  sir_operation operation;

  if (selector->cases.length < 0) {
    lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                "match has an invalid arm list");
    return 0;
  }
  case_count = (size_t)selector->cases.length;
  for (index = 0; index < case_count; index++) {
    ast_t match_case = selector->cases.data[index];
    int pattern_value;
    if (match_case == NULL || match_case->tag != matchcase) {
      lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                  "match arm is malformed");
      return 0;
    }
    if (match_pattern_default(match_case->data.matchcase.expr)) {
      if (index + 1 != case_count) {
        lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                    "match default arm must be last");
        return 0;
      }
      has_default = 1;
    } else if (!match_pattern_bool(match_case->data.matchcase.expr,
                                   &pattern_value)) {
      lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                  "checkpoint match patterns must be true, false, or default");
      return 0;
    }
  }

  scrutinee = lower_expression(state, selector->expr);
  if (scrutinee == SIR_INVALID_ID ||
      state->function.values[scrutinee].type.kind != SIR_TYPE_BOOL ||
      state->function.values[scrutinee].representation != SIR_REP_SCALAR ||
      state->function.values[scrutinee].ownership != SIR_OWNERSHIP_SCALAR) {
    lower_error(state, SIR_DIAGNOSTIC_TYPE_MISMATCH,
                "match scrutinee must be a bool scalar value");
    return 0;
  }
  memset(&slot, 0, sizeof(slot));
  slot.name = (string_view){private_name, sizeof(private_name) - 1};
  slot.type = sir_builtin_type(SIR_TYPE_BOOL);
  slot.representation = SIR_REP_SCALAR;
  slot.ownership = SIR_OWNERSHIP_SCALAR;
  scrutinee_slot = sir_function_add_slot(&state->function, slot);
  if (scrutinee_slot == SIR_INVALID_ID ||
      !add_private_binding(state, slot.name, scrutinee_slot)) {
    if (scrutinee_slot == SIR_INVALID_ID) {
      lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                  "could not allocate match scrutinee slot");
    }
    return 0;
  }
  operation = empty_operation(SIR_OP_INIT_SLOT);
  operation.slot = scrutinee_slot;
  operation.operands = &scrutinee;
  operation.operand_count = 1;
  if (!sir_block_add_operation(&state->function, state->block, &operation)) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not capture match scrutinee");
    return 0;
  }

  continuing_blocks = malloc((case_count + 1) * sizeof(*continuing_blocks));
  if (continuing_blocks == NULL) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not allocate match continuation list");
    return 0;
  }
  for (index = 0; index < case_count; index++) {
    ast_t match_case = selector->cases.data[index];
    sir_id test_block = state->block;
    sir_id arm_block = sir_function_add_block(&state->function);
    sir_id next_block = SIR_INVALID_ID;
    int arm_terminated;
    int pattern_value;
    if (arm_block == SIR_INVALID_ID) {
      lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                  "could not allocate match arm block");
      goto failure;
    }
    if (match_pattern_default(match_case->data.matchcase.expr)) {
      sir_id condition;
      sir_operation branch;
      state->block = test_block;
      if (!append_match_slot_load(state, scrutinee_slot, &condition))
        goto failure;
      branch = empty_operation(SIR_OP_BRANCH);
      branch.operands = &condition;
      branch.operand_count = 1;
      branch.targets[0] = arm_block;
      branch.targets[1] = arm_block;
      branch.target_count = 2;
      if (!sir_block_add_operation(&state->function, test_block, &branch)) {
        lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                    "could not append exhaustive match branch");
        goto failure;
      }
    } else {
      sir_id condition;
      sir_operation branch;
      next_block = sir_function_add_block(&state->function);
      if (next_block == SIR_INVALID_ID) {
        lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                    "could not allocate next match test block");
        goto failure;
      }
      state->block = test_block;
      if (!append_match_slot_load(state, scrutinee_slot, &condition))
        goto failure;
      (void)match_pattern_bool(match_case->data.matchcase.expr,
                               &pattern_value);
      branch = empty_operation(SIR_OP_BRANCH);
      branch.operands = &condition;
      branch.operand_count = 1;
      branch.targets[pattern_value ? 0 : 1] = arm_block;
      branch.targets[pattern_value ? 1 : 0] = next_block;
      branch.target_count = 2;
      if (!sir_block_add_operation(&state->function, test_block, &branch)) {
        lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                    "could not append match branch");
        goto failure;
      }
    }

    state->block = arm_block;
    if (!lower_scoped_body(state, match_case->data.matchcase.body,
                           &arm_terminated))
      goto failure;
    if (!arm_terminated)
      continuing_blocks[continuing_count++] = state->block;
    state->block = next_block;
  }
  if (!has_default)
    continuing_blocks[continuing_count++] = state->block;

  if (continuing_count == 0) {
    state->binding_count = binding_base;
    state->block = SIR_INVALID_ID;
    *terminated = 1;
    free(continuing_blocks);
    return 1;
  }
  {
    sir_id join_block = sir_function_add_block(&state->function);
    if (join_block == SIR_INVALID_ID) {
      lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                  "could not allocate match join block");
      goto failure;
    }
    for (index = 0; index < continuing_count; index++) {
      state->block = continuing_blocks[index];
      operation = empty_operation(SIR_OP_END_SLOT);
      operation.slot = scrutinee_slot;
      if (!sir_block_add_operation(&state->function, state->block, &operation) ||
          !append_jump(state, state->block, join_block)) {
        if (state->diagnostic != NULL &&
            state->diagnostic->code == SIR_DIAGNOSTIC_NONE) {
          lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                      "could not end match scrutinee lifetime");
        }
        goto failure;
      }
    }
    state->block = join_block;
  }
  state->binding_count = binding_base;
  *terminated = 0;
  free(continuing_blocks);
  return 1;

failure:
  state->binding_count = binding_base;
  free(continuing_blocks);
  return 0;
}

static int lower_statement(lower_state *state, ast_t statement,
                           int *terminated) {
  *terminated = 0;
  if (statement != NULL && statement->tag == vardef)
    return lower_vardef(state, statement);
  if (statement != NULL && statement->tag == assign)
    return lower_assignment(state, statement);
  if (statement != NULL && statement->tag == ret) {
    if (!lower_return(state, statement))
      return 0;
    *terminated = 1;
    return 1;
  }
  if (statement != NULL && statement->tag == ifstmt)
    return lower_if(state, statement, terminated);
  if (statement != NULL && statement->tag == while_loop)
    return lower_while(state, statement, terminated);
  if (statement != NULL && statement->tag == match)
    return lower_match(state, statement, terminated);
  if (statement != NULL && statement->tag == funcall) {
    int array_append = lower_array_append(state, statement);
    sir_id discarded;
    sir_operation boundary;
    if (array_append != 0)
      return array_append > 0;
    discarded = unresolved_array_builtin(&statement->data.funcall, "get", 2)
                    ? lower_array_get(state, statement)
                    : lower_call(state, statement);
    if (discarded == SIR_INVALID_ID)
      return 0;
    boundary = empty_operation(SIR_OP_EXPRESSION_END);
    boundary.operands = &discarded;
    boundary.operand_count = 1;
    if (!sir_block_add_operation(&state->function, state->block, &boundary)) {
      lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                  "could not append full-expression boundary");
      return 0;
    }
    return 1;
  }
  lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
              "AST node has no primitive semantic lowering yet");
  return 0;
}

sir_lower_options sir_lower_default_options(void) {
  sir_lower_options options;
  memset(&options, 0, sizeof(options));
  return options;
}

int sir_lower_signature(ast_t function_ast, uint32_t symbol_id,
                        string_view c_symbol, sir_signature *signature,
                        sir_diagnostic *diagnostic) {
  ast_fundef *definition;
  sir_signature_parameter *parameters = NULL;
  size_t index;
  if (diagnostic != NULL)
    memset(diagnostic, 0, sizeof(*diagnostic));
  if (signature == NULL || function_ast == NULL ||
      function_ast->tag != fundef ||
      function_ast->data.fundef.method_kind != METHOD_NONE ||
      function_ast->data.fundef.body == NULL || c_symbol.data == NULL ||
      c_symbol.length == 0) {
    if (diagnostic != NULL) {
      diagnostic->code = SIR_DIAGNOSTIC_UNSUPPORTED_AST;
      diagnostic->message =
          "semantic signature requires a selected user function";
    }
    return 0;
  }
  definition = &function_ast->data.fundef;
  memset(signature, 0, sizeof(*signature));
  if (!metadata_for_type(definition->ret_type, &signature->return_type,
                         &signature->return_representation,
                         &signature->return_ownership) ||
      signature->return_type.kind != SIR_TYPE_STRING ||
      signature->return_ownership != SIR_OWNERSHIP_OWNED ||
      definition->args.length < 0 || definition->types.length < 0 ||
      definition->args.length != definition->types.length) {
    if (diagnostic != NULL) {
      diagnostic->code = SIR_DIAGNOSTIC_TYPE_MISMATCH;
      diagnostic->message =
          "checkpoint call signatures require owned string results";
    }
    return 0;
  }
  if (definition->types.length != 0) {
    parameters = calloc((size_t)definition->types.length, sizeof(*parameters));
    if (parameters == NULL) {
      if (diagnostic != NULL) {
        diagnostic->code = SIR_DIAGNOSTIC_ALLOCATION_FAILED;
        diagnostic->message =
            "could not allocate semantic signature parameters";
      }
      return 0;
    }
  }
  for (index = 0; index < (size_t)definition->types.length; index++) {
    if (!metadata_for_type(
            definition->types.data[index], &parameters[index].type,
            &parameters[index].representation, &parameters[index].ownership) ||
        (parameters[index].type.kind != SIR_TYPE_STRING &&
         parameters[index].type.kind != SIR_TYPE_BOOL &&
         parameters[index].type.kind != SIR_TYPE_ARRAY)) {
      free(parameters);
      if (diagnostic != NULL) {
        diagnostic->code = SIR_DIAGNOSTIC_TYPE_MISMATCH;
        diagnostic->message =
            "checkpoint call parameters require string, bool, or string[] values";
      }
      return 0;
    }
    parameters[index].mode = parameters[index].type.kind == SIR_TYPE_BOOL
                                 ? SIR_ARGUMENT_SCALAR
                                 : SIR_ARGUMENT_CONSUME;
  }
  signature->kind = SIR_CALLEE_USER;
  signature->symbol_id = symbol_id;
  signature->c_symbol = c_symbol;
  signature->parameters = parameters;
  signature->parameter_count = (size_t)definition->types.length;
  signature->effects = sir_effects_none();
  signature->effects.flags =
      SIR_EFFECT_CALL | SIR_EFFECT_ALLOCATE | SIR_EFFECT_COLLECT |
      SIR_EFFECT_TERMINATE;
  signature->effects.mutation = SIR_MUTATION_ARGUMENT;
  signature->call_abi = SIR_CALL_ABI_C_RETURN_VALUE;
  return 1;
}

void sir_lower_signature_destroy(sir_signature *signature) {
  if (signature == NULL)
    return;
  free((void *)signature->parameters);
  memset(signature, 0, sizeof(*signature));
}

sir_lower_result sir_lower_function(ast_t function_ast,
                                    const sir_lower_options *options,
                                    sir_function *function,
                                    sir_diagnostic *diagnostic) {
  lower_state state;
  ast_fundef *definition;
  size_t index;
  int terminated = 0;

  if (options == NULL || !options->enabled) {
    return SIR_LOWER_SKIPPED;
  }
  memset(&state, 0, sizeof(state));
  sir_function_init(&state.function);
  state.block = SIR_INVALID_ID;
  state.options = options;
  state.diagnostic = diagnostic;
  if (diagnostic != NULL) {
    memset(diagnostic, 0, sizeof(*diagnostic));
  }
  if (function_ast == NULL || function_ast->tag != fundef || function == NULL) {
    lower_error(&state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                "semantic lowering requires a function definition");
    goto failure;
  }
  definition = &function_ast->data.fundef;
  state.function.name = definition->name.lexeme;
  state.function.external_symbol = definition->name.lexeme;
  state.function.c_symbol = definition->name.lexeme;
  if (!metadata_for_type(definition->ret_type, &state.function.return_type,
                         &state.function.return_representation,
                         &state.function.return_ownership)) {
    lower_error(&state, SIR_DIAGNOSTIC_UNKNOWN_TYPE,
                "function result type is not resolved by this checkpoint");
    goto failure;
  }
  if (definition->types.length < 0 || definition->args.length < 0 ||
      definition->types.length != definition->args.length) {
    lower_error(&state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                "function parameter names and types do not agree");
    goto failure;
  }
  for (index = 0; index < (size_t)definition->args.length; index++) {
    sir_slot slot;
    sir_id slot_id;
    if (!metadata_for_type(definition->types.data[index], &slot.type,
                           &slot.representation, &slot.ownership) ||
        (slot.type.kind != SIR_TYPE_STRING &&
         slot.type.kind != SIR_TYPE_BOOL &&
         slot.type.kind != SIR_TYPE_ARRAY)) {
      lower_error(&state, SIR_DIAGNOSTIC_UNKNOWN_TYPE,
                  "parameter type is not resolved by this checkpoint");
      goto failure;
    }
    slot.is_parameter = 1;
    slot.name = definition->args.data[index].lexeme;
    slot_id = sir_function_add_slot(&state.function, slot);
    if (slot_id == SIR_INVALID_ID ||
        !add_binding(&state, definition->args.data[index].lexeme, slot_id)) {
      if (slot_id == SIR_INVALID_ID) {
        lower_error(&state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                    "could not allocate parameter slot");
      }
      goto failure;
    }
  }
  state.block = sir_function_add_block(&state.function);
  state.function.entry_block = state.block;
  if (state.block == SIR_INVALID_ID || definition->body == NULL ||
      definition->body->tag != compound) {
    lower_error(&state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                "representative lowerer requires a compound body");
    goto failure;
  }
  for (index = 0; index < (size_t)definition->body->data.compound.stmts.length;
       index++) {
    ast_t statement = definition->body->data.compound.stmts.data[index];
    if (terminated) {
      lower_error(&state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                  "statement after terminal return is not reachable");
      goto failure;
    }
    if (!lower_statement(&state, statement, &terminated))
      goto failure;
  }
  if (!terminated) {
    lower_error(&state, SIR_DIAGNOSTIC_INVALID_TERMINATOR,
                "representative function path must end in return");
    goto failure;
  }
  if (!sir_verify_function(&state.function, options->environment, diagnostic)) {
    goto failure;
  }
  free(state.bindings);
  *function = state.function;
  return SIR_LOWER_OK;

failure:
  free(state.bindings);
  sir_function_destroy(&state.function);
  return SIR_LOWER_ERROR;
}
