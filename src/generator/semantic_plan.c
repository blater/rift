#include "generator/semantic_plan.h"

#include "ownership_plan/ownership_plan.h"
#include "plan_c_emitter/plan_c_emitter.h"
#include "semantic_ir/intrinsics.h"
#include "semantic_ir/lower.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct semantic_plan_function {
  ast_t ast;
  ownership_plan *plan;
  char *external_symbol;
  char *body_symbol;
} semantic_plan_function;

struct semantic_plan_program {
  semantic_plan_function *functions;
  size_t function_count;
  sir_signature *signatures;
  size_t user_signature_count;
  sir_lower_callee *callees;
  sir_environment environment;
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

static char *make_symbol(const char *prefix, size_t index) {
  char buffer[64];
  int length = snprintf(buffer, sizeof(buffer), "%s%zu", prefix, index);
  if (length < 0 || (size_t)length >= sizeof(buffer))
    return NULL;
  return strdup(buffer);
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
  size_t selected_count = 0;
  size_t signature_count;
  size_t selected_index = 0;
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
    if (!selected_function(function))
      continue;
    if (function->data.fundef.method_kind != METHOD_NONE) {
      set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_SELECTION, function,
                     "semantic-plan checkpoint does not select methods");
      goto failure;
    }
    selected_count++;
  }
  signature_count = selected_count + sir_intrinsic_count();
  if (selected_count != 0) {
    planned->functions = calloc(selected_count, sizeof(*planned->functions));
    planned->callees = calloc(selected_count, sizeof(*planned->callees));
    if (planned->functions == NULL || planned->callees == NULL) {
      set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_SELECTION, NULL,
                     "could not allocate semantic-plan function table");
      goto failure;
    }
  }
  if (signature_count != 0) {
    planned->signatures = calloc(signature_count, sizeof(*planned->signatures));
    if (planned->signatures == NULL) {
      set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_SELECTION, NULL,
                     "could not allocate semantic-plan signature table");
      goto failure;
    }
  }
  planned->function_count = selected_count;
  planned->user_signature_count = selected_count;
  for (index = 0; index < (size_t)statements.length; index++) {
    ast_t function = statements.data[index];
    semantic_plan_function *entry;
    sir_diagnostic semantic_diagnostic;
    string_view body_symbol;
    if (!selected_function(function))
      continue;
    entry = &planned->functions[selected_index];
    entry->ast = function;
    entry->external_symbol = make_symbol("rift_plan_fn_", index);
    entry->body_symbol = make_symbol("rift_plan_body_", index);
    if (entry->external_symbol == NULL || entry->body_symbol == NULL) {
      set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_SELECTION, function,
                     "could not assign semantic-plan function symbols");
      goto failure;
    }
    body_symbol = sv_from_cstr(entry->body_symbol);
    if (!sir_lower_signature(function, (uint32_t)selected_index, body_symbol,
                             &planned->signatures[selected_index],
                             &semantic_diagnostic)) {
      set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_LOWERING, function,
                     semantic_diagnostic.message);
      goto failure;
    }
    planned->callees[selected_index].definition = function;
    planned->callees[selected_index].signature = (sir_id)selected_index;
    selected_index++;
  }
  for (index = 0; index < sir_intrinsic_count(); index++) {
    const sir_intrinsic_descriptor *intrinsic = sir_intrinsic_at(index);
    planned->signatures[selected_count + index] = intrinsic->signature;
  }
  planned->environment.signatures = planned->signatures;
  planned->environment.signature_count = signature_count;
  for (selected_index = 0; selected_index < selected_count; selected_index++) {
    semantic_plan_function *entry = &planned->functions[selected_index];
    ast_t function = entry->ast;
    sir_function semantic;
    sir_lower_options options;
    sir_diagnostic semantic_diagnostic;
    ownership_diagnostic ownership_diagnostic;
    plan_c_diagnostic emitter_diagnostic;
    ownership_plan *plan;
    sir_function_init(&semantic);
    options = sir_lower_default_options();
    options.enabled = 1;
    options.environment = &planned->environment;
    options.callees = planned->callees;
    options.callee_count = selected_count;
    if (sir_lower_function(function, &options, &semantic,
                           &semantic_diagnostic) != SIR_LOWER_OK) {
      set_diagnostic(diagnostic, SEMANTIC_PLAN_STAGE_LOWERING, function,
                     semantic_diagnostic.message);
      sir_function_destroy(&semantic);
      goto failure;
    }
    semantic.external_symbol = sv_from_cstr(entry->external_symbol);
    semantic.c_symbol = sv_from_cstr(entry->body_symbol);
    plan = ownership_plan_build(&semantic, &planned->environment,
                                &ownership_diagnostic);
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
    entry->plan = plan;
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
  for (index = 0; index < program->user_signature_count; index++)
    sir_lower_signature_destroy(&program->signatures[index]);
  for (index = 0; index < program->function_count; index++) {
    ownership_plan_destroy(program->functions[index].plan);
    free(program->functions[index].external_symbol);
    free(program->functions[index].body_symbol);
  }
  free(program->callees);
  free(program->signatures);
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
  return declaration.external_symbol;
}

int semantic_plan_parameter_count(const semantic_plan_program *program,
                                  ast_t function, size_t *count) {
  const semantic_plan_function *selected = find_function(program, function);
  size_t slot_count;
  size_t index;
  int saw_local = 0;
  if (selected == NULL || count == NULL ||
      !ownership_plan_is_verified(selected->plan))
    return 0;
  *count = 0;
  slot_count = ownership_plan_slot_count(selected->plan);
  for (index = 0; index < slot_count; index++) {
    ownership_slot_view slot;
    if (!ownership_plan_slot_at(selected->plan, (ownership_id)index, &slot))
      return 0;
    if (!slot.is_parameter) {
      saw_local = 1;
      continue;
    }
    if (saw_local)
      return 0;
    (*count)++;
  }
  return 1;
}

int semantic_plan_parameter_at(const semantic_plan_program *program,
                               ast_t function, size_t index,
                               semantic_plan_parameter_abi *parameter) {
  const semantic_plan_function *selected = find_function(program, function);
  const plan_c_abi *abi = plan_c_rift_abi();
  ownership_slot_view slot;
  size_t parameter_count;
  if (parameter == NULL ||
      !semantic_plan_parameter_count(program, function, &parameter_count) ||
      index >= parameter_count || selected == NULL ||
      !ownership_plan_slot_at(selected->plan, (ownership_id)index, &slot) ||
      !slot.is_parameter)
    return 0;
  if (slot.type.kind == SIR_TYPE_BOOL &&
      slot.representation == SIR_REP_SCALAR &&
      slot.ownership == SIR_OWNERSHIP_SCALAR) {
    parameter->kind = SEMANTIC_PLAN_PARAMETER_BOOL_SCALAR;
    parameter->c_type = abi->bool_type;
    return 1;
  }
  if (slot.type.kind == SIR_TYPE_STRING &&
      slot.representation == SIR_REP_STRING_DESCRIPTOR &&
      slot.ownership == SIR_OWNERSHIP_OWNED) {
    parameter->kind = SEMANTIC_PLAN_PARAMETER_STRING_CONSUME;
    parameter->c_type = abi->string_type;
    return 1;
  }
  if (slot.type.kind == SIR_TYPE_ARRAY &&
      slot.type.nominal_id == SIR_TYPE_STRING &&
      slot.representation == SIR_REP_MANAGED_HANDLE &&
      slot.ownership == SIR_OWNERSHIP_OWNED) {
    parameter->kind = SEMANTIC_PLAN_PARAMETER_STRING_ARRAY_CONSUME;
    parameter->c_type = abi->array_type;
    return 1;
  }
  return 0;
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

int semantic_plan_emit_body_signature(const semantic_plan_program *program,
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
  if (!plan_c_emit_body_signature(output, selected->plan, plan_c_rift_abi(),
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
