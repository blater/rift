#ifndef SEMANTIC_IR_INTRINSICS_H
#define SEMANTIC_IR_INTRINSICS_H

#include "semantic_ir/semantic_ir.h"

typedef struct sir_intrinsic_descriptor {
  string_view source_name;
  sir_signature signature;
} sir_intrinsic_descriptor;

size_t sir_intrinsic_count(void);
const sir_intrinsic_descriptor *sir_intrinsic_at(size_t index);
const sir_intrinsic_descriptor *sir_intrinsic_lookup(string_view source_name,
                                                     size_t arity);
int sir_intrinsic_signature_matches(const sir_signature *signature);

#endif
