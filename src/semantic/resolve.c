#include "resolve.h"
#include "generator/components.h"
#include "generator/internal.h"
#include "generator/type_info.h"
#include "error.h"
#include "stringview.h"
#include <stdlib.h>
#include <string.h>

char *mangle_method(string_view type_name, string_view method_name, int is_array_method);
char *mangle_type_method(string_view type_name, string_view method_name, int argc);
char *mangle_fundef_method(ast_fundef fundef);
ast_t make_native_fundef(component_interface_spec *entry, method_kind_t method_kind);
static token_t value_constructor_named(ast_t definition, string_view name);
static int is_asset_category(generator_t *g, string_view type_name);
static ast_t find_nominal_member_type(name_table_t table, string_view owner,
                                      string_view member_name,
                                      int module_only);

int is_instance_method_kind(method_kind_t kind);

static component_interface_spec *opaque_for_sv(generator_t *g,
                                               string_view name) {
  char *owned = string_of_sv(name);
  component_interface_spec *result =
      find_opaque_interface(g->components, owned);
  return result;
}

static int is_opaque_type(string_view name, generator_t *g) {
  return opaque_for_sv(g, name) != NULL;
}

static int is_namespace_type(string_view name, generator_t *g) {
  char *owned = string_of_sv(name);
  return find_namespace(g->components, owned) != NULL;
}

static int type_method_name_exists(ast_t program, string_view owner,
                                   string_view method) {
  ast_array_t stmts = program->data.program.prog;
  for (int i = 0; i < stmts.length; i++) {
    ast_t stmt = stmts.data[i];
    if (stmt->tag != fundef) continue;
    ast_fundef fd = stmt->data.fundef;
    if (fd.method_kind == METHOD_TYPE_LEVEL &&
        svcmp(fd.type_name.lexeme, owner) == 0 &&
        svcmp(fd.name.lexeme, method) == 0)
      return 1;
  }
  return 0;
}

static int native_method_name_exists(generator_t *g, string_view owner,
                                     string_view method, int type_level) {
  char *owner_text = string_of_sv(owner);
  char *method_text = string_of_sv(method);
  int found = 0;
  for (int i = 0; i < g->components->interface_count; i++) {
    component_interface_spec *entry = &g->components->interfaces[i];
    component_interface_kind wanted = type_level ? COMPONENT_TYPE_METHOD
                                                  : COMPONENT_INSTANCE_METHOD;
    if (entry->kind == wanted && strcmp(entry->owner, owner_text) == 0 &&
        strcmp(entry->name, method_text) == 0) {
      found = 1;
      break;
    }
  }
  return found;
}

static int native_symbol_reserved(generator_t *g, string_view name) {
  char *text = string_of_sv(name);
  int reserved = 0;
  for (int i = 0; i < g->components->interface_count; i++) {
    component_interface_spec *entry = &g->components->interfaces[i];
    int sealed_symbol = entry->kind == COMPONENT_OPAQUE ||
                        entry->kind == COMPONENT_INSTANCE_METHOD ||
                        entry->kind == COMPONENT_TYPE_METHOD;
    if (sealed_symbol &&
        ((entry->c_symbol && strcmp(entry->c_symbol, text) == 0) ||
         (entry->constructor && strcmp(entry->constructor, text) == 0))) {
      reserved = 1;
      break;
    }
  }
  return reserved;
}

static token_t nominal_definition_name(ast_t stmt) {
  if (stmt->tag == tdef) return stmt->data.tdef.name;
  if (stmt->tag == enum_tdef) return stmt->data.enum_tdef.name;
  return (token_t){0};
}

static void validate_sprite_variant_name(token_t name) {
  if (svcmp(name.lexeme, sv_from_cstr("Sprite")) == 0)
    error(name.filename, name.line, name.col,
          "variant name 'Sprite' is reserved by the built-in value constructor");
}

static void validate_sprite_variant_names(ast_t node) {
  if (!node) return;
  switch (node->tag) {
  case program:
    for (int i = 0; i < node->data.program.prog.length; i++)
      validate_sprite_variant_names(node->data.program.prog.data[i]);
    return;
  case tdef:
    if (node->data.tdef.t == TDEF_PRO)
      for (int i = 0; i < node->data.tdef.constructors.length; i++) {
        ast_t member = node->data.tdef.constructors.data[i];
        if (member && member->tag == cons)
          validate_sprite_variant_name(member->data.cons.name);
      }
    return;
  case enum_tdef:
    for (int i = 0; i < node->data.enum_tdef.items.length; i++)
      validate_sprite_variant_name(node->data.enum_tdef.items.data[i]);
    return;
  default:
    return;
  }
}

static void validate_unique_member_names(ast_t stmt) {
  token_array_t names = new_token_array();
  if (stmt->tag == enum_tdef) {
    names = stmt->data.enum_tdef.items;
  } else if (stmt->tag == tdef) {
    ast_tdef td = stmt->data.tdef;
    ast_array_t members = td.t == TDEF_MODULE ? td.module_fields
                                              : td.constructors;
    for (int i = 0; i < members.length; i++) {
      ast_t member = members.data[i];
      token_t name = member->tag == vardef ? member->data.vardef.name
                                           : member->data.cons.name;
      token_array_push(&names, name);
    }
  } else {
    return;
  }

  for (int i = 0; i < names.length; i++) {
    for (int j = 0; j < i; j++) {
      if (svcmp(names.data[i].lexeme, names.data[j].lexeme) == 0) {
        error(names.data[i].filename, names.data[i].line, names.data[i].col,
              "duplicate member '" SV_Fmt "' in this type",
              SV_Arg(names.data[i].lexeme));
        break;
      }
    }
  }
}

static void validate_unique_value_constructor_names(ast_array_t statements,
                                                    int definition_index) {
  ast_t definition = statements.data[definition_index];
  token_array_t names = new_token_array();
  if (definition->tag == enum_tdef) {
    names = definition->data.enum_tdef.items;
  } else if (definition->tag == tdef &&
             definition->data.tdef.t == TDEF_PRO) {
    for (int i = 0; i < definition->data.tdef.constructors.length; i++) {
      ast_t constructor = definition->data.tdef.constructors.data[i];
      if (constructor && constructor->tag == cons)
        token_array_push(&names, constructor->data.cons.name);
    }
  } else {
    return;
  }

  for (int i = 0; i < names.length; i++) {
    for (int j = 0; j < definition_index; j++) {
      ast_t previous = statements.data[j];
      if (previous->tag != enum_tdef &&
          (previous->tag != tdef ||
           previous->data.tdef.t != TDEF_PRO))
        continue;
      token_t collision =
          value_constructor_named(previous, names.data[i].lexeme);
      if (collision.lexeme.length == 0) continue;
      error(names.data[i].filename, names.data[i].line, names.data[i].col,
            "value constructor '" SV_Fmt
            "' conflicts with an earlier enum or union value constructor",
            SV_Arg(names.data[i].lexeme));
      break;
    }
  }
}

static void register_program_symbols(generator_t *g, ast_t program_node) {
  ast_array_t stmts = program_node->data.program.prog;

  for (int i = 0; i < stmts.length; i++) {
    ast_t stmt = stmts.data[i];
    if (stmt->tag == tdef || stmt->tag == enum_tdef) {
      token_t name = nominal_definition_name(stmt);
      if (is_asset_category(g, name.lexeme)) {
        error(name.filename, name.line, name.col,
              "compile-time asset category '" SV_Fmt
              "' is sealed and cannot be redefined",
              SV_Arg(name.lexeme));
        continue;
      }
      if (is_opaque_type(name.lexeme, g) || is_namespace_type(name.lexeme, g)) {
        error(name.filename, name.line, name.col,
              "standard type namespace '" SV_Fmt "' is sealed and cannot be redefined",
              SV_Arg(name.lexeme));
        continue;
      }
      int duplicate = 0;
      for (int j = 0; j < i; j++) {
        ast_t previous = stmts.data[j];
        if (previous->tag != tdef && previous->tag != enum_tdef) continue;
        if (svcmp(nominal_definition_name(previous).lexeme, name.lexeme) == 0) {
          error(name.filename, name.line, name.col,
                "duplicate type definition '" SV_Fmt "' in this scope",
                SV_Arg(name.lexeme));
          duplicate = 1;
          break;
        }
      }
      validate_unique_member_names(stmt);
      validate_unique_value_constructor_names(stmts, i);
      if (!duplicate && stmt->tag == tdef &&
          !push_nt_unique(&g->table, name.lexeme, NT_USER_TYPE, stmt))
        error(name.filename, name.line, name.col,
              "duplicate type definition '" SV_Fmt "' in this scope",
              SV_Arg(name.lexeme));
    }
  }

  for (int i = 0; i < stmts.length; i++) {
    ast_t stmt = stmts.data[i];
    if (stmt->tag != fundef) continue;
    ast_fundef fd = stmt->data.fundef;
    int user_owner =
        lookup_nt_by_kind(fd.type_name.lexeme, NT_USER_TYPE, g->table).found;
    int builtin_owner =
        lookup_nt_by_kind(fd.type_name.lexeme, NT_BUILTIN_TYPE, g->table).found;
    int opaque_owner =
        lookup_nt_by_kind(fd.type_name.lexeme, NT_OPAQUE_TYPE, g->table).found;
    int namespace_owner =
        lookup_nt_by_kind(fd.type_name.lexeme, NT_NAMESPACE_TYPE, g->table).found;
    if (fd.method_kind != METHOD_NONE && (opaque_owner || namespace_owner)) {
      error(fd.type_name.filename, fd.type_name.line, fd.type_name.col,
            "standard type namespace '" SV_Fmt
            "' is sealed; source methods cannot extend it",
            SV_Arg(fd.type_name.lexeme));
      continue;
    }
    if (fd.method_kind == METHOD_TYPE_LEVEL && !user_owner) {
      error(fd.type_name.filename, fd.type_name.line, fd.type_name.col,
            "type-level method owner '" SV_Fmt
            "' must be a user-defined module, record, or union",
            SV_Arg(fd.type_name.lexeme));
      continue;
    }
    if (fd.method_kind != METHOD_NONE && !user_owner && !builtin_owner) {
      error(fd.type_name.filename, fd.type_name.line, fd.type_name.col,
            "method owner '" SV_Fmt "' is not a declared nominal type",
            SV_Arg(fd.type_name.lexeme));
      continue;
    }

    string_view key = fd.name.lexeme;
    if (fd.method_kind != METHOD_NONE)
      key = sv_from_cstr(mangle_fundef_method(fd));

    if (fd.method_kind == METHOD_NONE) {
      if (svcmp(fd.name.lexeme, sv_from_cstr("Sprite")) == 0) {
        error(fd.name.filename, fd.name.line, fd.name.col,
              "function name 'Sprite' is reserved by the built-in value type");
        continue;
      }
      int constructor_collision = 0;
      for (int j = 0; j < stmts.length; j++) {
        token_t constructor =
            value_constructor_named(stmts.data[j], fd.name.lexeme);
        if (constructor.lexeme.length == 0) continue;
        error(fd.name.filename, fd.name.line, fd.name.col,
              "function '" SV_Fmt
              "' conflicts with an enum or union value constructor",
              SV_Arg(fd.name.lexeme));
        constructor_collision = 1;
        break;
      }
      if (constructor_collision) continue;
      if (native_symbol_reserved(g, fd.name.lexeme)) {
        error(fd.name.filename, fd.name.line, fd.name.col,
              "function name '" SV_Fmt
              "' is reserved by a standard opaque interface",
              SV_Arg(fd.name.lexeme));
        continue;
      }
      for (int j = 0; j < i; j++) {
        ast_t previous = stmts.data[j];
        if (previous->tag != fundef) continue;
        ast_fundef other = previous->data.fundef;
        if (other.method_kind == METHOD_NONE &&
            svcmp(other.name.lexeme, fd.name.lexeme) == 0 &&
            other.args.length == fd.args.length) {
          error(fd.name.filename, fd.name.line, fd.name.col,
                "duplicate function '" SV_Fmt
                "' with %d argument(s) in this scope",
                SV_Arg(fd.name.lexeme), fd.args.length);
          break;
        }
      }
    } else if (fd.method_kind == METHOD_INSTANCE ||
        fd.method_kind == METHOD_ARRAY_INSTANCE ||
        fd.method_kind == METHOD_TYPE_LEVEL) {
      if (get_ref_by_kind(key, NT_FUN, g->table) != NULL) {
        error(fd.name.filename, fd.name.line, fd.name.col,
              "duplicate method '" SV_Fmt "." SV_Fmt "' for this receiver and arity",
              SV_Arg(fd.type_name.lexeme), SV_Arg(fd.name.lexeme));
        continue;
      }
    }
    push_nt(&g->table, key, NT_FUN, stmt);
  }

  for (int i = 0; i < stmts.length; i++) {
    ast_t stmt = stmts.data[i];
    if (stmt->tag == vardef) {
      token_t name = stmt->data.vardef.name;
      if (!push_nt_unique(&g->table, name.lexeme, NT_VAR, stmt))
        error(name.filename, name.line, name.col,
              "duplicate variable '" SV_Fmt "' in this scope",
              SV_Arg(name.lexeme));
    }
  }
}

static int same_asset_scope(ast_asset_decl left, ast_asset_decl right) {
  if (left.module.lexeme.length == 0 || right.module.lexeme.length == 0)
    return left.module.lexeme.length == right.module.lexeme.length;
  return svcmp(left.module.lexeme, right.module.lexeme) == 0;
}

static int sv_ascii_case_equal(string_view left, string_view right) {
  if (left.length != right.length) return 0;
  for (size_t i = 0; i < left.length; i++) {
    unsigned char a = (unsigned char)left.data[i];
    unsigned char b = (unsigned char)right.data[i];
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
    if (a != b) return 0;
  }
  return 1;
}

static component_asset_kind_spec *asset_kind_for_decl(generator_t *g,
                                                       ast_t declaration) {
  char *kind = string_of_sv(declaration->data.asset_decl.kind.lexeme);
  return find_asset_kind(g->components, kind);
}

static token_t value_constructor_named(ast_t definition, string_view name) {
  if (definition->tag == enum_tdef) {
    for (int i = 0; i < definition->data.enum_tdef.items.length; i++) {
      token_t item = definition->data.enum_tdef.items.data[i];
      if (svcmp(item.lexeme, name) == 0) return item;
    }
  } else if (definition->tag == tdef &&
             definition->data.tdef.t == TDEF_PRO) {
    for (int i = 0; i < definition->data.tdef.constructors.length; i++) {
      ast_t constructor = definition->data.tdef.constructors.data[i];
      if (constructor && constructor->tag == cons &&
          svcmp(constructor->data.cons.name.lexeme, name) == 0)
        return constructor->data.cons.name;
    }
  }
  return (token_t){0};
}

static int is_asset_category(generator_t *g, string_view type_name) {
  for (int i = 0; i < g->components->asset_kind_count; i++)
    if (svcmp(type_name,
              sv_from_cstr(g->components->asset_kinds[i].category)) == 0)
      return 1;
  return 0;
}

int is_asset_only_module(generator_t *g, ast_t definition) {
  if (!definition || definition->tag != tdef ||
      definition->data.tdef.t != TDEF_MODULE ||
      definition->data.tdef.module_fields.length != 0)
    return 0;
  string_view name = definition->data.tdef.name.lexeme;
  for (int i = 0; i < g->asset_decls.length; i++) {
    ast_asset_decl asset = g->asset_decls.data[i]->data.asset_decl;
    if (asset.module.lexeme.length > 0 &&
        svcmp(asset.module.lexeme, name) == 0)
      return 1;
  }
  return 0;
}

static void register_asset_declarations(generator_t *g, ast_t program_node) {
  ast_array_t stmts = program_node->data.program.prog;
  for (int i = 0; i < stmts.length; i++) {
    ast_t stmt = stmts.data[i];
    if (stmt->tag != asset_decl) continue;
    ast_asset_decl asset = stmt->data.asset_decl;
    component_asset_kind_spec *kind = asset_kind_for_decl(g, stmt);
    if (!kind) {
      error(asset.kind.filename, asset.kind.line, asset.kind.col,
            "unknown asset kind '" SV_Fmt "'", SV_Arg(asset.kind.lexeme));
      continue;
    }
    for (int j = 0; j < g->asset_decls.length; j++) {
      ast_asset_decl prior = g->asset_decls.data[j]->data.asset_decl;
      if (same_asset_scope(asset, prior) &&
          svcmp(asset.name.lexeme, prior.name.lexeme) == 0) {
        error(asset.name.filename, asset.name.line, asset.name.col,
              "duplicate asset '" SV_Fmt "' in this module",
              SV_Arg(asset.name.lexeme));
      } else if (same_asset_scope(asset, prior) &&
                 sv_ascii_case_equal(asset.name.lexeme, prior.name.lexeme)) {
        error(asset.name.filename, asset.name.line, asset.name.col,
              "asset '" SV_Fmt
              "' collides by case with another asset in this module",
              SV_Arg(asset.name.lexeme));
      }
    }
    if (asset.module.lexeme.length == 0) {
      for (int j = 0; j < stmts.length; j++) {
        ast_t other = stmts.data[j];
        token_t name = {0};
        if (other->tag == vardef) name = other->data.vardef.name;
        else if (other->tag == fundef &&
                 other->data.fundef.method_kind == METHOD_NONE)
          name = other->data.fundef.name;
        else if (other->tag == tdef || other->tag == enum_tdef)
          name = nominal_definition_name(other);
        if (name.lexeme.length > 0 &&
            svcmp(name.lexeme, asset.name.lexeme) == 0)
          error(asset.name.filename, asset.name.line, asset.name.col,
                "asset '" SV_Fmt "' conflicts with a top-level declaration",
                SV_Arg(asset.name.lexeme));
        token_t constructor = value_constructor_named(other, asset.name.lexeme);
        if (constructor.lexeme.length > 0)
          error(asset.name.filename, asset.name.line, asset.name.col,
                "asset '" SV_Fmt
                "' conflicts with an enum or union value constructor",
                SV_Arg(asset.name.lexeme));
      }
    } else {
      ast_t module = get_ref_by_kind(asset.module.lexeme, NT_USER_TYPE,
                                     g->table);
      if (module && module->tag == tdef) {
        ast_array_t fields = module->data.tdef.module_fields;
        for (int j = 0; j < fields.length; j++)
          if (svcmp(fields.data[j]->data.vardef.name.lexeme,
                    asset.name.lexeme) == 0)
            error(asset.name.filename, asset.name.line, asset.name.col,
                  "asset '" SV_Fmt "' conflicts with a module field",
                  SV_Arg(asset.name.lexeme));
      }
    }
    push_ast_array(&g->asset_decls, stmt);
  }
}

static ast_t asset_for_expr(generator_t *g, ast_t expr) {
  if (!expr) return NULL;
  string_view module = sv_from_cstr("");
  string_view name = sv_from_cstr("");
  if (expr->tag == identifier) {
    name = expr->data.identifier.id.lexeme;
    if (lookup_nt_by_kind(name, NT_VAR, g->table).found) return NULL;
    if (g->current_fundef &&
        is_instance_method_kind(g->current_fundef->data.fundef.method_kind)) {
      if (svcmp(name, sv_from_cstr("this")) == 0 ||
          find_nominal_member_type(
              g->table, g->current_fundef->data.fundef.type_name.lexeme,
              name, 1))
        return NULL;
    }
  } else if (expr->tag == sub) {
    ast_sub access = expr->data.sub;
    if (!access.receiver || access.receiver->tag != identifier ||
        access.path.length != 0 || !access.expr ||
        access.expr->tag != identifier)
      return NULL;
    module = access.receiver->data.identifier.id.lexeme;
    name = access.expr->data.identifier.id.lexeme;
    if (lookup_nt_by_kind(module, NT_VAR, g->table).found) return NULL;
    if (g->current_fundef &&
        is_instance_method_kind(g->current_fundef->data.fundef.method_kind) &&
        find_nominal_member_type(
            g->table, g->current_fundef->data.fundef.type_name.lexeme,
            module, 1))
      return NULL;
    ast_t module_ref = get_ref_by_kind(module, NT_USER_TYPE, g->table);
    if (!module_ref || module_ref->tag != tdef ||
        module_ref->data.tdef.t != TDEF_MODULE)
      return NULL;
  } else {
    return NULL;
  }
  for (int i = 0; i < g->asset_decls.length; i++) {
    ast_t declaration = g->asset_decls.data[i];
    ast_asset_decl asset = declaration->data.asset_decl;
    if (svcmp(asset.module.lexeme, module) == 0 &&
        svcmp(asset.name.lexeme, name) == 0)
      return declaration;
  }
  return NULL;
}

static int integer_literal_value(ast_t expr, long *value) {
  int negative = 0;
  if (expr && expr->tag == unary_op && expr->data.unary_op.op == TOK_MINUS) {
    negative = 1;
    expr = expr->data.unary_op.operand;
  }
  if (!expr || expr->tag != literal ||
      expr->data.literal.lit.type != TOK_NUM_LIT)
    return 0;
  char *text = string_of_sv(expr->data.literal.lit.lexeme);
  char *end = NULL;
  long parsed = strtol(text, &end, 10);
  if (!end || *end != '\0') return 0;
  *value = negative ? -parsed : parsed;
  return 1;
}

static void validate_literal_range(ast_t expr, long minimum, long maximum,
                                   const char *what) {
  long value;
  if (!integer_literal_value(expr, &value)) return;
  if (value < minimum || value > maximum) {
    token_t tok = token_for_expr(expr);
    error(tok.filename, tok.line, tok.col,
          "%s literal %ld is outside %ld..%ld", what, value, minimum,
          maximum);
  }
}

static void validate_integer_literal(ast_t expr, const char *what) {
  if (!expr) return;
  int numeric_literal =
      expr->tag == literal && expr->data.literal.lit.type == TOK_NUM_LIT;
  if (expr->tag == unary_op && expr->data.unary_op.op == TOK_MINUS &&
      expr->data.unary_op.operand && expr->data.unary_op.operand->tag == literal)
    numeric_literal =
        expr->data.unary_op.operand->data.literal.lit.type == TOK_NUM_LIT;
  long value;
  if (numeric_literal && !integer_literal_value(expr, &value)) {
    token_t tok = token_for_expr(expr);
    error(tok.filename, tok.line, tok.col, "%s must be an integer literal",
          what);
  }
}

static int sprite_slot_integer_expr(generator_t *g, ast_t expr) {
  if (!expr) return 0;
  long value;
  if (expr->tag == literal)
    return expr->data.literal.lit.type == TOK_NUM_LIT &&
           integer_literal_value(expr, &value);
  if (expr->tag == unary_op && expr->data.unary_op.op == TOK_MINUS &&
      expr->data.unary_op.operand && expr->data.unary_op.operand->tag == literal)
    return expr->data.unary_op.operand->data.literal.lit.type == TOK_NUM_LIT &&
           integer_literal_value(expr, &value);
  return svcmp(infer_expr_type(expr, g->table),
               sv_from_cstr("byte")) == 0;
}

static int sprite_value_expr(generator_t *g, ast_t expr) {
  return svcmp(infer_expr_type(expr, g->table),
               sv_from_cstr("Sprite")) == 0;
}

static int sprite_array_expr(generator_t *g, ast_t expr) {
  return svcmp(infer_expr_type(expr, g->table),
               sv_from_cstr("Sprite_array")) == 0;
}

static void validate_sprite_expected(generator_t *g, ast_type expected,
                                     ast_t expr, token_t where,
                                     const char *context) {
  if (svcmp(expected.name.lexeme, sv_from_cstr("Sprite")) != 0 || !expr)
    return;
  if (expected.is_array) {
    int empty_literal = expr->tag == literal &&
                        expr->data.literal.lit.type == TOK_ARR_DECL;
    if (!empty_literal && !sprite_array_expr(g, expr))
      error(where.filename, where.line, where.col,
            "%s requires a Sprite array value", context);
  } else if (!sprite_value_expr(g, expr)) {
    error(where.filename, where.line, where.col,
          "%s requires a Sprite value", context);
  }
}

static int value_constructor_arity(generator_t *g, string_view name,
                                   ast_t *payload_type) {
  *payload_type = NULL;
  if (!g->program || g->program->tag != program) return -1;
  ast_array_t statements = g->program->data.program.prog;
  for (int i = 0; i < statements.length; i++) {
    ast_t definition = statements.data[i];
    if (!definition) continue;
    if (definition->tag == enum_tdef) {
      for (int j = 0; j < definition->data.enum_tdef.items.length; j++)
        if (svcmp(definition->data.enum_tdef.items.data[j].lexeme, name) == 0)
          return -2;
    } else if (definition->tag == tdef &&
               definition->data.tdef.t == TDEF_PRO) {
      for (int j = 0; j < definition->data.tdef.constructors.length; j++) {
        ast_t constructor = definition->data.tdef.constructors.data[j];
        if (!constructor || constructor->tag != cons ||
            svcmp(constructor->data.cons.name.lexeme, name) != 0)
          continue;
        *payload_type = constructor->data.cons.type;
        return *payload_type && (*payload_type)->tag == type &&
                       svcmp((*payload_type)->data.type.name.lexeme,
                             sv_from_cstr("void")) != 0
                   ? 1
                   : 0;
      }
    }
  }
  return -1;
}

static void validate_sprite_record_expr(generator_t *g, string_view type_name,
                                        ast_t expr) {
  if (!expr || expr->tag != record_expr) return;
  ast_t definition = get_ref_by_kind(type_name, NT_USER_TYPE, g->table);
  if (!definition || definition->tag != tdef ||
      definition->data.tdef.t != TDEF_REC)
    return;
  ast_record_expr record = expr->data.record_expr;
  for (int i = 0; i < definition->data.tdef.constructors.length; i++) {
    ast_t field = definition->data.tdef.constructors.data[i];
    if (!field || field->tag != cons || !field->data.cons.type ||
        field->data.cons.type->tag != type ||
        svcmp(field->data.cons.type->data.type.name.lexeme,
              sv_from_cstr("Sprite")) != 0)
      continue;
    int initialized = 0;
    for (int j = 0; j < record.names.length; j++) {
      if (svcmp(field->data.cons.name.lexeme,
                record.names.data[j].lexeme) == 0) {
        initialized = 1;
        break;
      }
    }
    if (!initialized) {
      token_t where = field->data.cons.name;
      error(where.filename, where.line, where.col,
            "Sprite record field '" SV_Fmt
            "' requires explicit initialization",
            SV_Arg(field->data.cons.name.lexeme));
    }
  }
  for (int i = 0; i < record.names.length; i++) {
    for (int j = 0; j < definition->data.tdef.constructors.length; j++) {
      ast_t field = definition->data.tdef.constructors.data[j];
      if (!field || field->tag != cons ||
          svcmp(field->data.cons.name.lexeme, record.names.data[i].lexeme) != 0)
        continue;
      ast_type expected = field->data.cons.type->data.type;
      validate_sprite_expected(g, expected, record.exprs.data[i],
                               record.names.data[i],
                               "Sprite record field initializer");
      if (!expected.is_array)
        validate_sprite_record_expr(g, expected.name.lexeme,
                                    record.exprs.data[i]);
      break;
    }
  }
}

static void validate_sprite_parameters(generator_t *g, ast_t target,
                                       ast_array_t arguments, int offset,
                                       const char *context) {
  if (!target || target->tag != fundef) return;
  ast_array_t parameters = target->data.fundef.types;
  int has_sprite_parameter = 0;
  for (int i = offset; i < parameters.length; i++) {
    ast_t parameter = parameters.data[i];
    if (parameter && parameter->tag == type &&
        svcmp(parameter->data.type.name.lexeme,
              sv_from_cstr("Sprite")) == 0) {
      has_sprite_parameter = 1;
      break;
    }
  }
  int expected_arguments = parameters.length - offset;
  int returns_sprite = target->data.fundef.ret_type &&
                       target->data.fundef.ret_type->tag == type &&
                       svcmp(target->data.fundef.ret_type->data.type.name.lexeme,
                             sv_from_cstr("Sprite")) == 0;
  if ((has_sprite_parameter || returns_sprite) &&
      arguments.length != expected_arguments) {
    token_t where = target->data.fundef.name;
    if (arguments.length > expected_arguments)
      where = token_for_expr(arguments.data[expected_arguments]);
    error(where.filename, where.line, where.col,
          "%s count for Sprite-related call must be %d, got %d", context,
          expected_arguments, arguments.length);
    return;
  }
  for (int i = offset; i < parameters.length; i++) {
    ast_t parameter = parameters.data[i];
    if (!parameter || parameter->tag != type ||
        svcmp(parameter->data.type.name.lexeme,
              sv_from_cstr("Sprite")) != 0)
      continue;
    int argument_index = i - offset;
    validate_sprite_expected(g, parameter->data.type,
                             arguments.data[argument_index],
                             token_for_expr(arguments.data[argument_index]),
                             context);
  }
}

static int statement_definitely_returns(ast_t statement) {
  if (!statement) return 0;
  if (statement->tag == ret) return 1;
  if (statement->tag == compound) {
    ast_array_t statements = statement->data.compound.stmts;
    for (int i = 0; i < statements.length; i++)
      if (statement_definitely_returns(statements.data[i])) return 1;
    return 0;
  }
  if (statement->tag == ifstmt)
    return statement->data.ifstmt.elsestmt &&
           statement_definitely_returns(statement->data.ifstmt.body) &&
           statement_definitely_returns(statement->data.ifstmt.elsestmt);
  if (statement->tag == match) {
    ast_array_t cases = statement->data.match.cases;
    int has_default = 0;
    for (int i = 0; i < cases.length; i++) {
      ast_t match_case = cases.data[i];
      if (!match_case || match_case->tag != matchcase ||
          !statement_definitely_returns(match_case->data.matchcase.body))
        return 0;
      ast_t selector = match_case->data.matchcase.expr;
      if (selector && selector->tag == literal &&
          selector->data.literal.lit.type == TOK_WILDCARD)
        has_default = 1;
    }
    return has_default;
  }
  return 0;
}

static void validate_no_asset_type(generator_t *g, ast_t type_node,
                                   token_t where) {
  if (!type_node || type_node->tag != type) return;
  if (is_asset_category(g, type_node->data.type.name.lexeme))
    error(where.filename, where.line, where.col,
          "compile-time asset category '" SV_Fmt
          "' cannot be used as runtime storage, a parameter, or a return type",
          SV_Arg(type_node->data.type.name.lexeme));
  if (lookup_nt_by_kind(type_node->data.type.name.lexeme, NT_NAMESPACE_TYPE,
                        g->table).found &&
      svcmp(type_node->data.type.name.lexeme,
            sv_from_cstr("Sprite")) != 0)
    error(where.filename, where.line, where.col,
          "type namespace '" SV_Fmt "' cannot be used as a runtime value type",
          SV_Arg(type_node->data.type.name.lexeme));
  ast_t definition = get_ref_by_kind(type_node->data.type.name.lexeme,
                                     NT_USER_TYPE, g->table);
  if (is_asset_only_module(g, definition))
    error(where.filename, where.line, where.col,
          "asset-only module '" SV_Fmt
          "' is a compile-time namespace, not a runtime value type",
          SV_Arg(type_node->data.type.name.lexeme));
}

static void validate_asset_uses(generator_t *g, ast_t node, int allow_asset) {
  if (!node) return;
  ast_t direct_asset = asset_for_expr(g, node);
  if (direct_asset) {
    if (!allow_asset) {
      token_t tok = token_for_expr(node);
      error(tok.filename, tok.line, tok.col,
            "asset '" SV_Fmt
            "' may only appear directly in a registered consumer call",
            SV_Arg(direct_asset->data.asset_decl.name.lexeme));
    }
    return;
  }
  switch (node->tag) {
  case program:
    for (int i = 0; i < node->data.program.prog.length; i++)
      validate_asset_uses(g, node->data.program.prog.data[i], 0);
    return;
  case fundef: {
    ast_t saved_fundef = g->current_fundef;
    g->current_fundef = node;
    validate_no_asset_type(g, node->data.fundef.ret_type,
                           node->data.fundef.name);
    new_nt_scope(&g->table);
    for (int i = 0; i < node->data.fundef.types.length; i++) {
      validate_no_asset_type(g, node->data.fundef.types.data[i],
                             node->data.fundef.args.data[i]);
      push_nt(&g->table, node->data.fundef.args.data[i].lexeme, NT_VAR, node);
    }
    validate_asset_uses(g, node->data.fundef.body, 0);
    if (node->data.fundef.ret_type &&
        node->data.fundef.ret_type->tag == type &&
        svcmp(node->data.fundef.ret_type->data.type.name.lexeme,
              sv_from_cstr("Sprite")) == 0 &&
        !statement_definitely_returns(node->data.fundef.body)) {
      token_t where = node->data.fundef.name;
      error(where.filename, where.line, where.col,
            "Sprite-returning function may fall through without returning a value");
    }
    end_nt_scope(&g->table);
    g->current_fundef = saved_fundef;
    return;
  }
  case compound:
    new_nt_scope(&g->table);
    for (int i = 0; i < node->data.compound.stmts.length; i++) {
      ast_t stmt = node->data.compound.stmts.data[i];
      validate_asset_uses(g, stmt, 0);
      if (stmt->tag == vardef)
        push_nt(&g->table, stmt->data.vardef.name.lexeme, NT_VAR, stmt);
    }
    end_nt_scope(&g->table);
    return;
  case method_call: {
    ast_method_call call = node->data.method_call;
    int sprite_method = call.is_resolved &&
                        svcmp(call.resolved_owner,
                              sv_from_cstr("Sprite")) == 0;
    int sprite_frame = sprite_method &&
                       call.resolved_kind == METHOD_INSTANCE &&
                       svcmp(call.method.lexeme,
                             sv_from_cstr("frame")) == 0;
    int sprite_position = sprite_method &&
                          call.resolved_kind == METHOD_INSTANCE &&
                          svcmp(call.method.lexeme,
                                sv_from_cstr("position")) == 0;
    validate_asset_uses(g, call.receiver, 0);
    for (int i = 0; i < call.args.length; i++) {
      if (!sprite_frame || i != 0) {
        validate_asset_uses(g, call.args.data[i], 0);
        continue;
      }
      ast_t declaration = call.resolved_asset;
      component_asset_kind_spec *kind =
          declaration ? asset_kind_for_decl(g, declaration) : NULL;
      if (!declaration || !kind || strcmp(kind->category, "SpritePattern") != 0) {
        token_t tok = token_for_expr(call.args.data[i]);
        error(tok.filename, tok.line, tok.col,
              "Sprite.frame pattern must be a SpritePattern binding");
      } else {
        declaration->data.asset_decl.referenced = 1;
        record_component(g, kind->component_id);
        validate_asset_uses(g, call.args.data[i], 1);
        long frame_bytes = strcmp(kind->kind, "sprite8") == 0 ? 256 : 128;
        long frames = declaration->data.asset_decl.byte_length / frame_bytes;
        if (call.args.length > 1) {
          validate_integer_literal(call.args.data[1], "sprite frame");
        }
        if (call.args.length > 1 && frames > 0)
          validate_literal_range(call.args.data[1], 0, frames - 1,
                                 "sprite frame");
      }
    }
    if (sprite_position && call.args.length >= 2) {
      validate_integer_literal(call.args.data[0], "sprite x");
      validate_integer_literal(call.args.data[1], "sprite y");
      validate_literal_range(call.args.data[0], 0, 319, "sprite x");
      validate_literal_range(call.args.data[1], 0, 255, "sprite y");
    }
    validate_sprite_parameters(
        g, call.resolved_target, call.args,
        call.resolved_kind == METHOD_TYPE_LEVEL ? 0 : 1,
        "method argument");
    return;
  }
  case funcall: {
    if (svcmp(node->data.funcall.name.lexeme, sv_from_cstr("Sprite")) == 0) {
      if (node->data.funcall.args.length != 1) {
        token_t tok = node->data.funcall.name;
        error(tok.filename, tok.line, tok.col,
              "Sprite() expects exactly one byte slot argument");
      } else {
        if (!sprite_slot_integer_expr(g, node->data.funcall.args.data[0])) {
          token_t tok = token_for_expr(node->data.funcall.args.data[0]);
          error(tok.filename, tok.line, tok.col,
                "Sprite() slot argument must be a byte-compatible integer expression");
        }
        validate_literal_range(node->data.funcall.args.data[0], 0, 127,
                               "sprite slot");
      }
    }
    ast_funcall function = node->data.funcall;
    validate_sprite_parameters(g, function.resolved_target, function.args, 0,
                               "function argument");
    ast_t constructor_type = NULL;
    int constructor_arity = value_constructor_arity(
        g, function.name.lexeme, &constructor_type);
    if (constructor_arity == -2) {
      token_t where = function.name;
      error(where.filename, where.line, where.col,
            "enum item '" SV_Fmt
            "' is a value; use " SV_Fmt " without parentheses",
            SV_Arg(function.name.lexeme), SV_Arg(function.name.lexeme));
    }
    if (constructor_arity >= 0 && function.args.length != constructor_arity) {
      token_t where = function.name;
      error(where.filename, where.line, where.col,
            "value constructor '" SV_Fmt
            "' expects exactly %d argument(s), got %d",
            SV_Arg(function.name.lexeme), constructor_arity,
            function.args.length);
    }
    if (constructor_arity == 1 && function.args.length == 1 &&
        constructor_type && constructor_type->tag == type &&
        svcmp(constructor_type->data.type.name.lexeme,
              sv_from_cstr("Sprite")) == 0) {
      validate_sprite_expected(g, constructor_type->data.type,
                               function.args.data[0],
                               token_for_expr(function.args.data[0]),
                               "union constructor argument");
    }
    int resolved_user = function.resolved_target &&
                        function.resolved_target->tag == fundef &&
                        function.resolved_target->data.fundef.body != NULL;
    int writes_array = !resolved_user &&
        (svcmp(function.name.lexeme, sv_from_cstr("append")) == 0 ||
         svcmp(function.name.lexeme, sv_from_cstr("set")) == 0 ||
         svcmp(function.name.lexeme, sv_from_cstr("insert")) == 0);
    if (writes_array && function.args.length >= 2) {
      string_view element = get_array_element_type(
          function.args.data[0], g->table, function.name);
      if (svcmp(element, sv_from_cstr("Sprite")) == 0) {
        ast_type expected = {0};
        expected.name.lexeme = sv_from_cstr("Sprite");
        ast_t value = function.args.data[function.args.length - 1];
        validate_sprite_expected(g, expected, value, token_for_expr(value),
                                 "Sprite array write");
      }
    }
    for (int i = 0; i < node->data.funcall.args.length; i++)
      validate_asset_uses(g, node->data.funcall.args.data[i], 0);
    return;
  }
  case vardef: {
    validate_no_asset_type(g, node->data.vardef.type,
                           node->data.vardef.name);
    ast_type declared = node->data.vardef.type->data.type;
    validate_sprite_expected(g, declared, node->data.vardef.expr,
                             node->data.vardef.name,
                             "Sprite declaration");
    if (!declared.is_array)
      validate_sprite_record_expr(g, declared.name.lexeme,
                                  node->data.vardef.expr);
    validate_asset_uses(g, node->data.vardef.expr, 0);
    return;
  }
  case assign: {
    if (node->data.assign.target &&
        node->data.assign.target->tag == arr_index &&
        node->data.assign.target->data.arr_index.has_field) {
      token_t where =
          token_for_expr(node->data.assign.target->data.arr_index.field_expr);
      error(where.filename, where.line, where.col,
            "assignment to a field through an array element is not supported");
    }
    string_view target_type =
        infer_expr_type(node->data.assign.target, g->table);
    if (svcmp(target_type, sv_from_cstr("Sprite")) == 0 &&
        !sprite_value_expr(g, node->data.assign.expr)) {
      token_t tok = token_for_expr(node->data.assign.expr);
      error(tok.filename, tok.line, tok.col,
            "Sprite assignment requires a Sprite value");
    }
    int empty_array = node->data.assign.expr &&
                      node->data.assign.expr->tag == literal &&
                      node->data.assign.expr->data.literal.lit.type ==
                          TOK_ARR_DECL;
    if (svcmp(target_type, sv_from_cstr("Sprite_array")) == 0 &&
        !empty_array && !sprite_array_expr(g, node->data.assign.expr)) {
      token_t tok = token_for_expr(node->data.assign.expr);
      error(tok.filename, tok.line, tok.col,
            "Sprite array assignment requires a Sprite array value");
    }
    validate_sprite_record_expr(g, target_type, node->data.assign.expr);
    validate_asset_uses(g, node->data.assign.target, 0);
    validate_asset_uses(g, node->data.assign.expr, 0);
    return;
  }
  case op:
    validate_asset_uses(g, node->data.op.left, 0);
    validate_asset_uses(g, node->data.op.right, 0);
    return;
  case unary_op:
    validate_asset_uses(g, node->data.unary_op.operand, 0);
    return;
  case ret:
    if (g->current_fundef && g->current_fundef->tag == fundef &&
        g->current_fundef->data.fundef.ret_type &&
        g->current_fundef->data.fundef.ret_type->tag == type) {
      ast_type expected =
          g->current_fundef->data.fundef.ret_type->data.type;
      if (svcmp(expected.name.lexeme, sv_from_cstr("Sprite")) == 0 &&
          !node->data.ret.expr) {
        token_t where = g->current_fundef->data.fundef.name;
        error(where.filename, where.line, where.col,
              "Sprite-returning function requires an explicit return value");
      } else {
        validate_sprite_expected(g, expected, node->data.ret.expr,
                                 token_for_expr(node->data.ret.expr),
                                 "return expression");
      }
    }
    validate_asset_uses(g, node->data.ret.expr, 0);
    return;
  case ifstmt:
    validate_asset_uses(g, node->data.ifstmt.expression, 0);
    validate_asset_uses(g, node->data.ifstmt.body, 0);
    validate_asset_uses(g, node->data.ifstmt.elsestmt, 0);
    return;
  case while_loop:
    validate_asset_uses(g, node->data.while_loop.condition, 0);
    validate_asset_uses(g, node->data.while_loop.statement, 0);
    return;
  case loop:
    validate_asset_uses(g, node->data.loop.start, 0);
    validate_asset_uses(g, node->data.loop.end, 0);
    new_nt_scope(&g->table);
    push_nt(&g->table, node->data.loop.variable.lexeme, NT_VAR, node);
    validate_asset_uses(g, node->data.loop.statement, 0);
    end_nt_scope(&g->table);
    return;
  case iter_loop:
    validate_asset_uses(g, node->data.iter_loop.iterable, 0);
    (void)get_array_element_type(node->data.iter_loop.iterable, g->table,
                                 node->data.iter_loop.variable);
    new_nt_scope(&g->table);
    push_nt(&g->table, node->data.iter_loop.variable.lexeme, NT_VAR, node);
    validate_asset_uses(g, node->data.iter_loop.statement, 0);
    end_nt_scope(&g->table);
    return;
  case sub:
    validate_asset_uses(g, node->data.sub.receiver, 0);
    validate_asset_uses(g, node->data.sub.expr, 0);
    return;
  case arr_index:
    validate_asset_uses(g, node->data.arr_index.array, 0);
    validate_asset_uses(g, node->data.arr_index.index, 0);
    validate_asset_uses(g, node->data.arr_index.field_expr, 0);
    return;
  case record_expr:
    for (int i = 0; i < node->data.record_expr.exprs.length; i++)
      validate_asset_uses(g, node->data.record_expr.exprs.data[i], 0);
    return;
  case match:
    validate_asset_uses(g, node->data.match.expr, 0);
    for (int i = 0; i < node->data.match.cases.length; i++)
      validate_asset_uses(g, node->data.match.cases.data[i], 0);
    return;
  case matchcase:
    validate_asset_uses(g, node->data.matchcase.expr, 0);
    validate_asset_uses(g, node->data.matchcase.body, 0);
    return;
  case tdef:
    for (int i = 0; i < node->data.tdef.constructors.length; i++) {
      ast_t member = node->data.tdef.constructors.data[i];
      if (member->tag == cons)
        validate_no_asset_type(g, member->data.cons.type,
                               member->data.cons.name);
    }
    for (int i = 0; i < node->data.tdef.module_fields.length; i++)
      validate_asset_uses(g, node->data.tdef.module_fields.data[i], 0);
    return;
  case asset_decl:
  case literal:
  case identifier:
  case enum_tdef:
  case embed:
  case collect_stmt:
  case cons:
  case type:
    return;
  }
}

static void resolve_method_calls(generator_t *g, ast_t node);

static ast_t find_nominal_member_type(name_table_t table, string_view owner,
                                      string_view member_name,
                                      int module_only) {
  ast_t owner_ref = get_ref_by_kind(owner, NT_USER_TYPE, table);
  if (!owner_ref || owner_ref->tag != tdef) return NULL;
  ast_tdef td = owner_ref->data.tdef;
  if (module_only && td.t != TDEF_MODULE) return NULL;
  ast_array_t members = td.t == TDEF_MODULE ? td.module_fields
                                            : td.constructors;
  for (int i = 0; i < members.length; i++) {
    ast_t member = members.data[i];
    token_t name = member->tag == vardef ? member->data.vardef.name
                                         : member->data.cons.name;
    if (svcmp(name.lexeme, member_name) != 0) continue;
    return member->tag == vardef ? member->data.vardef.type
                                 : member->data.cons.type;
  }
  return NULL;
}

static string_view declared_type_identity(ast_t type_node) {
  if (!type_node || type_node->tag != type) return sv_from_cstr("");
  ast_type declared = type_node->data.type;
  return declared.is_array ? make_array_type_sv(declared.name.lexeme)
                           : declared.name.lexeme;
}

static string_view value_ref_type(ast_t value_ref, string_view name,
                                  name_table_t table) {
  if (!value_ref) return sv_from_cstr("");
  if (value_ref->tag == vardef)
    return declared_type_identity(value_ref->data.vardef.type);
  if (value_ref->tag == fundef) {
    ast_fundef fd = value_ref->data.fundef;
    for (int i = 0; i < fd.args.length; i++) {
      if (svcmp(fd.args.data[i].lexeme, name) == 0)
        return declared_type_identity(fd.types.data[i]);
    }
  }
  if (value_ref->tag == iter_loop &&
      svcmp(value_ref->data.iter_loop.variable.lexeme, name) == 0)
    return get_array_element_type(value_ref->data.iter_loop.iterable, table,
                                  value_ref->data.iter_loop.variable);
  return sv_from_cstr("");
}

static ast_t find_user_function_overload(generator_t *g, string_view name,
                                         int arity) {
  if (!g->program || g->program->tag != program) return NULL;
  ast_array_t statements = g->program->data.program.prog;
  for (int i = 0; i < statements.length; i++) {
    ast_t candidate = statements.data[i];
    if (!candidate || candidate->tag != fundef) continue;
    ast_fundef function = candidate->data.fundef;
    if (function.method_kind == METHOD_NONE &&
        function.args.length == arity &&
        svcmp(function.name.lexeme, name) == 0)
      return candidate;
  }
  return NULL;
}

static void resolve_method_call(generator_t *g, ast_t node) {
  ast_method_call *mc = &node->data.method_call;
  if (mc->receiver->tag != identifier)
    resolve_method_calls(g, mc->receiver);
  for (int i = 0; i < mc->args.length; i++)
    resolve_method_calls(g, mc->args.data[i]);

  int type_receiver = 0;
  string_view owner = sv_from_cstr("");
  if (mc->receiver->tag == identifier) {
    string_view receiver_name = mc->receiver->data.identifier.id.lexeme;
    nt_lookup_t value_lookup =
        lookup_nt_by_kind(receiver_name, NT_VAR, g->table);
    ast_t value_ref = value_lookup.ref;
    ast_t type_ref = get_ref_by_kind(receiver_name, NT_USER_TYPE, g->table);
    if (type_ref == NULL)
      type_ref = get_ref_by_kind(receiver_name, NT_OPAQUE_TYPE, g->table);
    if (type_ref == NULL)
      type_ref = get_ref_by_kind(receiver_name, NT_NAMESPACE_TYPE, g->table);
    ast_t implicit_field_type = NULL;
    if ((!value_lookup.found || value_lookup.scope == 0) && g->current_fundef &&
        is_instance_method_kind(g->current_fundef->data.fundef.method_kind)) {
      implicit_field_type = find_nominal_member_type(
          g->table, g->current_fundef->data.fundef.type_name.lexeme,
          receiver_name, 1);
    }
    if ((!value_lookup.found || value_lookup.scope == 0) &&
        implicit_field_type == NULL && type_ref != NULL) {
      type_receiver = 1;
      owner = receiver_name;
    } else if (value_lookup.found && value_lookup.scope > 0) {
      owner = value_ref_type(value_ref, receiver_name, g->table);
    } else if (implicit_field_type != NULL) {
      owner = declared_type_identity(implicit_field_type);
    } else if (value_lookup.found) {
      owner = value_ref_type(value_ref, receiver_name, g->table);
    }
  }

  char *c_name = NULL;
  ast_t target = NULL;
  if (type_receiver) {
    c_name = mangle_type_method(owner, mc->method.lexeme, mc->args.length);
    target = get_ref_by_kind(sv_from_cstr(c_name), NT_FUN, g->table);
    if (target == NULL) {
      char *instance_name = mangle_method(owner, mc->method.lexeme, 0);
      if (get_ref_by_kind(sv_from_cstr(instance_name), NT_FUN, g->table)) {
        error(mc->method.filename, mc->method.line, mc->method.col,
              "'" SV_Fmt "." SV_Fmt
              "' is an instance method; call it through a value",
              SV_Arg(owner), SV_Arg(mc->method.lexeme));
      } else if (type_method_name_exists(g->program, owner, mc->method.lexeme) ||
                 native_method_name_exists(g, owner, mc->method.lexeme, 1)) {
        error(mc->method.filename, mc->method.line, mc->method.col,
              "type-level method '" SV_Fmt "." SV_Fmt
              "' has no overload taking %d argument(s)",
              SV_Arg(owner), SV_Arg(mc->method.lexeme), mc->args.length);
      } else {
        error(mc->method.filename, mc->method.line, mc->method.col,
              "undefined type-level method '" SV_Fmt "." SV_Fmt "'",
              SV_Arg(owner), SV_Arg(mc->method.lexeme));
      }
      return;
    }
    mc->resolved_kind = METHOD_TYPE_LEVEL;
  } else {
    if (owner.length == 0 && mc->receiver->tag != identifier)
      owner = infer_expr_type(mc->receiver, g->table);
    if (owner.length == 0) {
      if (mc->receiver->tag == identifier && g->current_fundef &&
          g->current_fundef->data.fundef.method_kind == METHOD_TYPE_LEVEL) {
        token_t receiver = mc->receiver->data.identifier.id;
        string_view current_owner =
            g->current_fundef->data.fundef.type_name.lexeme;
        if (find_nominal_member_type(g->table, current_owner,
                                     receiver.lexeme, 0)) {
          error(receiver.filename, receiver.line, receiver.col,
                "type-level method cannot access instance field '" SV_Fmt
                "' without a value receiver",
                SV_Arg(receiver.lexeme));
          return;
        }
      }
      error(mc->method.filename, mc->method.line, mc->method.col,
            "cannot determine type of receiver for method call '" SV_Fmt "'",
            SV_Arg(mc->method.lexeme));
      return;
    }
    c_name = mangle_method(owner, mc->method.lexeme, 0);
    target = get_ref_by_kind(sv_from_cstr(c_name), NT_FUN, g->table);
    if (target == NULL) {
      if (type_method_name_exists(g->program, owner, mc->method.lexeme) ||
          native_method_name_exists(g, owner, mc->method.lexeme, 1)) {
        error(mc->method.filename, mc->method.line, mc->method.col,
              "'" SV_Fmt "." SV_Fmt
              "' is a type-level method; call " SV_Fmt "." SV_Fmt "()",
              SV_Arg(owner), SV_Arg(mc->method.lexeme), SV_Arg(owner),
              SV_Arg(mc->method.lexeme));
      } else {
        error(mc->method.filename, mc->method.line, mc->method.col,
              "undefined instance method '" SV_Fmt "." SV_Fmt "'",
              SV_Arg(owner), SV_Arg(mc->method.lexeme));
      }
      return;
    }
    if (target->tag == fundef && target->data.fundef.body == NULL) {
      int expected = target->data.fundef.types.length - 1;
      if (expected != mc->args.length) {
        error(mc->method.filename, mc->method.line, mc->method.col,
              "instance method '" SV_Fmt "." SV_Fmt
              "' expects %d argument(s), got %d",
              SV_Arg(owner), SV_Arg(mc->method.lexeme), expected,
              mc->args.length);
        return;
      }
    }
    mc->resolved_kind =
        target->tag == fundef &&
                target->data.fundef.method_kind == METHOD_ARRAY_INSTANCE
            ? METHOD_ARRAY_INSTANCE
            : METHOD_INSTANCE;
  }

  if (target && target->tag == fundef && target->data.fundef.emitted_c_name)
    c_name = target->data.fundef.emitted_c_name;

  mc->is_resolved = 1;
  mc->resolved_owner = owner;
  mc->resolved_target = target;
  mc->resolved_c_name = c_name;
  if (mc->resolved_kind == METHOD_INSTANCE &&
      svcmp(owner, sv_from_cstr("Sprite")) == 0 &&
      svcmp(mc->method.lexeme, sv_from_cstr("frame")) == 0 &&
      mc->args.length > 0) {
    mc->resolved_asset = asset_for_expr(g, mc->args.data[0]);
    if (mc->resolved_asset) {
      component_asset_kind_spec *kind =
          asset_kind_for_decl(g, mc->resolved_asset);
      if (kind && strcmp(kind->kind, "sprite8") == 0)
        mc->resolved_c_name = "rift__im_L6_Sprite_L6_frame8_A2";
      else if (kind && strcmp(kind->kind, "sprite4") == 0)
        mc->resolved_c_name = "rift__im_L6_Sprite_L6_frame4_A2";
    }
  }
  record_fundef_component(g, target);
}

static void resolve_method_calls(generator_t *g, ast_t node) {
  if (!node) return;
  switch (node->tag) {
  case program: {
    ast_array_t stmts = node->data.program.prog;
    for (int i = 0; i < stmts.length; i++) resolve_method_calls(g, stmts.data[i]);
    return;
  }
  case fundef: {
    ast_fundef fd = node->data.fundef;
    ast_t saved_fundef = g->current_fundef;
    g->current_fundef = node;
    new_nt_scope(&g->table);
    for (int i = 0; i < fd.args.length; i++) {
      token_t arg = fd.args.data[i];
      if (!push_nt_unique(&g->table, arg.lexeme, NT_VAR, node))
        error(arg.filename, arg.line, arg.col,
              "duplicate parameter '" SV_Fmt "' in this scope",
              SV_Arg(arg.lexeme));
    }
    /* Parameters and declarations in the function's outermost block occupy
     * one C scope. Nested compound statements still open child scopes. */
    ast_array_t stmts = fd.body->data.compound.stmts;
    for (int i = 0; i < stmts.length; i++) {
      ast_t stmt = stmts.data[i];
      resolve_method_calls(g, stmt);
      if (stmt->tag == vardef) {
        token_t name = stmt->data.vardef.name;
        if (!push_nt_unique(&g->table, name.lexeme, NT_VAR, stmt))
          error(name.filename, name.line, name.col,
                "duplicate variable '" SV_Fmt "' in this scope",
                SV_Arg(name.lexeme));
      }
    }
    end_nt_scope(&g->table);
    g->current_fundef = saved_fundef;
    return;
  }
  case compound: {
    new_nt_scope(&g->table);
    ast_array_t stmts = node->data.compound.stmts;
    for (int i = 0; i < stmts.length; i++) {
      ast_t stmt = stmts.data[i];
      resolve_method_calls(g, stmt);
      if (stmt->tag == vardef) {
        token_t name = stmt->data.vardef.name;
        if (!push_nt_unique(&g->table, name.lexeme, NT_VAR, stmt))
          error(name.filename, name.line, name.col,
                "duplicate variable '" SV_Fmt "' in this scope",
                SV_Arg(name.lexeme));
      }
    }
    end_nt_scope(&g->table);
    return;
  }
  case method_call:
    resolve_method_call(g, node);
    return;
  case funcall: {
    for (int i = 0; i < node->data.funcall.args.length; i++)
      resolve_method_calls(g, node->data.funcall.args.data[i]);
    ast_funcall call = node->data.funcall;
    char *name = string_of_sv(call.name.lexeme);
    int native_name = 0;
    for (int i = 0; i < g->components->interface_count; i++) {
      component_interface_spec *entry = &g->components->interfaces[i];
      int callable = entry->kind == COMPONENT_BUILTIN ||
                     entry->kind == COMPONENT_LOWERED_BUILTIN ||
                     entry->kind == COMPONENT_OPAQUE;
      if (callable && strcmp(entry->name, name) == 0) {
        native_name = 1;
        break;
      }
    }
    ast_t target = find_user_function_overload(
        g, call.name.lexeme, call.args.length);
    if (!target)
      target = get_ref_by_kind(call.name.lexeme, NT_FUN, g->table);
    int user_target = target && target->tag == fundef &&
                      target->data.fundef.body != NULL;
    component_interface_spec *exact =
        find_builtin_interface(g->components, name, call.args.length);
    if (!user_target && native_name && !exact)
      error(call.name.filename, call.name.line, call.name.col,
            "standard function '" SV_Fmt
            "' has no overload taking %d argument(s)",
            SV_Arg(call.name.lexeme), call.args.length);
    if (!user_target && exact)
      target = make_native_fundef(exact, METHOD_NONE);
    node->data.funcall.resolved_target = target;
    return;
  }
  case vardef:
    if (is_opaque_type(node->data.vardef.type->data.type.name.lexeme, g) &&
        node->data.vardef.expr && node->data.vardef.expr->tag == record_expr) {
      token_t name = node->data.vardef.name;
      error(name.filename, name.line, name.col,
            "standard opaque type '" SV_Fmt
            "' cannot be constructed with an aggregate literal",
            SV_Arg(node->data.vardef.type->data.type.name.lexeme));
      return;
    }
    resolve_method_calls(g, node->data.vardef.expr);
    return;
  case assign:
    if (node->data.assign.expr && node->data.assign.expr->tag == record_expr &&
        is_opaque_type(infer_expr_type(node->data.assign.target, g->table), g)) {
      token_t target = token_for_expr(node->data.assign.target);
      error(target.filename, target.line, target.col,
            "standard opaque values cannot be assigned an aggregate literal");
      return;
    }
    resolve_method_calls(g, node->data.assign.target);
    resolve_method_calls(g, node->data.assign.expr);
    return;
  case op:
    resolve_method_calls(g, node->data.op.left);
    resolve_method_calls(g, node->data.op.right);
    return;
  case unary_op:
    resolve_method_calls(g, node->data.unary_op.operand);
    return;
  case ret:
    resolve_method_calls(g, node->data.ret.expr);
    return;
  case ifstmt:
    resolve_method_calls(g, node->data.ifstmt.expression);
    resolve_method_calls(g, node->data.ifstmt.body);
    resolve_method_calls(g, node->data.ifstmt.elsestmt);
    return;
  case while_loop:
    resolve_method_calls(g, node->data.while_loop.condition);
    resolve_method_calls(g, node->data.while_loop.statement);
    return;
  case loop:
    resolve_method_calls(g, node->data.loop.start);
    resolve_method_calls(g, node->data.loop.end);
    new_nt_scope(&g->table);
    push_nt(&g->table, node->data.loop.variable.lexeme, NT_VAR, node);
    resolve_method_calls(g, node->data.loop.statement);
    end_nt_scope(&g->table);
    return;
  case iter_loop:
    resolve_method_calls(g, node->data.iter_loop.iterable);
    new_nt_scope(&g->table);
    push_nt(&g->table, node->data.iter_loop.variable.lexeme, NT_VAR, node);
    resolve_method_calls(g, node->data.iter_loop.statement);
    end_nt_scope(&g->table);
    return;
  case sub:
    resolve_method_calls(g, node->data.sub.receiver);
    if (is_opaque_type(infer_expr_type(node->data.sub.receiver, g->table), g)) {
      token_t field = node->data.sub.expr && node->data.sub.expr->tag == identifier
                          ? node->data.sub.expr->data.identifier.id
                          : token_for_expr(node->data.sub.receiver);
      error(field.filename, field.line, field.col,
            "fields of standard opaque type cannot be accessed");
      return;
    }
    /* A terminal identifier is a qualified field label, not an unqualified
     * value read in the current method body. More complex terminal
     * expressions still need their calls/operands resolved. */
    if (node->data.sub.expr && node->data.sub.expr->tag != identifier)
      resolve_method_calls(g, node->data.sub.expr);
    return;
  case arr_index:
    resolve_method_calls(g, node->data.arr_index.array);
    resolve_method_calls(g, node->data.arr_index.index);
    if (node->data.arr_index.has_field &&
        is_opaque_type(get_array_element_type(node->data.arr_index.array,
                                              g->table,
                                              token_for_expr(node)), g)) {
      token_t field = token_for_expr(node->data.arr_index.field_expr);
      error(field.filename, field.line, field.col,
            "fields of standard opaque type cannot be accessed");
      return;
    }
    if (node->data.arr_index.field_expr &&
        node->data.arr_index.field_expr->tag != identifier)
      resolve_method_calls(g, node->data.arr_index.field_expr);
    return;
  case record_expr:
    for (int i = 0; i < node->data.record_expr.exprs.length; i++)
      resolve_method_calls(g, node->data.record_expr.exprs.data[i]);
    return;
  case match:
    resolve_method_calls(g, node->data.match.expr);
    for (int i = 0; i < node->data.match.cases.length; i++)
      resolve_method_calls(g, node->data.match.cases.data[i]);
    return;
  case matchcase:
    /* A bare match arm identifier names a union variant. It is not an
     * implicit field read, even when it shares an owner's field spelling. */
    if (node->data.matchcase.expr &&
        node->data.matchcase.expr->tag != identifier)
      resolve_method_calls(g, node->data.matchcase.expr);
    resolve_method_calls(g, node->data.matchcase.body);
    return;
  case tdef:
    for (int i = 0; i < node->data.tdef.module_fields.length; i++)
      resolve_method_calls(g, node->data.tdef.module_fields.data[i]);
    return;
  case identifier:
    if (g->current_fundef &&
        g->current_fundef->data.fundef.method_kind == METHOD_TYPE_LEVEL) {
      token_t id = node->data.identifier.id;
      if (svcmp(id.lexeme, sv_from_cstr("this")) == 0) {
        error(id.filename, id.line, id.col,
              "type-level methods have no 'this' receiver");
        return;
      }
      if (get_ref_by_kind(id.lexeme, NT_VAR, g->table) == NULL) {
        string_view owner = g->current_fundef->data.fundef.type_name.lexeme;
        if (find_nominal_member_type(g->table, owner, id.lexeme, 0)) {
          error(id.filename, id.line, id.col,
                "type-level method cannot access instance field '" SV_Fmt
                "' without a value receiver",
                SV_Arg(id.lexeme));
          return;
        }
      }
    }
    return;
  default:
    return;
  }
}

void semantic_prepare_program(generator_t *g, ast_t program) {
  validate_sprite_variant_names(program);
  register_program_symbols(g, program);
  register_asset_declarations(g, program);
  resolve_method_calls(g, program);
  validate_asset_uses(g, program, 0);
}
