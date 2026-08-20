#include "pools.h"
#include "zxn_test.h"

#include <stdint.h>

typedef struct raw_parent_payload {
  void *child;
  uint16_t marker;
} raw_parent_payload;

static const char raw_static[] = "static";

int main(void) {
  void *blocks[40];
  void *pinned;
  void *reused;
  raw_parent_payload *parent;
  unsigned int destructor_count = 0;
  unsigned int index;
  int passed = 1;
  zxn_test_begin();
  rift_pools_init(NULL);
  if (raw_static[0] != 's') passed = 0;
  for (index = 0; index < 40; index++) {
    blocks[index] = rift_longlived_alloc(sizeof(uint16_t));
    if (!blocks[index]) passed = 0;
    *(uint16_t *)blocks[index] = (uint16_t)(index * 7u);
  }
  for (index = 0; index < 40; index++) {
    if (*(uint16_t *)blocks[index] != (uint16_t)(index * 7u)) passed = 0;
  }

  pinned = blocks[3];
  *(uint16_t *)pinned = 0x55aau;
  rift_longlived_free(pinned);
  destructor_count++;
  reused = rift_longlived_alloc(sizeof(uint16_t));
  if (!reused) passed = 0;
  blocks[3] = reused;

  parent = (raw_parent_payload *)rift_longlived_alloc(sizeof(*parent));
  if (!parent) passed = 0;
  parent->child = rift_longlived_alloc(2);
  if (!parent->child) passed = 0;
  parent->marker = 0xbeefu;
  rift_longlived_free(parent->child);
  destructor_count++;
  rift_longlived_free(parent);
  destructor_count++;

  for (index = 0; index < 40; index++) {
    rift_longlived_free(blocks[index]);
    destructor_count++;
  }
  if (destructor_count != 43u || rift_longlived_used() != 0) passed = 0;
  if (passed)
    zxn_test_pass();
  else
    zxn_test_fail();
  zxn_test_finish();
  rift_pools_deinit();
  return 0;
}
