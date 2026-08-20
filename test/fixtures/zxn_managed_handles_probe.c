#include "managed_heap.h"
#include "pools.h"
#include "zxn_test.h"

#include <stdint.h>
#include <stdlib.h>

#define TYPE_TRIVIAL 1u
#define TYPE_PARENT 2u

typedef struct parent_payload {
  managed_ref child;
  uint16_t marker;
} parent_payload;

const managed_static_entry rift_managed_static_table[] = {{"static", 6}};
const size_t rift_managed_static_count = 1;

static unsigned int destructor_count;

static void fault_dispatch(managed_fault_reason reason, size_t requested) {
  (void)reason;
  (void)requested;
  zxn_test_fail();
  zxn_test_finish();
  exit(1);
}

static void destroy_dispatch(managed_type_id type, void *payload) {
  destructor_count++;
  if (type == TYPE_PARENT) {
    parent_payload *parent = (parent_payload *)payload;
    if (parent->child) managed_release_typed(parent->child, TYPE_TRIVIAL);
  }
}

int main(void) {
  managed_heap_options options;
  managed_ref refs[40];
  managed_ref child;
  managed_ref parent;
  managed_ref reused;
  managed_access_token access;
  managed_pin_token pin;
  unsigned int index;
  int passed = 1;

  zxn_test_begin();
  rift_pools_init(NULL);
  options.destroy = destroy_dispatch;
  options.fault = fault_dispatch;
  managed_heap_init(&options);

  if (sizeof(managed_ref) != 2 || sizeof(managed_static_entry) != 4 ||
      sizeof(managed_access_token) != 2 || sizeof(managed_pin_token) != 6)
    passed = 0;

  managed_access_begin(MANAGED_REF_STATIC_BIT, &access);
  if (managed_access_ptr(&access) != rift_managed_static_table[0].payload)
    passed = 0;
  managed_access_end(&access);

  for (index = 0; index < 40; index++) {
    refs[index] = managed_alloc(sizeof(uint16_t));
    if (refs[index] != (managed_ref)(index + 1u)) passed = 0;
    managed_access_begin(refs[index], &access);
    *(uint16_t *)managed_access_ptr(&access) = (uint16_t)(index * 7u);
    managed_access_end(&access);
  }
  for (index = 0; index < 40; index++) {
    managed_access_begin(refs[index], &access);
    if (*(uint16_t *)managed_access_ptr(&access) != (uint16_t)(index * 7u))
      passed = 0;
    managed_access_end(&access);
  }

  managed_pin_typed(refs[3], 77u, &pin);
  managed_release_typed(refs[3], TYPE_TRIVIAL);
  *(uint16_t *)managed_pin_ptr(&pin) = 0x55aau;
  managed_unpin_typed(&pin);
  if (pin.ref || pin.payload || managed_heap_pin_count() != 0) passed = 0;
  reused = managed_alloc(sizeof(uint16_t));
  if (reused != refs[3]) passed = 0;
  refs[3] = reused;

  child = managed_alloc(2);
  parent = managed_alloc(sizeof(parent_payload));
  managed_access_begin(parent, &access);
  ((parent_payload *)managed_access_ptr(&access))->child = child;
  ((parent_payload *)managed_access_ptr(&access))->marker = 0xbeefu;
  managed_access_end(&access);
  managed_release_typed(parent, TYPE_PARENT);

  for (index = 0; index < 40; index++)
    managed_release_typed(refs[index], TYPE_TRIVIAL);
  if (managed_heap_live_count() != 0 || managed_heap_pin_count() != 0 ||
      destructor_count != 43u || rift_longlived_used() != 0)
    passed = 0;

  managed_heap_deinit();
  if (passed)
    zxn_test_pass();
  else
    zxn_test_fail();
  zxn_test_finish();
  rift_pools_deinit();
  return 0;
}
