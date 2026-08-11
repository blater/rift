/*****************************************************
 * ROCKER AST HEADER
 * MIT License
 * Copyright (c) 2024 Paul Passeron
 *****************************************************/

#ifndef AST_H
#define AST_H

#include "token.h"

typedef struct node_t node_t;
typedef node_t *ast_t;

typedef struct ast_array_t {
  ast_t *data;
  int length;
  int capacity;
} ast_array_t;

#define INIT_AST_ARR 8

typedef struct ast_op ast_op;
typedef struct ast_unary_op ast_unary_op;
typedef struct ast_literal ast_literal;
typedef struct ast_identifier ast_identifier;
typedef struct ast_fundef ast_fundef;
typedef struct ast_funcall ast_funcall;
typedef struct ast_ret ast_ret;
typedef struct ast_vardef ast_vardef;
typedef struct ast_match ast_match;
typedef struct ast_matchcase ast_matchcase;
typedef struct ast_sub ast_sub;
typedef struct ast_program ast_program;
typedef struct ast_compound ast_compound;
typedef struct ast_ifstmt ast_ifstmt;
typedef struct ast_tdef ast_tdef;
typedef struct ast_cons ast_cons;
typedef struct ast_record_expr ast_record_expr;
typedef struct ast_loop ast_loop;
typedef struct ast_assign ast_assign;
typedef struct ast_while_loop ast_while_loop;
typedef struct ast_iter_loop ast_iter_loop;
typedef struct ast_enum_tdef ast_enum_tdef;
typedef struct ast_type ast_type;
typedef struct ast_arr_index ast_arr_index;
typedef struct ast_embed ast_embed;
typedef struct ast_method_call ast_method_call;

struct ast_arr_index {
  ast_t array;              // array identifier expression
  ast_t index;              // index expression
  int has_field;            // 1 if field access follows (e.g., arr[i].name)
  token_array_t field_path; // intermediate fields (empty for single-level access)
  ast_t field_expr;         // final field expression (NULL when has_field == 0)
};

struct ast_embed {
  char *lang;               // "c", "asm", etc. (language name)
  char *body;               // raw source code verbatim
  int is_function;          // 1 if top-level native function, 0 if inline block
};

struct ast_op {
  token_type_t op;
  ast_t left;
  ast_t right;
};

struct ast_unary_op {
  token_type_t op;
  ast_t operand;
};

struct ast_literal {
  token_t lit;
};

struct ast_identifier {
  token_t id;
};

struct ast_fundef {
  token_t name;
  token_t type_name;   // receiver type for method declarations (e.g. "string")
  int is_method;       // 1 when declared as "sub Type.method(...)"
  int is_array_method; // 1 when declared as "sub Type[].method(...)"
  token_array_t args;
  ast_array_t types;
  ast_t body;
  ast_t ret_type;
};

struct ast_method_call {
  ast_t receiver;      // any expression — identifier, funcall, arr_index, etc.
  token_t method;      // method name token
  ast_array_t args;    // call args, NOT including receiver
};

struct ast_funcall {
  token_t name;
  ast_array_t args;
};

struct ast_ret {
  ast_t expr;
};

struct ast_vardef {
  token_t name;
  int is_rec;
  ast_t expr;
  ast_t type;
};

struct ast_match {
  ast_t expr;
  ast_array_t cases;
};

struct ast_matchcase {
  ast_t expr;
  ast_t body;
};

struct ast_sub {
  ast_t receiver;
  token_array_t path;
  ast_t expr;
};
struct ast_type {
  token_t name;
  int is_array;
  int array_capacity;  // 0 = dynamic, >0 = fixed size
};
struct ast_program {
  ast_array_t prog;
};

struct ast_compound {
  ast_array_t stmts;
};

struct ast_ifstmt {
  ast_t expression;
  ast_t body;
  ast_t elsestmt;
};

typedef enum tdef_type_t {
  TDEF_REC,
  TDEF_PRO,
  TDEF_MODULE,
} tdef_type_t;

struct ast_tdef {
  token_t name;
  tdef_type_t t;
  ast_array_t constructors;
  ast_array_t module_fields; // vardef nodes for per-instance state (TDEF_MODULE only)
};

struct ast_cons {
  token_t name;
  ast_t type;
};

struct ast_record_expr {
  token_array_t names;
  ast_array_t exprs;
};

struct ast_loop {
  token_t variable;
  ast_t start;
  ast_t end;
  ast_t statement;
};

struct ast_assign {
  ast_t target;
  ast_t expr;
};

struct ast_while_loop {
  ast_t condition;
  ast_t statement;
};

struct ast_iter_loop {
  token_t variable;
  ast_t iterable;
  ast_t statement;
};

struct ast_enum_tdef {
  token_t name;
  token_array_t items;
};

struct node_t {
  enum tag {
    op,
    unary_op,
    literal,
    identifier,
    fundef,
    funcall,
    ret,
    vardef,
    match,
    matchcase,
    sub,
    program,
    compound,
    ifstmt,
    tdef,
    cons,
    record_expr,
    loop,
    assign,
    while_loop,
    iter_loop,
    enum_tdef,
    type,
    arr_index,
    embed,
    method_call,
    collect_stmt,
  } tag;
  union data {
    ast_op op;
    ast_unary_op unary_op;
    ast_literal literal;
    ast_identifier identifier;
    ast_fundef fundef;
    ast_funcall funcall;
    ast_ret ret;
    ast_vardef vardef;
    ast_match match;
    ast_matchcase matchcase;
    ast_sub sub;
    ast_compound compound;
    ast_program program;
    ast_ifstmt ifstmt;
    ast_tdef tdef;
    ast_cons cons;
    ast_record_expr record_expr;
    ast_loop loop;
    ast_assign assign;
    ast_while_loop while_loop;
    ast_iter_loop iter_loop;
    ast_enum_tdef enum_tdef;
    ast_type type;
    ast_arr_index arr_index;
    ast_embed embed;
    ast_method_call method_call;
  } data;
};

ast_t new_ast(node_t node);
ast_array_t new_ast_array(void);

void push_ast_array(ast_array_t *arr, ast_t a);
void print_ast(ast_t root);
#endif // AST_H
