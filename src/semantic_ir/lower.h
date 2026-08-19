#ifndef SEMANTIC_IR_LOWER_H
#define SEMANTIC_IR_LOWER_H

#include "ast.h"
#include "semantic_ir/semantic_ir.h"

typedef struct sir_lower_options {
  int enabled;
} sir_lower_options;

typedef enum sir_lower_result {
  SIR_LOWER_SKIPPED,
  SIR_LOWER_OK,
  SIR_LOWER_ERROR,
} sir_lower_result;

sir_lower_options sir_lower_default_options(void);
sir_lower_result sir_lower_function(ast_t function_ast,
                                    const sir_lower_options *options,
                                    sir_function *function,
                                    sir_diagnostic *diagnostic);

#endif
