#ifndef OWNERSHIP_PLAN_INTERNAL_H
#define OWNERSHIP_PLAN_INTERNAL_H

#include "ownership_plan/ownership_plan.h"

typedef ownership_token_view ownership_token;
typedef struct ownership_operation {
  ownership_op_kind kind;
  ownership_id result;
  ownership_id operand;
  ownership_id callee;
  size_t parameter_index;
  int bool_value;
  int int_value;
  ownership_id *operands;
  size_t operand_count;
  ownership_id targets[2];
  size_t target_count;
} ownership_operation;

typedef struct ownership_block {
  ownership_operation *operations;
  size_t operation_count;
  size_t operation_capacity;
} ownership_block;

typedef struct ownership_slot {
  char *name;
  sir_type type;
  sir_representation representation;
  sir_ownership ownership;
  int is_parameter;
  ownership_id token;
} ownership_slot;

typedef struct ownership_callee {
  sir_callee_kind kind;
  uint32_t symbol_id;
  char *c_symbol;
  sir_signature_parameter *parameters;
  size_t parameter_count;
  sir_type return_type;
  sir_representation return_representation;
  sir_ownership return_ownership;
  sir_effects effects;
  sir_call_abi call_abi;
} ownership_callee;

struct ownership_plan {
  char *function_name;
  char *function_external_symbol;
  char *function_c_symbol;
  sir_type return_type;
  sir_representation return_representation;
  sir_ownership return_ownership;
  ownership_slot *slots;
  size_t slot_count;
  ownership_token *tokens;
  size_t token_count;
  size_t token_capacity;
  ownership_block *blocks;
  size_t block_count;
  size_t block_capacity;
  ownership_id entry_block;
  ownership_callee *callees;
  size_t callee_count;
  int sealed;
};

typedef enum ownership_token_state {
  OWNERSHIP_STATE_UNINITIALIZED,
  OWNERSHIP_STATE_SCALAR,
  OWNERSHIP_STATE_OWNED,
  OWNERSHIP_STATE_BORROWED,
  OWNERSHIP_STATE_MOVED,
  OWNERSHIP_STATE_RELEASED,
  OWNERSHIP_STATE_RETURNED,
} ownership_token_state;

void ownership_plan_internal_init(ownership_plan *plan);
void ownership_plan_internal_destroy(ownership_plan *plan);
ownership_id ownership_plan_internal_add_token(ownership_plan *plan,
                                               ownership_token token);
ownership_id ownership_plan_internal_add_block(ownership_plan *plan);
int ownership_plan_internal_add_operation(ownership_plan *plan,
                                          ownership_id block,
                                          ownership_operation operation);
int ownership_plan_internal_verify_and_seal(ownership_plan *plan,
                                            ownership_diagnostic *diagnostic);

#endif
