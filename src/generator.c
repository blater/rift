//-----------------------------------------------------------------------------
//  ROCKER GENERATOR
//  MIT License
//  Copyright (c) 2024 Paul Passeron
//-----------------------------------------------------------------------------

#include "generator.h"
#include "lib/alloc.h"
#include "ast.h"
#include "error.h"
#include "name_table.h"
#include "stringview.h"
#include "token.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void generate_statement(generator_t *g, ast_t stmt);
void generate_compound(generator_t *g, ast_t comp);
void generate_tdef(generator_t *g, ast_t tdef_ast);
void generate_fundef(generator_t *g, ast_t fun);
int is_builtin_typename(char *name);
void generate_sub_as_expression(generator_t *g, ast_t expr);
void generate_method_call(generator_t *g, ast_t node);
string_view infer_expr_type(ast_t expr, name_table_t table);
void generate_assignement(generator_t *g, ast_t assignment);
void generate_iter_loop(generator_t *g, ast_t loop);
void generate_embed(generator_t *g, ast_t node);
void write_allocator_code(FILE *f);
void generate_expression(generator_t *g, ast_t expr);
const char *allocate_string_tmp(generator_t *g);
string_view get_var_type(string_view name, name_table_t table);
string_view get_array_element_type(ast_t arr, name_table_t table, token_t call_token);
string_view try_get_field_array_type(string_view base_type, string_view field_name,
                                     name_table_t table);
int get_literal_string_length(token_t tok);
static int is_heap_allocated_type(string_view type_name, name_table_t table);
static char *capture_expression(generator_t *g, ast_t expr);
static int expr_is_array(ast_t expr, name_table_t table, int *is_string);

// Helper: Flush accumulated pre-statements to destination and reset pre_f
static void flush_pre_f(generator_t *g, FILE *dest);

// Constant to avoid repeated SV_STRING + strlen
static const string_view SV_STRING = {.data = "string", .length = 6};

// Field names emitted into the C struct that backs every Rock `union`.
// The discriminator is `key` (an enum); the payload is `value` (a C union).
// Centralised so a future rename touches one place.
#define UNION_KEY_FIELD   "key"
#define UNION_VALUE_FIELD "value"

// --- Unified scope tracking (linked lists, zero pre-allocation) ---

static void push_scope(generator_t *g, int bump_mark_id) {
  scope_t *s = malloc(sizeof(scope_t));
  s->prev = g->scope;
  s->vars = NULL;
  s->bump_mark_id = bump_mark_id;
  g->scope = s;
}

static void track_var(generator_t *g, string_view name, track_kind_t kind,
                      int is_string_array, int owns_name, string_view type_name) {
  if (!g->scope) return;
  tracked_var_t *v = malloc(sizeof(tracked_var_t));
  v->name = name;
  v->type_name = type_name;
  v->kind = kind;
  v->is_string_array = is_string_array;
  v->owns_name = owns_name;
  v->next = g->scope->vars;
  g->scope->vars = v;
}

static void emit_scope_cleanup(generator_t *g) {
  if (!g->scope) return;
  FILE *f = g->f;
  for (tracked_var_t *v = g->scope->vars; v; v = v->next) {
    switch (v->kind) {
    case TRACK_STRING:
      fprintf(f, "__string_release(" SV_Fmt ");\n", SV_Arg(v->name));
      break;
    case TRACK_ARRAY:
      fprintf(f, "__internal_free_array(" SV_Fmt ", %d);\n",
              SV_Arg(v->name), v->is_string_array);
      break;
    case TRACK_HANDLE:
      fprintf(f, "__rock_release_" SV_Fmt "(" SV_Fmt ");\n",
              SV_Arg(v->type_name), SV_Arg(v->name));
      break;
    }
  }
}

static void pop_scope(generator_t *g) {
  if (!g->scope) return;
  scope_t *top = g->scope;
  g->scope = top->prev;
  tracked_var_t *v = top->vars;
  while (v) {
    tracked_var_t *next = v->next;
    if (v->owns_name) free((char *)v->name.data);
    free(v);
    v = next;
  }
  free(top);
}

// Emit cleanup for ALL enclosing scopes (used before return).
// Releases strings and
// records (refcount-aware __handle_release). Arrays still skipped — they
// may be aliased or returned and have no retain/release wired yet.
// `skip_name` suppresses release of one variable, which the caller uses
// when the return value is already captured into a typed temp by name.
static void emit_return_cleanup(generator_t *g, string_view skip_name) {
  FILE *f = g->f;
  for (scope_t *s = g->scope; s; s = s->prev) {
    for (tracked_var_t *v = s->vars; v; v = v->next) {
      if (skip_name.length > 0 && svcmp(v->name, skip_name) == 0) continue;
      switch (v->kind) {
      case TRACK_STRING:
        fprintf(f, "__string_release(" SV_Fmt ");\n", SV_Arg(v->name));
        break;
      case TRACK_HANDLE:
        fprintf(f, "__rock_release_" SV_Fmt "(" SV_Fmt ");\n",
                SV_Arg(v->type_name), SV_Arg(v->name));
        break;
      case TRACK_ARRAY:
        fprintf(f, "__internal_free_array(" SV_Fmt ", %d);\n",
                SV_Arg(v->name), v->is_string_array);
        break;
      }
    }
  }
  for (scope_t *s = g->scope; s; s = s->prev) {
    if (s->bump_mark_id >= 0) {
      fprintf(f, "rock_bump_restore(__bm_%d);\n", s->bump_mark_id);
    }
  }
}

// --- Convenience wrappers (keep call sites readable) ---

static void track_string_var(generator_t *g, string_view name) {
  track_var(g, name, TRACK_STRING, 0, 0, sv_from_cstr(""));
}

static void track_string_tmp(generator_t *g, const char *tmp_name) {
  string_view sv = {.data = (char *)tmp_name, .length = strlen(tmp_name)};
  track_var(g, sv, TRACK_STRING, 0, 1, sv_from_cstr(""));
}

static void track_array_var(generator_t *g, string_view name, int is_string_array) {
  track_var(g, name, TRACK_ARRAY, is_string_array, 0, sv_from_cstr(""));
}

static void track_handle_var(generator_t *g, string_view name, string_view type_name) {
  track_var(g, name, TRACK_HANDLE, 0, 0, type_name);
}

// Helper: nullify a string temp after ownership transfer (prevents double-free)
static void emit_nullify_tmp(FILE *f, const char *tmp_name) {
  fprintf(f,
          "%s.data = NULL; %s.length = 0; %s.capacity = 0; "
          "%s.backing = NULL;\n",
          tmp_name, tmp_name, tmp_name, tmp_name);
}

/* Emit release of a statically-known aggregate field. The final aggregate
 * release walks these fields before returning the outer block to the pool. */
static void emit_owned_field_release(generator_t *g, ast_t type_node,
                                     const char *owner, string_view field_name) {
  if (!type_node || type_node->tag != type) return;
  FILE *f = g->f;
  ast_type field_type = type_node->data.type;
  if (field_type.is_array) {
    fprintf(f, "__internal_free_array(%s->" SV_Fmt ", %d);\n", owner,
            SV_Arg(field_name), svcmp(field_type.name.lexeme, SV_STRING) == 0);
  } else if (svcmp(field_type.name.lexeme, SV_STRING) == 0) {
    fprintf(f, "__string_release(%s->" SV_Fmt ");\n", owner, SV_Arg(field_name));
  } else if (is_heap_allocated_type(field_type.name.lexeme, g->table)) {
    fprintf(f, "__rock_release_" SV_Fmt "(%s->" SV_Fmt ");\n",
            SV_Arg(field_type.name.lexeme), owner, SV_Arg(field_name));
  }
}

static void emit_type_release_walker(generator_t *g, string_view name,
                                     ast_tdef tdef, int is_module) {
  FILE *f = g->f;
  fprintf(f, "static void __rock_release_" SV_Fmt "(void *__raw) {\n", SV_Arg(name));
  fprintf(f, "  " SV_Fmt " __rock_value = (" SV_Fmt ")__raw;\n", SV_Arg(name), SV_Arg(name));
  fprintf(f, "  if (!__rock_value) return;\n");
  fprintf(f, "  rock_block_header *header = ((rock_block_header *)__rock_value) - 1;\n");
  fprintf(f, "  if (header->refcount == ROCK_RC_STATIC) return;\n");
  fprintf(f, "  if (header->refcount == ROCK_RC_FREE || header->refcount == ROCK_RC_MAGAZINE) { fprintf(stderr, \"rock: aggregate release on already-freed block\\n\"); exit(1); }\n");
  fprintf(f, "  if (--header->refcount != 0) return;\n");

  if (is_module) {
    for (int i = 0; i < tdef.module_fields.length; i++) {
      ast_vardef field = tdef.module_fields.data[i]->data.vardef;
      emit_owned_field_release(g, field.type, "__rock_value", field.name.lexeme);
    }
  } else if (tdef.t == TDEF_PRO) {
    fprintf(f, "  switch (__rock_value->" UNION_KEY_FIELD ") {\n");
    for (int i = 0; i < tdef.constructors.length; i++) {
      ast_cons constructor = tdef.constructors.data[i]->data.cons;
      fprintf(f, "  case " SV_Fmt ":\n", SV_Arg(constructor.name.lexeme));
      if (constructor.type && constructor.type->tag == type) {
        ast_type payload = constructor.type->data.type;
        if (payload.is_array) {
          fprintf(f, "    __internal_free_array(__rock_value->" UNION_VALUE_FIELD "." SV_Fmt ", %d);\n",
                  SV_Arg(constructor.name.lexeme),
                  svcmp(payload.name.lexeme, SV_STRING) == 0);
        } else if (svcmp(payload.name.lexeme, SV_STRING) == 0) {
          fprintf(f, "    __string_release(__rock_value->" UNION_VALUE_FIELD "." SV_Fmt ");\n",
                  SV_Arg(constructor.name.lexeme));
        } else if (is_heap_allocated_type(payload.name.lexeme, g->table)) {
          fprintf(f, "    __rock_release_" SV_Fmt "(__rock_value->" UNION_VALUE_FIELD "." SV_Fmt ");\n",
                  SV_Arg(payload.name.lexeme), SV_Arg(constructor.name.lexeme));
        }
      }
      fprintf(f, "    break;\n");
    }
    fprintf(f, "  }\n");
  } else {
    for (int i = 0; i < tdef.constructors.length; i++) {
      ast_cons field = tdef.constructors.data[i]->data.cons;
      emit_owned_field_release(g, field.type, "__rock_value", field.name.lexeme);
    }
  }
  fprintf(f, "  rock_longlived_free(__rock_value);\n}\n");
}

static token_t token_for_expr(ast_t expr) {
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

// Helper: Capture expression into a temporary buffer and return it
// Caller must free() the returned buffer
static char* capture_expression(generator_t *g, ast_t expr) {
  char *buf = NULL;
  size_t size = 0;
  FILE *f = open_memstream(&buf, &size);
  FILE *saved_f = g->f;
  g->f = f;
  generate_expression(g, expr);
  fflush(f);
  fclose(f);
  g->f = saved_f;
  return buf;
}

// Helper: Create a synthetic AST type node for registering builtin signatures
// Build a mangled method name: "TypeName_methodName" or "TypeName_array_methodName".
// Returns a null-terminated string allocated in the compiler persistent arena.
static char *mangle_method(string_view type_name, string_view method_name, int is_array_method) {
  size_t tl = type_name.length;
  size_t ml = method_name.length;
  char *buf;
  if (is_array_method) {
    buf = allocate_compiler_persistent(tl + 7 + ml + 1);
    memcpy(buf, type_name.data, tl);
    memcpy(buf + tl, "_array_", 7);
    memcpy(buf + tl + 7, method_name.data, ml);
    buf[tl + 7 + ml] = '\0';
  } else {
    buf = allocate_compiler_persistent(tl + 1 + ml + 1);
    memcpy(buf, type_name.data, tl);
    buf[tl] = '_';
    memcpy(buf + tl + 1, method_name.data, ml);
    buf[tl + 1 + ml] = '\0';
  }
  return buf;
}

static ast_t make_type_node(const char *type_name) {
  node_t n = {0};
  n.tag = type;
  n.data.type.name.lexeme = sv_from_cstr((char*)type_name);
  return new_ast(n);
}

// Helper: Register a builtin function in the name table with its return type
static void register_builtin(name_table_t *table, const char *name,
                              const char *ret_type) {
  node_t fn = {0};
  fn.tag = fundef;
  fn.data.fundef.name.lexeme = sv_from_cstr((char*)name);
  fn.data.fundef.ret_type = make_type_node(ret_type);
  push_nt(table, sv_from_cstr((char*)name), NT_FUN, new_ast(fn));
}

// Variadic variant: also records parameter types so --auto-cast can wrap
// numeric args in `(byte)`/`(word)`/`(dword)` for matching callee params.
// Caller passes type names as `const char *` after `n_params`.
static void register_builtin_typed(name_table_t *table, const char *name,
                                    const char *ret_type, int n_params, ...) {
  node_t fn = {0};
  fn.tag = fundef;
  fn.data.fundef.name.lexeme = sv_from_cstr((char*)name);
  fn.data.fundef.ret_type = make_type_node(ret_type);
  if (n_params > 0) {
    ast_array_t types = new_ast_array();
    va_list ap;
    va_start(ap, n_params);
    for (int i = 0; i < n_params; i++) {
      const char *pt = va_arg(ap, const char *);
      push_ast_array(&types, make_type_node(pt));
    }
    va_end(ap);
    fn.data.fundef.types = types;
  }
  push_nt(table, sv_from_cstr((char*)name), NT_FUN, new_ast(fn));
}

// Helper: Register a builtin that returns an array type (e.g. get_args → string[])
static void register_builtin_array(name_table_t *table, const char *name,
                                   const char *elem_type) {
  node_t rt = {0};
  rt.tag = type;
  rt.data.type.name.lexeme = sv_from_cstr((char*)elem_type);
  rt.data.type.is_array = 1;
  node_t fn = {0};
  fn.tag = fundef;
  fn.data.fundef.name.lexeme = sv_from_cstr((char*)name);
  fn.data.fundef.ret_type = new_ast(rt);
  push_nt(table, sv_from_cstr((char*)name), NT_FUN, new_ast(fn));
}

// Count top-level user fundefs (non-methods) sharing a name. Used to decide
// whether to mangle a name for arity-based overload dispatch. Only user fundefs
// are counted — builtins are not overloaded in Phase 1.
static int program_user_fundef_count(ast_t program, string_view name) {
  if (program == NULL) return 0;
  int n = 0;
  ast_array_t stmts = program->data.program.prog;
  for (int i = 0; i < stmts.length; i++) {
    ast_t s = stmts.data[i];
    if (s->tag == fundef && !s->data.fundef.is_method &&
        svcmp(s->data.fundef.name.lexeme, name) == 0)
      n++;
  }
  return n;
}

// Emit a function name for both declaration and call sites. When a user
// fundef name is overloaded (>=2 top-level user fundefs share the name),
// the name is mangled as `name__N` where N is the arity. Single-definition
// names emit unmangled for backward compatibility and diff minimisation.
static void emit_fun_name(FILE *f, generator_t *g, string_view name, int argc) {
  if (program_user_fundef_count(g->program, name) > 1)
    fprintf(f, SV_Fmt "__%d", SV_Arg(name), argc);
  else
    fprintf(f, SV_Fmt, SV_Arg(name));
}

// Helper: Check if a function is a known operation that's always available
// These functions emit code patterns we recognize regardless of name table
// Unified string concat: capture arguments, emit setup to pre_f, emit tmp var
void emit_concat(generator_t *g, ast_funcall call) {
  ast_t arg = call.args.data[1];
  int is_char = 0;

  if (arg->tag == literal && arg->data.literal.lit.type == TOK_CHR_LIT) {
    is_char = 1;
  } else if (arg->tag == identifier) {
    string_view type = get_var_type(arg->data.identifier.id.lexeme, g->table);
    if (svcmp(type, sv_from_cstr("char")) == 0) is_char = 1;
  } else if (arg->tag == arr_index) {
    string_view elem_type = get_array_element_type(arg->data.arr_index.array, g->table, call.name);
    if (svcmp(elem_type, sv_from_cstr("char")) == 0) is_char = 1;
  } else if (arg->tag == funcall) {
    string_view fname = arg->data.funcall.name.lexeme;
    if ((svcmp(fname, sv_from_cstr("get")) == 0 || svcmp(fname, sv_from_cstr("pop")) == 0) &&
        arg->data.funcall.args.length > 0) {
      string_view elem_type = get_array_element_type(arg->data.funcall.args.data[0], g->table, call.name);
      if (svcmp(elem_type, sv_from_cstr("char")) == 0) is_char = 1;
    }
  }

  const char *func_name = is_char ? "__concat_char" : "__concat_str";
  const char *tmp_var = allocate_string_tmp(g);

  char *arg0_buf = capture_expression(g, call.args.data[0]);
  char *arg1_buf = capture_expression(g, call.args.data[1]);

  fprintf(g->pre_f, "string %s; %s(&%s, %s, %s);\n",
          tmp_var, func_name, tmp_var, arg0_buf, arg1_buf);
  fprintf(g->f, "%s", tmp_var);

  free(arg0_buf);
  free(arg1_buf);
  track_string_tmp(g, tmp_var);
}

void emit_to_string(generator_t *g, ast_funcall call) {
  // Capture argument, emit setup to pre_f, emit tmp var to main output
  ast_t arg = call.args.data[0];
  const char *fn = "__to_string_int";

  if (arg->tag == identifier) {
    string_view type = get_var_type(arg->data.identifier.id.lexeme, g->table);
    if (svcmp(type, sv_from_cstr("byte")) == 0) fn = "__to_string_byte";
    else if (svcmp(type, sv_from_cstr("word")) == 0) fn = "__to_string_word";
    else if (svcmp(type, sv_from_cstr("dword")) == 0) fn = "__to_string_dword";
    else if (svcmp(type, sv_from_cstr("float")) == 0) fn = "__to_string_float";
  }

  const char *tmp_var = allocate_string_tmp(g);
  char *arg_buf = capture_expression(g, arg);

  fprintf(g->pre_f, "string %s; %s(&%s, %s);\n", tmp_var, fn, tmp_var, arg_buf);
  fprintf(g->f, "%s", tmp_var);

  free(arg_buf);
  track_string_tmp(g, tmp_var);
}

void emit_substring(generator_t *g, ast_funcall call) {
  // Capture arguments, emit setup to pre_f, emit tmp var to main output
  const char *tmp_var = allocate_string_tmp(g);

  if (call.args.length == 2) {
    char *arg0_buf = capture_expression(g, call.args.data[0]);
    char *arg1_buf = capture_expression(g, call.args.data[1]);

    fprintf(g->pre_f, "string %s; __substring_from(&%s, %s, %s);\n",
            tmp_var, tmp_var, arg0_buf, arg1_buf);
    fprintf(g->f, "%s", tmp_var);

    free(arg0_buf);
    free(arg1_buf);
    track_string_tmp(g, tmp_var);
  } else if (call.args.length == 3) {
    char *arg0_buf = capture_expression(g, call.args.data[0]);
    char *arg1_buf = capture_expression(g, call.args.data[1]);
    char *arg2_buf = capture_expression(g, call.args.data[2]);

    fprintf(g->pre_f, "string %s; __substring_range(&%s, %s, %s, %s);\n",
            tmp_var, tmp_var, arg0_buf, arg1_buf, arg2_buf);
    fprintf(g->f, "%s", tmp_var);

    free(arg0_buf);
    free(arg1_buf);
    free(arg2_buf);
    track_string_tmp(g, tmp_var);
  } else {
    token_t tok = call.name;
    error(tok.filename, tok.line, tok.col, "substring() requires 2 or 3 arguments, got %d",
          call.args.length);
  }
}

void emit_string_literal(generator_t *g, const char *tmp_var, token_t tok) {
  /* Emit a real rock_block_header, rather than duplicating its layout. The
   * allocator header differs between host and ZXN builds, so hard-coding the
   * first fields here would make literal strings unsafe on one target. */
  int byte_len = get_literal_string_length(tok) - 1;
  int id = g->lit_counter++;
  fprintf(g->pre_f,
          "static struct { rock_block_header header; char data[%d]; } "
          "__rock_lit_%d = { .header = { .size = %d, .refcount = ROCK_RC_STATIC, "
          ".next_free = 0 }, .data = " SV_Fmt " };\n",
          byte_len + 1, id, byte_len, SV_Arg(tok.lexeme));
  fprintf(g->pre_f,
          "string %s; __rock_make_string(&%s, __rock_lit_%d.data, %d); "
          "%s.backing = &__rock_lit_%d.header;\n",
          tmp_var, tmp_var, id, byte_len, tmp_var, id);
  fprintf(g->f, "%s", tmp_var);
}

// Helper: Flush accumulated pre-statements to destination and reset pre_f
// (used in Step 3 for ZXN statement splitting)
__attribute__((unused))
static void flush_pre_f(generator_t *g, FILE *dest) {
  if (g->pre_f == NULL)
    return;
  fflush(g->pre_f);
  if (g->pre_buf && g->pre_buf_size > 0)
    fprintf(dest, "%s", g->pre_buf);
  // Reset pre_f for next batch of statements
  fclose(g->pre_f);
  free(g->pre_buf);
  g->pre_buf = NULL;
  g->pre_buf_size = 0;
  g->pre_f = open_memstream(&g->pre_buf, &g->pre_buf_size);
}

// Helper: Append a string to the deferred global init list, growing as needed.
static void push_deferred_global_init(generator_t *g, char *code) {
  if (g->deferred_global_init_count >= g->deferred_global_init_capacity) {
    g->deferred_global_init_capacity = g->deferred_global_init_capacity == 0 ? 8 : g->deferred_global_init_capacity * 2;
    g->deferred_global_init_code = realloc(g->deferred_global_init_code,
                                           g->deferred_global_init_capacity * sizeof(char *));
  }
  g->deferred_global_init_code[g->deferred_global_init_count++] = code;
}

// Helper: Save deferred init code (pre_f setup + assignment) for emitting in main()
static void defer_global_init(generator_t *g, string_view var_name, const char *expr_text) {
  // Build: pre_f_content + "var = expr;\n"
  char *code = NULL;
  size_t size = 0;
  FILE *out = open_memstream(&code, &size);
  fflush(g->pre_f);
  if (g->pre_buf && g->pre_buf_size > 0)
    fprintf(out, "%s", g->pre_buf);
  fprintf(out, SV_Fmt " = %s;\n", SV_Arg(var_name), expr_text);
  fflush(out);
  fclose(out);
  // Reset pre_f
  fclose(g->pre_f);
  free(g->pre_buf);
  g->pre_buf = NULL;
  g->pre_buf_size = 0;
  g->pre_f = open_memstream(&g->pre_buf, &g->pre_buf_size);
  push_deferred_global_init(g, code);
}

generator_t new_generator(char *filename) {
  generator_t res;
  res.f = fopen(filename, "wb");
  if (res.f == NULL)
    perror("Could not open file !");
  res.table = new_name_table();
  res.str_tmp_counter = 0;
  res.target = TARGET_HOST;
  res.current_module_type = sv_from_cstr("");
  res.in_global_scope = 1;
  res.deferred_module_inits = new_ast_array();
  res.deferred_global_init_code = NULL;
  res.deferred_global_init_count = 0;
  res.deferred_global_init_capacity = 0;
  res.program = NULL;
  res.scope = NULL;
  res.auto_cast = 0;
  res.zxn_test = 0;
  res.zxn_memory_profile = 0;
  res.lit_counter = 0;
  res.current_fundef = NULL;
  res.bump_mark_counter = 0;

  // Register C library builtin functions with their return types
  // Stdlib / I/O
  register_builtin(&res.table, "print",                "void");
  register_builtin(&res.table, "printf",               "void");
  // Array operations - handled specially by name lookup in generator
  register_builtin(&res.table, "length",               "int");
  // String operations from fundefs.h
  register_builtin(&res.table, "charAt",               "char");
  register_builtin(&res.table, "setCharAt",            "void");
  register_builtin(&res.table, "equals",               "int");
  register_builtin(&res.table, "string_to_cstr",       "void");
  register_builtin(&res.table, "cstr_to_string",       "string");
  register_builtin(&res.table, "new_string",           "string");
  // File operations
  register_builtin(&res.table, "read_file",            "string");
  register_builtin(&res.table, "write_string_to_file", "void");
  register_builtin(&res.table, "get_abs_path",         "string");
  // Numeric conversions
  register_builtin(&res.table, "to_int",               "int");
  register_builtin(&res.table, "to_byte",              "byte");
  register_builtin(&res.table, "to_word",              "word");
  register_builtin(&res.table, "to_dword",             "dword");
  register_builtin(&res.table, "to_float",             "float");
  register_builtin(&res.table, "set_string_index_base","void");
  // Built-in array/string functions with special code generation
  register_builtin(&res.table, "substring",            "string");
  register_builtin(&res.table, "concat",               "string");
  register_builtin(&res.table, "toString",             "string");
  // Command-line argument access
  register_builtin_array(&res.table, "get_args",       "string");
  // Core compiler functions - always available
  register_builtin(&res.table, "exit",                 "void");
  register_builtin(&res.table, "halt",                 "void");
  register_builtin(&res.table, "putchar",              "void");
  register_builtin(&res.table, "fill_cmd_args",        "void");
  register_builtin(&res.table, "zxn_test_begin",       "void");
  register_builtin(&res.table, "zxn_test_stage",       "void");
  register_builtin(&res.table, "zxn_test_pass",        "void");
  register_builtin(&res.table, "zxn_test_fail",        "void");
  register_builtin_typed(&res.table, "zxn_test_assert_pass", "void", 1, "string");
  register_builtin_typed(&res.table, "zxn_test_assert_fail", "void", 3,
                         "string", "string", "string");
  register_builtin(&res.table, "zxn_test_finish",      "void");
  // Memory operations
  register_builtin(&res.table, "poke",                 "void");
  register_builtin(&res.table, "peek",                 "byte");

  register_builtin(&res.table, "scan_keyboard",        "void");
  register_builtin_typed(&res.table, "key_pressed",    "byte", 1, "byte");

  register_builtin_typed(&res.table, "border",         "void", 1, "byte");
  register_builtin(&res.table, "border_get",           "byte");

  register_builtin_typed(&res.table, "ink",            "void", 1, "byte");
  register_builtin_typed(&res.table, "paper",          "void", 1, "byte");
  register_builtin_typed(&res.table, "bright",         "void", 1, "byte");
  register_builtin_typed(&res.table, "flash",          "void", 1, "byte");
  register_builtin_typed(&res.table, "inverse",        "void", 1, "byte");
  register_builtin_typed(&res.table, "over",           "void", 1, "byte");
  register_builtin(&res.table, "graphics_on",          "void");
  register_builtin(&res.table, "graphics_off",         "void");

  register_builtin(&res.table, "cls",                  "void");

  register_builtin_typed(&res.table, "plot",           "void", 2, "byte", "byte");
  register_builtin_typed(&res.table, "point",          "byte", 2, "byte", "byte");
  register_builtin_typed(&res.table, "draw",           "void", 4, "byte", "byte", "byte", "byte");
  register_builtin(&res.table, "polyline",             "void");
  register_builtin_typed(&res.table, "circle",         "void", 3, "byte", "byte", "byte");
  register_builtin_typed(&res.table, "fill",           "void", 4, "byte", "byte", "byte", "byte");
  register_builtin_typed(&res.table, "triangle",       "void", 6, "byte", "byte", "byte", "byte", "byte", "byte");
  register_builtin_typed(&res.table, "trianglefill",   "void", 6, "byte", "byte", "byte", "byte", "byte", "byte");

  register_builtin(&res.table, "randomize",            "void");
  register_builtin_typed(&res.table, "random_byte",    "byte", 1, "byte");
  register_builtin_typed(&res.table, "random_word",    "word", 1, "word");

  register_builtin_typed(&res.table, "next_reg_set",   "void", 2, "byte", "byte");
  register_builtin_typed(&res.table, "next_reg_get",   "byte", 1, "byte");
  register_builtin_typed(&res.table, "cpu_speed_set",  "void", 1, "byte");
  register_builtin(&res.table, "cpu_speed_get",        "byte");
  register_builtin_typed(&res.table, "mmu_set",        "void", 2, "byte", "byte");

  register_builtin(&res.table, "odd",                  "byte");
  register_builtin(&res.table, "even",                 "byte");
  register_builtin(&res.table, "hi",                   "byte");
  register_builtin(&res.table, "lo",                   "byte");
  register_builtin(&res.table, "swap",                 "word");
  register_builtin(&res.table, "upcase",               "char");
  register_builtin(&res.table, "locase",               "char");
  register_builtin(&res.table, "abs_int",              "int");
  register_builtin(&res.table, "abs_word",             "word");

  register_builtin(&res.table, "fsin",                 "float");
  register_builtin(&res.table, "fcos",                 "float");
  register_builtin(&res.table, "fsqrt",                "float");
  register_builtin(&res.table, "fabs_float",           "float");
  register_builtin(&res.table, "fpi",                  "float");

  register_builtin(&res.table, "sleep",                "void");
  register_builtin(&res.table, "beep",                 "void");
  register_builtin(&res.table, "inkey",                "byte");
  register_builtin(&res.table, "keypress",             "byte");
  /* print(x, y, text) — 3-arg overload of `print`, routed to C print_at
   * via a fast-path branch in generate_funcall. Not registered as a
   * separate Rock name; the C symbol lives in src/lib/print_at.c. */

  // Always initialize pre_f buffer for statement splitting
  res.pre_buf = NULL;
  res.pre_buf_size = 0;
  res.pre_f = open_memstream(&res.pre_buf, &res.pre_buf_size);
  if (res.pre_f == NULL)
    perror("Could not open memstream for pre_f!");

  return res;
}

void kill_generator(generator_t g) {
  // Free any remaining scope nodes (handles error-exit paths)
  while (g.scope) pop_scope(&g);
  fclose(g.f);
  // Clean up pre_f buffer if initialized (ZXN target only)
  if (g.pre_f != NULL) {
    fflush(g.pre_f);
    fclose(g.pre_f);
  }
}

name_table_t new_name_table(void) {
  name_table_t res;
  res.length = 0;
  res.capacity = INIT_NT_CAP;
  res.scope = 0;
  res.refs = new_ast_array();
  res.kinds = allocate_compiler_persistent(sizeof(nt_kind) * res.capacity);
  res.names = allocate_compiler_persistent(sizeof(char *) * res.capacity);
  res.scopes = allocate_compiler_persistent(sizeof(int) * res.capacity);
  return res;
}

void generate_type(FILE *f, ast_t a) {
  ast_type type = a->data.type;
  if (type.is_array)
    fprintf(f, "__internal_dynamic_array_t");
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

// Returns 1 if the named variable is a scalar (non-array) string type
static int is_scalar_string_var(string_view name, name_table_t table) {
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
static int rhs_is_borrower(ast_t expr) {
  if (!expr) return 0;
  if (expr->tag == identifier || expr->tag == sub || expr->tag == arr_index)
    return 1;
  return expr->tag == funcall &&
      svcmp(expr->data.funcall.name.lexeme, sv_from_cstr("get")) == 0;
}

static int expr_is_array(ast_t expr, name_table_t table, int *is_string) {
  ast_type type = {0};
  ast_t ref = NULL;
  if (!expr) return 0;
  if (expr->tag == identifier) ref = get_ref(expr->data.identifier.id.lexeme, table);
  else if (expr->tag == funcall) ref = get_ref(expr->data.funcall.name.lexeme, table);
  else if (expr->tag == method_call) {
    ast_method_call call = expr->data.method_call;
    string_view receiver_type = infer_expr_type(call.receiver, table);
    if (receiver_type.length > 0) {
      ref = get_ref(sv_from_cstr(mangle_method(receiver_type, call.method.lexeme, 0)),
                    table);
    }
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
static void emit_borrowed_container_retain(generator_t *g, ast_t expr,
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
static int is_heap_allocated_type(string_view type_name, name_table_t table) {
  if (is_builtin_typename(string_of_sv(type_name))) return 0;
  ast_t ref = get_ref(type_name, table);
  return ref && ref->tag == tdef;
}

// Returns 1 if name resolves to a non-array vardef/parameter whose declared
// type is a record/union/module — i.e. an aggregate handle subject to
// __handle_retain / __handle_release. Mirrors is_scalar_string_var.
static int is_scalar_aggregate_var(string_view name, name_table_t table) {
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
static string_view make_array_type_sv(string_view base) {
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
  ast_t tdef_ref = get_ref(base_type, table);
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
static int is_sub_target_scalar_string(ast_t sub_expr, name_table_t table) {
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
  ast_t tdef_ref = get_ref(current_type, table);
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
static int is_sub_target_scalar_aggregate(ast_t sub_expr, name_table_t table) {
  if (!sub_expr || sub_expr->tag != sub) return 0;
  ast_sub s = sub_expr->data.sub;
  string_view current_type = infer_expr_type(s.receiver, table);
  if (current_type.length == 0) return 0;
  for (int i = 0; i < s.path.length; i++) {
    current_type = get_field_type(current_type, s.path.data[i].lexeme, table);
    if (current_type.length == 0) return 0;
  }
  string_view field_name = s.expr->data.identifier.id.lexeme;
  ast_t tdef_ref = get_ref(current_type, table);
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
static string_view sub_target_array_element_type(ast_t sub_expr, name_table_t table) {
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

// Helper: Infer the Rock type name of an arbitrary expression.
// Returns empty string_view when the type cannot be determined.
string_view infer_expr_type(ast_t expr, name_table_t table) {
  if (!expr) return sv_from_cstr("");

  if (expr->tag == identifier) {
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
    return sv_from_cstr("");
  }

  if (expr->tag == literal) {
    token_type_t t = expr->data.literal.lit.type;
    if (t == TOK_STR_LIT) return SV_STRING;
    if (t == TOK_NUM_LIT) return sv_from_cstr("int");
    if (t == TOK_CHR_LIT) return sv_from_cstr("char");
    return sv_from_cstr("");
  }

  if (expr->tag == funcall) {
    ast_funcall call = expr->data.funcall;
    // get(arr, idx) → element type of arr (same as get_var_type since Rock
    // stores element type directly in the vardef)
    if (svcmp(call.name.lexeme, sv_from_cstr("get")) == 0 &&
        call.args.length >= 1 && call.args.data[0]->tag == identifier) {
      return get_var_type(call.args.data[0]->data.identifier.id.lexeme, table);
    }
    // User-defined function → look up declared return type
    ast_t ref = get_ref(call.name.lexeme, table);
    if (ref && ref->tag == fundef && ref->data.fundef.ret_type) {
      ast_type rt = ref->data.fundef.ret_type->data.type;
      return rt.is_array ? make_array_type_sv(rt.name.lexeme) : rt.name.lexeme;
    }
    return sv_from_cstr("");
  }

  if (expr->tag == method_call) {
    // Recursive: infer receiver type → form mangled name → look up return type
    ast_method_call mc = expr->data.method_call;
    string_view recv_type = infer_expr_type(mc.receiver, table);
    if (recv_type.length == 0) return sv_from_cstr("");
    ast_t ref = get_ref(sv_from_cstr(mangle_method(recv_type, mc.method.lexeme, 0)), table);
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
    // Resolve the terminal field
    return get_field_type(current_type, s.expr->data.identifier.id.lexeme, table);
  }

  if (expr->tag == arr_index) {
    ast_arr_index ai = expr->data.arr_index;
    // Direct arr[idx] inherits the array element type.
    if (!ai.has_field) {
      token_t tok = token_for_expr(ai.array);
      return get_array_element_type(ai.array, table, tok);
    }
    return sv_from_cstr("");
  }

  return sv_from_cstr("");
}

// Helper: Determine if an expression returns a string type
// Returns 1 if expression type is "string", 0 otherwise
int expr_returns_string(ast_t expr, name_table_t table) {
  return svcmp(infer_expr_type(expr, table), SV_STRING) == 0;
}

// Helper: Convert a Rock string expression to C string (const char*)
// Rock strings are null-terminated, so we extract the .data field
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
  ast_t tdef_ref = get_ref(base_type, table);
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

void generate_expression(generator_t *g, ast_t expr);

void generate_subscript(generator_t *g, ast_t expr) {
  FILE *f = g->f;
  ast_arr_index sub = expr->data.arr_index;
  token_t call_token = token_for_expr(sub.array);
  string_view elem_type = get_array_element_type(sub.array, g->table, call_token);

  // For string arrays, use pre_f to emit setup statements
  if (svcmp(elem_type, SV_STRING) == 0) {
    const char *tmp = allocate_string_tmp(g);
    char *arr_buf = capture_expression(g, sub.array);
    char *idx_buf = capture_expression(g, sub.index);
    fprintf(g->pre_f, "string %s; string_get_elem(&%s, %s, (size_t)(%s));\n",
            tmp, tmp, arr_buf, idx_buf);
    fprintf(f, "%s", tmp);
    free(arr_buf);
    free(idx_buf);
    // Handle field access if present
    if (sub.has_field) {
      for (int i = 0; i < sub.field_path.length; i++)
        fprintf(f, "->" SV_Fmt, SV_Arg(sub.field_path.data[i].lexeme));
      fprintf(f, "->");
      generate_expression(g, sub.field_expr);
    }
    track_string_tmp(g, tmp);
  } else {
    // Non-string arrays: emit directly as before
    fprintf(f, SV_Fmt "_get_elem(", SV_Arg(elem_type));
    generate_expression(g, sub.array);
    fprintf(f, ", (size_t)(");
    generate_expression(g, sub.index);
    fprintf(f, "))");

    // Emit: ->field0->field1->...->field_expr
    if (sub.has_field) {
      for (int i = 0; i < sub.field_path.length; i++)
        fprintf(f, "->" SV_Fmt, SV_Arg(sub.field_path.data[i].lexeme));
      fprintf(f, "->");
      generate_expression(g, sub.field_expr);
    }
  }
}

// Metadata for array operations
typedef struct {
  int expected_args;
  const char *suffix;
  const char *name;
} array_op_t;

// Unified code generator for all array operations (append, get, set, pop)
void generate_array_op(generator_t *g, ast_funcall call, array_op_t op) {
  FILE *f = g->f;

  // Validate argument count
  if (call.args.length != op.expected_args) {
    error(call.name.filename, call.name.line, call.name.col,
          "%s() requires %d argument(s), got %d", op.name,
          op.expected_args, call.args.length);
    return;
  }

  // Determine element type (pass call.name as fallback for error reporting)
  string_view type_name =
      get_array_element_type(call.args.data[0], g->table, call.name);

  /* User-type array wrappers consume an owned element.  Retain just the
   * aliases which borrow an existing value; temporary producers transfer. */
  if (!is_builtin_typename(string_of_sv(type_name)) &&
      (strcmp(op.suffix, "_push_array") == 0 ||
       strcmp(op.suffix, "_set_elem") == 0 ||
       strcmp(op.suffix, "_insert") == 0)) {
    ast_type element_type = {0};
    element_type.name.lexeme = type_name;
    emit_borrowed_container_retain(g, call.args.data[op.expected_args - 1],
                                   element_type);
  }

  // For string arrays with get_elem or pop_array, use pre_f for setup statements
  if (svcmp(type_name, SV_STRING) == 0 &&
      (strcmp(op.suffix, "_get_elem") == 0 || strcmp(op.suffix, "_pop_array") == 0)) {
    const char *tmp = allocate_string_tmp(g);
    if (strcmp(op.suffix, "_get_elem") == 0) {
      // string_get_elem: 2 args (array, index)
      char *arr_buf = capture_expression(g, call.args.data[0]);
      char *idx_buf = capture_expression(g, call.args.data[1]);
      fprintf(g->pre_f, "string %s; string_get_elem(&%s, %s, (size_t)(%s));\n",
              tmp, tmp, arr_buf, idx_buf);
      fprintf(f, "%s", tmp);
      free(arr_buf);
      free(idx_buf);
    } else {
      // string_pop_array: 1 arg (array)
      char *arr_buf = capture_expression(g, call.args.data[0]);
      fprintf(g->pre_f, "string %s; string_pop_array(&%s, %s);\n",
              tmp, tmp, arr_buf);
      fprintf(f, "%s", tmp);
      free(arr_buf);
    }
    track_string_tmp(g, tmp);
  } else {
    // Non-string or void-returning operations: emit directly
    fprintf(f, SV_Fmt "%s(", SV_Arg(type_name), op.suffix);
    for (int i = 0; i < call.args.length; i++) {
      if (i > 0)
        fprintf(f, ", ");
      generate_expression(g, call.args.data[i]);
    }
    fprintf(f, ")");
  }
}

// Convenience wrappers for each array operation
void generate_append(generator_t *g, ast_funcall call) {
  array_op_t op = {2, "_push_array", "append"};
  generate_array_op(g, call, op);
}

void generate_get(generator_t *g, ast_funcall call) {
  array_op_t op = {2, "_get_elem", "get"};
  generate_array_op(g, call, op);
}

void generate_set(generator_t *g, ast_funcall call) {
  array_op_t op = {3, "_set_elem", "set"};
  generate_array_op(g, call, op);
}

void generate_pop(generator_t *g, ast_funcall call) {
  array_op_t op = {1, "_pop_array", "pop"};
  generate_array_op(g, call, op);
}

void generate_insert(generator_t *g, ast_funcall call) {
  array_op_t op = {3, "_insert", "insert"};
  generate_array_op(g, call, op);
}

// Helper: Allocate a temporary string variable and return its name
const char *allocate_string_tmp(generator_t *g) {
  char tmpname_buf[64];
  snprintf(tmpname_buf, sizeof(tmpname_buf), "__strtmp_%d", g->str_tmp_counter);
  g->str_tmp_counter++;
  return strdup(tmpname_buf);
}

void generate_substring(generator_t *g, ast_funcall call) {
  if (call.args.length != 2 && call.args.length != 3) {
    token_t tok = call.name;
    error(tok.filename, tok.line, tok.col,
          "substring() requires 2 or 3 arguments, got %d", call.args.length);
    return;
  }
  emit_substring(g, call);
}

void generate_concat(generator_t *g, ast_funcall call) {
  if (call.args.length != 2) {
    error(call.name.filename, call.name.line, call.name.col, "concat() requires 2 arguments, got %d",
          call.args.length);
  }
  emit_concat(g, call);
}

void generate_to_string(generator_t *g, ast_funcall call) {
  if (call.args.length != 1) {
    error(call.name.filename, call.name.line, call.name.col, "toString() requires 1 argument, got %d",
          call.args.length);
  }
  emit_to_string(g, call);
}

void generate_funcall(generator_t *g, ast_t fun) {
  FILE *f = g->f;
  ast_funcall funcall = fun->data.funcall;
  if (svcmp(funcall.name.lexeme, sv_from_cstr("append")) == 0) {
    generate_append(g, funcall);
  } else if (svcmp(funcall.name.lexeme, sv_from_cstr("get")) == 0) {
    generate_get(g, funcall);
  } else if (svcmp(funcall.name.lexeme, sv_from_cstr("set")) == 0) {
    generate_set(g, funcall);
  } else if (svcmp(funcall.name.lexeme, sv_from_cstr("pop")) == 0) {
    generate_pop(g, funcall);
  } else if (svcmp(funcall.name.lexeme, sv_from_cstr("insert")) == 0) {
    generate_insert(g, funcall);
  } else if (svcmp(funcall.name.lexeme, sv_from_cstr("substring")) == 0) {
    generate_substring(g, funcall);
  } else if (svcmp(funcall.name.lexeme, sv_from_cstr("concat")) == 0) {
    generate_concat(g, funcall);
  } else if (svcmp(funcall.name.lexeme, sv_from_cstr("toString")) == 0) {
    generate_to_string(g, funcall);
  } else if (svcmp(funcall.name.lexeme, sv_from_cstr("halt")) == 0) {
    /* halt is terminal just like return: release every active owned value and
     * restore bump marks before entering the runtime's non-returning exit. */
    FILE *saved_f = g->f;
    g->f = g->pre_f;
    emit_return_cleanup(g, sv_from_cstr(""));
    g->f = saved_f;
    fprintf(f, "halt(");
    for (int i = 0; i < funcall.args.length; i++) {
      if (i > 0) fprintf(f, ", ");
      generate_expression(g, funcall.args.data[i]);
    }
    fprintf(f, ")");
  } else if (svcmp(funcall.name.lexeme, sv_from_cstr("print")) == 0 &&
             funcall.args.length == 3) {
    // Overloaded print(x, y, text) — routes to the C print_at entry point
    // defined in src/lib/print_at.c. 1-arg print(text) falls through to the
    // generic path below and emits as C `print`.
    FILE *f = g->f;
    fprintf(f, "print_at(");
    for (int i = 0; i < funcall.args.length; i++) {
      if (i > 0) fprintf(f, ", ");
      generate_expression(g, funcall.args.data[i]);
    }
    fprintf(f, ")");
  } else if (svcmp(funcall.name.lexeme, sv_from_cstr("sleep")) == 0) {
    // Rock `sleep` → C `rock_sleep` to avoid POSIX unistd.h collision.
    FILE *f = g->f;
    fprintf(f, "rock_sleep(");
    for (int i = 0; i < funcall.args.length; i++) {
      if (i > 0) fprintf(f, ", ");
      generate_expression(g, funcall.args.data[i]);
    }
    fprintf(f, ")");
  } else if (svcmp(funcall.name.lexeme, sv_from_cstr("printf")) == 0) {
    // Rock printf takes one string argument.
    // ADR-0003 §13: route Rock string expressions through the length-aware
    // print() runtime helper. Substring views are not null-terminated, so
    // C %s would print past the substring's end into the source's bytes.
    FILE *f = g->f;
    ast_t arg = funcall.args.data[0];

    if (expr_returns_string(arg, g->table)) {
      fprintf(f, "print(");
      generate_expression(g, arg);
      fprintf(f, ")");
    }
    // Non-string: emit C printf as before
    else {
      fprintf(f, "printf(");
      generate_expression(g, arg);
      fprintf(f, ")");
    }
  } else {
    // Check if function is defined (either in name table or as a special builtin)
    string_view fname = funcall.name.lexeme;
    ast_t func_ref = get_ref(fname, g->table);

    // Function must be defined in name table
    if (func_ref == NULL) {
      error(funcall.name.filename, funcall.name.line, funcall.name.col,
            "undefined function " SV_Fmt, SV_Arg(fname));
      return;
    }

    // Tagged union constructor: Some(42) → Optional_Some(42)
    if (get_nt_kind(fname, g->table) == NT_ENUM_VARIANT &&
        func_ref->tag == tdef) {
      string_view type_name = func_ref->data.tdef.name.lexeme;
      ast_type payload_type = {0};
      int has_payload_type = 0;
      /* Union constructors take ownership of their payload.  Preserve a
       * borrower before the constructor adopts it; let produced values move
       * directly into the union. */
      if (funcall.args.length == 1) {
        ast_tdef union_def = func_ref->data.tdef;
        for (int i = 0; i < union_def.constructors.length; i++) {
          ast_cons constructor = union_def.constructors.data[i]->data.cons;
          if (svcmp(constructor.name.lexeme, fname) == 0 && constructor.type &&
              constructor.type->tag == type) {
            payload_type = constructor.type->data.type;
            has_payload_type = 1;
            emit_borrowed_container_retain(g, funcall.args.data[0], payload_type);
            break;
          }
        }
      }
      /* A string-producing expression commonly materialises as a tracked
       * __strtmp.  A direct constructor call cannot put a nullification
       * statement after its argument, so lower it through a prelude local and
       * transfer that temporary explicitly. */
      if (has_payload_type &&
          svcmp(payload_type.name.lexeme, SV_STRING) == 0) {
        char *payload = capture_expression(g, funcall.args.data[0]);
        char union_tmp[64];
        snprintf(union_tmp, sizeof(union_tmp), "__uniontmp_%d", g->str_tmp_counter++);
        fprintf(g->pre_f, SV_Fmt " %s;\n", SV_Arg(type_name), union_tmp);
        fprintf(g->pre_f, "%s = " SV_Fmt "_" SV_Fmt "(%s);\n", union_tmp,
                SV_Arg(type_name), SV_Arg(fname), payload);
        if (!rhs_is_borrower(funcall.args.data[0]) &&
            strncmp(payload, "__strtmp_", 9) == 0)
          emit_nullify_tmp(g->pre_f, payload);
        fprintf(f, "%s", union_tmp);
        free(payload);
        return;
      }
      fprintf(f, SV_Fmt "_" SV_Fmt "(", SV_Arg(type_name), SV_Arg(fname));
      for (int i = 0; i < funcall.args.length; i++) {
        if (i > 0)
          fprintf(f, ", ");
        generate_expression(g, funcall.args.data[i]);
      }
      fprintf(f, ")");
    } else {
      // Emit plain function call (mangled if the name is overloaded)
      emit_fun_name(f, g, funcall.name.lexeme, funcall.args.length);
      fprintf(f, "(");
      ast_array_t param_types = func_ref->data.fundef.types;
      int user_function = func_ref->data.fundef.body != NULL;
      /* User functions consume owned values. This retain is deliberately at
       * the call site: aliases keep their source share; constructor, pop, and
       * subscript results transfer without an otherwise-ownerless reference. */
      if (user_function) {
        for (int i = 0; i < funcall.args.length && i < param_types.length; i++) {
          if (param_types.data[i] && param_types.data[i]->tag == type) {
            ast_type param_type = param_types.data[i]->data.type;
            if (param_type.is_array ||
                is_heap_allocated_type(param_type.name.lexeme, g->table))
              emit_borrowed_container_retain(g, funcall.args.data[i], param_type);
          }
        }
      }
      for (int i = 0; i < funcall.args.length; i++) {
        if (i > 0)
          fprintf(f, ", ");
        string_view cast = sv_from_cstr("");
        if (g->auto_cast && i < param_types.length &&
            param_types.data[i] && param_types.data[i]->tag == type) {
          string_view ptn = param_types.data[i]->data.type.name.lexeme;
          if (svcmp(ptn, sv_from_cstr("byte"))  == 0 ||
              svcmp(ptn, sv_from_cstr("word"))  == 0 ||
              svcmp(ptn, sv_from_cstr("dword")) == 0) {
            cast = ptn;
          }
        }
        if (cast.length > 0) fprintf(f, "(" SV_Fmt ")(", SV_Arg(cast));
        generate_expression(g, funcall.args.data[i]);
        if (cast.length > 0) fprintf(f, ")");
      }
      fprintf(f, ")");
    }
  }
}

int calcEscapedLength(const char *str) {
  int length = 0;
  int i = 0;
  while (str[i]) {
    if (str[i] == '\\') { // Check if it is an escape character
      i++; // Move to the next character to interpret the escape sequence
      if (str[i] == 'n' || str[i] == 't' || str[i] == '\\' || str[i] == '"' ||
          str[i] == '\'' || str[i] == 'r') {
        length++; // These are single character escape sequences
      } else {
        length += 2; // For unrecognized escape sequences, count both characters
      }
    } else {
      length++;
    }
    i++;
  }
  return length - 1;
}

int get_literal_string_length(token_t tok) {
  return calcEscapedLength(string_of_sv(tok.lexeme));
}

void generate_op(generator_t *g, ast_t expr) {
  ast_op op = expr->data.op;
  FILE *f = g->f;
  generate_expression(g, op.left);
  if (op.op == TOK_EQUAL)
    fprintf(f, " == ");
  else
    fprintf(f, " " SV_Fmt " ", SV_Arg(lexeme_of_type(op.op)));
  generate_expression(g, op.right);
}

void generate_unary_op(generator_t *g, ast_t expr) {
  FILE *f = g->f;
  ast_unary_op u = expr->data.unary_op;
  fprintf(f, SV_Fmt "(", SV_Arg(lexeme_of_type(u.op)));
  generate_expression(g, u.operand);
  fprintf(f, ")");
}

void generate_if_statement(generator_t *g, ast_t stmt) {
  FILE *f = g->f;
  ast_ifstmt ifstmt = stmt->data.ifstmt;
  fprintf(f, "if (");
  generate_expression(g, ifstmt.expression);
  fprintf(f, ")\n");

  // Wrap non-compound bodies in braces to allow setup statements
  int wrap_body = (ifstmt.body->tag != compound);
  if (wrap_body) { fprintf(f, "{\n"); push_scope(g, -1); }
  generate_statement(g, ifstmt.body);
  if (wrap_body) { emit_scope_cleanup(g); pop_scope(g); fprintf(f, "}\n"); }

  if (ifstmt.elsestmt != NULL) {
    fprintf(f, "else\n");
    int wrap_else = (ifstmt.elsestmt->tag != compound);
    if (wrap_else) { fprintf(f, "{\n"); push_scope(g, -1); }
    generate_statement(g, ifstmt.elsestmt);
    if (wrap_else) { emit_scope_cleanup(g); pop_scope(g); fprintf(f, "}\n"); }
  }
}

// Generate all type-specific array helper functions (make_array, push_array, pop_array, etc.)
// This is a template that generates 6 helper functions for a given element type
void generate_array_funcs(generator_t *g, char *type_name) {
  FILE *f = g->f;
  int owns_handles = !is_builtin_typename(type_name);

  if (owns_handles) {
    fprintf(f, "static void __rock_release_array_elem_%s(void *slot) {\n", type_name);
    fprintf(f, "  __rock_release_%s(*(%s *)slot);\n", type_name, type_name);
    fprintf(f, "}\n\n");
  }

  // Template group: make_array, push_array, pop_array, get_elem, set_elem, insert
  // Each generates a wrapper around __internal_* functions with proper type handling

  fprintf(f, "__internal_dynamic_array_t %s_make_array(void) {\n", type_name);
  if (owns_handles) {
    fprintf(f, "  return __internal_make_array_with_release(sizeof(%s), 0, ", type_name);
    fprintf(f, "__rock_release_array_elem_%s);\n", type_name);
  } else {
    fprintf(f, "  return __internal_make_array(sizeof(%s), 0);\n", type_name);
  }
  fprintf(f, "}\n\n");

  fprintf(f, "void %s_push_array(__internal_dynamic_array_t arr, %s elem) {\n",
          type_name, type_name);
  /* elem is transferred in. Code generation retains only borrower args. */
  fprintf(f, "  __internal_push_array(arr, &elem);\n");
  fprintf(f, "}\n\n");

  fprintf(f, "%s %s_pop_array(__internal_dynamic_array_t arr) {\n", type_name,
          type_name);
  fprintf(f, "  %s result = {0};\n", type_name);
  fprintf(f, "  __internal_pop_into(arr, &result);\n");
  fprintf(f, "  return result;\n");
  fprintf(f, "}\n\n");

  fprintf(f, "%s %s_get_elem(__internal_dynamic_array_t arr, size_t index) {\n",
          type_name, type_name);
  fprintf(f, "  %s *res = __internal_get_elem(arr, index);\n", type_name);
  fprintf(f,
          "  if (res == NULL){ printf(\"NULL ELEMENT IN %s_get_elem\"); "
          "exit(1);}\n",
          type_name);
  /* Array reads borrow the slot. Destinations that outlive the expression
   * retain through rhs_is_borrower(), avoiding an untracked owner for
   * expressions such as get(items, 0).field. */
  fprintf(f, "  return *res;\n");
  fprintf(f, "}\n\n");

  fprintf(f,
          "void %s_set_elem(__internal_dynamic_array_t arr, size_t index, "
          "%s elem) {\n",
          type_name, type_name);
  if (owns_handles) {
    fprintf(f, "  %s *old = __internal_get_elem(arr, index);\n", type_name);
    fprintf(f, "  __rock_release_%s(*old);\n", type_name);
  }
  fprintf(f, "  __internal_set_elem(arr, index, &elem);\n");
  fprintf(f, "}\n\n");

  fprintf(f,
          "void %s_insert(__internal_dynamic_array_t arr, size_t index, "
          "%s elem) {\n",
          type_name, type_name);
  fprintf(f, "  __internal_insert(arr, index, &elem);\n");
  fprintf(f, "}\n\n");
}


void generate_loop(generator_t *g, ast_t loop_ast) {
  FILE *f = g->f;
  ast_loop loop = loop_ast->data.loop;
  new_nt_scope(&g->table);
  push_nt(&g->table, loop.variable.lexeme, NT_VAR, loop_ast);
  fprintf(f, "for(int " SV_Fmt " =", SV_Arg(loop.variable.lexeme));
  generate_expression(g, loop.start);
  fprintf(f, "; " SV_Fmt " <= (int)", SV_Arg(loop.variable.lexeme));
  generate_expression(g, loop.end);
  fprintf(f, "; " SV_Fmt "++)\n", SV_Arg(loop.variable.lexeme));

  // Wrap non-compound bodies in braces to allow setup statements
  int wrap_body = (loop.statement->tag != compound);
  if (wrap_body) { fprintf(f, "{\n"); push_scope(g, -1); }
  generate_statement(g, loop.statement);
  if (wrap_body) { emit_scope_cleanup(g); pop_scope(g); fprintf(f, "}\n"); }

  end_nt_scope(&g->table);
}

void generate_method_call(generator_t *g, ast_t node) {
  FILE *f = g->f;
  ast_method_call mc = node->data.method_call;
  string_view recv_type = infer_expr_type(mc.receiver, g->table);
  if (recv_type.length == 0) {
    error(mc.method.filename, mc.method.line, mc.method.col,
          "cannot determine type of receiver for method call '" SV_Fmt "'",
          SV_Arg(mc.method.lexeme));
  }
  char *method_name = mangle_method(recv_type, mc.method.lexeme, 0);
  ast_t method_ref = get_ref(sv_from_cstr(method_name), g->table);
  int receiver_is_array = recv_type.length > 6 &&
      strncmp(recv_type.data + recv_type.length - 6, "_array", 6) == 0;
  if (method_ref && method_ref->tag == fundef &&
      method_ref->data.fundef.body != NULL &&
      (receiver_is_array || is_heap_allocated_type(recv_type, g->table))) {
    ast_type receiver_type = {0};
    receiver_type.is_array = receiver_is_array;
    receiver_type.name.lexeme = receiver_is_array ?
        (string_view){.data = recv_type.data, .length = recv_type.length - 6} : recv_type;
    emit_borrowed_container_retain(g, mc.receiver, receiver_type);
  }
  if (method_ref && method_ref->tag == fundef &&
      method_ref->data.fundef.body != NULL) {
    ast_array_t params = method_ref->data.fundef.types;
    /* Method type lists include the implicit `this` receiver first. */
    for (int i = 0; i < mc.args.length && i + 1 < params.length; i++) {
      if (params.data[i + 1] && params.data[i + 1]->tag == type) {
        ast_type param = params.data[i + 1]->data.type;
        if (param.is_array || is_heap_allocated_type(param.name.lexeme, g->table))
          emit_borrowed_container_retain(g, mc.args.data[i], param);
      }
    }
  }
  fprintf(f, "%s(", mangle_method(recv_type, mc.method.lexeme, 0));
  generate_expression(g, mc.receiver);
  for (int i = 0; i < mc.args.length; i++) {
    fprintf(f, ", ");
    generate_expression(g, mc.args.data[i]);
  }
  fprintf(f, ")");
}

void generate_sub_as_expression(generator_t *g, ast_t expr) {
  FILE *f = g->f;
  ast_sub sub = expr->data.sub;
  generate_expression(g, sub.receiver);
  fprintf(f, "->");
  for (int i = 0; i < sub.path.length; i++) {
    fprintf(f, SV_Fmt "->", SV_Arg(sub.path.data[i].lexeme));
  }
  generate_expression(g, sub.expr);
}

void generate_while_loop(generator_t *g, ast_t loop) {
  FILE *f = g->f;
  ast_while_loop while_loop = loop->data.while_loop;
  fprintf(f, "while (");
  generate_expression(g, while_loop.condition);
  fprintf(f, ")\n");

  // Wrap non-compound bodies in braces to allow setup statements
  int wrap_body = (while_loop.statement->tag != compound);
  if (wrap_body) { fprintf(f, "{\n"); push_scope(g, -1); }
  generate_statement(g, while_loop.statement);
  if (wrap_body) { emit_scope_cleanup(g); pop_scope(g); fprintf(f, "}\n"); }
}

void generate_iter_loop(generator_t *g, ast_t loop) {
  FILE *f = g->f;
  ast_iter_loop iter_loop = loop->data.iter_loop;
  new_nt_scope(&g->table);

  fprintf(f, "for(int __iter_i = 0; __iter_i < ");
  generate_expression(g, iter_loop.iterable);
  fprintf(f, "->length; __iter_i++)\n");

  fprintf(f, "{\n");

  // Infer the element type from the iterable if it's an identifier
  if (iter_loop.iterable->tag == identifier) {
    string_view iter_name = iter_loop.iterable->data.identifier.id.lexeme;
    ast_t ref = get_ref(iter_name, g->table);
    ast_type elem_type = {0};
    int found_type = 0;
    if (ref != NULL && ref->tag == vardef) {
      elem_type = ref->data.vardef.type->data.type;
      found_type = 1;
    } else if (ref != NULL && ref->tag == fundef) {
      // Parameter lookup (e.g. "this" in an array method body)
      ast_fundef fd = ref->data.fundef;
      for (int j = 0; j < fd.args.length; j++) {
        if (svcmp(fd.args.data[j].lexeme, iter_name) == 0) {
          elem_type = fd.types.data[j]->data.type;
          found_type = 1;
          break;
        }
      }
    }
    if (found_type) {
      // Generate assignment with cast to proper pointer type before indexing
      fprintf(f, SV_Fmt " " SV_Fmt " = ((", SV_Arg(elem_type.name.lexeme), SV_Arg(iter_loop.variable.lexeme));
      fprintf(f, SV_Fmt " *)", SV_Arg(elem_type.name.lexeme));
      generate_expression(g, iter_loop.iterable);
      fprintf(f, "->data)[__iter_i];\n");
    } else {
      // Fallback: can't determine type, assign without type
      fprintf(f, "void *" SV_Fmt " = ", SV_Arg(iter_loop.variable.lexeme));
      generate_expression(g, iter_loop.iterable);
      fprintf(f, "->data[__iter_i];\n");
    }
  } else {
    // If iterable is not an identifier, we can't infer the type
    fprintf(f, "void *" SV_Fmt " = ", SV_Arg(iter_loop.variable.lexeme));
    generate_expression(g, iter_loop.iterable);
    fprintf(f, "->data[__iter_i];\n");
  }

  push_nt(&g->table, iter_loop.variable.lexeme, NT_VAR, loop);
  push_scope(g, -1);
  generate_statement(g, iter_loop.statement);
  emit_scope_cleanup(g);
  pop_scope(g);

  fprintf(f, "}\n");
  end_nt_scope(&g->table);
}

void generate_enum_tdef(generator_t *g, ast_t expr) {
  FILE *f = g->f;
  ast_enum_tdef enum_tdef = expr->data.enum_tdef;
  fprintf(f, "enum " SV_Fmt " {\n", SV_Arg(enum_tdef.name.lexeme));
  for (int i = 0; i < enum_tdef.items.length; i++) {
    if (i > 0)
      fprintf(f, ",\n");
    fprintf(f, SV_Fmt, SV_Arg(enum_tdef.items.data[i].lexeme));
    push_nt(&g->table, enum_tdef.items.data[i].lexeme, NT_ENUM_VARIANT, expr);
  }
  fprintf(f, "};\n");
}

void generate_expression(generator_t *g, ast_t expr) {
  FILE *f = g->f;
  if (expr->tag == literal) {
    token_t tok = expr->data.literal.lit;
    if (tok.type != TOK_STR_LIT)
      fprintf(f, SV_Fmt, SV_Arg(tok.lexeme));
    else {
      const char *tmp_var = allocate_string_tmp(g);
      emit_string_literal(g, tmp_var, tok);
      track_string_tmp(g, tmp_var);
    }
  } else if (expr->tag == identifier) {
    string_view lexeme = expr->data.identifier.id.lexeme;
    // In a module method, rewrite module field names to this->field
    if (g->current_module_type.length > 0) {
      ast_t mod_ref = get_ref(g->current_module_type, g->table);
      if (mod_ref && mod_ref->tag == tdef && mod_ref->data.tdef.t == TDEF_MODULE) {
        ast_array_t fields = mod_ref->data.tdef.module_fields;
        for (int i = 0; i < fields.length; i++) {
          ast_vardef vd = fields.data[i]->data.vardef;
          if (svcmp(vd.name.lexeme, lexeme) == 0) {
            fprintf(f, "this->" SV_Fmt, SV_Arg(lexeme));
            return;
          }
        }
      }
    }
    fprintf(f, SV_Fmt, SV_Arg(lexeme));
  } else if (expr->tag == funcall)
    generate_funcall(g, expr);
  else if (expr->tag == op)
    generate_op(g, expr);
  else if (expr->tag == unary_op)
    generate_unary_op(g, expr);
  else if (expr->tag == ifstmt)
    generate_if_statement(g, expr);
  else if (expr->tag == compound)
    generate_compound(g, expr);
  else if (expr->tag == loop)
    generate_loop(g, expr);
  else if (expr->tag == sub)
    generate_sub_as_expression(g, expr);
  else if (expr->tag == assign)
    generate_assignement(g, expr);
  else if (expr->tag == while_loop)
    generate_while_loop(g, expr);
  else if (expr->tag == iter_loop)
    generate_iter_loop(g, expr);
  else if (expr->tag == arr_index)
    generate_subscript(g, expr);
  else if (expr->tag == method_call)
    generate_method_call(g, expr);

  else {
    token_t tok = token_for_expr(expr);
    error(tok.filename, tok.line, tok.col,
          "unexpected AST node (tag %d) in expression context", expr->tag);
  }
}

void generate_assignement(generator_t *g, ast_t assignment) {
  FILE *f = g->f;
  ast_assign assign = assignment->data.assign;
  if (assign.target->tag == arr_index) {
    // arr[i] := value  =>  TYPE_set_elem(arr, i, value)
    ast_arr_index sub = assign.target->data.arr_index;
    token_t tok = token_for_expr(sub.array);
    string_view elem_type = get_array_element_type(sub.array, g->table, tok);
    if (!is_builtin_typename(string_of_sv(elem_type))) {
      ast_type element_type = {0};
      element_type.name.lexeme = elem_type;
      emit_borrowed_container_retain(g, assign.expr, element_type);
    }
    fprintf(f, SV_Fmt "_set_elem(", SV_Arg(elem_type));
    generate_expression(g, sub.array);
    fprintf(f, ", (size_t)(");
    generate_expression(g, sub.index);
    fprintf(f, "), ");
    generate_expression(g, assign.expr);
    fprintf(f, ")");
  } else {
    // For string reassignment, free the old value before overwriting
    if (assign.target->tag == identifier &&
        is_scalar_string_var(assign.target->data.identifier.id.lexeme, g->table)) {
      {
        // Capture RHS (populates pre_f with setup like __strtmp declarations)
        char *rhs_text = capture_expression(g, assign.expr);
        flush_pre_f(g, f);
        /* Retain a borrowed RHS before releasing the old slot. */
        if (rhs_is_borrower(assign.expr)) {
          fprintf(f, "__string_retain(%s);\n", rhs_text);
        }
        fprintf(f, "__string_release(" SV_Fmt ");\n",
                SV_Arg(assign.target->data.identifier.id.lexeme));
        // Borrower RHS: descriptor copy + retain. Replaces the legacy
        // deep-copy via new_string.
        if (rhs_is_borrower(assign.expr)) {
          fprintf(f, SV_Fmt " = %s;\n",
                  SV_Arg(assign.target->data.identifier.id.lexeme), rhs_text);
        } else {
          fprintf(f, SV_Fmt " = %s;\n",
                  SV_Arg(assign.target->data.identifier.id.lexeme), rhs_text);
          // Transfer ownership from temp to user var (prevents double-free at scope exit)
          if (strncmp(rhs_text, "__strtmp_", 9) == 0) {
            emit_nullify_tmp(f, rhs_text);
          }
        }
        free(rhs_text);
        return;
      }
    }
    // Aggregate reassignment. Borrower RHS: descriptor-copy + retain.
    // Producer RHS (funcall): accept the rc=1 reference __return_handle
    // already supplied on the callee side.
    if (assign.target->tag == identifier &&
        is_scalar_aggregate_var(assign.target->data.identifier.id.lexeme, g->table)) {
      string_view tname = assign.target->data.identifier.id.lexeme;
      // Self-assignment (`x := x`): release-then-retain on rc=1 would free
      // the block before the retain runs. Skip the no-op entirely.
      if (assign.expr->tag == identifier &&
          svcmp(assign.expr->data.identifier.id.lexeme, tname) == 0) {
        return;
      }
      char *rhs_text = capture_expression(g, assign.expr);
      flush_pre_f(g, f);
      string_view target_type = infer_expr_type(assign.target, g->table);
      fprintf(f, "__rock_release_" SV_Fmt "(" SV_Fmt ");\n",
              SV_Arg(target_type), SV_Arg(tname));
      if (rhs_is_borrower(assign.expr)) {
        fprintf(f, SV_Fmt " = %s; __handle_retain(" SV_Fmt ");\n",
                SV_Arg(tname), rhs_text, SV_Arg(tname));
      } else {
        fprintf(f, SV_Fmt " = %s;\n", SV_Arg(tname), rhs_text);
      }
      free(rhs_text);
      return;
    }
    if (assign.target->tag == sub) {
      string_view array_element_type =
          sub_target_array_element_type(assign.target, g->table);
      if (array_element_type.length > 0) {
        char *rhs_text = capture_expression(g, assign.expr);
        char *target_text = capture_expression(g, assign.target);
        flush_pre_f(g, f);
        if (strcmp(rhs_text, target_text) != 0) {
          if (rhs_is_borrower(assign.expr)) {
            fprintf(f, "__internal_retain_array(%s);\n", rhs_text);
          }
          fprintf(f, "__internal_free_array(%s, %d);\n", target_text,
                  svcmp(array_element_type, SV_STRING) == 0);
          fprintf(f, "%s = %s;\n", target_text, rhs_text);
        }
        free(rhs_text);
        free(target_text);
        return;
      }
    }
    if (assign.target->tag == sub && is_sub_target_scalar_aggregate(assign.target, g->table)) {
      char *rhs_text = capture_expression(g, assign.expr);
      char *target_text = capture_expression(g, assign.target);
      flush_pre_f(g, f);
      // Self-assignment (`p.f := p.f`) — same hazard as above.
      if (strcmp(rhs_text, target_text) == 0) {
        free(rhs_text);
        free(target_text);
        return;
      }
      string_view target_type = infer_expr_type(assign.target, g->table);
      fprintf(f, "__rock_release_" SV_Fmt "(%s);\n",
              SV_Arg(target_type), target_text);
      if (rhs_is_borrower(assign.expr)) {
        fprintf(f, "%s = %s; __handle_retain(%s);\n",
                target_text, rhs_text, target_text);
      } else {
        fprintf(f, "%s = %s;\n", target_text, rhs_text);
      }
      free(rhs_text);
      free(target_text);
      return;
    }
    // For string record field assignment (p.name := "Bob"), free old + deep-copy new
    if (assign.target->tag == sub && is_sub_target_scalar_string(assign.target, g->table)) {
      {
        char *rhs_text = capture_expression(g, assign.expr);
        char *target_text = capture_expression(g, assign.target);
        flush_pre_f(g, f);
        if (rhs_is_borrower(assign.expr)) {
          fprintf(f, "__string_retain(%s);\n", rhs_text);
        }
        fprintf(f, "__string_release(%s);\n", target_text);
        // Borrower RHS: descriptor copy + retain. Producer RHS: copy +
        // nullify the temp so scope cleanup doesn't double-free.
        if (rhs_is_borrower(assign.expr)) {
          fprintf(f, "%s = %s;\n", target_text, rhs_text);
        } else {
          fprintf(f, "%s = %s;\n", target_text, rhs_text);
          if (strncmp(rhs_text, "__strtmp_", 9) == 0) {
            emit_nullify_tmp(f, rhs_text);
          }
        }
        free(rhs_text);
        free(target_text);
        return;
      }
    }
    generate_expression(g, assign.target);
    fprintf(f, " = ");
    generate_expression(g, assign.expr);
  }
}

// Returns 1 if type_name refers to a module type
static int is_module_type(string_view type_name, name_table_t table) {
  ast_t ref = get_ref(type_name, table);
  return ref && ref->tag == tdef && ref->data.tdef.t == TDEF_MODULE;
}

// Returns 1 if the type is a pointer-allocated user type (module, record, or union)
void generate_vardef(generator_t *g, ast_t var) {
  FILE *f = g->f;
  ast_vardef vardef = var->data.vardef;
  push_nt(&g->table, vardef.name.lexeme, NT_VAR, var);
  string_view type_name = vardef.type->data.type.name.lexeme;

  // Global module vars cannot be initialized with TypeName_new() (not a constant).
  // Emit NULL and defer the real initialization to main().
  if (g->in_global_scope && !vardef.type->data.type.is_array &&
      is_module_type(type_name, g->table)) {
    generate_type(f, vardef.type);
    fprintf(f, " " SV_Fmt " = NULL;\n", SV_Arg(vardef.name.lexeme));
    push_ast_array(&g->deferred_module_inits, var);
    return;
  }
  if (vardef.type->data.type.is_array) {
    if (g->in_global_scope) {
      // Array initialization requires a function call — defer to main()
      generate_type(f, vardef.type);
      fprintf(f, " " SV_Fmt " = NULL;\n", SV_Arg(vardef.name.lexeme));
      // Build deferred init code
      char *code = NULL;
      size_t code_size = 0;
      FILE *code_f = open_memstream(&code, &code_size);
      int capacity = vardef.type->data.type.array_capacity;
      if (!is_builtin_typename(string_of_sv(type_name))) {
        fprintf(code_f, SV_Fmt " = " SV_Fmt "_make_array();\n",
                SV_Arg(vardef.name.lexeme), SV_Arg(type_name));
      } else {
        fprintf(code_f, SV_Fmt " = __internal_make_array(sizeof(" SV_Fmt "), %d);\n",
                SV_Arg(vardef.name.lexeme), SV_Arg(type_name), capacity);
      }
      fflush(code_f);
      fclose(code_f);
      push_deferred_global_init(g, code);
      return;
    }
    generate_type(f, vardef.type);
    fprintf(f, " " SV_Fmt " = \n", SV_Arg(vardef.name.lexeme));
    if (vardef.expr->tag != literal) {
      generate_expression(g, vardef.expr);
      fprintf(f, ";\n");
      if (rhs_is_borrower(vardef.expr)) {
        fprintf(f, "__internal_retain_array(" SV_Fmt ");\n",
                SV_Arg(vardef.name.lexeme));
      }
    } else if (vardef.expr->data.literal.lit.type != TOK_ARR_DECL) {
      error(vardef.name.filename, vardef.name.line, vardef.name.col,
            "Cannot declare arrays this way yet");
      return;
    } else {
      // Pass array capacity (0 for dynamic, or fixed size)
      int capacity = vardef.type->data.type.array_capacity;
      if (!is_builtin_typename(string_of_sv(type_name))) {
        fprintf(f, SV_Fmt "_make_array();\n", SV_Arg(type_name));
      } else {
        fprintf(f, "__internal_make_array(sizeof(" SV_Fmt "), %d);\n",
                SV_Arg(type_name), capacity);
      }
    }
    // Track array variable for scope cleanup (not global — those are freed at exit)
    if (!g->in_global_scope) {
      int is_str = (svcmp(type_name, SV_STRING) == 0);
      track_array_var(g, vardef.name.lexeme, is_str);
    }
  } else if (is_builtin_typename(string_of_sv(type_name))) {
    // Capture the expression reference in a temp buffer while accumulating
    // setup statements to pre_f
    char *expr_text = capture_expression(g, vardef.expr);

    if (g->in_global_scope) {
      // Check if the expression requires runtime setup (non-constant)
      fflush(g->pre_f);
      if (g->pre_buf && g->pre_buf_size > 0) {
        // Non-constant: emit zero-init at global scope, defer real init to main()
        defer_global_init(g, vardef.name.lexeme, expr_text);
        generate_type(f, vardef.type);
        fprintf(f, " " SV_Fmt " = {0};\n", SV_Arg(vardef.name.lexeme));
        free(expr_text);
        return;
      }
    }

    // Constant expression (e.g. integer literal) or inside a function: emit normally
    flush_pre_f(g, f);

    // String-to-string init from a borrower (identifier, field read, array
    // index): descriptor copy + retain. Both alias share the same backing.
    if (!g->in_global_scope && svcmp(type_name, SV_STRING) == 0
        && !vardef.type->data.type.is_array && rhs_is_borrower(vardef.expr)) {
      fprintf(f, "string " SV_Fmt " = %s; __string_retain(" SV_Fmt ");\n",
              SV_Arg(vardef.name.lexeme), expr_text, SV_Arg(vardef.name.lexeme));
    } else {
      generate_type(f, vardef.type);
      fprintf(f, " " SV_Fmt " = %s;\n", SV_Arg(vardef.name.lexeme), expr_text);
      // Transfer ownership from temp to user var (prevents double-free at scope exit)
      if (!g->in_global_scope && svcmp(type_name, SV_STRING) == 0
          && !vardef.type->data.type.is_array
          && strncmp(expr_text, "__strtmp_", 9) == 0) {
        emit_nullify_tmp(f, expr_text);
      }
    }

    // Track string variables for scope-based cleanup (not arrays)
    if (!g->in_global_scope && svcmp(type_name, SV_STRING) == 0
        && !vardef.type->data.type.is_array) {
      track_string_var(g, vardef.name.lexeme);
    }

    free(expr_text);
  } else if (!is_builtin_typename(string_of_sv(type_name)) &&
             vardef.expr->tag == record_expr) {
    ast_record_expr rec = vardef.expr->data.record_expr;

    // Look up struct type to get field types
    ast_t struct_ref = get_ref(type_name, g->table);

    if (vardef.is_rec) {
      // First, capture all field expressions (which fills pre_f with setup)
      char **field_bufs = malloc(rec.names.length * sizeof(char*));

      for (int i = 0; i < rec.names.length; i++) {
        ast_t expr = rec.exprs.data[i];

        // Check if this field is an array and expression is array literal
        int is_array_field = 0;
        string_view field_element_type = sv_from_cstr("");
        if (struct_ref && struct_ref->tag == tdef) {
          ast_tdef tdef = struct_ref->data.tdef;
          for (int j = 0; j < tdef.constructors.length; j++) {
            ast_t cons_ast = tdef.constructors.data[j];
            if (cons_ast->tag == cons) {
              ast_cons cons = cons_ast->data.cons;
              if (svcmp(cons.name.lexeme, rec.names.data[i].lexeme) == 0) {
                if (cons.type && cons.type->tag == type) {
                  ast_type field_type = cons.type->data.type;
                  if (field_type.is_array) {
                    is_array_field = 1;
                    field_element_type = field_type.name.lexeme;
                  }
                }
                break;
              }
            }
          }
        }

        // Capture the field value
        if (is_array_field && expr->tag == literal &&
            expr->data.literal.lit.type == TOK_ARR_DECL) {
          // Array field with array literal: use the element-specific factory
          // when it owns aggregate handles.
          field_bufs[i] = malloc(256);
          if (!is_builtin_typename(string_of_sv(field_element_type))) {
            snprintf(field_bufs[i], 256, SV_Fmt "_make_array()",
                     SV_Arg(field_element_type));
          } else {
            snprintf(field_bufs[i], 256, "__internal_make_array(sizeof(" SV_Fmt "), 0)",
                     SV_Arg(field_element_type));
          }
        } else if (expr->tag == sub) {
          field_bufs[i] = capture_expression(g, expr);
        } else {
          field_bufs[i] = capture_expression(g, expr);
        }
      }

      // Flush all setup statements before emitting struct initializer
      flush_pre_f(g, f);

      // Now emit the struct initializer with captured field values
      fprintf(f, "struct ");
      generate_type(f, vardef.type);
      fprintf(f, " tmp_" SV_Fmt " = {\n", SV_Arg(vardef.name.lexeme));
      for (int i = 0; i < rec.names.length; i++) {
        if (i > 0)
          fprintf(f, ",\n");
        fprintf(f, "." SV_Fmt " = %s", SV_Arg(rec.names.data[i].lexeme), field_bufs[i]);
      }
      fprintf(f, "};\n");

      // Free captured field buffers
      for (int i = 0; i < rec.names.length; i++) {
        free(field_bufs[i]);
      }
      free(field_bufs);

    } else {
      if (vardef.expr->tag == sub) {
        generate_sub_as_expression(g, vardef.expr);
      } else {
        generate_expression(g, vardef.expr);
      }
      fprintf(f, ";\n");
    }
    /* ADR-0003 Phase D extension: record body lives in the longlived
     * pool with a universal block header preceding it. The handle (the
     * pointer to the body) is the payload pointer returned by
     * rock_longlived_alloc; the header is at handle - sizeof(rock_block_header). */
    generate_type(f, vardef.type);
    fprintf(f, " " SV_Fmt " = rock_longlived_alloc(sizeof(struct ",
            SV_Arg(vardef.name.lexeme));
    generate_type(f, vardef.type);
    fprintf(f, "));\n");
    fprintf(f, "*" SV_Fmt " = tmp_" SV_Fmt ";\n", SV_Arg(vardef.name.lexeme),
            SV_Arg(vardef.name.lexeme));
    // Deep-copy string fields to prevent aliasing (the source may be a temp or variable
    // that gets freed at scope exit, while the record may outlive the scope)
    if (struct_ref && struct_ref->tag == tdef) {
      ast_tdef td = struct_ref->data.tdef;
      for (int j = 0; j < td.constructors.length; j++) {
        ast_t cons_ast = td.constructors.data[j];
        if (cons_ast->tag == cons) {
          ast_cons c = cons_ast->data.cons;
          if (c.type && c.type->tag == type) {
            ast_type ft = c.type->data.type;
            /* Record literals permit named fields in any order. Locate the
             * source expression by this declaration's field name rather than
             * assuming it shares the declaration index. */
            ast_t field_expr = NULL;
            for (int k = 0; k < rec.names.length; k++) {
              if (svcmp(rec.names.data[k].lexeme, c.name.lexeme) == 0) {
                field_expr = rec.exprs.data[k];
                break;
              }
            }
            if (ft.is_array && field_expr && rhs_is_borrower(field_expr)) {
              fprintf(f, "__internal_retain_array(" SV_Fmt "->" SV_Fmt ");\n",
                      SV_Arg(vardef.name.lexeme), SV_Arg(c.name.lexeme));
            } else if (!ft.is_array && svcmp(ft.name.lexeme, SV_STRING) == 0) {
              fprintf(f, "new_string(&" SV_Fmt "->" SV_Fmt ", tmp_" SV_Fmt "." SV_Fmt ");\n",
                      SV_Arg(vardef.name.lexeme), SV_Arg(c.name.lexeme),
                      SV_Arg(vardef.name.lexeme), SV_Arg(c.name.lexeme));
            } else if (!ft.is_array && is_heap_allocated_type(ft.name.lexeme, g->table)
                       && field_expr && rhs_is_borrower(field_expr)) {
              fprintf(f, "__handle_retain(" SV_Fmt "->" SV_Fmt ");\n",
                      SV_Arg(vardef.name.lexeme), SV_Arg(c.name.lexeme));
            }
          }
        }
      }
    }
    // Track record variable for scope cleanup
    if (!g->in_global_scope) {
      track_handle_var(g, vardef.name.lexeme, type_name);
    }
  } else {
    // Capture expression to allow pre_f setup (e.g. string temps in function args)
    char *expr_text = capture_expression(g, vardef.expr);
    flush_pre_f(g, f);
    generate_type(f, vardef.type);
    fprintf(f, " " SV_Fmt " = %s;\n", SV_Arg(vardef.name.lexeme), expr_text);
    int is_heap = !g->in_global_scope && is_heap_allocated_type(type_name, g->table);
    // Borrower init: alias of an existing handle needs a retain so source
    // and alias each carry an rc share. Producer RHS (funcall) already
    // brings rc=1 via __return_handle on the callee.
    if (is_heap && rhs_is_borrower(vardef.expr)) {
      fprintf(f, "__handle_retain(" SV_Fmt ");\n", SV_Arg(vardef.name.lexeme));
    }
    free(expr_text);
    if (is_heap) {
      track_handle_var(g, vardef.name.lexeme, type_name);
    }
  }
}

void generate_match(generator_t *g, ast_t match_ast) {
  FILE *f = g->f;
  ast_match m = match_ast->data.match;

  char *expr_text = capture_expression(g, m.expr);
  flush_pre_f(g, f);

  string_view type = infer_expr_type(m.expr, g->table);
  int is_string = svcmp(type, SV_STRING) == 0;
  int string_temp_scrutinee = strncmp(expr_text, "__strtmp_", 9) == 0;

  // For unions (TDEF_PRO), case arms compare against the discriminator key
  // rather than the value as a whole.
  int is_union = 0;
  if (type.length > 0) {
    ast_t type_ref = get_ref(type, g->table);
    if (type_ref && type_ref->tag == tdef &&
        type_ref->data.tdef.t == TDEF_PRO) {
      is_union = 1;
    }
  }

  if (type.length > 0) {
    fprintf(f, "{ " SV_Fmt " __match_tmp = %s;\n", SV_Arg(type), expr_text);
  } else {
    fprintf(f, "{ int __match_tmp = %s;\n", expr_text);
  }
  free(expr_text);
  push_scope(g, -1);

  // Pre-capture all case expressions and flush their setup statements
  // before the if-else chain so string temporaries are declared in scope.
  int n = m.cases.length;
  char **case_texts = allocate_compiler_persistent(n * sizeof(char *));
  int *wildcards = allocate_compiler_persistent(n * sizeof(int));
  for (int i = 0; i < n; i++) {
    ast_matchcase mc = m.cases.data[i]->data.matchcase;
    wildcards[i] = (mc.expr->tag == literal &&
                    mc.expr->data.literal.lit.type == TOK_WILDCARD);
    if (!wildcards[i]) {
      case_texts[i] = capture_expression(g, mc.expr);
      flush_pre_f(g, f);
    } else {
      case_texts[i] = NULL;
    }
  }

  int first = 1;
  for (int i = 0; i < n; i++) {
    ast_matchcase mc = m.cases.data[i]->data.matchcase;
    if (wildcards[i]) {
      if (!first)
        fprintf(f, "else {\n");
      else
        fprintf(f, "{\n");
    } else {
      if (first) {
        if (is_string)
          fprintf(f, "if (equals(__match_tmp, %s)) {\n", case_texts[i]);
        else if (is_union)
          fprintf(f, "if (__match_tmp->" UNION_KEY_FIELD " == %s) {\n", case_texts[i]);
        else
          fprintf(f, "if (__match_tmp == %s) {\n", case_texts[i]);
        first = 0;
      } else {
        if (is_string)
          fprintf(f, "else if (equals(__match_tmp, %s)) {\n", case_texts[i]);
        else if (is_union)
          fprintf(f, "else if (__match_tmp->" UNION_KEY_FIELD " == %s) {\n", case_texts[i]);
        else
          fprintf(f, "else if (__match_tmp == %s) {\n", case_texts[i]);
      }
      free(case_texts[i]);
    }
    push_scope(g, -1);
    generate_statement(g, mc.body);
    emit_scope_cleanup(g);
    pop_scope(g);
    fprintf(f, "}\n");
  }
  emit_scope_cleanup(g);
  pop_scope(g);
  /* A produced scrutinee is an owned temporary. Match arms only borrow it. */
  int match_string_array = 0;
  if (!rhs_is_borrower(m.expr) && expr_is_array(m.expr, g->table, &match_string_array)) {
    fprintf(f, "__internal_free_array(__match_tmp, %d);\n", match_string_array);
  } else if (!rhs_is_borrower(m.expr) && is_heap_allocated_type(type, g->table)) {
    fprintf(f, "__rock_release_" SV_Fmt "(__match_tmp);\n", SV_Arg(type));
  } else if (!rhs_is_borrower(m.expr) && is_string && !string_temp_scrutinee) {
    fprintf(f, "__string_release(__match_tmp);\n");
  }
  fprintf(f, "}\n");
}

void generate_return(generator_t *g, ast_t ret_ast) {
  FILE *f = g->f;
  if (ret_ast->data.ret.expr != NULL) {
    char *expr_text = capture_expression(g, ret_ast->data.ret.expr);
    flush_pre_f(g, f);

    /* ADR-0003 §10.3: non-scalar returns are materialised into longlived
     * via the per-type return helper. For strings: __return_string inc's
     * the longlived refcount (or copies bump→longlived; or returns static
     * unchanged). The caller transfers the producer into its destination.
     *
     * Sequencing: capture the wrap result BEFORE the cleanup pass. The
     * cleanup releases every owned local in every enclosing scope; the
     * wrap captured an extra reference to the return value's backing so
     * the release dec's it back to its pre-call refcount, then the
     * caller's transfer takes ownership. */
    if (expr_returns_string(ret_ast->data.ret.expr, g->table)) {
      char retval[32];
      snprintf(retval, sizeof(retval), "__retval_%d", g->str_tmp_counter++);
      fprintf(f, "string %s = __return_string(%s);\n", retval, expr_text);
      /* No skip: release every owned local. The wrap captured the value
       * we need so the cleanup safely dec's everything else. */
      emit_return_cleanup(g, sv_from_cstr(""));
      fprintf(f, "return %s;\n", retval);
      free(expr_text);
      return;
    }

    /* Capture into a typed temp before cleanup so the cleanup pass can
     * release parameters/locals the return expression still references.
     * For aggregate handles, route the capture through __return_handle so
     * the subsequent release leaves the caller with an owned reference. */
    if (g->current_fundef != NULL
        && g->current_fundef->data.fundef.ret_type != NULL) {
      ast_t ret_type_node = g->current_fundef->data.fundef.ret_type;
      int is_aggregate =
          ret_type_node->tag == type
          && !ret_type_node->data.type.is_array
          && is_heap_allocated_type(ret_type_node->data.type.name.lexeme, g->table);
      int is_array = ret_type_node->tag == type && ret_type_node->data.type.is_array;
      int aggregate_borrower = is_aggregate && rhs_is_borrower(ret_ast->data.ret.expr);
      char retval[32];
      snprintf(retval, sizeof(retval), "__retval_%d", g->str_tmp_counter++);
      generate_type(f, ret_type_node);
      fprintf(f, " %s = %s%s%s;\n", retval,
              aggregate_borrower ? "__return_handle(" : "",
              expr_text,
              aggregate_borrower ? ")" : "");
      if (is_array && rhs_is_borrower(ret_ast->data.ret.expr)) {
        fprintf(f, "__internal_retain_array(%s);\n", retval);
      }
      emit_return_cleanup(g, sv_from_cstr(""));
      fprintf(f, "return %s;\n", retval);
      free(expr_text);
      return;
    }

    string_view skip = sv_from_cstr("");
    if (ret_ast->data.ret.expr->tag == identifier) {
      string_view ret_name = ret_ast->data.ret.expr->data.identifier.id.lexeme;
      if (is_scalar_string_var(ret_name, g->table))
        skip = ret_name;
    }
    if (strncmp(expr_text, "__strtmp_", 9) == 0) {
      skip = (string_view){.data = expr_text, .length = strlen(expr_text)};
    }

    emit_return_cleanup(g, skip);
    fprintf(f, "return %s;\n", expr_text);
    free(expr_text);
  } else {
    // Void return — clean up all strings in all scopes (not arrays — see above)
    emit_return_cleanup(g, sv_from_cstr(""));
    fprintf(f, "return;\n");
  }
}

void generate_embed(generator_t *g, ast_t node) {
  FILE *f = g->f;
  ast_embed e = node->data.embed;

  if (strcmp(e.lang, "c") == 0) {
    if (e.is_function) {
      // Emit verbatim at file scope
      fprintf(f, "%s\n", e.body);
    } else {
      // Emit verbatim inside current function scope
      fprintf(f, "{\n%s\n}\n", e.body);
    }
  } else if (strcmp(e.lang, "asm") == 0) {
    // Z88DK inline assembly: emitted only when compiled with SDCC.
    // Host/GCC builds skip the block entirely; Z80 mnemonics are not valid x86.
    // Leading newline ensures the #ifdef starts on its own line (function open brace
    // is emitted without a trailing newline).
    fprintf(f, "\n#ifdef __SDCC\n#asm\n%s\n#endasm\n#endif\n", e.body);
  } else {
    // Unknown language: emit warning
    fprintf(f, "/* WARNING: Unknown embed language '%s' */\n", e.lang);
  }
}

void generate_statement(generator_t *g, ast_t stmt) {
  FILE *f = g->f;
  if (stmt->tag == vardef) {
    generate_vardef(g, stmt);
  } else if (stmt->tag == embed) {
    // Skip top-level embed functions (they're already handled in generate_forward_defs)
    // Only emit inline embed blocks
    ast_embed e = stmt->data.embed;
    if (!e.is_function) {
      generate_embed(g, stmt);
    }
  } else if (stmt->tag == match) {
    generate_match(g, stmt);
  } else if (stmt->tag == collect_stmt) {
    fprintf(f, "rock_collect();\n");
  } else if (stmt->tag == ret) {
    generate_return(g, stmt);
  } else if (stmt->tag == compound) {
    generate_compound(g, stmt);
  } else if (stmt->tag == ifstmt) {
    generate_if_statement(g, stmt);
  } else if (stmt->tag == tdef) {
    generate_tdef(g, stmt);
  } else if (stmt->tag == fundef) {
    generate_fundef(g, stmt);

  } else if (stmt->tag == enum_tdef) {
    // Already emitted by generate_forward_defs(); skip.
  } else if (stmt->tag == iter_loop)
    generate_iter_loop(g, stmt);
  else {
    // For expression statements, capture the expression (which fills pre_f with setup)
    // then flush the setup before emitting the expression
    char *expr_text = capture_expression(g, stmt);
    // Flush any accumulated setup statements from pre_f
    flush_pre_f(g, f);
    string_view value_type = infer_expr_type(stmt, g->table);
    int discard_string_array = 0;
    if (!rhs_is_borrower(stmt) && expr_is_array(stmt, g->table, &discard_string_array)) {
      fprintf(f, "__internal_free_array(%s, %d);\n", expr_text, discard_string_array);
    } else if (!rhs_is_borrower(stmt) && is_heap_allocated_type(value_type, g->table)) {
      /* Materialise once before releasing: re-emitting a producer would
       * allocate a second value and leave the first ownerless. */
      char discard_tmp[64];
      snprintf(discard_tmp, sizeof(discard_tmp), "__discardtmp_%d", g->str_tmp_counter++);
      fprintf(f, SV_Fmt " %s = %s;\n", SV_Arg(value_type), discard_tmp, expr_text);
      fprintf(f, "__rock_release_" SV_Fmt "(%s);\n", SV_Arg(value_type), discard_tmp);
    } else if (!rhs_is_borrower(stmt) && svcmp(value_type, SV_STRING) == 0 &&
               strncmp(expr_text, "__strtmp_", 9) != 0) {
      fprintf(f, "%s;\n", expr_text);
      fprintf(f, "__string_release(%s);\n", expr_text);
    } else {
      // Emit the captured expression and semicolon.
      fprintf(f, "%s;\n", expr_text);
    }
    free(expr_text);
  }
}
void generate_compound(generator_t *g, ast_t comp) {
  FILE *f = g->f;
  int bump_mark_id = g->bump_mark_counter++;
  fprintf(f, "{");
  fprintf(f, "rock_bump_mark __bm_%d = rock_bump_save();\n", bump_mark_id);
  ast_compound compound = comp->data.compound;
  new_nt_scope(&g->table);
  push_scope(g, bump_mark_id);
  for (int i = 0; i < compound.stmts.length; i++)
    generate_statement(g, compound.stmts.data[i]);
  emit_scope_cleanup(g);
  pop_scope(g);
  end_nt_scope(&g->table);
  fprintf(f, "rock_bump_restore(__bm_%d);\n", bump_mark_id);
  fprintf(f, "}");
}

/* Same structure as generate_compound. User-function parameters consume an
 * owned reference: callers retain only borrowed arguments, while a produced
 * result moves directly into the parameter slot. Tracking hands that reference
 * to the normal scope/return cleanup. */
void generate_function_body(generator_t *g, ast_t fun) {
  FILE *f = g->f;
  ast_fundef fundef = fun->data.fundef;
  int bump_mark_id = g->bump_mark_counter++;
  fprintf(f, "{");
  fprintf(f, "rock_bump_mark __bm_%d = rock_bump_save();\n", bump_mark_id);
  ast_compound compound = fundef.body->data.compound;
  new_nt_scope(&g->table);
  push_scope(g, bump_mark_id);

  /* Make the enclosing fundef visible to generate_return so it can emit
   * the correct return-value temp type. Saved/restored to support nested
   * function definitions cleanly (defensive — Rock currently doesn't
   * have nested functions, but the pattern is safe regardless). */
  ast_t saved_fundef = g->current_fundef;
  g->current_fundef = fun;

  for (int i = 0; i < fundef.args.length; i++) {
    if (i >= fundef.types.length) continue;
    ast_t t = fundef.types.data[i];
    if (t == NULL || t->tag != type) continue;
    string_view tname = t->data.type.name.lexeme;
    string_view pname = fundef.args.data[i].lexeme;
    if (t->data.type.is_array) {
      track_array_var(g, pname, svcmp(tname, SV_STRING) == 0);
    } else if (svcmp(tname, SV_STRING) == 0) {
      /* String expression lowering uses tracked descriptor temporaries, so
       * strings retain the established borrowed-parameter ABI. */
      fprintf(f, "__string_retain(" SV_Fmt ");\n", SV_Arg(pname));
      track_string_var(g, pname);
    } else if (is_heap_allocated_type(tname, g->table)) {
      track_handle_var(g, pname, tname);
    }
  }

  for (int i = 0; i < compound.stmts.length; i++)
    generate_statement(g, compound.stmts.data[i]);

  emit_scope_cleanup(g);
  pop_scope(g);
  end_nt_scope(&g->table);
  g->current_fundef = saved_fundef;
  fprintf(f, "rock_bump_restore(__bm_%d);\n", bump_mark_id);
  fprintf(f, "}");
}

void generate_fundef(generator_t *g, ast_t fun) {
  // add to name table
  FILE *f = g->f;
  ast_fundef fundef = fun->data.fundef;
  new_nt_scope(&g->table);

  string_view emit_name;
  if (fundef.is_method)
    emit_name = sv_from_cstr(mangle_method(fundef.type_name.lexeme, fundef.name.lexeme, fundef.is_array_method));
  else
    emit_name = fundef.name.lexeme;
  push_nt(&g->table, emit_name, NT_FUN, fun);

  if (svcmp(fundef.name.lexeme, sv_from_cstr("main")) != 0) {
    generate_type(f, fundef.ret_type);
    fprintf(f, " ");
    if (fundef.is_method)
      fprintf(f, SV_Fmt, SV_Arg(emit_name));
    else
      emit_fun_name(f, g, fundef.name.lexeme, fundef.args.length);
    fprintf(f, "(");
    if (fundef.args.length == 0) {
      fprintf(f, "void");
    } else {
      for (int i = 0; i < fundef.args.length; i++) {
        push_nt(&g->table, fundef.args.data[i].lexeme, NT_VAR, fun);
        if (i > 0)
          fprintf(f, ", ");
        generate_type(f, fundef.types.data[i]);
        fprintf(f, " ");
        fprintf(f, SV_Fmt, SV_Arg(fundef.args.data[i].lexeme));
      }
    }
    fprintf(f, ")\n");
    // For module methods, expose field names as implicit this-> references
    string_view saved_module_type = g->current_module_type;
    if (fundef.is_method)
      g->current_module_type = fundef.type_name.lexeme;
    int saved_global = g->in_global_scope;
    g->in_global_scope = 0;
    generate_function_body(g, fun);
    g->in_global_scope = saved_global;
    g->current_module_type = saved_module_type;
    fprintf(f, "\n\n");
  } else {
    fprintf(f, "int main(int argc, char **argv) {\n");
    if (g->zxn_test)
      fprintf(f, "zxn_test_begin();\n");
    /* ADR-0003 §4: pool runtime init. Pool sizes are placeholders pending
     * Phase A.1 measurement; ZXN target gets smaller defaults than host. */
    if (g->target == TARGET_ZXN || g->zxn_memory_profile) {
      if (g->zxn_test)
        fprintf(f, "zxn_test_stage(\"pools\");\n");
      fprintf(f, "rock_pools_init(1024, 6144);\n");
      fprintf(f, "__internal_set_dynamic_array_initial_capacity(8);\n");
    } else {
      fprintf(f, "rock_pools_init(4u * 1024u * 1024u, 4u * 1024u * 1024u);\n");
    }
    if (g->zxn_test)
      fprintf(f, "zxn_test_stage(\"arguments\");\n");
    fprintf(f, "fill_cmd_args(argc, argv);\n");
    if (g->zxn_test)
      fprintf(f, "zxn_test_stage(\"rtl\");\n");
    fprintf(f, "rock_rtl_init();\n");
    // Initialize any global module vars that were deferred from global scope
    for (int i = 0; i < g->deferred_module_inits.length; i++) {
      ast_vardef vd = g->deferred_module_inits.data[i]->data.vardef;
      string_view tn = vd.type->data.type.name.lexeme;
      fprintf(f, SV_Fmt " = " SV_Fmt "_new();\n",
              SV_Arg(vd.name.lexeme), SV_Arg(tn));
    }
    // Initialize any global vars with non-constant expressions (e.g. strings)
    for (int i = 0; i < g->deferred_global_init_count; i++) {
      fprintf(f, "%s", g->deferred_global_init_code[i]);
    }
    int saved_global = g->in_global_scope;
    g->in_global_scope = 0;
    generate_compound(g, fundef.body);
    g->in_global_scope = saved_global;
    if (g->zxn_test)
      fprintf(f, "zxn_test_finish();\n");
    fprintf(f, "rock_rtl_shutdown();\n");
    fprintf(f, "rock_pools_deinit();\n");
    fprintf(f, "return 0;\n");
    fprintf(f, "}\n\n");
  }
  end_nt_scope(&g->table);
}

int is_builtin_typename(char *name) {
  if (strcmp(name, "boolean") == 0)
    return 1;
  if (strcmp(name, "int") == 0)
    return 1;
  if (strcmp(name, "char") == 0)
    return 1;
  if (strcmp(name, "byte") == 0)
    return 1;
  if (strcmp(name, "word") == 0)
    return 1;
  if (strcmp(name, "dword") == 0)
    return 1;
  if (strcmp(name, "float") == 0)
    return 1;
  if (strcmp(name, "string") == 0)
    return 1;
  if (strcmp(name, "void") == 0)
    return 1;
  return 0;
}

void generate_forward_defs(generator_t *g, ast_t program) {
  FILE *f = g->f;
  ast_array_t stmts = program->data.program.prog;
  for (int i = 0; i < stmts.length; i++) {
    ast_t stmt = stmts.data[i];
    /* Module field initializers are emitted before ordinary function bodies,
     * but may legitimately call a later source function.  Register every
     * top-level function while producing its C prototype so those initializers
     * have the same lookup contract as normal expressions. */
    if (stmt->tag == fundef) {
      ast_fundef fd = stmt->data.fundef;
      push_nt(&g->table, fd.name.lexeme, NT_FUN, stmt);
    }
    if (stmt->tag == tdef) {
      struct ast_tdef tdef = stmt->data.tdef;
      string_view name = tdef.name.lexeme;
      fprintf(f, "typedef struct " SV_Fmt " *" SV_Fmt ";\n", SV_Arg(name),
              SV_Arg(name));
      if (tdef.t == TDEF_MODULE)
        fprintf(f, SV_Fmt " " SV_Fmt "_new(void);\n", SV_Arg(name), SV_Arg(name));
      if (tdef.t == TDEF_PRO) {
        for (int j = 0; j < tdef.constructors.length; j++) {
          ast_cons cons = tdef.constructors.data[j]->data.cons;
          ast_type ctype = cons.type->data.type;
          int is_void = svcmp(ctype.name.lexeme, sv_from_cstr("void")) == 0;
          if (is_void) {
            fprintf(f, SV_Fmt " " SV_Fmt "_" SV_Fmt "(void);\n",
                    SV_Arg(name), SV_Arg(name), SV_Arg(cons.name.lexeme));
          } else {
            fprintf(f, SV_Fmt " " SV_Fmt "_" SV_Fmt "(",
                    SV_Arg(name), SV_Arg(name), SV_Arg(cons.name.lexeme));
            generate_type(f, cons.type);
            fprintf(f, ");\n");
          }
        }
      }
    }
    if (stmt->tag == enum_tdef) {
      generate_enum_tdef(g, stmt);
      ast_enum_tdef etdef = stmt->data.enum_tdef;
      fprintf(f, "typedef enum " SV_Fmt " " SV_Fmt ";\n",
              SV_Arg(etdef.name.lexeme), SV_Arg(etdef.name.lexeme));
    }
    if (stmt->tag == embed) {
      ast_embed e = stmt->data.embed;
      if (e.is_function) {
        // Top-level embed functions - emit them here and register in name table
        generate_embed(g, stmt);

        // Register the function name from the embed block
        // Simple heuristic: look for "type name(" pattern
        if (strcmp(e.lang, "c") == 0) {
          // Extract function name from C code
          // Look for the last identifier before '('
          char *body = e.body;
          char *paren = strchr(body, '(');
          if (paren != NULL) {
            // Walk back to find the function name
            int i = paren - body - 1;

            // Skip whitespace
            while (i >= 0 && (body[i] == ' ' || body[i] == '\t')) i--;

            // Find end of identifier
            int end = i + 1;

            // Walk back to find start of identifier
            while (i >= 0 && ((body[i] >= 'a' && body[i] <= 'z') ||
                             (body[i] >= 'A' && body[i] <= 'Z') ||
                             (body[i] >= '0' && body[i] <= '9') ||
                             body[i] == '_')) {
              i--;
            }
            int start = i + 1;

            if (start < end) {
              // Register it in the name table
              string_view fname_sv = sv_from_parts(body + start, end - start);
              push_nt(&g->table, fname_sv, NT_FUN, stmt);
            }
          }
        }
      }
    }
  }

  for (int i = 0; i < stmts.length; i++) {
    ast_t stmt = stmts.data[i];
    if (stmt->tag == fundef) {
      ast_fundef fundef = stmt->data.fundef;
      if (svcmp(fundef.name.lexeme, sv_from_cstr("main")) != 0) {
        generate_type(f, fundef.ret_type);

        if (fundef.is_method) {
          fprintf(f, " %s(", mangle_method(fundef.type_name.lexeme, fundef.name.lexeme, fundef.is_array_method));
        } else {
          fprintf(f, " ");
          emit_fun_name(f, g, fundef.name.lexeme, fundef.args.length);
          fprintf(f, "(");
        }
        if (fundef.args.length == 0) {
          fprintf(f, "void");
        } else {
          for (int i = 0; i < fundef.args.length; i++) {
            if (i > 0)
              fprintf(f, ", ");
            generate_type(f, fundef.types.data[i]);
            fprintf(f, " ");
            fprintf(f, SV_Fmt, SV_Arg(fundef.args.data[i].lexeme));
          }
        }
        fprintf(f, ");\n\n");
      }
    }
  }

  for (int i = 0; i < stmts.length; i++) {
    ast_t stmt = stmts.data[i];
    if (stmt->tag == tdef) {
      struct ast_tdef tdef = stmt->data.tdef;
      char *name = string_of_sv(tdef.name.lexeme);
      generate_array_funcs(g, name);
    }
  }
}

void generate_tdef(generator_t *g, ast_t tdef_ast) {
  FILE *f = g->f;
  struct ast_tdef tdef = tdef_ast->data.tdef;
  string_view name = tdef.name.lexeme;

  // Register the type in the name table so it can be looked up later
  push_nt(&g->table, name, NT_USER_TYPE, tdef_ast);

  if (tdef.t == TDEF_MODULE) {
    // Register TypeName_new in the name table
    register_builtin(&g->table, mangle_method(name, sv_from_cstr("new"), 0), string_of_sv(name));

    if (tdef.module_fields.length == 0) {
      fprintf(f, "struct " SV_Fmt " { char _reserved; };\n", SV_Arg(name));
    } else {
      fprintf(f, "struct " SV_Fmt " {\n", SV_Arg(name));
      for (int i = 0; i < tdef.module_fields.length; i++) {
        ast_vardef vd = tdef.module_fields.data[i]->data.vardef;
        generate_type(f, vd.type);
        fprintf(f, " " SV_Fmt ";\n", SV_Arg(vd.name.lexeme));
      }
      fprintf(f, "};\n");
    }
    // Emit allocator: TypeName TypeName_new(void) { ... }
    fprintf(f, SV_Fmt " " SV_Fmt "_new(void) {\n", SV_Arg(name), SV_Arg(name));
    /* ADR-0003 Phase D extension: module body in the longlived pool. */
    fprintf(f, "  " SV_Fmt " __inst = (" SV_Fmt ")rock_longlived_alloc(sizeof(struct " SV_Fmt "));\n",
            SV_Arg(name), SV_Arg(name), SV_Arg(name));
    for (int i = 0; i < tdef.module_fields.length; i++) {
      ast_vardef vd = tdef.module_fields.data[i]->data.vardef;
      ast_type field_type = vd.type->data.type;
      char *field_text = capture_expression(g, vd.expr);
      flush_pre_f(g, f);
      if (!field_type.is_array &&
          svcmp(field_type.name.lexeme, SV_STRING) == 0) {
        /* Modules retain a private copy of every string initializer. */
        fprintf(f, "  new_string(&__inst->" SV_Fmt ", %s);\n",
                SV_Arg(vd.name.lexeme), field_text);
        if (!rhs_is_borrower(vd.expr))
          fprintf(f, "  __string_release(%s);\n", field_text);
      } else {
        fprintf(f, "  __inst->" SV_Fmt " = %s;\n",
                SV_Arg(vd.name.lexeme), field_text);
        if ((field_type.is_array ||
             is_heap_allocated_type(field_type.name.lexeme, g->table)) &&
            rhs_is_borrower(vd.expr)) {
          if (field_type.is_array)
            fprintf(f, "  __internal_retain_array(__inst->" SV_Fmt ");\n",
                    SV_Arg(vd.name.lexeme));
          else
            fprintf(f, "  __handle_retain(__inst->" SV_Fmt ");\n",
                    SV_Arg(vd.name.lexeme));
        }
      }
      free(field_text);
    }
    fprintf(f, "  return __inst;\n}\n");
    emit_type_release_walker(g, name, tdef, 1);
    return;
  }

  // fprintf(f, "typedef struct %s %s;\n", name, name);
  fprintf(f, "struct " SV_Fmt "{\n", SV_Arg(name));
  if (tdef.t == TDEF_PRO) {
    fprintf(f, "enum {\n");
    for (int i = 0; i < tdef.constructors.length; i++) {
      ast_cons cons = tdef.constructors.data[i]->data.cons;
      if (i > 0)
        fprintf(f, ",\n");
      fprintf(f, SV_Fmt, SV_Arg(cons.name.lexeme));
    }
    fprintf(f, "\n} " UNION_KEY_FIELD "; \n");
    fprintf(f, "union {\n");
    for (int i = 0; i < tdef.constructors.length; i++) {
      ast_cons cons = tdef.constructors.data[i]->data.cons;
      ast_type type = cons.type->data.type;
      if (svcmp(type.name.lexeme, sv_from_cstr("void")) != 0) {
        generate_type(f, cons.type);
        fprintf(f, " " SV_Fmt ";\n", SV_Arg(cons.name.lexeme));
      }
    }
    fprintf(f, "} " UNION_VALUE_FIELD ";");
  } else {
    for (int i = 0; i < tdef.constructors.length; i++) {
      ast_cons cons = tdef.constructors.data[i]->data.cons;
      ast_type type = cons.type->data.type;
      if (svcmp(type.name.lexeme, sv_from_cstr("void")) != 0) {
        generate_type(f, cons.type);
        fprintf(f, " " SV_Fmt ";\n", SV_Arg(cons.name.lexeme));
      }
    }
  }
  fprintf(f, "};\n");

  // Emit constructor functions for unions (TDEF_PRO)
  if (tdef.t == TDEF_PRO) {
    for (int i = 0; i < tdef.constructors.length; i++) {
      ast_cons cons = tdef.constructors.data[i]->data.cons;
      ast_type ctype = cons.type->data.type;
      int is_void = svcmp(ctype.name.lexeme, sv_from_cstr("void")) == 0;

      // Function signature: TypeName TypeName_VariantName(payload_type payload)
      if (is_void) {
        fprintf(f, SV_Fmt " " SV_Fmt "_" SV_Fmt "(void) {\n",
                SV_Arg(name), SV_Arg(name), SV_Arg(cons.name.lexeme));
      } else {
        fprintf(f, SV_Fmt " " SV_Fmt "_" SV_Fmt "(",
                SV_Arg(name), SV_Arg(name), SV_Arg(cons.name.lexeme));
        generate_type(f, cons.type);
        fprintf(f, " payload) {\n");
      }

      // Body: allocate, set tag, set data, return
      /* ADR-0003 Phase D extension: union body in the longlived pool. */
      fprintf(f, "  " SV_Fmt " __inst = rock_longlived_alloc(sizeof(struct " SV_Fmt "));\n",
              SV_Arg(name), SV_Arg(name));
      fprintf(f, "  __inst->" UNION_KEY_FIELD " = " SV_Fmt ";\n", SV_Arg(cons.name.lexeme));
      if (!is_void) {
        fprintf(f, "  __inst->" UNION_VALUE_FIELD "." SV_Fmt " = payload;\n", SV_Arg(cons.name.lexeme));
      }
      fprintf(f, "  return __inst;\n}\n");

      // Register variant as NT_ENUM_VARIANT with the tdef AST as ref
      push_nt(&g->table, cons.name.lexeme, NT_ENUM_VARIANT, tdef_ast);
    }
  }

  emit_type_release_walker(g, name, tdef, 0);

  return;
}

void transpile(generator_t *g, ast_t program) {
  FILE *f = g->f;
  g->program = program;
  ast_array_t stmts = program->data.program.prog;

  fprintf(f, "#include <stdlib.h>\n");
  fprintf(f, "#include \"pools.h\"\n");
  fprintf(f, "#include \"fundefs.h\"\n");
  fprintf(f, "#include \"fundefs_internal.h\"\n");
  fprintf(f, "#include \"typedefs.h\"\n");
  fprintf(f, "#include \"host_caps.h\"\n");
  fprintf(f, "#include \"keyboard.h\"\n");
  fprintf(f, "#include \"border.h\"\n");
  fprintf(f, "#include \"ink_paper.h\"\n");
  fprintf(f, "#include \"cls.h\"\n");
  fprintf(f, "#include \"time.h\"\n");
  fprintf(f, "#include \"sound.h\"\n");
  fprintf(f, "#include \"input.h\"\n");
  fprintf(f, "#include \"print_at.h\"\n");
  fprintf(f, "#include \"plot.h\"\n");
  fprintf(f, "#include \"draw.h\"\n");
  fprintf(f, "#include \"polyline.h\"\n");
  fprintf(f, "#include \"circle.h\"\n");
  fprintf(f, "#include \"fill.h\"\n");
  fprintf(f, "#include \"triangle.h\"\n");
  fprintf(f, "#include \"random.h\"\n");
  fprintf(f, "#include \"nextreg.h\"\n");
  fprintf(f, "#include \"helpers.h\"\n");
  fprintf(f, "#include \"fmath.h\"\n");
  fprintf(f, "#include \"zxn_test.h\"\n\n");

  /* Aggregate release functions take void* in their declarations so they can
   * be used by functions emitted before a later type definition. */
  for (int i = 0; i < stmts.length; i++) {
    ast_t stmt = stmts.data[i];
    if (stmt->tag == tdef) {
      fprintf(f, "static void __rock_release_" SV_Fmt "(void *);\n",
              SV_Arg(stmt->data.tdef.name.lexeme));
    }
  }
  fprintf(f, "\n");

  generate_forward_defs(g, program);

  for (int i = 0; i < stmts.length; i++) {
    ast_t stmt = stmts.data[i];
    generate_statement(g, stmt);
  }
}
