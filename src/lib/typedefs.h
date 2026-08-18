#ifndef RIFT_TYPEDEFS_INTERNAL
#define RIFT_TYPEDEFS_INTERNAL

#include <stddef.h>
#include <stdint.h>

#include "pools.h"  /* for rift_block_header */

/* ADR-0003 §7.1: a string is a descriptor referencing backing bytes. The
 * `backing` field is the universal block header for refcount-managed bytes
 * in the longlived pool; NULL means the bytes live in static or bump
 * storage and are not refcounted. `capacity == 0` is a read-only view;
 * `capacity > 0` is in-place writable backing. The `__string_block` typedef
 * is an alias for `rift_block_header` per §7.1. */
typedef rift_block_header __string_block;

typedef struct string {
  char *data;
  size_t length;
  size_t capacity;       /* 0 = read-only view; >0 = writable backing */
  __string_block *backing; /* header for refcounted backing; NULL = bump/static */
} string;

typedef struct __internal_dynamic_array *__internal_dynamic_array_t;
typedef void (*__internal_array_release_fn)(void *slot);

struct __internal_dynamic_array {
  void *data;
  size_t length;
  size_t capacity;
  size_t elem_size;
  size_t max_capacity;  // 0 = unlimited (dynamic), >0 = fixed size limit
  __internal_array_release_fn release_elem; // NULL for scalar elements
};

typedef char boolean;
typedef unsigned char byte;
typedef unsigned short word;
typedef uint32_t dword;
#define true 1
#define false 0

#endif // RIFT_TYPEDEFS_INTERNAL
