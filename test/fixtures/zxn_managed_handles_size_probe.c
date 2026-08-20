#include "managed_heap.h"
#include "pools.h"

#include <stdint.h>
#include <stdlib.h>

const managed_static_entry rift_managed_static_table[] = {{"s", 1}};
const size_t rift_managed_static_count = 1;

static void size_fault(managed_fault_reason reason, size_t requested) {
  (void)reason;
  (void)requested;
  exit(1);
}

static void size_destroy(managed_type_id type, void *payload) {
  (void)type;
  (void)payload;
}

int main(void) {
  managed_heap_options options;
  managed_access_token access;
  managed_pin_token pin;
  managed_ref ref;

  rift_pools_init(NULL);
  options.destroy = size_destroy;
  options.fault = size_fault;
  managed_heap_init(&options);
  ref = managed_alloc(sizeof(uint16_t));
  managed_access_begin(ref, &access);
  *(uint16_t *)managed_access_ptr(&access) = 7u;
  managed_access_end(&access);
  managed_retain(ref);
  managed_release_typed(ref, 1u);
  managed_pin_typed(ref, 1u, &pin);
  managed_release_typed(ref, 1u);
  managed_unpin_typed(&pin);
  managed_access_begin(MANAGED_REF_STATIC_BIT, &access);
  managed_access_end(&access);
  managed_heap_deinit();
  rift_pools_deinit();
  return 0;
}
