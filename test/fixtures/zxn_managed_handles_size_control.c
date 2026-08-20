#include "pools.h"

#include <stdint.h>

static const char size_static[] = "s";

int main(void) {
  uint16_t *payload;
  const void *access;

  rift_pools_init(NULL);
  payload = (uint16_t *)rift_longlived_alloc(sizeof(*payload));
  access = payload;
  *(uint16_t *)access = 7u;
  access = NULL;
  (void)access;
  access = size_static;
  access = NULL;
  (void)access;
  rift_longlived_free(payload);
  rift_pools_deinit();
  return 0;
}
