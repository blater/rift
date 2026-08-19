#ifndef SEMANTIC_IR_LOWER_H
#define SEMANTIC_IR_LOWER_H

#include "ast.h"
#include "semantic_ir/semantic_ir.h"

typedef struct sir_lower_options {
  int enabled;
  const sir_environment *environment;
  const struct sir_lower_callee *callees;
  size_t callee_count;
} sir_lower_options;

typedef struct sir_lower_callee {
  ast_t definition;
  sir_id signature;
} sir_lower_callee;

typedef enum sir_lower_result {
  SIR_LOWER_SKIPPED,
  SIR_LOWER_OK,
  SIR_LOWER_ERROR,
} sir_lower_result;

sir_lower_options sir_lower_default_options(void);
int sir_lower_signature(ast_t function_ast, uint32_t symbol_id,
                        string_view c_symbol, sir_signature *signature,
                        sir_diagnostic *diagnostic);
void sir_lower_signature_destroy(sir_signature *signature);
sir_lower_result sir_lower_function(ast_t function_ast,
                                    const sir_lower_options *options,
                                    sir_function *function,
                                    sir_diagnostic *diagnostic);

#endif
