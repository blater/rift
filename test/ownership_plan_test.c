#include "ownership_plan/internal.h"
#include "semantic_ir/intrinsics.h"
#include "semantic_ir/lower.h"

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

static ast_t build_echo_function(node_t *nodes, token_t *arguments,
                                 ast_t *types, ast_t *statements) {
  memset(nodes, 0, 7 * sizeof(*nodes));
  memset(arguments, 0, sizeof(*arguments));
  nodes[0].tag = type;
  nodes[0].data.type.name = named_token("string");
  nodes[1].tag = identifier;
  nodes[1].data.identifier.id = named_token("value");
  nodes[2].tag = vardef;
  nodes[2].data.vardef.name = named_token("copy");
  nodes[2].data.vardef.type = &nodes[0];
  nodes[2].data.vardef.expr = &nodes[1];
  nodes[3].tag = identifier;
  nodes[3].data.identifier.id = named_token("copy");
  nodes[4].tag = ret;
  nodes[4].data.ret.expr = &nodes[3];
  statements[0] = &nodes[2];
  statements[1] = &nodes[4];
  nodes[5].tag = compound;
  nodes[5].data.compound.stmts.data = statements;
  nodes[5].data.compound.stmts.length = 2;
  nodes[5].data.compound.stmts.capacity = 2;
  arguments[0] = named_token("value");
  types[0] = &nodes[0];
  nodes[6].tag = fundef;
  nodes[6].data.fundef.name = named_token("echo");
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

static ownership_operation operation(ownership_op_kind kind) {
  ownership_operation result;
  memset(&result, 0, sizeof(result));
  result.kind = kind;
  result.result = OWNERSHIP_INVALID_ID;
  result.operand = OWNERSHIP_INVALID_ID;
  result.callee = OWNERSHIP_INVALID_ID;
  result.parameter_index = SIZE_MAX;
  result.targets[0] = OWNERSHIP_INVALID_ID;
  result.targets[1] = OWNERSHIP_INVALID_ID;
  return result;
}

static sir_operation semantic_operation(sir_op_kind kind) {
  sir_operation result;
  memset(&result, 0, sizeof(result));
  result.kind = kind;
  result.effects = sir_effects_none();
  result.result = SIR_INVALID_ID;
  result.slot = SIR_INVALID_ID;
  result.callee = SIR_INVALID_ID;
  result.parameter_index = SIZE_MAX;
  result.targets[0] = SIR_INVALID_ID;
  result.targets[1] = SIR_INVALID_ID;
  return result;
}

static ownership_token managed_string_token(void) {
  ownership_token token;
  memset(&token, 0, sizeof(token));
  token.kind = OWNERSHIP_TOKEN_MANAGED;
  token.type = sir_builtin_type(SIR_TYPE_STRING);
  token.representation = SIR_REP_STRING_DESCRIPTOR;
  token.origin_kind = OWNERSHIP_ORIGIN_SYNTHETIC;
  token.origin = OWNERSHIP_INVALID_ID;
  return token;
}

static ownership_token bool_token(void) {
  ownership_token token;
  memset(&token, 0, sizeof(token));
  token.kind = OWNERSHIP_TOKEN_SCALAR;
  token.type = sir_builtin_type(SIR_TYPE_BOOL);
  token.representation = SIR_REP_SCALAR;
  token.origin_kind = OWNERSHIP_ORIGIN_SYNTHETIC;
  token.origin = OWNERSHIP_INVALID_ID;
  return token;
}

static int add_test_callees(ownership_plan *plan, size_t count) {
  size_t index;
  plan->callees = calloc(count, sizeof(*plan->callees));
  if (plan->callees == NULL)
    return 0;
  plan->callee_count = count;
  for (index = 0; index < count; index++) {
    char name[32];
    size_t length;
    snprintf(name, sizeof(name), "rift_plan_body_%zu", index);
    length = strlen(name);
    plan->callees[index].kind = SIR_CALLEE_USER;
    plan->callees[index].c_symbol = malloc(length + 1);
    plan->callees[index].parameters =
        calloc(1, sizeof(*plan->callees[index].parameters));
    if (plan->callees[index].c_symbol == NULL ||
        plan->callees[index].parameters == NULL)
      return 0;
    memcpy(plan->callees[index].c_symbol, name, length + 1);
    plan->callees[index].parameter_count = 1;
    plan->callees[index].parameters[0].type = sir_builtin_type(SIR_TYPE_STRING);
    plan->callees[index].parameters[0].representation =
        SIR_REP_STRING_DESCRIPTOR;
    plan->callees[index].parameters[0].ownership = SIR_OWNERSHIP_OWNED;
    plan->callees[index].parameters[0].mode = SIR_ARGUMENT_CONSUME;
    plan->callees[index].return_type = sir_builtin_type(SIR_TYPE_STRING);
    plan->callees[index].return_representation = SIR_REP_STRING_DESCRIPTOR;
    plan->callees[index].return_ownership = SIR_OWNERSHIP_OWNED;
    plan->callees[index].effects = sir_effects_none();
    plan->callees[index].effects.flags = SIR_EFFECT_CALL;
    plan->callees[index].call_abi = SIR_CALL_ABI_C_RETURN_VALUE;
  }
  return 1;
}

static int configure_concat_callee(ownership_plan *plan, size_t index) {
  const sir_intrinsic_descriptor *descriptor =
      sir_intrinsic_lookup((string_view){"concat", 6}, 2);
  ownership_callee *callee;
  if (descriptor == NULL || index >= plan->callee_count)
    return 0;
  callee = &plan->callees[index];
  free(callee->c_symbol);
  free(callee->parameters);
  memset(callee, 0, sizeof(*callee));
  callee->kind = descriptor->signature.kind;
  callee->symbol_id = descriptor->signature.symbol_id;
  callee->c_symbol = malloc(descriptor->signature.c_symbol.length + 1);
  callee->parameters = malloc(descriptor->signature.parameter_count *
                              sizeof(*callee->parameters));
  if (callee->c_symbol == NULL || callee->parameters == NULL)
    return 0;
  memcpy(callee->c_symbol, descriptor->signature.c_symbol.data,
         descriptor->signature.c_symbol.length);
  callee->c_symbol[descriptor->signature.c_symbol.length] = '\0';
  memcpy(callee->parameters, descriptor->signature.parameters,
         descriptor->signature.parameter_count * sizeof(*callee->parameters));
  callee->parameter_count = descriptor->signature.parameter_count;
  callee->return_type = descriptor->signature.return_type;
  callee->return_representation = descriptor->signature.return_representation;
  callee->return_ownership = descriptor->signature.return_ownership;
  callee->effects = descriptor->signature.effects;
  callee->call_abi = descriptor->signature.call_abi;
  return 1;
}

static void test_representative_plan(void) {
  node_t nodes[7];
  token_t arguments[1];
  ast_t types[1];
  ast_t statements[2];
  sir_function semantic;
  sir_lower_options options = sir_lower_default_options();
  sir_diagnostic semantic_diagnostic;
  ownership_plan *plan;
  ownership_diagnostic diagnostic;
  ast_t ast = build_echo_function(nodes, arguments, types, statements);

  sir_function_init(&semantic);
  plan = NULL;
  options.enabled = 1;
  expect(sir_lower_function(ast, &options, &semantic, &semantic_diagnostic) ==
             SIR_LOWER_OK,
         "representative AST reaches verified semantic IR");
  plan = ownership_plan_build(&semantic, NULL, &diagnostic);
  expect(plan != NULL, "verified semantic IR produces an ownership plan");
  expect(ownership_plan_is_verified(plan),
         "ownership builder returns only a sealed verified plan");
  {
    ownership_operation_view operations[4];
    size_t indexes[4] = {4, 5, 6, 7};
    size_t index;
    int queried = 1;
    for (index = 0; index < 4; index++) {
      queried = queried && ownership_plan_operation_at(plan, 0, indexes[index],
                                                       &operations[index]);
    }
    expect(queried && ownership_plan_block_count(plan) == 1 &&
               ownership_plan_operation_count(plan, 0) == 8 &&
               operations[0].kind == OWNERSHIP_OP_HOLD &&
               operations[1].kind == OWNERSHIP_OP_RELEASE &&
               operations[2].kind == OWNERSHIP_OP_RELEASE &&
               operations[3].kind == OWNERSHIP_OP_RETURN,
           "return is held before reverse-order owner cleanup and transfer");
  }
  expect(ownership_plan_internal_add_block(plan) == OWNERSHIP_INVALID_ID,
         "sealed ownership plan rejects post-verification mutation");
  ownership_plan_destroy(plan);
  sir_function_destroy(&semantic);
}

static void test_borrow_provenance_rejected(void) {
  ownership_plan plan;
  ownership_diagnostic diagnostic;
  ownership_operation op;
  ownership_id block;
  ownership_id owner;
  ownership_id borrower;

  ownership_plan_internal_init(&plan);
  owner = ownership_plan_internal_add_token(&plan, managed_string_token());
  borrower = ownership_plan_internal_add_token(&plan, managed_string_token());
  block = ownership_plan_internal_add_block(&plan);
  plan.entry_block = block;
  op = operation(OWNERSHIP_OP_ACQUIRE);
  op.result = owner;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  op = operation(OWNERSHIP_OP_RELEASE);
  op.operand = owner;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  op = operation(OWNERSHIP_OP_BORROW);
  op.operand = owner;
  op.result = borrower;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  op = operation(OWNERSHIP_OP_RETURN);
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  expect(!ownership_plan_internal_verify_and_seal(&plan, &diagnostic) &&
             diagnostic.code == OWNERSHIP_DIAGNOSTIC_BORROW_PROVENANCE,
         "ownership verifier rejects a borrow without a live owner");
  ownership_plan_internal_destroy(&plan);
}

static void test_double_release_rejected(void) {
  ownership_plan plan;
  ownership_diagnostic diagnostic;
  ownership_operation op;
  ownership_id block;
  ownership_id owner;

  ownership_plan_internal_init(&plan);
  owner = ownership_plan_internal_add_token(&plan, managed_string_token());
  block = ownership_plan_internal_add_block(&plan);
  plan.entry_block = block;
  op = operation(OWNERSHIP_OP_ACQUIRE);
  op.result = owner;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  op = operation(OWNERSHIP_OP_RELEASE);
  op.operand = owner;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  op = operation(OWNERSHIP_OP_RETURN);
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  expect(!ownership_plan_internal_verify_and_seal(&plan, &diagnostic) &&
             diagnostic.code == OWNERSHIP_DIAGNOSTIC_DOUBLE_RELEASE,
         "ownership verifier rejects double release on one path");
  ownership_plan_internal_destroy(&plan);
}

static void test_unbalanced_join_rejected(void) {
  ownership_plan plan;
  ownership_diagnostic diagnostic;
  ownership_operation op;
  ownership_id entry;
  ownership_id left;
  ownership_id right;
  ownership_id join;
  ownership_id owner;
  ownership_id condition;

  ownership_plan_internal_init(&plan);
  owner = ownership_plan_internal_add_token(&plan, managed_string_token());
  condition = ownership_plan_internal_add_token(&plan, bool_token());
  entry = ownership_plan_internal_add_block(&plan);
  left = ownership_plan_internal_add_block(&plan);
  right = ownership_plan_internal_add_block(&plan);
  join = ownership_plan_internal_add_block(&plan);
  plan.entry_block = entry;
  op = operation(OWNERSHIP_OP_ACQUIRE);
  op.result = owner;
  (void)ownership_plan_internal_add_operation(&plan, entry, op);
  op = operation(OWNERSHIP_OP_DEFINE_SCALAR);
  op.result = condition;
  (void)ownership_plan_internal_add_operation(&plan, entry, op);
  op = operation(OWNERSHIP_OP_BRANCH);
  op.operand = condition;
  op.targets[0] = left;
  op.targets[1] = right;
  op.target_count = 2;
  (void)ownership_plan_internal_add_operation(&plan, entry, op);
  op = operation(OWNERSHIP_OP_RELEASE);
  op.operand = owner;
  (void)ownership_plan_internal_add_operation(&plan, left, op);
  op = operation(OWNERSHIP_OP_JUMP);
  op.targets[0] = join;
  op.target_count = 1;
  (void)ownership_plan_internal_add_operation(&plan, left, op);
  (void)ownership_plan_internal_add_operation(&plan, right, op);
  op = operation(OWNERSHIP_OP_RETURN);
  (void)ownership_plan_internal_add_operation(&plan, join, op);
  expect(!ownership_plan_internal_verify_and_seal(&plan, &diagnostic) &&
             diagnostic.code == OWNERSHIP_DIAGNOSTIC_UNBALANCED_JOIN,
         "ownership verifier rejects a join with unequal owner states");
  ownership_plan_internal_destroy(&plan);
}

static void test_slot_owner_join_accepts_distinct_backing_tokens(void) {
  ownership_plan plan;
  ownership_diagnostic diagnostic;
  ownership_operation op;
  ownership_token slot_token = managed_string_token();
  ownership_id entry;
  ownership_id left;
  ownership_id right;
  ownership_id join;
  ownership_id slot;
  ownership_id left_value;
  ownership_id right_value;
  ownership_id condition;

  ownership_plan_internal_init(&plan);
  slot_token.origin_kind = OWNERSHIP_ORIGIN_SLOT;
  slot_token.origin = 0;
  slot = ownership_plan_internal_add_token(&plan, slot_token);
  left_value = ownership_plan_internal_add_token(&plan, managed_string_token());
  right_value =
      ownership_plan_internal_add_token(&plan, managed_string_token());
  condition = ownership_plan_internal_add_token(&plan, bool_token());
  entry = ownership_plan_internal_add_block(&plan);
  left = ownership_plan_internal_add_block(&plan);
  right = ownership_plan_internal_add_block(&plan);
  join = ownership_plan_internal_add_block(&plan);
  plan.entry_block = entry;
  op = operation(OWNERSHIP_OP_ADOPT);
  op.result = slot;
  (void)ownership_plan_internal_add_operation(&plan, entry, op);
  op = operation(OWNERSHIP_OP_DEFINE_SCALAR);
  op.result = condition;
  (void)ownership_plan_internal_add_operation(&plan, entry, op);
  op = operation(OWNERSHIP_OP_BRANCH);
  op.operand = condition;
  op.targets[0] = left;
  op.targets[1] = right;
  op.target_count = 2;
  (void)ownership_plan_internal_add_operation(&plan, entry, op);

  op = operation(OWNERSHIP_OP_ACQUIRE);
  op.result = left_value;
  (void)ownership_plan_internal_add_operation(&plan, left, op);
  op = operation(OWNERSHIP_OP_REPLACE_SLOT);
  op.result = slot;
  op.operand = left_value;
  (void)ownership_plan_internal_add_operation(&plan, left, op);
  op = operation(OWNERSHIP_OP_JUMP);
  op.targets[0] = join;
  op.target_count = 1;
  (void)ownership_plan_internal_add_operation(&plan, left, op);

  op = operation(OWNERSHIP_OP_ACQUIRE);
  op.result = right_value;
  (void)ownership_plan_internal_add_operation(&plan, right, op);
  op = operation(OWNERSHIP_OP_REPLACE_SLOT);
  op.result = slot;
  op.operand = right_value;
  (void)ownership_plan_internal_add_operation(&plan, right, op);
  op = operation(OWNERSHIP_OP_JUMP);
  op.targets[0] = join;
  op.target_count = 1;
  (void)ownership_plan_internal_add_operation(&plan, right, op);

  op = operation(OWNERSHIP_OP_RELEASE);
  op.operand = slot;
  (void)ownership_plan_internal_add_operation(&plan, join, op);
  op = operation(OWNERSHIP_OP_RETURN);
  (void)ownership_plan_internal_add_operation(&plan, join, op);
  expect(ownership_plan_internal_verify_and_seal(&plan, &diagnostic),
         "ownership join tracks the outer slot, not branch token identity");
  ownership_plan_internal_destroy(&plan);
}

static void test_live_branch_loan_join_rejected(void) {
  ownership_plan plan;
  ownership_diagnostic diagnostic;
  ownership_operation op;
  ownership_id entry;
  ownership_id left;
  ownership_id right;
  ownership_id join;
  ownership_id owner;
  ownership_id loan;
  ownership_id condition;

  ownership_plan_internal_init(&plan);
  owner = ownership_plan_internal_add_token(&plan, managed_string_token());
  loan = ownership_plan_internal_add_token(&plan, managed_string_token());
  condition = ownership_plan_internal_add_token(&plan, bool_token());
  entry = ownership_plan_internal_add_block(&plan);
  left = ownership_plan_internal_add_block(&plan);
  right = ownership_plan_internal_add_block(&plan);
  join = ownership_plan_internal_add_block(&plan);
  plan.entry_block = entry;
  op = operation(OWNERSHIP_OP_ACQUIRE);
  op.result = owner;
  (void)ownership_plan_internal_add_operation(&plan, entry, op);
  op = operation(OWNERSHIP_OP_DEFINE_SCALAR);
  op.result = condition;
  (void)ownership_plan_internal_add_operation(&plan, entry, op);
  op = operation(OWNERSHIP_OP_BRANCH);
  op.operand = condition;
  op.targets[0] = left;
  op.targets[1] = right;
  op.target_count = 2;
  (void)ownership_plan_internal_add_operation(&plan, entry, op);
  op = operation(OWNERSHIP_OP_BORROW);
  op.result = loan;
  op.operand = owner;
  (void)ownership_plan_internal_add_operation(&plan, left, op);
  op = operation(OWNERSHIP_OP_JUMP);
  op.targets[0] = join;
  op.target_count = 1;
  (void)ownership_plan_internal_add_operation(&plan, left, op);
  (void)ownership_plan_internal_add_operation(&plan, right, op);
  op = operation(OWNERSHIP_OP_RELEASE);
  op.operand = owner;
  (void)ownership_plan_internal_add_operation(&plan, join, op);
  op = operation(OWNERSHIP_OP_RETURN);
  (void)ownership_plan_internal_add_operation(&plan, join, op);
  expect(!ownership_plan_internal_verify_and_seal(&plan, &diagnostic) &&
             diagnostic.code == OWNERSHIP_DIAGNOSTIC_UNBALANCED_JOIN,
         "ownership join rejects a branch-only live loan");
  ownership_plan_internal_destroy(&plan);
}

static void test_use_after_move_rejected(void) {
  ownership_plan plan;
  ownership_diagnostic diagnostic;
  ownership_operation op;
  ownership_id block;
  ownership_id source;
  ownership_id destination;

  ownership_plan_internal_init(&plan);
  source = ownership_plan_internal_add_token(&plan, managed_string_token());
  destination =
      ownership_plan_internal_add_token(&plan, managed_string_token());
  block = ownership_plan_internal_add_block(&plan);
  plan.entry_block = block;
  op = operation(OWNERSHIP_OP_ACQUIRE);
  op.result = source;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  op = operation(OWNERSHIP_OP_MOVE);
  op.operand = source;
  op.result = destination;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  op = operation(OWNERSHIP_OP_RELEASE);
  op.operand = source;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  op.operand = destination;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  op = operation(OWNERSHIP_OP_RETURN);
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  expect(!ownership_plan_internal_verify_and_seal(&plan, &diagnostic) &&
             diagnostic.code == OWNERSHIP_DIAGNOSTIC_INVALID_OPERATION,
         "ownership verifier rejects use after move");
  ownership_plan_internal_destroy(&plan);
}

static void test_malformed_call_frames_rejected(void) {
  ownership_plan plan;
  ownership_diagnostic diagnostic;
  ownership_operation op;
  ownership_id operands[2];
  ownership_id block;
  ownership_id source;
  ownership_id prepared;
  ownership_id result;

  ownership_plan_internal_init(&plan);
  expect(add_test_callees(&plan, 2), "malformed-call test creates callees");
  source = ownership_plan_internal_add_token(&plan, managed_string_token());
  prepared = ownership_plan_internal_add_token(&plan, managed_string_token());
  result = ownership_plan_internal_add_token(&plan, managed_string_token());
  block = ownership_plan_internal_add_block(&plan);
  plan.entry_block = block;
  op = operation(OWNERSHIP_OP_ACQUIRE);
  op.result = source;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  op = operation(OWNERSHIP_OP_MOVE);
  op.operand = source;
  op.result = prepared;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  operands[0] = prepared;
  op = operation(OWNERSHIP_OP_CALL);
  op.callee = 0;
  op.operands = operands;
  op.operand_count = 1;
  op.result = result;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  op = operation(OWNERSHIP_OP_RELEASE);
  op.operand = result;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  op = operation(OWNERSHIP_OP_RETURN);
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  expect(!ownership_plan_internal_verify_and_seal(&plan, &diagnostic) &&
             diagnostic.code == OWNERSHIP_DIAGNOSTIC_INVALID_OPERATION,
         "ownership verifier rejects an unprepared call operand");
  ownership_plan_internal_destroy(&plan);

  ownership_plan_internal_init(&plan);
  expect(add_test_callees(&plan, 2), "wrong-callee test creates callees");
  source = ownership_plan_internal_add_token(&plan, managed_string_token());
  prepared = ownership_plan_internal_add_token(&plan, managed_string_token());
  result = ownership_plan_internal_add_token(&plan, managed_string_token());
  block = ownership_plan_internal_add_block(&plan);
  plan.entry_block = block;
  op = operation(OWNERSHIP_OP_ACQUIRE);
  op.result = source;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  op = operation(OWNERSHIP_OP_MOVE);
  op.operand = source;
  op.result = prepared;
  op.callee = 0;
  op.parameter_index = 0;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  operands[0] = prepared;
  op = operation(OWNERSHIP_OP_CALL);
  op.callee = 1;
  op.operands = operands;
  op.operand_count = 1;
  op.result = result;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  op = operation(OWNERSHIP_OP_RELEASE);
  op.operand = result;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  op = operation(OWNERSHIP_OP_RETURN);
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  expect(!ownership_plan_internal_verify_and_seal(&plan, &diagnostic) &&
             diagnostic.code == OWNERSHIP_DIAGNOSTIC_INVALID_OPERATION,
         "ownership verifier rejects a prepared share for the wrong callee");
  ownership_plan_internal_destroy(&plan);

  ownership_plan_internal_init(&plan);
  expect(add_test_callees(&plan, 1) && configure_concat_callee(&plan, 0),
         "duplicate-operand test creates intrinsic callee");
  source = ownership_plan_internal_add_token(&plan, managed_string_token());
  prepared = ownership_plan_internal_add_token(&plan, managed_string_token());
  result = ownership_plan_internal_add_token(&plan, managed_string_token());
  block = ownership_plan_internal_add_block(&plan);
  plan.entry_block = block;
  op = operation(OWNERSHIP_OP_ACQUIRE);
  op.result = source;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  op = operation(OWNERSHIP_OP_MOVE);
  op.operand = source;
  op.result = prepared;
  op.callee = 0;
  op.parameter_index = 0;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  operands[0] = prepared;
  operands[1] = prepared;
  op = operation(OWNERSHIP_OP_CALL);
  op.callee = 0;
  op.operands = operands;
  op.operand_count = 2;
  op.result = result;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  op = operation(OWNERSHIP_OP_RELEASE);
  op.operand = result;
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  op = operation(OWNERSHIP_OP_RETURN);
  (void)ownership_plan_internal_add_operation(&plan, block, op);
  expect(!ownership_plan_internal_verify_and_seal(&plan, &diagnostic) &&
             diagnostic.code == OWNERSHIP_DIAGNOSTIC_INVALID_OPERATION,
         "ownership verifier rejects one prepared share reused for aliased "
         "intrinsic operands");
  ownership_plan_internal_destroy(&plan);
}

static void test_unsupported_semantic_op_rejected(void) {
  sir_function semantic;
  sir_diagnostic semantic_diagnostic;
  ownership_plan *plan;
  ownership_diagnostic diagnostic;
  sir_value value;
  sir_operation constant;
  sir_operation primitive;
  sir_operation return_op;
  sir_id value_id;
  sir_id block;

  sir_function_init(&semantic);
  semantic.return_type = sir_builtin_type(SIR_TYPE_BOOL);
  semantic.return_representation = SIR_REP_SCALAR;
  semantic.return_ownership = SIR_OWNERSHIP_SCALAR;
  value.type = semantic.return_type;
  value.representation = semantic.return_representation;
  value.ownership = semantic.return_ownership;
  value_id = sir_function_add_value(&semantic, value);
  block = sir_function_add_block(&semantic);
  semantic.entry_block = block;
  constant = semantic_operation(SIR_OP_CONSTANT);
  constant.result = value_id;
  (void)sir_block_add_operation(&semantic, block, &constant);
  primitive = semantic_operation(SIR_OP_PRIMITIVE);
  primitive.primitive = SIR_PRIMITIVE_COPY;
  primitive.operands = &value_id;
  primitive.operand_count = 1;
  primitive.result = sir_function_add_value(&semantic, value);
  (void)sir_block_add_operation(&semantic, block, &primitive);
  return_op = semantic_operation(SIR_OP_RETURN);
  return_op.operands = &primitive.result;
  return_op.operand_count = 1;
  (void)sir_block_add_operation(&semantic, block, &return_op);
  expect(sir_verify_function(&semantic, NULL, &semantic_diagnostic),
         "semantic verifier accepts the explicit constant primitive");
  plan = ownership_plan_build(&semantic, NULL, &diagnostic);
  expect(plan == NULL &&
             diagnostic.code == OWNERSHIP_DIAGNOSTIC_UNSUPPORTED_SOURCE_IR,
         "checkpoint planner rejects a verified but unsupported semantic op");
  ownership_plan_destroy(plan);
  sir_function_destroy(&semantic);
}

int main(void) {
  test_representative_plan();
  test_borrow_provenance_rejected();
  test_double_release_rejected();
  test_unbalanced_join_rejected();
  test_slot_owner_join_accepts_distinct_backing_tokens();
  test_live_branch_loan_join_rejected();
  test_use_after_move_rejected();
  test_malformed_call_frames_rejected();
  test_unsupported_semantic_op_rejected();
  if (failures != 0) {
    fprintf(stderr, "%d ownership plan test(s) failed\n", failures);
    return 1;
  }
  printf("All ownership plan tests passed\n");
  return 0;
}
