#ifndef SEMANTIC_IR_H
#define SEMANTIC_IR_H

#include "stringview.h"

#include <stddef.h>
#include <stdint.h>

#define SIR_INVALID_ID UINT32_MAX

typedef uint32_t sir_id;

typedef enum sir_type_kind {
  SIR_TYPE_UNKNOWN,
  SIR_TYPE_VOID,
  SIR_TYPE_BOOL,
  SIR_TYPE_BYTE,
  SIR_TYPE_WORD,
  SIR_TYPE_DWORD,
  SIR_TYPE_INT,
  SIR_TYPE_FLOAT,
  SIR_TYPE_CHAR,
  SIR_TYPE_STRING,
  SIR_TYPE_ARRAY,
  SIR_TYPE_RECORD,
  SIR_TYPE_EXTERNAL,
} sir_type_kind;

typedef enum sir_representation {
  SIR_REP_UNKNOWN,
  SIR_REP_NONE,
  SIR_REP_SCALAR,
  SIR_REP_STRING_DESCRIPTOR,
  SIR_REP_MANAGED_HANDLE,
  SIR_REP_EXTERNAL_TOKEN,
} sir_representation;

typedef enum sir_ownership {
  SIR_OWNERSHIP_UNKNOWN,
  SIR_OWNERSHIP_NONE,
  SIR_OWNERSHIP_SCALAR,
  SIR_OWNERSHIP_BORROWED,
  SIR_OWNERSHIP_OWNED,
  SIR_OWNERSHIP_EXTERNAL_OWNED,
} sir_ownership;

typedef struct sir_type {
  sir_type_kind kind;
  uint32_t nominal_id;
} sir_type;

typedef enum sir_mutation_scope {
  SIR_MUTATION_UNKNOWN,
  SIR_MUTATION_NONE,
  SIR_MUTATION_LOCAL,
  SIR_MUTATION_ARGUMENT,
  SIR_MUTATION_GLOBAL,
} sir_mutation_scope;

typedef enum sir_effect_flag {
  SIR_EFFECT_NONE = 0,
  SIR_EFFECT_CALL = 1u << 0,
  SIR_EFFECT_ALLOCATE = 1u << 1,
  SIR_EFFECT_COLLECT = 1u << 2,
  SIR_EFFECT_CALLBACK = 1u << 3,
  SIR_EFFECT_TERMINATE = 1u << 4,
  SIR_EFFECT_NONRETURNING = 1u << 5,
  SIR_EFFECT_FAIL = 1u << 6,
} sir_effect_flag;

typedef struct sir_effects {
  int known;
  unsigned flags;
  sir_mutation_scope mutation;
} sir_effects;

typedef enum sir_callee_kind {
  SIR_CALLEE_UNKNOWN,
  SIR_CALLEE_USER,
  SIR_CALLEE_NATIVE,
  SIR_CALLEE_INTRINSIC,
} sir_callee_kind;

typedef enum sir_argument_mode {
  SIR_ARGUMENT_UNKNOWN,
  SIR_ARGUMENT_SCALAR,
  SIR_ARGUMENT_BORROW,
  SIR_ARGUMENT_CONSUME,
  SIR_ARGUMENT_EXTERNAL_CONSUME,
} sir_argument_mode;

typedef struct sir_signature_parameter {
  sir_type type;
  sir_representation representation;
  sir_ownership ownership;
  sir_argument_mode mode;
} sir_signature_parameter;

typedef struct sir_signature {
  sir_callee_kind kind;
  uint32_t symbol_id;
  const sir_signature_parameter *parameters;
  size_t parameter_count;
  sir_type return_type;
  sir_representation return_representation;
  sir_ownership return_ownership;
  sir_effects effects;
} sir_signature;

typedef struct sir_environment {
  const sir_signature *signatures;
  size_t signature_count;
} sir_environment;

typedef enum sir_primitive {
  SIR_PRIMITIVE_UNKNOWN,
  SIR_PRIMITIVE_COPY,
  SIR_PRIMITIVE_ADD,
  SIR_PRIMITIVE_SUBTRACT,
  SIR_PRIMITIVE_MULTIPLY,
  SIR_PRIMITIVE_DIVIDE,
  SIR_PRIMITIVE_COMPARE,
  SIR_PRIMITIVE_HIDDEN_CALL,
} sir_primitive;

typedef enum sir_op_kind {
  SIR_OP_UNKNOWN,
  SIR_OP_CONSTANT,
  SIR_OP_LOAD_SLOT,
  SIR_OP_BORROW_SLOT,
  SIR_OP_INIT_SLOT,
  SIR_OP_PRIMITIVE,
  SIR_OP_CALL,
  SIR_OP_TERMINAL_CALL,
  SIR_OP_JUMP,
  SIR_OP_BRANCH,
  SIR_OP_RETURN,
  SIR_OP_OPAQUE_EXPRESSION,
} sir_op_kind;

typedef struct sir_value {
  sir_type type;
  sir_representation representation;
  sir_ownership ownership;
} sir_value;

typedef struct sir_slot {
  string_view name;
  sir_type type;
  sir_representation representation;
  sir_ownership ownership;
  int is_parameter;
} sir_slot;

typedef struct sir_operation {
  sir_op_kind kind;
  sir_primitive primitive;
  sir_effects effects;
  sir_id result;
  sir_id slot;
  sir_id callee;
  sir_id *operands;
  size_t operand_count;
  sir_id targets[2];
  size_t target_count;
} sir_operation;

typedef struct sir_block {
  sir_operation *operations;
  size_t operation_count;
  size_t operation_capacity;
} sir_block;

typedef struct sir_function {
  string_view name;
  string_view c_symbol;
  sir_type return_type;
  sir_representation return_representation;
  sir_ownership return_ownership;
  sir_slot *slots;
  size_t slot_count;
  size_t slot_capacity;
  sir_value *values;
  size_t value_count;
  size_t value_capacity;
  sir_block *blocks;
  size_t block_count;
  size_t block_capacity;
  sir_id entry_block;
} sir_function;

typedef enum sir_diagnostic_code {
  SIR_DIAGNOSTIC_NONE,
  SIR_DIAGNOSTIC_ALLOCATION_FAILED,
  SIR_DIAGNOSTIC_UNKNOWN_TYPE,
  SIR_DIAGNOSTIC_UNKNOWN_REPRESENTATION,
  SIR_DIAGNOSTIC_UNKNOWN_OWNERSHIP,
  SIR_DIAGNOSTIC_UNKNOWN_EFFECT,
  SIR_DIAGNOSTIC_INVALID_TYPE_REPRESENTATION,
  SIR_DIAGNOSTIC_INVALID_VALUE,
  SIR_DIAGNOSTIC_VALUE_NOT_DOMINATING,
  SIR_DIAGNOSTIC_INVALID_SLOT,
  SIR_DIAGNOSTIC_INVALID_BLOCK,
  SIR_DIAGNOSTIC_UNREACHABLE_BLOCK,
  SIR_DIAGNOSTIC_INVALID_TERMINATOR,
  SIR_DIAGNOSTIC_INVALID_OPERATION,
  SIR_DIAGNOSTIC_HIDDEN_EXPRESSION,
  SIR_DIAGNOSTIC_HIDDEN_CALL,
  SIR_DIAGNOSTIC_UNRESOLVED_CALLEE,
  SIR_DIAGNOSTIC_CALL_ABI_MISMATCH,
  SIR_DIAGNOSTIC_TYPE_MISMATCH,
  SIR_DIAGNOSTIC_UNSUPPORTED_AST,
} sir_diagnostic_code;

typedef struct sir_diagnostic {
  sir_diagnostic_code code;
  size_t block_index;
  size_t operation_index;
  const char *message;
} sir_diagnostic;

sir_type sir_builtin_type(sir_type_kind kind);
sir_effects sir_effects_none(void);
sir_effects sir_effects_unknown(void);

void sir_function_init(sir_function *function);
void sir_function_destroy(sir_function *function);
sir_id sir_function_add_slot(sir_function *function, sir_slot slot);
sir_id sir_function_add_value(sir_function *function, sir_value value);
sir_id sir_function_add_block(sir_function *function);
int sir_block_add_operation(sir_function *function, sir_id block,
                            const sir_operation *operation);

int sir_type_equal(sir_type left, sir_type right);
int sir_verify_function(const sir_function *function,
                        const sir_environment *environment,
                        sir_diagnostic *diagnostic);

#endif
