#include "generator/semantic_plan.h"

#include "ownership_plan/ownership_plan.h"
#include "plan_c_emitter/plan_c_emitter.h"
#include "semantic_ir/lower.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct semantic_plan_function {
  ast_t ast;
  ownership_plan *plan;
} semantic_plan_function;

struct semantic_plan_program {
  semantic_plan_function *functions;
  size_t function_count;
  size_t function_capacity;
};

static void set_diagnostic(semantic_plan_diagnostic *diagnostic,
                           semantic_plan_stage stage, ast_t function,
                           const char *message) {
  if (diagnostic == NULL)
    return;
  diagnostic->stage = stage;
  diagnostic->function = function;
  diagnostic->message = message;
}

static int view_is(string_view view, const char *text) {
  size_t length = strlen(text);
  return view.length == length && memcmp(view.data, text, length) == 0;
}

static int selected_function(ast_t statement) {
  return statement != NULL && statement->tag == fundef &&
         statement->data.fundef.body != NULL &&
         !view_is(statement->data.fundef.name.lexeme, "main");
}

static int reserve_functions(semantic_plan_program *program, size_t required) {
  semantic_plan_function *functions;
  size_t capacity;
  if (program->function_capacity >= required)
    return 1;
  capacity = program->function_capacity == 0 ? 4 : program->function_capacity;
  while (capacity < required) {
    if (capacity > SIZE_MAX / 2)
      return 0;
    capacity *= 2;
  }
  if (capacity > SIZE_MAX / sizeof(*functions))
    return 0;
  functions = realloc(program->functions, capacity * sizeof(*functions));
  if (functions == NULL)
    return 0;
  program->functions = functions;
  program->function_capacity = capacity;
  return 1;
}

static const semantic_plan_function *
find_function(const semantic_plan_program *program, ast_t function) {
  size_t index;
  if (program == NULL)
    return NULL;
  for (index = 0; index < program->function_count; index++) {
    if (program->functions[index].ast == function)
      return &program->functions[index];
  }
  return NULL;
}

semantic_plan_program *
semantic_plan_prepare(ast_t program_ast, semantic_plan_diagnostic *diagnostic) {
  semantic_plan_program *planned;
  ast_array_t statements;
  size_t index;
  set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_NONE, NULL, NULL);
  if (program_ast == NULL || program_ast->tag != program) {
    set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_SELECTION, NULL,
                   "semantic-plan mode requires a complete program");
    return NULL;
  }
  planned = calloc(1, sizeof(*planned));
  if (planned == NULL) {
    set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_SELECTION, NULL,
                   "could not allocate semantic-plan program");
    return NULL;
  }
  statements = program_ast->data.program.prog;
  for (index = 0; index < (size_t)statements.length; index++) {
    ast_t function = statements.data[index];
    sir_function semantic;
    sir_lower_options options;
    sir_diagnostic semantic_diagnostic;
    ownership_diagnostic ownership_diagnostic;
    plan_c_diagnostic emitter_diagnostic;
    ownership_plan *plan;
    char symbol[64];
    int symbol_length;
    if (!selected_function(function))
      continue;
    if (function->data.fundef.method_kind != METHOD_NONE) {
      set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_SELECTION, function,
                     "semantic-plan checkpoint does not select methods");
      goto failure;
    }
    sir_function_init(&semantic);
    options = sir_lower_default_options();
    options.enabled = 1;
    if (sir_lower_function(function, &options, &semantic,
                           &semantic_diagnostic) != SIR_LOWER_OK) {
      set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_LOWERING, function,
                     semantic_diagnostic.message);
      sir_function_destroy(&semantic);
      goto failure;
    }
    symbol_length = snprintf(symbol, sizeof(symbol), "rift_plan_fn_%zu", index);
    if (symbol_length < 0 || (size_t)symbol_length >= sizeof(symbol)) {
      set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_SELECTION, function,
                     "could not assign semantic-plan function symbol");
      sir_function_destroy(&semantic);
      goto failure;
    }
    semantic.c_symbol = sv_from_parts(symbol, (size_t)symbol_length);
    plan = ownership_plan_build(&semantic, NULL, &ownership_diagnostic);
    sir_function_destroy(&semantic);
    if (plan == NULL) {
      set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_OWNERSHIP, function,
                     ownership_diagnostic.message);
      goto failure;
    }
    if (!plan_c_validate(plan, plan_c_rift_abi(), &emitter_diagnostic)) {
      set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_EMISSION, function,
                     emitter_diagnostic.message);
      ownership_plan_destroy(plan);
      goto failure;
    }
    if (!reserve_functions(planned, planned->function_count + 1)) {
      set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_SELECTION, function,
                     "could not allocate semantic-plan function table");
      ownership_plan_destroy(plan);
      goto failure;
    }
    planned->functions[planned->function_count].ast = function;
    planned->functions[planned->function_count].plan = plan;
    planned->function_count++;
  }
  return planned;

failure:
  semantic_plan_destroy(planned);
  return NULL;
}

void semantic_plan_destroy(semantic_plan_program *program) {
  size_t index;
  if (program == NULL)
    return;
  for (index = 0; index < program->function_count; index++)
    ownership_plan_destroy(program->functions[index].plan);
  free(program->functions);
  free(program);
}

int semantic_plan_selects(const semantic_plan_program *program,
                          ast_t function) {
  return find_function(program, function) != NULL;
}

const char *semantic_plan_function_symbol(const semantic_plan_program *program,
                                          ast_t function) {
  const semantic_plan_function *selected = find_function(program, function);
  ownership_function_view declaration;
  if (selected == NULL ||
      !ownership_plan_function(selected->plan, &declaration)) {
    return NULL;
  }
  return declaration.c_symbol;
}

int semantic_plan_emit_signature(const semantic_plan_program *program,
                                 ast_t function, FILE *output,
                                 semantic_plan_diagnostic *diagnostic) {
  const semantic_plan_function *selected = find_function(program, function);
  plan_c_diagnostic emitter_diagnostic;
  set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_NONE, function, NULL);
  if (selected == NULL) {
    set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_SELECTION, function,
                   "function is not selected by the semantic plan");
    return 0;
  }
  if (!plan_c_emit_function_signature(output, selected->plan, plan_c_rift_abi(),
                                      &emitter_diagnostic)) {
    set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_EMISSION, function,
                   emitter_diagnostic.message);
    return 0;
  }
  return 1;
}

int semantic_plan_emit_body(const semantic_plan_program *program,
                            ast_t function, FILE *output,
                            semantic_plan_diagnostic *diagnostic) {
  const semantic_plan_function *selected = find_function(program, function);
  plan_c_diagnostic emitter_diagnostic;
  set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_NONE, function, NULL);
  if (selected == NULL) {
    set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_SELECTION, function,
                   "function is not selected by the semantic plan");
    return 0;
  }
  if (!plan_c_emit_function_body(output, selected->plan, plan_c_rift_abi(),
                                 &emitter_diagnostic)) {
    set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_EMISSION, function,
                   emitter_diagnostic.message);
    return 0;
  }
  return 1;
}
