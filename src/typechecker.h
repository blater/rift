/*****************************************************
 * RIFT TYPECHECKER HEADER
 * MIT License
 * Copyright (c) 2024 Paul Passeron
 *****************************************************/

#ifndef TYPECHECKER_H
#define TYPECHECKER_H

#include "ast.h"
#include "name_table.h"
#include "stringview.h"

typedef struct typechecker_t {
  name_table_t nt;
  ast_t current_function;
} typechecker_t;

typedef struct rift_type_t {
  enum {
    builtin_int,
    builtin_char,
    builtin_bool,
    builtin_string,
    user_defined,
    error_type,
  } tag;
  union {
    int builtin;
    struct {
      string_view name;
    } user_defined_type;
  } data;
} rift_type_t;

int tc_program(ast_t program);

/* ADR-0003 §9.4: structural-acyclicity check.
 * Rejects user-defined types whose field graph (walking transitively
 * through scalars, descriptors, and array/handle indirection) contains
 * the type itself as a reachable node.
 *
 * Returns 1 if the program's user-type graph is acyclic, 0 otherwise.
 * Reports diagnostics via error() for every cycle found. */
int check_acyclic_types(ast_t program);

#endif  // TYPECHECKER_H
