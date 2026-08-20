#ifndef OWNERSHIP_PLAN_H
#define OWNERSHIP_PLAN_H

#include "semantic_ir/semantic_ir.h"

#include <stddef.h>

#define OWNERSHIP_INVALID_ID SIR_INVALID_ID

typedef sir_id ownership_id;
typedef struct ownership_plan ownership_plan;

typedef enum ownership_token_kind {
  OWNERSHIP_TOKEN_UNKNOWN,
  OWNERSHIP_TOKEN_SCALAR,
  OWNERSHIP_TOKEN_MANAGED,
  OWNERSHIP_TOKEN_EXTERNAL,
} ownership_token_kind;

typedef enum ownership_token_origin_kind {
  OWNERSHIP_ORIGIN_UNKNOWN,
  OWNERSHIP_ORIGIN_SLOT,
  OWNERSHIP_ORIGIN_VALUE,
  OWNERSHIP_ORIGIN_SYNTHETIC,
} ownership_token_origin_kind;

typedef enum ownership_op_kind {
  OWNERSHIP_OP_UNKNOWN,
  OWNERSHIP_OP_ACQUIRE,
  OWNERSHIP_OP_ADOPT,
  OWNERSHIP_OP_DEFINE_SCALAR,
  OWNERSHIP_OP_COPY_SCALAR,
  OWNERSHIP_OP_BORROW,
  OWNERSHIP_OP_HOLD,
  OWNERSHIP_OP_MOVE,
  OWNERSHIP_OP_REPLACE_SLOT,
  OWNERSHIP_OP_END_SLOT,
  OWNERSHIP_OP_RELEASE,
  OWNERSHIP_OP_END_BORROW,
  OWNERSHIP_OP_CALL,
  OWNERSHIP_OP_ARRAY_NEW_STRING,
  OWNERSHIP_OP_ARRAY_APPEND_STRING,
  OWNERSHIP_OP_ARRAY_GET_STRING,
  OWNERSHIP_OP_ARRAY_REPLACE_STRING,
  OWNERSHIP_OP_JUMP,
  OWNERSHIP_OP_BRANCH,
  OWNERSHIP_OP_RETURN,
} ownership_op_kind;

typedef enum ownership_diagnostic_code {
  OWNERSHIP_DIAGNOSTIC_NONE,
  OWNERSHIP_DIAGNOSTIC_ALLOCATION_FAILED,
  OWNERSHIP_DIAGNOSTIC_INVALID_SOURCE_IR,
  OWNERSHIP_DIAGNOSTIC_UNSUPPORTED_SOURCE_IR,
  OWNERSHIP_DIAGNOSTIC_UNKNOWN_TOKEN,
  OWNERSHIP_DIAGNOSTIC_INVALID_TOKEN,
  OWNERSHIP_DIAGNOSTIC_INVALID_BLOCK,
  OWNERSHIP_DIAGNOSTIC_INVALID_OPERATION,
  OWNERSHIP_DIAGNOSTIC_INVALID_TERMINATOR,
  OWNERSHIP_DIAGNOSTIC_DUPLICATE_DEFINITION,
  OWNERSHIP_DIAGNOSTIC_BORROW_PROVENANCE,
  OWNERSHIP_DIAGNOSTIC_LIVE_BORROW,
  OWNERSHIP_DIAGNOSTIC_DOUBLE_RELEASE,
  OWNERSHIP_DIAGNOSTIC_INVALID_MOVE,
  OWNERSHIP_DIAGNOSTIC_UNBALANCED_JOIN,
  OWNERSHIP_DIAGNOSTIC_LIVE_OWNER_AT_RETURN,
  OWNERSHIP_DIAGNOSTIC_UNREACHABLE_BLOCK,
  OWNERSHIP_DIAGNOSTIC_UNVERIFIED_PLAN,
} ownership_diagnostic_code;

typedef struct ownership_diagnostic {
  ownership_diagnostic_code code;
  size_t block_index;
  size_t operation_index;
  ownership_id token;
  const char *message;
} ownership_diagnostic;

typedef struct ownership_token_view {
  ownership_token_kind kind;
  sir_type type;
  sir_representation representation;
  ownership_token_origin_kind origin_kind;
  ownership_id origin;
} ownership_token_view;

typedef struct ownership_function_view {
  const char *name;
  const char *external_symbol;
  const char *c_symbol;
  sir_type return_type;
  sir_representation return_representation;
  sir_ownership return_ownership;
} ownership_function_view;

typedef struct ownership_slot_view {
  const char *name;
  sir_type type;
  sir_representation representation;
  sir_ownership ownership;
  int is_parameter;
  ownership_id token;
} ownership_slot_view;

typedef struct ownership_callee_view {
  sir_callee_kind kind;
  uint32_t symbol_id;
  const char *c_symbol;
  const sir_signature_parameter *parameters;
  size_t parameter_count;
  sir_type return_type;
  sir_representation return_representation;
  sir_ownership return_ownership;
  sir_effects effects;
  sir_call_abi call_abi;
} ownership_callee_view;

typedef struct ownership_operation_view {
  ownership_op_kind kind;
  ownership_id result;
  ownership_id operand;
  ownership_id callee;
  size_t parameter_index;
  sir_array_input_mode array_input_mode;
  int bool_value;
  int int_value;
  const ownership_id *operands;
  size_t operand_count;
  ownership_id targets[2];
  size_t target_count;
} ownership_operation_view;

ownership_plan *ownership_plan_build(const sir_function *function,
                                     const sir_environment *environment,
                                     ownership_diagnostic *diagnostic);
void ownership_plan_destroy(ownership_plan *plan);

int ownership_plan_is_verified(const ownership_plan *plan);
int ownership_plan_function(const ownership_plan *plan,
                            ownership_function_view *view);
size_t ownership_plan_slot_count(const ownership_plan *plan);
int ownership_plan_slot_at(const ownership_plan *plan, ownership_id slot,
                           ownership_slot_view *view);
const char *ownership_plan_callee_symbol(const ownership_plan *plan,
                                         ownership_id callee);
int ownership_plan_callee_at(const ownership_plan *plan, ownership_id callee,
                             ownership_callee_view *view);
size_t ownership_plan_token_count(const ownership_plan *plan);
size_t ownership_plan_block_count(const ownership_plan *plan);
ownership_id ownership_plan_entry_block(const ownership_plan *plan);
int ownership_plan_token_at(const ownership_plan *plan, ownership_id token,
                            ownership_token_view *view);
size_t ownership_plan_operation_count(const ownership_plan *plan,
                                      ownership_id block);
int ownership_plan_operation_at(const ownership_plan *plan, ownership_id block,
                                size_t operation,
                                ownership_operation_view *view);

#endif
