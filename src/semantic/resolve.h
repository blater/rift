#ifndef SEMANTIC_RESOLVE_H
#define SEMANTIC_RESOLVE_H

#include "generator/generator.h"

void semantic_prepare_program(generator_t *g, ast_t program);
int is_asset_only_module(generator_t *g, ast_t definition);

#endif
