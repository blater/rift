#include "semantic_ir/lower.h"

#include <stdlib.h>
#include <string.h>

typedef struct lower_binding {
  string_view name;
  sir_id slot;
} lower_binding;

typedef struct lower_state {
  sir_function function;
  lower_binding *bindings;
  size_t binding_count;
  size_t binding_capacity;
  sir_id block;
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
  if (type_ast == NULL || type_ast->tag != type ||
      type_ast->data.type.is_array) {
    return 0;
  }
  name = type_ast->data.type.name.lexeme;
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

static int lower_vardef(lower_state *state, ast_t statement) {
  sir_slot slot;
  sir_operation operation;
  sir_id source;
  sir_id slot_id;

  if (statement->data.vardef.is_rec ||
      !metadata_for_type(statement->data.vardef.type, &slot.type,
                         &slot.representation, &slot.ownership)) {
    lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                "representative lowerer requires an explicit builtin type");
    return 0;
  }
  source = lower_identifier(state, statement->data.vardef.expr);
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

static int lower_return(lower_state *state, ast_t statement) {
  sir_operation operation;
  sir_id value;

  if (state->function.return_type.kind == SIR_TYPE_VOID) {
    if (statement->data.ret.expr != NULL) {
      lower_error(state, SIR_DIAGNOSTIC_TYPE_MISMATCH,
                  "void function cannot return a value");
      return 0;
    }
    operation = empty_operation(SIR_OP_RETURN);
  } else {
    value = lower_identifier(state, statement->data.ret.expr);
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
  if (!sir_block_add_operation(&state->function, state->block, &operation)) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not append return operation");
    return 0;
  }
  return 1;
}

sir_lower_options sir_lower_default_options(void) {
  sir_lower_options options;
  options.enabled = 0;
  return options;
}

sir_lower_result sir_lower_function(ast_t function_ast,
                                    const sir_lower_options *options,
                                    sir_function *function,
                                    sir_diagnostic *diagnostic) {
  lower_state state;
  ast_fundef *definition;
  size_t index;
  int saw_return = 0;

  if (options == NULL || !options->enabled) {
    return SIR_LOWER_SKIPPED;
  }
  memset(&state, 0, sizeof(state));
  sir_function_init(&state.function);
  state.block = SIR_INVALID_ID;
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
                           &slot.representation, &slot.ownership)) {
      lower_error(&state, SIR_DIAGNOSTIC_UNKNOWN_TYPE,
                  "parameter type is not resolved by this checkpoint");
      goto failure;
    }
    slot.is_parameter = 1;
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
    if (saw_return) {
      lower_error(&state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                  "statement after terminal return is not reachable");
      goto failure;
    }
    if (statement != NULL && statement->tag == vardef) {
      if (!lower_vardef(&state, statement)) {
        goto failure;
      }
    } else if (statement != NULL && statement->tag == ret) {
      if (!lower_return(&state, statement)) {
        goto failure;
      }
      saw_return = 1;
    } else {
      lower_error(&state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                  "AST node has no primitive semantic lowering yet");
      goto failure;
    }
  }
  if (!saw_return) {
    lower_error(&state, SIR_DIAGNOSTIC_INVALID_TERMINATOR,
                "representative function path must end in return");
    goto failure;
  }
  if (!sir_verify_function(&state.function, NULL, diagnostic)) {
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
