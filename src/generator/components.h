#ifndef GENERATOR_COMPONENTS_H
#define GENERATOR_COMPONENTS_H

#include "internal.h"

int component_index(generator_t *g, const char *id);
void record_component(generator_t *g, const char *id);
void record_fundef_component(generator_t *g, ast_t ref);

void generator_collect_component_uses(generator_t *g, ast_t program);
void generator_compute_component_closure(generator_t *g);
void generator_write_component_output(generator_t *g);
void generator_emit_manifest_headers(generator_t *g);
void generator_emit_component_init(generator_t *g);
void generator_emit_component_shutdown(generator_t *g);

#endif
