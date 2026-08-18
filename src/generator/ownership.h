#ifndef GENERATOR_OWNERSHIP_H
#define GENERATOR_OWNERSHIP_H

#include "internal.h"

void push_scope(generator_t *g, int bump_mark_id);
void pop_scope(generator_t *g);
void emit_scope_cleanup(generator_t *g);
void emit_return_cleanup(generator_t *g, string_view skip_name);
void track_string_var(generator_t *g, string_view name);
void track_string_tmp(generator_t *g, const char *tmp_name);
void track_array_var(generator_t *g, string_view name, int is_string_array);
void track_handle_var(generator_t *g, string_view name, string_view type_name);
void emit_nullify_tmp(FILE *f, const char *tmp_name);
void emit_type_release_walker(generator_t *g, string_view name,
                              ast_tdef tdef, int is_module);

#endif
