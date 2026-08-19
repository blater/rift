#ifndef GENERATOR_INTERNAL_H
#define GENERATOR_INTERNAL_H

#include "assets.h"
#include "generator.h"
#include "name_table.h"
#include <stdio.h>

typedef struct semantic_plan_program semantic_plan_program;

typedef enum {
  TRACK_STRING,
  TRACK_ARRAY,
  TRACK_HANDLE
} track_kind_t;

typedef struct tracked_var {
  struct tracked_var *next;
  string_view name;
  string_view type_name;
  track_kind_t kind;
  int is_string_array;
  int owns_name;
} tracked_var_t;

typedef struct scope {
  struct scope *prev;
  tracked_var_t *vars;
  int bump_mark_id;
} scope_t;

struct generator_t {
  FILE *f;
  name_table_t table;
  int str_tmp_counter;
  target_t target;
  FILE *pre_f;
  char *pre_buf;
  size_t pre_buf_size;
  string_view current_module_type;
  int in_global_scope;
  ast_array_t deferred_module_inits;
  char **deferred_global_init_code;
  int deferred_global_init_count;
  int deferred_global_init_capacity;
  ast_t program;
  scope_t *scope;
  int auto_cast;
  int zxn_test;
  int lit_counter;
  ast_t current_fundef;
  int bump_mark_counter;
  component_manifest *components;
  unsigned char direct_components[COMPONENT_MANIFEST_MAX_COMPONENTS];
  unsigned char closed_components[COMPONENT_MANIFEST_MAX_COMPONENTS];
  int component_order[COMPONENT_MANIFEST_MAX_COMPONENTS];
  int component_order_count;
  char *component_output_path;
  char *asset_asm_output_path;
  ast_array_t asset_decls;
  asset_plan *assets;
  int in_main_body;
  int main_needs_epilogue;
  int select_all_components;
  int semantic_plan_enabled;
  semantic_plan_program *semantic_plans;
  unsigned char opaque_value_used[COMPONENT_MANIFEST_MAX_INTERFACES];
  unsigned char opaque_array_used[COMPONENT_MANIFEST_MAX_INTERFACES];
  int zxn_tiny_eligible;
  int zxn_tiny_uses_stdout;
  int zxn_tiny_simple_stdout;
  int zxn_light_core_eligible;
  int zxn_pools_required;
  int zxn_bump_required;
};

#endif
