/*****************************************************
 * ROCKER GENERATOR HEADER
 * MIT License
 * Copyright (c) 2024 Paul Passeron
 *****************************************************/

#ifndef GENERATOR_H
#define GENERATOR_H

#include "ast.h"
#include "component_manifest.h"
#include "name_table.h"
#include <stdio.h>

typedef enum {
  TARGET_HOST,
  TARGET_ZXN
} target_t;

// --- Unified scope tracking (linked lists, zero pre-allocation) ---

typedef enum {
  TRACK_STRING,   // emit __string_release
  TRACK_ARRAY,    // emit __internal_free_array(name, is_string_array)
  TRACK_HANDLE    // record/union/module — emit __handle_release(name)
} track_kind_t;

typedef struct tracked_var {
  struct tracked_var *next;
  string_view name;
  string_view type_name;     // aggregate type for its generated release walker
  track_kind_t kind;
  int is_string_array;   // only meaningful when kind == TRACK_ARRAY
  int owns_name;         // 1 if name.data was strdup'd and must be freed
} tracked_var_t;

typedef struct scope {
  struct scope *prev;       // outer scope (NULL at bottom of stack)
  tracked_var_t *vars;      // linked list head (most-recently-added first)
  int bump_mark_id;         // generated bump mark for this lexical region, or -1
} scope_t;

typedef struct generator_t generator_t;

typedef struct generator_t {
  FILE *f;
  name_table_t table;
  int str_tmp_counter;
  target_t target;
  FILE *pre_f;               // Buffer for pre-statements (ZXN statement splitting)
  char *pre_buf;             // Contents of pre_f buffer
  size_t pre_buf_size;       // Size of pre_buf
  string_view current_module_type; // Set while generating a module method body
  int in_global_scope;       // 1 when emitting top-level statements, 0 inside functions
  ast_array_t deferred_module_inits; // global module vars needing _new() in main()
  char **deferred_global_init_code; // runtime-init code to emit at start of main()
  int deferred_global_init_count;
  int deferred_global_init_capacity;
  ast_t program;             // top-level program node, set at top of transpile()
  scope_t *scope;             // top of scope stack (NULL when empty)
  int auto_cast;             // when set, wrap int args with (byte)/(word)/(dword) for matching callee params
  int zxn_test;              // when set, emit test-only ZXN emulator result markers
  int zxn_memory_profile;    // host build using the exact 1 KiB/6 KiB ZXN pools
  int lit_counter;            // ADR-0003 §7.1: unique id for each emitted static __string_block
  ast_t current_fundef;       // ADR-0003 §10.3: enclosing fundef during body emission (NULL otherwise)
  int bump_mark_counter;
  component_manifest *components;
  unsigned char direct_components[COMPONENT_MANIFEST_MAX_COMPONENTS];
  unsigned char closed_components[COMPONENT_MANIFEST_MAX_COMPONENTS];
  int component_order[COMPONENT_MANIFEST_MAX_COMPONENTS];
  int component_order_count;
  char *component_output_path;
  int in_main_body;
  int main_needs_epilogue;
  int select_all_components;
  unsigned char opaque_value_used[COMPONENT_MANIFEST_MAX_INTERFACES];
  unsigned char opaque_array_used[COMPONENT_MANIFEST_MAX_INTERFACES];
  int zxn_tiny_eligible;
  int zxn_tiny_uses_stdout;
  int zxn_tiny_simple_stdout;
} generator_t;

generator_t new_generator(char *filename, const char *output_base,
                          component_manifest *components);
void kill_generator(generator_t g);

void transpile(generator_t *g, ast_t program);

#endif // GENERATOR_H
