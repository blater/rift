#include "plan_c_emitter/plan_c_emitter.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const plan_c_abi RIFT_ABI = {
    .bool_type = "int",
    .string_type = "string",
    .string_retain = "__string_retain",
    .string_release = "__string_release",
    .array_type = "__internal_dynamic_array_t",
    .array_retain = "__internal_retain_array",
    .array_release = "__internal_free_array",
    .string_array_make = "string_make_array",
    .string_array_push_copy = "string_push_array",
    .array_push_transfer = "__internal_push_array",
    .string_array_get = "string_get_elem",
    .array_get = "__internal_get_elem",
    .array_set = "__internal_set_elem",
};

static void set_diagnostic(plan_c_diagnostic *diagnostic,
                           plan_c_diagnostic_code code, ownership_id block,
                           size_t operation, const char *message) {
  if (diagnostic == NULL)
    return;
  diagnostic->code = code;
  diagnostic->block = block;
  diagnostic->operation = operation;
  diagnostic->message = message;
}

static int c_identifier(const char *name) {
  static const char *const keywords[] = {
      "_Alignas",
      "_Alignof",
      "_Atomic",
      "_BitInt",
      "_Bool",
      "_Complex",
      "_Decimal128",
      "_Decimal32",
      "_Decimal64",
      "_Generic",
      "_Imaginary",
      "_Noreturn",
      "_Static_assert",
      "_Thread_local",
      "alignas",
      "alignof",
      "asm",
      "auto",
      "bool",
      "break",
      "case",
      "char",
      "const",
      "constexpr",
      "continue",
      "default",
      "do",
      "double",
      "else",
      "enum",
      "extern",
      "false",
      "float",
      "for",
      "goto",
      "if",
      "inline",
      "int",
      "long",
      "nullptr",
      "register",
      "restrict",
      "return",
      "short",
      "signed",
      "sizeof",
      "static",
      "static_assert",
      "struct",
      "switch",
      "thread_local",
      "true",
      "typedef",
      "typeof",
      "typeof_unqual",
      "union",
      "unsigned",
      "void",
      "volatile",
      "while",
  };
  const unsigned char *cursor = (const unsigned char *)name;
  size_t keyword_index;
  if (cursor == NULL || !(isalpha((unsigned char)*cursor) || *cursor == '_')) {
    return 0;
  }
  cursor++;
  while (*cursor != '\0') {
    if (!(isalnum((unsigned char)*cursor) || *cursor == '_'))
      return 0;
    cursor++;
  }
  if (strncmp(name, "__rift_plan_t_", 14) == 0 ||
      strncmp(name, "rift_plan_slot_", 15) == 0 ||
      strncmp(name, "rift_plan_tmp_", 14) == 0)
    return 0;
  for (keyword_index = 0; keyword_index < sizeof(keywords) / sizeof(*keywords);
       keyword_index++) {
    if (strcmp(name, keywords[keyword_index]) == 0)
      return 0;
  }
  return 1;
}

static int valid_abi(const plan_c_abi *abi) {
  return abi != NULL && abi->bool_type != NULL &&
         strcmp(abi->bool_type, "int") == 0 && c_identifier(abi->string_type) &&
         c_identifier(abi->string_retain) && c_identifier(abi->string_release) &&
         c_identifier(abi->array_type) && c_identifier(abi->array_retain) &&
         c_identifier(abi->array_release) &&
         c_identifier(abi->string_array_make) &&
         c_identifier(abi->string_array_push_copy) &&
         c_identifier(abi->array_push_transfer) &&
         c_identifier(abi->string_array_get) && c_identifier(abi->array_get) &&
         c_identifier(abi->array_set);
}

static int string_token(const ownership_token_view *token) {
  return token->kind == OWNERSHIP_TOKEN_MANAGED &&
         token->type.kind == SIR_TYPE_STRING && token->type.nominal_id == 0 &&
         token->representation == SIR_REP_STRING_DESCRIPTOR;
}

static int string_array_token(const ownership_token_view *token) {
  return token->kind == OWNERSHIP_TOKEN_MANAGED &&
         token->type.kind == SIR_TYPE_ARRAY &&
         token->type.nominal_id == SIR_TYPE_STRING &&
         token->representation == SIR_REP_MANAGED_HANDLE;
}

static int bool_token(const ownership_token_view *token) {
  return token->kind == OWNERSHIP_TOKEN_SCALAR &&
         token->type.kind == SIR_TYPE_BOOL && token->type.nominal_id == 0 &&
         token->representation == SIR_REP_SCALAR;
}

static int int_token(const ownership_token_view *token) {
  return token->kind == OWNERSHIP_TOKEN_SCALAR &&
         token->type.kind == SIR_TYPE_INT && token->type.nominal_id == 0 &&
         token->representation == SIR_REP_SCALAR;
}

static int scalar_token(const ownership_token_view *token) {
  return bool_token(token) || int_token(token);
}

static int token_name(const ownership_plan *plan, ownership_id token,
                      char *buffer, size_t buffer_size, const char **name_out) {
  ownership_token_view token_view;
  ownership_slot_view slot;
  int written;
  if (!ownership_plan_token_at(plan, token, &token_view))
    return 0;
  if (token_view.origin_kind == OWNERSHIP_ORIGIN_SLOT) {
    if (!ownership_plan_slot_at(plan, token_view.origin, &slot) ||
        slot.token != token) {
      return 0;
    }
    written = snprintf(buffer, buffer_size, "rift_plan_slot_%u",
                       (unsigned int)token_view.origin);
    if (written < 0 || (size_t)written >= buffer_size)
      return 0;
    *name_out = buffer;
    return 1;
  }
  if (token_view.origin_kind != OWNERSHIP_ORIGIN_VALUE &&
      token_view.origin_kind != OWNERSHIP_ORIGIN_SYNTHETIC) {
    return 0;
  }
  written =
      snprintf(buffer, buffer_size, "rift_plan_tmp_%u", (unsigned int)token);
  if (written < 0 || (size_t)written >= buffer_size)
    return 0;
  *name_out = buffer;
  return 1;
}

static int validate_operation(const ownership_plan *plan, ownership_id block,
                              size_t operation_index,
                              const ownership_operation_view *operation,
                              plan_c_diagnostic *diagnostic) {
  ownership_token_view token;
  char name_buffer[64];
  const char *name;
  switch (operation->kind) {
  case OWNERSHIP_OP_ACQUIRE:
  case OWNERSHIP_OP_ADOPT:
  case OWNERSHIP_OP_BORROW:
  case OWNERSHIP_OP_HOLD:
  case OWNERSHIP_OP_MOVE:
    if (!ownership_plan_token_at(plan, operation->result, &token) ||
        (!string_token(&token) && !string_array_token(&token)) ||
        !token_name(plan, operation->result, name_buffer, sizeof(name_buffer),
                    &name)) {
      break;
    }
    if (operation->kind == OWNERSHIP_OP_ACQUIRE ||
        operation->kind == OWNERSHIP_OP_ADOPT) {
      ownership_slot_view slot;
      if (token.origin_kind != OWNERSHIP_ORIGIN_SLOT ||
          !ownership_plan_slot_at(plan, token.origin, &slot) ||
          !slot.is_parameter || slot.token != operation->result) {
        break;
      }
      return 1;
    }
    if (operation->kind == OWNERSHIP_OP_BORROW &&
        token.origin_kind != OWNERSHIP_ORIGIN_VALUE) {
      break;
    }
    if ((operation->kind == OWNERSHIP_OP_HOLD ||
         operation->kind == OWNERSHIP_OP_MOVE) &&
        token.origin_kind == OWNERSHIP_ORIGIN_SLOT) {
      ownership_slot_view slot;
      if (!ownership_plan_slot_at(plan, token.origin, &slot) ||
          slot.is_parameter || slot.token != operation->result) {
        break;
      }
    } else if ((operation->kind == OWNERSHIP_OP_HOLD ||
                operation->kind == OWNERSHIP_OP_MOVE) &&
               token.origin_kind != OWNERSHIP_ORIGIN_VALUE &&
               token.origin_kind != OWNERSHIP_ORIGIN_SYNTHETIC) {
      break;
    }
    if (!ownership_plan_token_at(plan, operation->operand, &token) ||
        (!string_token(&token) && !string_array_token(&token)) ||
        !token_name(plan, operation->operand, name_buffer, sizeof(name_buffer),
                    &name)) {
      break;
    }
    return 1;
  case OWNERSHIP_OP_RELEASE:
  case OWNERSHIP_OP_END_BORROW:
    if (ownership_plan_token_at(plan, operation->operand, &token) &&
        (string_token(&token) || string_array_token(&token)) &&
        token_name(plan, operation->operand, name_buffer, sizeof(name_buffer),
                   &name)) {
      return 1;
    }
    break;
  case OWNERSHIP_OP_DEFINE_SCALAR:
    if (ownership_plan_token_at(plan, operation->result, &token) &&
        scalar_token(&token) &&
        token_name(plan, operation->result, name_buffer, sizeof(name_buffer),
                   &name) &&
        (!bool_token(&token) || operation->bool_value == 0 ||
         operation->bool_value == 1)) {
      return 1;
    }
    break;
  case OWNERSHIP_OP_COPY_SCALAR:
    if (ownership_plan_token_at(plan, operation->result, &token) &&
        bool_token(&token) &&
        token_name(plan, operation->result, name_buffer, sizeof(name_buffer),
                   &name) &&
        ownership_plan_token_at(plan, operation->operand, &token) &&
        bool_token(&token) &&
        token_name(plan, operation->operand, name_buffer, sizeof(name_buffer),
                   &name)) {
      return 1;
    }
    break;
  case OWNERSHIP_OP_REPLACE_SLOT: {
    ownership_token_view target;
    if (!ownership_plan_token_at(plan, operation->result, &target) ||
        (!string_token(&target) && !bool_token(&target)) ||
        target.origin_kind != OWNERSHIP_ORIGIN_SLOT ||
        !token_name(plan, operation->result, name_buffer, sizeof(name_buffer),
                    &name) ||
        !ownership_plan_token_at(plan, operation->operand, &token) ||
        token.kind != target.kind || !sir_type_equal(token.type, target.type) ||
        token.representation != target.representation ||
        !token_name(plan, operation->operand, name_buffer, sizeof(name_buffer),
                    &name)) {
      break;
    }
  }
    return 1;
  case OWNERSHIP_OP_END_SLOT:
    if (ownership_plan_token_at(plan, operation->operand, &token) &&
        (string_token(&token) || string_array_token(&token) ||
         bool_token(&token)) &&
        token.origin_kind == OWNERSHIP_ORIGIN_SLOT &&
        token_name(plan, operation->operand, name_buffer, sizeof(name_buffer),
                   &name)) {
      return 1;
    }
    break;
  case OWNERSHIP_OP_CALL: {
    size_t argument_index;
    ownership_callee_view callee;
    if (!ownership_plan_callee_at(plan, operation->callee, &callee) ||
        callee.c_symbol == NULL || !c_identifier(callee.c_symbol) ||
        callee.parameter_count != operation->operand_count ||
        (callee.call_abi != SIR_CALL_ABI_C_RETURN_VALUE &&
         callee.call_abi != SIR_CALL_ABI_OUT_STRING_FIRST) ||
        !ownership_plan_token_at(plan, operation->result, &token) ||
        !string_token(&token) ||
        !sir_type_equal(token.type, callee.return_type) ||
        token.representation != callee.return_representation ||
        !token_name(plan, operation->result, name_buffer, sizeof(name_buffer),
                    &name)) {
      break;
    }
    for (argument_index = 0; argument_index < operation->operand_count;
         argument_index++) {
      if (!ownership_plan_token_at(plan, operation->operands[argument_index],
                                   &token) ||
          !sir_type_equal(token.type, callee.parameters[argument_index].type) ||
          token.representation !=
              callee.parameters[argument_index].representation ||
          (!string_token(&token) && !string_array_token(&token) &&
           !scalar_token(&token)) ||
          !token_name(plan, operation->operands[argument_index], name_buffer,
                      sizeof(name_buffer), &name)) {
        break;
      }
    }
    if (argument_index == operation->operand_count)
      return 1;
    break;
  }
  case OWNERSHIP_OP_ARRAY_NEW_STRING:
    if (ownership_plan_token_at(plan, operation->result, &token) &&
        string_array_token(&token) &&
        token_name(plan, operation->result, name_buffer, sizeof(name_buffer),
                   &name)) {
      return 1;
    }
    break;
  case OWNERSHIP_OP_ARRAY_APPEND_STRING:
  case OWNERSHIP_OP_ARRAY_GET_STRING:
  case OWNERSHIP_OP_ARRAY_REPLACE_STRING: {
    size_t expected =
        operation->kind == OWNERSHIP_OP_ARRAY_REPLACE_STRING ? 3 : 2;
    ownership_token_view receiver;
    ownership_token_view index;
    ownership_token_view value;
    if (operation->operand_count != expected ||
        !ownership_plan_token_at(plan, operation->operands[0], &receiver) ||
        !string_array_token(&receiver) ||
        !token_name(plan, operation->operands[0], name_buffer,
                    sizeof(name_buffer), &name)) {
      break;
    }
    if (operation->kind == OWNERSHIP_OP_ARRAY_APPEND_STRING) {
      if (!ownership_plan_token_at(plan, operation->operands[1], &value) ||
          !string_token(&value))
        break;
    } else {
      if (!ownership_plan_token_at(plan, operation->operands[1], &index) ||
          !int_token(&index))
        break;
      if (operation->kind == OWNERSHIP_OP_ARRAY_GET_STRING) {
        if (!ownership_plan_token_at(plan, operation->result, &value) ||
            !string_token(&value))
          break;
      } else if (!ownership_plan_token_at(plan, operation->operands[2],
                                          &value) ||
                 !string_token(&value)) {
        break;
      }
    }
    return 1;
  }
  case OWNERSHIP_OP_RETURN:
    if (operation->operand != OWNERSHIP_INVALID_ID &&
        ownership_plan_token_at(plan, operation->operand, &token) &&
        string_token(&token) &&
        token_name(plan, operation->operand, name_buffer, sizeof(name_buffer),
                   &name)) {
      return 1;
    }
    break;
  case OWNERSHIP_OP_JUMP:
    if (operation->target_count == 1 &&
        operation->targets[0] < ownership_plan_block_count(plan)) {
      return 1;
    }
    break;
  case OWNERSHIP_OP_BRANCH:
    if (ownership_plan_token_at(plan, operation->operand, &token) &&
        bool_token(&token) && operation->target_count == 2 &&
        operation->targets[0] < ownership_plan_block_count(plan) &&
        operation->targets[1] < ownership_plan_block_count(plan) &&
        token_name(plan, operation->operand, name_buffer, sizeof(name_buffer),
                   &name)) {
      return 1;
    }
    break;
  case OWNERSHIP_OP_UNKNOWN:
    break;
  }
  set_diagnostic(diagnostic, PLAN_C_DIAGNOSTIC_UNSUPPORTED_PLAN, block,
                 operation_index,
                 "ownership operation is outside the plan C checkpoint");
  return 0;
}

const plan_c_abi *plan_c_rift_abi(void) { return &RIFT_ABI; }

int plan_c_validate(const ownership_plan *plan, const plan_c_abi *abi,
                    plan_c_diagnostic *diagnostic) {
  ownership_function_view function;
  ownership_id block = OWNERSHIP_INVALID_ID;
  size_t index = 0;
  set_diagnostic(diagnostic, PLAN_C_DIAGNOSTIC_NONE, OWNERSHIP_INVALID_ID, 0,
                 NULL);
  if (!ownership_plan_is_verified(plan)) {
    set_diagnostic(diagnostic, PLAN_C_DIAGNOSTIC_UNVERIFIED_PLAN,
                   OWNERSHIP_INVALID_ID, 0,
                   "C emission requires a sealed ownership plan");
    return 0;
  }
  if (!valid_abi(abi)) {
    set_diagnostic(diagnostic, PLAN_C_DIAGNOSTIC_INVALID_ABI,
                   OWNERSHIP_INVALID_ID, 0,
                   "C emission ABI is missing a valid string contract");
    return 0;
  }
  if (!ownership_plan_function(plan, &function) || function.name == NULL ||
      function.name[0] == '\0' || !c_identifier(function.external_symbol) ||
      !c_identifier(function.c_symbol)) {
    set_diagnostic(diagnostic, PLAN_C_DIAGNOSTIC_INVALID_DECLARATION,
                   OWNERSHIP_INVALID_ID, 0,
                   "plan function symbol is not a C identifier");
    return 0;
  }
  for (index = 0; index < ownership_plan_slot_count(plan); index++) {
    ownership_slot_view slot;
    if (!ownership_plan_slot_at(plan, (ownership_id)index, &slot) ||
        slot.name == NULL || slot.name[0] == '\0') {
      set_diagnostic(diagnostic, PLAN_C_DIAGNOSTIC_INVALID_DECLARATION,
                     OWNERSHIP_INVALID_ID, index,
                     "plan slot name is not a C identifier");
      return 0;
    }
  }
  for (block = 0; block < ownership_plan_block_count(plan); block++) {
    for (index = 0; index < ownership_plan_operation_count(plan, block);
         index++) {
      ownership_operation_view operation;
      if (!ownership_plan_operation_at(plan, block, index, &operation) ||
          !validate_operation(plan, block, index, &operation, diagnostic)) {
        return 0;
      }
    }
  }
  if (function.return_type.kind != SIR_TYPE_STRING ||
      function.return_representation != SIR_REP_STRING_DESCRIPTOR ||
      function.return_ownership != SIR_OWNERSHIP_OWNED) {
    set_diagnostic(diagnostic, PLAN_C_DIAGNOSTIC_INVALID_DECLARATION,
                   OWNERSHIP_INVALID_ID, 0,
                   "plan result declaration is outside the string checkpoint");
    return 0;
  }
  for (index = 0; index < ownership_plan_slot_count(plan); index++) {
    ownership_slot_view slot;
    (void)ownership_plan_slot_at(plan, (ownership_id)index, &slot);
    if (!((slot.type.kind == SIR_TYPE_STRING &&
           slot.representation == SIR_REP_STRING_DESCRIPTOR &&
           slot.ownership == SIR_OWNERSHIP_OWNED) ||
          (slot.type.kind == SIR_TYPE_BOOL &&
           slot.representation == SIR_REP_SCALAR &&
           slot.ownership == SIR_OWNERSHIP_SCALAR) ||
          (slot.type.kind == SIR_TYPE_ARRAY &&
           slot.type.nominal_id == SIR_TYPE_STRING &&
           slot.representation == SIR_REP_MANAGED_HANDLE &&
           slot.ownership == SIR_OWNERSHIP_OWNED))) {
      set_diagnostic(diagnostic, PLAN_C_DIAGNOSTIC_INVALID_DECLARATION,
                     OWNERSHIP_INVALID_ID, index,
                     "plan slot declaration is outside the string checkpoint");
      return 0;
    }
  }
  return 1;
}

static int emit_signature(FILE *output, const ownership_plan *plan,
                          const plan_c_abi *abi, const char *symbol,
                          plan_c_diagnostic *diagnostic) {
  size_t index = 0;
  int emitted_parameter = 0;
  if (output == NULL || fprintf(output, "%s %s(", abi->string_type, symbol) < 0)
    goto output_failure;
  for (index = 0; index < ownership_plan_slot_count(plan); index++) {
    ownership_slot_view slot;
    (void)ownership_plan_slot_at(plan, (ownership_id)index, &slot);
    if (!slot.is_parameter)
      continue;
    if (emitted_parameter && fprintf(output, ", ") < 0)
      goto output_failure;
    if (fprintf(output, "%s rift_plan_slot_%u",
                slot.type.kind == SIR_TYPE_BOOL
                    ? abi->bool_type
                    : (slot.type.kind == SIR_TYPE_ARRAY ? abi->array_type
                                                       : abi->string_type),
                (unsigned int)index) < 0)
      goto output_failure;
    emitted_parameter = 1;
  }
  if (!emitted_parameter && fprintf(output, "void") < 0)
    goto output_failure;
  if (fprintf(output, ")") < 0)
    goto output_failure;
  return 1;

output_failure:
  set_diagnostic(diagnostic, PLAN_C_DIAGNOSTIC_OUTPUT_FAILED,
                 OWNERSHIP_INVALID_ID, index,
                 "could not emit verified ownership plan signature");
  return 0;
}

int plan_c_emit_function_signature(FILE *output, const ownership_plan *plan,
                                   const plan_c_abi *abi,
                                   plan_c_diagnostic *diagnostic) {
  ownership_function_view function;
  if (!plan_c_validate(plan, abi, diagnostic))
    return 0;
  (void)ownership_plan_function(plan, &function);
  return emit_signature(output, plan, abi, function.external_symbol,
                        diagnostic);
}

int plan_c_emit_body_signature(FILE *output, const ownership_plan *plan,
                               const plan_c_abi *abi,
                               plan_c_diagnostic *diagnostic) {
  ownership_function_view function;
  if (!plan_c_validate(plan, abi, diagnostic))
    return 0;
  (void)ownership_plan_function(plan, &function);
  return emit_signature(output, plan, abi, function.c_symbol, diagnostic);
}

static int emit_definition(FILE *output, const ownership_plan *plan,
                           const plan_c_abi *abi, ownership_id token,
                           ownership_id source, int retain) {
  ownership_token_view token_view;
  char token_buffer[64];
  char source_buffer[64];
  const char *token_name_text;
  const char *source_name_text;
  if (!ownership_plan_token_at(plan, token, &token_view) ||
      !token_name(plan, token, token_buffer, sizeof(token_buffer),
                  &token_name_text) ||
      !token_name(plan, source, source_buffer, sizeof(source_buffer),
                  &source_name_text)) {
    return 0;
  }
  if (token_view.origin_kind == OWNERSHIP_ORIGIN_SLOT) {
    ownership_slot_view slot;
    if (!ownership_plan_slot_at(plan, token_view.origin, &slot) ||
        slot.is_parameter) {
      return 0;
    }
  }
  if (fprintf(output, "%s %s = %s;\n",
              string_array_token(&token_view) ? abi->array_type
                                              : abi->string_type,
              token_name_text, source_name_text) < 0) {
    return 0;
  }
  return !retain ||
         fprintf(output, "%s(%s);\n",
                 string_array_token(&token_view) ? abi->array_retain
                                                 : abi->string_retain,
                 token_name_text) >= 0;
}

int plan_c_emit_function_body(FILE *output, const ownership_plan *plan,
                              const plan_c_abi *abi,
                              plan_c_diagnostic *diagnostic) {
  ownership_id block = OWNERSHIP_INVALID_ID;
  size_t index = 0;
  ownership_function_view function;
  size_t slot_index;
  int emitted_argument = 0;
  if (!plan_c_validate(plan, abi, diagnostic))
    return 0;
  (void)ownership_plan_function(plan, &function);
  /* The external symbol is an owned-ABI adapter. Legacy selected-call routing
   * has already transferred one share per argument, so forwarding must not
   * retain those shares again. */
  if (output == NULL || fprintf(output, "{\n%s rift_plan_adapter_result = %s(",
                                abi->string_type, function.c_symbol) < 0)
    goto output_failure;
  for (slot_index = 0; slot_index < ownership_plan_slot_count(plan);
       slot_index++) {
    ownership_slot_view slot;
    (void)ownership_plan_slot_at(plan, (ownership_id)slot_index, &slot);
    if (!slot.is_parameter)
      continue;
    if (emitted_argument && fprintf(output, ", ") < 0)
      goto output_failure;
    if (fprintf(output, "rift_plan_slot_%u", (unsigned int)slot_index) < 0)
      goto output_failure;
    emitted_argument = 1;
  }
  if (fprintf(output, ");\nreturn rift_plan_adapter_result;\n}\n") < 0 ||
      !emit_signature(output, plan, abi, function.c_symbol, diagnostic) ||
      fprintf(output, "\n{\ngoto rift_plan_block_%u;\n",
              (unsigned int)ownership_plan_entry_block(plan)) < 0)
    goto output_failure;
  for (block = 0; block < ownership_plan_block_count(plan); block++) {
    if (fprintf(output, "rift_plan_block_%u: ;\n", (unsigned int)block) < 0)
      goto output_failure;
    for (index = 0; index < ownership_plan_operation_count(plan, block);
         index++) {
      ownership_operation_view operation;
      char operand_buffer[64];
      const char *operand;
      (void)ownership_plan_operation_at(plan, block, index, &operation);
      switch (operation.kind) {
      case OWNERSHIP_OP_ACQUIRE:
        if (!token_name(plan, operation.result, operand_buffer,
                        sizeof(operand_buffer), &operand) ||
            fprintf(output, "%s(%s);\n", abi->string_retain, operand) < 0)
          goto output_failure;
        break;
      case OWNERSHIP_OP_ADOPT:
        break;
      case OWNERSHIP_OP_BORROW:
        if (!emit_definition(output, plan, abi, operation.result,
                             operation.operand, 0))
          goto output_failure;
        break;
      case OWNERSHIP_OP_HOLD:
        if (!emit_definition(output, plan, abi, operation.result,
                             operation.operand, 1))
          goto output_failure;
        break;
      case OWNERSHIP_OP_MOVE:
        if (!emit_definition(output, plan, abi, operation.result,
                             operation.operand, 0))
          goto output_failure;
        break;
      case OWNERSHIP_OP_RELEASE:
        {
          ownership_token_view released;
          if (!ownership_plan_token_at(plan, operation.operand, &released) ||
              !token_name(plan, operation.operand, operand_buffer,
                          sizeof(operand_buffer), &operand) ||
              (string_array_token(&released)
                   ? fprintf(output, "%s(%s, 1);\n", abi->array_release,
                             operand)
                   : fprintf(output, "%s(%s);\n", abi->string_release,
                             operand)) < 0)
            goto output_failure;
        }
        break;
      case OWNERSHIP_OP_REPLACE_SLOT: {
        ownership_token_view target;
        char result_buffer[64];
        const char *result;
        if (!ownership_plan_token_at(plan, operation.result, &target) ||
            !token_name(plan, operation.result, result_buffer,
                        sizeof(result_buffer), &result) ||
            !token_name(plan, operation.operand, operand_buffer,
                        sizeof(operand_buffer), &operand))
          goto output_failure;
        if (string_token(&target) &&
            fprintf(output, "%s(%s);\n", abi->string_release, result) < 0)
          goto output_failure;
        if (fprintf(output, "%s = %s;\n", result, operand) < 0)
          goto output_failure;
        break;
      }
      case OWNERSHIP_OP_END_SLOT: {
        ownership_token_view ended;
        if (!ownership_plan_token_at(plan, operation.operand, &ended) ||
            !token_name(plan, operation.operand, operand_buffer,
                        sizeof(operand_buffer), &operand))
          goto output_failure;
        if (string_token(&ended) &&
            fprintf(output, "%s(%s);\n", abi->string_release, operand) < 0)
          goto output_failure;
        if (string_array_token(&ended) &&
            fprintf(output, "%s(%s, 1);\n", abi->array_release, operand) < 0)
          goto output_failure;
        break;
      }
      case OWNERSHIP_OP_END_BORROW:
        break;
      case OWNERSHIP_OP_CALL: {
        size_t argument_index;
        char result_buffer[64];
        const char *result;
        ownership_callee_view callee;
        if (!ownership_plan_callee_at(plan, operation.callee, &callee) ||
            !token_name(plan, operation.result, result_buffer,
                        sizeof(result_buffer), &result))
          goto output_failure;
        if (callee.call_abi == SIR_CALL_ABI_OUT_STRING_FIRST) {
          if (fprintf(output, "%s %s;\n%s(&%s", abi->string_type, result,
                      callee.c_symbol, result) < 0)
            goto output_failure;
          if (operation.operand_count != 0 && fprintf(output, ", ") < 0)
            goto output_failure;
        } else if (fprintf(output, "%s %s = %s(", abi->string_type, result,
                           callee.c_symbol) < 0) {
          goto output_failure;
        }
        for (argument_index = 0; argument_index < operation.operand_count;
             argument_index++) {
          char argument_buffer[64];
          const char *argument;
          if (!token_name(plan, operation.operands[argument_index],
                          argument_buffer, sizeof(argument_buffer),
                          &argument) ||
              (argument_index != 0 && fprintf(output, ", ") < 0) ||
              fprintf(output, "%s", argument) < 0)
            goto output_failure;
        }
        if (fprintf(output, ");\n") < 0)
          goto output_failure;
        break;
      }
      case OWNERSHIP_OP_ARRAY_NEW_STRING: {
        char result_buffer[64];
        const char *result;
        if (!token_name(plan, operation.result, result_buffer,
                        sizeof(result_buffer), &result) ||
            fprintf(output, "%s %s = %s();\n", abi->array_type, result,
                    abi->string_array_make) < 0)
          goto output_failure;
        break;
      }
      case OWNERSHIP_OP_ARRAY_APPEND_STRING: {
        char receiver_buffer[64];
        char value_buffer[64];
        const char *receiver;
        const char *value;
        if (!token_name(plan, operation.operands[0], receiver_buffer,
                        sizeof(receiver_buffer), &receiver) ||
            !token_name(plan, operation.operands[1], value_buffer,
                        sizeof(value_buffer), &value))
          goto output_failure;
        if (operation.array_input_mode == SIR_ARRAY_INPUT_COPY_BORROWED) {
          if (fprintf(output, "%s(%s, %s);\n%s(%s);\n%s(%s, 1);\n",
                      abi->string_array_push_copy, receiver, value,
                      abi->string_release, value, abi->array_release,
                      receiver) < 0)
            goto output_failure;
        } else if (operation.array_input_mode ==
                   SIR_ARRAY_INPUT_TRANSFER_OWNED) {
          if (fprintf(output, "%s(%s, &%s);\n%s(%s, 1);\n",
                      abi->array_push_transfer, receiver, value,
                      abi->array_release, receiver) < 0)
            goto output_failure;
        } else {
          goto output_failure;
        }
        break;
      }
      case OWNERSHIP_OP_ARRAY_GET_STRING: {
        char receiver_buffer[64];
        char index_buffer[64];
        char result_buffer[64];
        const char *receiver;
        const char *array_index;
        const char *result;
        if (!token_name(plan, operation.operands[0], receiver_buffer,
                        sizeof(receiver_buffer), &receiver) ||
            !token_name(plan, operation.operands[1], index_buffer,
                        sizeof(index_buffer), &array_index) ||
            !token_name(plan, operation.result, result_buffer,
                        sizeof(result_buffer), &result) ||
            fprintf(output,
                    "%s %s;\n%s(&%s, %s, (size_t)(%s));\n%s(%s, 1);\n",
                    abi->string_type, result, abi->string_array_get, result,
                    receiver, array_index, abi->array_release, receiver) < 0)
          goto output_failure;
        break;
      }
      case OWNERSHIP_OP_ARRAY_REPLACE_STRING: {
        char receiver_buffer[64];
        char index_buffer[64];
        char value_buffer[64];
        const char *receiver;
        const char *array_index;
        const char *value;
        if (!token_name(plan, operation.operands[0], receiver_buffer,
                        sizeof(receiver_buffer), &receiver) ||
            !token_name(plan, operation.operands[1], index_buffer,
                        sizeof(index_buffer), &array_index) ||
            !token_name(plan, operation.operands[2], value_buffer,
                        sizeof(value_buffer), &value) ||
            fprintf(output,
                    "%s rift_plan_array_old_%u = *(%s *)%s(%s, "
                    "(size_t)(%s));\n%s(%s, (size_t)(%s), &%s);\n%s("
                    "rift_plan_array_old_%u);\n%s(%s, 1);\n",
                    abi->string_type, (unsigned int)operation.operands[2],
                    abi->string_type, abi->array_get, receiver, array_index,
                    abi->array_set, receiver, array_index, value,
                    abi->string_release,
                    (unsigned int)operation.operands[2], abi->array_release,
                    receiver) < 0)
          goto output_failure;
        break;
      }
      case OWNERSHIP_OP_RETURN:
        if (!token_name(plan, operation.operand, operand_buffer,
                        sizeof(operand_buffer), &operand) ||
            fprintf(output, "return %s;\n", operand) < 0)
          goto output_failure;
        break;
      case OWNERSHIP_OP_DEFINE_SCALAR: {
        ownership_token_view result_token;
        ownership_slot_view slot;
        char result_buffer[64];
        const char *result;
        if (!ownership_plan_token_at(plan, operation.result, &result_token) ||
            !token_name(plan, operation.result, result_buffer,
                        sizeof(result_buffer), &result))
          goto output_failure;
        if (result_token.origin_kind == OWNERSHIP_ORIGIN_SLOT &&
            ownership_plan_slot_at(plan, result_token.origin, &slot) &&
            slot.is_parameter)
          break;
        if (fprintf(output, "%s %s = %d;\n", abi->bool_type, result,
                    result_token.type.kind == SIR_TYPE_INT
                        ? operation.int_value
                        : operation.bool_value) < 0)
          goto output_failure;
        break;
      }
      case OWNERSHIP_OP_COPY_SCALAR: {
        char result_buffer[64];
        const char *result;
        if (!token_name(plan, operation.result, result_buffer,
                        sizeof(result_buffer), &result) ||
            !token_name(plan, operation.operand, operand_buffer,
                        sizeof(operand_buffer), &operand) ||
            fprintf(output, "%s %s = %s;\n", abi->bool_type, result, operand) <
                0)
          goto output_failure;
        break;
      }
      case OWNERSHIP_OP_JUMP:
        if (fprintf(output, "goto rift_plan_block_%u;\n",
                    (unsigned int)operation.targets[0]) < 0)
          goto output_failure;
        break;
      case OWNERSHIP_OP_BRANCH:
        if (!token_name(plan, operation.operand, operand_buffer,
                        sizeof(operand_buffer), &operand) ||
            fprintf(output,
                    "if (%s) goto rift_plan_block_%u; else goto "
                    "rift_plan_block_%u;\n",
                    operand, (unsigned int)operation.targets[0],
                    (unsigned int)operation.targets[1]) < 0)
          goto output_failure;
        break;
      case OWNERSHIP_OP_UNKNOWN:
        goto output_failure;
      }
    }
  }
  if (fprintf(output, "}") < 0)
    goto output_failure;
  return 1;

output_failure:
  set_diagnostic(diagnostic, PLAN_C_DIAGNOSTIC_OUTPUT_FAILED, block, index,
                 "could not emit verified ownership plan as C");
  return 0;
}
