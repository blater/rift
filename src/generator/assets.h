//-----------------------------------------------------------------------------
//  RIFT ASSET GENERATOR
//  MIT License
//  Copyright (c) 2024 Paul Passeron
//-----------------------------------------------------------------------------

#ifndef ASSET_GENERATOR_H
#define ASSET_GENERATOR_H

#include "ast.h"
#include <stdio.h>

typedef struct asset_plan asset_plan;

/* Build the referenced-only physical pattern layout and snapshot its bytes. */
asset_plan *asset_generator_plan(ast_array_t declarations);
void asset_generator_free(asset_plan *plan);

int asset_generator_base(const asset_plan *plan, ast_t declaration);

/* Host data and initialization are emitted directly at the main() call site.
 * ZXN initialization is a single banked upload from PAGE_24 upward. */
void asset_generator_emit_init(const asset_plan *plan, FILE *output,
                               int target_zxn);

/* Emit the ordinary ZXN assembly intermediate.  Host and empty plans remove
 * a stale output left by an earlier build. */
void asset_generator_emit_asm(const asset_plan *plan, const char *path,
                              int target_zxn);

#endif
