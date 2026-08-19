#include "ownership_plan/internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct ownership_flow_state {
  ownership_token_state *states;
  ownership_id *provenance;
} ownership_flow_state;

static void set_diagnostic(ownership_diagnostic *diagnostic,
                           ownership_diagnostic_code code, size_t block_index,
                           size_t operation_index, ownership_id token,
                           const char *message) {
  if (diagnostic == NULL) {
    return;
  }
  diagnostic->code = code;
  diagnostic->block_index = block_index;
  diagnostic->operation_index = operation_index;
  diagnostic->token = token;
  diagnostic->message = message;
}

static int grow_array(void **data, size_t *capacity, size_t item_size,
                      size_t required) {
  size_t next_capacity;
  void *next;
  if (*capacity >= required) {
    return 1;
  }
  next_capacity = *capacity == 0 ? 4 : *capacity;
  while (next_capacity < required) {
    if (next_capacity > SIZE_MAX / 2) {
      return 0;
    }
    next_capacity *= 2;
  }
  if (item_size != 0 && next_capacity > SIZE_MAX / item_size) {
    return 0;
  }
  next = realloc(*data, next_capacity * item_size);
  if (next == NULL) {
    return 0;
  }
  *data = next;
  *capacity = next_capacity;
  return 1;
}

static ownership_operation empty_operation(ownership_op_kind kind) {
  ownership_operation operation;
  memset(&operation, 0, sizeof(operation));
  operation.kind = kind;
  operation.result = OWNERSHIP_INVALID_ID;
  operation.operand = OWNERSHIP_INVALID_ID;
  operation.callee = OWNERSHIP_INVALID_ID;
  operation.parameter_index = SIZE_MAX;
  operation.targets[0] = OWNERSHIP_INVALID_ID;
  operation.targets[1] = OWNERSHIP_INVALID_ID;
  return operation;
}

static int is_terminator(ownership_op_kind kind) {
  return kind == OWNERSHIP_OP_JUMP || kind == OWNERSHIP_OP_BRANCH ||
         kind == OWNERSHIP_OP_RETURN;
}

static int operation_defines_token(ownership_op_kind kind) {
  return kind == OWNERSHIP_OP_ACQUIRE || kind == OWNERSHIP_OP_ADOPT ||
         kind == OWNERSHIP_OP_DEFINE_SCALAR ||
         kind == OWNERSHIP_OP_COPY_SCALAR || kind == OWNERSHIP_OP_BORROW ||
         kind == OWNERSHIP_OP_HOLD || kind == OWNERSHIP_OP_MOVE ||
         kind == OWNERSHIP_OP_CALL;
}

static int static_operation_contract(const ownership_operation *operation) {
  switch (operation->kind) {
  case OWNERSHIP_OP_ACQUIRE:
  case OWNERSHIP_OP_ADOPT:
  case OWNERSHIP_OP_DEFINE_SCALAR:
    return operation->result != OWNERSHIP_INVALID_ID &&
           operation->operand == OWNERSHIP_INVALID_ID &&
           operation->callee == OWNERSHIP_INVALID_ID &&
           operation->parameter_index == SIZE_MAX &&
           operation->operand_count == 0 && operation->target_count == 0;
  case OWNERSHIP_OP_COPY_SCALAR:
  case OWNERSHIP_OP_BORROW:
    return operation->result != OWNERSHIP_INVALID_ID &&
           operation->operand != OWNERSHIP_INVALID_ID &&
           operation->callee == OWNERSHIP_INVALID_ID &&
           operation->parameter_index == SIZE_MAX &&
           operation->operand_count == 0 && operation->target_count == 0;
  case OWNERSHIP_OP_HOLD:
  case OWNERSHIP_OP_MOVE:
    return operation->result != OWNERSHIP_INVALID_ID &&
           operation->operand != OWNERSHIP_INVALID_ID &&
           ((operation->callee == OWNERSHIP_INVALID_ID &&
             operation->parameter_index == SIZE_MAX) ||
            (operation->callee != OWNERSHIP_INVALID_ID &&
             operation->parameter_index != SIZE_MAX)) &&
           operation->operand_count == 0 && operation->target_count == 0;
  case OWNERSHIP_OP_RELEASE:
  case OWNERSHIP_OP_END_BORROW:
    return operation->result == OWNERSHIP_INVALID_ID &&
           operation->operand != OWNERSHIP_INVALID_ID &&
           operation->callee == OWNERSHIP_INVALID_ID &&
           operation->parameter_index == SIZE_MAX &&
           operation->operand_count == 0 && operation->target_count == 0;
  case OWNERSHIP_OP_CALL:
    return operation->result != OWNERSHIP_INVALID_ID &&
           operation->operand == OWNERSHIP_INVALID_ID &&
           operation->callee != OWNERSHIP_INVALID_ID &&
           operation->parameter_index == SIZE_MAX &&
           (operation->operand_count == 0 || operation->operands != NULL) &&
           operation->target_count == 0;
  case OWNERSHIP_OP_JUMP:
    return operation->result == OWNERSHIP_INVALID_ID &&
           operation->operand == OWNERSHIP_INVALID_ID &&
           operation->target_count == 1;
  case OWNERSHIP_OP_BRANCH:
    return operation->result == OWNERSHIP_INVALID_ID &&
           operation->operand != OWNERSHIP_INVALID_ID &&
           operation->target_count == 2;
  case OWNERSHIP_OP_RETURN:
    return operation->result == OWNERSHIP_INVALID_ID &&
           operation->target_count == 0;
  case OWNERSHIP_OP_UNKNOWN:
    return 0;
  }
  return 0;
}

static int token_metadata_valid(const ownership_token *token) {
  if (token->origin_kind == OWNERSHIP_ORIGIN_UNKNOWN ||
      (token->origin_kind != OWNERSHIP_ORIGIN_SYNTHETIC &&
       token->origin == OWNERSHIP_INVALID_ID)) {
    return 0;
  }
  switch (token->kind) {
  case OWNERSHIP_TOKEN_SCALAR:
    return token->type.nominal_id == 0 &&
           token->representation == SIR_REP_SCALAR &&
           token->type.kind >= SIR_TYPE_BOOL &&
           token->type.kind <= SIR_TYPE_CHAR;
  case OWNERSHIP_TOKEN_MANAGED:
    return (token->type.nominal_id == 0 &&
            token->representation == SIR_REP_STRING_DESCRIPTOR &&
            token->type.kind == SIR_TYPE_STRING) ||
           (token->type.nominal_id != 0 &&
            token->representation == SIR_REP_MANAGED_HANDLE &&
            (token->type.kind == SIR_TYPE_ARRAY ||
             token->type.kind == SIR_TYPE_RECORD));
  case OWNERSHIP_TOKEN_EXTERNAL:
    return token->type.nominal_id != 0 &&
           token->representation == SIR_REP_EXTERNAL_TOKEN &&
           token->type.kind == SIR_TYPE_EXTERNAL;
  case OWNERSHIP_TOKEN_UNKNOWN:
    return 0;
  }
  return 0;
}

static int token_metadata_equal(const ownership_token *left,
                                const ownership_token *right) {
  return left->kind == right->kind &&
         left->representation == right->representation &&
         sir_type_equal(left->type, right->type);
}

void ownership_plan_internal_init(ownership_plan *plan) {
  memset(plan, 0, sizeof(*plan));
  plan->entry_block = OWNERSHIP_INVALID_ID;
}

void ownership_plan_internal_destroy(ownership_plan *plan) {
  size_t block_index;
  size_t slot_index;
  if (plan == NULL) {
    return;
  }
  for (block_index = 0; block_index < plan->block_count; block_index++) {
    size_t operation_index;
    for (operation_index = 0;
         operation_index < plan->blocks[block_index].operation_count;
         operation_index++) {
      free(plan->blocks[block_index].operations[operation_index].operands);
    }
    free(plan->blocks[block_index].operations);
  }
  for (slot_index = 0; slot_index < plan->slot_count; slot_index++) {
    free(plan->slots[slot_index].name);
  }
  free(plan->function_name);
  free(plan->function_external_symbol);
  free(plan->function_c_symbol);
  for (slot_index = 0; slot_index < plan->callee_count; slot_index++)
    free(plan->callee_symbols[slot_index]);
  free(plan->callee_symbols);
  free(plan->slots);
  free(plan->blocks);
  free(plan->tokens);
  ownership_plan_internal_init(plan);
}

ownership_id ownership_plan_internal_add_token(ownership_plan *plan,
                                               ownership_token token) {
  ownership_id result;
  if (plan->sealed ||
      !grow_array((void **)&plan->tokens, &plan->token_capacity,
                  sizeof(*plan->tokens), plan->token_count + 1) ||
      plan->token_count >= OWNERSHIP_INVALID_ID) {
    return OWNERSHIP_INVALID_ID;
  }
  result = (ownership_id)plan->token_count;
  plan->tokens[plan->token_count++] = token;
  return result;
}

ownership_id ownership_plan_internal_add_block(ownership_plan *plan) {
  ownership_id result;
  ownership_block block;
  memset(&block, 0, sizeof(block));
  if (plan->sealed ||
      !grow_array((void **)&plan->blocks, &plan->block_capacity,
                  sizeof(*plan->blocks), plan->block_count + 1) ||
      plan->block_count >= OWNERSHIP_INVALID_ID) {
    return OWNERSHIP_INVALID_ID;
  }
  result = (ownership_id)plan->block_count;
  plan->blocks[plan->block_count++] = block;
  return result;
}

int ownership_plan_internal_add_operation(ownership_plan *plan,
                                          ownership_id block_id,
                                          ownership_operation operation) {
  ownership_block *block;
  ownership_operation copy;
  if (plan->sealed || block_id >= plan->block_count) {
    return 0;
  }
  block = &plan->blocks[block_id];
  if (!grow_array((void **)&block->operations, &block->operation_capacity,
                  sizeof(*block->operations), block->operation_count + 1)) {
    return 0;
  }
  copy = operation;
  copy.operands = NULL;
  if (operation.operand_count != 0) {
    if (operation.operands == NULL ||
        operation.operand_count > SIZE_MAX / sizeof(*copy.operands)) {
      return 0;
    }
    copy.operands = malloc(operation.operand_count * sizeof(*copy.operands));
    if (copy.operands == NULL)
      return 0;
    memcpy(copy.operands, operation.operands,
           operation.operand_count * sizeof(*copy.operands));
  }
  block->operations[block->operation_count++] = copy;
  return 1;
}

static ownership_token token_from_value(const sir_value *value,
                                        ownership_token_origin_kind origin_kind,
                                        ownership_id origin) {
  ownership_token token;
  token.type = value->type;
  token.representation = value->representation;
  if (value->representation == SIR_REP_SCALAR) {
    token.kind = OWNERSHIP_TOKEN_SCALAR;
  } else if (value->representation == SIR_REP_EXTERNAL_TOKEN) {
    token.kind = OWNERSHIP_TOKEN_EXTERNAL;
  } else if (value->representation == SIR_REP_STRING_DESCRIPTOR ||
             value->representation == SIR_REP_MANAGED_HANDLE) {
    token.kind = OWNERSHIP_TOKEN_MANAGED;
  } else {
    token.kind = OWNERSHIP_TOKEN_UNKNOWN;
  }
  token.origin_kind = origin_kind;
  token.origin = origin;
  return token;
}

static ownership_token token_from_slot(const sir_slot *slot,
                                       ownership_id slot_id) {
  sir_value value;
  value.type = slot->type;
  value.representation = slot->representation;
  value.ownership = slot->ownership;
  return token_from_value(&value, OWNERSHIP_ORIGIN_SLOT, slot_id);
}

static char *copy_name(string_view name) {
  char *copy;
  if ((name.data == NULL && name.length != 0) || name.length == SIZE_MAX) {
    return NULL;
  }
  copy = malloc(name.length + 1);
  if (copy == NULL) {
    return NULL;
  }
  if (name.length != 0) {
    memcpy(copy, name.data, name.length);
  }
  copy[name.length] = '\0';
  return copy;
}

static int copy_declarations(const sir_function *function, ownership_plan *plan,
                             ownership_diagnostic *diagnostic) {
  size_t index;
  plan->function_name = copy_name(function->name);
  plan->function_external_symbol = copy_name(function->external_symbol);
  plan->function_c_symbol = copy_name(function->c_symbol);
  plan->return_type = function->return_type;
  plan->return_representation = function->return_representation;
  plan->return_ownership = function->return_ownership;
  if (plan->function_name == NULL || plan->function_external_symbol == NULL ||
      plan->function_c_symbol == NULL) {
    set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_ALLOCATION_FAILED, 0, 0,
                   OWNERSHIP_INVALID_ID,
                   "could not copy ownership function declaration");
    return 0;
  }
  if (function->slot_count == 0) {
    return 1;
  }
  plan->slots = calloc(function->slot_count, sizeof(*plan->slots));
  if (plan->slots == NULL) {
    set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_ALLOCATION_FAILED, 0, 0,
                   OWNERSHIP_INVALID_ID,
                   "could not allocate ownership slot declarations");
    return 0;
  }
  plan->slot_count = function->slot_count;
  for (index = 0; index < function->slot_count; index++) {
    ownership_slot *slot = &plan->slots[index];
    slot->name = copy_name(function->slots[index].name);
    slot->type = function->slots[index].type;
    slot->representation = function->slots[index].representation;
    slot->ownership = function->slots[index].ownership;
    slot->is_parameter = function->slots[index].is_parameter;
    slot->token = OWNERSHIP_INVALID_ID;
    if (slot->name == NULL) {
      set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_ALLOCATION_FAILED, 0, 0,
                     OWNERSHIP_INVALID_ID,
                     "could not copy ownership slot declaration");
      return 0;
    }
  }
  return 1;
}

static int copy_callees(const sir_environment *environment,
                        ownership_plan *plan,
                        ownership_diagnostic *diagnostic) {
  size_t index;
  if (environment == NULL || environment->signature_count == 0)
    return 1;
  plan->callee_symbols =
      calloc(environment->signature_count, sizeof(*plan->callee_symbols));
  if (plan->callee_symbols == NULL) {
    set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_ALLOCATION_FAILED, 0, 0,
                   OWNERSHIP_INVALID_ID,
                   "could not allocate ownership callee declarations");
    return 0;
  }
  plan->callee_count = environment->signature_count;
  for (index = 0; index < environment->signature_count; index++) {
    plan->callee_symbols[index] =
        copy_name(environment->signatures[index].c_symbol);
    if (plan->callee_symbols[index] == NULL) {
      set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_ALLOCATION_FAILED, 0, 0,
                     OWNERSHIP_INVALID_ID,
                     "could not copy ownership callee declaration");
      return 0;
    }
  }
  return 1;
}

static int append_or_fail(ownership_plan *plan, ownership_id block,
                          ownership_operation operation,
                          ownership_diagnostic *diagnostic) {
  if (ownership_plan_internal_add_operation(plan, block, operation)) {
    return 1;
  }
  set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_ALLOCATION_FAILED,
                 (size_t)block, 0, OWNERSHIP_INVALID_ID,
                 "could not append ownership operation");
  return 0;
}

static ownership_id add_token_or_fail(ownership_plan *plan,
                                      ownership_token token,
                                      ownership_diagnostic *diagnostic) {
  ownership_id result = ownership_plan_internal_add_token(plan, token);
  if (result == OWNERSHIP_INVALID_ID) {
    set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_ALLOCATION_FAILED, 0, 0,
                   OWNERSHIP_INVALID_ID, "could not allocate ownership token");
  }
  return result;
}

static int append_parameter_initializers(const sir_function *function,
                                         ownership_plan *plan,
                                         ownership_id block,
                                         const ownership_id *slot_tokens,
                                         ownership_diagnostic *diagnostic) {
  size_t index;
  for (index = 0; index < function->slot_count; index++) {
    ownership_operation operation;
    if (!function->slots[index].is_parameter) {
      continue;
    }
    operation =
        empty_operation(function->slots[index].representation == SIR_REP_SCALAR
                            ? OWNERSHIP_OP_DEFINE_SCALAR
                            : OWNERSHIP_OP_ADOPT);
    operation.result = slot_tokens[index];
    if (!append_or_fail(plan, block, operation, diagnostic)) {
      return 0;
    }
  }
  return 1;
}

static int append_cleanup(const sir_function *function, ownership_plan *plan,
                          ownership_id block, const ownership_id *slot_tokens,
                          ownership_id return_token,
                          ownership_diagnostic *diagnostic) {
  size_t index;
  for (index = function->slot_count; index != 0; index--) {
    ownership_id token = slot_tokens[index - 1];
    ownership_operation operation;
    if (function->slots[index - 1].representation == SIR_REP_SCALAR ||
        token == return_token) {
      continue;
    }
    operation = empty_operation(OWNERSHIP_OP_RELEASE);
    operation.operand = token;
    if (!append_or_fail(plan, block, operation, diagnostic)) {
      return 0;
    }
  }
  return 1;
}

ownership_plan *ownership_plan_build(const sir_function *function,
                                     const sir_environment *environment,
                                     ownership_diagnostic *diagnostic) {
  ownership_plan *built = NULL;
  ownership_id *slot_tokens = NULL;
  ownership_id *value_tokens = NULL;
  ownership_id block;
  size_t index;
  sir_diagnostic semantic_diagnostic;

  set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_NONE, 0, 0,
                 OWNERSHIP_INVALID_ID, NULL);
  if (!sir_verify_function(function, environment, &semantic_diagnostic)) {
    set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_SOURCE_IR,
                   semantic_diagnostic.block_index,
                   semantic_diagnostic.operation_index, OWNERSHIP_INVALID_ID,
                   "ownership planning requires verified semantic IR");
    return NULL;
  }
  if (function->block_count != 1) {
    set_diagnostic(
        diagnostic, OWNERSHIP_DIAGNOSTIC_UNSUPPORTED_SOURCE_IR, 0, 0,
        OWNERSHIP_INVALID_ID,
        "checkpoint planner accepts one complete straight-line path");
    return NULL;
  }
  built = malloc(sizeof(*built));
  if (built == NULL) {
    set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_ALLOCATION_FAILED, 0, 0,
                   OWNERSHIP_INVALID_ID, "could not allocate ownership plan");
    return NULL;
  }
  ownership_plan_internal_init(built);
  if (!copy_declarations(function, built, diagnostic)) {
    goto failure;
  }
  if (!copy_callees(environment, built, diagnostic)) {
    goto failure;
  }
  slot_tokens = malloc((function->slot_count == 0 ? 1 : function->slot_count) *
                       sizeof(*slot_tokens));
  value_tokens =
      malloc((function->value_count == 0 ? 1 : function->value_count) *
             sizeof(*value_tokens));
  if (slot_tokens == NULL || value_tokens == NULL) {
    set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_ALLOCATION_FAILED, 0, 0,
                   OWNERSHIP_INVALID_ID,
                   "could not allocate semantic-to-plan token map");
    goto failure;
  }
  for (index = 0; index < function->slot_count; index++) {
    slot_tokens[index] = add_token_or_fail(
        built, token_from_slot(&function->slots[index], (ownership_id)index),
        diagnostic);
    if (slot_tokens[index] == OWNERSHIP_INVALID_ID) {
      goto failure;
    }
    built->slots[index].token = slot_tokens[index];
  }
  for (index = 0; index < function->value_count; index++) {
    value_tokens[index] = add_token_or_fail(
        built,
        token_from_value(&function->values[index], OWNERSHIP_ORIGIN_VALUE,
                         (ownership_id)index),
        diagnostic);
    if (value_tokens[index] == OWNERSHIP_INVALID_ID) {
      goto failure;
    }
  }
  block = ownership_plan_internal_add_block(built);
  built->entry_block = block;
  if (block == OWNERSHIP_INVALID_ID) {
    set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_ALLOCATION_FAILED, 0, 0,
                   OWNERSHIP_INVALID_ID, "could not allocate ownership block");
    goto failure;
  }
  if (!append_parameter_initializers(function, built, block, slot_tokens,
                                     diagnostic)) {
    goto failure;
  }

  for (index = 0; index < function->blocks[0].operation_count; index++) {
    const sir_operation *source = &function->blocks[0].operations[index];
    ownership_operation operation;
    ownership_id source_token;
    switch (source->kind) {
    case SIR_OP_LOAD_SLOT:
      operation = empty_operation(OWNERSHIP_OP_COPY_SCALAR);
      operation.operand = slot_tokens[source->slot];
      operation.result = value_tokens[source->result];
      break;
    case SIR_OP_BORROW_SLOT:
      operation = empty_operation(OWNERSHIP_OP_BORROW);
      operation.operand = slot_tokens[source->slot];
      operation.result = value_tokens[source->result];
      break;
    case SIR_OP_INIT_SLOT:
      source_token = value_tokens[source->operands[0]];
      if (function->slots[source->slot].representation == SIR_REP_SCALAR) {
        operation = empty_operation(OWNERSHIP_OP_COPY_SCALAR);
      } else if (function->values[source->operands[0]].ownership ==
                 SIR_OWNERSHIP_BORROWED) {
        operation = empty_operation(OWNERSHIP_OP_HOLD);
      } else {
        operation = empty_operation(OWNERSHIP_OP_MOVE);
      }
      operation.operand = source_token;
      operation.result = slot_tokens[source->slot];
      break;
    case SIR_OP_PREPARE_ARGUMENT:
      source_token = value_tokens[source->operands[0]];
      operation =
          empty_operation(function->values[source->operands[0]].ownership ==
                                  SIR_OWNERSHIP_BORROWED
                              ? OWNERSHIP_OP_HOLD
                              : OWNERSHIP_OP_MOVE);
      operation.operand = source_token;
      operation.result = value_tokens[source->result];
      operation.callee = source->callee;
      operation.parameter_index = source->parameter_index;
      break;
    case SIR_OP_CALL: {
      size_t argument_index;
      operation = empty_operation(OWNERSHIP_OP_CALL);
      operation.result = value_tokens[source->result];
      operation.callee = source->callee;
      operation.operand_count = source->operand_count;
      if (source->operand_count != 0) {
        operation.operands =
            malloc(source->operand_count * sizeof(*operation.operands));
        if (operation.operands == NULL) {
          set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_ALLOCATION_FAILED, 0,
                         index, OWNERSHIP_INVALID_ID,
                         "could not allocate ownership call operands");
          goto failure;
        }
        for (argument_index = 0; argument_index < source->operand_count;
             argument_index++) {
          operation.operands[argument_index] =
              value_tokens[source->operands[argument_index]];
        }
      }
      if (!append_or_fail(built, block, operation, diagnostic)) {
        free(operation.operands);
        goto failure;
      }
      free(operation.operands);
      continue;
    }
    case SIR_OP_EXPRESSION_END:
      operation = empty_operation(OWNERSHIP_OP_RELEASE);
      operation.operand = value_tokens[source->operands[0]];
      break;
    case SIR_OP_RETURN:
      operation = empty_operation(OWNERSHIP_OP_RETURN);
      if (source->operand_count == 1) {
        source_token = value_tokens[source->operands[0]];
        if (function->values[source->operands[0]].ownership ==
            SIR_OWNERSHIP_BORROWED) {
          ownership_token token = token_from_value(
              &function->values[source->operands[0]],
              OWNERSHIP_ORIGIN_SYNTHETIC, OWNERSHIP_INVALID_ID);
          ownership_id held = add_token_or_fail(built, token, diagnostic);
          ownership_operation hold;
          if (held == OWNERSHIP_INVALID_ID) {
            goto failure;
          }
          hold = empty_operation(OWNERSHIP_OP_HOLD);
          hold.operand = source_token;
          hold.result = held;
          if (!append_or_fail(built, block, hold, diagnostic)) {
            goto failure;
          }
          source_token = held;
        }
        if (!append_cleanup(function, built, block, slot_tokens, source_token,
                            diagnostic)) {
          goto failure;
        }
        operation.operand = source_token;
      } else if (!append_cleanup(function, built, block, slot_tokens,
                                 OWNERSHIP_INVALID_ID, diagnostic)) {
        goto failure;
      }
      break;
    case SIR_OP_CONSTANT:
    case SIR_OP_PRIMITIVE:
    case SIR_OP_TERMINAL_CALL:
    case SIR_OP_JUMP:
    case SIR_OP_BRANCH:
    case SIR_OP_UNKNOWN:
    case SIR_OP_OPAQUE_EXPRESSION:
      set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_UNSUPPORTED_SOURCE_IR, 0,
                     index, OWNERSHIP_INVALID_ID,
                     "semantic operation is outside the checkpoint planner");
      goto failure;
    }
    if (!append_or_fail(built, block, operation, diagnostic)) {
      goto failure;
    }
  }
  if (!ownership_plan_internal_verify_and_seal(built, diagnostic)) {
    goto failure;
  }
  free(value_tokens);
  free(slot_tokens);
  return built;

failure:
  free(value_tokens);
  free(slot_tokens);
  if (built != NULL) {
    ownership_plan_internal_destroy(built);
    free(built);
  }
  return NULL;
}

static int has_live_borrow(const ownership_plan *plan,
                           const ownership_flow_state *state,
                           ownership_id owner) {
  size_t index;
  for (index = 0; index < plan->token_count; index++) {
    if (state->states[index] == OWNERSHIP_STATE_BORROWED &&
        state->provenance[index] == owner) {
      return 1;
    }
  }
  return 0;
}

static int validate_static_structure(const ownership_plan *plan,
                                     unsigned char *definitions,
                                     ownership_diagnostic *diagnostic) {
  size_t token_index;
  size_t block_index;
  for (token_index = 0; token_index < plan->token_count; token_index++) {
    if (!token_metadata_valid(&plan->tokens[token_index])) {
      set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_UNKNOWN_TOKEN, 0, 0,
                     (ownership_id)token_index,
                     "ownership token has unknown or inconsistent metadata");
      return 0;
    }
  }
  if (plan->entry_block >= plan->block_count) {
    set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_BLOCK, 0, 0,
                   OWNERSHIP_INVALID_ID,
                   "ownership entry block is not declared");
    return 0;
  }
  for (block_index = 0; block_index < plan->block_count; block_index++) {
    const ownership_block *block = &plan->blocks[block_index];
    size_t operation_index;
    if (block->operation_count == 0) {
      set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_TERMINATOR,
                     block_index, 0, OWNERSHIP_INVALID_ID,
                     "ownership block is empty");
      return 0;
    }
    for (operation_index = 0; operation_index < block->operation_count;
         operation_index++) {
      const ownership_operation *operation =
          &block->operations[operation_index];
      size_t target_index;
      if (operation->target_count > 2) {
        set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_BLOCK,
                       block_index, operation_index, OWNERSHIP_INVALID_ID,
                       "ownership operation has too many branch targets");
        return 0;
      }
      if (operation->kind == OWNERSHIP_OP_UNKNOWN) {
        set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_OPERATION,
                       block_index, operation_index, OWNERSHIP_INVALID_ID,
                       "ownership operation is unknown");
        return 0;
      }
      if (!static_operation_contract(operation)) {
        set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_OPERATION,
                       block_index, operation_index, OWNERSHIP_INVALID_ID,
                       "ownership operation has an invalid static shape");
        return 0;
      }
      if (is_terminator(operation->kind) !=
          (operation_index + 1 == block->operation_count)) {
        set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_TERMINATOR,
                       block_index, operation_index, OWNERSHIP_INVALID_ID,
                       "each ownership block must end in one terminator");
        return 0;
      }
      if (operation_defines_token(operation->kind)) {
        if (operation->result >= plan->token_count) {
          set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_TOKEN,
                         block_index, operation_index, operation->result,
                         "ownership result token is not declared");
          return 0;
        }
        if (definitions[operation->result]) {
          set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_DUPLICATE_DEFINITION,
                         block_index, operation_index, operation->result,
                         "ownership token has multiple definitions");
          return 0;
        }
        definitions[operation->result] = 1;
      }
      if (operation->operand != OWNERSHIP_INVALID_ID &&
          operation->operand >= plan->token_count) {
        set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_TOKEN,
                       block_index, operation_index, operation->operand,
                       "ownership operand token is not declared");
        return 0;
      }
      if (operation->callee != OWNERSHIP_INVALID_ID &&
          operation->callee >= plan->callee_count) {
        set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_OPERATION,
                       block_index, operation_index, OWNERSHIP_INVALID_ID,
                       "ownership call target is not declared");
        return 0;
      }
      for (target_index = 0; target_index < operation->operand_count;
           target_index++) {
        if (operation->operands[target_index] >= plan->token_count) {
          set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_TOKEN,
                         block_index, operation_index,
                         operation->operands[target_index],
                         "ownership call operand token is not declared");
          return 0;
        }
      }
      for (target_index = 0; target_index < operation->target_count;
           target_index++) {
        if (operation->targets[target_index] >= plan->block_count) {
          set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_BLOCK,
                         block_index, operation_index, OWNERSHIP_INVALID_ID,
                         "ownership branch target is not declared");
          return 0;
        }
      }
    }
  }
  for (token_index = 0; token_index < plan->token_count; token_index++) {
    if (!definitions[token_index]) {
      set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_TOKEN, 0, 0,
                     (ownership_id)token_index,
                     "ownership token has no definition");
      return 0;
    }
  }
  for (block_index = 0; block_index < plan->block_count; block_index++) {
    const ownership_block *block = &plan->blocks[block_index];
    size_t operation_index;
    for (operation_index = 0; operation_index < block->operation_count;
         operation_index++) {
      const ownership_operation *call = &block->operations[operation_index];
      size_t argument_index;
      size_t previous_definition = 0;
      int has_previous_definition = 0;
      if (call->kind != OWNERSHIP_OP_CALL)
        continue;
      for (argument_index = 0; argument_index < call->operand_count;
           argument_index++) {
        size_t definition_index;
        const ownership_operation *preparation = NULL;
        for (definition_index = 0; definition_index < operation_index;
             definition_index++) {
          const ownership_operation *candidate =
              &block->operations[definition_index];
          if (operation_defines_token(candidate->kind) &&
              candidate->result == call->operands[argument_index]) {
            preparation = candidate;
            break;
          }
        }
        if (preparation == NULL ||
            (preparation->kind != OWNERSHIP_OP_HOLD &&
             preparation->kind != OWNERSHIP_OP_MOVE) ||
            preparation->callee != call->callee ||
            preparation->parameter_index != argument_index ||
            (has_previous_definition &&
             definition_index <= previous_definition)) {
          set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_OPERATION,
                         block_index, operation_index,
                         call->operands[argument_index],
                         "ownership call operand is not an ordered prepared "
                         "share");
          return 0;
        }
        previous_definition = definition_index;
        has_previous_definition = 1;
      }
    }
  }
  return 1;
}

static int require_uninitialized(const ownership_flow_state *state,
                                 ownership_id token, size_t block_index,
                                 size_t operation_index,
                                 ownership_diagnostic *diagnostic) {
  if (state->states[token] == OWNERSHIP_STATE_UNINITIALIZED) {
    return 1;
  }
  set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_DUPLICATE_DEFINITION,
                 block_index, operation_index, token,
                 "ownership result is already initialized on this path");
  return 0;
}

static int process_operation(const ownership_plan *plan,
                             ownership_flow_state *state,
                             const ownership_operation *operation,
                             size_t block_index, size_t operation_index,
                             ownership_diagnostic *diagnostic) {
  ownership_id owner;
  switch (operation->kind) {
  case OWNERSHIP_OP_ACQUIRE:
  case OWNERSHIP_OP_ADOPT:
    if (!require_uninitialized(state, operation->result, block_index,
                               operation_index, diagnostic) ||
        plan->tokens[operation->result].kind == OWNERSHIP_TOKEN_SCALAR) {
      break;
    }
    state->states[operation->result] = OWNERSHIP_STATE_OWNED;
    return 1;
  case OWNERSHIP_OP_DEFINE_SCALAR:
    if (!require_uninitialized(state, operation->result, block_index,
                               operation_index, diagnostic) ||
        plan->tokens[operation->result].kind != OWNERSHIP_TOKEN_SCALAR) {
      break;
    }
    state->states[operation->result] = OWNERSHIP_STATE_SCALAR;
    return 1;
  case OWNERSHIP_OP_COPY_SCALAR:
    if (!require_uninitialized(state, operation->result, block_index,
                               operation_index, diagnostic) ||
        state->states[operation->operand] != OWNERSHIP_STATE_SCALAR ||
        plan->tokens[operation->result].kind != OWNERSHIP_TOKEN_SCALAR ||
        !token_metadata_equal(&plan->tokens[operation->operand],
                              &plan->tokens[operation->result])) {
      break;
    }
    state->states[operation->result] = OWNERSHIP_STATE_SCALAR;
    return 1;
  case OWNERSHIP_OP_BORROW:
    if (!require_uninitialized(state, operation->result, block_index,
                               operation_index, diagnostic)) {
      return 0;
    }
    if (state->states[operation->operand] != OWNERSHIP_STATE_OWNED ||
        plan->tokens[operation->operand].kind != OWNERSHIP_TOKEN_MANAGED ||
        !token_metadata_equal(&plan->tokens[operation->operand],
                              &plan->tokens[operation->result])) {
      set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_BORROW_PROVENANCE,
                     block_index, operation_index, operation->operand,
                     "borrow does not originate from a live managed owner");
      return 0;
    }
    state->states[operation->result] = OWNERSHIP_STATE_BORROWED;
    state->provenance[operation->result] = operation->operand;
    return 1;
  case OWNERSHIP_OP_HOLD:
    if (!require_uninitialized(state, operation->result, block_index,
                               operation_index, diagnostic) ||
        state->states[operation->operand] != OWNERSHIP_STATE_BORROWED ||
        !token_metadata_equal(&plan->tokens[operation->operand],
                              &plan->tokens[operation->result])) {
      break;
    }
    owner = state->provenance[operation->operand];
    if (owner >= plan->token_count ||
        state->states[owner] != OWNERSHIP_STATE_OWNED) {
      set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_BORROW_PROVENANCE,
                     block_index, operation_index, operation->operand,
                     "borrow owner is no longer live at hold");
      return 0;
    }
    state->states[operation->operand] = OWNERSHIP_STATE_RELEASED;
    state->provenance[operation->operand] = OWNERSHIP_INVALID_ID;
    state->states[operation->result] = OWNERSHIP_STATE_OWNED;
    return 1;
  case OWNERSHIP_OP_MOVE:
    if (!require_uninitialized(state, operation->result, block_index,
                               operation_index, diagnostic)) {
      return 0;
    }
    if (state->states[operation->operand] != OWNERSHIP_STATE_OWNED ||
        has_live_borrow(plan, state, operation->operand) ||
        !token_metadata_equal(&plan->tokens[operation->operand],
                              &plan->tokens[operation->result])) {
      set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_MOVE, block_index,
                     operation_index, operation->operand,
                     "move requires an unborrowed live owner");
      return 0;
    }
    state->states[operation->operand] = OWNERSHIP_STATE_MOVED;
    state->states[operation->result] = OWNERSHIP_STATE_OWNED;
    return 1;
  case OWNERSHIP_OP_RELEASE:
    if (state->states[operation->operand] == OWNERSHIP_STATE_RELEASED) {
      set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_DOUBLE_RELEASE,
                     block_index, operation_index, operation->operand,
                     "owner is released twice on one path");
      return 0;
    }
    if (state->states[operation->operand] != OWNERSHIP_STATE_OWNED) {
      set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_OPERATION,
                     block_index, operation_index, operation->operand,
                     "release requires a live owner");
      return 0;
    }
    if (has_live_borrow(plan, state, operation->operand)) {
      set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_LIVE_BORROW, block_index,
                     operation_index, operation->operand,
                     "owner cannot be released while a borrow is live");
      return 0;
    }
    state->states[operation->operand] = OWNERSHIP_STATE_RELEASED;
    return 1;
  case OWNERSHIP_OP_END_BORROW:
    if (state->states[operation->operand] != OWNERSHIP_STATE_BORROWED) {
      break;
    }
    state->states[operation->operand] = OWNERSHIP_STATE_RELEASED;
    state->provenance[operation->operand] = OWNERSHIP_INVALID_ID;
    return 1;
  case OWNERSHIP_OP_CALL: {
    size_t argument_index;
    if (!require_uninitialized(state, operation->result, block_index,
                               operation_index, diagnostic) ||
        operation->callee >= plan->callee_count ||
        plan->tokens[operation->result].kind != OWNERSHIP_TOKEN_MANAGED) {
      break;
    }
    for (argument_index = 0; argument_index < operation->operand_count;
         argument_index++) {
      ownership_id argument = operation->operands[argument_index];
      if (argument >= plan->token_count ||
          state->states[argument] != OWNERSHIP_STATE_OWNED ||
          has_live_borrow(plan, state, argument)) {
        set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_MOVE,
                       block_index, operation_index, argument,
                       "call requires prepared owned arguments");
        return 0;
      }
      state->states[argument] = OWNERSHIP_STATE_MOVED;
    }
    state->states[operation->result] = OWNERSHIP_STATE_OWNED;
    return 1;
  }
  case OWNERSHIP_OP_BRANCH:
    if (operation->target_count == 2 &&
        state->states[operation->operand] == OWNERSHIP_STATE_SCALAR &&
        plan->tokens[operation->operand].type.kind == SIR_TYPE_BOOL) {
      return 1;
    }
    break;
  case OWNERSHIP_OP_JUMP:
    if (operation->target_count == 1 &&
        operation->operand == OWNERSHIP_INVALID_ID) {
      return 1;
    }
    break;
  case OWNERSHIP_OP_RETURN:
    if (operation->target_count != 0) {
      break;
    }
    if (operation->operand != OWNERSHIP_INVALID_ID) {
      if (state->states[operation->operand] == OWNERSHIP_STATE_OWNED) {
        if (has_live_borrow(plan, state, operation->operand)) {
          set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_LIVE_BORROW,
                         block_index, operation_index, operation->operand,
                         "returned owner still has a live borrow");
          return 0;
        }
      } else if (state->states[operation->operand] != OWNERSHIP_STATE_SCALAR) {
        set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_MOVE,
                       block_index, operation_index, operation->operand,
                       "return requires a scalar or owned token");
        return 0;
      }
      state->states[operation->operand] = OWNERSHIP_STATE_RETURNED;
    }
    for (owner = 0; owner < plan->token_count; owner++) {
      if (state->states[owner] == OWNERSHIP_STATE_OWNED) {
        set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_LIVE_OWNER_AT_RETURN,
                       block_index, operation_index, owner,
                       "owned token is not released or returned");
        return 0;
      }
      if (state->states[owner] == OWNERSHIP_STATE_BORROWED) {
        set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_LIVE_BORROW,
                       block_index, operation_index, owner,
                       "borrow remains live at return");
        return 0;
      }
    }
    return 1;
  case OWNERSHIP_OP_UNKNOWN:
    break;
  }
  set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_OPERATION,
                 block_index, operation_index, operation->operand,
                 "ownership operation is invalid for the current token state");
  return 0;
}

static int merge_successor(const ownership_plan *plan,
                           ownership_flow_state *incoming,
                           unsigned char *has_incoming, unsigned char *queued,
                           ownership_id *queue, size_t *queue_count,
                           ownership_id successor,
                           const ownership_flow_state *outgoing,
                           size_t block_index, size_t operation_index,
                           ownership_diagnostic *diagnostic) {
  size_t token_index;
  ownership_flow_state *target = &incoming[successor];
  if (!has_incoming[successor]) {
    memcpy(target->states, outgoing->states,
           plan->token_count * sizeof(*target->states));
    memcpy(target->provenance, outgoing->provenance,
           plan->token_count * sizeof(*target->provenance));
    has_incoming[successor] = 1;
    if (!queued[successor]) {
      queue[(*queue_count)++] = successor;
      queued[successor] = 1;
    }
    return 1;
  }
  for (token_index = 0; token_index < plan->token_count; token_index++) {
    if (target->states[token_index] != outgoing->states[token_index] ||
        (target->states[token_index] == OWNERSHIP_STATE_BORROWED &&
         target->provenance[token_index] !=
             outgoing->provenance[token_index])) {
      set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_UNBALANCED_JOIN,
                     block_index, operation_index, (ownership_id)token_index,
                     "control-flow join has different ownership states");
      return 0;
    }
  }
  return 1;
}

int ownership_plan_internal_verify_and_seal(ownership_plan *plan,
                                            ownership_diagnostic *diagnostic) {
  unsigned char *definitions = NULL;
  ownership_token_state *state_storage = NULL;
  ownership_id *provenance_storage = NULL;
  ownership_flow_state *incoming = NULL;
  unsigned char *has_incoming = NULL;
  unsigned char *queued = NULL;
  ownership_id *queue = NULL;
  size_t queue_head = 0;
  size_t queue_count = 0;
  size_t token_storage_count;
  size_t block_index;
  int valid = 0;

  set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_NONE, 0, 0,
                 OWNERSHIP_INVALID_ID, NULL);
  if (plan == NULL) {
    set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_INVALID_BLOCK, 0, 0,
                   OWNERSHIP_INVALID_ID, "ownership plan is missing");
    return 0;
  }
  if (plan->sealed) {
    return 1;
  }
  definitions = calloc(plan->token_count == 0 ? 1 : plan->token_count,
                       sizeof(*definitions));
  if (definitions == NULL ||
      !validate_static_structure(plan, definitions, diagnostic)) {
    goto cleanup;
  }
  if (plan->block_count != 0 &&
      plan->token_count > SIZE_MAX / plan->block_count) {
    set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_ALLOCATION_FAILED, 0, 0,
                   OWNERSHIP_INVALID_ID,
                   "ownership verifier state is too large");
    goto cleanup;
  }
  token_storage_count = plan->block_count * plan->token_count;
  if (token_storage_count == 0) {
    token_storage_count = plan->block_count == 0 ? 1 : plan->block_count;
  }
  state_storage = calloc(token_storage_count, sizeof(*state_storage));
  provenance_storage =
      malloc(token_storage_count * sizeof(*provenance_storage));
  incoming = calloc(plan->block_count, sizeof(*incoming));
  has_incoming = calloc(plan->block_count, sizeof(*has_incoming));
  queued = calloc(plan->block_count, sizeof(*queued));
  queue = malloc(plan->block_count * sizeof(*queue));
  if (state_storage == NULL || provenance_storage == NULL || incoming == NULL ||
      has_incoming == NULL || queued == NULL || queue == NULL) {
    set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_ALLOCATION_FAILED, 0, 0,
                   OWNERSHIP_INVALID_ID,
                   "could not allocate ownership verifier state");
    goto cleanup;
  }
  for (block_index = 0; block_index < token_storage_count; block_index++) {
    provenance_storage[block_index] = OWNERSHIP_INVALID_ID;
  }
  for (block_index = 0; block_index < plan->block_count; block_index++) {
    incoming[block_index].states =
        state_storage + block_index * plan->token_count;
    incoming[block_index].provenance =
        provenance_storage + block_index * plan->token_count;
  }
  has_incoming[plan->entry_block] = 1;
  queued[plan->entry_block] = 1;
  queue[queue_count++] = plan->entry_block;

  while (queue_head < queue_count) {
    ownership_id current = queue[queue_head++];
    ownership_block *block = &plan->blocks[current];
    ownership_token_state *states = malloc(
        (plan->token_count == 0 ? 1 : plan->token_count) * sizeof(*states));
    ownership_id *provenance = malloc(
        (plan->token_count == 0 ? 1 : plan->token_count) * sizeof(*provenance));
    ownership_flow_state outgoing;
    size_t operation_index;
    size_t target_index;
    const ownership_operation *terminator;
    if (states == NULL || provenance == NULL) {
      free(states);
      free(provenance);
      set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_ALLOCATION_FAILED,
                     current, 0, OWNERSHIP_INVALID_ID,
                     "could not allocate ownership block state");
      goto cleanup;
    }
    memcpy(states, incoming[current].states,
           plan->token_count * sizeof(*states));
    memcpy(provenance, incoming[current].provenance,
           plan->token_count * sizeof(*provenance));
    outgoing.states = states;
    outgoing.provenance = provenance;
    for (operation_index = 0; operation_index < block->operation_count;
         operation_index++) {
      if (!process_operation(plan, &outgoing,
                             &block->operations[operation_index], current,
                             operation_index, diagnostic)) {
        free(provenance);
        free(states);
        goto cleanup;
      }
    }
    terminator = &block->operations[block->operation_count - 1];
    for (target_index = 0; target_index < terminator->target_count;
         target_index++) {
      if (!merge_successor(plan, incoming, has_incoming, queued, queue,
                           &queue_count, terminator->targets[target_index],
                           &outgoing, current, block->operation_count - 1,
                           diagnostic)) {
        free(provenance);
        free(states);
        goto cleanup;
      }
    }
    free(provenance);
    free(states);
  }
  for (block_index = 0; block_index < plan->block_count; block_index++) {
    if (!has_incoming[block_index]) {
      set_diagnostic(diagnostic, OWNERSHIP_DIAGNOSTIC_UNREACHABLE_BLOCK,
                     block_index, 0, OWNERSHIP_INVALID_ID,
                     "ownership block is unreachable");
      goto cleanup;
    }
  }
  plan->sealed = 1;
  valid = 1;

cleanup:
  free(queue);
  free(queued);
  free(has_incoming);
  free(incoming);
  free(provenance_storage);
  free(state_storage);
  free(definitions);
  return valid;
}

void ownership_plan_destroy(ownership_plan *plan) {
  if (plan == NULL) {
    return;
  }
  ownership_plan_internal_destroy(plan);
  free(plan);
}

int ownership_plan_is_verified(const ownership_plan *plan) {
  return plan != NULL && plan->sealed;
}

int ownership_plan_function(const ownership_plan *plan,
                            ownership_function_view *view) {
  if (!ownership_plan_is_verified(plan) || view == NULL) {
    return 0;
  }
  view->name = plan->function_name;
  view->external_symbol = plan->function_external_symbol;
  view->c_symbol = plan->function_c_symbol;
  view->return_type = plan->return_type;
  view->return_representation = plan->return_representation;
  view->return_ownership = plan->return_ownership;
  return 1;
}

size_t ownership_plan_slot_count(const ownership_plan *plan) {
  return ownership_plan_is_verified(plan) ? plan->slot_count : 0;
}

int ownership_plan_slot_at(const ownership_plan *plan, ownership_id slot,
                           ownership_slot_view *view) {
  ownership_slot *source;
  if (!ownership_plan_is_verified(plan) || view == NULL ||
      slot >= plan->slot_count) {
    return 0;
  }
  source = &plan->slots[slot];
  view->name = source->name;
  view->type = source->type;
  view->representation = source->representation;
  view->ownership = source->ownership;
  view->is_parameter = source->is_parameter;
  view->token = source->token;
  return 1;
}

const char *ownership_plan_callee_symbol(const ownership_plan *plan,
                                         ownership_id callee) {
  if (!ownership_plan_is_verified(plan) || callee >= plan->callee_count)
    return NULL;
  return plan->callee_symbols[callee];
}

size_t ownership_plan_token_count(const ownership_plan *plan) {
  return ownership_plan_is_verified(plan) ? plan->token_count : 0;
}

size_t ownership_plan_block_count(const ownership_plan *plan) {
  return ownership_plan_is_verified(plan) ? plan->block_count : 0;
}

ownership_id ownership_plan_entry_block(const ownership_plan *plan) {
  return ownership_plan_is_verified(plan) ? plan->entry_block
                                          : OWNERSHIP_INVALID_ID;
}

int ownership_plan_token_at(const ownership_plan *plan, ownership_id token,
                            ownership_token_view *view) {
  if (!ownership_plan_is_verified(plan) || view == NULL ||
      token >= plan->token_count) {
    return 0;
  }
  *view = plan->tokens[token];
  return 1;
}

size_t ownership_plan_operation_count(const ownership_plan *plan,
                                      ownership_id block) {
  if (!ownership_plan_is_verified(plan) || block >= plan->block_count) {
    return 0;
  }
  return plan->blocks[block].operation_count;
}

int ownership_plan_operation_at(const ownership_plan *plan, ownership_id block,
                                size_t operation,
                                ownership_operation_view *view) {
  if (!ownership_plan_is_verified(plan) || view == NULL ||
      block >= plan->block_count ||
      operation >= plan->blocks[block].operation_count) {
    return 0;
  }
  {
    const ownership_operation *source =
        &plan->blocks[block].operations[operation];
    view->kind = source->kind;
    view->result = source->result;
    view->operand = source->operand;
    view->callee = source->callee;
    view->parameter_index = source->parameter_index;
    view->operands = source->operands;
    view->operand_count = source->operand_count;
    view->targets[0] = source->targets[0];
    view->targets[1] = source->targets[1];
    view->target_count = source->target_count;
  }
  return 1;
}
