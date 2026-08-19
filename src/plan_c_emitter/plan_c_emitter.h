#ifndef PLAN_C_EMITTER_H
#define PLAN_C_EMITTER_H

#include "ownership_plan/ownership_plan.h"

#include <stdio.h>

typedef struct plan_c_abi {
  const char *bool_type;
  const char *string_type;
  const char *string_retain;
  const char *string_release;
} plan_c_abi;

typedef enum plan_c_diagnostic_code {
  PLAN_C_DIAGNOSTIC_NONE,
  PLAN_C_DIAGNOSTIC_UNVERIFIED_PLAN,
  PLAN_C_DIAGNOSTIC_INVALID_DECLARATION,
  PLAN_C_DIAGNOSTIC_INVALID_ABI,
  PLAN_C_DIAGNOSTIC_UNSUPPORTED_PLAN,
  PLAN_C_DIAGNOSTIC_OUTPUT_FAILED,
} plan_c_diagnostic_code;

typedef struct plan_c_diagnostic {
  plan_c_diagnostic_code code;
  ownership_id block;
  size_t operation;
  const char *message;
} plan_c_diagnostic;

const plan_c_abi *plan_c_rift_abi(void);
int plan_c_validate(const ownership_plan *plan, const plan_c_abi *abi,
                    plan_c_diagnostic *diagnostic);
int plan_c_emit_function_signature(FILE *output, const ownership_plan *plan,
                                   const plan_c_abi *abi,
                                   plan_c_diagnostic *diagnostic);
int plan_c_emit_body_signature(FILE *output, const ownership_plan *plan,
                               const plan_c_abi *abi,
                               plan_c_diagnostic *diagnostic);
int plan_c_emit_function_body(FILE *output, const ownership_plan *plan,
                              const plan_c_abi *abi,
                              plan_c_diagnostic *diagnostic);

#endif
