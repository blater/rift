//-----------------------------------------------------------------------------
//  RIFT GENERATOR
//  MIT License
//  Copyright (c) 2024 Paul Passeron
//-----------------------------------------------------------------------------

#include "internal.h"
#include "components.h"
#include "ownership.h"
#include "profile.h"
#include "semantic_plan.h"
#include "type_info.h"
#include "semantic/resolve.h"
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

void generate_assignement(generator_t *g, ast_t assignment);
void generate_compound(generator_t *g, ast_t comp);
void generate_embed(generator_t *g, ast_t node);
void generate_expression(generator_t *g, ast_t expr);
void generate_fundef(generator_t *g, ast_t fun);
void generate_iter_loop(generator_t *g, ast_t loop);
void generate_method_call(generator_t *g, ast_t node);
void generate_statement(generator_t *g, ast_t stmt);
void generate_sub_as_expression(generator_t *g, ast_t expr);
void generate_tdef(generator_t *g, ast_t tdef_ast);
void write_allocator_code(FILE *f);
int is_builtin_typename(char *name);
const char *allocate_string_tmp(generator_t *g);
int get_literal_string_length(token_t tok);
char *capture_expression(generator_t *g, ast_t expr);
ast_t make_native_fundef(component_interface_spec *entry,
                         method_kind_t method_kind);

// Helper: Flush accumulated pre-statements to destination and reset pre_f
static void flush_pre_f(generator_t *g, FILE *dest);

// Constant to avoid repeated SV_STRING + strlen
static const string_view SV_STRING = {.data = "string", .length = 6};

static int printable_type_is(string_view type, const char *name);
static int printable_type_supported(string_view type);
static const char *printable_c_type(string_view type);

/* Sprite is nominal in Rift but has byte storage and uses the existing byte
 * array helpers in generated C. */
static string_view c_scalar_type_name(string_view rift_type) {
  if (svcmp(rift_type, sv_from_cstr("Sprite")) == 0)
    return sv_from_cstr("byte");
  return rift_type;
}

// Field names emitted into the C struct that backs every `union`.
// The discriminator is `key` (an enum); the payload is `value` (a C union).
// Centralised so a future rename touches one place.
#define UNION_KEY_FIELD   "key"
#define UNION_VALUE_FIELD "value"

// Helper: Capture expression into a temporary buffer and return it
// Caller must free() the returned buffer
char *capture_expression(generator_t *g, ast_t expr) {
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
char *mangle_method(string_view type_name, string_view method_name,
                    int is_array_method) {
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

char *mangle_type_method(string_view type_name, string_view method_name,
                         int arity) {
  int needed = snprintf(NULL, 0, "rift__tm_L%zu_%.*s_L%zu_%.*s_A%d",
                        type_name.length, (int)type_name.length, type_name.data,
                        method_name.length, (int)method_name.length,
                        method_name.data, arity);
  char *buf = allocate_compiler_persistent((size_t)needed + 1);
  snprintf(buf, (size_t)needed + 1, "rift__tm_L%zu_%.*s_L%zu_%.*s_A%d",
           type_name.length, (int)type_name.length, type_name.data,
           method_name.length, (int)method_name.length, method_name.data,
           arity);
  return buf;
}

int is_instance_method_kind(method_kind_t kind) {
  return kind == METHOD_INSTANCE || kind == METHOD_ARRAY_INSTANCE;
}

char *mangle_fundef_method(ast_fundef fundef) {
  if (fundef.emitted_c_name) return fundef.emitted_c_name;
  if (fundef.method_kind == METHOD_TYPE_LEVEL) {
    return mangle_type_method(fundef.type_name.lexeme, fundef.name.lexeme,
                              fundef.args.length);
  }
  return mangle_method(fundef.type_name.lexeme, fundef.name.lexeme,
                       fundef.method_kind == METHOD_ARRAY_INSTANCE);
}

static ast_t make_type_node(const char *type_name) {
  node_t n = {0};
  n.tag = type;
  n.data.type.name.lexeme = sv_from_cstr((char*)type_name);
  return new_ast(n);
}

static ast_t make_manifest_type_node(const char *type_name) {
  size_t length = strlen(type_name);
  int is_array = length >= 2 && type_name[length - 2] == '[' &&
                 type_name[length - 1] == ']';
  char *base = allocate_compiler_persistent(length + 1);
  memcpy(base, type_name, length + 1);
  if (is_array) base[length - 2] = '\0';
  ast_t result = make_type_node(base);
  result->data.type.is_array = is_array;
  return result;
}

ast_t make_native_fundef(component_interface_spec *entry,
                         method_kind_t method_kind) {
  node_t node = {0};
  node.tag = fundef;
  node.data.fundef.name.lexeme = sv_from_cstr(entry->name);
  node.data.fundef.type_name.lexeme =
      sv_from_cstr(entry->owner ? entry->owner : "");
  node.data.fundef.method_kind = method_kind;
  node.data.fundef.ret_type = make_manifest_type_node(entry->return_type);
  node.data.fundef.types = new_ast_array();
  node.data.fundef.args = new_token_array();
  node.data.fundef.component_id = entry->component_id;
  node.data.fundef.emitted_c_name = entry->c_symbol;
  if (method_kind == METHOD_INSTANCE) {
    token_t receiver = {0};
    receiver.lexeme = sv_from_cstr("this");
    token_array_push(&node.data.fundef.args, receiver);
    push_ast_array(&node.data.fundef.types,
                   make_manifest_type_node(entry->owner));
  }
  int parameter_count = component_parameter_count(entry->parameters);
  for (int i = 0; i < parameter_count; i++) {
    char parameter[128];
    if (!component_parameter_at(entry->parameters, i, parameter,
                                sizeof(parameter)))
      continue;
    token_t argument = {0};
    char *argument_name = allocate_compiler_persistent(24);
    snprintf(argument_name, 24, "arg%d", i);
    argument.lexeme = sv_from_cstr(argument_name);
    token_array_push(&node.data.fundef.args, argument);
    push_ast_array(&node.data.fundef.types,
                   make_manifest_type_node(parameter));
  }
  return new_ast(node);
}

static void register_manifest_interfaces(generator_t *g) {
  for (int i = 0; i < g->components->namespace_count; i++) {
    component_namespace_spec *entry = &g->components->namespaces[i];
    push_nt(&g->table, sv_from_cstr(entry->owner), NT_NAMESPACE_TYPE,
            make_manifest_type_node(entry->owner));
  }
  for (int i = 0; i < g->components->interface_count; i++) {
    component_interface_spec *entry = &g->components->interfaces[i];
    if (entry->kind == COMPONENT_OPAQUE) {
      push_nt(&g->table, sv_from_cstr(entry->owner), NT_OPAQUE_TYPE,
              make_manifest_type_node(entry->owner));
      ast_t constructor = make_native_fundef(entry, METHOD_NONE);
      push_nt(&g->table, sv_from_cstr(entry->constructor), NT_FUN,
              constructor);
    } else if (entry->kind == COMPONENT_BUILTIN) {
      ast_t builtin = make_native_fundef(entry, METHOD_NONE);
      push_nt(&g->table, sv_from_cstr(entry->name), NT_FUN, builtin);
    } else if (entry->kind == COMPONENT_INSTANCE_METHOD) {
      ast_t method = make_native_fundef(entry, METHOD_INSTANCE);
      char *key = mangle_method(sv_from_cstr(entry->owner),
                                sv_from_cstr(entry->name), 0);
      push_nt(&g->table, sv_from_cstr(key), NT_FUN, method);
    } else if (entry->kind == COMPONENT_TYPE_METHOD) {
      ast_t method = make_native_fundef(entry, METHOD_TYPE_LEVEL);
      char *key = mangle_type_method(
          sv_from_cstr(entry->owner), sv_from_cstr(entry->name),
          component_parameter_count(entry->parameters));
      push_nt(&g->table, sv_from_cstr(key), NT_FUN, method);
    }
  }
}

static void register_builtin_type(name_table_t *table, const char *name) {
  push_nt(table, sv_from_cstr((char *)name), NT_BUILTIN_TYPE,
          make_type_node(name));
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

// Count top-level user fundefs (non-methods) sharing a name. Used to decide
// whether to mangle a name for arity-based overload dispatch. Only user fundefs
// are counted — builtins are not overloaded in Phase 1.
static int program_user_fundef_count(ast_t program, string_view name) {
  if (program == NULL) return 0;
  int n = 0;
  ast_array_t stmts = program->data.program.prog;
  for (int i = 0; i < stmts.length; i++) {
    ast_t s = stmts.data[i];
    if (s->tag == fundef && s->data.fundef.method_kind == METHOD_NONE &&
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
  int lowered_name = svcmp(name, sv_from_cstr("printf")) == 0 ||
                     svcmp(name, sv_from_cstr("print")) == 0 ||
                     svcmp(name, sv_from_cstr("println")) == 0 ||
                     svcmp(name, sv_from_cstr("concat")) == 0 ||
                     svcmp(name, sv_from_cstr("input")) == 0 ||
                     svcmp(name, sv_from_cstr("putchar")) == 0;
  if (lowered_name) fprintf(f, "rift_user_");
  if (program_user_fundef_count(g->program, name) > 1)
    fprintf(f, SV_Fmt "__%d", SV_Arg(name), argc);
  else
    fprintf(f, SV_Fmt, SV_Arg(name));
}

// Helper: Check if a function is a known operation that's always available.
// Intrinsic concat trees are flattened so every operand is evaluated once, in
// source order, before one final allocation is made.
typedef struct {
  ast_t *data;
  int length;
  int capacity;
} concat_leaf_array;

static int intrinsic_concat(ast_t expr) {
  if (!expr || expr->tag != funcall) return 0;
  ast_funcall call = expr->data.funcall;
  int resolved_user = call.resolved_target &&
                      call.resolved_target->tag == fundef &&
                      call.resolved_target->data.fundef.body != NULL;
  return !resolved_user && call.args.length == 2 &&
         svcmp(call.name.lexeme, sv_from_cstr("concat")) == 0;
}

static void push_concat_leaf(concat_leaf_array *leaves, ast_t leaf) {
  if (leaves->length == leaves->capacity) {
    leaves->capacity = leaves->capacity == 0 ? 4 : leaves->capacity * 2;
    leaves->data = realloc(leaves->data,
                           sizeof(*leaves->data) * (size_t)leaves->capacity);
    if (!leaves->data) {
      fprintf(stderr, "riftc: out of memory while lowering concat\n");
      exit(1);
    }
  }
  leaves->data[leaves->length++] = leaf;
}

static void collect_concat_leaves(concat_leaf_array *leaves, ast_t expr) {
  if (intrinsic_concat(expr)) {
    collect_concat_leaves(leaves, expr->data.funcall.args.data[0]);
    collect_concat_leaves(leaves, expr->data.funcall.args.data[1]);
  } else {
    push_concat_leaf(leaves, expr);
  }
}

static const char *concat_format_function(string_view type) {
  if (svcmp(type, sv_from_cstr("boolean")) == 0 ||
      svcmp(type, sv_from_cstr("bool")) == 0)
    return "rift_format_boolean";
  if (svcmp(type, sv_from_cstr("byte")) == 0)
    return "rift_format_byte";
  if (svcmp(type, sv_from_cstr("word")) == 0)
    return "rift_format_word";
  if (svcmp(type, sv_from_cstr("dword")) == 0)
    return "rift_format_dword";
  if (svcmp(type, sv_from_cstr("float")) == 0)
    return "rift_format_float";
  return "rift_format_int";
}

void emit_concat(generator_t *g, ast_funcall call) {
  concat_leaf_array leaves = {0};
  const char *tmp_var = allocate_string_tmp(g);
  int id = g->str_tmp_counter++;
  int valid = 1;

  collect_concat_leaves(&leaves, call.args.data[0]);
  collect_concat_leaves(&leaves, call.args.data[1]);

  for (int i = 0; i < leaves.length; i++) {
    ast_t leaf = leaves.data[i];
    string_view type = infer_expr_type(leaf, g->table);
    token_t token = token_for_expr(leaf);
    char *text;
    if (!printable_type_supported(type)) {
      error(token.filename, token.line, token.col,
            "concat operand must be string, char, boolean, byte, word, dword, int, or float");
      valid = 0;
      continue;
    }
    text = capture_expression(g, leaf);
    fprintf(g->pre_f, "%s __concat_value_%d_%d = %s;\n",
            printable_c_type(type), id, i, text);
    if (printable_type_is(type, "string")) {
      if (rhs_is_borrower(leaf))
        fprintf(g->pre_f, "__string_retain(__concat_value_%d_%d);\n", id,
                i);
      else if (strncmp(text, "__strtmp_", 9) == 0)
        emit_nullify_tmp(g->pre_f, text);
      fprintf(g->pre_f,
              "size_t __concat_length_%d_%d = "
              "__concat_value_%d_%d.data ? __concat_value_%d_%d.length : 0;\n",
              id, i, id, i, id, i);
    } else if (printable_type_is(type, "char")) {
      fprintf(g->pre_f, "size_t __concat_length_%d_%d = 1;\n", id, i);
    } else {
      int buffer_size = printable_type_is(type, "float") ? 20 : 12;
      fprintf(g->pre_f, "char __concat_buffer_%d_%d[%d];\n", id, i,
              buffer_size);
      fprintf(g->pre_f,
              "size_t __concat_length_%d_%d = %s(__concat_buffer_%d_%d, "
              "__concat_value_%d_%d);\n",
              id, i, concat_format_function(type), id, i, id, i);
    }
    free(text);
  }

  if (!valid) {
    fprintf(g->f, "(string){0}");
    free(leaves.data);
    free((char *)tmp_var);
    return;
  }

  fprintf(g->pre_f, "size_t __concat_total_%d = 0;\n", id);
  for (int i = 0; i < leaves.length; i++)
    fprintf(g->pre_f,
            "__concat_total_%d = __concat_checked_add(__concat_total_%d, "
            "__concat_length_%d_%d);\n",
            id, id, id, i);
  fprintf(g->pre_f, "string %s; __rift_make_longlived_string(&%s, "
                    "__concat_total_%d);\n",
          tmp_var, tmp_var, id);
  fprintf(g->pre_f, "size_t __concat_offset_%d = 0;\n", id);
  for (int i = 0; i < leaves.length; i++) {
    string_view type = infer_expr_type(leaves.data[i], g->table);
    const char *data;
    char data_name[96];
    if (printable_type_is(type, "string"))
      snprintf(data_name, sizeof(data_name), "__concat_value_%d_%d.data", id,
               i);
    else if (printable_type_is(type, "char"))
      snprintf(data_name, sizeof(data_name), "&__concat_value_%d_%d", id, i);
    else
      snprintf(data_name, sizeof(data_name), "__concat_buffer_%d_%d", id, i);
    data = data_name;
    fprintf(g->pre_f,
            "__concat_offset_%d = __concat_append_bytes(%s, "
            "__concat_offset_%d, %s, __concat_length_%d_%d);\n",
            id, tmp_var, id, data, id, i);
    if (printable_type_is(type, "string"))
      fprintf(g->pre_f, "__string_release(__concat_value_%d_%d);\n", id, i);
  }
  fprintf(g->pre_f, "%s.data[__concat_total_%d] = 0;\n", tmp_var, id);
  fprintf(g->f, "%s", tmp_var);
  track_string_tmp(g, tmp_var);
  free(leaves.data);
}

/* Lower the common growth form `slot := concat(slot, ...)` without changing
 * its source semantics. The first value is retained before any later operand
 * runs, every remaining leaf is evaluated once from left to right, and all
 * lengths are checked before the destination can be mutated. The runtime may
 * then reuse only full, writable backing whose references are entirely owned
 * by this assignment; aliases and substring views take the copy-on-write
 * path. */
static int emit_concat_assignment(generator_t *g, string_view target,
                                  ast_t expr) {
  concat_leaf_array leaves = {0};
  int id;

  if (!intrinsic_concat(expr)) return 0;
  collect_concat_leaves(&leaves, expr);
  if (leaves.length < 2 || leaves.data[0]->tag != identifier ||
      svcmp(leaves.data[0]->data.identifier.id.lexeme, target) != 0) {
    free(leaves.data);
    return 0;
  }

  id = g->str_tmp_counter++;
  fprintf(g->pre_f, "string __concat_base_%d = " SV_Fmt ";\n", id,
          SV_Arg(target));
  fprintf(g->pre_f, "__string_retain(__concat_base_%d);\n", id);

  for (int i = 1; i < leaves.length; i++) {
    ast_t leaf = leaves.data[i];
    string_view type = infer_expr_type(leaf, g->table);
    token_t token = token_for_expr(leaf);
    char *text;
    if (!printable_type_supported(type)) {
      error(token.filename, token.line, token.col,
            "concat operand must be string, char, boolean, byte, word, dword, int, or float");
      free(leaves.data);
      return 1;
    }
    text = capture_expression(g, leaf);
    fprintf(g->pre_f, "%s __concat_value_%d_%d = %s;\n",
            printable_c_type(type), id, i, text);
    if (printable_type_is(type, "string")) {
      if (rhs_is_borrower(leaf))
        fprintf(g->pre_f, "__string_retain(__concat_value_%d_%d);\n", id,
                i);
      else if (strncmp(text, "__strtmp_", 9) == 0)
        emit_nullify_tmp(g->pre_f, text);
      fprintf(g->pre_f,
              "size_t __concat_length_%d_%d = "
              "__concat_value_%d_%d.data ? __concat_value_%d_%d.length : 0;\n",
              id, i, id, i, id, i);
    } else if (printable_type_is(type, "char")) {
      fprintf(g->pre_f, "size_t __concat_length_%d_%d = 1;\n", id, i);
    } else {
      int buffer_size = printable_type_is(type, "float") ? 20 : 12;
      fprintf(g->pre_f, "char __concat_buffer_%d_%d[%d];\n", id, i,
              buffer_size);
      fprintf(g->pre_f,
              "size_t __concat_length_%d_%d = %s(__concat_buffer_%d_%d, "
              "__concat_value_%d_%d);\n",
              id, i, concat_format_function(type), id, i, id, i);
    }
    free(text);
  }

  fprintf(g->pre_f, "size_t __concat_total_%d = __concat_base_%d.length;\n",
          id, id);
  for (int i = 1; i < leaves.length; i++)
    fprintf(g->pre_f,
            "__concat_total_%d = __concat_checked_add(__concat_total_%d, "
            "__concat_length_%d_%d);\n",
            id, id, id, i);
  fprintf(g->pre_f, "(void)__concat_total_%d;\n", id);
  flush_pre_f(g, g->f);

  for (int i = 1; i < leaves.length; i++) {
    string_view type = infer_expr_type(leaves.data[i], g->table);
    char data_name[96];
    if (printable_type_is(type, "string"))
      snprintf(data_name, sizeof(data_name), "__concat_value_%d_%d.data", id,
               i);
    else if (printable_type_is(type, "char"))
      snprintf(data_name, sizeof(data_name), "&__concat_value_%d_%d", id, i);
    else
      snprintf(data_name, sizeof(data_name), "__concat_buffer_%d_%d", id, i);
    fprintf(g->f,
            "__concat_append_owned(&__concat_base_%d, %s, "
            "__concat_length_%d_%d, (" SV_Fmt ".backing != NULL && "
            SV_Fmt ".backing == __concat_base_%d.backing && " SV_Fmt
            ".data == __concat_base_%d.data) ? 2u : 1u);\n",
            id, data_name, id, i, SV_Arg(target), SV_Arg(target), id,
            SV_Arg(target), id);
    if (printable_type_is(type, "string"))
      fprintf(g->f, "__string_release(__concat_value_%d_%d);\n", id, i);
  }
  fprintf(g->f, "__string_release(" SV_Fmt ");\n", SV_Arg(target));
  fprintf(g->f, SV_Fmt " = __concat_base_%d;\n", SV_Arg(target), id);
  free(leaves.data);
  return 1;
}

void emit_to_string(generator_t *g, ast_funcall call) {
  // Capture argument, emit setup to pre_f, emit tmp var to main output
  ast_t arg = call.args.data[0];
  const char *fn = "__to_string_int";
  string_view type = infer_expr_type(arg, g->table);
  if (svcmp(type, sv_from_cstr("byte")) == 0) fn = "__to_string_byte";
  else if (svcmp(type, sv_from_cstr("word")) == 0) fn = "__to_string_word";
  else if (svcmp(type, sv_from_cstr("dword")) == 0) fn = "__to_string_dword";
  else if (svcmp(type, sv_from_cstr("float")) == 0) fn = "__to_string_float";
  else if (svcmp(type, sv_from_cstr("boolean")) == 0 ||
           svcmp(type, sv_from_cstr("bool")) == 0)
    fn = "__to_string_boolean";
  else if (svcmp(type, sv_from_cstr("char")) == 0)
    fn = "__to_string_char";
  else if (svcmp(type, sv_from_cstr("string")) == 0)
    fn = "__to_string_string";

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
  /* Emit a real rift_block_header, rather than duplicating its layout. The
   * allocator header differs between host and ZXN builds, so hard-coding the
   * first fields here would make literal strings unsafe on one target. */
  int byte_len = get_literal_string_length(tok) - 1;
  int id = g->lit_counter++;
  fprintf(g->pre_f,
          "static struct { rift_block_header header; char data[%d]; } "
          "__rift_lit_%d = { .header = { .size = %d, .refcount = RIFT_RC_STATIC, "
          ".next_free = 0 }, .data = " SV_Fmt " };\n",
          byte_len + 1, id, byte_len, SV_Arg(tok.lexeme));
  fprintf(g->pre_f,
          "string %s; __rift_make_string(&%s, __rift_lit_%d.data, %d); "
          "%s.backing = &__rift_lit_%d.header;\n",
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

generator_t *new_generator(char *filename, const char *output_base,
                           component_manifest *components,
                           generator_options options) {
  generator_t *res = allocate_compiler_persistent(sizeof(*res));
  *res = (generator_t){0};
  res->f = fopen(filename, "wb");
  if (res->f == NULL)
    perror("Could not open file !");
  res->table = new_name_table();
  res->str_tmp_counter = 0;
  res->target = options.target;
  res->current_module_type = sv_from_cstr("");
  res->in_global_scope = 1;
  res->deferred_module_inits = new_ast_array();
  res->deferred_global_init_code = NULL;
  res->deferred_global_init_count = 0;
  res->deferred_global_init_capacity = 0;
  res->program = NULL;
  res->scope = NULL;
  res->auto_cast = options.auto_cast;
  res->zxn_test = options.zxn_test;
  res->lit_counter = 0;
  res->current_fundef = NULL;
  res->bump_mark_counter = 0;
  res->components = components;
  size_t component_path_size = strlen(output_base) + 12;
  res->component_output_path =
      allocate_compiler_persistent(component_path_size);
  snprintf(res->component_output_path, component_path_size, "%s.components",
           output_base);
  size_t asset_path_size = strlen(output_base) + 12;
  res->asset_asm_output_path =
      allocate_compiler_persistent(asset_path_size);
  snprintf(res->asset_asm_output_path, asset_path_size, "%s.assets.asm",
           output_base);
  res->asset_decls = new_ast_array();
  res->select_all_components = options.select_all_components;
  res->semantic_plan_enabled = options.semantic_plan;

  register_builtin_type(&res->table, "int");
  register_builtin_type(&res->table, "byte");
  register_builtin_type(&res->table, "word");
  register_builtin_type(&res->table, "dword");
  register_builtin_type(&res->table, "char");
  register_builtin_type(&res->table, "string");
  register_builtin_type(&res->table, "bool");
  register_builtin_type(&res->table, "boolean");
  register_builtin_type(&res->table, "float");
  register_builtin_type(&res->table, "void");
  register_builtin_type(&res->table, "Sprite");

  // Register C library builtin functions with their return types
  // Stdlib / I/O
  register_builtin(&res->table, "print",                "void");
  register_builtin(&res->table, "println",              "void");
  // Array operations - handled specially by name lookup in generator
  register_builtin(&res->table, "length",               "int");
  // String operations from fundefs.h
  register_builtin(&res->table, "charAt",               "char");
  register_builtin(&res->table, "setCharAt",            "void");
  register_builtin(&res->table, "equals",               "int");
  register_builtin(&res->table, "string_to_cstr",       "void");
  register_builtin(&res->table, "cstr_to_string",       "string");
  register_builtin(&res->table, "new_string",           "string");
  // Numeric conversions
  register_builtin(&res->table, "to_int",               "int");
  register_builtin(&res->table, "to_byte",              "byte");
  register_builtin(&res->table, "to_word",              "word");
  register_builtin(&res->table, "to_dword",             "dword");
  register_builtin(&res->table, "to_float",             "float");
  register_builtin(&res->table, "set_string_index_base","void");
  // Built-in array/string functions with special code generation
  register_builtin(&res->table, "substring",            "string");
  register_builtin(&res->table, "concat",               "string");
  register_builtin(&res->table, "toString",             "string");
  // Core compiler functions - always available
  register_builtin(&res->table, "exit",                 "void");
  register_builtin(&res->table, "halt",                 "void");
  register_builtin(&res->table, "putchar",              "void");
  register_builtin(&res->table, "putchar_at",           "void");
  register_builtin(&res->table, "putchar_addr",         "void");
  /* Generated startup markers use the compiler-private C-string stage ABI.
   * Rift-visible target-test helpers are declared by the manifest. */
  register_builtin(&res->table, "zxn_test_stage",       "void");
  register_builtin_typed(&res->table, "Sprite",         "Sprite", 1, "byte");

  /* Component-backed built-ins are registered from components.manifest by
   * register_manifest_interfaces(). Keep only compiler-core built-ins here. */
  /* print(x, y, text) — 3-arg overload of `print`, routed to C print_at
   * via a fast-path branch in generate_funcall. Not registered as a
   * separate Rift name; the C symbol lives in src/lib/print_at.c. */

  register_manifest_interfaces(res);

  // Always initialize pre_f buffer for statement splitting
  res->pre_buf = NULL;
  res->pre_buf_size = 0;
  res->pre_f = open_memstream(&res->pre_buf, &res->pre_buf_size);
  if (res->pre_f == NULL)
    perror("Could not open memstream for pre_f!");

  return res;
}

void kill_generator(generator_t *g) {
  // Free any remaining scope nodes (handles error-exit paths)
  while (g->scope) pop_scope(g);
  fclose(g->f);
  // Clean up pre_f buffer if initialized (ZXN target only)
  if (g->pre_f != NULL) {
    fflush(g->pre_f);
    fclose(g->pre_f);
  }
  asset_generator_free(g->assets);
  semantic_plan_destroy(g->semantic_plans);
}

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
    string_view c_type = c_scalar_type_name(elem_type);
    fprintf(f, SV_Fmt "_get_elem(", SV_Arg(c_type));
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
  string_view c_type_name = c_scalar_type_name(type_name);

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
    fprintf(f, SV_Fmt "%s(", SV_Arg(c_type_name), op.suffix);
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

static int printable_type_is(string_view type, const char *name) {
  return svcmp(type, sv_from_cstr((char *)name)) == 0;
}

static int printable_type_supported(string_view type) {
  return printable_type_is(type, "string") || printable_type_is(type, "char") ||
         printable_type_is(type, "boolean") || printable_type_is(type, "bool") ||
         printable_type_is(type, "byte") || printable_type_is(type, "word") ||
         printable_type_is(type, "dword") || printable_type_is(type, "int") ||
         printable_type_is(type, "float");
}

static const char *printable_c_type(string_view type) {
  if (printable_type_is(type, "bool")) return "boolean";
  if (printable_type_is(type, "string")) return "string";
  if (printable_type_is(type, "char")) return "char";
  if (printable_type_is(type, "boolean")) return "boolean";
  if (printable_type_is(type, "byte")) return "byte";
  if (printable_type_is(type, "word")) return "word";
  if (printable_type_is(type, "dword")) return "dword";
  if (printable_type_is(type, "float")) return "float";
  return "int";
}

static void emit_printable_text(FILE *f, string_view type, const char *text) {
  if (printable_type_is(type, "string"))
    fprintf(f, "print(%s)", text);
  else if (printable_type_is(type, "char"))
    fprintf(f, "rift_console_putc((char)(%s))", text);
  else if (printable_type_is(type, "boolean") ||
           printable_type_is(type, "bool"))
    fprintf(f, "rift_print_boolean((boolean)(%s))", text);
  else if (printable_type_is(type, "byte"))
    fprintf(f, "rift_print_byte((byte)(%s))", text);
  else if (printable_type_is(type, "word"))
    fprintf(f, "rift_print_word((word)(%s))", text);
  else if (printable_type_is(type, "dword"))
    fprintf(f, "rift_print_dword((dword)(%s))", text);
  else if (printable_type_is(type, "float"))
    fprintf(f, "rift_print_float((float)(%s))", text);
  else
    fprintf(f, "rift_print_int((int)(%s))", text);
}

static void emit_printable_call(generator_t *g, ast_t value) {
  string_view type = infer_expr_type(value, g->table);
  token_t token = token_for_expr(value);
  if (!printable_type_supported(type)) {
    error(token.filename, token.line, token.col,
          "print value must be string, char, boolean, byte, word, dword, int, or float");
    fprintf(g->f, "(void)0");
    return;
  }
  char *text = capture_expression(g, value);
  if (printable_type_is(type, "string")) {
    int id = g->str_tmp_counter++;
    fprintf(g->pre_f, "string __print_string_%d = %s;\n", id, text);
    if (rhs_is_borrower(value))
      fprintf(g->pre_f, "__string_retain(__print_string_%d);\n", id);
    else if (strncmp(text, "__strtmp_", 9) == 0)
      emit_nullify_tmp(g->pre_f, text);
    fprintf(g->pre_f, "print(__print_string_%d);\n", id);
    fprintf(g->pre_f, "__string_release(__print_string_%d);\n", id);
    fprintf(g->f, "(void)0");
  } else {
    emit_printable_text(g->f, type, text);
  }
  free(text);
}

static void emit_positioned_print(generator_t *g, ast_funcall call) {
  string_view value_type = infer_expr_type(call.args.data[2], g->table);
  token_t value_token = token_for_expr(call.args.data[2]);
  if (!printable_type_supported(value_type)) {
    error(value_token.filename, value_token.line, value_token.col,
          "positioned print value must be string, char, boolean, byte, word, dword, int, or float");
    fprintf(g->f, "(void)0");
    return;
  }

  int id = g->str_tmp_counter++;
  char *x_text = capture_expression(g, call.args.data[0]);
  fprintf(g->pre_f, "byte __print_x_%d = (byte)(%s);\n", id, x_text);
  free(x_text);
  char *y_text = capture_expression(g, call.args.data[1]);
  fprintf(g->pre_f, "byte __print_y_%d = (byte)(%s);\n", id, y_text);
  free(y_text);
  if (call.args.data[2]->tag == literal &&
      call.args.data[2]->data.literal.lit.type == TOK_STR_LIT) {
    token_t literal = call.args.data[2]->data.literal.lit;
    fprintf(g->pre_f,
            "rift_console_set_cursor(__print_x_%d, __print_y_%d);\n", id,
            id);
    fprintf(g->pre_f, "rift_print_bytes(" SV_Fmt ", %d);\n",
            SV_Arg(literal.lexeme), get_literal_string_length(literal) - 1);
    fprintf(g->f, "(void)0");
    return;
  }
  char *value_text = capture_expression(g, call.args.data[2]);
  fprintf(g->pre_f, "%s __print_value_%d = %s;\n",
          printable_c_type(value_type), id, value_text);
  if (printable_type_is(value_type, "string")) {
    if (rhs_is_borrower(call.args.data[2]))
      fprintf(g->pre_f, "__string_retain(__print_value_%d);\n", id);
    else if (strncmp(value_text, "__strtmp_", 9) == 0)
      emit_nullify_tmp(g->pre_f, value_text);
  }
  free(value_text);
  fprintf(g->pre_f, "rift_console_set_cursor(__print_x_%d, __print_y_%d);\n",
          id, id);
  char value_name[64];
  snprintf(value_name, sizeof(value_name), "__print_value_%d", id);
  emit_printable_text(g->pre_f, value_type, value_name);
  fprintf(g->pre_f, ";\n");
  if (printable_type_is(value_type, "string"))
    fprintf(g->pre_f, "__string_release(__print_value_%d);\n", id);
  fprintf(g->f, "(void)0");
}

static void emit_positioned_putchar(generator_t *g, ast_funcall call,
                                    int raw_address) {
  int id = g->str_tmp_counter++;
  char *first_text = capture_expression(g, call.args.data[0]);
  char *value_text;
  if (raw_address)
    fprintf(g->pre_f, "word __putchar_address_%d = (word)(%s);\n", id,
            first_text);
  else
    fprintf(g->pre_f, "byte __putchar_x_%d = (byte)(%s);\n", id,
            first_text);
  free(first_text);
  if (!raw_address) {
    char *y_text = capture_expression(g, call.args.data[1]);
    fprintf(g->pre_f, "byte __putchar_y_%d = (byte)(%s);\n", id, y_text);
    free(y_text);
  }
  value_text = capture_expression(g, call.args.data[raw_address ? 1 : 2]);
  fprintf(g->pre_f, "char __putchar_value_%d = (char)(%s);\n", id,
          value_text);
  free(value_text);
  if (raw_address)
    fprintf(g->pre_f,
            "rift_console_putc_addr(__putchar_address_%d, "
            "__putchar_value_%d);\n",
            id, id);
  else
    fprintf(g->pre_f,
            "rift_console_putc_at(__putchar_x_%d, __putchar_y_%d, "
            "__putchar_value_%d);\n",
            id, id, id);
  fprintf(g->f, "(void)0");
}

void generate_funcall(generator_t *g, ast_t fun) {
  FILE *f = g->f;
  ast_funcall funcall = fun->data.funcall;
  ast_t resolved = funcall.resolved_target;
  int resolved_user = resolved && resolved->tag == fundef &&
                      resolved->data.fundef.body != NULL;
  if (!resolved_user && svcmp(funcall.name.lexeme, sv_from_cstr("Sprite")) == 0) {
    if (funcall.args.length != 1) {
      error(funcall.name.filename, funcall.name.line, funcall.name.col,
            "Sprite() expects exactly one byte slot argument");
      fprintf(f, "(byte)0");
      return;
    }
    fprintf(f, "(byte)(");
    generate_expression(g, funcall.args.data[0]);
    fprintf(f, ")");
  } else if (!resolved_user && svcmp(funcall.name.lexeme, sv_from_cstr("append")) == 0) {
    generate_append(g, funcall);
  } else if (!resolved_user && svcmp(funcall.name.lexeme, sv_from_cstr("get")) == 0) {
    generate_get(g, funcall);
  } else if (!resolved_user && svcmp(funcall.name.lexeme, sv_from_cstr("set")) == 0) {
    generate_set(g, funcall);
  } else if (!resolved_user && svcmp(funcall.name.lexeme, sv_from_cstr("pop")) == 0) {
    generate_pop(g, funcall);
  } else if (!resolved_user && svcmp(funcall.name.lexeme, sv_from_cstr("insert")) == 0) {
    generate_insert(g, funcall);
  } else if (!resolved_user && svcmp(funcall.name.lexeme, sv_from_cstr("substring")) == 0) {
    generate_substring(g, funcall);
  } else if (!resolved_user && svcmp(funcall.name.lexeme, sv_from_cstr("concat")) == 0) {
    generate_concat(g, funcall);
  } else if (!resolved_user && svcmp(funcall.name.lexeme, sv_from_cstr("toString")) == 0) {
    generate_to_string(g, funcall);
  } else if (!resolved_user &&
             svcmp(funcall.name.lexeme, sv_from_cstr("length")) == 0 &&
             funcall.args.length == 1) {
    string_view argument_type = infer_expr_type(funcall.args.data[0], g->table);
    if (svcmp(argument_type, SV_STRING) == 0)
      fprintf(g->f, "__length_string(");
    else
      fprintf(g->f, "__length_array(");
    generate_expression(g, funcall.args.data[0]);
    fprintf(g->f, ")");
  } else if (!resolved_user &&
             svcmp(funcall.name.lexeme, sv_from_cstr("input")) == 0 &&
             funcall.args.length == 0) {
    /* input() produces an owned long-lived string. Put it under the same
     * tracked temporary discipline as substring/concat so discard, return,
     * argument, print, and concat contexts each release exactly once. */
    const char *tmp = allocate_string_tmp(g);
    fprintf(g->pre_f, "string %s = input();\n", tmp);
    fprintf(g->f, "%s", tmp);
    track_string_tmp(g, tmp);
  } else if (!resolved_user && svcmp(funcall.name.lexeme, sv_from_cstr("halt")) == 0) {
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
  } else if (!resolved_user && svcmp(funcall.name.lexeme, sv_from_cstr("print")) == 0 &&
             funcall.args.length == 1 &&
             funcall.args.data[0]->tag == literal &&
             funcall.args.data[0]->data.literal.lit.type == TOK_STR_LIT) {
    /* A literal does not need a Rift string descriptor, static allocation
     * header, refcount assignment, or scope cleanup.  Keep the byte length
     * explicit so this remains equivalent to print(string), including for
     * literals that are not safely represented by a C `%s` conversion. */
    token_t tok = funcall.args.data[0]->data.literal.lit;
    fprintf(g->f, "rift_print_bytes(" SV_Fmt ", %d)",
            SV_Arg(tok.lexeme), get_literal_string_length(tok) - 1);
  } else if (!resolved_user && svcmp(funcall.name.lexeme, sv_from_cstr("print")) == 0 &&
             funcall.args.length == 3) {
    emit_positioned_print(g, funcall);
  } else if (!resolved_user &&
             svcmp(funcall.name.lexeme, sv_from_cstr("print")) == 0 &&
             funcall.args.length == 1) {
    emit_printable_call(g, funcall.args.data[0]);
  } else if (!resolved_user &&
             svcmp(funcall.name.lexeme, sv_from_cstr("println")) == 0) {
    if (funcall.args.length == 0) {
      fprintf(g->f, "rift_console_newline()");
    } else if (funcall.args.length == 1) {
      fprintf(g->f, "(");
      emit_printable_call(g, funcall.args.data[0]);
      fprintf(g->f, ", rift_console_newline())");
    } else {
      error(funcall.name.filename, funcall.name.line, funcall.name.col,
            "println() requires 0 or 1 arguments, got %d", funcall.args.length);
      fprintf(g->f, "(void)0");
    }
  } else if (!resolved_user &&
             svcmp(funcall.name.lexeme, sv_from_cstr("putchar")) == 0 &&
             funcall.args.length == 1) {
    fprintf(g->f, "rift_console_putc((char)(");
    generate_expression(g, funcall.args.data[0]);
    fprintf(g->f, "))");
  } else if (!resolved_user &&
             svcmp(funcall.name.lexeme, sv_from_cstr("putchar_at")) == 0 &&
             funcall.args.length == 3) {
    emit_positioned_putchar(g, funcall, 0);
  } else if (!resolved_user &&
             svcmp(funcall.name.lexeme, sv_from_cstr("putchar_addr")) == 0 &&
             funcall.args.length == 2) {
    emit_positioned_putchar(g, funcall, 1);
  } else if (!resolved_user && svcmp(funcall.name.lexeme, sv_from_cstr("sleep")) == 0) {
    // Rift `sleep` → C `rift_sleep` to avoid POSIX unistd.h collision.
    FILE *f = g->f;
    fprintf(f, "rift_sleep(");
    for (int i = 0; i < funcall.args.length; i++) {
      if (i > 0) fprintf(f, ", ");
      generate_expression(g, funcall.args.data[i]);
    }
    fprintf(f, ")");
  } else {
    // Check if function is defined (either in name table or as a special builtin)
    string_view fname = funcall.name.lexeme;
    ast_t func_ref = resolved ? resolved : get_ref(fname, g->table);

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
      int semantic_selected =
          semantic_plan_selects(g->semantic_plans, func_ref);
      const char *semantic_symbol =
          semantic_plan_function_symbol(g->semantic_plans, func_ref);
      char (*semantic_arguments)[64] = NULL;
      semantic_plan_parameter_abi *semantic_parameters = NULL;
      size_t semantic_parameter_count = 0;
      if (semantic_selected && semantic_symbol == NULL) {
        error(funcall.name.filename, funcall.name.line, funcall.name.col,
              "selected call is missing its sealed function ABI");
        fprintf(f, "rift_plan_invalid_call");
        return;
      }
      if (semantic_symbol &&
          (!semantic_plan_parameter_count(g->semantic_plans, func_ref,
                                          &semantic_parameter_count) ||
           funcall.args.length < 0 ||
           semantic_parameter_count != (size_t)funcall.args.length)) {
        error(funcall.name.filename, funcall.name.line, funcall.name.col,
              "selected call is missing its sealed parameter ABI");
        fprintf(f, "rift_plan_invalid_call");
        return;
      }
      if (semantic_symbol && semantic_parameter_count > 0) {
        semantic_arguments = calloc(semantic_parameter_count,
                                    sizeof(*semantic_arguments));
        semantic_parameters =
            calloc(semantic_parameter_count, sizeof(*semantic_parameters));
        if (semantic_arguments == NULL || semantic_parameters == NULL) {
          free(semantic_arguments);
          free(semantic_parameters);
          error(funcall.name.filename, funcall.name.line, funcall.name.col,
                "could not allocate semantic-plan call arguments");
          fprintf(f, "rift_plan_invalid_call");
          return;
        }
        for (size_t i = 0; i < semantic_parameter_count; i++) {
          if (!semantic_plan_parameter_at(g->semantic_plans, func_ref, i,
                                          &semantic_parameters[i]) ||
              semantic_parameters[i].c_type == NULL ||
              (semantic_parameters[i].kind !=
                   SEMANTIC_PLAN_PARAMETER_BOOL_SCALAR &&
               semantic_parameters[i].kind !=
                   SEMANTIC_PLAN_PARAMETER_STRING_CONSUME)) {
            free(semantic_arguments);
            free(semantic_parameters);
            error(funcall.name.filename, funcall.name.line, funcall.name.col,
                  "selected call parameter has an unsupported sealed ABI");
            fprintf(f, "rift_plan_invalid_call");
            return;
          }
        }
        for (size_t i = 0; i < semantic_parameter_count; i++) {
          semantic_plan_parameter_abi parameter = semantic_parameters[i];
          char *argument = capture_expression(g, funcall.args.data[i]);
          snprintf(semantic_arguments[i], sizeof(*semantic_arguments),
                   "rift_plan_legacy_arg_%d", g->str_tmp_counter++);
          fprintf(g->pre_f, "%s %s = %s;\n", parameter.c_type,
                  semantic_arguments[i], argument);
          if (parameter.kind == SEMANTIC_PLAN_PARAMETER_STRING_CONSUME) {
            if (rhs_is_borrower(funcall.args.data[i])) {
              fprintf(g->pre_f, "__string_retain(%s);\n",
                      semantic_arguments[i]);
            } else if (strncmp(argument, "__strtmp_", 9) == 0) {
              emit_nullify_tmp(g->pre_f, argument);
            }
          }
          free(argument);
        }
      }
      if (semantic_symbol)
        fprintf(f, "%s", semantic_symbol);
      else if (func_ref->tag == fundef && func_ref->data.fundef.emitted_c_name)
        fprintf(f, "%s", func_ref->data.fundef.emitted_c_name);
      else
        emit_fun_name(f, g, funcall.name.lexeme, funcall.args.length);
      fprintf(f, "(");
      ast_array_t param_types = func_ref->data.fundef.types;
      int user_function = func_ref->data.fundef.body != NULL;
      /* User functions consume owned values. This retain is deliberately at
       * the call site: aliases keep their source share; constructor, pop, and
       * subscript results transfer without an otherwise-ownerless reference. */
        for (int i = 0; i < funcall.args.length && i < param_types.length; i++) {
      if (user_function && !semantic_symbol) {
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
        if (!semantic_symbol && g->auto_cast && i < param_types.length &&
            param_types.data[i] && param_types.data[i]->tag == type) {
          string_view ptn = param_types.data[i]->data.type.name.lexeme;
          if (svcmp(ptn, sv_from_cstr("byte"))  == 0 ||
              svcmp(ptn, sv_from_cstr("word"))  == 0 ||
              svcmp(ptn, sv_from_cstr("dword")) == 0) {
            cast = ptn;
          }
        }
        if (cast.length > 0) fprintf(f, "(" SV_Fmt ")(", SV_Arg(cast));
        if (cast.length > 0) fprintf(f, ")");
        if (semantic_symbol) {
          fprintf(f, "%s", semantic_arguments[i]);
        } else {
          generate_expression(g, funcall.args.data[i]);
        }
      }
      fprintf(f, ")");
      free(semantic_arguments);
      free(semantic_parameters);
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
    fprintf(f, "static void __rift_release_array_elem_%s(void *slot) {\n", type_name);
    fprintf(f, "  __rift_release_%s(*(%s *)slot);\n", type_name, type_name);
    fprintf(f, "}\n\n");
  }

  // Template group: make_array, push_array, pop_array, get_elem, set_elem, insert
  // Each generates a wrapper around __internal_* functions with proper type handling

  fprintf(f, "__internal_dynamic_array_t %s_make_array(void) {\n", type_name);
  if (owns_handles) {
    fprintf(f, "  return __internal_make_array_with_release(sizeof(%s), 0, ", type_name);
    fprintf(f, "__rift_release_array_elem_%s);\n", type_name);
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
  fprintf(f, "  if (res == NULL) {\n");
  fprintf(f, "    rift_error_text(\"NULL ELEMENT IN %s_get_elem\");\n",
          type_name);
  fprintf(f, "    rift_error_newline();\n");
  fprintf(f, "    exit(1);\n");
  fprintf(f, "  }\n");
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
    fprintf(f, "  if (index < arr->length) {\n");
    fprintf(f, "    %s *old = __internal_get_elem(arr, index);\n", type_name);
    fprintf(f, "    __rift_release_%s(*old);\n", type_name);
    fprintf(f, "  }\n");
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
  if (!mc.is_resolved || mc.resolved_target == NULL || mc.resolved_c_name == NULL) {
    error(mc.method.filename, mc.method.line, mc.method.col,
          "unresolved method call '" SV_Fmt "'",
          SV_Arg(mc.method.lexeme));
    return;
  }
  string_view recv_type = mc.resolved_owner;
  ast_t method_ref = mc.resolved_target;
  int receiver_is_array = mc.resolved_kind == METHOD_ARRAY_INSTANCE;
  int native_method = method_ref->tag == fundef &&
                      method_ref->data.fundef.body == NULL;
  char receiver_tmp[64] = {0};
  char argument_tmps[COMPONENT_MANIFEST_MAX_PARAMS][64] = {{0}};
  ast_type argument_tmp_types[COMPONENT_MANIFEST_MAX_PARAMS] = {{0}};
  int cleanup_count = 0;
  int cleanup_receiver = 0;
  if (native_method && mc.resolved_kind != METHOD_TYPE_LEVEL &&
      !rhs_is_borrower(mc.receiver) &&
      (receiver_is_array || is_heap_allocated_type(recv_type, g->table))) {
    snprintf(receiver_tmp, sizeof(receiver_tmp), "__native_recv_%d",
             g->str_tmp_counter++);
    char *receiver = capture_expression(g, mc.receiver);
    if (receiver_is_array)
      fprintf(g->pre_f, "__internal_dynamic_array_t %s = %s;\n",
              receiver_tmp, receiver);
    else
      fprintf(g->pre_f, SV_Fmt " %s = %s;\n", SV_Arg(recv_type),
              receiver_tmp, receiver);
    free(receiver);
    cleanup_receiver = 1;
    cleanup_count++;
  }
  ast_array_t native_params = method_ref->data.fundef.types;
  int native_offset = mc.resolved_kind == METHOD_TYPE_LEVEL ? 0 : 1;
  if (native_method) {
    for (int i = 0; i < mc.args.length && i < COMPONENT_MANIFEST_MAX_PARAMS;
         i++) {
      if (i + native_offset >= native_params.length ||
          !native_params.data[i + native_offset] ||
          native_params.data[i + native_offset]->tag != type ||
          rhs_is_borrower(mc.args.data[i]))
        continue;
      ast_type param = native_params.data[i + native_offset]->data.type;
      if (!param.is_array && svcmp(param.name.lexeme, SV_STRING) != 0 &&
          !is_heap_allocated_type(param.name.lexeme, g->table))
        continue;
      snprintf(argument_tmps[i], sizeof(argument_tmps[i]), "__native_arg_%d",
               g->str_tmp_counter++);
      char *argument = capture_expression(g, mc.args.data[i]);
      generate_type(g->pre_f, native_params.data[i + native_offset]);
      fprintf(g->pre_f, " %s = %s;\n", argument_tmps[i], argument);
      if (svcmp(param.name.lexeme, SV_STRING) == 0 &&
          strncmp(argument, "__strtmp_", 9) == 0)
        emit_nullify_tmp(g->pre_f, argument);
      free(argument);
      argument_tmp_types[i] = param;
      cleanup_count++;
    }
  }
  if (mc.resolved_kind != METHOD_TYPE_LEVEL &&
      method_ref && method_ref->tag == fundef &&
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
    int param_offset = mc.resolved_kind == METHOD_TYPE_LEVEL ? 0 : 1;
    for (int i = 0; i < mc.args.length && i + param_offset < params.length; i++) {
      if (params.data[i + param_offset] &&
          params.data[i + param_offset]->tag == type) {
        ast_type param = params.data[i + param_offset]->data.type;
        if (param.is_array || is_heap_allocated_type(param.name.lexeme, g->table))
          emit_borrowed_container_retain(g, mc.args.data[i], param);
      }
    }
  }
  ast_type native_return = method_ref->data.fundef.ret_type->data.type;
  int native_returns_void = svcmp(native_return.name.lexeme,
                                  sv_from_cstr("void")) == 0;
  char result_tmp[64] = {0};
  if (cleanup_count) {
    fprintf(f, "(");
    if (!native_returns_void) {
      snprintf(result_tmp, sizeof(result_tmp), "__native_result_%d",
               g->str_tmp_counter++);
      generate_type(g->pre_f, method_ref->data.fundef.ret_type);
      fprintf(g->pre_f, " %s;\n", result_tmp);
      fprintf(f, "%s = ", result_tmp);
    }
  }
  fprintf(f, "%s(", mc.resolved_c_name);
  if (mc.resolved_kind != METHOD_TYPE_LEVEL) {
    if (receiver_tmp[0]) fprintf(f, "%s", receiver_tmp);
    else generate_expression(g, mc.receiver);
  }
  ast_array_t param_types = method_ref->data.fundef.types;
  int param_offset = mc.resolved_kind == METHOD_TYPE_LEVEL ? 0 : 1;
  for (int i = 0; i < mc.args.length; i++) {
    if (i > 0 || mc.resolved_kind != METHOD_TYPE_LEVEL) fprintf(f, ", ");
    int asset_argument = mc.resolved_kind == METHOD_INSTANCE && i == 0 &&
                         svcmp(mc.resolved_owner, sv_from_cstr("Sprite")) == 0 &&
                         svcmp(mc.method.lexeme, sv_from_cstr("frame")) == 0;
    if (asset_argument) {
      ast_t declaration = mc.resolved_asset;
      int base = asset_generator_base(g->assets, declaration);
      if (base < 0) {
        token_t tok = token_for_expr(mc.args.data[i]);
        error(tok.filename, tok.line, tok.col,
              "unresolved SpritePattern asset during lowering");
        fprintf(f, "0");
      } else {
        fprintf(f, "%d", base);
      }
      continue;
    }
    if (g->auto_cast && i + param_offset < param_types.length &&
        param_types.data[i + param_offset] &&
        param_types.data[i + param_offset]->tag == type) {
      string_view parameter_name =
          param_types.data[i + param_offset]->data.type.name.lexeme;
      if (svcmp(parameter_name, sv_from_cstr("byte")) == 0 ||
          svcmp(parameter_name, sv_from_cstr("word")) == 0 ||
          svcmp(parameter_name, sv_from_cstr("dword")) == 0)
        fprintf(f, "(" SV_Fmt ")(", SV_Arg(parameter_name));
      else
        parameter_name = sv_from_cstr("");
      if (argument_tmps[i][0]) fprintf(f, "%s", argument_tmps[i]);
      else generate_expression(g, mc.args.data[i]);
      if (parameter_name.length > 0) fprintf(f, ")");
      continue;
    }
    if (argument_tmps[i][0]) fprintf(f, "%s", argument_tmps[i]);
    else generate_expression(g, mc.args.data[i]);
  }
  fprintf(f, ")");
  if (cleanup_count) {
    if (cleanup_receiver) {
      if (receiver_is_array)
        fprintf(f, ", __internal_free_array(%s, 0)", receiver_tmp);
      else
        fprintf(f, ", __rift_release_" SV_Fmt "(%s)", SV_Arg(recv_type),
                receiver_tmp);
    }
    for (int i = 0; i < mc.args.length && i < COMPONENT_MANIFEST_MAX_PARAMS;
         i++) {
      if (!argument_tmps[i][0]) continue;
      ast_type param = argument_tmp_types[i];
      if (param.is_array)
        fprintf(f, ", __internal_free_array(%s, %d)", argument_tmps[i],
                svcmp(param.name.lexeme, SV_STRING) == 0);
      else if (svcmp(param.name.lexeme, SV_STRING) == 0)
        fprintf(f, ", __string_release(%s)", argument_tmps[i]);
      else
        fprintf(f, ", __rift_release_" SV_Fmt "(%s)",
                SV_Arg(param.name.lexeme), argument_tmps[i]);
    }
    if (!native_returns_void) fprintf(f, ", %s", result_tmp);
    fprintf(f, ")");
  }
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

  string_view element_type = get_array_element_type(
      iter_loop.iterable, g->table, iter_loop.variable);
  string_view c_element_type = c_scalar_type_name(element_type);
  fprintf(f, SV_Fmt " " SV_Fmt " = ((", SV_Arg(c_element_type),
          SV_Arg(iter_loop.variable.lexeme));
  fprintf(f, SV_Fmt " *)", SV_Arg(c_element_type));
  generate_expression(g, iter_loop.iterable);
  fprintf(f, "->data)[__iter_i];\n");

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
      nt_lookup_t lexical_value =
          lookup_nt_by_kind(lexeme, NT_VAR, g->table);
      ast_t mod_ref = get_ref_by_kind(g->current_module_type, NT_USER_TYPE,
                                      g->table);
      if ((!lexical_value.found || lexical_value.scope == 0) && mod_ref &&
          mod_ref->tag == tdef && mod_ref->data.tdef.t == TDEF_MODULE) {
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
    string_view c_elem_type = c_scalar_type_name(elem_type);
    fprintf(f, SV_Fmt "_set_elem(", SV_Arg(c_elem_type));
    generate_expression(g, sub.array);
    fprintf(f, ", (size_t)(");
    generate_expression(g, sub.index);
    fprintf(f, "), ");
    generate_expression(g, assign.expr);
    fprintf(f, ")");
  } else {
    /* An array variable owns one reference. Retain a borrowed RHS before
     * releasing the old destination so aliases remain live; producer results
     * transfer their existing reference into the destination. */
    ast_type target_array_type = {0};
    if (assign.target->tag == identifier &&
        get_identifier_array_type(
            assign.target->data.identifier.id.lexeme, g->table,
            &target_array_type)) {
      char *rhs_text = capture_expression(g, assign.expr);
      char *target_text = capture_expression(g, assign.target);
      flush_pre_f(g, f);
      if (strcmp(rhs_text, target_text) != 0) {
        if (rhs_is_borrower(assign.expr)) {
          fprintf(f, "__internal_retain_array(%s);\n", rhs_text);
        }
        fprintf(f, "__internal_free_array(%s, %d);\n", target_text,
                svcmp(target_array_type.name.lexeme, SV_STRING) == 0);
        fprintf(f, "%s = %s;\n", target_text, rhs_text);
      }
      free(rhs_text);
      free(target_text);
      return;
    }
    // For string reassignment, free the old value before overwriting
    if (assign.target->tag == identifier &&
        is_scalar_string_var(assign.target->data.identifier.id.lexeme, g->table)) {
      {
        if (emit_concat_assignment(
                g, assign.target->data.identifier.id.lexeme, assign.expr))
          return;
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
      fprintf(f, "__rift_release_" SV_Fmt "(" SV_Fmt ");\n",
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
        char *rhs_text;
        if (assign.expr->tag == literal &&
            assign.expr->data.literal.lit.type == TOK_ARR_DECL) {
          string_view c_element_type =
              c_scalar_type_name(array_element_type);
          rhs_text = malloc(c_element_type.length + 48);
          snprintf(rhs_text, c_element_type.length + 48,
                   "__internal_make_array(sizeof(" SV_Fmt "), 0)",
                   SV_Arg(c_element_type));
        } else {
          rhs_text = capture_expression(g, assign.expr);
        }
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
      fprintf(f, "__rift_release_" SV_Fmt "(%s);\n",
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
  if (lookup_nt_by_kind(type_name, NT_OPAQUE_TYPE, table).found) return 1;
  ast_t ref = get_ref_by_kind(type_name, NT_USER_TYPE, table);
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
        string_view c_type = c_scalar_type_name(type_name);
        fprintf(code_f, SV_Fmt " = __internal_make_array(sizeof(" SV_Fmt "), %d);\n",
                SV_Arg(vardef.name.lexeme), SV_Arg(c_type), capacity);
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
        string_view c_type = c_scalar_type_name(type_name);
        fprintf(f, "__internal_make_array(sizeof(" SV_Fmt "), %d);\n",
                SV_Arg(c_type), capacity);
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
    ast_t struct_ref = get_ref_by_kind(type_name, NT_USER_TYPE, g->table);

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
            string_view c_field_type = c_scalar_type_name(field_element_type);
            snprintf(field_bufs[i], 256, "__internal_make_array(sizeof(" SV_Fmt "), 0)",
                     SV_Arg(c_field_type));
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
     * rift_longlived_alloc; the header is at handle - sizeof(rift_block_header). */
    generate_type(f, vardef.type);
    fprintf(f, " " SV_Fmt " = rift_longlived_alloc(sizeof(struct ",
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
    ast_t type_ref = get_ref_by_kind(type, NT_USER_TYPE, g->table);
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
    fprintf(f, "__rift_release_" SV_Fmt "(__match_tmp);\n", SV_Arg(type));
  } else if (!rhs_is_borrower(m.expr) && is_string && !string_temp_scrutinee) {
    fprintf(f, "__string_release(__match_tmp);\n");
  }
  fprintf(f, "}\n");
}

void generate_return(generator_t *g, ast_t ret_ast) {
  FILE *f = g->f;
  if (g->in_main_body) {
    g->main_needs_epilogue = 1;
    if (ret_ast->data.ret.expr != NULL) {
      char *discard = capture_expression(g, ret_ast->data.ret.expr);
      flush_pre_f(g, f);
      fprintf(f, "(void)(%s);\n", discard);
      free(discard);
    }
    emit_return_cleanup(g, sv_from_cstr(""));
    fprintf(f, "goto rift_main_epilogue;\n");
    return;
  }
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
      char source[32];
      int explicit_producer_release =
          !rhs_is_borrower(ret_ast->data.ret.expr) &&
          strncmp(expr_text, "__strtmp_", 9) != 0;
      snprintf(retval, sizeof(retval), "__retval_%d", g->str_tmp_counter++);
      if (explicit_producer_release) {
        snprintf(source, sizeof(source), "__retsrc_%d", g->str_tmp_counter++);
        fprintf(f, "string %s = %s;\n", source, expr_text);
        fprintf(f, "string %s = __return_string(%s);\n", retval, source);
        fprintf(f, "__string_release(%s);\n", source);
      } else {
        fprintf(f, "string %s = __return_string(%s);\n", retval, expr_text);
      }
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
  if (stmt->tag == asset_decl) {
    /* Compile-time-only declaration: consumed by the asset generator. */
    return;
  } else if (stmt->tag == vardef) {
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
    fprintf(f, "rift_collect();\n");
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
      fprintf(f, "__rift_release_" SV_Fmt "(%s);\n", SV_Arg(value_type), discard_tmp);
    } else if (!rhs_is_borrower(stmt) && svcmp(value_type, SV_STRING) == 0 &&
               strncmp(expr_text, "__strtmp_", 9) != 0) {
      char discard_tmp[64];
      snprintf(discard_tmp, sizeof(discard_tmp), "__discardtmp_%d",
               g->str_tmp_counter++);
      fprintf(f, "string %s = %s;\n", discard_tmp, expr_text);
      fprintf(f, "__string_release(%s);\n", discard_tmp);
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
  fprintf(f, "rift_bump_mark __bm_%d = rift_bump_save();\n", bump_mark_id);
  ast_compound compound = comp->data.compound;
  new_nt_scope(&g->table);
  push_scope(g, bump_mark_id);
  for (int i = 0; i < compound.stmts.length; i++)
    generate_statement(g, compound.stmts.data[i]);
  emit_scope_cleanup(g);
  pop_scope(g);
  end_nt_scope(&g->table);
  fprintf(f, "rift_bump_restore(__bm_%d);\n", bump_mark_id);
  fprintf(f, "}");
}

/* Same structure as generate_compound. User-function parameters consume an
 * owned reference: callers retain only borrowed arguments, while a produced
 * result moves directly into the parameter slot. Tracking hands that reference
 * to the normal scope/return cleanup. */
void generate_function_body(generator_t *g, ast_t fun) {
  FILE *f = g->f;
  ast_fundef fundef = fun->data.fundef;
  if (g->semantic_plan_enabled && g->semantic_plans != NULL &&
      semantic_plan_selects(g->semantic_plans, fun)) {
    error(fundef.name.filename, fundef.name.line, fundef.name.col,
          "internal semantic-plan error: selected body entered legacy lowering");
    return;
  }
  int bump_mark_id = g->bump_mark_counter++;
  fprintf(f, "{");
  fprintf(f, "rift_bump_mark __bm_%d = rift_bump_save();\n", bump_mark_id);
  ast_compound compound = fundef.body->data.compound;
  new_nt_scope(&g->table);
  push_scope(g, bump_mark_id);

  /* Make the enclosing fundef visible to generate_return so it can emit
   * the correct return-value temp type. Saved/restored to support nested
   * function definitions cleanly (defensive — Rift currently doesn't
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
  fprintf(f, "rift_bump_restore(__bm_%d);\n", bump_mark_id);
  fprintf(f, "}");
}

void generate_fundef(generator_t *g, ast_t fun) {
  FILE *f = g->f;
  ast_fundef fundef = fun->data.fundef;
  new_nt_scope(&g->table);

  if (g->semantic_plan_enabled &&
      semantic_plan_selects(g->semantic_plans, fun)) {
    semantic_plan_diagnostic diagnostic;
    if (!semantic_plan_emit_signature(g->semantic_plans, fun, f,
                                      &diagnostic)) {
      error(fundef.name.filename, fundef.name.line, fundef.name.col,
            "semantic-plan signature emission failed: %s",
            diagnostic.message ? diagnostic.message : "unknown error");
    }
    fprintf(f, "\n");
    if (!semantic_plan_emit_body(g->semantic_plans, fun, f, &diagnostic)) {
      error(fundef.name.filename, fundef.name.line, fundef.name.col,
            "semantic-plan body emission failed: %s",
            diagnostic.message ? diagnostic.message : "unknown error");
    }
    fprintf(f, "\n\n");
    end_nt_scope(&g->table);
    return;
  }

  string_view emit_name;
  if (fundef.method_kind != METHOD_NONE)
    emit_name = sv_from_cstr(mangle_fundef_method(fundef));
  else
    emit_name = fundef.name.lexeme;

  if (fundef.method_kind != METHOD_NONE ||
      svcmp(fundef.name.lexeme, sv_from_cstr("main")) != 0) {
    generate_type(f, fundef.ret_type);
    fprintf(f, " ");
    if (fundef.method_kind != METHOD_NONE)
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
    if (is_instance_method_kind(fundef.method_kind))
      g->current_module_type = fundef.type_name.lexeme;
    int saved_global = g->in_global_scope;
    g->in_global_scope = 0;
    if (g->semantic_plan_enabled) {
      semantic_plan_diagnostic diagnostic;
      if (!semantic_plan_emit_body(g->semantic_plans, fun, f, &diagnostic)) {
        error(fundef.name.filename, fundef.name.line, fundef.name.col,
              "semantic-plan emission failed: %s",
              diagnostic.message ? diagnostic.message : "unknown error");
      }
    } else {
      generate_function_body(g, fun);
    }
    g->in_global_scope = saved_global;
    g->current_module_type = saved_module_type;
    fprintf(f, "\n\n");
  } else {
    fprintf(f, "int main(int argc, char **argv) {\n");
    if (g->zxn_test)
      fprintf(f, "zxn_test_begin();\n");
    if (g->zxn_test)
      fprintf(f, "zxn_test_stage(\"pools\");\n");
    fprintf(f, "rift_pools_init(NULL);\n");
    if (g->target == TARGET_ZXN) {
      int arrays = component_index(g, "arrays");
      if (arrays >= 0 && g->closed_components[arrays])
        fprintf(f, "__internal_set_dynamic_array_initial_capacity(8);\n");
    }
    if (g->zxn_test)
      fprintf(f, "zxn_test_stage(\"arguments\");\n");
    int process_args = component_index(g, "process_args");
    if (process_args >= 0 && g->closed_components[process_args])
      fprintf(f, "fill_cmd_args(argc, argv);\n");
    else
      fprintf(f, "(void)argc; (void)argv;\n");
    if (g->zxn_test)
      fprintf(f, "zxn_test_stage(\"rtl\");\n");
    generator_emit_component_init(g);
    asset_generator_emit_init(g->assets, f, g->target == TARGET_ZXN);
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
    g->in_main_body = 1;
    generate_compound(g, fundef.body);
    g->in_main_body = 0;
    g->in_global_scope = saved_global;
    if (g->main_needs_epilogue) fprintf(f, "rift_main_epilogue:\n");
    if (g->zxn_test)
      fprintf(f, "zxn_test_finish();\n");
    generator_emit_component_shutdown(g);
    fprintf(f, "rift_pools_deinit();\n");
    fprintf(f, "return 0;\n");
    fprintf(f, "}\n\n");
  }
  end_nt_scope(&g->table);
}

int is_builtin_typename(char *name) {
  if (strcmp(name, "Sprite") == 0)
    return 1;
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
    if (stmt->tag == tdef) {
      if (is_asset_only_module(g, stmt)) continue;
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
      if (fundef.method_kind != METHOD_NONE ||
          svcmp(fundef.name.lexeme, sv_from_cstr("main")) != 0) {
        if (g->semantic_plan_enabled &&
            semantic_plan_selects(g->semantic_plans, stmt)) {
          semantic_plan_diagnostic diagnostic;
          if (!semantic_plan_emit_signature(g->semantic_plans, stmt, f,
                                            &diagnostic)) {
            error(fundef.name.filename, fundef.name.line, fundef.name.col,
                  "semantic-plan signature emission failed: %s",
                  diagnostic.message ? diagnostic.message : "unknown error");
          }
          fprintf(f, ";\n");
          if (!semantic_plan_emit_body_signature(g->semantic_plans, stmt, f,
                                                 &diagnostic)) {
            error(fundef.name.filename, fundef.name.line, fundef.name.col,
                  "semantic-plan body signature emission failed: %s",
                  diagnostic.message ? diagnostic.message : "unknown error");
          }
          fprintf(f, ";\n\n");
          continue;
        }
        generate_type(f, fundef.ret_type);

        if (fundef.method_kind != METHOD_NONE) {
          fprintf(f, " %s(", mangle_fundef_method(fundef));
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
      if (is_asset_only_module(g, stmt)) continue;
      struct ast_tdef tdef = stmt->data.tdef;
      char *name = string_of_sv(tdef.name.lexeme);
      generate_array_funcs(g, name);
    }
  }
  for (int i = 0; i < g->components->interface_count; i++) {
    component_interface_spec *entry = &g->components->interfaces[i];
    if (entry->kind == COMPONENT_OPAQUE && g->opaque_array_used[i])
      generate_array_funcs(g, entry->owner);
  }
}

void generate_tdef(generator_t *g, ast_t tdef_ast) {
  FILE *f = g->f;
  struct ast_tdef tdef = tdef_ast->data.tdef;
  string_view name = tdef.name.lexeme;

  if (is_asset_only_module(g, tdef_ast)) return;

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
    fprintf(f, "  " SV_Fmt " __inst = (" SV_Fmt ")rift_longlived_alloc(sizeof(struct " SV_Fmt "));\n",
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
      fprintf(f, "  " SV_Fmt " __inst = rift_longlived_alloc(sizeof(struct " SV_Fmt "));\n",
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

  semantic_prepare_program(g, program);
  if (get_error_count() > 0) return;
  if (g->semantic_plan_enabled) {
    semantic_plan_diagnostic diagnostic;
    g->semantic_plans = semantic_plan_prepare(program, &diagnostic);
    if (g->semantic_plans == NULL) {
      token_t location = diagnostic.function
                             ? token_for_expr(diagnostic.function)
                             : token_for_expr(program);
      error(location.filename, location.line, location.col,
            "semantic-plan preflight failed: %s",
            diagnostic.message ? diagnostic.message : "unknown error");
      return;
    }
  }
  g->assets = asset_generator_plan(g->asset_decls);
  if (get_error_count() > 0) return;
  generator_profile_analysis tiny = generator_analyse_profile(g->target, program);
  g->zxn_tiny_eligible = tiny.eligible;
  g->zxn_tiny_uses_stdout = tiny.uses_stdout;
  g->zxn_tiny_simple_stdout = tiny.simple_stdout;
  g->zxn_light_core_eligible = g->target == TARGET_ZXN;
  g->zxn_bump_required = g->select_all_components;
  generator_collect_component_uses(g, program);
  if (g->zxn_test) record_component(g, "tiny_test");
  if (tiny.eligible) {
    int core = component_index(g, "core");
    if (core >= 0) g->direct_components[core] = 0;
    if (tiny.uses_stdout) record_component(g, "tiny_print");
  }
  generator_compute_component_closure(g);
  int closure_startup31_safe = 1;
  g->zxn_pools_required = 0;
  for (int i = 0; i < g->components->component_count; i++) {
    if (g->closed_components[i]) {
      if (!g->components->components[i].zxn_startup31_safe)
        closure_startup31_safe = 0;
      if (g->components->components[i].zxn_pools_required)
        g->zxn_pools_required = 1;
    }
  }
  if (tiny.eligible && !closure_startup31_safe) {
    tiny.eligible = 0;
    g->zxn_tiny_eligible = 0;
    g->zxn_light_core_eligible = 0;
  } else if (!tiny.eligible && g->zxn_light_core_eligible) {
    g->zxn_light_core_eligible = closure_startup31_safe;
  }
  generator_write_component_output(g);
  asset_generator_emit_asm(g->assets, g->asset_asm_output_path,
                           g->target == TARGET_ZXN);

  if (tiny.eligible) {
    if (!tiny.uses_stdout)
      fprintf(f, "/* RIFT_PROFILE:ZXN_TINY_CORE:STARTUP=31 */\n");
    else if (tiny.simple_stdout)
      fprintf(f, "/* RIFT_PROFILE:ZXN_TINY_CORE:STARTUP=31 */\n");
    else
      fprintf(f, "/* RIFT_PROFILE:ZXN_TINY_CORE:STARTUP=31:LIGHT_CONSOLE */\n");
  } else if (g->zxn_light_core_eligible) {
    fprintf(f, "/* RIFT_PROFILE:ZXN_LIGHT_CORE:STARTUP=31 */\n");
  }

  fprintf(f, "#include <stdlib.h>\n");
  if (g->target == TARGET_ZXN && !tiny.eligible &&
      !g->zxn_light_core_eligible)
    fprintf(f, "#include <stdio.h>\n");
  generator_emit_manifest_headers(g);
  fprintf(f, "\n");

  /* Aggregate release functions take void* in their declarations so they can
   * be used by functions emitted before a later type definition. */
  for (int i = 0; i < stmts.length; i++) {
    ast_t stmt = stmts.data[i];
    if (stmt->tag == tdef) {
      if (is_asset_only_module(g, stmt)) continue;
      fprintf(f, "static void __rift_release_" SV_Fmt "(void *);\n",
              SV_Arg(stmt->data.tdef.name.lexeme));
    }
  }
  for (int i = 0; i < g->components->interface_count; i++) {
    component_interface_spec *entry = &g->components->interfaces[i];
    if (entry->kind == COMPONENT_OPAQUE && g->opaque_value_used[i])
      fprintf(f, "static void __rift_release_%s(void *value) { "
                 "__handle_release(value); }\n", entry->owner);
  }
  fprintf(f, "\n");

  generate_forward_defs(g, program);

  for (int i = 0; i < stmts.length; i++) {
    ast_t stmt = stmts.data[i];
    generate_statement(g, stmt);
  }
}
