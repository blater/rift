#include "../src/lib/fundefs_internal.h"
#include "../src/lib/pools.h"

#include <stdio.h>
#include <string.h>

#define POOL_CAPACITY (64u * 1024u)

static string make_owned_string(const char *text) {
  size_t length = strlen(text);
  char *data = rift_longlived_alloc(length + 1);
  string result;

  memcpy(data, text, length + 1);
  result.data = data;
  result.length = length;
  result.capacity = length + 1;
  result.backing = ((rift_block_header *)data) - 1;
  return result;
}

static int check_poisoned_string_append(void) {
  __internal_dynamic_array_t strings;
  string first = make_owned_string("first");
  string second = make_owned_string("second");
  string replacement = make_owned_string("replacement");

  strings = __internal_make_array(sizeof(string), 2);
  memset(strings->data, 0xa5, strings->capacity * strings->elem_size);

  string_set_elem(strings, 0, first);
  string_set_elem(strings, 1, second);
  if (strings->length != 2 || first.backing->refcount != 2 ||
      second.backing->refcount != 2) {
    fprintf(stderr, "FAIL: sequential string append ownership is wrong\n");
    return 0;
  }

  string_set_elem(strings, 0, replacement);
  if (first.backing->refcount != 1 || replacement.backing->refcount != 2) {
    fprintf(stderr, "FAIL: string replacement did not release the old owner\n");
    return 0;
  }

  __internal_free_array(strings, 1);
  __string_release(first);
  __string_release(second);
  __string_release(replacement);
  return 1;
}

int main(int argc, char **argv) {
  __internal_dynamic_array_t sprites;
  byte slot;
  rift_arena_options arena_options = {
      .memory_max = POOL_CAPACITY,
      .memory_max_present = 1,
  };

  rift_pools_init(&arena_options);
  sprites = __internal_make_array(sizeof(byte), 3);

  if (argc == 2 && strcmp(argv[1], "gap") == 0) {
    slot = 2;
    __internal_set_elem(sprites, 2, &slot);
    return 2;
  }

  for (slot = 0; slot < 3; slot++)
    __internal_set_elem(sprites, slot, &slot);
  if (__length_array(sprites) != 3 ||
      *(byte *)__internal_get_elem(sprites, 0) != 0 ||
      *(byte *)__internal_get_elem(sprites, 1) != 1 ||
      *(byte *)__internal_get_elem(sprites, 2) != 2) {
    fprintf(stderr, "FAIL: sequential fixed-array initialization changed\n");
    return 1;
  }

  __internal_free_array(sprites, 0);
  if (!check_poisoned_string_append()) return 1;
  rift_pools_deinit();
  puts("PASS: fixed arrays initialize sequentially without releasing poison");
  return 0;
}
