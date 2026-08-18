#ifndef GENERATOR_TYPE_INFO_H
#define GENERATOR_TYPE_INFO_H

#include "internal.h"

token_t token_for_expr(ast_t expr);
void generate_type(FILE *f, ast_t type);
string_view get_array_var_type(string_view name, name_table_t table,
                               token_t token);
string_view get_var_type(string_view name, name_table_t table);
int get_identifier_array_type(string_view name, name_table_t table,
                              ast_type *result);
int is_scalar_string_var(string_view name, name_table_t table);
int rhs_is_borrower(ast_t expr);
int expr_is_array(ast_t expr, name_table_t table, int *is_string);
void emit_borrowed_container_retain(generator_t *g, ast_t expr,
                                    ast_type value_type);
int is_heap_allocated_type(string_view type_name, name_table_t table);
int is_scalar_aggregate_var(string_view name, name_table_t table);
string_view make_array_type_sv(string_view base);
string_view get_field_type(string_view base_type, string_view field_name,
                           name_table_t table);
int is_sub_target_scalar_string(ast_t expr, name_table_t table);
int is_sub_target_scalar_aggregate(ast_t expr, name_table_t table);
string_view sub_target_array_element_type(ast_t expr, name_table_t table);
string_view infer_expr_type(ast_t expr, name_table_t table);
int expr_returns_string(ast_t expr, name_table_t table);
void generate_string_to_cstr(generator_t *g, ast_t expr);
string_view try_get_field_array_type(string_view base_type,
                                     string_view field_name,
                                     name_table_t table);
string_view get_array_element_type(ast_t array, name_table_t table,
                                   token_t call_token);

#endif
