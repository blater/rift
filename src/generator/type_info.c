#include "type_info.h"
#include "error.h"
#include "stringview.h"
#include <stdlib.h>
#include <string.h>

static const string_view SV_STRING = {.data = "string", .length = 6};

static int numeric_literal_is_float(string_view text) {
  for (size_t i = 0; i < text.length; i++)
    if (text.data[i] == '.' || text.data[i] == 'e' || text.data[i] == 'E')
      return 1;
  return 0;
}

static int comparison_operator(token_type_t op) {
  return op == TOK_LSSR || op == TOK_LSSR_EQ || op == TOK_GRTR ||
         op == TOK_GRTR_EQ || op == TOK_EQUAL || op == TOK_DIFF ||
         op == TOK_LOG_AND || op == TOK_LOG_OR;
}

static int type_is(string_view type, const char *name) {
  return svcmp(type, sv_from_cstr((char *)name)) == 0;
}

static string_view arithmetic_result_type(string_view left,
                                          string_view right) {
  if (type_is(left, "float") || type_is(right, "float"))
    return sv_from_cstr("float");
  if (type_is(left, "dword") || type_is(right, "dword"))
    return sv_from_cstr("dword");
  if (type_is(left, "word") || type_is(right, "word"))
    return sv_from_cstr("word");
  if (left.length && right.length) return sv_from_cstr("int");
  return sv_from_cstr("");
}

char *capture_expression(generator_t *g, ast_t expr);
void generate_expression(generator_t *g, ast_t expr);
int is_builtin_typename(char *name);

token_t token_for_expr(ast_t expr) {
  if (!expr) {
    token_t tok = {0};
    tok.type = TOK_EOF;
    tok.lexeme = sv_from_cstr("<expr>");
    tok.filename = "unknown";
    return tok;
  }

  if (expr->tag == identifier) return expr->data.identifier.id;
  if (expr->tag == literal) return expr->data.literal.lit;
  if (expr->tag == funcall) return expr->data.funcall.name;
  if (expr->tag == method_call) return expr->data.method_call.method;
  if (expr->tag == sub) return token_for_expr(expr->data.sub.expr);
  if (expr->tag == arr_index) return token_for_expr(expr->data.arr_index.array);
  if (expr->tag == vardef) return expr->data.vardef.name;
  if (expr->tag == cons) return expr->data.cons.name;

  token_t tok = {0};
  tok.type = TOK_EOF;
  tok.lexeme = sv_from_cstr("<expr>");
  tok.filename = "unknown";
  return tok;
}

void generate_type(FILE *f, ast_t a) {
  ast_type type = a->data.type;
  if (type.is_array)
    fprintf(f, "__internal_dynamic_array_t");
  else if (svcmp(type.name.lexeme, sv_from_cstr("Sprite")) == 0)
    fprintf(f, "byte");
  else
    fprintf(f, SV_Fmt, SV_Arg(type.name.lexeme));
}

// Helper: Extract type from a variable identifier (used by array operations)
// Returns type name on success, exits on error
string_view get_array_var_type(string_view var_name, name_table_t table,
                                token_t tok) {
  ast_t ref = get_ref(var_name, table);
  if (ref == NULL) {
    error(tok.filename, tok.line, tok.col, "Array is not declared in the current scope");
  }
  if (ref->tag == vardef) {
    ast_type type = ref->data.vardef.type->data.type;
    return type.name.lexeme;
  }
  if (ref->tag == fundef) {
    // Parameter lookup (e.g. "this" in an array method body)
    ast_fundef fd = ref->data.fundef;
    for (int i = 0; i < fd.args.length; i++) {
      if (svcmp(fd.args.data[i].lexeme, var_name) == 0) {
        ast_type type = fd.types.data[i]->data.type;
        return type.name.lexeme;
      }
    }
  }
  error(tok.filename, tok.line, tok.col, "Arrays must be declared as variables (got tag %d)",
        ref->tag);
  return sv_from_cstr(""); // unreachable
}

// Helper: Get the declared type of a variable identifier
// Returns type name as string_view, or empty string_view if not found
string_view get_var_type(string_view name, name_table_t table) {
  ast_t ref = get_ref(name, table);
  if (!ref) return sv_from_cstr("");

  if (ref->tag == vardef) {
    ast_type type = ref->data.vardef.type->data.type;
    return type.name.lexeme;
  }

  if (ref->tag == fundef) {
    // Look up parameter type in function definition
    ast_fundef fundef = ref->data.fundef;
    for (int i = 0; i < fundef.args.length; i++) {
      if (svcmp(fundef.args.data[i].lexeme, name) == 0) {
        ast_type type = fundef.types.data[i]->data.type;
        return type.name.lexeme;
      }
    }
  }

  return sv_from_cstr("");
}

// Resolve an identifier's declared array type. Parameters are represented in
// the name table by their enclosing function definition, so handle both local
// variable and parameter entries here.
int get_identifier_array_type(string_view name, name_table_t table,
                              ast_type *result) {
  ast_t ref = get_ref(name, table);
  if (!ref) return 0;

  if (ref->tag == vardef) {
    ast_type declared_type = ref->data.vardef.type->data.type;
    if (!declared_type.is_array) return 0;
    if (result) *result = declared_type;
    return 1;
  }

  if (ref->tag == fundef) {
    ast_fundef fundef = ref->data.fundef;
    for (int i = 0; i < fundef.args.length; i++) {
      if (svcmp(fundef.args.data[i].lexeme, name) == 0) {
        ast_type declared_type = fundef.types.data[i]->data.type;
        if (!declared_type.is_array) return 0;
        if (result) *result = declared_type;
        return 1;
      }
    }
  }

  return 0;
}

// Returns 1 if the named variable is a scalar (non-array) string type
int is_scalar_string_var(string_view name, name_table_t table) {
  ast_t ref = get_ref(name, table);
  if (!ref) return 0;
  if (ref->tag == vardef) {
    ast_type type = ref->data.vardef.type->data.type;
    if (type.is_array) return 0;
    return svcmp(type.name.lexeme, SV_STRING) == 0;
  }
  if (ref->tag == fundef) {
    ast_fundef fundef = ref->data.fundef;
    for (int i = 0; i < fundef.args.length; i++) {
      if (svcmp(fundef.args.data[i].lexeme, name) == 0) {
        ast_type type = fundef.types.data[i]->data.type;
        if (type.is_array) return 0;
        return svcmp(type.name.lexeme, SV_STRING) == 0;
      }
    }
  }
  return 0;
}

// An RHS expression "borrows" from existing storage rather than allocating
// fresh. Borrowers (identifier, field read, or array read) need a retain at
// the destination so source and destination each carry an rc share.
// Producers (funcall via __return_T, record literals) come pre-retained.
int rhs_is_borrower(ast_t expr) {
  if (!expr) return 0;
  if (expr->tag == identifier || expr->tag == sub || expr->tag == arr_index)
    return 1;
  if (expr->tag != funcall ||
      svcmp(expr->data.funcall.name.lexeme, sv_from_cstr("get")) != 0)
    return 0;
  ast_t target = expr->data.funcall.resolved_target;
  return !target || target->tag != fundef || target->data.fundef.body == NULL;
}

int expr_is_array(ast_t expr, name_table_t table, int *is_string) {
  ast_type type = {0};
  ast_t ref = NULL;
  if (!expr) return 0;
  if (expr->tag == identifier) ref = get_ref(expr->data.identifier.id.lexeme, table);
  else if (expr->tag == funcall) {
    ast_funcall call = expr->data.funcall;
    ref = call.resolved_target ? call.resolved_target
                               : get_ref(call.name.lexeme, table);
  }
  else if (expr->tag == method_call) {
    ast_method_call call = expr->data.method_call;
    ref = call.is_resolved ? call.resolved_target : NULL;
  }
  if (!ref || (ref->tag != fundef && ref->tag != vardef)) return 0;
  if (ref->tag == vardef) type = ref->data.vardef.type->data.type;
  else if ((expr->tag == funcall || expr->tag == method_call) &&
           ref->data.fundef.ret_type)
    type = ref->data.fundef.ret_type->data.type;
  else return 0;
  if (!type.is_array) return 0;
  if (is_string) *is_string = svcmp(type.name.lexeme, SV_STRING) == 0;
  return 1;
}

/* A container constructor consumes an owned value.  The compiler supplies an
 * additional reference only when the source expression borrows an existing
 * value; producer expressions transfer their single result reference. */
void emit_borrowed_container_retain(generator_t *g, ast_t expr,
                                    ast_type value_type) {
  if (!rhs_is_borrower(expr)) return;
  char *expr_text = capture_expression(g, expr);
  if (value_type.is_array) {
    fprintf(g->pre_f, "__internal_retain_array(%s);\n", expr_text);
  } else if (svcmp(value_type.name.lexeme, SV_STRING) == 0) {
    fprintf(g->pre_f, "__string_retain(%s);\n", expr_text);
  } else if (is_heap_allocated_type(value_type.name.lexeme, g->table)) {
    fprintf(g->pre_f, "__handle_retain(%s);\n", expr_text);
  }
  free(expr_text);
}

// Returns 1 if the type is a pointer-allocated user type (module, record, or union).
int is_heap_allocated_type(string_view type_name, name_table_t table) {
  if (is_builtin_typename(string_of_sv(type_name))) return 0;
  if (lookup_nt_by_kind(type_name, NT_OPAQUE_TYPE, table).found) return 1;
  ast_t ref = get_ref_by_kind(type_name, NT_USER_TYPE, table);
  return ref && ref->tag == tdef;
}

// Returns 1 if name resolves to a non-array vardef/parameter whose declared
// type is a record/union/module — i.e. an aggregate handle subject to
// __handle_retain / __handle_release. Mirrors is_scalar_string_var.
int is_scalar_aggregate_var(string_view name, name_table_t table) {
  ast_t ref = get_ref(name, table);
  if (!ref) return 0;
  if (ref->tag == vardef) {
    ast_type t = ref->data.vardef.type->data.type;
    if (t.is_array) return 0;
    return is_heap_allocated_type(t.name.lexeme, table);
  }
  if (ref->tag == fundef) {
    ast_fundef fd = ref->data.fundef;
    for (int i = 0; i < fd.args.length; i++) {
      if (svcmp(fd.args.data[i].lexeme, name) == 0) {
        ast_type t = fd.types.data[i]->data.type;
        if (t.is_array) return 0;
        return is_heap_allocated_type(t.name.lexeme, table);
      }
    }
  }
  return 0;
}

// Helper: Build an array type name string_view, e.g. "int" → "int_array".
string_view make_array_type_sv(string_view base) {
  char *buf = allocate_compiler_persistent(base.length + 7);
  sprintf(buf, SV_Fmt "_array", SV_Arg(base));
  return sv_from_cstr(buf);
}

// Helper: Return the declared type name of any field in a user-defined record.
// For scalar fields returns the type name (e.g. "Address").
// For array fields returns the element type name (e.g. "Person" for Person[]).
// Returns empty string_view if the record or field is not found.
string_view get_field_type(string_view base_type, string_view field_name,
                           name_table_t table) {
  ast_t tdef_ref = get_ref_by_kind(base_type, NT_USER_TYPE, table);
  if (!tdef_ref || tdef_ref->tag != tdef) return sv_from_cstr("");
  ast_tdef td = tdef_ref->data.tdef;
  if (td.t == TDEF_MODULE) {
    for (int i = 0; i < td.module_fields.length; i++) {
      ast_vardef field = td.module_fields.data[i]->data.vardef;
      if (svcmp(field.name.lexeme, field_name) == 0)
        return field.type->data.type.name.lexeme;
    }
    return sv_from_cstr("");
  }
  for (int i = 0; i < td.constructors.length; i++) {
    ast_t cons_ast = td.constructors.data[i];
    if (cons_ast->tag != cons) continue;
    ast_cons c = cons_ast->data.cons;
    if (svcmp(c.name.lexeme, field_name) != 0) continue;
    if (c.type && c.type->tag == type)
      return c.type->data.type.name.lexeme;
  }
  return sv_from_cstr("");
}

// Returns 1 if the sub expression's terminal field is a scalar (non-array) string.
int is_sub_target_scalar_string(ast_t sub_expr, name_table_t table) {
  if (!sub_expr || sub_expr->tag != sub) return 0;
  ast_sub s = sub_expr->data.sub;
  string_view current_type = infer_expr_type(s.receiver, table);
  if (current_type.length == 0) return 0;
  for (int i = 0; i < s.path.length; i++) {
    current_type = get_field_type(current_type, s.path.data[i].lexeme, table);
    if (current_type.length == 0) return 0;
  }
  // Look up the terminal field's full type info (including is_array)
  string_view field_name = s.expr->data.identifier.id.lexeme;
  ast_t tdef_ref = get_ref_by_kind(current_type, NT_USER_TYPE, table);
  if (!tdef_ref || tdef_ref->tag != tdef) return 0;
  ast_tdef td = tdef_ref->data.tdef;
  for (int i = 0; i < td.constructors.length; i++) {
    ast_t cons_ast = td.constructors.data[i];
    if (cons_ast->tag != cons) continue;
    ast_cons c = cons_ast->data.cons;
    if (svcmp(c.name.lexeme, field_name) != 0) continue;
    if (c.type && c.type->tag == type) {
      ast_type ft = c.type->data.type;
      return (!ft.is_array && svcmp(ft.name.lexeme, SV_STRING) == 0);
    }
  }
  return 0;
}

// Returns 1 if the sub expression's terminal field is a scalar (non-array)
// aggregate handle (record/union/module). Mirrors is_sub_target_scalar_string.
int is_sub_target_scalar_aggregate(ast_t sub_expr, name_table_t table) {
  if (!sub_expr || sub_expr->tag != sub) return 0;
  ast_sub s = sub_expr->data.sub;
  string_view current_type = infer_expr_type(s.receiver, table);
  if (current_type.length == 0) return 0;
  for (int i = 0; i < s.path.length; i++) {
    current_type = get_field_type(current_type, s.path.data[i].lexeme, table);
    if (current_type.length == 0) return 0;
  }
  string_view field_name = s.expr->data.identifier.id.lexeme;
  ast_t tdef_ref = get_ref_by_kind(current_type, NT_USER_TYPE, table);
  if (!tdef_ref || tdef_ref->tag != tdef) return 0;
  ast_tdef td = tdef_ref->data.tdef;
  for (int i = 0; i < td.constructors.length; i++) {
    ast_t cons_ast = td.constructors.data[i];
    if (cons_ast->tag != cons) continue;
    ast_cons c = cons_ast->data.cons;
    if (svcmp(c.name.lexeme, field_name) != 0) continue;
    if (c.type && c.type->tag == type) {
      ast_type ft = c.type->data.type;
      return (!ft.is_array && is_heap_allocated_type(ft.name.lexeme, table));
    }
  }
  return 0;
}

/* Returns the element type of an array-valued field assignment target, or an
 * empty view when the target is not an array field. */
string_view sub_target_array_element_type(ast_t sub_expr,
                                          name_table_t table) {
  if (!sub_expr || sub_expr->tag != sub) return sv_from_cstr("");
  ast_sub s = sub_expr->data.sub;
  string_view current_type = infer_expr_type(s.receiver, table);
  if (current_type.length == 0) return sv_from_cstr("");
  for (int i = 0; i < s.path.length; i++) {
    current_type = get_field_type(current_type, s.path.data[i].lexeme, table);
    if (current_type.length == 0) return sv_from_cstr("");
  }
  if (!s.expr || s.expr->tag != identifier) return sv_from_cstr("");
  return try_get_field_array_type(current_type, s.expr->data.identifier.id.lexeme, table);
}

// Helper: Infer the Rift type name of an arbitrary expression.
// Returns empty string_view when the type cannot be determined.
string_view infer_expr_type(ast_t expr, name_table_t table) {
  if (!expr) return sv_from_cstr("");

  if (expr->tag == identifier) {
    if (svcmp(expr->data.identifier.id.lexeme, sv_from_cstr("true")) == 0 ||
        svcmp(expr->data.identifier.id.lexeme, sv_from_cstr("false")) == 0)
      return sv_from_cstr("boolean");
    ast_t ref = get_ref(expr->data.identifier.id.lexeme, table);
    if (!ref) return sv_from_cstr("");
    if (ref->tag == vardef) {
      ast_type t = ref->data.vardef.type->data.type;
      return t.is_array ? make_array_type_sv(t.name.lexeme) : t.name.lexeme;
    }
    if (ref->tag == fundef) {
      ast_fundef fd = ref->data.fundef;
      string_view name = expr->data.identifier.id.lexeme;
      for (int i = 0; i < fd.args.length; i++) {
        if (svcmp(fd.args.data[i].lexeme, name) == 0) {
          ast_type t = fd.types.data[i]->data.type;
          return t.is_array ? make_array_type_sv(t.name.lexeme) : t.name.lexeme;
        }
      }
    }
    if (ref->tag == iter_loop &&
        svcmp(ref->data.iter_loop.variable.lexeme,
              expr->data.identifier.id.lexeme) == 0)
      return get_array_element_type(ref->data.iter_loop.iterable, table,
                                    ref->data.iter_loop.variable);
    if (ref->tag == loop &&
        svcmp(ref->data.loop.variable.lexeme,
              expr->data.identifier.id.lexeme) == 0)
      return sv_from_cstr("int");
    return sv_from_cstr("");
  }

  if (expr->tag == literal) {
    token_type_t t = expr->data.literal.lit.type;
    if (t == TOK_STR_LIT) return SV_STRING;
    if (t == TOK_NUM_LIT)
      return numeric_literal_is_float(expr->data.literal.lit.lexeme)
                 ? sv_from_cstr("float")
                 : sv_from_cstr("int");
    if (t == TOK_CHR_LIT) return sv_from_cstr("char");
    return sv_from_cstr("");
  }

  if (expr->tag == funcall) {
    ast_funcall call = expr->data.funcall;
    // Array reads preserve the nominal Rift element type even when its C
    // storage maps to an existing scalar helper.
    int resolved_user = call.resolved_target &&
                        call.resolved_target->tag == fundef &&
                        call.resolved_target->data.fundef.body != NULL;
    int array_read = svcmp(call.name.lexeme, sv_from_cstr("get")) == 0 ||
                     svcmp(call.name.lexeme, sv_from_cstr("pop")) == 0;
    if (!resolved_user && array_read && call.args.length >= 1)
      return get_array_element_type(call.args.data[0], table, call.name);
    // User-defined function → look up declared return type
    ast_t ref = call.resolved_target ? call.resolved_target
                                     : get_ref(call.name.lexeme, table);
    if (ref && ref->tag == fundef && ref->data.fundef.ret_type) {
      ast_type rt = ref->data.fundef.ret_type->data.type;
      return rt.is_array ? make_array_type_sv(rt.name.lexeme) : rt.name.lexeme;
    }
    return sv_from_cstr("");
  }

  if (expr->tag == method_call) {
    ast_method_call mc = expr->data.method_call;
    ast_t ref = mc.is_resolved ? mc.resolved_target : NULL;
    if (ref && ref->tag == fundef && ref->data.fundef.ret_type) {
      ast_type rt = ref->data.fundef.ret_type->data.type;
      return rt.is_array ? make_array_type_sv(rt.name.lexeme) : rt.name.lexeme;
    }
    return sv_from_cstr("");
  }

  if (expr->tag == sub) {
    ast_sub s = expr->data.sub;
    // expr must be the terminal field identifier
    if (!s.expr || s.expr->tag != identifier) return sv_from_cstr("");
    if (!s.receiver) return sv_from_cstr("");
    // Start from the receiver expression's type
    string_view current_type = infer_expr_type(s.receiver, table);
    if (current_type.length == 0) return sv_from_cstr("");
    // Walk any intermediate path segments
    for (int i = 0; i < s.path.length; i++) {
      current_type = get_field_type(current_type, s.path.data[i].lexeme, table);
      if (current_type.length == 0) return sv_from_cstr("");
    }
    // Preserve array identity for terminal fields; intermediate path walking
    // still requires scalar aggregate fields.
    string_view array_element = try_get_field_array_type(
        current_type, s.expr->data.identifier.id.lexeme, table);
    if (array_element.length > 0)
      return make_array_type_sv(array_element);
    return get_field_type(current_type, s.expr->data.identifier.id.lexeme,
                          table);
  }

  if (expr->tag == arr_index) {
    ast_arr_index ai = expr->data.arr_index;
    // Direct arr[idx] inherits the array element type.
    if (!ai.has_field) {
      token_t tok = token_for_expr(ai.array);
      return get_array_element_type(ai.array, table, tok);
    }
    token_t tok = token_for_expr(ai.array);
    string_view current_type = get_array_element_type(ai.array, table, tok);
    for (int i = 0; i < ai.field_path.length; i++) {
      current_type = get_field_type(current_type,
                                    ai.field_path.data[i].lexeme, table);
      if (current_type.length == 0) return sv_from_cstr("");
    }
    if (!ai.field_expr || ai.field_expr->tag != identifier)
      return sv_from_cstr("");
    string_view field_name = ai.field_expr->data.identifier.id.lexeme;
    string_view array_element = try_get_field_array_type(
        current_type, field_name, table);
    if (array_element.length > 0) return make_array_type_sv(array_element);
    return get_field_type(current_type, field_name, table);
  }

  if (expr->tag == op) {
    ast_op operation = expr->data.op;
    if (comparison_operator(operation.op)) return sv_from_cstr("boolean");
    return arithmetic_result_type(infer_expr_type(operation.left, table),
                                  infer_expr_type(operation.right, table));
  }

  if (expr->tag == unary_op) {
    string_view operand = infer_expr_type(expr->data.unary_op.operand, table);
    if (type_is(operand, "byte") || type_is(operand, "char") ||
        type_is(operand, "boolean") || type_is(operand, "bool"))
      return sv_from_cstr("int");
    return operand;
  }

  return sv_from_cstr("");
}

// Helper: Determine if an expression returns a string type
// Returns 1 if expression type is "string", 0 otherwise
int expr_returns_string(ast_t expr, name_table_t table) {
  return svcmp(infer_expr_type(expr, table), SV_STRING) == 0;
}

// Helper: Convert a Rift string expression to C string (const char*)
// Rift strings are null-terminated, so we extract the .data field
void generate_string_to_cstr(generator_t *g, ast_t expr) {
  generate_expression(g, expr);
  fprintf(g->f, ".data");
}

// Helper: Get element type for struct field access (::)
// Handles known type/field combinations
// Returns element type on success, or empty string if unknown
string_view try_get_field_array_type(string_view base_type,
                                      string_view field_name,
                                      name_table_t table) {
  // Special case: tdef_ast::constructors is ast[]
  if (svcmp(base_type, sv_from_cstr("tdef_ast")) == 0) {
    if (svcmp(field_name, sv_from_cstr("constructors")) == 0) {
      return sv_from_cstr("ast");
    }
  }

  // Special case: type_spec::constructors is constructor_spec[]
  if (svcmp(base_type, sv_from_cstr("type_spec")) == 0) {
    if (svcmp(field_name, sv_from_cstr("constructors")) == 0) {
      return sv_from_cstr("constructor_spec");
    }
  }

  // Special case: ast_array_t and similar types
  if (svcmp(base_type, sv_from_cstr("ast_array_t")) == 0 ||
      svcmp(base_type, sv_from_cstr("ast")) == 0 ||
      svcmp(base_type, sv_from_cstr("ast []")) == 0) {
    return sv_from_cstr("ast");
  }

  // Special case: token_array_t fields
  if (svcmp(base_type, sv_from_cstr("token_array_t")) == 0 ||
      svcmp(base_type, sv_from_cstr("token []")) == 0) {
    return sv_from_cstr("token");
  }

  // Try to look up user-defined struct types
  ast_t tdef_ref = get_ref_by_kind(base_type, NT_USER_TYPE, table);
  if (tdef_ref && tdef_ref->tag == tdef) {
    ast_tdef tdef = tdef_ref->data.tdef;
    if (tdef.t == TDEF_MODULE) {
      for (int i = 0; i < tdef.module_fields.length; i++) {
        ast_vardef field = tdef.module_fields.data[i]->data.vardef;
        if (svcmp(field.name.lexeme, field_name) == 0) {
          return field.type->data.type.is_array ?
              field.type->data.type.name.lexeme : sv_from_cstr("");
        }
      }
      return sv_from_cstr("");
    }
    // Search constructors (fields) for matching field name
    for (int i = 0; i < tdef.constructors.length; i++) {
      ast_t cons_ast = tdef.constructors.data[i];
      if (cons_ast->tag == cons) {
        ast_cons cons = cons_ast->data.cons;
        if (svcmp(cons.name.lexeme, field_name) == 0) {
          // Found the field - check if its type is an array
          if (cons.type && cons.type->tag == type) {
            ast_type field_type = cons.type->data.type;
            if (field_type.is_array) {
              // Return the element type
              return field_type.name.lexeme;
            }
          }
          // Field exists but is not an array
          return sv_from_cstr("");
        }
      }
    }
  }

  // If field ends with 's' or contains 'array', might be array
  // For now, default to empty string for unknown cases
  return sv_from_cstr("");
}

// Helper: Determine element type of an array expression
// Handles identifiers, field access (::), and known function calls (like get_args)
// call_token: fallback token for error reporting (e.g., the function call token)
// Returns type name on success, exits on error
string_view get_array_element_type(ast_t arr, name_table_t table,
                                    token_t call_token) {
  if (arr->tag == identifier) {
    token_t tok = arr->data.identifier.id;
    string_view name = tok.lexeme;
    return get_array_var_type(name, table, tok);
  } else if (arr->tag == sub) {
    // Handle field access: receiver::field or receiver.field
    ast_sub sub = arr->data.sub;

    string_view current_type = sv_from_cstr("");
    string_view field_name = sv_from_cstr("");

    if (sub.receiver) {
      current_type = infer_expr_type(sub.receiver, table);
    }

    for (int i = 0; i < sub.path.length && current_type.length > 0; i++) {
      current_type = get_field_type(current_type, sub.path.data[i].lexeme, table);
    }

    // Get the field name from expr (should be an identifier)
    if (sub.expr && sub.expr->tag == identifier) {
      field_name = sub.expr->data.identifier.id.lexeme;
    }

    // Look up the field type if we have both receiver type and field name
    if (current_type.length > 0 && field_name.length > 0) {
      string_view field_type = try_get_field_array_type(current_type, field_name, table);

      // If we found a known field type, return it
      if (field_type.length > 0) {
        return field_type;
      }
    }

    // If we couldn't determine the type, error out
    error(call_token.filename, call_token.line, call_token.col,
          "Cannot infer array type from field access expression; array element type must be determinable from an identifier or known function call");
    return sv_from_cstr("");
  } else if (arr->tag == funcall) {
    // For function calls, try to infer type from function name
    token_t tok = arr->data.funcall.name;
    string_view func_name = tok.lexeme;
    if (svcmp(func_name, sv_from_cstr("get_args")) == 0) {
      return SV_STRING;
    } else {
      error(tok.filename, tok.line, tok.col, "Cannot infer array type from function call: " SV_Fmt,
            SV_Arg(func_name));
      return sv_from_cstr("");
    }
  } else {
    // Try to extract location info from the expression itself, fallback to call_token
    char *filename = call_token.filename;
    int line = call_token.line;
    int col = call_token.col;

    // Try to get more precise info from various expression types
    if (arr->tag == literal) {
      filename = arr->data.literal.lit.filename;
      line = arr->data.literal.lit.line;
      col = arr->data.literal.lit.col;
    } else if (arr->tag == op) {
      // For operators, use left operand's location
      if (arr->data.op.left && arr->data.op.left->tag == literal) {
        filename = arr->data.op.left->data.literal.lit.filename;
        line = arr->data.op.left->data.literal.lit.line;
        col = arr->data.op.left->data.literal.lit.col;
      }
    }

    error(filename, line, col,
          "Array argument must be an identifier, field access (::), or get_args() call; got unsupported expression type");
    return sv_from_cstr("");
  }
}
