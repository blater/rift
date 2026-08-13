/*****************************************************
 * ROCKER NAME_TABLE HEADER
 * MIT License
 * Copyright (c) 2024 Paul Passeron
 *****************************************************/

#ifndef NAME_TABLE_H
#define NAME_TABLE_H

#include "ast.h"
#include "stringview.h"

#define INIT_NT_CAP 1024

typedef enum nt_kind {
  NT_NOT_FOUND,
  NT_FUN,
  NT_VAR,
  NT_BUILTIN_TYPE,
  NT_USER_TYPE,
  NT_OPAQUE_TYPE,
  NT_ENUM_VARIANT,
} nt_kind;

typedef struct nt_lookup_t {
  int found;
  nt_kind kind;
  ast_t ref;
  int scope;
} nt_lookup_t;

typedef struct name_table_t {
  nt_kind *kinds;
  string_view *names;
  int *scopes;
  ast_array_t refs;
  int scope;
  int length;
  int capacity;
} name_table_t;

ast_t get_ref(string_view name, name_table_t table);
ast_t get_ref_by_kind(string_view name, nt_kind kind, name_table_t table);
nt_kind get_nt_kind(string_view name, name_table_t table);
nt_lookup_t lookup_nt(string_view name, name_table_t table);
nt_lookup_t lookup_nt_by_kind(string_view name, nt_kind kind,
                              name_table_t table);
int has_nt_in_current_scope(string_view name, nt_kind kind,
                            name_table_t table);
int push_nt_unique(name_table_t *table, string_view name, nt_kind kind,
                   ast_t ref);
void new_nt_scope(name_table_t *table);
void end_nt_scope(name_table_t *table);
void reallocate_table(name_table_t *table);
void push_nt(name_table_t *table, string_view name, nt_kind kind, ast_t ref);
name_table_t new_name_table(void);

#endif // NAME_TABLE_H
