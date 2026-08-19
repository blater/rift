#ifndef GENERATOR_SEMANTIC_PLAN_H
#define GENERATOR_SEMANTIC_PLAN_H

#include "ast.h"

#include <stdio.h>

typedef struct semantic_plan_program semantic_plan_program;

typedef enum semantic_plan_stage {
  SEMANTIC_PLAN_STAGE_NONE,
  SEMANTIC_PLAN_STAGE_SELECTION,
  SEMANTIC_PLAN_STAGE_LOWERING,
  SEMANTIC_PLAN_STAGE_OWNERSHIP,
  SEMANTIC_PLAN_STAGE_EMISSION,
} semantic_plan_stage;

typedef struct semantic_plan_diagnostic {
  semantic_plan_stage stage;
  ast_t function;
  const char *message;
} semantic_plan_diagnostic;

semantic_plan_program *
semantic_plan_prepare(ast_t program, semantic_plan_diagnostic *diagnostic);
void semantic_plan_destroy(semantic_plan_program *program);
int semantic_plan_selects(const semantic_plan_program *program, ast_t function);
const char *semantic_plan_function_symbol(const semantic_plan_program *program,
                                          ast_t function);
int semantic_plan_emit_signature(const semantic_plan_program *program,
                                 ast_t function, FILE *output,
                                 semantic_plan_diagnostic *diagnostic);
int semantic_plan_emit_body_signature(const semantic_plan_program *program,
                                      ast_t function, FILE *output,
                                      semantic_plan_diagnostic *diagnostic);
int semantic_plan_emit_body(const semantic_plan_program *program,
                            ast_t function, FILE *output,
                            semantic_plan_diagnostic *diagnostic);

#endif
