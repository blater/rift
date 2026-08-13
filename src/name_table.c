#include "name_table.h"
#include "lib/alloc.h"
#include <stdio.h>
#include <string.h>

name_table_t new_name_table(void) {
  name_table_t res;
  res.length = 0;
  res.capacity = INIT_NT_CAP;
  res.scope = 0;
  res.refs = new_ast_array();
  res.kinds = allocate_compiler_persistent(sizeof(nt_kind) * res.capacity);
  res.names =
      allocate_compiler_persistent(sizeof(string_view) * res.capacity);
  res.scopes = allocate_compiler_persistent(sizeof(int) * res.capacity);
  return res;
}

nt_lookup_t lookup_nt(string_view name, name_table_t table) {
  for (int i = table.length - 1; i >= 0; i--) {
    if (svcmp(table.names[i], name) == 0) {
      return (nt_lookup_t){1, table.kinds[i], table.refs.data[i],
                           table.scopes[i]};
    }
  }
  return (nt_lookup_t){0, NT_NOT_FOUND, NULL, -1};
}

nt_lookup_t lookup_nt_by_kind(string_view name, nt_kind kind,
                              name_table_t table) {
  for (int i = table.length - 1; i >= 0; i--) {
    if (table.kinds[i] == kind && svcmp(table.names[i], name) == 0) {
      return (nt_lookup_t){1, kind, table.refs.data[i], table.scopes[i]};
    }
  }
  return (nt_lookup_t){0, NT_NOT_FOUND, NULL, -1};
}

ast_t get_ref(string_view name, name_table_t table) {
  return lookup_nt(name, table).ref;
}

ast_t get_ref_by_kind(string_view name, nt_kind kind, name_table_t table) {
  return lookup_nt_by_kind(name, kind, table).ref;
}

nt_kind get_nt_kind(string_view name, name_table_t table) {
  return lookup_nt(name, table).kind;
}

int has_nt_in_current_scope(string_view name, nt_kind kind,
                            name_table_t table) {
  for (int i = table.length - 1; i >= 0; i--) {
    if (table.scopes[i] < table.scope) break;
    if (table.scopes[i] == table.scope && table.kinds[i] == kind &&
        svcmp(table.names[i], name) == 0)
      return 1;
  }
  return 0;
}

void new_nt_scope(name_table_t *table) {
  table->scope++;
  return;
}

void end_nt_scope(name_table_t *table) {
  if (table->scope <= 0) return;
  for (int i = table->length - 1; i >= 0; i--) {
    if (table->scopes[i] >= table->scope) {
      table->length--;
      table->refs.length--;
    } else
      break;
  }
  table->scope--;
}

void reallocate_table(name_table_t *table) {
  table->capacity *= 2;
  table->names = reallocate_compiler_persistent(
      table->names, table->capacity * sizeof(string_view));
  table->kinds = reallocate_compiler_persistent(
      table->kinds, table->capacity * sizeof(nt_kind));
  table->scopes = reallocate_compiler_persistent(table->scopes,
                                                 table->capacity * sizeof(int));
}

void push_nt(name_table_t *table, string_view name, nt_kind kind, ast_t ref) {
  if (table->length >= table->capacity) {
    reallocate_table(table);
  }
  table->names[table->length] = name;
  table->kinds[table->length] = kind;
  table->scopes[table->length] = table->scope;
  push_ast_array(&table->refs, ref);
  table->length++;
}

int push_nt_unique(name_table_t *table, string_view name, nt_kind kind,
                   ast_t ref) {
  if (has_nt_in_current_scope(name, kind, *table)) return 0;
  push_nt(table, name, kind, ref);
  return 1;
}
