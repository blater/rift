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
  operation.parameter_index = SIZE_MAX;
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

static sir_id lower_expression(lower_state *state, ast_t expression);

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
  if (!resolved_callee(state, call->resolved_target, &signature_id)) {
    lower_error(state, SIR_DIAGNOSTIC_UNRESOLVED_CALLEE,
                "semantic call target is outside the selected user program");
    return SIR_INVALID_ID;
  }
  signature = &state->options->environment->signatures[signature_id];
  if (signature->kind != SIR_CALLEE_USER ||
      (signature->effects.flags & SIR_EFFECT_NONRETURNING) != 0 ||
      signature->return_type.kind != SIR_TYPE_STRING ||
      signature->return_ownership != SIR_OWNERSHIP_OWNED ||
      call->args.length < 0 ||
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
    if (source == SIR_INVALID_ID || parameter->mode != SIR_ARGUMENT_CONSUME ||
        !sir_type_equal(state->function.values[source].type, parameter->type) ||
        state->function.values[source].representation !=
            parameter->representation ||
        (state->function.values[source].ownership != SIR_OWNERSHIP_BORROWED &&
         state->function.values[source].ownership != SIR_OWNERSHIP_OWNED)) {
      lower_error(state, SIR_DIAGNOSTIC_CALL_ABI_MISMATCH,
                  "selected call argument does not match its consuming ABI");
      goto failure;
    }
    value.type = parameter->type;
    value.representation = parameter->representation;
    value.ownership = parameter->ownership;
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
  if (expression->tag == identifier)
    return lower_identifier(state, expression);
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
                         &slot.representation, &slot.ownership)) {
    lower_error(state, SIR_DIAGNOSTIC_UNSUPPORTED_AST,
                "representative lowerer requires an explicit builtin type");
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
  if (!sir_block_add_operation(&state->function, state->block, &operation)) {
    lower_error(state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                "could not append return operation");
    return 0;
  }
  return 1;
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
        parameters[index].type.kind != SIR_TYPE_STRING ||
        parameters[index].ownership != SIR_OWNERSHIP_OWNED) {
      free(parameters);
      if (diagnostic != NULL) {
        diagnostic->code = SIR_DIAGNOSTIC_TYPE_MISMATCH;
        diagnostic->message =
            "checkpoint call parameters require owned strings";
      }
      return 0;
    }
    parameters[index].mode = SIR_ARGUMENT_CONSUME;
  }
  signature->kind = SIR_CALLEE_USER;
  signature->symbol_id = symbol_id;
  signature->c_symbol = c_symbol;
  signature->parameters = parameters;
  signature->parameter_count = (size_t)definition->types.length;
  signature->effects = sir_effects_none();
  signature->effects.flags = SIR_EFFECT_CALL;
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
  int saw_return = 0;

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
                           &slot.representation, &slot.ownership)) {
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
    } else if (statement != NULL && statement->tag == funcall) {
      sir_id discarded = lower_call(&state, statement);
      sir_operation boundary;
      if (discarded == SIR_INVALID_ID)
        goto failure;
      boundary = empty_operation(SIR_OP_EXPRESSION_END);
      boundary.operands = &discarded;
      boundary.operand_count = 1;
      if (!sir_block_add_operation(&state.function, state.block, &boundary)) {
        lower_error(&state, SIR_DIAGNOSTIC_ALLOCATION_FAILED,
                    "could not append full-expression boundary");
        goto failure;
      }
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
