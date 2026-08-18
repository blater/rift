#include "ownership.h"
#include "stringview.h"
#include "type_info.h"
#include <stdlib.h>
#include <string.h>

static const string_view SV_STRING = {.data = "string", .length = 6};

#define UNION_KEY_FIELD "key"
#define UNION_VALUE_FIELD "value"

void push_scope(generator_t *g, int bump_mark_id) {
  scope_t *scope = malloc(sizeof(*scope));
  scope->prev = g->scope;
  scope->vars = NULL;
  scope->bump_mark_id = bump_mark_id;
  g->scope = scope;
}

static void track_var(generator_t *g, string_view name, track_kind_t kind,
                      int is_string_array, int owns_name,
                      string_view type_name) {
  if (!g->scope) return;
  tracked_var_t *var = malloc(sizeof(*var));
  var->name = name;
  var->type_name = type_name;
  var->kind = kind;
  var->is_string_array = is_string_array;
  var->owns_name = owns_name;
  var->next = g->scope->vars;
  g->scope->vars = var;
}

void emit_scope_cleanup(generator_t *g) {
  if (!g->scope) return;
  for (tracked_var_t *var = g->scope->vars; var; var = var->next) {
    switch (var->kind) {
    case TRACK_STRING:
      fprintf(g->f, "__string_release(" SV_Fmt ");\n", SV_Arg(var->name));
      break;
    case TRACK_ARRAY:
      fprintf(g->f, "__internal_free_array(" SV_Fmt ", %d);\n",
              SV_Arg(var->name), var->is_string_array);
      break;
    case TRACK_HANDLE:
      fprintf(g->f, "__rift_release_" SV_Fmt "(" SV_Fmt ");\n",
              SV_Arg(var->type_name), SV_Arg(var->name));
      break;
    }
  }
}

void pop_scope(generator_t *g) {
  if (!g->scope) return;
  scope_t *top = g->scope;
  g->scope = top->prev;
  tracked_var_t *var = top->vars;
  while (var) {
    tracked_var_t *next = var->next;
    if (var->owns_name) free((char *)var->name.data);
    free(var);
    var = next;
  }
  free(top);
}

void emit_return_cleanup(generator_t *g, string_view skip_name) {
  for (scope_t *scope = g->scope; scope; scope = scope->prev) {
    for (tracked_var_t *var = scope->vars; var; var = var->next) {
      if (skip_name.length > 0 && svcmp(var->name, skip_name) == 0) continue;
      switch (var->kind) {
      case TRACK_STRING:
        fprintf(g->f, "__string_release(" SV_Fmt ");\n",
                SV_Arg(var->name));
        break;
      case TRACK_HANDLE:
        fprintf(g->f, "__rift_release_" SV_Fmt "(" SV_Fmt ");\n",
                SV_Arg(var->type_name), SV_Arg(var->name));
        break;
      case TRACK_ARRAY:
        fprintf(g->f, "__internal_free_array(" SV_Fmt ", %d);\n",
                SV_Arg(var->name), var->is_string_array);
        break;
      }
    }
  }
  for (scope_t *scope = g->scope; scope; scope = scope->prev)
    if (scope->bump_mark_id >= 0)
      fprintf(g->f, "rift_bump_restore(__bm_%d);\n", scope->bump_mark_id);
}

void track_string_var(generator_t *g, string_view name) {
  track_var(g, name, TRACK_STRING, 0, 0, sv_from_cstr(""));
}

void track_string_tmp(generator_t *g, const char *tmp_name) {
  string_view name = {.data = (char *)tmp_name, .length = strlen(tmp_name)};
  track_var(g, name, TRACK_STRING, 0, 1, sv_from_cstr(""));
}

void track_array_var(generator_t *g, string_view name, int is_string_array) {
  track_var(g, name, TRACK_ARRAY, is_string_array, 0, sv_from_cstr(""));
}

void track_handle_var(generator_t *g, string_view name,
                      string_view type_name) {
  track_var(g, name, TRACK_HANDLE, 0, 0, type_name);
}

void emit_nullify_tmp(FILE *f, const char *tmp_name) {
  fprintf(f,
          "%s.data = NULL; %s.length = 0; %s.capacity = 0; "
          "%s.backing = NULL;\n",
          tmp_name, tmp_name, tmp_name, tmp_name);
}

static void emit_owned_field_release(generator_t *g, ast_t type_node,
                                     const char *owner,
                                     string_view field_name) {
  if (!type_node || type_node->tag != type) return;
  ast_type field_type = type_node->data.type;
  if (field_type.is_array) {
    fprintf(g->f, "__internal_free_array(%s->" SV_Fmt ", %d);\n", owner,
            SV_Arg(field_name), svcmp(field_type.name.lexeme, SV_STRING) == 0);
  } else if (svcmp(field_type.name.lexeme, SV_STRING) == 0) {
    fprintf(g->f, "__string_release(%s->" SV_Fmt ");\n", owner,
            SV_Arg(field_name));
  } else if (is_heap_allocated_type(field_type.name.lexeme, g->table)) {
    fprintf(g->f, "__rift_release_" SV_Fmt "(%s->" SV_Fmt ");\n",
            SV_Arg(field_type.name.lexeme), owner, SV_Arg(field_name));
  }
}

void emit_type_release_walker(generator_t *g, string_view name,
                              ast_tdef tdef, int is_module) {
  fprintf(g->f, "static void __rift_release_" SV_Fmt "(void *__raw) {\n",
          SV_Arg(name));
  fprintf(g->f, "  " SV_Fmt " __rift_value = (" SV_Fmt ")__raw;\n",
          SV_Arg(name), SV_Arg(name));
  fprintf(g->f, "  if (!__rift_value) return;\n");
  fprintf(g->f, "  rift_block_header *header = "
                "((rift_block_header *)__rift_value) - 1;\n");
  fprintf(g->f, "  if (header->refcount == RIFT_RC_STATIC) return;\n");
  fprintf(g->f, "  if (header->refcount == RIFT_RC_FREE || "
                "header->refcount == RIFT_RC_MAGAZINE) { "
                "fprintf(stderr, \"rift: aggregate release on already-freed "
                "block\\n\"); exit(1); }\n");
  fprintf(g->f, "  if (--header->refcount != 0) return;\n");

  if (is_module) {
    for (int i = 0; i < tdef.module_fields.length; i++) {
      ast_vardef field = tdef.module_fields.data[i]->data.vardef;
      emit_owned_field_release(g, field.type, "__rift_value",
                               field.name.lexeme);
    }
  } else if (tdef.t == TDEF_PRO) {
    fprintf(g->f, "  switch (__rift_value->" UNION_KEY_FIELD ") {\n");
    for (int i = 0; i < tdef.constructors.length; i++) {
      ast_cons constructor = tdef.constructors.data[i]->data.cons;
      fprintf(g->f, "  case " SV_Fmt ":\n", SV_Arg(constructor.name.lexeme));
      if (constructor.type && constructor.type->tag == type) {
        ast_type payload = constructor.type->data.type;
        if (payload.is_array) {
          fprintf(g->f, "    __internal_free_array(__rift_value->"
                        UNION_VALUE_FIELD "." SV_Fmt ", %d);\n",
                  SV_Arg(constructor.name.lexeme),
                  svcmp(payload.name.lexeme, SV_STRING) == 0);
        } else if (svcmp(payload.name.lexeme, SV_STRING) == 0) {
          fprintf(g->f, "    __string_release(__rift_value->"
                        UNION_VALUE_FIELD "." SV_Fmt ");\n",
                  SV_Arg(constructor.name.lexeme));
        } else if (is_heap_allocated_type(payload.name.lexeme, g->table)) {
          fprintf(g->f, "    __rift_release_" SV_Fmt "(__rift_value->"
                        UNION_VALUE_FIELD "." SV_Fmt ");\n",
                  SV_Arg(payload.name.lexeme), SV_Arg(constructor.name.lexeme));
        }
      }
      fprintf(g->f, "    break;\n");
    }
    fprintf(g->f, "  }\n");
  } else {
    for (int i = 0; i < tdef.constructors.length; i++) {
      ast_cons field = tdef.constructors.data[i]->data.cons;
      emit_owned_field_release(g, field.type, "__rift_value",
                               field.name.lexeme);
    }
  }
  fprintf(g->f, "  rift_longlived_free(__rift_value);\n}\n");
}
