#include "semantic_ir/lower.h"
#include "semantic_ir/semantic_ir.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *message) {
  if (condition) {
    printf("PASS: %s\n", message);
  } else {
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

static sir_operation operation(sir_op_kind kind) {
  sir_operation result;
  memset(&result, 0, sizeof(result));
  result.kind = kind;
  result.primitive = SIR_PRIMITIVE_UNKNOWN;
  result.effects = sir_effects_none();
  result.result = SIR_INVALID_ID;
  result.slot = SIR_INVALID_ID;
  result.callee = SIR_INVALID_ID;
  result.parameter_index = SIZE_MAX;
  result.targets[0] = SIR_INVALID_ID;
  result.targets[1] = SIR_INVALID_ID;
  return result;
}

static ast_t build_echo_function(node_t *nodes, token_t *arguments,
                                 ast_t *types, ast_t *statements) {
  node_t *string_type = &nodes[0];
  node_t *parameter_read = &nodes[1];
  node_t *local_definition = &nodes[2];
  node_t *local_read = &nodes[3];
  node_t *return_statement = &nodes[4];
  node_t *body = &nodes[5];
  node_t *function = &nodes[6];

  memset(nodes, 0, 7 * sizeof(*nodes));
  memset(arguments, 0, sizeof(*arguments));
  string_type->tag = type;
  string_type->data.type.name = named_token("string");
  parameter_read->tag = identifier;
  parameter_read->data.identifier.id = named_token("value");
  local_definition->tag = vardef;
  local_definition->data.vardef.name = named_token("copy");
  local_definition->data.vardef.type = string_type;
  local_definition->data.vardef.expr = parameter_read;
  local_read->tag = identifier;
  local_read->data.identifier.id = named_token("copy");
  return_statement->tag = ret;
  return_statement->data.ret.expr = local_read;
  statements[0] = local_definition;
  statements[1] = return_statement;
  body->tag = compound;
  body->data.compound.stmts.data = statements;
  body->data.compound.stmts.length = 2;
  body->data.compound.stmts.capacity = 2;
  arguments[0] = named_token("value");
  types[0] = string_type;
  function->tag = fundef;
  function->data.fundef.name = named_token("echo");
  function->data.fundef.args.data = arguments;
  function->data.fundef.args.length = 1;
  function->data.fundef.args.capacity = 1;
  function->data.fundef.types.data = types;
  function->data.fundef.types.length = 1;
  function->data.fundef.types.capacity = 1;
  function->data.fundef.body = body;
  function->data.fundef.ret_type = string_type;
  return function;
}

static void test_inactive_gate(void) {
  node_t nodes[7];
  token_t arguments[1];
  ast_t types[1];
  ast_t statements[2];
  sir_function function;
  sir_lower_options options = sir_lower_default_options();
  sir_diagnostic diagnostic;
  ast_t ast = build_echo_function(nodes, arguments, types, statements);

  sir_function_init(&function);
  expect(sir_lower_function(ast, &options, &function, &diagnostic) ==
             SIR_LOWER_SKIPPED,
         "semantic lowering is inactive by default");
  expect(function.block_count == 0,
         "inactive semantic lowering leaves the destination untouched");
  sir_function_destroy(&function);
}

static void test_representative_function(void) {
  node_t nodes[7];
  token_t arguments[1];
  ast_t types[1];
  ast_t statements[2];
  sir_function function;
  sir_lower_options options = sir_lower_default_options();
  sir_diagnostic diagnostic;
  ast_t ast = build_echo_function(nodes, arguments, types, statements);

  sir_function_init(&function);
  options.enabled = 1;
  expect(sir_lower_function(ast, &options, &function, &diagnostic) ==
             SIR_LOWER_OK,
         "representative managed function lowers through the semantic gate");
  expect(sir_verify_function(&function, NULL, &diagnostic),
         "representative semantic function verifies");
  expect(function.slot_count == 2 && function.value_count == 2 &&
             function.block_count == 1,
         "semantic lowering exposes parameter, local, values, and CFG block");
  expect(function.blocks[0].operation_count == 4 &&
             function.blocks[0].operations[0].kind == SIR_OP_BORROW_SLOT &&
             function.blocks[0].operations[1].kind == SIR_OP_INIT_SLOT &&
             function.blocks[0].operations[2].kind == SIR_OP_BORROW_SLOT &&
             function.blocks[0].operations[3].kind == SIR_OP_RETURN,
         "semantic lowering contains only explicit primitive operations");
  expect(function.values[0].type.kind == SIR_TYPE_STRING &&
             function.values[0].representation == SIR_REP_STRING_DESCRIPTOR &&
             function.values[0].ownership == SIR_OWNERSHIP_BORROWED &&
             function.blocks[0].operations[0].effects.known,
         "semantic values carry exact type, representation, ownership, and "
         "effect");
  sir_function_destroy(&function);
}

static void test_lowerer_hard_errors(void) {
  node_t nodes[7];
  token_t arguments[1];
  ast_t types[1];
  ast_t statements[2];
  sir_function function;
  sir_lower_options options = sir_lower_default_options();
  sir_diagnostic diagnostic;
  ast_t ast;

  options.enabled = 1;
  sir_function_init(&function);
  ast = build_echo_function(nodes, arguments, types, statements);
  nodes[0].data.type.name = named_token("UnknownType");
  expect(sir_lower_function(ast, &options, &function, &diagnostic) ==
                 SIR_LOWER_ERROR &&
             diagnostic.code == SIR_DIAGNOSTIC_UNKNOWN_TYPE,
         "lowerer hard-errors on an unresolved type");
  sir_function_destroy(&function);

  sir_function_init(&function);
  ast = build_echo_function(nodes, arguments, types, statements);
  nodes[1].tag = funcall;
  expect(sir_lower_function(ast, &options, &function, &diagnostic) ==
                 SIR_LOWER_ERROR &&
             diagnostic.code == SIR_DIAGNOSTIC_UNRESOLVED_CALLEE,
         "lowerer hard-errors instead of hiding an unresolved call");
  sir_function_destroy(&function);

  sir_function_init(&function);
  ast = build_echo_function(nodes, arguments, types, statements);
  nodes[2].tag = ifstmt;
  expect(sir_lower_function(ast, &options, &function, &diagnostic) ==
                 SIR_LOWER_ERROR &&
             diagnostic.code == SIR_DIAGNOSTIC_UNSUPPORTED_AST,
         "lowerer hard-errors on unsupported control flow");
  sir_function_destroy(&function);
}

static void build_borrow_call_function(sir_function *function,
                                       sir_signature_parameter *parameter,
                                       sir_signature *signature,
                                       sir_environment *environment) {
  sir_slot slot;
  sir_value value;
  sir_operation borrow;
  sir_operation call;
  sir_operation return_op;
  sir_id slot_id;
  sir_id value_id;
  sir_id block;

  sir_function_init(function);
  slot.type = sir_builtin_type(SIR_TYPE_STRING);
  slot.representation = SIR_REP_STRING_DESCRIPTOR;
  slot.ownership = SIR_OWNERSHIP_OWNED;
  slot.is_parameter = 1;
  slot_id = sir_function_add_slot(function, slot);
  value.type = slot.type;
  value.representation = slot.representation;
  value.ownership = SIR_OWNERSHIP_BORROWED;
  value_id = sir_function_add_value(function, value);
  block = sir_function_add_block(function);
  function->entry_block = block;
  borrow = operation(SIR_OP_BORROW_SLOT);
  borrow.slot = slot_id;
  borrow.result = value_id;
  (void)sir_block_add_operation(function, block, &borrow);

  parameter->type = value.type;
  parameter->representation = value.representation;
  parameter->ownership = value.ownership;
  parameter->mode = SIR_ARGUMENT_BORROW;
  memset(signature, 0, sizeof(*signature));
  signature->kind = SIR_CALLEE_NATIVE;
  signature->symbol_id = 17;
  signature->c_symbol.data = "native_probe";
  signature->c_symbol.length = strlen("native_probe");
  signature->parameters = parameter;
  signature->parameter_count = 1;
  signature->return_type = sir_builtin_type(SIR_TYPE_VOID);
  signature->return_representation = SIR_REP_NONE;
  signature->return_ownership = SIR_OWNERSHIP_NONE;
  signature->effects = sir_effects_none();
  signature->effects.flags = SIR_EFFECT_CALL | SIR_EFFECT_ALLOCATE;
  environment->signatures = signature;
  environment->signature_count = 1;

  call = operation(SIR_OP_CALL);
  call.callee = 0;
  call.effects = signature->effects;
  call.operands = &value_id;
  call.operand_count = 1;
  (void)sir_block_add_operation(function, block, &call);
  return_op = operation(SIR_OP_RETURN);
  (void)sir_block_add_operation(function, block, &return_op);
}

static void test_authoritative_call_abi(void) {
  sir_function function;
  sir_signature_parameter parameter;
  sir_signature signature;
  sir_environment environment;
  sir_diagnostic diagnostic;
  sir_operation *call;
  sir_type saved_type;
  sir_representation saved_representation;
  sir_ownership saved_ownership;
  sir_argument_mode saved_mode;
  sir_effects saved_effects;

  build_borrow_call_function(&function, &parameter, &signature, &environment);
  call = &function.blocks[0].operations[1];
  expect(sir_verify_function(&function, &environment, &diagnostic),
         "resolved call matches authoritative arity, ABI, result, and effects");

  call->callee = 1;
  expect(!sir_verify_function(&function, &environment, &diagnostic) &&
             diagnostic.code == SIR_DIAGNOSTIC_UNRESOLVED_CALLEE,
         "semantic verifier rejects a nonexistent resolved callee");
  call->callee = 0;

  call->operand_count = 0;
  expect(!sir_verify_function(&function, &environment, &diagnostic) &&
             diagnostic.code == SIR_DIAGNOSTIC_CALL_ABI_MISMATCH,
         "semantic verifier rejects call arity mismatch");
  call->operand_count = 1;

  saved_type = parameter.type;
  saved_representation = parameter.representation;
  parameter.type.kind = SIR_TYPE_ARRAY;
  parameter.type.nominal_id = 23;
  parameter.representation = SIR_REP_MANAGED_HANDLE;
  expect(!sir_verify_function(&function, &environment, &diagnostic) &&
             diagnostic.code == SIR_DIAGNOSTIC_CALL_ABI_MISMATCH,
         "semantic verifier rejects call type and representation mismatch");
  parameter.type = saved_type;
  parameter.representation = saved_representation;

  saved_ownership = parameter.ownership;
  saved_mode = parameter.mode;
  parameter.ownership = SIR_OWNERSHIP_OWNED;
  parameter.mode = SIR_ARGUMENT_CONSUME;
  expect(!sir_verify_function(&function, &environment, &diagnostic) &&
             diagnostic.code == SIR_DIAGNOSTIC_CALL_ABI_MISMATCH,
         "semantic verifier rejects call ownership-mode mismatch");
  parameter.ownership = saved_ownership;
  parameter.mode = saved_mode;

  signature.return_type = sir_builtin_type(SIR_TYPE_INT);
  signature.return_representation = SIR_REP_SCALAR;
  signature.return_ownership = SIR_OWNERSHIP_SCALAR;
  expect(!sir_verify_function(&function, &environment, &diagnostic) &&
             diagnostic.code == SIR_DIAGNOSTIC_CALL_ABI_MISMATCH,
         "semantic verifier rejects call result ABI mismatch");
  signature.return_type = sir_builtin_type(SIR_TYPE_VOID);
  signature.return_representation = SIR_REP_NONE;
  signature.return_ownership = SIR_OWNERSHIP_NONE;

  saved_effects = signature.effects;
  call->effects.flags = SIR_EFFECT_CALL;
  expect(!sir_verify_function(&function, &environment, &diagnostic) &&
             diagnostic.code == SIR_DIAGNOSTIC_CALL_ABI_MISMATCH,
         "semantic verifier rejects caller-authored effect mismatch");
  call->effects = saved_effects;

  call->kind = SIR_OP_TERMINAL_CALL;
  expect(!sir_verify_function(&function, &environment, &diagnostic) &&
             diagnostic.code == SIR_DIAGNOSTIC_CALL_ABI_MISMATCH,
         "semantic verifier rejects a returning callee as a terminal call");
  call->kind = SIR_OP_CALL;

  signature.effects.flags |= SIR_EFFECT_NONRETURNING;
  call->effects = signature.effects;
  expect(!sir_verify_function(&function, &environment, &diagnostic) &&
             diagnostic.code == SIR_DIAGNOSTIC_CALL_ABI_MISMATCH,
         "semantic verifier rejects nonreturning callee as an ordinary call");
  sir_function_destroy(&function);
}

static void test_consuming_user_call_requires_prepared_arguments(void) {
  sir_function function;
  sir_signature_parameter parameter;
  sir_signature signature;
  sir_environment environment;
  sir_diagnostic diagnostic;
  sir_slot slot;
  sir_value value;
  sir_operation op;
  sir_id block;
  sir_id slot_id;
  sir_id borrowed;
  sir_id prepared;
  sir_id result;

  sir_function_init(&function);
  function.return_type = sir_builtin_type(SIR_TYPE_VOID);
  function.return_representation = SIR_REP_NONE;
  function.return_ownership = SIR_OWNERSHIP_NONE;
  slot.type = sir_builtin_type(SIR_TYPE_STRING);
  slot.representation = SIR_REP_STRING_DESCRIPTOR;
  slot.ownership = SIR_OWNERSHIP_OWNED;
  slot.is_parameter = 1;
  slot.name.data = "value";
  slot.name.length = strlen("value");
  slot_id = sir_function_add_slot(&function, slot);
  value.type = slot.type;
  value.representation = slot.representation;
  value.ownership = SIR_OWNERSHIP_BORROWED;
  borrowed = sir_function_add_value(&function, value);
  value.ownership = SIR_OWNERSHIP_OWNED;
  prepared = sir_function_add_value(&function, value);
  result = sir_function_add_value(&function, value);
  parameter.type = value.type;
  parameter.representation = value.representation;
  parameter.ownership = SIR_OWNERSHIP_OWNED;
  parameter.mode = SIR_ARGUMENT_CONSUME;
  memset(&signature, 0, sizeof(signature));
  signature.kind = SIR_CALLEE_USER;
  signature.c_symbol.data = "rift_plan_body_1";
  signature.c_symbol.length = strlen("rift_plan_body_1");
  signature.parameters = &parameter;
  signature.parameter_count = 1;
  signature.return_type = value.type;
  signature.return_representation = value.representation;
  signature.return_ownership = value.ownership;
  signature.effects = sir_effects_none();
  signature.effects.flags = SIR_EFFECT_CALL;
  environment.signatures = &signature;
  environment.signature_count = 1;
  block = sir_function_add_block(&function);
  function.entry_block = block;
  op = operation(SIR_OP_BORROW_SLOT);
  op.slot = slot_id;
  op.result = borrowed;
  (void)sir_block_add_operation(&function, block, &op);
  op = operation(SIR_OP_PREPARE_ARGUMENT);
  op.callee = 0;
  op.parameter_index = 0;
  op.operands = &borrowed;
  op.operand_count = 1;
  op.result = prepared;
  (void)sir_block_add_operation(&function, block, &op);
  op = operation(SIR_OP_CALL);
  op.callee = 0;
  op.effects = signature.effects;
  op.operands = &prepared;
  op.operand_count = 1;
  op.result = result;
  (void)sir_block_add_operation(&function, block, &op);
  op = operation(SIR_OP_EXPRESSION_END);
  op.operands = &result;
  op.operand_count = 1;
  (void)sir_block_add_operation(&function, block, &op);
  op = operation(SIR_OP_RETURN);
  (void)sir_block_add_operation(&function, block, &op);

  expect(sir_verify_function(&function, &environment, &diagnostic),
         "semantic verifier accepts an explicitly prepared consuming call");
  function.blocks[0].operations[2].operands[0] = borrowed;
  expect(!sir_verify_function(&function, &environment, &diagnostic) &&
             diagnostic.code == SIR_DIAGNOSTIC_CALL_ABI_MISMATCH,
         "semantic verifier rejects a consuming call without its prepared "
         "owned argument");
  function.blocks[0].operations[2].operands[0] = prepared;
  function.blocks[0].operations[1].parameter_index = 1;
  expect(!sir_verify_function(&function, &environment, &diagnostic) &&
             diagnostic.code == SIR_DIAGNOSTIC_INVALID_OPERATION,
         "semantic verifier rejects argument preparation for the wrong "
         "parameter");
  sir_function_destroy(&function);
}

static void test_unknown_type_rejected(void) {
  sir_function function;
  sir_diagnostic diagnostic;
  sir_value value;
  sir_operation constant;
  sir_operation return_op;
  sir_id block;
  sir_id value_id;

  sir_function_init(&function);
  function.return_type = sir_builtin_type(SIR_TYPE_INT);
  function.return_representation = SIR_REP_SCALAR;
  function.return_ownership = SIR_OWNERSHIP_SCALAR;
  value.type = sir_builtin_type(SIR_TYPE_UNKNOWN);
  value.representation = SIR_REP_SCALAR;
  value.ownership = SIR_OWNERSHIP_SCALAR;
  value_id = sir_function_add_value(&function, value);
  block = sir_function_add_block(&function);
  function.entry_block = block;
  constant = operation(SIR_OP_CONSTANT);
  constant.result = value_id;
  (void)sir_block_add_operation(&function, block, &constant);
  return_op = operation(SIR_OP_RETURN);
  return_op.operands = &value_id;
  return_op.operand_count = 1;
  (void)sir_block_add_operation(&function, block, &return_op);
  expect(!sir_verify_function(&function, NULL, &diagnostic) &&
             diagnostic.code == SIR_DIAGNOSTIC_UNKNOWN_TYPE,
         "semantic verifier rejects an unknown type");
  sir_function_destroy(&function);
}

static void test_unknown_effect_rejected(void) {
  sir_function function;
  sir_diagnostic diagnostic;
  sir_operation return_op;
  sir_id block;

  sir_function_init(&function);
  block = sir_function_add_block(&function);
  function.entry_block = block;
  return_op = operation(SIR_OP_RETURN);
  return_op.effects = sir_effects_unknown();
  (void)sir_block_add_operation(&function, block, &return_op);
  expect(!sir_verify_function(&function, NULL, &diagnostic) &&
             diagnostic.code == SIR_DIAGNOSTIC_UNKNOWN_EFFECT,
         "semantic verifier rejects an unknown effect");
  sir_function_destroy(&function);
}

static void build_hidden_operation_function(sir_function *function,
                                            sir_op_kind kind,
                                            sir_primitive primitive) {
  sir_value value;
  sir_operation hidden;
  sir_operation return_op;
  sir_id block;
  sir_id value_id;

  sir_function_init(function);
  function->return_type = sir_builtin_type(SIR_TYPE_INT);
  function->return_representation = SIR_REP_SCALAR;
  function->return_ownership = SIR_OWNERSHIP_SCALAR;
  value.type = function->return_type;
  value.representation = SIR_REP_SCALAR;
  value.ownership = SIR_OWNERSHIP_SCALAR;
  value_id = sir_function_add_value(function, value);
  block = sir_function_add_block(function);
  function->entry_block = block;
  hidden = operation(kind);
  hidden.primitive = primitive;
  hidden.result = value_id;
  (void)sir_block_add_operation(function, block, &hidden);
  return_op = operation(SIR_OP_RETURN);
  return_op.operands = &value_id;
  return_op.operand_count = 1;
  (void)sir_block_add_operation(function, block, &return_op);
}

static void test_hidden_expression_rejected(void) {
  sir_function function;
  sir_diagnostic diagnostic;
  build_hidden_operation_function(&function, SIR_OP_OPAQUE_EXPRESSION,
                                  SIR_PRIMITIVE_UNKNOWN);
  expect(!sir_verify_function(&function, NULL, &diagnostic) &&
             diagnostic.code == SIR_DIAGNOSTIC_HIDDEN_EXPRESSION,
         "semantic verifier rejects a hidden expression");
  sir_function_destroy(&function);
}

static void test_hidden_call_rejected(void) {
  sir_function function;
  sir_diagnostic diagnostic;
  build_hidden_operation_function(&function, SIR_OP_PRIMITIVE,
                                  SIR_PRIMITIVE_HIDDEN_CALL);
  expect(!sir_verify_function(&function, NULL, &diagnostic) &&
             diagnostic.code == SIR_DIAGNOSTIC_HIDDEN_CALL,
         "semantic verifier rejects a call hidden inside a primitive");
  sir_function_destroy(&function);
}

int main(void) {
  test_inactive_gate();
  test_representative_function();
  test_lowerer_hard_errors();
  test_authoritative_call_abi();
  test_consuming_user_call_requires_prepared_arguments();
  test_unknown_type_rejected();
  test_unknown_effect_rejected();
  test_hidden_expression_rejected();
  test_hidden_call_rejected();
  if (failures != 0) {
    fprintf(stderr, "%d semantic IR test(s) failed\n", failures);
    return 1;
  }
  printf("All semantic IR tests passed\n");
  return 0;
}
