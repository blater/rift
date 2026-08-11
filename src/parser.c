#include "parser.h"
#include "lib/alloc.h"
#include "ast.h"
#include "error.h"
#include "lexer.h"
#include "stringview.h"
#include "token.h"
#include <limits.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *includes[1024];
int includes_num = 0;

// Registry of module type names (for synthesize_default)
static char *module_type_names[256];
static int module_type_count = 0;

static void register_module_type(string_view name) {
  if (module_type_count >= 256) {
    fprintf(stderr, "error: too many module types (max 256)\n");
    exit(1);
  }
  module_type_names[module_type_count++] = string_of_sv(name);
}

static int is_module_typename(string_view name) {
  for (int i = 0; i < module_type_count; i++) {
    if (svcmp(name, sv_from_cstr(module_type_names[i])) == 0)
      return 1;
  }
  return 0;
}


ast_t parse_expression(parser_t *p);
ast_t parse_type(parser_t *p);
ast_t parse_fundef(parser_t *p);
static int is_new_style_vardef(parser_t p);
static ast_t parse_new_style_var_def(parser_t *p);
static ast_t parse_module(parser_t *p);
ast_t parse_match(parser_t *p);
ast_t parse_loop(parser_t *p);
ast_t parse_compound(parser_t *p);
int is_sub(parser_t p);
ast_t parse_statement(parser_t *p);
ast_t parse_while_loop(parser_t *p);
ast_t parse_iter_loop(parser_t *p);
ast_t parse_leaf(parser_t *p);
ast_t parse_embed(parser_t *p);
static ast_t parse_graphics_statement(parser_t *p);

void print_error_prefix(parser_t p) {
  token_t tok = peek_token(p);
  printf("%s:%d:%d:", tok.filename, tok.line, tok.col);
}

token_type_t peek_type(parser_t p) {
  if (p.cursor >= p.tokens.length) {
    return TOK_EOF;  // Return EOF if we're past the end
  }
  return p.tokens.data[p.cursor].type;
}

token_t peek_token(parser_t p) {
  if (p.cursor >= p.tokens.length) {
    // Return a fake EOF token with position info from the last token
    static token_t eof_token = {
      .type = TOK_EOF,
      .lexeme = {.data = (char*)"<EOF>", .length = 5},
      .filename = "unknown",
      .line = 0,
      .col = 0
    };
    if (p.tokens.length > 0) {
      eof_token.filename = p.tokens.data[p.tokens.length - 1].filename;
      eof_token.line = p.tokens.data[p.tokens.length - 1].line;
      eof_token.col = p.tokens.data[p.tokens.length - 1].col;
    }
    return eof_token;
  }
  return p.tokens.data[p.cursor];
}

token_t consume_token(parser_t *p) {
  if (p->cursor < 0)
    return (token_t){0};
  return p->tokens.data[p->cursor++];
}

parser_t new_parser(token_array_t tokens) {
  parser_t res;
  res.cursor = 0;
  res.prog = NULL;
  res.tokens = tokens;
  res.source = NULL;
  res.source_length = 0;
  res.scope_depth = 0;
  return res;
}

void expect(parser_t p, token_type_t b) {
  token_type_t a = peek_type(p);
  if (a != b) {
    token_t tok = peek_token(p);
    error(tok.filename, tok.line, tok.col, "Expected " SV_Fmt ", found: " SV_Fmt,
          SV_Arg(lexeme_of_type(b)), SV_Arg(lexeme_of_type(a)));
  }
}

void expect_one_of(parser_t p, token_type_t *valid, int count) {
  token_type_t actual = peek_type(p);

  // Check if actual token is in valid list
  for (int i = 0; i < count; i++) {
    if (valid[i] == actual) return;
  }

  // Error: generate message from valid token list
  token_t tok = peek_token(p);
  printf("%s:%d:%d: error: Expected ", tok.filename, tok.line, tok.col);

  for (int i = 0; i < count; i++) {
    if (i > 0) {
      if (i == count - 1) {
        printf(" or ");
      } else {
        printf(", ");
      }
    }
    printf(SV_Fmt, SV_Arg(lexeme_of_type(valid[i])));
  }

  printf(", found: " SV_Fmt "\n", SV_Arg(lexeme_of_type(actual)));
  exit(1);
}

int is_assign(parser_t p) {
  int scope = 0;
  token_type_t current = consume_token(&p).type;
  if (current == TOK_OPEN_BRACE || current == TOK_CLOSE_BRACE)
    return 0;
  while (scope >= 0 && current != TOK_GETS) {
    if (current == TOK_OPEN_PAREN || current == TOK_OPEN_BRACKET)
      scope++;
    else if (current == TOK_CLOSE_PAREN || current == TOK_CLOSE_BRACKET)
      scope--;
    else if (current == TOK_COMMA)
      break;
    else if (current == TOK_SEMICOL)
      break;
    else if (current == TOK_COLON)
      break;
    else if (current == TOK_SMALL_ARROW || current == TOK_TO)
      break;
    else if (current == TOK_LOOP || current == TOK_FOR)
      break;
    else if (current == TOK_EOF)  // Safety: don't loop past EOF
      break;
    current = consume_token(&p).type;
  }
  return current == TOK_GETS;
}

ast_t parse_assign(parser_t *p) {
  expect(*p, TOK_IDENTIFIER);
  ast_t target = parse_expression(p);
  token_type_t op = peek_type(*p);
  if (op != TOK_GETS) {
    token_t tok = peek_token(*p);
    printf("%s:%d:%d: error: Expected ':=' in assignment, got " SV_Fmt "\n",
           tok.filename, tok.line, tok.col, SV_Arg(lexeme_of_type(op)));
    exit(1);
  }
  consume_token(p);
  ast_t expr = parse_expression(p);
  expect(*p, TOK_SEMICOL);
  consume_token(p);
  return new_ast(
      (node_t){assign, {.assign = {.expr = expr, .target = target}}});
}

ast_t parse_cons(parser_t *p) {
  // Name : type [,]
  expect(*p, TOK_IDENTIFIER);
  token_t name = consume_token(p);
  expect(*p, TOK_COLON);
  consume_token(p);
  ast_t type = parse_type(p);
  if (peek_type(*p) == TOK_COMMA)
    consume_token(p);
  return new_ast((node_t){cons, {.cons = {name, type}}});
}

static ast_t parse_record_field(parser_t *p) {
  ast_t type = parse_type(p);
  expect(*p, TOK_IDENTIFIER);
  token_t name = consume_token(p);
  if (peek_type(*p) == TOK_COMMA)
    consume_token(p);
  return new_ast((node_t){cons, {.cons = {name, type}}});
}

ast_t parse_tdef(parser_t *p) {
  tdef_type_t type = -1;
  if (peek_type(*p) == TOK_REC)
    type = TDEF_REC;
  else
    expect(*p, TOK_REC); // Error reporting purposes
  consume_token(p);
  expect(*p, TOK_IDENTIFIER);
  token_t name = consume_token(p);
  expect(*p, TOK_OPEN_BRACE);
  consume_token(p);
  ast_array_t conss = new_ast_array();
  while (peek_type(*p) != TOK_CLOSE_BRACE) {
    if (type == TDEF_REC)
      push_ast_array(&conss, parse_record_field(p));
    else
      push_ast_array(&conss, parse_cons(p));
  }
  consume_token(p);
  return new_ast((node_t){tdef, {.tdef = {name, type, conss, new_ast_array()}}});
}

ast_t parse_ret(parser_t *p) {
  expect(*p, TOK_RETURN);
  consume_token(p);
  if (peek_type(*p) == TOK_SEMICOL) {
    consume_token(p);
    return new_ast((node_t){ret, {.ret = {NULL}}});
  }
  ast_t expr = parse_expression(p);
  expect(*p, TOK_SEMICOL);
  consume_token(p);
  return new_ast((node_t){ret, {.ret = {expr}}});
}

ast_t parse_literal(parser_t *p) {
  return new_ast((node_t){literal, {.literal = {.lit = consume_token(p)}}});
}

// Inject `receiver` at the base of a method_call chain.
// If expr is a funcall, wrap it as method_call{receiver, ...}.
// If expr is a method_call, recurse into its receiver.
// Returns NULL if expr is neither.
static ast_t inject_method_receiver(ast_t receiver, ast_t expr) {
  if (expr->tag == funcall) {
    ast_funcall call = expr->data.funcall;
    return new_ast((node_t){method_call, {.method_call = {receiver, call.name, call.args}}});
  }
  if (expr->tag == method_call) {
    ast_method_call mc = expr->data.method_call;
    ast_t new_recv = inject_method_receiver(receiver, mc.receiver);
    if (!new_recv) return NULL;
    mc.receiver = new_recv;
    return new_ast((node_t){method_call, {.method_call = mc}});
  }
  return NULL;
}

static ast_t make_field_access(ast_t receiver, token_t field) {
  ast_t field_expr = new_ast((node_t){identifier, {.identifier = {field}}});
  return new_ast((node_t){sub,
                  {.sub = {.receiver = receiver,
                           .path = new_token_array(),
                           .expr = field_expr}}});
}

ast_t parse_sub(parser_t *p) {
  token_t base = consume_token(p);
  ast_t receiver = new_ast((node_t){identifier, {.identifier = {base}}});
  token_array_t path = new_token_array();
  while (peek_type(*p) == TOK_DBLCOLON || peek_type(*p) == TOK_DOT) {
    consume_token(p);
    if (is_sub(*p))
      token_array_push(&path, consume_token(p));
    else
      break;
  }
  ast_t expr = parse_expression(p);

  // Method call: path[0..n].method(args) → ast_method_call
  // Also handles chained calls: path.method1().method2() → nested method_call
  if (expr->tag == funcall || expr->tag == method_call) {
    ast_t method_receiver;
    if (path.length == 0) {
      method_receiver = receiver;
    } else {
      // a.b.method() → receiver = sub{receiver:a, expr:identifier("b")}
      token_array_t recv_path = new_token_array();
      for (int i = 0; i < path.length - 1; i++) {
        token_array_push(&recv_path, path.data[i]);
      }
      ast_t last = new_ast((node_t){identifier,
                            {.identifier = {path.data[path.length - 1]}}});
      method_receiver = new_ast((node_t){sub,
                                 {.sub = {.receiver = receiver,
                                          .path = recv_path,
                                          .expr = last}}});
    }
    ast_t result = inject_method_receiver(method_receiver, expr);
    if (result) return result;
  }

  return new_ast((node_t){sub,
                  {.sub = {.receiver = receiver,
                           .expr = expr,
                           .path = path}}});
}

ast_t parse_if(parser_t *p) {
  consume_token(p);
  ast_t condition = parse_expression(p);
  // 'then' is optional
  if (peek_type(*p) == TOK_THEN) {
    consume_token(p);
  }
  ast_t body = parse_statement(p);
  ast_t else_body = NULL;
  if (peek_type(*p) == TOK_ELSE) {
    consume_token(p);
    else_body = parse_statement(p);
  }
  return new_ast((node_t){ifstmt,
                          {.ifstmt = {
                               .body = body,
                               .elsestmt = else_body,
                               .expression = condition,
                           }}});
}

ast_t parse_embed(parser_t *p) {
  token_t embed_tok = consume_token(p);  // consume @embed

  // Language and body are already extracted by lexer and stored in token
  char *lang = embed_tok.embed_lang ? embed_tok.embed_lang : "c";
  char *body = embed_tok.embed_body ? embed_tok.embed_body : "";

  // Determine if we're at top-level (file scope) or inline (inside function/block)
  int is_function = (p->scope_depth == 0) ? 1 : 0;

  return new_ast((node_t){embed, {.embed = {
    .lang = lang,
    .body = body,
    .is_function = is_function
  }}});
}

static ast_t make_void_type_ast(token_t origin) {
  token_t void_tok = origin;
  void_tok.lexeme = sv_from_cstr("void");
  void_tok.type = TOK_IDENTIFIER;
  return new_ast((node_t){type, {.type = {void_tok, 0, 0}}});
}

// Consume `KEYWORD IDENT {`, returning the IDENT. Leaves `*p` positioned at
// the first token inside the braces.
static token_t parse_tdef_header(parser_t *p, token_type_t keyword) {
  expect(*p, keyword);
  consume_token(p);
  expect(*p, TOK_IDENTIFIER);
  token_t name = consume_token(p);
  expect(*p, TOK_OPEN_BRACE);
  consume_token(p);
  return name;
}

static ast_t parse_union_variant(parser_t *p) {
  expect(*p, TOK_IDENTIFIER);
  parser_t la = *p;
  consume_token(&la);
  if (peek_type(la) == TOK_ARR_DECL) consume_token(&la);
  else if (peek_type(la) == TOK_OPEN_BRACKET) {
    consume_token(&la);
    if (peek_type(la) == TOK_NUM_LIT) consume_token(&la);
    if (peek_type(la) == TOK_CLOSE_BRACKET) consume_token(&la);
  }
  int is_typed = (peek_type(la) == TOK_IDENTIFIER);

  token_t name;
  ast_t payload_type;
  if (is_typed) {
    payload_type = parse_type(p);
    expect(*p, TOK_IDENTIFIER);
    name = consume_token(p);
  } else {
    name = consume_token(p);
    payload_type = make_void_type_ast(name);
  }
  if (peek_type(*p) == TOK_COMMA) consume_token(p);
  return new_ast((node_t){cons, {.cons = {name, payload_type}}});
}

ast_t parse_union(parser_t *p) {
  token_t name = parse_tdef_header(p, TOK_UNION);
  ast_array_t conss = new_ast_array();
  while (peek_type(*p) != TOK_CLOSE_BRACE) {
    push_ast_array(&conss, parse_union_variant(p));
  }
  consume_token(p);
  return new_ast((node_t){tdef, {.tdef = {name, TDEF_PRO, conss, new_ast_array()}}});
}

ast_t parse_enum(parser_t *p) {
  token_t name = parse_tdef_header(p, TOK_ENUM);
  token_array_t elems = new_token_array();
  while (peek_type(*p) != TOK_CLOSE_BRACE) {
    expect(*p, TOK_IDENTIFIER);
    token_array_push(&elems, consume_token(p));
    if (peek_type(*p) != TOK_COMMA) break;
    consume_token(p);
  }
  expect(*p, TOK_CLOSE_BRACE);
  consume_token(p);
  return new_ast((node_t){enum_tdef, {.enum_tdef = {name, elems}}});
}

ast_t parse_statement(parser_t *p) {
  token_type_t a = peek_type(*p);
  // Stop parsing when we reach EOF
  if (a == TOK_EOF)
    return NULL;
  if (a == TOK_IF)
    return parse_if(p);
  else if (a == TOK_ENUM)
    return parse_enum(p);
  else if (a == TOK_UNION)
    return parse_union(p);
  else if (a == TOK_REC)
    return parse_tdef(p);
  else if (a == TOK_MODULE)
    return parse_module(p);
  else if (a == TOK_WHILE)
    return parse_while_loop(p);
  else if (a == TOK_SUB) {
    // Check sub before is_assign — is_assign scans ahead and could find := inside a function body
    return parse_fundef(p);
  }
  else if (a == TOK_IDENTIFIER && is_new_style_vardef(*p))
    return parse_new_style_var_def(p);
  else if (is_assign(*p))
    return parse_assign(p);
  else if (a == TOK_FOR) {
    // 'for' handles both counter loops (for i := 0 to N) and iterator loops (for x in arr)
    {
      // Lookahead to distinguish: for x:= arr vs for i:= 0 to 10 vs for x in arr
      parser_t p_lookahead = *p;
      consume_token(&p_lookahead);  // skip 'for'
      if (peek_type(p_lookahead) == TOK_IDENTIFIER) {
        consume_token(&p_lookahead);  // skip identifier
        token_type_t sep = peek_type(p_lookahead);
        if (sep == TOK_IN) {
          // for x in arr — unambiguously an iterator loop
          return parse_iter_loop(p);
        }
        if (sep == TOK_GETS) {
          // Both counter and iterator loops use :=
          // Scan ahead for 'to' operator to detect counter loop
          consume_token(&p_lookahead);  // skip ':='
          int paren_depth = 0;
          int lookahead_limit = 100;  // prevent infinite loop
          while (lookahead_limit > 0) {
            token_type_t t = peek_type(p_lookahead);
            // Check for statement/expression boundaries
            if (t == TOK_OPEN_BRACE) {
              // End of expression, no range operator found - item loop
              return parse_iter_loop(p);
            }
            // Check for counter loop range operator (only at depth 0)
            if (paren_depth == 0 && t == TOK_TO) {
              // Found counter loop operator: for i:= 0 to ...
              return parse_loop(p);
            }
            // Track parenthesis depth to skip operators inside calls
            if (t == TOK_OPEN_PAREN || t == TOK_OPEN_BRACKET) {
              paren_depth++;
            } else if (t == TOK_CLOSE_PAREN || t == TOK_CLOSE_BRACKET) {
              paren_depth--;
            }
            consume_token(&p_lookahead);
            lookahead_limit--;
          }
          // Default to item loop if we reach end
          return parse_iter_loop(p);
        }
      }
    }
    return parse_loop(p);
  }
  else if (a == TOK_RETURN) {
    return parse_ret(p);
  } else if (a == TOK_MATCH)
    return parse_match(p);
  else if (a == TOK_COLLECT) {
    consume_token(p);
    expect(*p, TOK_SEMICOL);
    consume_token(p);
    return new_ast((node_t){collect_stmt, {0}});
  }
  else if (a == TOK_EMBED)
    return parse_embed(p);
  else if (a == TOK_GRAPHICS)
    return parse_graphics_statement(p);
  else if (a == TOK_OPEN_BRACE) {
    return parse_compound(p);
  } else {
    ast_t expr = parse_expression(p);
    expect(*p, TOK_SEMICOL);
    consume_token(p);
    return expr;
  }
  return NULL;
}

static ast_t parse_graphics_statement(parser_t *p) {
  token_t graphics_tok = consume_token(p);
  expect(*p, TOK_IDENTIFIER);
  token_t mode_tok = consume_token(p);
  int is_on = svcmp(mode_tok.lexeme, sv_from_cstr("on")) == 0;
  int is_off = svcmp(mode_tok.lexeme, sv_from_cstr("off")) == 0;
  if (!is_on && !is_off) {
    error(mode_tok.filename, mode_tok.line, mode_tok.col,
          "Expected 'on' or 'off' after 'graphics'");
  }
  expect(*p, TOK_SEMICOL);
  consume_token(p);

  char *fn = is_on ? "graphics_on" : "graphics_off";
  graphics_tok.type = TOK_IDENTIFIER;
  graphics_tok.lexeme = sv_from_cstr(fn);
  return new_ast((node_t){funcall,
                          {.funcall = {graphics_tok, new_ast_array()}}});
}

ast_t parse_matchcase(parser_t *p) {
  ast_t expr;
  if (peek_type(*p) == TOK_DEFAULT) {
    // default: → synthesize a wildcard literal node
    token_t def_tok = consume_token(p);
    def_tok.type = TOK_WILDCARD;
    expr = new_ast((node_t){literal, {.literal = {def_tok}}});
  } else {
    expr = parse_expression(p);
  }
  expect(*p, TOK_COLON);
  consume_token(p);
  ast_t stmt = parse_statement(p);
  return new_ast(
      (node_t){matchcase, {.matchcase = {.body = stmt, .expr = expr}}});
}

ast_t parse_match(parser_t *p) {
  expect(*p, TOK_MATCH);
  consume_token(p);
  ast_t to_match = parse_expression(p);
  expect(*p, TOK_OPEN_BRACE);
  consume_token(p);
  ast_array_t cases = new_ast_array();
  while (peek_type(*p) != TOK_CLOSE_BRACE) {
    push_ast_array(&cases, parse_matchcase(p));
  }
  expect(*p, TOK_CLOSE_BRACE);
  consume_token(p);
  return new_ast(
      (node_t){match, {.match = {.cases = cases, .expr = to_match}}});
}

ast_t parse_loop(parser_t *p) {
  // Accept both 'loop' and 'for' keywords
  token_type_t t = peek_type(*p);
  if (t != TOK_FOR) {
    print_error_prefix(*p);
    printf("Expected 'for' keyword\n");
    exit(1);
  }
  consume_token(p);

  expect(*p, TOK_IDENTIFIER);
  token_t var_name = consume_token(p);

  // Accept ':=' for counter loops
  token_type_t sep = peek_type(*p);
  if (sep != TOK_GETS) {
    token_t tok = peek_token(*p);
    printf("%s:%d:%d: error: Expected ':=' in loop declaration\n",
           tok.filename, tok.line, tok.col);
    exit(1);
  }
  consume_token(p);

  ast_t begin = parse_expression(p);

  // Accept 'to' keyword
  token_type_t range_sep = peek_type(*p);
  if (range_sep != TOK_TO) {
    token_t tok = peek_token(*p);
    printf("%s:%d:%d: error: Expected 'to' in loop declaration\n",
           tok.filename, tok.line, tok.col);
    exit(1);
  }
  consume_token(p);

  ast_t end = parse_expression(p);

  ast_t stmt = parse_statement(p);
  return new_ast((node_t){loop, {.loop = {var_name, begin, end, stmt}}});
}

ast_t parse_while_loop(parser_t *p) {
  expect(*p, TOK_WHILE);
  consume_token(p);
  ast_t condition = parse_expression(p);
  // 'do' is optional - consume it if present
  if (peek_type(*p) == TOK_DO) {
    consume_token(p);
  }
  ast_t statement = parse_statement(p);
  return new_ast((node_t){
      while_loop,
      {.while_loop = {.statement = statement, .condition = condition}}});
}

ast_t parse_iter_loop(parser_t *p) {
  // Accept both 'iter' and 'for' keywords
  token_type_t t = peek_type(*p);
  if (t != TOK_FOR) {
    print_error_prefix(*p);
    printf("Expected 'for' keyword\n");
    exit(1);
  }
  consume_token(p);
  expect(*p, TOK_IDENTIFIER);
  token_t var_name = consume_token(p);
  // Accept both ':=' and 'in'
  token_type_t sep = peek_type(*p);
  if (sep != TOK_GETS && sep != TOK_IN) {
    token_t tok = peek_token(*p);
    printf("%s:%d:%d: error: Expected ':=' or 'in' in iterator loop declaration\n",
           tok.filename, tok.line, tok.col);
    exit(1);
  }
  consume_token(p);
  ast_t iterable = parse_expression(p);
  ast_t statement = parse_statement(p);
  return new_ast((node_t){
      iter_loop,
      {.iter_loop = {.variable = var_name, .iterable = iterable, .statement = statement}}});
}

ast_t parse_record_expression(parser_t *p) {
  token_array_t names = new_token_array();
  ast_array_t exprs = new_ast_array();
  while (peek_type(*p) != TOK_CLOSE_BRACE) {
    expect(*p, TOK_IDENTIFIER);
    token_t cons_name = consume_token(p);
    token_array_push(&names, cons_name);
    token_type_t sep = peek_type(*p);
    if (sep != TOK_GETS) {
      token_t tok = peek_token(*p);
      printf("%s:%d:%d: error: Expected ':=' in record initializer, got " SV_Fmt "\n",
             tok.filename, tok.line, tok.col, SV_Arg(lexeme_of_type(sep)));
      exit(1);
    }
    consume_token(p);
    ast_t expr = parse_expression(p);
    push_ast_array(&exprs, expr);
    if (peek_type(*p) != TOK_COMMA) {
      break;
    }
    consume_token(p);
  }
  return new_ast(
      (node_t){record_expr, {.record_expr = {.exprs = exprs, .names = names}}});
}

static ast_t synthesize_default(ast_t type_node) {
  ast_type *t = &type_node->data.type;
  token_t pos = t->name;  // use type name token for error location

  // Any array type: default to []
  if (t->is_array) {
    token_t tok = pos;
    tok.type = TOK_ARR_DECL;
    tok.lexeme = (string_view)SV_Static("[]");
    return new_ast((node_t){literal, {.literal = {tok}}});
  }

  string_view name = t->name.lexeme;

  // Numeric/boolean types: default to 0
  if (svcmp(name, (string_view)SV_Static("int"))     == 0 ||
      svcmp(name, (string_view)SV_Static("char"))    == 0 ||
      svcmp(name, (string_view)SV_Static("byte"))    == 0 ||
      svcmp(name, (string_view)SV_Static("word"))    == 0 ||
      svcmp(name, (string_view)SV_Static("dword"))   == 0 ||
      svcmp(name, (string_view)SV_Static("float"))   == 0 ||
      svcmp(name, (string_view)SV_Static("boolean")) == 0) {
    token_t tok = pos;
    tok.type = TOK_NUM_LIT;
    tok.lexeme = (string_view)SV_Static("0");
    return new_ast((node_t){literal, {.literal = {tok}}});
  }

  // String: default to ""
  if (svcmp(name, (string_view)SV_Static("string")) == 0) {
    token_t tok = pos;
    tok.type = TOK_STR_LIT;
    // The lexeme must include the quotes, so for empty string it's ""
    tok.lexeme = (string_view)SV_Static("\"\"");
    return new_ast((node_t){literal, {.literal = {tok}}});
  }

  // Module types: default to TypeName_new()
  if (is_module_typename(name)) {
    size_t nl = name.length;
    char *fn = allocate_compiler_persistent(nl + 5);
    memcpy(fn, name.data, nl);
    memcpy(fn + nl, "_new", 4);
    fn[nl + 4] = '\0';
    token_t tok = pos;
    tok.type = TOK_IDENTIFIER;
    tok.lexeme = sv_from_cstr(fn);
    ast_array_t no_args = new_ast_array();
    return new_ast((node_t){funcall, {.funcall = {tok, no_args}}});
  }

  // User-defined types: no sensible default
  error(pos.filename, pos.line, pos.col,
        "Type '" SV_Fmt "' has no default value; use explicit initialization",
        SV_Arg(name));
  return NULL;  // unreachable
}

// Lookahead predicate: does the current token stream start a C/Java-style vardef?
// Patterns:
//   IDENT IDENT ( := | ; )          →  type varname
//   IDENT ARR_DECL IDENT ( := | ; ) →  type[] varname
//   IDENT [ NUM ] IDENT ( := | ; )  →  type[N] varname
static int is_new_style_vardef(parser_t p) {
  if (consume_token(&p).type != TOK_IDENTIFIER) return 0;
  token_type_t t1 = peek_type(p);

  if (t1 == TOK_IDENTIFIER) {
    consume_token(&p);
    token_type_t t2 = peek_type(p);
    return t2 == TOK_GETS || t2 == TOK_SEMICOL;
  }
  if (t1 == TOK_ARR_DECL) {
    consume_token(&p);
    if (peek_type(p) != TOK_IDENTIFIER) return 0;
    consume_token(&p);
    token_type_t t3 = peek_type(p);
    return t3 == TOK_GETS || t3 == TOK_SEMICOL;
  }
  if (t1 == TOK_OPEN_BRACKET) {
    consume_token(&p);
    if (peek_type(p) != TOK_NUM_LIT) return 0;
    consume_token(&p);
    if (peek_type(p) != TOK_CLOSE_BRACKET) return 0;
    consume_token(&p);
    if (peek_type(p) != TOK_IDENTIFIER) return 0;
    consume_token(&p);
    token_type_t t5 = peek_type(p);
    return t5 == TOK_GETS || t5 == TOK_SEMICOL;
  }
  return 0;
}

// Parse a module declaration: module Name;
// Produces a tdef node with TDEF_MODULE type and no constructors.
static ast_t parse_module(parser_t *p) {
  consume_token(p);  // consume 'module'
  expect(*p, TOK_IDENTIFIER);
  token_t name = consume_token(p);
  expect(*p, TOK_SEMICOL);
  consume_token(p);
  register_module_type(name.lexeme);
  ast_array_t empty = new_ast_array();
  return new_ast((node_t){tdef, {.tdef = {name, TDEF_MODULE, empty, new_ast_array()}}});
}

// Parse a C/Java-style variable declaration: type varname [:= expr];
static ast_t parse_new_style_var_def(parser_t *p) {
  ast_t type = parse_type(p);

  expect(*p, TOK_IDENTIFIER);
  token_t id = consume_token(p);

  token_type_t op = peek_type(*p);
  int is_rec = 0;
  ast_t expr = NULL;

  if (op == TOK_GETS) {
    consume_token(p);
    if (peek_type(*p) == TOK_OPEN_BRACE) {
      is_rec = 1;
      consume_token(p);
      expr = parse_record_expression(p);
      expect(*p, TOK_CLOSE_BRACE);
      consume_token(p);
    } else if (peek_type(*p) == TOK_ARR_DECL) {
      expr = new_ast((node_t){literal, {.literal = {consume_token(p)}}});
    } else {
      expr = parse_expression(p);
    }
  } else if (op == TOK_SEMICOL) {
    expr = synthesize_default(type);
  } else {
    token_t tok = peek_token(*p);
    error(tok.filename, tok.line, tok.col,
          "Expected ':=' or ';' in variable declaration, got " SV_Fmt,
          SV_Arg(lexeme_of_type(op)));
  }

  expect(*p, TOK_SEMICOL);
  consume_token(p);
  return new_ast((node_t){
      vardef,
      {.vardef = {.expr = expr, .name = id, .type = type, .is_rec = is_rec}}});
}

ast_t parse_type(parser_t *p) {
  expect(*p, TOK_IDENTIFIER);
  token_t name = consume_token(p);
  int is_array = 0;
  int array_capacity = 0;

  // Check for array declaration: int[] or int[10]
  if (peek_type(*p) == TOK_ARR_DECL) {
    // Shorthand: int[]
    consume_token(p);
    is_array = 1;
    array_capacity = 0;

  } else if (peek_type(*p) == TOK_OPEN_BRACKET) {
    // Explicit brackets: int[10] or int[]
    consume_token(p);  // consume [
    is_array = 1;
    array_capacity = 0;

    // Check for size
    if (peek_type(*p) == TOK_NUM_LIT) {
      token_t size_tok = consume_token(p);
      // Convert string_view to integer
      char size_str[32];
      int len = size_tok.lexeme.length;
      if (len > 31) len = 31;
      for (int i = 0; i < len; i++) {
        size_str[i] = size_tok.lexeme.data[i];
      }
      size_str[len] = 0;
      array_capacity = atoi(size_str);
    }

    expect(*p, TOK_CLOSE_BRACKET);
    consume_token(p);  // consume ]
  }

  return new_ast((node_t){type, {.type = {name, is_array, array_capacity}}});
}

ast_t parse_compound(parser_t *p) {
  expect(*p, TOK_OPEN_BRACE);
  consume_token(p);
  p->scope_depth++;
  ast_array_t stmts = new_ast_array();
  while (peek_type(*p) != TOK_CLOSE_BRACE)
    push_ast_array(&stmts, parse_statement(p));
  p->scope_depth--;
  expect(*p, TOK_CLOSE_BRACE);
  consume_token(p);
  return new_ast((node_t){compound, {.compound = {stmts}}});
}

ast_t parse_fundef(parser_t *p) {
  // Only accept 'sub' for function definitions
  token_type_t t = peek_type(*p);
  if (t != TOK_SUB) {
    print_error_prefix(*p);
    printf("Expected 'sub' for function definition\n");
    exit(1);
  }
  consume_token(p);
  expect(*p, TOK_IDENTIFIER);
  token_t id = consume_token(p);

  // Method declaration: sub Type.method(...) or sub Type[].method(...)
  int is_method = 0;
  int is_array_method = 0;
  token_t type_name_tok = id;  // safe default (overwritten below if is_method)
  if (peek_type(*p) == TOK_ARR_DECL) {
    // Look ahead: expect [] (TOK_ARR_DECL) followed by .
    parser_t la = *p;
    consume_token(&la);  // consume []
    if (peek_type(la) == TOK_DOT) {
      // Confirmed: sub Type[].method(...)
      consume_token(p);  // consume []
      consume_token(p);  // consume .
      is_method = 1;
      is_array_method = 1;
      expect(*p, TOK_IDENTIFIER);
      id = consume_token(p);  // id is now the method name
    }
  } else if (peek_type(*p) == TOK_DOT) {
    consume_token(p);  // consume '.'
    is_method = 1;
    // type_name_tok already holds the type (e.g. "string")
    expect(*p, TOK_IDENTIFIER);
    id = consume_token(p);  // id is now the method name (e.g. "add")
  }

  expect(*p, TOK_OPEN_PAREN);
  consume_token(p);
  token_array_t args = new_token_array();
  ast_array_t types = new_ast_array();

  // Prepend implicit "this" parameter for method declarations
  if (is_method) {
    token_t this_tok = type_name_tok;
    this_tok.lexeme = sv_from_cstr("this");
    token_array_push(&args, this_tok);
    int this_is_array = is_array_method ? 1 : 0;
    ast_t this_type = new_ast((node_t){type, {.type = {type_name_tok, this_is_array, 0}}});
    push_ast_array(&types, this_type);
  }

  while (peek_type(*p) != TOK_CLOSE_PAREN) {
    ast_t param_type = parse_type(p);
    expect(*p, TOK_IDENTIFIER);
    token_t arg = consume_token(p);

    token_array_push(&args, arg);
    push_ast_array(&types, param_type);

    if (peek_type(*p) != TOK_COMMA)
      break;
    consume_token(p);
  }

  expect(*p, TOK_CLOSE_PAREN);
  consume_token(p);

  ast_t ret_type;
  if (peek_type(*p) == TOK_RETURNS) {
    consume_token(p);
    ret_type = parse_type(p);
  } else {
    ret_type = make_void_type_ast(id);
  }

  ast_t body = parse_compound(p);
  return new_ast((node_t){fundef,
                          {.fundef = {.args = args,
                                      .body = body,
                                      .name = id,
                                      .type_name = type_name_tok,
                                      .is_method = is_method,
                                      .is_array_method = is_array_method,
                                      .types = types,
                                      .ret_type = ret_type}}});
}

int is_primary(parser_t p) {
  token_type_t t = peek_type(p);
  return t == TOK_OPEN_BRACE || t == TOK_OPEN_PAREN || t == TOK_IDENTIFIER ||
         t == TOK_STR_LIT || t == TOK_CHR_LIT || t == TOK_NUM_LIT ||
         t == TOK_WILDCARD || t == TOK_ARR_DECL;
}

int is_funcall(parser_t p) {
  return consume_token(&p).type == TOK_IDENTIFIER &&
         peek_type(p) == TOK_OPEN_PAREN;
}

ast_t parse_funcall(parser_t *p) {
  token_t id = consume_token(p);
  expect(*p, TOK_OPEN_PAREN);
  consume_token(p);
  ast_array_t args = new_ast_array();
  while (peek_type(*p) != TOK_CLOSE_PAREN) {
    push_ast_array(&args, parse_expression(p));
    if (peek_type(*p) != TOK_COMMA)
      break;
    consume_token(p);
  }

  expect(*p, TOK_CLOSE_PAREN);
  consume_token(p);
  return new_ast((node_t){funcall, {.funcall = {id, args}}});
}

ast_t parse_leaf(parser_t *p) {
  token_t t = consume_token(p);
  if (is_funcall(*p))
    return parse_funcall(p);
  if (t.type == TOK_IDENTIFIER)
    return new_ast((node_t){identifier, {.identifier = {t}}});
  else if (t.type == TOK_NUM_LIT || t.type == TOK_STR_LIT ||
           t.type == TOK_CHR_LIT || t.type == TOK_WILDCARD ||
           t.type == TOK_ARR_DECL)
    return new_ast((node_t){literal, {.literal = {t}}});
  printf("%s:%d:%d: error: Expected leaf node (identifier or literal), got "
         SV_Fmt "\n",
         t.filename, t.line, t.col, SV_Arg(lexeme_of_type(t.type)));
  exit(1);
  return NULL; // For TCC
}

int is_sub(parser_t p) {
  if (consume_token(&p).type != TOK_IDENTIFIER)
    return 0;
  token_type_t next = consume_token(&p).type;
  if (next != TOK_DBLCOLON && next != TOK_DOT)
    return 0;
  return 1;
}

int is_subscript(parser_t p) {
  return consume_token(&p).type == TOK_IDENTIFIER &&
         peek_type(p) == TOK_OPEN_BRACKET;
}

ast_t parse_subscript(parser_t *p) {
  token_t id = consume_token(p);       // consume array name
  consume_token(p);                     // consume '['
  ast_t index = parse_expression(p);   // parse index
  expect(*p, TOK_CLOSE_BRACKET);
  consume_token(p);                     // consume ']'

  ast_t arr_id = new_ast((node_t){identifier, {.identifier = {id}}});
  token_array_t fp = new_token_array();
  ast_arr_index result = {arr_id, index, 0, fp, NULL};

  // Handle chained field access: arr[i].field or arr[i]::field
  if (peek_type(*p) == TOK_DOT || peek_type(*p) == TOK_DBLCOLON) {
    while (peek_type(*p) == TOK_DOT || peek_type(*p) == TOK_DBLCOLON) {
      consume_token(p);         // consume . or ::
      if (is_sub(*p))
        token_array_push(&result.field_path, consume_token(p));
      else
        break;
    }
    result.has_field = 1;
    result.field_expr = parse_expression(p);  // final field name

    // arr[idx].method(args) with no intermediate fields → method_call node
    if (result.field_path.length == 0 && result.field_expr->tag == funcall) {
      ast_t base = new_ast((node_t){arr_index,
                    {.arr_index = {arr_id, index, 0, new_token_array(), NULL}}});
      ast_funcall call = result.field_expr->data.funcall;
      return new_ast((node_t){method_call,
                      {.method_call = {base, call.name, call.args}}});
    }
  }

  return new_ast((node_t){arr_index, {.arr_index = result}});
}

ast_t parse_primary(parser_t *p) {
  token_type_t type = peek_type(*p);
  if (is_funcall(*p)) {
    return parse_funcall(p);
  } else if (is_subscript(*p)) {
    return parse_subscript(p);
  } else if (type == TOK_STR_LIT || type == TOK_CHR_LIT ||
             type == TOK_NUM_LIT || type == TOK_WILDCARD ||
             type == TOK_IDENTIFIER || type == TOK_ARR_DECL) {
    return parse_leaf(p);
  } else if (type == TOK_OPEN_PAREN) {
    (void)consume_token(p);
    ast_t res = parse_expression(p);
    type = peek_type(*p);

    if (type != TOK_CLOSE_PAREN) {
      token_t tok = peek_token(*p);
      printf("%s:%d:%d: error: Expected ')' to close parenthesis, got "
             SV_Fmt "\n",
             tok.filename, tok.line, tok.col, SV_Arg(lexeme_of_type(type)));
      exit(1);
    }
    (void)consume_token(p);
    return res;
  } else if (type == TOK_OPEN_BRACE) {
    return parse_record_expression(p);
  } else if (type == TOK_MINUS) {
    token_t op_tok = consume_token(p);
    ast_t operand = parse_primary(p);
    return new_ast((node_t){unary_op, {.unary_op = {op_tok.type, operand}}});
  } else {
    token_t tok = peek_token(*p);
    printf("%s:%d:%d: error: Expected an expression, got " SV_Fmt "\n",
           tok.filename, tok.line, tok.col, SV_Arg(lexeme_of_type(type)));
    exit(1);
  }
  return NULL; // For TCC
}

ast_t parse_increasing_precedence(parser_t *p, ast_t left, int min_prec);

ast_t parse_expression_aux(parser_t *p, int min_precedence);

ast_t parse_expression(parser_t *p) {
  token_type_t a = peek_type(*p);

  // Handle record expressions specially (they're always complete)
  if (a == TOK_OPEN_BRACE) {
    consume_token(p);
    return parse_record_expression(p);
  }
  // For all other cases (including parenthesized expressions), use expression_aux
  // This ensures operators after parenthesized sub-expressions are handled correctly
  // e.g., (3+1)-1 will parse as ((3+1)-1), not just (3+1)
  return parse_expression_aux(p, -1);
}

ast_t parse_expression_aux(parser_t *p, int min_precedence) {
  ast_t left = parse_primary(p);

  // Postfix field access, method calls, and indexing on arbitrary receivers.
  while (1) {
    token_type_t next = peek_type(*p);

    if (next == TOK_DOT || next == TOK_DBLCOLON) {
      token_type_t sep = consume_token(p).type;
      expect(*p, TOK_IDENTIFIER);
      token_t field = consume_token(p);

      if (sep == TOK_DOT && peek_type(*p) == TOK_OPEN_PAREN) {
        consume_token(p);  // consume '('
        ast_array_t args = new_ast_array();
        while (peek_type(*p) != TOK_CLOSE_PAREN) {
          push_ast_array(&args, parse_expression(p));
          if (peek_type(*p) != TOK_COMMA) break;
          consume_token(p);
        }
        expect(*p, TOK_CLOSE_PAREN);
        consume_token(p);
        left = new_ast((node_t){method_call, {.method_call = {left, field, args}}});
        continue;
      }

      left = make_field_access(left, field);
      continue;
    }

    if (next == TOK_OPEN_BRACKET) {
      consume_token(p);                   // consume '['
      ast_t index = parse_expression(p);  // parse index
      expect(*p, TOK_CLOSE_BRACKET);
      consume_token(p);                   // consume ']'

      left = new_ast((node_t){arr_index,
                      {.arr_index = {left, index, 0, new_token_array(), NULL}}});
      continue;
    }

    break;
  }

  while (1) {
    ast_t node = parse_increasing_precedence(p, left, min_precedence);
    if (node == left)
      break;
    left = node;
  }

  // Validate that the next token is valid after an expression
  token_type_t next = peek_type(*p);
  // Valid tokens after expression: terminators, separators, continuation operators
  if (next != TOK_SEMICOL && next != TOK_CLOSE_BRACE && next != TOK_EOF &&
      next != TOK_COMMA && next != TOK_CLOSE_PAREN && next != TOK_CLOSE_BRACKET &&
      next != TOK_SMALL_ARROW && next != TOK_TO &&
      next != TOK_OPEN_BRACE) {  // { is valid in loop contexts
    // Check if this looks like an unexpected expression start
    if (next == TOK_STR_LIT || next == TOK_CHR_LIT || next == TOK_NUM_LIT ||
        next == TOK_IDENTIFIER || next == TOK_OPEN_PAREN || next == TOK_OPEN_BRACKET) {
      token_t tok = peek_token(*p);

      // If the expression was an identifier followed by something that looks like
      // function arguments, suggest adding parentheses
      if (left->tag == identifier && (next == TOK_STR_LIT || next == TOK_CHR_LIT ||
                                       next == TOK_NUM_LIT || next == TOK_IDENTIFIER)) {
        token_t id_tok = left->data.identifier.id;
        printf("%s:%d:%d: error: After identifier '%.*s', expected '(' for function call, found: " SV_Fmt "\n",
               tok.filename, tok.line, tok.col,
               (int)id_tok.lexeme.length, id_tok.lexeme.data,
               SV_Arg(lexeme_of_type(next)));
      } else {
        printf("%s:%d:%d: error: Unexpected token after expression. Expected ;, }, ), or ,, found: " SV_Fmt "\n",
               tok.filename, tok.line, tok.col, SV_Arg(lexeme_of_type(next)));
      }
      exit(1);
    }
  }

  return left;
}

ast_t parse_increasing_precedence(parser_t *p, ast_t left, int min_prec) {
  token_t next = peek_token(*p);
  if (!is_type_operator(next.type))
    return left;
  int next_prec = get_precedence(next.type);
  if (next_prec < min_prec)
    return left;
  (void)consume_token(p);
  ast_t right = parse_expression_aux(p, next_prec);
  return new_ast((node_t){op, {.op = {next.type, left, right}}});
}

int has_been_included(char *filename) {
  for (int i = 0; i < includes_num; i++)
    if (strcmp(filename, includes[i]) == 0)
      return 1;
  return 0;
}

char *get_sub_string(char *s, size_t length) {
  if (s == NULL)
    return NULL;
  if (strlen(s) < length)
    return s;
  char *res = allocate_compiler_persistent(length + 1);
  memcpy(res, s, length);
  res[length] = 0;
  return res;
}

char *get_abs_path(char *s, char *base_dir) {
  char buffer[PATH_MAX];
  if (base_dir && s[0] != '/') {
    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, s);
    if (!realpath(full_path, buffer)) {
      fprintf(stderr, "error: cannot resolve path '%s'\n", full_path);
      exit(1);
    }
  } else {
    if (!realpath(s, buffer)) {
      fprintf(stderr, "error: cannot resolve path '%s'\n", s);
      exit(1);
    }
  }
  size_t l = strlen(buffer);
  char *res = allocate_compiler_persistent(l + 1);
  strcpy(res, buffer);
  return res;
}

void parse_program(parser_t *p) {
  ast_array_t prog = new_ast_array();
  ast_t last_module = NULL; // tracks the most recent module tdef for field collection
  while (p->cursor < p->tokens.length) {
    if (peek_type(*p) == TOK_EMBED) {
      ast_t stmt = parse_embed(p);
      push_ast_array(&prog, stmt);
      last_module = NULL;
      continue;
    }
    if (peek_type(*p) == TOK_INCLUDE) {
      consume_token(p);
      expect(*p, TOK_STR_LIT);
      token_t tok = consume_token(p);
      char tok_dir_buf[PATH_MAX];
      strcpy(tok_dir_buf, tok.filename);
      char *tok_dir = dirname(tok_dir_buf);
      char *abs_path = get_abs_path(
          get_sub_string(string_of_sv(tok.lexeme) + 1, tok.lexeme.length - 2),
          tok_dir);
      if (!has_been_included(abs_path)) {
        if (includes_num >= 1024) {
          printf("%s:%d:%d: error: Include limit reached (max 1024 files)\n",
                 tok.filename, tok.line, tok.col);
          exit(1);
        }
        includes[includes_num++] = abs_path;

        lexer_t l = new_lexer(abs_path);
        token_array_t toks = lex_program(&l);

        // All included files must begin with a module declaration
        if (toks.length == 0 || toks.data[0].type != TOK_MODULE) {
          error(tok.filename, tok.line, tok.col,
                "included file '%s' must start with a module declaration (module Name;)",
                abs_path);
        }

        token_array_t new_toks = new_token_array();
        // Add tokens before cursor from original
        for (int i = 0; i < p->cursor; i++)
          token_array_push(&new_toks, p->tokens.data[i]);
        // Add included tokens, but skip the EOF token at the end
        for (int i = 0; i < toks.length - 1; i++)  // -1 to skip EOF
          token_array_push(&new_toks, toks.data[i]);
        // Add tokens after cursor from original, but skip if they start with EOF
        for (int i = p->cursor; i < p->tokens.length; i++) {
          if (p->tokens.data[i].type != TOK_EOF)
            token_array_push(&new_toks, p->tokens.data[i]);
        }
        p->tokens = new_toks;
      }
      continue;
    }
    ast_t stmt = parse_statement(p);
    if (stmt == NULL) break;
    // Vardefs immediately following a module declaration become instance fields
    if (stmt->tag == vardef && last_module != NULL) {
      push_ast_array(&last_module->data.tdef.module_fields, stmt);
      continue;
    }
    last_module = (stmt->tag == tdef && stmt->data.tdef.t == TDEF_MODULE) ? stmt : NULL;
    push_ast_array(&prog, stmt);
  }
  p->prog = new_ast((node_t){program, {.program = {prog}}});
}
