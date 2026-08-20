#define RIFT_MANAGED_DYNAMIC 0
#include "../src/lib/managed_ref.h"

#include <stdio.h>

static const unsigned char static_bytes[] = {1, 2, 3, 4};

const managed_static_entry rift_managed_static_table[] = {
    {static_bytes, sizeof(static_bytes)}};
const size_t rift_managed_static_count = 1;

int main(void) {
  managed_ref ref = MANAGED_REF_STATIC_BIT;
  managed_access_token access;
  managed_pin_token pin;
  managed_access_begin(ref, &access);
  if (managed_access_ptr(&access) != static_bytes) return 1;
  managed_access_end(&access);
  if (managed_retain(ref) != ref) return 2;
  managed_pin_typed(ref, 7, &pin);
  if (managed_pin_ptr(&pin) != static_bytes) return 3;
  managed_unpin_typed(&pin);
  managed_release_typed(ref, 7);
  puts("PASS");
  return 0;
}
