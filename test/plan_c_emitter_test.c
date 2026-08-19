#include "ownership_plan/ownership_plan.h"
#include "plan_c_emitter/plan_c_emitter.h"
#include "semantic_ir/lower.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *message) {
  if (condition)
    printf("PASS: %s\n", message);
  else {
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
  }
}

static token_t named_token(char *name) {
  token_t token;
  memset(&token, 0, sizeof(token));
  token.type = TOK_IDENTIFIER;
  token.lexeme.data = name;
  token.lexeme.length = strlen(name);
  return token;
}

static ast_t build_copy_function(node_t *nodes, token_t *arguments,
                                 ast_t *types, ast_t *statements,
                                 char *function_name, char *type_name,
                                 char *parameter_name, char *local_name) {
  memset(nodes, 0, 7 * sizeof(*nodes));
  nodes[0].tag = type;
  nodes[0].data.type.name = named_token(type_name);
  nodes[1].tag = identifier;
  nodes[1].data.identifier.id = named_token(parameter_name);
  nodes[2].tag = vardef;
  nodes[2].data.vardef.name = named_token(local_name);
  nodes[2].data.vardef.type = &nodes[0];
  nodes[2].data.vardef.expr = &nodes[1];
  nodes[3].tag = identifier;
  nodes[3].data.identifier.id = named_token(local_name);
  nodes[4].tag = ret;
  nodes[4].data.ret.expr = &nodes[3];
  statements[0] = &nodes[2];
  statements[1] = &nodes[4];
  nodes[5].tag = compound;
  nodes[5].data.compound.stmts.data = statements;
  nodes[5].data.compound.stmts.length = 2;
  nodes[5].data.compound.stmts.capacity = 2;
  arguments[0] = named_token(parameter_name);
  types[0] = &nodes[0];
  nodes[6].tag = fundef;
  nodes[6].data.fundef.name = named_token(function_name);
  nodes[6].data.fundef.args.data = arguments;
  nodes[6].data.fundef.args.length = 1;
  nodes[6].data.fundef.args.capacity = 1;
  nodes[6].data.fundef.types.data = types;
  nodes[6].data.fundef.types.length = 1;
  nodes[6].data.fundef.types.capacity = 1;
  nodes[6].data.fundef.body = &nodes[5];
  nodes[6].data.fundef.ret_type = &nodes[0];
  return &nodes[6];
}

static ownership_plan *build_named_plan_with_symbol(char *function_name,
                                                    char *c_symbol,
                                                    char *type_name,
                                                    char *parameter_name,
                                                    char *local_name) {
  node_t nodes[7];
  token_t arguments[1];
  ast_t types[1];
  ast_t statements[2];
  sir_function semantic;
  sir_lower_options options = sir_lower_default_options();
  sir_diagnostic semantic_diagnostic;
  ownership_diagnostic ownership_diagnostic;
  ownership_plan *plan;
  ast_t ast =
      build_copy_function(nodes, arguments, types, statements, function_name,
                          type_name, parameter_name, local_name);
  sir_function_init(&semantic);
  options.enabled = 1;
  if (sir_lower_function(ast, &options, &semantic, &semantic_diagnostic) !=
      SIR_LOWER_OK) {
    sir_function_destroy(&semantic);
    return NULL;
  }
  semantic.c_symbol.data = c_symbol;
  semantic.c_symbol.length = strlen(c_symbol);
  plan = ownership_plan_build(&semantic, NULL, &ownership_diagnostic);
  sir_function_destroy(&semantic);
  return plan;
}

static ownership_plan *build_named_plan(char *function_name, char *type_name,
                                        char *parameter_name,
                                        char *local_name) {
  return build_named_plan_with_symbol(function_name, "rift_plan_fn_0",
                                      type_name, parameter_name, local_name);
}

static ownership_plan *build_plan(char *function_name, char *type_name) {
  return build_named_plan(function_name, type_name, "value", "copy");
}

static void expect_invalid_declaration(char *name, const char *message) {
  ownership_plan *plan =
      build_named_plan_with_symbol("echo", name, "string", "value", "copy");
  plan_c_diagnostic diagnostic;
  char *text = NULL;
  size_t size = 0;
  FILE *output = open_memstream(&text, &size);
  expect(plan != NULL && output != NULL &&
             !plan_c_emit_function_body(output, plan, plan_c_rift_abi(),
                                        &diagnostic) &&
             diagnostic.code == PLAN_C_DIAGNOSTIC_INVALID_DECLARATION,
         message);
  fflush(output);
  expect(size == 0, "invalid C declaration writes no output bytes");
  fclose(output);
  free(text);
  ownership_plan_destroy(plan);
}

static void test_plan_only_emission(void) {
  ownership_plan *plan = build_plan("echo", "string");
  ownership_function_view function;
  ownership_slot_view parameter;
  ownership_token_view token;
  plan_c_diagnostic diagnostic;
  char *text = NULL;
  size_t size = 0;
  FILE *output = open_memstream(&text, &size);
  expect(plan != NULL && ownership_plan_function(plan, &function) &&
             strcmp(function.name, "echo") == 0 &&
             strcmp(function.c_symbol, "rift_plan_fn_0") == 0,
         "sealed plan owns its function declaration");
  expect(ownership_plan_slot_at(plan, 0, &parameter) &&
             strcmp(parameter.name, "value") == 0 && parameter.is_parameter &&
             ownership_plan_token_at(plan, parameter.token, &token) &&
             token.origin_kind == OWNERSHIP_ORIGIN_SLOT && token.origin == 0,
         "sealed plan exposes immutable slot and token origin metadata");
  expect(output != NULL && plan_c_emit_function_body(
                               output, plan, plan_c_rift_abi(), &diagnostic),
         "verified string plan emits without consulting AST state");
  fclose(output);
  expect(strstr(text, "rift_bump") == NULL,
         "plan body contains no legacy body-lowering scaffolding");
  {
    char *release_copy = strstr(text, "__string_release(rift_plan_slot_1)");
    char *release_value = strstr(text, "__string_release(rift_plan_slot_0)");
    expect(release_copy != NULL && release_value != NULL &&
               release_copy < release_value,
           "plan output preserves reverse cleanup order");
  }
  free(text);
  ownership_plan_destroy(plan);
}

static void test_slot_name_encoding(void) {
  ownership_plan *plan =
      build_named_plan("echo_slots", "string", "restrict", "asm");
  plan_c_diagnostic diagnostic;
  char *text = NULL;
  size_t size = 0;
  FILE *output = open_memstream(&text, &size);
  expect(plan != NULL && output != NULL &&
             plan_c_emit_function_signature(output, plan, plan_c_rift_abi(),
                                            &diagnostic) &&
             fputc('\n', output) != EOF &&
             plan_c_emit_function_body(output, plan, plan_c_rift_abi(),
                                       &diagnostic),
         "C keywords in Rift slots emit through canonical slot IDs");
  fclose(output);
  expect(strstr(text, "string rift_plan_fn_0(string rift_plan_slot_0)") !=
                 NULL &&
             strstr(text, "string rift_plan_slot_1 =") != NULL &&
             strstr(text, "string restrict") == NULL &&
             strstr(text, "string asm") == NULL,
         "source slot names never enter the emitted C namespace");
  free(text);
  ownership_plan_destroy(plan);

  plan =
      build_named_plan("echo_collision", "string", "__rift_plan_t_2", "copy");
  text = NULL;
  size = 0;
  output = open_memstream(&text, &size);
  expect(plan != NULL && output != NULL &&
             plan_c_emit_function_signature(output, plan, plan_c_rift_abi(),
                                            &diagnostic) &&
             fputc('\n', output) != EOF &&
             plan_c_emit_function_body(output, plan, plan_c_rift_abi(),
                                       &diagnostic),
         "Rift slot names cannot collide with generated temporaries");
  fclose(output);
  expect(strstr(text, "__rift_plan_t_2") == NULL &&
             strstr(text, "rift_plan_slot_0") != NULL,
         "temporary-prefix source names are encoded as canonical slots");
  free(text);
  ownership_plan_destroy(plan);
}

static void test_preflight_is_output_free(void) {
  ownership_plan *plan = build_plan("echo", "string");
  plan_c_abi bad_abi = *plan_c_rift_abi();
  plan_c_diagnostic diagnostic;
  char *text = NULL;
  size_t size = 0;
  FILE *output = open_memstream(&text, &size);
  bad_abi.string_retain = "";
  expect(!plan_c_emit_function_body(output, plan, &bad_abi, &diagnostic) &&
             diagnostic.code == PLAN_C_DIAGNOSTIC_INVALID_ABI,
         "bad target ABI is rejected before emission");
  fflush(output);
  expect(size == 0, "bad ABI rejection writes no output bytes");
  fclose(output);
  free(text);
  ownership_plan_destroy(plan);

  plan = build_named_plan_with_symbol("echo", "bad-name", "string", "value",
                                      "copy");
  text = NULL;
  size = 0;
  output = open_memstream(&text, &size);
  expect(!plan_c_emit_function_body(output, plan, plan_c_rift_abi(),
                                    &diagnostic) &&
             diagnostic.code == PLAN_C_DIAGNOSTIC_INVALID_DECLARATION,
         "bad sealed declaration is rejected before emission");
  fflush(output);
  expect(size == 0, "bad declaration rejection writes no output bytes");
  fclose(output);
  free(text);
  ownership_plan_destroy(plan);

  expect_invalid_declaration("restrict",
                             "C keyword function symbol is rejected preflight");
  expect_invalid_declaration(
      "asm", "C extension function symbol is rejected preflight");
  expect_invalid_declaration(
      "rift_plan_tmp_2",
      "generated temporary function namespace is rejected preflight");

  plan = build_plan("echo", "string");
  {
    plan_c_abi internal_abi = *plan_c_rift_abi();
    internal_abi.string_retain = "rift_plan_tmp_0";
    text = NULL;
    size = 0;
    output = open_memstream(&text, &size);
    expect(
        !plan_c_emit_function_body(output, plan, &internal_abi, &diagnostic) &&
            diagnostic.code == PLAN_C_DIAGNOSTIC_INVALID_ABI,
        "generated temporary ABI namespace is rejected preflight");
    fflush(output);
    expect(size == 0, "internal-prefix ABI rejection writes no output bytes");
    fclose(output);
    free(text);
  }
  ownership_plan_destroy(plan);

  plan = build_plan("copy_int", "int");
  text = NULL;
  size = 0;
  output = open_memstream(&text, &size);
  expect(!plan_c_emit_function_body(output, plan, plan_c_rift_abi(),
                                    &diagnostic) &&
             diagnostic.code == PLAN_C_DIAGNOSTIC_UNSUPPORTED_PLAN,
         "unsupported verified opcode is rejected before emission");
  fflush(output);
  expect(size == 0, "unsupported opcode rejection writes no output bytes");
  fclose(output);
  free(text);
  ownership_plan_destroy(plan);
}

int main(void) {
  test_plan_only_emission();
  test_slot_name_encoding();
  test_preflight_is_output_free();
  return failures == 0 ? 0 : 1;
}
