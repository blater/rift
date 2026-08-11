#ifndef ROCKER_FUNDEFS_INTERNAL
#define ROCKER_FUNDEFS_INTERNAL

#include "typedefs.h"
#include "fundefs.h"

#define __INTERNAL_DYNAMIC_ARRAY_CAP 64

void __internal_set_dynamic_array_initial_capacity(size_t capacity);

void init_rocker(int argc, char **argv);
void end_rocker(void);
void exit_rocker(int status);

/* Rock-exposed program termination. Mirrors PASTA/80's Halt(ExitCode).
 * Runs end_rocker() then exits with the given code. */
void halt(byte code);

__internal_dynamic_array_t __internal_make_array(size_t size, size_t max_capacity);
__internal_dynamic_array_t __internal_make_array_with_release(
    size_t size, size_t max_capacity, __internal_array_release_fn release_elem);
int __internal_push_array(__internal_dynamic_array_t arr, void *elem);
int __internal_pop_into(__internal_dynamic_array_t arr, void *out);
void __internal_insert(__internal_dynamic_array_t arr, size_t index,
                       void *elem);
void *__internal_get_elem(__internal_dynamic_array_t arr, size_t index);
void __internal_set_elem(__internal_dynamic_array_t arr, size_t index,
                         void *elem);

void __internal_free_array(__internal_dynamic_array_t arr, int is_string_array);
void __internal_retain_array(__internal_dynamic_array_t arr);

size_t __length_array(__internal_dynamic_array_t arr);
size_t __length_string(string s);
#ifndef __SDCC
#define length(x) _Generic((x), string: __length_string, default: __length_array)(x)
#else
#define length(x) __length_array(x)
#endif

__internal_dynamic_array_t get_args(void);
void fill_cmd_args(int argc, char **argv);

// Builtin array methods (generated)

__internal_dynamic_array_t int_make_array(void);

void int_push_array(__internal_dynamic_array_t arr, int elem);

int int_pop_array(__internal_dynamic_array_t arr);
int int_get_elem(__internal_dynamic_array_t arr, size_t index);

void int_set_elem(__internal_dynamic_array_t arr, size_t index, int elem);

void int_insert(__internal_dynamic_array_t arr, size_t index, int elem);

__internal_dynamic_array_t boolean_make_array(void);

void boolean_push_array(__internal_dynamic_array_t arr, boolean elem);

boolean boolean_pop_array(__internal_dynamic_array_t arr);

boolean boolean_get_elem(__internal_dynamic_array_t arr, size_t index);
void boolean_set_elem(__internal_dynamic_array_t arr, size_t index,
                      boolean elem);

void boolean_insert(__internal_dynamic_array_t arr, size_t index, boolean elem);

__internal_dynamic_array_t string_make_array(void);

void string_push_array(__internal_dynamic_array_t arr, string elem);

void string_pop_array(string *out, __internal_dynamic_array_t arr);

void string_get_elem(string *out, __internal_dynamic_array_t arr, size_t index);

void string_set_elem(__internal_dynamic_array_t arr, size_t index, string elem);

void string_insert(__internal_dynamic_array_t arr, size_t index, string elem);

__internal_dynamic_array_t char_make_array(void);

void char_push_array(__internal_dynamic_array_t arr, char elem);

char char_pop_array(__internal_dynamic_array_t arr);

char char_get_elem(__internal_dynamic_array_t arr, size_t index);

void char_set_elem(__internal_dynamic_array_t arr, size_t index, char elem);

void char_insert(__internal_dynamic_array_t arr, size_t index, char elem);

__internal_dynamic_array_t byte_make_array(void);

void byte_push_array(__internal_dynamic_array_t arr, byte elem);

byte byte_pop_array(__internal_dynamic_array_t arr);

byte byte_get_elem(__internal_dynamic_array_t arr, size_t index);

void byte_set_elem(__internal_dynamic_array_t arr, size_t index, byte elem);

void byte_insert(__internal_dynamic_array_t arr, size_t index, byte elem);

__internal_dynamic_array_t word_make_array(void);

void word_push_array(__internal_dynamic_array_t arr, word elem);

word word_pop_array(__internal_dynamic_array_t arr);

word word_get_elem(__internal_dynamic_array_t arr, size_t index);

void word_set_elem(__internal_dynamic_array_t arr, size_t index, word elem);

void word_insert(__internal_dynamic_array_t arr, size_t index, word elem);

__internal_dynamic_array_t float_make_array(void);

void float_push_array(__internal_dynamic_array_t arr, float elem);

float float_pop_array(__internal_dynamic_array_t arr);

float float_get_elem(__internal_dynamic_array_t arr, size_t index);

void float_set_elem(__internal_dynamic_array_t arr, size_t index, float elem);

void float_insert(__internal_dynamic_array_t arr, size_t index, float elem);

__internal_dynamic_array_t dword_make_array(void);

void dword_push_array(__internal_dynamic_array_t arr, dword elem);

dword dword_pop_array(__internal_dynamic_array_t arr);

dword dword_get_elem(__internal_dynamic_array_t arr, size_t index);

void dword_set_elem(__internal_dynamic_array_t arr, size_t index, dword elem);

void dword_insert(__internal_dynamic_array_t arr, size_t index, dword elem);

#endif // ROCKER_FUNDEFS_INTERNAL
