#include "fundefs_internal.h"

#include "fundefs.h"
#include "error_sink.h"
#include "pools.h"
#include "typedefs.h"
#include <stdlib.h>
#include <string.h>

static int checked_multiply(size_t left, size_t right, size_t *result) {
  if (left != 0 && right > SIZE_MAX / left) return 0;
  *result = left * right;
  return 1;
}

static int checked_double(size_t value, size_t *result) {
  if (value > SIZE_MAX / 2) return 0;
  *result = value * 2;
  return 1;
}

static void array_size_error(const char *operation) {
  rift_error_text("Array size overflow during ");
  rift_error_text(operation);
  rift_error_newline();
  exit_rift(1);
  exit(1); /* documents non-return to compilers that do not know exit_rift */
}

static size_t dynamic_array_initial_capacity = __INTERNAL_DYNAMIC_ARRAY_CAP;

void __internal_set_dynamic_array_initial_capacity(size_t capacity) {
  dynamic_array_initial_capacity = capacity == 0 ? 1 : capacity;
}

__internal_dynamic_array_t __internal_make_array_with_release(
    size_t size, size_t max_capacity, __internal_array_release_fn release_elem) {
  /* ADR-0003 Phase D extension: array header struct lives inside a
   * universal block header in the longlived pool. The handle returned to
   * Rift code is a pointer to the payload (the struct itself); the
   * rift_block_header sits at handle - sizeof(rift_block_header). */
  if (size == 0) {
    rift_error_text("Could not create dynamic array: BAD ELEMENT SIZE\n");
    exit_rift(1);
  }
  size_t capacity = max_capacity > 0 ? max_capacity : dynamic_array_initial_capacity;
  size_t byte_size;
  if (!checked_multiply(size, capacity, &byte_size)) array_size_error("creation");
  __internal_dynamic_array_t ptr =
      rift_longlived_alloc(sizeof(struct __internal_dynamic_array));
  (*ptr).elem_size = size;
  (*ptr).max_capacity = max_capacity;
  (*ptr).release_elem = release_elem;
  (*ptr).capacity = capacity;

  /* Element buffer is a separate longlived block. */
  (*ptr).data = rift_longlived_alloc(byte_size);
  (*ptr).length = 0;
  return ptr;
}

__internal_dynamic_array_t __internal_make_array(size_t size, size_t max_capacity) {
  return __internal_make_array_with_release(size, max_capacity, NULL);
}

void __internal_free_array(__internal_dynamic_array_t arr, int is_string_array) {
  if (arr == NULL) return;
  rift_block_header *header = ((rift_block_header *)arr) - 1;
  if (header->refcount == RIFT_RC_STATIC) return;
  if (header->refcount == RIFT_RC_FREE || header->refcount == RIFT_RC_MAGAZINE) {
    rift_error_text("rift: array release on already-freed block\n");
    exit(1);
  }
  if (--header->refcount != 0) return;
  if (is_string_array && arr->data != NULL) {
    for (size_t i = 0; i < arr->length; i++) {
      string *s = (string *)((char *)arr->data + i * arr->elem_size);
      __string_release(*s);
    }
  } else if (arr->release_elem != NULL && arr->data != NULL) {
    for (size_t i = 0; i < arr->length; i++) {
      arr->release_elem((char *)arr->data + i * arr->elem_size);
    }
  }
  if (arr->data != NULL) {
    rift_longlived_free(arr->data);
    arr->data = NULL;
  }
  rift_longlived_free(arr);
}

void __internal_retain_array(__internal_dynamic_array_t arr) {
  (void)__handle_retain(arr);
}

int __internal_push_array(__internal_dynamic_array_t arr, void *elem) {
  if (arr->data == NULL) {
    rift_error_text("Uninitialized array !\n");
    exit_rift(1);
  }
  if (elem == NULL) {
    rift_error_text("Could not push elem to dynamic array: BAD ELEM\n");
    exit_rift(1);
  }

  // Check if we can add more elements
  if (arr->max_capacity > 0 && arr->length >= arr->max_capacity) {
    rift_error_text("Error: Cannot append to fixed-size array\n");
    exit_rift(1);
  }

  if (arr->length >= arr->capacity) {
    // For dynamic arrays only, grow the capacity
    if (arr->max_capacity == 0) {
      /* No realloc in the longlived pool — allocate fresh, copy, free old.
       * ADR-0003 §8.4 already mandates fixed-capacity arrays for long-lived
       * use cases; this growth path remains for transitional bump-only
       * arrays during Phase D extension. */
      size_t new_capacity = 0, new_byte_size = 0, copied_byte_size = 0;
      if (!checked_double(arr->capacity, &new_capacity) ||
          !checked_multiply(new_capacity, arr->elem_size, &new_byte_size) ||
          !checked_multiply(arr->length, arr->elem_size, &copied_byte_size)) {
        array_size_error("growth");
      }
      void *new_data = rift_longlived_alloc(new_byte_size);
      memcpy(new_data, arr->data, copied_byte_size);
      rift_longlived_free(arr->data);
      arr->data = new_data;
      arr->capacity = new_capacity;
    } else {
      rift_error_text("Error: Array capacity exceeded\n");
      exit_rift(1);
    }
  }
  void *dst = (char *)arr->data + arr->length * arr->elem_size;
  if (dst == NULL) {
    rift_error_text("Could not push elem to dynamic array: BAD ARRAY\n");
    exit_rift(1);
  }
  // memccpy(dst, elem, 1, arr->elem_size);
  for (int i = 0; i < arr->elem_size; i++) {
    ((char *)dst)[i] = ((char *)elem)[i];
  }
  arr->length++;
  return 0;
}

int __internal_pop_into(__internal_dynamic_array_t arr, void *out) {
  if (arr->length == 0) {
    rift_error_text("Could not pop elem out of dynamic array: EMPTY ARRAY\n");
    exit_rift(1);
    return 0;
  }
  if (arr->elem_size == 0) {
    rift_error_text("Could not pop elem out of dynamic array: BAD ELEMENT SIZE\n");
    exit_rift(1);
    return 0;
  }
  void *src = (char *)arr->data + (arr->length - 1) * arr->elem_size;
  memcpy(out, src, arr->elem_size);
  memset(src, 0, arr->elem_size);
  arr->length--;
  return 1;
}

void *__internal_get_elem(__internal_dynamic_array_t arr, size_t index) {
  // For fixed-size arrays, check against capacity; for dynamic, check against length
  size_t limit = (arr->max_capacity > 0) ? arr->max_capacity : arr->length;
  if (index >= limit) {
    rift_error_text("Could not get elem from dynamic array: INDEX OUT OF BOUNDS (");
    rift_error_size(index);
    rift_error_text(")\n");

    int *tmp = NULL;
    *tmp = 3;
    return NULL;
  }
  if (arr->elem_size == 0) {
    rift_error_text("Could not get elem from dynamic array: BAD ELEMENT SIZE\n");
    return NULL;
  }
  void *src = (char *)arr->data + index * arr->elem_size;
  if (src == NULL)
    return NULL;
  return src;
}

void __internal_insert(__internal_dynamic_array_t arr, size_t index,
                       void *elem) {
  if (arr->data == NULL) {
    rift_error_text("Uninitialized array!\n");
    exit_rift(1);
  }

  if (elem == NULL) {
    rift_error_text("Could not insert elem into dynamic array: BAD ELEM\n");
    exit_rift(1);
  }

  if (arr->elem_size == 0) {
    rift_error_text("Could not insert elem into dynamic array: BAD ELEMENT SIZE\n");
    exit_rift(1);
  }

  // For insert, index can be from 0 to length (inclusive)
  if (index > arr->length) {
    rift_error_text("Could not insert elem: INDEX OUT OF BOUNDS\n");
    exit_rift(1);
  }

  // Check if we have room
  if (arr->length >= arr->capacity) {
    // For dynamic arrays only, grow capacity
    if (arr->max_capacity == 0) {
      /* alloc-copy-free pattern; see __internal_push_array. */
      size_t new_capacity = 0, new_byte_size = 0, copied_byte_size = 0;
      if (!checked_double(arr->capacity, &new_capacity) ||
          !checked_multiply(new_capacity, arr->elem_size, &new_byte_size) ||
          !checked_multiply(arr->length, arr->elem_size, &copied_byte_size)) {
        array_size_error("growth");
      }
      void *new_data = rift_longlived_alloc(new_byte_size);
      memcpy(new_data, arr->data, copied_byte_size);
      rift_longlived_free(arr->data);
      arr->data = new_data;
      arr->capacity = new_capacity;
    } else {
      rift_error_text("Error: Array capacity exceeded, cannot insert\n");
      exit_rift(1);
    }
  }

  // Shift elements right: move from [index...length-1] to [index+1...length]
  // Must go backward to avoid overwriting
  for (int i = arr->length; i > index; i--) {
    void *src = (char *)arr->data + (i - 1) * arr->elem_size;
    void *dst = (char *)arr->data + i * arr->elem_size;
    memcpy(dst, src, arr->elem_size);
  }

  // Copy element into the insertion position
  void *dst = (char *)arr->data + index * arr->elem_size;
  memcpy(dst, elem, arr->elem_size);
  arr->length++;
}

void __internal_set_elem(__internal_dynamic_array_t arr, size_t index,
                         void *elem) {
  size_t limit = arr->length;
  if (arr->max_capacity > 0)
    limit = arr->length < arr->capacity ? arr->length + 1 : arr->capacity;
  if (index >= limit) {
    rift_error_text("Could not set elem in dynamic array: INDEX OUT OF BOUNDS\n");
    exit_rift(1);
  }
  if (arr->elem_size == 0) {
    rift_error_text("Could not set elem in dynamic array: BAD ELEMENT SIZE\n");
    exit_rift(1);
  }
  void *dst = (char *)arr->data + index * arr->elem_size;
  if (dst == NULL) {
    rift_error_text("Could not set elem in dynamic array: BAD ARRAY\n");
    exit_rift(1);
  }
  memcpy(dst, elem, arr->elem_size);
  // Fixed arrays may be initialized sequentially up to their capacity.
  if (index == arr->length) {
    if (index == SIZE_MAX) array_size_error("set");
    arr->length = index + 1;
  }
}

size_t __length_array(__internal_dynamic_array_t arr) {
  if (arr == NULL)
    return 0;
  return arr->length;
}

__internal_dynamic_array_t int_make_array(void) {
  return __internal_make_array(sizeof(int), 0);
}

void int_push_array(__internal_dynamic_array_t arr, int elem) {
  __internal_push_array(arr, &elem);
}

int int_pop_array(__internal_dynamic_array_t arr) {
  int result = 0;
  __internal_pop_into(arr, &result);
  return result;
}

int int_get_elem(__internal_dynamic_array_t arr, size_t index) {
  int *res = __internal_get_elem(arr, index);
  return *res;
}

void int_set_elem(__internal_dynamic_array_t arr, size_t index, int elem) {
  __internal_set_elem(arr, index, &elem);
}

void int_insert(__internal_dynamic_array_t arr, size_t index, int elem) {
  __internal_insert(arr, index, &elem);
}

__internal_dynamic_array_t boolean_make_array(void) {
  return __internal_make_array(sizeof(boolean), 0);
}

void boolean_push_array(__internal_dynamic_array_t arr, boolean elem) {
  __internal_push_array(arr, &elem);
}

boolean boolean_pop_array(__internal_dynamic_array_t arr) {
  boolean result = 0;
  __internal_pop_into(arr, &result);
  return result;
}

boolean boolean_get_elem(__internal_dynamic_array_t arr, size_t index) {
  boolean *res = __internal_get_elem(arr, index);
  return *res;
}

void boolean_set_elem(__internal_dynamic_array_t arr, size_t index,
                      boolean elem) {
  __internal_set_elem(arr, index, &elem);
}

void boolean_insert(__internal_dynamic_array_t arr, size_t index,
                    boolean elem) {
  __internal_insert(arr, index, &elem);
}

__internal_dynamic_array_t string_make_array(void) {
  return __internal_make_array(sizeof(string), 0);
}

void string_push_array(__internal_dynamic_array_t arr, string elem) {
  string copy;
  new_string(&copy, elem);
  __internal_push_array(arr, &copy);
}

void string_pop_array(string *out, __internal_dynamic_array_t arr) {
  if (!__internal_pop_into(arr, out)) {
    __rift_make_string(out, "", 0);
  }
}

void string_get_elem(string *out, __internal_dynamic_array_t arr, size_t index) {
  string *tmp = __internal_get_elem(arr, index);
  if (tmp == NULL) {
    __rift_make_string(out, "", 0);
    return;
  }
  new_string(out, *tmp);
}

void string_set_elem(__internal_dynamic_array_t arr, size_t index,
                     string elem) {
  if (index < arr->length) {
    string *old = __internal_get_elem(arr, index);
    if (old == NULL) return;
    __string_retain(elem);
    __string_release(*old);
  } else {
    __string_retain(elem);
  }
  __internal_set_elem(arr, index, &elem);
}

void string_insert(__internal_dynamic_array_t arr, size_t index, string elem) {
  __string_retain(elem);
  __internal_insert(arr, index, &elem);
}

__internal_dynamic_array_t char_make_array(void) {
  return __internal_make_array(sizeof(char), 0);
}

void char_push_array(__internal_dynamic_array_t arr, char elem) {
  __internal_push_array(arr, &elem);
}

char char_pop_array(__internal_dynamic_array_t arr) {
  char result = 0;
  __internal_pop_into(arr, &result);
  return result;
}

char char_get_elem(__internal_dynamic_array_t arr, size_t index) {
  char *res = __internal_get_elem(arr, index);
  return *res;
}

void char_set_elem(__internal_dynamic_array_t arr, size_t index, char elem) {
  __internal_set_elem(arr, index, &elem);
}

void char_insert(__internal_dynamic_array_t arr, size_t index, char elem) {
  __internal_insert(arr, index, &elem);
}

__internal_dynamic_array_t byte_make_array(void) {
  return __internal_make_array(sizeof(byte), 0);
}

void byte_push_array(__internal_dynamic_array_t arr, byte elem) {
  __internal_push_array(arr, &elem);
}

byte byte_pop_array(__internal_dynamic_array_t arr) {
  byte result = 0;
  __internal_pop_into(arr, &result);
  return result;
}

byte byte_get_elem(__internal_dynamic_array_t arr, size_t index) {
  byte *res = __internal_get_elem(arr, index);
  return *res;
}

void byte_set_elem(__internal_dynamic_array_t arr, size_t index, byte elem) {
  __internal_set_elem(arr, index, &elem);
}

void byte_insert(__internal_dynamic_array_t arr, size_t index, byte elem) {
  __internal_insert(arr, index, &elem);
}

__internal_dynamic_array_t word_make_array(void) {
  return __internal_make_array(sizeof(word), 0);
}
void word_push_array(__internal_dynamic_array_t arr, word elem) {
  __internal_push_array(arr, &elem);
}
word word_pop_array(__internal_dynamic_array_t arr) {
  word result = 0;
  __internal_pop_into(arr, &result);
  return result;
}
word word_get_elem(__internal_dynamic_array_t arr, size_t index) {
  word *res = __internal_get_elem(arr, index);
  return *res;
}
void word_set_elem(__internal_dynamic_array_t arr, size_t index, word elem) {
  __internal_set_elem(arr, index, &elem);
}
void word_insert(__internal_dynamic_array_t arr, size_t index, word elem) {
  __internal_insert(arr, index, &elem);
}

__internal_dynamic_array_t dword_make_array(void) {
  return __internal_make_array(sizeof(dword), 0);
}
void dword_push_array(__internal_dynamic_array_t arr, dword elem) {
  __internal_push_array(arr, &elem);
}
dword dword_pop_array(__internal_dynamic_array_t arr) {
  dword result = 0;
  __internal_pop_into(arr, &result);
  return result;
}
dword dword_get_elem(__internal_dynamic_array_t arr, size_t index) {
  dword *res = __internal_get_elem(arr, index);
  return *res;
}
void dword_set_elem(__internal_dynamic_array_t arr, size_t index, dword elem) {
  __internal_set_elem(arr, index, &elem);
}
void dword_insert(__internal_dynamic_array_t arr, size_t index, dword elem) {
  __internal_insert(arr, index, &elem);
}

__internal_dynamic_array_t float_make_array(void) {
  return __internal_make_array(sizeof(float), 0);
}
void float_push_array(__internal_dynamic_array_t arr, float elem) {
  __internal_push_array(arr, &elem);
}
float float_pop_array(__internal_dynamic_array_t arr) {
  float result = 0;
  __internal_pop_into(arr, &result);
  return result;
}
float float_get_elem(__internal_dynamic_array_t arr, size_t index) {
  float *res = __internal_get_elem(arr, index);
  return *res;
}
void float_set_elem(__internal_dynamic_array_t arr, size_t index, float elem) {
  __internal_set_elem(arr, index, &elem);
}
void float_insert(__internal_dynamic_array_t arr, size_t index, float elem) {
  __internal_insert(arr, index, &elem);
}
