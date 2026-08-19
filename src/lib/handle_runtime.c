#include "fundefs.h"

#include "error_sink.h"
#include "pools.h"
#include <stdlib.h>

void *__handle_retain(void *payload) {
  if (!payload) return payload;
  rift_block_header *h = ((rift_block_header *)payload) - 1;
  if (h->refcount == RIFT_RC_STATIC) return payload;
  if (h->refcount == RIFT_RC_FREE || h->refcount == RIFT_RC_MAGAZINE) {
    rift_error_text("rift: __handle_retain on already-freed block\n");
    exit(1);
  }
  if (h->refcount >= RIFT_RC_FREE - 1) {
    rift_error_text("rift: __handle_retain refcount overflow\n");
    exit(1);
  }
  h->refcount++;
  return payload;
}

void __handle_release(void *payload) {
  if (!payload) return;
  rift_block_header *h = ((rift_block_header *)payload) - 1;
  if (h->refcount == RIFT_RC_STATIC) return;
  if (h->refcount == RIFT_RC_FREE || h->refcount == RIFT_RC_MAGAZINE) {
    rift_error_text("rift: __handle_release on already-freed block\n");
    exit(1);
  }
  if (--h->refcount == 0) rift_longlived_free(payload);
}
