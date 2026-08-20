#include "semantic_ir/semantic_ir.h"

#include "semantic_ir/intrinsics.h"

#include <stdlib.h>
#include <string.h>

static void set_diagnostic(sir_diagnostic *diagnostic, sir_diagnostic_code code,
                           size_t block_index, size_t operation_index,
                           const char *message) {
  if (diagnostic == NULL) {
    return;
  }
  diagnostic->code = code;
  diagnostic->block_index = block_index;
  diagnostic->operation_index = operation_index;
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

static int type_metadata_valid(sir_type type, sir_representation representation,
                               sir_ownership ownership,
                               sir_diagnostic *diagnostic) {
  if (type.kind == SIR_TYPE_UNKNOWN) {
    set_diagnostic(diagnostic, SIR_DIAGNOSTIC_UNKNOWN_TYPE, 0, 0,
                   "semantic value has an unknown type");
    return 0;
  }
  if (representation == SIR_REP_UNKNOWN) {
    set_diagnostic(diagnostic, SIR_DIAGNOSTIC_UNKNOWN_REPRESENTATION, 0, 0,
                   "semantic value has an unknown representation");
    return 0;
  }
  if (ownership == SIR_OWNERSHIP_UNKNOWN) {
    set_diagnostic(diagnostic, SIR_DIAGNOSTIC_UNKNOWN_OWNERSHIP, 0, 0,
                   "semantic value has unknown ownership");
    return 0;
  }

  switch (type.kind) {
  case SIR_TYPE_VOID:
    if (type.nominal_id == 0 && representation == SIR_REP_NONE &&
        ownership == SIR_OWNERSHIP_NONE) {
      return 1;
    }
    break;
  case SIR_TYPE_STRING:
    if (type.nominal_id == 0 && representation == SIR_REP_STRING_DESCRIPTOR &&
        (ownership == SIR_OWNERSHIP_BORROWED ||
         ownership == SIR_OWNERSHIP_OWNED)) {
      return 1;
    }
    break;
  case SIR_TYPE_ARRAY:
  case SIR_TYPE_RECORD:
    if (type.nominal_id != 0 && representation == SIR_REP_MANAGED_HANDLE &&
        (ownership == SIR_OWNERSHIP_BORROWED ||
         ownership == SIR_OWNERSHIP_OWNED)) {
      return 1;
    }
    break;
  case SIR_TYPE_EXTERNAL:
    if (type.nominal_id != 0 && representation == SIR_REP_EXTERNAL_TOKEN &&
        ownership == SIR_OWNERSHIP_EXTERNAL_OWNED) {
      return 1;
    }
    break;
  case SIR_TYPE_BOOL:
  case SIR_TYPE_BYTE:
  case SIR_TYPE_WORD:
  case SIR_TYPE_DWORD:
  case SIR_TYPE_INT:
  case SIR_TYPE_FLOAT:
  case SIR_TYPE_CHAR:
    if (type.nominal_id == 0 && representation == SIR_REP_SCALAR &&
        ownership == SIR_OWNERSHIP_SCALAR) {
      return 1;
    }
    break;
  case SIR_TYPE_UNKNOWN:
    break;
  }

  set_diagnostic(diagnostic, SIR_DIAGNOSTIC_INVALID_TYPE_REPRESENTATION, 0, 0,
                 "type, representation, and ownership do not agree");
  return 0;
}

static int effects_valid(sir_effects effects) {
  unsigned known_flags = SIR_EFFECT_CALL | SIR_EFFECT_ALLOCATE |
                         SIR_EFFECT_COLLECT | SIR_EFFECT_CALLBACK |
                         SIR_EFFECT_TERMINATE | SIR_EFFECT_NONRETURNING |
                         SIR_EFFECT_FAIL;
  return effects.known && effects.mutation > SIR_MUTATION_UNKNOWN &&
         effects.mutation <= SIR_MUTATION_GLOBAL &&
         (effects.flags & ~known_flags) == 0;
}

static int effects_are_pure(sir_effects effects) {
  return effects.flags == SIR_EFFECT_NONE &&
         effects.mutation == SIR_MUTATION_NONE;
}

static int operation_is_terminator(sir_op_kind kind) {
  return kind == SIR_OP_JUMP || kind == SIR_OP_BRANCH ||
         kind == SIR_OP_RETURN || kind == SIR_OP_TERMINAL_CALL;
}

static int value_matches_slot(const sir_function *function, sir_id value_id,
                              sir_id slot_id) {
  const sir_value *value = &function->values[value_id];
  const sir_slot *slot = &function->slots[slot_id];
  return sir_type_equal(value->type, slot->type) &&
         value->representation == slot->representation;
}

static int value_metadata_equal(const sir_value *left, const sir_value *right) {
  return sir_type_equal(left->type, right->type) &&
         left->representation == right->representation &&
         left->ownership == right->ownership;
}

static int value_is_numeric(const sir_value *value) {
  return value->representation == SIR_REP_SCALAR &&
         value->ownership == SIR_OWNERSHIP_SCALAR &&
         value->type.kind >= SIR_TYPE_BYTE &&
         value->type.kind <= SIR_TYPE_FLOAT;
}

static int value_is_string_array(const sir_value *value) {
  return value->type.kind == SIR_TYPE_ARRAY &&
         value->type.nominal_id == SIR_TYPE_STRING &&
         value->representation == SIR_REP_MANAGED_HANDLE;
}

static int value_is_owned_string(const sir_value *value) {
  return value->type.kind == SIR_TYPE_STRING && value->type.nominal_id == 0 &&
         value->representation == SIR_REP_STRING_DESCRIPTOR &&
         value->ownership == SIR_OWNERSHIP_OWNED;
}

static sir_effects expected_array_effects(sir_op_kind kind) {
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

static int prepared_mode_matches(const sir_function *function,
                                 size_t block_index, size_t operation_index,
                                 sir_id value, sir_array_input_mode mode) {
  const sir_block *block = &function->blocks[block_index];
  size_t index;
  for (index = 0; index < operation_index; index++) {
    const sir_operation *candidate = &block->operations[index];
    if (candidate->result == value) {
      return candidate->kind == SIR_OP_PREPARE_OWNED &&
             candidate->array_input_mode == mode;
    }
  }
  return 0;
}

static int effects_equal(sir_effects left, sir_effects right) {
  return left.known == right.known && left.flags == right.flags &&
         left.mutation == right.mutation;
}

static int call_arguments_valid(const sir_function *function,
                                const sir_operation *operation,
                                const sir_signature *signature,
                                size_t block_index, size_t operation_index) {
  size_t index;
  size_t previous_preparation = 0;
  int has_previous_preparation = 0;
  if (signature->parameter_count != operation->operand_count) {
    return 0;
  }
  for (index = 0; index < operation->operand_count; index++) {
    const sir_value *value = &function->values[operation->operands[index]];
    const sir_signature_parameter *parameter = &signature->parameters[index];
    if (!sir_type_equal(value->type, parameter->type) ||
        value->representation != parameter->representation ||
        (value->ownership != parameter->ownership &&
         !(signature->kind == SIR_CALLEE_INTRINSIC &&
           parameter->mode == SIR_ARGUMENT_BORROW &&
           value->ownership == SIR_OWNERSHIP_OWNED))) {
      return 0;
    }
    if (signature->kind == SIR_CALLEE_USER ||
        signature->kind == SIR_CALLEE_INTRINSIC) {
      const sir_block *block = &function->blocks[block_index];
      size_t definition_index;
      int prepared = 0;
      for (definition_index = 0; definition_index < operation_index;
           definition_index++) {
        const sir_operation *definition = &block->operations[definition_index];
        if (definition->result == operation->operands[index]) {
          prepared = parameter->mode == SIR_ARGUMENT_SCALAR ||
                     (definition->kind == SIR_OP_PREPARE_ARGUMENT &&
                      definition->callee == operation->callee &&
                      definition->parameter_index == index);
          break;
        }
      }
      if (!prepared || (has_previous_preparation &&
                        definition_index <= previous_preparation))
        return 0;
      previous_preparation = definition_index;
      has_previous_preparation = 1;
    }
  }
  return 1;
}

static int signature_parameter_valid(const sir_signature_parameter *parameter,
                                     sir_diagnostic *diagnostic) {
  if (!type_metadata_valid(parameter->type, parameter->representation,
                           parameter->ownership, diagnostic)) {
    return 0;
  }
  switch (parameter->mode) {
  case SIR_ARGUMENT_SCALAR:
    return parameter->ownership == SIR_OWNERSHIP_SCALAR;
  case SIR_ARGUMENT_BORROW:
    return parameter->ownership == SIR_OWNERSHIP_BORROWED &&
           (parameter->representation == SIR_REP_STRING_DESCRIPTOR ||
            parameter->representation == SIR_REP_MANAGED_HANDLE);
  case SIR_ARGUMENT_CONSUME:
    return parameter->ownership == SIR_OWNERSHIP_OWNED &&
           (parameter->representation == SIR_REP_STRING_DESCRIPTOR ||
            parameter->representation == SIR_REP_MANAGED_HANDLE);
  case SIR_ARGUMENT_EXTERNAL_CONSUME:
    return parameter->ownership == SIR_OWNERSHIP_EXTERNAL_OWNED &&
           parameter->representation == SIR_REP_EXTERNAL_TOKEN;
  case SIR_ARGUMENT_UNKNOWN:
    return 0;
  }
  return 0;
}

static int environment_valid(const sir_environment *environment,
                             sir_diagnostic *diagnostic) {
  size_t index;
  if (environment == NULL) {
    return 1;
  }
  if (environment->signature_count != 0 && environment->signatures == NULL) {
    set_diagnostic(diagnostic, SIR_DIAGNOSTIC_UNRESOLVED_CALLEE, 0, 0,
                   "semantic environment signature table is missing");
    return 0;
  }
  for (index = 0; index < environment->signature_count; index++) {
    const sir_signature *signature = &environment->signatures[index];
    size_t parameter_index;
    size_t duplicate;
    if (signature->kind <= SIR_CALLEE_UNKNOWN ||
        signature->kind > SIR_CALLEE_INTRINSIC ||
        (signature->parameter_count != 0 && signature->parameters == NULL) ||
        signature->c_symbol.data == NULL || signature->c_symbol.length == 0 ||
        !effects_valid(signature->effects) ||
        (signature->effects.flags & SIR_EFFECT_CALL) == 0 ||
        !type_metadata_valid(signature->return_type,
                             signature->return_representation,
                             signature->return_ownership, diagnostic)) {
      set_diagnostic(diagnostic, SIR_DIAGNOSTIC_UNRESOLVED_CALLEE, 0, 0,
                     "semantic environment contains an invalid signature");
      return 0;
    }
    if ((signature->kind == SIR_CALLEE_INTRINSIC &&
         !sir_intrinsic_signature_matches(signature)) ||
        (signature->kind != SIR_CALLEE_INTRINSIC &&
         signature->call_abi != SIR_CALL_ABI_C_RETURN_VALUE)) {
      set_diagnostic(diagnostic, SIR_DIAGNOSTIC_UNRESOLVED_CALLEE, 0, 0,
                     "semantic call ABI is not authoritative");
      return 0;
    }
    if ((signature->return_representation == SIR_REP_STRING_DESCRIPTOR ||
         signature->return_representation == SIR_REP_MANAGED_HANDLE) &&
        signature->return_ownership != SIR_OWNERSHIP_OWNED) {
      set_diagnostic(diagnostic, SIR_DIAGNOSTIC_UNRESOLVED_CALLEE, 0, 0,
                     "managed call results must transfer ownership");
      return 0;
    }
    if ((signature->effects.flags & SIR_EFFECT_NONRETURNING) != 0 &&
        signature->return_type.kind != SIR_TYPE_VOID) {
      set_diagnostic(diagnostic, SIR_DIAGNOSTIC_UNRESOLVED_CALLEE, 0, 0,
                     "nonreturning signature cannot declare a result");
      return 0;
    }
    for (parameter_index = 0; parameter_index < signature->parameter_count;
         parameter_index++) {
      if (!signature_parameter_valid(&signature->parameters[parameter_index],
                                     diagnostic)) {
        set_diagnostic(diagnostic, SIR_DIAGNOSTIC_UNRESOLVED_CALLEE, 0, 0,
                       "semantic environment parameter ABI is invalid");
        return 0;
      }
    }
    for (duplicate = 0; duplicate < index; duplicate++) {
      if (environment->signatures[duplicate].kind == signature->kind &&
          environment->signatures[duplicate].symbol_id ==
              signature->symbol_id) {
        set_diagnostic(diagnostic, SIR_DIAGNOSTIC_UNRESOLVED_CALLEE, 0, 0,
                       "semantic environment contains a duplicate signature");
        return 0;
      }
    }
  }
  return 1;
}

sir_type sir_builtin_type(sir_type_kind kind) {
  sir_type type;
  type.kind = kind;
  type.nominal_id = 0;
  return type;
}

sir_effects sir_effects_none(void) {
  sir_effects effects;
  effects.known = 1;
  effects.flags = SIR_EFFECT_NONE;
  effects.mutation = SIR_MUTATION_NONE;
  return effects;
}

sir_effects sir_effects_unknown(void) {
  sir_effects effects;
  effects.known = 0;
  effects.flags = SIR_EFFECT_NONE;
  effects.mutation = SIR_MUTATION_UNKNOWN;
  return effects;
}

void sir_function_init(sir_function *function) {
  memset(function, 0, sizeof(*function));
  function->return_type = sir_builtin_type(SIR_TYPE_VOID);
  function->return_representation = SIR_REP_NONE;
  function->return_ownership = SIR_OWNERSHIP_NONE;
  function->entry_block = SIR_INVALID_ID;
}

void sir_function_destroy(sir_function *function) {
  size_t block_index;
  size_t operation_index;

  if (function == NULL) {
    return;
  }
  for (block_index = 0; block_index < function->block_count; block_index++) {
    sir_block *block = &function->blocks[block_index];
    for (operation_index = 0; operation_index < block->operation_count;
         operation_index++) {
      free(block->operations[operation_index].operands);
      free(block->operations[operation_index].cleanup_slots);
    }
    free(block->operations);
  }
  free(function->blocks);
  free(function->values);
  free(function->slots);
  sir_function_init(function);
}

sir_id sir_function_add_slot(sir_function *function, sir_slot slot) {
  sir_id result;
  if (!grow_array((void **)&function->slots, &function->slot_capacity,
                  sizeof(*function->slots), function->slot_count + 1)) {
    return SIR_INVALID_ID;
  }
  if (function->slot_count >= SIR_INVALID_ID) {
    return SIR_INVALID_ID;
  }
  result = (sir_id)function->slot_count;
  function->slots[function->slot_count++] = slot;
  return result;
}

sir_id sir_function_add_value(sir_function *function, sir_value value) {
  sir_id result;
  if (!grow_array((void **)&function->values, &function->value_capacity,
                  sizeof(*function->values), function->value_count + 1)) {
    return SIR_INVALID_ID;
  }
  if (function->value_count >= SIR_INVALID_ID) {
    return SIR_INVALID_ID;
  }
  result = (sir_id)function->value_count;
  function->values[function->value_count++] = value;
  return result;
}

sir_id sir_function_add_block(sir_function *function) {
  sir_id result;
  sir_block block;
  memset(&block, 0, sizeof(block));
  if (!grow_array((void **)&function->blocks, &function->block_capacity,
                  sizeof(*function->blocks), function->block_count + 1)) {
    return SIR_INVALID_ID;
  }
  if (function->block_count >= SIR_INVALID_ID) {
    return SIR_INVALID_ID;
  }
  result = (sir_id)function->block_count;
  function->blocks[function->block_count++] = block;
  return result;
}

int sir_block_add_operation(sir_function *function, sir_id block_id,
                            const sir_operation *operation) {
  sir_block *block;
  sir_operation copy;

  if (block_id >= function->block_count || operation == NULL) {
    return 0;
  }
  block = &function->blocks[block_id];
  if (!grow_array((void **)&block->operations, &block->operation_capacity,
                  sizeof(*block->operations), block->operation_count + 1)) {
    return 0;
  }
  copy = *operation;
  copy.operands = NULL;
  copy.cleanup_slots = NULL;
  if (operation->operand_count != 0) {
    if (operation->operands == NULL ||
        operation->operand_count > SIZE_MAX / sizeof(*copy.operands)) {
      return 0;
    }
    copy.operands = malloc(operation->operand_count * sizeof(*copy.operands));
    if (copy.operands == NULL) {
      return 0;
    }
    memcpy(copy.operands, operation->operands,
           operation->operand_count * sizeof(*copy.operands));
  }
  if (operation->cleanup_slot_count != 0) {
    if (operation->cleanup_slots == NULL ||
        operation->cleanup_slot_count >
            SIZE_MAX / sizeof(*copy.cleanup_slots)) {
      free(copy.operands);
      return 0;
    }
    copy.cleanup_slots =
        malloc(operation->cleanup_slot_count * sizeof(*copy.cleanup_slots));
    if (copy.cleanup_slots == NULL) {
      free(copy.operands);
      return 0;
    }
    memcpy(copy.cleanup_slots, operation->cleanup_slots,
           operation->cleanup_slot_count * sizeof(*copy.cleanup_slots));
  }
  block->operations[block->operation_count++] = copy;
  return 1;
}

int sir_type_equal(sir_type left, sir_type right) {
  return left.kind == right.kind && left.nominal_id == right.nominal_id;
}

static int validate_operation_references(
    const sir_function *function, const sir_operation *operation,
    size_t block_index, size_t operation_index, unsigned char *definitions,
    size_t *definition_blocks, size_t *definition_operations,
    sir_diagnostic *diagnostic) {
  size_t index;

  if (!effects_valid(operation->effects)) {
    set_diagnostic(diagnostic, SIR_DIAGNOSTIC_UNKNOWN_EFFECT, block_index,
                   operation_index, "operation effect is unknown");
    return 0;
  }
  if (operation->kind == SIR_OP_OPAQUE_EXPRESSION) {
    set_diagnostic(diagnostic, SIR_DIAGNOSTIC_HIDDEN_EXPRESSION, block_index,
                   operation_index,
                   "opaque expressions are forbidden in semantic IR");
    return 0;
  }
  if (operation->kind == SIR_OP_UNKNOWN) {
    set_diagnostic(diagnostic, SIR_DIAGNOSTIC_INVALID_OPERATION, block_index,
                   operation_index, "operation kind is unknown");
    return 0;
  }
  if (operation->result != SIR_INVALID_ID) {
    if (operation->result >= function->value_count) {
      set_diagnostic(diagnostic, SIR_DIAGNOSTIC_INVALID_VALUE, block_index,
                     operation_index, "operation result is not declared");
      return 0;
    }
    if (definitions[operation->result]) {
      set_diagnostic(diagnostic, SIR_DIAGNOSTIC_INVALID_VALUE, block_index,
                     operation_index,
                     "semantic value has multiple definitions");
      return 0;
    }
    definitions[operation->result] = 1;
    definition_blocks[operation->result] = block_index;
    definition_operations[operation->result] = operation_index;
  }
  if (operation->operand_count != 0 && operation->operands == NULL) {
    set_diagnostic(diagnostic, SIR_DIAGNOSTIC_INVALID_VALUE, block_index,
                   operation_index, "operation operands are missing");
    return 0;
  }
  if (operation->cleanup_slot_count != 0 && operation->cleanup_slots == NULL) {
    set_diagnostic(diagnostic, SIR_DIAGNOSTIC_INVALID_SLOT, block_index,
                   operation_index, "return cleanup slots are missing");
    return 0;
  }
  if (operation->target_count > 2) {
    set_diagnostic(diagnostic, SIR_DIAGNOSTIC_INVALID_BLOCK, block_index,
                   operation_index,
                   "operation has too many control-flow targets");
    return 0;
  }
  for (index = 0; index < operation->operand_count; index++) {
    if (operation->operands[index] >= function->value_count) {
      set_diagnostic(diagnostic, SIR_DIAGNOSTIC_INVALID_VALUE, block_index,
                     operation_index, "operation operand is not declared");
      return 0;
    }
  }
  for (index = 0; index < operation->target_count; index++) {
    if (operation->targets[index] >= function->block_count) {
      set_diagnostic(diagnostic, SIR_DIAGNOSTIC_INVALID_BLOCK, block_index,
                     operation_index, "operation target is not declared");
      return 0;
    }
  }
  for (index = 0; index < operation->cleanup_slot_count; index++) {
    if (operation->cleanup_slots[index] >= function->slot_count ||
        (index != 0 && operation->cleanup_slots[index - 1] <=
                           operation->cleanup_slots[index])) {
      set_diagnostic(diagnostic, SIR_DIAGNOSTIC_INVALID_SLOT, block_index,
                     operation_index,
                     "return cleanup slots must be unique reverse order");
      return 0;
    }
  }
  return 1;
}

static int block_targets(const sir_block *block, size_t target) {
  const sir_operation *terminator =
      &block->operations[block->operation_count - 1];
  size_t index;
  for (index = 0; index < terminator->target_count; index++) {
    if (terminator->targets[index] == target) {
      return 1;
    }
  }
  return 0;
}

static int verify_value_dominance(const sir_function *function,
                                  const size_t *definition_blocks,
                                  const size_t *definition_operations,
                                  sir_diagnostic *diagnostic) {
  unsigned char *reachable = NULL;
  unsigned char *dominators = NULL;
  sir_id *queue = NULL;
  size_t queue_head = 0;
  size_t queue_count = 0;
  size_t block_index;
  int changed;
  int valid = 0;

  if (function->block_count > SIZE_MAX / function->block_count) {
    set_diagnostic(diagnostic, SIR_DIAGNOSTIC_ALLOCATION_FAILED, 0, 0,
                   "semantic dominance matrix is too large");
    return 0;
  }
  reachable = calloc(function->block_count, sizeof(*reachable));
  dominators = calloc(function->block_count * function->block_count,
                      sizeof(*dominators));
  queue = malloc(function->block_count * sizeof(*queue));
  if (reachable == NULL || dominators == NULL || queue == NULL) {
    set_diagnostic(diagnostic, SIR_DIAGNOSTIC_ALLOCATION_FAILED, 0, 0,
                   "could not allocate semantic dominance state");
    goto cleanup;
  }
  reachable[function->entry_block] = 1;
  queue[queue_count++] = function->entry_block;
  while (queue_head < queue_count) {
    sir_id current = queue[queue_head++];
    const sir_operation *terminator =
        &function->blocks[current]
             .operations[function->blocks[current].operation_count - 1];
    size_t target_index;
    for (target_index = 0; target_index < terminator->target_count;
         target_index++) {
      sir_id target = terminator->targets[target_index];
      if (!reachable[target]) {
        reachable[target] = 1;
        queue[queue_count++] = target;
      }
    }
  }
  for (block_index = 0; block_index < function->block_count; block_index++) {
    size_t candidate;
    if (!reachable[block_index]) {
      set_diagnostic(diagnostic, SIR_DIAGNOSTIC_UNREACHABLE_BLOCK, block_index,
                     0, "semantic block is unreachable");
      goto cleanup;
    }
    for (candidate = 0; candidate < function->block_count; candidate++) {
      dominators[block_index * function->block_count + candidate] =
          block_index == function->entry_block
              ? candidate == function->entry_block
              : reachable[candidate];
    }
  }
  do {
    changed = 0;
    for (block_index = 0; block_index < function->block_count; block_index++) {
      size_t candidate;
      if (block_index == function->entry_block) {
        continue;
      }
      for (candidate = 0; candidate < function->block_count; candidate++) {
        size_t predecessor;
        int has_predecessor = 0;
        int dominates = candidate == block_index;
        if (!dominates) {
          dominates = 1;
          for (predecessor = 0; predecessor < function->block_count;
               predecessor++) {
            if (block_targets(&function->blocks[predecessor], block_index)) {
              has_predecessor = 1;
              if (!dominators[predecessor * function->block_count +
                              candidate]) {
                dominates = 0;
              }
            }
          }
          if (!has_predecessor) {
            dominates = 0;
          }
        }
        if (dominators[block_index * function->block_count + candidate] !=
            dominates) {
          dominators[block_index * function->block_count + candidate] =
              (unsigned char)dominates;
          changed = 1;
        }
      }
    }
  } while (changed);

  for (block_index = 0; block_index < function->block_count; block_index++) {
    const sir_block *block = &function->blocks[block_index];
    size_t operation_index;
    for (operation_index = 0; operation_index < block->operation_count;
         operation_index++) {
      const sir_operation *operation = &block->operations[operation_index];
      size_t operand_index;
      for (operand_index = 0; operand_index < operation->operand_count;
           operand_index++) {
        sir_id operand = operation->operands[operand_index];
        size_t definition_block = definition_blocks[operand];
        if ((definition_block == block_index &&
             definition_operations[operand] >= operation_index) ||
            (definition_block != block_index &&
             !dominators[block_index * function->block_count +
                         definition_block])) {
          set_diagnostic(
              diagnostic, SIR_DIAGNOSTIC_VALUE_NOT_DOMINATING, block_index,
              operation_index,
              "semantic operand definition does not dominate its use");
          goto cleanup;
        }
      }
    }
  }
  valid = 1;

cleanup:
  free(queue);
  free(dominators);
  free(reachable);
  return valid;
}

static int validate_operation_contract(const sir_function *function,
                                       const sir_environment *environment,
                                       const sir_operation *operation,
                                       size_t block_index,
                                       size_t operation_index,
                                       sir_diagnostic *diagnostic) {
  const sir_value *result = NULL;

  if (operation->result != SIR_INVALID_ID) {
    result = &function->values[operation->result];
  }
  if (operation->kind != SIR_OP_CALL &&
      operation->kind != SIR_OP_TERMINAL_CALL &&
      operation->kind != SIR_OP_PREPARE_ARGUMENT &&
      operation->callee != SIR_INVALID_ID) {
    set_diagnostic(diagnostic, SIR_DIAGNOSTIC_INVALID_OPERATION, block_index,
                   operation_index,
                   "only explicit calls may reference a resolved signature");
    return 0;
  }
  if (operation->kind != SIR_OP_RETURN && operation->cleanup_slot_count != 0) {
    set_diagnostic(diagnostic, SIR_DIAGNOSTIC_INVALID_OPERATION, block_index,
                   operation_index,
                   "only return may carry an explicit unwind list");
    return 0;
  }
  switch (operation->kind) {
  case SIR_OP_CONSTANT:
    if (result == NULL || operation->operand_count != 0 ||
        operation->target_count != 0 || operation->slot != SIR_INVALID_ID ||
        !effects_are_pure(operation->effects) ||
        result->representation != SIR_REP_SCALAR ||
        result->ownership != SIR_OWNERSHIP_SCALAR ||
        (result->type.kind != SIR_TYPE_BOOL &&
         result->type.kind != SIR_TYPE_INT) ||
        (result->type.kind == SIR_TYPE_BOOL && operation->bool_value != 0 &&
         operation->bool_value != 1) ||
        (result->type.kind == SIR_TYPE_INT &&
         (operation->int_value < INT16_MIN ||
          operation->int_value > INT16_MAX))) {
      break;
    }
    return 1;
  case SIR_OP_LOAD_SLOT:
    if (result != NULL && operation->slot < function->slot_count &&
        operation->operand_count == 0 && operation->target_count == 0 &&
        value_matches_slot(function, operation->result, operation->slot) &&
        result->ownership == SIR_OWNERSHIP_SCALAR &&
        effects_are_pure(operation->effects)) {
      return 1;
    }
    break;
  case SIR_OP_BORROW_SLOT:
    if (result != NULL && operation->slot < function->slot_count &&
        operation->operand_count == 0 && operation->target_count == 0 &&
        value_matches_slot(function, operation->result, operation->slot) &&
        result->ownership == SIR_OWNERSHIP_BORROWED &&
        function->slots[operation->slot].ownership == SIR_OWNERSHIP_OWNED &&
        effects_are_pure(operation->effects)) {
      return 1;
    }
    break;
  case SIR_OP_INIT_SLOT:
    if (result == NULL && operation->slot < function->slot_count &&
        operation->operand_count == 1 && operation->target_count == 0 &&
        value_matches_slot(function, operation->operands[0], operation->slot) &&
        effects_are_pure(operation->effects)) {
      return 1;
    }
    break;
  case SIR_OP_ASSIGN_SLOT:
    if (result == NULL && operation->slot < function->slot_count &&
        operation->operand_count == 1 && operation->target_count == 0 &&
        value_matches_slot(function, operation->operands[0], operation->slot) &&
        effects_are_pure(operation->effects)) {
      return 1;
    }
    break;
  case SIR_OP_END_SLOT:
    if (result == NULL && operation->slot < function->slot_count &&
        operation->operand_count == 0 && operation->target_count == 0 &&
        effects_are_pure(operation->effects)) {
      return 1;
    }
    break;
  case SIR_OP_PREPARE_ARGUMENT:
    if (environment == NULL ||
        operation->callee >= environment->signature_count || result == NULL ||
        operation->operand_count != 1 || operation->target_count != 0 ||
        operation->slot != SIR_INVALID_ID ||
        !effects_are_pure(operation->effects)) {
      break;
    }
    {
      const sir_signature *signature =
          &environment->signatures[operation->callee];
      const sir_value *source = &function->values[operation->operands[0]];
      const sir_signature_parameter *parameter;
      const sir_operation *previous;
      if ((signature->kind != SIR_CALLEE_USER &&
           signature->kind != SIR_CALLEE_INTRINSIC) ||
          operation->parameter_index >= signature->parameter_count ||
          operation_index == 0) {
        break;
      }
      parameter = &signature->parameters[operation->parameter_index];
      previous = &function->blocks[block_index].operations[operation_index - 1];
      if (((signature->kind == SIR_CALLEE_USER &&
            parameter->mode == SIR_ARGUMENT_CONSUME) ||
           (signature->kind == SIR_CALLEE_INTRINSIC &&
            parameter->mode == SIR_ARGUMENT_BORROW)) &&
          previous->result == operation->operands[0] &&
          sir_type_equal(source->type, parameter->type) &&
          source->representation == parameter->representation &&
          (source->ownership == SIR_OWNERSHIP_BORROWED ||
           source->ownership == SIR_OWNERSHIP_OWNED) &&
          sir_type_equal(result->type, parameter->type) &&
          result->representation == parameter->representation &&
          result->ownership == SIR_OWNERSHIP_OWNED) {
        return 1;
      }
    }
    break;
  case SIR_OP_PREPARE_OWNED:
    if (result != NULL && operation->operand_count == 1 &&
        operation->target_count == 0 && operation->slot == SIR_INVALID_ID &&
        effects_are_pure(operation->effects)) {
      const sir_value *source = &function->values[operation->operands[0]];
      const sir_operation *previous =
          operation_index == 0
              ? NULL
              : &function->blocks[block_index]
                     .operations[operation_index - 1];
      sir_array_input_mode expected =
          source->ownership == SIR_OWNERSHIP_BORROWED
              ? SIR_ARRAY_INPUT_COPY_BORROWED
              : SIR_ARRAY_INPUT_TRANSFER_OWNED;
      if (previous != NULL && previous->result == operation->operands[0] &&
          (source->ownership == SIR_OWNERSHIP_BORROWED ||
           source->ownership == SIR_OWNERSHIP_OWNED) &&
          sir_type_equal(source->type, result->type) &&
          source->representation == result->representation &&
          result->ownership == SIR_OWNERSHIP_OWNED &&
          operation->array_input_mode == expected) {
        return 1;
      }
    }
    break;
  case SIR_OP_ARRAY_NEW_STRING:
    if (result != NULL && value_is_string_array(result) &&
        result->ownership == SIR_OWNERSHIP_OWNED &&
        operation->operand_count == 0 && operation->target_count == 0 &&
        operation->slot == SIR_INVALID_ID &&
        operation->array_input_mode == SIR_ARRAY_INPUT_UNKNOWN &&
        effects_equal(operation->effects,
                      expected_array_effects(operation->kind))) {
      return 1;
    }
    break;
  case SIR_OP_ARRAY_APPEND_STRING:
    if (result == NULL && operation->operand_count == 2 &&
        operation->target_count == 0 &&
        operation->array_input_mode > SIR_ARRAY_INPUT_UNKNOWN &&
        operation->array_input_mode <= SIR_ARRAY_INPUT_TRANSFER_OWNED &&
        effects_equal(operation->effects,
                      expected_array_effects(operation->kind)) &&
        value_is_string_array(
            &function->values[operation->operands[0]]) &&
        function->values[operation->operands[0]].ownership ==
            SIR_OWNERSHIP_OWNED &&
        value_is_owned_string(&function->values[operation->operands[1]]) &&
        prepared_mode_matches(function, block_index, operation_index,
                              operation->operands[1],
                              operation->array_input_mode)) {
      return 1;
    }
    break;
  case SIR_OP_ARRAY_GET_STRING:
    if (result != NULL && value_is_owned_string(result) &&
        operation->operand_count == 2 && operation->target_count == 0 &&
        operation->array_input_mode == SIR_ARRAY_INPUT_UNKNOWN &&
        effects_equal(operation->effects,
                      expected_array_effects(operation->kind)) &&
        value_is_string_array(
            &function->values[operation->operands[0]]) &&
        function->values[operation->operands[0]].ownership ==
            SIR_OWNERSHIP_OWNED &&
        function->values[operation->operands[1]].type.kind == SIR_TYPE_INT &&
        function->values[operation->operands[1]].representation ==
            SIR_REP_SCALAR) {
      return 1;
    }
    break;
  case SIR_OP_ARRAY_REPLACE_STRING:
    if (result == NULL && operation->operand_count == 3 &&
        operation->target_count == 0 &&
        operation->array_input_mode > SIR_ARRAY_INPUT_UNKNOWN &&
        operation->array_input_mode <= SIR_ARRAY_INPUT_TRANSFER_OWNED &&
        effects_equal(operation->effects,
                      expected_array_effects(operation->kind)) &&
        value_is_string_array(
            &function->values[operation->operands[0]]) &&
        function->values[operation->operands[0]].ownership ==
            SIR_OWNERSHIP_OWNED &&
        function->values[operation->operands[1]].type.kind == SIR_TYPE_INT &&
        function->values[operation->operands[1]].representation ==
            SIR_REP_SCALAR &&
        value_is_owned_string(&function->values[operation->operands[2]]) &&
        prepared_mode_matches(function, block_index, operation_index,
                              operation->operands[2],
                              operation->array_input_mode)) {
      return 1;
    }
    break;
  case SIR_OP_PRIMITIVE:
    if (operation->primitive == SIR_PRIMITIVE_HIDDEN_CALL ||
        (operation->effects.flags & SIR_EFFECT_CALL) != 0) {
      set_diagnostic(diagnostic, SIR_DIAGNOSTIC_HIDDEN_CALL, block_index,
                     operation_index,
                     "call-like work must be represented by an explicit call");
      return 0;
    }
    if (operation->primitive == SIR_PRIMITIVE_UNKNOWN || result == NULL ||
        operation->target_count != 0 || !effects_are_pure(operation->effects)) {
      break;
    }
    if (operation->primitive == SIR_PRIMITIVE_COPY &&
        operation->operand_count == 1 &&
        value_metadata_equal(result,
                             &function->values[operation->operands[0]]) &&
        result->ownership == SIR_OWNERSHIP_SCALAR) {
      return 1;
    }
    if (operation->primitive >= SIR_PRIMITIVE_ADD &&
        operation->primitive <= SIR_PRIMITIVE_DIVIDE &&
        operation->operand_count == 2 && value_is_numeric(result) &&
        value_metadata_equal(result,
                             &function->values[operation->operands[0]]) &&
        value_metadata_equal(result,
                             &function->values[operation->operands[1]])) {
      return 1;
    }
    if (operation->primitive == SIR_PRIMITIVE_COMPARE &&
        operation->operand_count == 2 && result->type.kind == SIR_TYPE_BOOL &&
        result->representation == SIR_REP_SCALAR &&
        function->values[operation->operands[0]].representation ==
            SIR_REP_SCALAR &&
        value_metadata_equal(&function->values[operation->operands[0]],
                             &function->values[operation->operands[1]])) {
      return 1;
    }
    break;
  case SIR_OP_CALL:
    if (environment == NULL ||
        operation->callee >= environment->signature_count) {
      set_diagnostic(diagnostic, SIR_DIAGNOSTIC_UNRESOLVED_CALLEE, block_index,
                     operation_index, "call target is not resolved");
      return 0;
    }
    {
      const sir_signature *signature =
          &environment->signatures[operation->callee];
      int result_matches =
          (signature->return_type.kind == SIR_TYPE_VOID && result == NULL) ||
          (signature->return_type.kind != SIR_TYPE_VOID && result != NULL &&
           sir_type_equal(result->type, signature->return_type) &&
           result->representation == signature->return_representation &&
           result->ownership == signature->return_ownership);
      if ((signature->effects.flags & SIR_EFFECT_NONRETURNING) == 0 &&
          operation->target_count == 0 && result_matches &&
          effects_equal(operation->effects, signature->effects) &&
          call_arguments_valid(function, operation, signature, block_index,
                               operation_index)) {
        return 1;
      }
      set_diagnostic(diagnostic, SIR_DIAGNOSTIC_CALL_ABI_MISMATCH, block_index,
                     operation_index,
                     "call does not match its authoritative signature");
      return 0;
    }
    break;
  case SIR_OP_TERMINAL_CALL:
    if (environment == NULL ||
        operation->callee >= environment->signature_count) {
      set_diagnostic(diagnostic, SIR_DIAGNOSTIC_UNRESOLVED_CALLEE, block_index,
                     operation_index, "terminal call target is not resolved");
      return 0;
    }
    {
      const sir_signature *signature =
          &environment->signatures[operation->callee];
      if (result == NULL && operation->target_count == 0 &&
          signature->return_type.kind == SIR_TYPE_VOID &&
          (signature->effects.flags & SIR_EFFECT_NONRETURNING) != 0 &&
          effects_equal(operation->effects, signature->effects) &&
          call_arguments_valid(function, operation, signature, block_index,
                               operation_index)) {
        return 1;
      }
      set_diagnostic(
          diagnostic, SIR_DIAGNOSTIC_CALL_ABI_MISMATCH, block_index,
          operation_index,
          "terminal call does not match its authoritative signature");
      return 0;
    }
    break;
  case SIR_OP_EXPRESSION_END:
    if (result == NULL && operation->operand_count == 1 &&
        operation->target_count == 0 && operation->slot == SIR_INVALID_ID &&
        operation->callee == SIR_INVALID_ID &&
        effects_are_pure(operation->effects)) {
      const sir_value *discarded = &function->values[operation->operands[0]];
      const sir_operation *definition =
          operation_index == 0
              ? NULL
              : &function->blocks[block_index].operations[operation_index - 1];
      if (definition != NULL && definition->result == operation->operands[0] &&
          (definition->kind == SIR_OP_CALL ||
           definition->kind == SIR_OP_ARRAY_GET_STRING) &&
          discarded->ownership == SIR_OWNERSHIP_OWNED &&
          discarded->representation == SIR_REP_STRING_DESCRIPTOR) {
        return 1;
      }
    }
    break;
  case SIR_OP_JUMP:
    if (result == NULL && operation->operand_count == 0 &&
        operation->target_count == 1 && effects_are_pure(operation->effects)) {
      return 1;
    }
    break;
  case SIR_OP_BRANCH:
    if (result == NULL && operation->operand_count == 1 &&
        operation->target_count == 2 &&
        function->values[operation->operands[0]].type.kind == SIR_TYPE_BOOL &&
        effects_are_pure(operation->effects)) {
      return 1;
    }
    break;
  case SIR_OP_RETURN:
    if (result != NULL || operation->target_count != 0 ||
        !effects_are_pure(operation->effects)) {
      break;
    }
    if (function->return_type.kind == SIR_TYPE_VOID &&
        operation->operand_count == 0) {
      return 1;
    }
    if (operation->operand_count == 1 &&
        sir_type_equal(function->values[operation->operands[0]].type,
                       function->return_type) &&
        function->values[operation->operands[0]].representation ==
            function->return_representation) {
      return 1;
    }
    break;
  case SIR_OP_UNKNOWN:
  case SIR_OP_OPAQUE_EXPRESSION:
    break;
  }

  set_diagnostic(diagnostic, SIR_DIAGNOSTIC_INVALID_OPERATION, block_index,
                 operation_index,
                 "operation does not satisfy its primitive contract");
  return 0;
}

static int validate_prepared_argument_uses(const sir_function *function,
                                           sir_diagnostic *diagnostic) {
  size_t block_index;
  for (block_index = 0; block_index < function->block_count; block_index++) {
    const sir_block *block = &function->blocks[block_index];
    size_t operation_index;
    for (operation_index = 0; operation_index < block->operation_count;
         operation_index++) {
      const sir_operation *preparation = &block->operations[operation_index];
      size_t use_count = 0;
      size_t use_block;
      if (preparation->kind != SIR_OP_PREPARE_ARGUMENT)
        continue;
      for (use_block = 0; use_block < function->block_count; use_block++) {
        const sir_block *candidate_block = &function->blocks[use_block];
        size_t use_operation;
        for (use_operation = 0;
             use_operation < candidate_block->operation_count;
             use_operation++) {
          const sir_operation *candidate =
              &candidate_block->operations[use_operation];
          size_t operand_index;
          for (operand_index = 0; operand_index < candidate->operand_count;
               operand_index++) {
            if (candidate->operands[operand_index] != preparation->result)
              continue;
            use_count++;
            if (candidate->kind != SIR_OP_CALL ||
                candidate->callee != preparation->callee ||
                operand_index != preparation->parameter_index) {
              set_diagnostic(
                  diagnostic, SIR_DIAGNOSTIC_CALL_ABI_MISMATCH, block_index,
                  operation_index,
                  "prepared argument is used outside its resolved call slot");
              return 0;
            }
          }
        }
      }
      if (use_count != 1) {
        set_diagnostic(diagnostic, SIR_DIAGNOSTIC_CALL_ABI_MISMATCH,
                       block_index, operation_index,
                       "prepared argument must be consumed exactly once");
        return 0;
      }
    }
  }
  return 1;
}

int sir_verify_function(const sir_function *function,
                        const sir_environment *environment,
                        sir_diagnostic *diagnostic) {
  size_t index;
  size_t block_index;
  unsigned char *definitions;
  size_t *definition_blocks;
  size_t *definition_operations;
  int valid = 0;

  set_diagnostic(diagnostic, SIR_DIAGNOSTIC_NONE, 0, 0, NULL);
  if (function == NULL ||
      !type_metadata_valid(function->return_type,
                           function->return_representation,
                           function->return_ownership, diagnostic)) {
    return 0;
  }
  if ((function->return_representation == SIR_REP_STRING_DESCRIPTOR ||
       function->return_representation == SIR_REP_MANAGED_HANDLE) &&
      function->return_ownership != SIR_OWNERSHIP_OWNED) {
    set_diagnostic(diagnostic, SIR_DIAGNOSTIC_UNKNOWN_OWNERSHIP, 0, 0,
                   "managed function results must transfer an owned value");
    return 0;
  }
  if (!environment_valid(environment, diagnostic)) {
    return 0;
  }
  if (function->entry_block >= function->block_count) {
    set_diagnostic(diagnostic, SIR_DIAGNOSTIC_INVALID_BLOCK, 0, 0,
                   "function entry block is not declared");
    return 0;
  }
  for (index = 0; index < function->slot_count; index++) {
    if (!type_metadata_valid(function->slots[index].type,
                             function->slots[index].representation,
                             function->slots[index].ownership, diagnostic)) {
      return 0;
    }
    if ((function->slots[index].representation == SIR_REP_STRING_DESCRIPTOR ||
         function->slots[index].representation == SIR_REP_MANAGED_HANDLE) &&
        function->slots[index].ownership != SIR_OWNERSHIP_OWNED) {
      set_diagnostic(diagnostic, SIR_DIAGNOSTIC_UNKNOWN_OWNERSHIP, 0, 0,
                     "managed storage slots must own their values");
      return 0;
    }
  }
  for (index = 0; index < function->value_count; index++) {
    if (!type_metadata_valid(function->values[index].type,
                             function->values[index].representation,
                             function->values[index].ownership, diagnostic)) {
      return 0;
    }
  }

  definitions = calloc(function->value_count == 0 ? 1 : function->value_count,
                       sizeof(*definitions));
  definition_blocks =
      malloc((function->value_count == 0 ? 1 : function->value_count) *
             sizeof(*definition_blocks));
  definition_operations =
      malloc((function->value_count == 0 ? 1 : function->value_count) *
             sizeof(*definition_operations));
  if (definitions == NULL || definition_blocks == NULL ||
      definition_operations == NULL) {
    set_diagnostic(diagnostic, SIR_DIAGNOSTIC_ALLOCATION_FAILED, 0, 0,
                   "could not allocate semantic verifier state");
    goto cleanup;
  }
  for (index = 0; index < function->value_count; index++) {
    definition_blocks[index] = SIZE_MAX;
    definition_operations[index] = SIZE_MAX;
  }
  for (block_index = 0; block_index < function->block_count; block_index++) {
    const sir_block *block = &function->blocks[block_index];
    size_t operation_index;
    if (block->operation_count == 0) {
      set_diagnostic(diagnostic, SIR_DIAGNOSTIC_INVALID_TERMINATOR, block_index,
                     0, "semantic block is empty");
      goto cleanup;
    }
    for (operation_index = 0; operation_index < block->operation_count;
         operation_index++) {
      const sir_operation *operation = &block->operations[operation_index];
      if (!validate_operation_references(
              function, operation, block_index, operation_index, definitions,
              definition_blocks, definition_operations, diagnostic) ||
          !validate_operation_contract(function, environment, operation,
                                       block_index, operation_index,
                                       diagnostic)) {
        goto cleanup;
      }
      if (operation_is_terminator(operation->kind) !=
          (operation_index + 1 == block->operation_count)) {
        set_diagnostic(diagnostic, SIR_DIAGNOSTIC_INVALID_TERMINATOR,
                       block_index, operation_index,
                       "each semantic block must end in one terminator");
        goto cleanup;
      }
    }
  }
  for (index = 0; index < function->value_count; index++) {
    if (!definitions[index]) {
      set_diagnostic(diagnostic, SIR_DIAGNOSTIC_INVALID_VALUE, 0, 0,
                     "declared semantic value has no definition");
      goto cleanup;
    }
  }
  if (!verify_value_dominance(function, definition_blocks,
                              definition_operations, diagnostic)) {
    goto cleanup;
  }
  if (!validate_prepared_argument_uses(function, diagnostic)) {
    goto cleanup;
  }
  valid = 1;

cleanup:
  free(definition_operations);
  free(definition_blocks);
  free(definitions);
  return valid;
}
