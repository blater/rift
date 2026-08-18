#include "typechecker.h"
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "error.h"
#include "name_table.h"
#include "stringview.h"
#include "token.h"

rift_type_t get_error_type(void) {
  return (rift_type_t){error_type, {.builtin = 1}};
}

rift_type_t type_of_ast_type(typechecker_t tc, ast_type t) {
  string_view type_name = t.name.lexeme;
  if (svcmp(type_name, sv_from_cstr("int")) == 0)
    return (rift_type_t){builtin_int, {.builtin = 1}};
  if (svcmp(type_name, sv_from_cstr("char")) == 0)
    return (rift_type_t){builtin_char, {.builtin = 1}};
  if (svcmp(type_name, sv_from_cstr("string")) == 0)
    return (rift_type_t){builtin_string, {.builtin = 1}};
  if (svcmp(type_name, sv_from_cstr("bool")) == 0 ||
      svcmp(type_name, sv_from_cstr("boolean")) == 0)
    return (rift_type_t){builtin_bool, {.builtin = 1}};
  // User-defined type — look up in name table
  ast_t ref = get_ref(type_name, tc.nt);
  if (ref == NULL)
    return get_error_type();
  return (rift_type_t){user_defined, {.user_defined_type = {type_name}}};
}

rift_type_t get_type_of_expr(typechecker_t tc, ast_t expr) {
  if (expr->tag == literal) {
    token_type_t t = expr->data.literal.lit.type;
    switch (t) {
      case TOK_STR_LIT:
        return (rift_type_t){builtin_string, {.builtin = 1}};
      case TOK_CHR_LIT:
        return (rift_type_t){builtin_char, {.builtin = 1}};
      case TOK_NUM_LIT:
        return (rift_type_t){builtin_int, {.builtin = 1}};
      default:
        return get_error_type();
    }
  } else if (expr->tag == funcall) {
    ast_t fundef = get_ref(expr->data.funcall.name.lexeme, tc.nt);
    if (fundef == NULL)
      return get_error_type();
    ast_fundef fun = fundef->data.fundef;
    return type_of_ast_type(tc, fun.ret_type->data.type);
  }
  return get_error_type();
}

int are_types_compatibles(typechecker_t tc,
                          rift_type_t t1,
                          rift_type_t t2) {
  if (t1.tag == error_type || t2.tag == error_type)
    return 0;
  if (t1.tag == user_defined || t2.tag == user_defined)
    return 1;
  if (t1.tag == t2.tag)
    return 1;
  if ((t1.tag == builtin_char && t2.tag == builtin_int) ||
      (t1.tag == builtin_int && t2.tag == builtin_char))
    return 1;
  if ((t1.tag == builtin_bool && t2.tag == builtin_int) ||
      (t1.tag == builtin_int && t2.tag == builtin_bool))
    return 1;
  if ((t1.tag == builtin_bool && t2.tag == builtin_char) ||
      (t1.tag == builtin_char && t2.tag == builtin_bool))
    return 1;
  (void)tc;
  return 0;
}

int tc_program(ast_t program) {
  ast_program prog = program->data.program;
  typechecker_t tc;
  tc.current_function = NULL;

  tc.nt = new_name_table();
  push_nt(&tc.nt, sv_from_cstr("int"), NT_BUILTIN_TYPE, program);
  push_nt(&tc.nt, sv_from_cstr("char"), NT_BUILTIN_TYPE, program);
  push_nt(&tc.nt, sv_from_cstr("string"), NT_BUILTIN_TYPE, program);
  push_nt(&tc.nt, sv_from_cstr("boolean"), NT_BUILTIN_TYPE, program);
  new_nt_scope(&tc.nt);
  for (int i = 0; i < prog.prog.length; i++) {
    ast_t stmt = prog.prog.data[i];
    if (stmt->tag == tdef) {
      push_nt(&tc.nt, stmt->data.tdef.name.lexeme, NT_USER_TYPE, stmt);
    }
    if (stmt->tag == vardef) {
      push_nt(&tc.nt, stmt->data.vardef.name.lexeme, NT_VAR, stmt);
    }
    if (stmt->tag == fundef) {
      push_nt(&tc.nt, stmt->data.fundef.name.lexeme, NT_FUN, stmt);
    }
  }

  for (int i = 0; i < prog.prog.length; i++) {
    ast_t stmt = prog.prog.data[i];
    if (stmt->tag == vardef) {
      ast_vardef vardef = stmt->data.vardef;
      rift_type_t expr_type = get_type_of_expr(tc, vardef.expr);
      rift_type_t expected = type_of_ast_type(tc, vardef.type->data.type);
      if (!are_types_compatibles(tc, expr_type, expected))
        return 0;
    }
  }

  end_nt_scope(&tc.nt);
  return 1;
}

/* ============================================================
 * ADR-0003 §9.4 — structural-acyclicity check.
 * ============================================================ */

typedef struct cycle_visit_t {
  string_view name;
  struct cycle_visit_t *next;
} cycle_visit_t;

static int sv_in_chain(cycle_visit_t *chain, string_view target) {
  for (cycle_visit_t *c = chain; c; c = c->next) {
    if (svcmp(c->name, target) == 0) return 1;
  }
  return 0;
}

/* If `name` resolves to a user-type definition, store the tdef AST in
 * `*out_tdef` and return 1. Otherwise (builtin, unknown, or non-tdef)
 * return 0. */
static int resolve_user_tdef(name_table_t nt, string_view name, ast_t *out_tdef) {
  ast_t ref = get_ref(name, nt);
  if (!ref) return 0;
  nt_kind k = get_nt_kind(name, nt);
  if (k != NT_USER_TYPE) return 0;
  if (ref->tag != tdef) return 0;
  *out_tdef = ref;
  return 1;
}

/* Walk a tdef's field/variant graph. Reports cycles via error() and
 * returns 1 if a cycle was found, 0 otherwise. `visited` is the chain
 * of type names currently on the recursion stack, with the current
 * tdef's own name already pushed. */
static int walk_tdef(name_table_t nt, ast_t tdef_node, cycle_visit_t *visited) {
  ast_tdef td = tdef_node->data.tdef;
  ast_array_t fields;
  if (td.t == TDEF_REC || td.t == TDEF_PRO) {
    fields = td.constructors;
  } else { /* TDEF_MODULE */
    fields = td.module_fields;
  }

  for (int i = 0; i < fields.length; i++) {
    ast_t field = fields.data[i];
    ast_t field_type_node = NULL;
    string_view field_name;
    field_name.data = NULL;
    field_name.length = 0;

    if (field->tag == cons) {
      if (field->data.cons.type == NULL) continue; /* unit variant */
      field_type_node = field->data.cons.type;
      field_name = field->data.cons.name.lexeme;
    } else if (field->tag == vardef) {
      field_type_node = field->data.vardef.type;
      field_name = field->data.vardef.name.lexeme;
    } else {
      continue;
    }

    if (field_type_node == NULL || field_type_node->tag != type) continue;
    string_view base_name = field_type_node->data.type.name.lexeme;

    /* Cycle: this field's base type is already on the recursion stack. */
    if (sv_in_chain(visited, base_name)) {
      token_t origin = td.name;
      error(origin.filename, origin.line, origin.col,
            "recursive type definition '" SV_Fmt
            "' forbidden — field '" SV_Fmt "' contains '" SV_Fmt
            "' transitively",
            SV_Arg(origin.lexeme), SV_Arg(field_name), SV_Arg(base_name));
      error(origin.filename, origin.line, origin.col,
            "note: Rift rejects recursive type definitions so refcount-based "
            "reclamation is complete; express tree- or graph-shaped data as a "
            "flat collection with index references");
      return 1;
    }

    /* Recurse into the field's user-defined type, if any. */
    ast_t child_tdef = NULL;
    if (resolve_user_tdef(nt, base_name, &child_tdef)) {
      cycle_visit_t entry;
      entry.name = base_name;
      entry.next = visited;
      if (walk_tdef(nt, child_tdef, &entry)) return 1;
    }
  }
  return 0;
}

int check_acyclic_types(ast_t program) {
  ast_program prog = program->data.program;
  name_table_t nt = new_name_table();

  /* Builtins. */
  push_nt(&nt, sv_from_cstr("int"),     NT_BUILTIN_TYPE, program);
  push_nt(&nt, sv_from_cstr("byte"),    NT_BUILTIN_TYPE, program);
  push_nt(&nt, sv_from_cstr("word"),    NT_BUILTIN_TYPE, program);
  push_nt(&nt, sv_from_cstr("dword"),   NT_BUILTIN_TYPE, program);
  push_nt(&nt, sv_from_cstr("char"),    NT_BUILTIN_TYPE, program);
  push_nt(&nt, sv_from_cstr("string"),  NT_BUILTIN_TYPE, program);
  push_nt(&nt, sv_from_cstr("bool"),    NT_BUILTIN_TYPE, program);
  push_nt(&nt, sv_from_cstr("boolean"), NT_BUILTIN_TYPE, program);
  push_nt(&nt, sv_from_cstr("float"),   NT_BUILTIN_TYPE, program);

  /* Register all user types so forward references resolve. */
  for (int i = 0; i < prog.prog.length; i++) {
    ast_t stmt = prog.prog.data[i];
    if (stmt->tag == tdef) {
      push_nt(&nt, stmt->data.tdef.name.lexeme, NT_USER_TYPE, stmt);
    }
  }

  int errors = 0;
  for (int i = 0; i < prog.prog.length; i++) {
    ast_t stmt = prog.prog.data[i];
    if (stmt->tag == tdef) {
      cycle_visit_t origin;
      origin.name = stmt->data.tdef.name.lexeme;
      origin.next = NULL;
      if (walk_tdef(nt, stmt, &origin)) errors++;
    }
  }

  return errors == 0;
}
