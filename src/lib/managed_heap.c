#define RIFT_MANAGED_IMPLEMENTATION 1
#include "managed_heap.h"

#include "error_sink.h"
#include "pools.h"
#include "segregated_heap.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SDCC
#define MANAGED_PRIVATE_CALLEE __z88dk_callee
#else
#define MANAGED_PRIVATE_CALLEE
#endif

#define MANAGED_RADIX_BITS 4u
#define MANAGED_RADIX_COUNT 16u
#define MANAGED_CACHE_COUNT 8u
#define MANAGED_META_COUNT_MASK 0x1fu
#define MANAGED_META_LEVEL_SHIFT 5u
#define MANAGED_STATE_LIVE 0x51u
#define MANAGED_STATE_DESTROYING 0xd1u
#define MANAGED_LIFECYCLE_READY 0x5au

#ifdef __SDCC
#define MANAGED_MAX_LEVEL 3u
#define MANAGED_TAG_BIT ((rift_pool_offset_t)0x8000u)
#else
#define MANAGED_MAX_LEVEL 7u
#define MANAGED_TAG_BIT \
  ((rift_pool_offset_t)1u << (sizeof(rift_pool_offset_t) * 8u - 1u))
#endif

#define MANAGED_TAG_RESERVED MANAGED_TAG_BIT
#define MANAGED_TAG_LEVEL(level) \
  (MANAGED_TAG_BIT | (rift_pool_offset_t)((level) + 1u))

typedef struct managed_page managed_page;

#ifdef __SDCC
typedef struct managed_page_header {
  managed_page *parent;
  unsigned char parent_slot;
  unsigned char meta;
} managed_page_header;

struct managed_page {
  managed_page_header header;
  void *slots[MANAGED_RADIX_COUNT];
  uint16_t bitmap;
  unsigned char reserved[2];
};

typedef struct managed_object_prefix {
  unsigned char pin_count;
  unsigned char state;
} managed_object_prefix;

typedef struct managed_cache_entry {
  managed_ref id;
  void *payload;
} managed_cache_entry;

#else
typedef struct managed_page_header {
  managed_page *parent;
  unsigned char parent_slot;
  unsigned char meta;
  unsigned char reserved[6];
} managed_page_header;

struct managed_page {
  managed_page_header header;
  void *slots[MANAGED_RADIX_COUNT];
  uint16_t bitmap;
  unsigned char reserved[14];
};

typedef struct managed_object_prefix {
  unsigned char pin_count;
  unsigned char state;
  unsigned char reserved[14];
} managed_object_prefix;

typedef struct managed_cache_entry {
  managed_ref id;
  uint32_t reserved;
  void *payload;
} managed_cache_entry;

#endif

typedef struct managed_heap_state {
  managed_cache_entry cache[MANAGED_CACHE_COUNT];
  managed_page *root;
  managed_destructor_dispatch destroy;
  managed_fault_dispatch fault;
  managed_ref live_count;
  managed_ref pin_count;
  unsigned char lifecycle;
} managed_heap_state;

#ifdef __SDCC
typedef char managed_assert_ref_size[(sizeof(managed_ref) == 2) ? 1 : -1];
typedef char managed_assert_static_entry[(sizeof(managed_static_entry) == 4)
                                             ? 1
                                             : -1];
typedef char managed_assert_access[(sizeof(managed_access_token) == 2) ? 1
                                                                        : -1];
typedef char managed_assert_pin[(sizeof(managed_pin_token) == 6) ? 1 : -1];
typedef char managed_assert_prefix[(sizeof(managed_object_prefix) == 2) ? 1
                                                                         : -1];
typedef char managed_assert_raw_header[(sizeof(rift_block_header) == 6) ? 1
                                                                        : -1];
typedef char managed_assert_page[(sizeof(managed_page) == 40) ? 1 : -1];
typedef char managed_assert_state[(sizeof(managed_heap_state) <= 48) ? 1 : -1];
#else
_Static_assert(sizeof(managed_ref) == 4, "host managed ref layout");
_Static_assert(sizeof(managed_static_entry) == 16,
               "host static entry layout");
_Static_assert(sizeof(managed_access_token) == 8,
               "host access token layout");
_Static_assert(sizeof(managed_pin_token) == 16, "host pin token layout");
_Static_assert(sizeof(managed_object_prefix) == 16,
               "host managed object prefix layout");
_Static_assert(sizeof(rift_block_header) == 24,
               "host managed raw header layout");
_Static_assert(sizeof(managed_page) == 160, "host radix page layout");
#endif

static managed_heap_state managed_state;

#ifdef RIFT_MANAGED_TEST
static managed_heap_test_stats test_stats;
static int test_allocations_before_failure = -1;
#define MANAGED_TEST_INC(field) (test_stats.field++)
#else
#define MANAGED_TEST_INC(field) ((void)0)
#endif

static void managed_fault(managed_fault_reason reason, size_t requested)
    MANAGED_PRIVATE_CALLEE {
  managed_state.fault(reason, requested);
  exit(1);
}

#ifdef __SDCC
#define raw_header(raw_payload) (((rift_block_header *)(raw_payload)) - 1)
#define page_level(page) \
  ((unsigned char)((page)->header.meta >> MANAGED_META_LEVEL_SHIFT))
#define page_count(page) \
  ((unsigned char)((page)->header.meta & MANAGED_META_COUNT_MASK))
#define page_set_count(page, count) \
  ((page)->header.meta = (unsigned char)( \
       ((page)->header.meta & ~MANAGED_META_COUNT_MASK) | (count)))
#define page_is_full(page) ((page)->bitmap == UINT16_MAX)
#define terminal_leaf_base() \
  ((managed_ref)(MANAGED_REF_DYNAMIC_MAX - 15u))
#define cache_slot(id) \
  ((unsigned int)((id) & (managed_ref)(MANAGED_CACHE_COUNT - 1u)))
#define object_prefix(payload) \
  ((managed_object_prefix *)((unsigned char *)(payload) - \
                              sizeof(managed_object_prefix)))
#define object_raw_payload(payload) \
  ((void *)((unsigned char *)(payload) - sizeof(managed_object_prefix)))
#else
static rift_block_header *raw_header(void *raw_payload) {
  return ((rift_block_header *)raw_payload) - 1;
}
#endif

#if defined(__SDCC) && !defined(RIFT_MANAGED_TEST)
#define set_live_tag(raw_payload, tag) \
  (raw_header(raw_payload)->next_free = (tag))
#define private_try_alloc(bytes) rift_heap_try_alloc(bytes)
#define private_free(payload) ((void)rift_heap_try_free(payload))
#else
static void set_live_tag(void *raw_payload, rift_pool_offset_t tag) {
  raw_header(raw_payload)->next_free = tag;
}

static void *private_try_alloc(size_t bytes) {
#ifdef RIFT_MANAGED_TEST
  if (test_allocations_before_failure == 0) return NULL;
  if (test_allocations_before_failure > 0) test_allocations_before_failure--;
#endif
  return rift_heap_try_alloc(bytes);
}

static void private_free(void *payload) {
  if (rift_heap_try_free(payload) != RIFT_HEAP_FREE_OK)
    managed_fault(MANAGED_FAULT_INTERNAL, 0);
}
#endif

#ifndef __SDCC
static unsigned int page_level(const managed_page *page) {
  return (unsigned int)(page->header.meta >> MANAGED_META_LEVEL_SHIFT);
}

static unsigned int page_count(const managed_page *page) {
  return (unsigned int)(page->header.meta & MANAGED_META_COUNT_MASK);
}

static void page_set_count(managed_page *page, unsigned int count) {
  page->header.meta =
      (unsigned char)((page->header.meta & ~MANAGED_META_COUNT_MASK) | count);
}

static int page_is_full(const managed_page *page) {
  return page->bitmap == UINT16_MAX;
}

static managed_ref terminal_leaf_base(void) {
  return (managed_ref)(MANAGED_REF_DYNAMIC_MAX - 15u);
}
#endif

#ifdef __SDCC
#define page_init(page, level, leaf_base, root)                         \
  do {                                                                 \
    memset((page), 0, sizeof(*(page)));                                \
    (page)->header.meta =                                              \
        (unsigned char)((level) << MANAGED_META_LEVEL_SHIFT);          \
    if ((root) && (level) == MANAGED_MAX_LEVEL)                        \
      (page)->bitmap = 0xff00u;                                        \
    if ((level) == 0 && (leaf_base) == terminal_leaf_base())           \
      (page)->bitmap |= 0x8000u;                                       \
  } while (0)
#else
static void page_init(managed_page *page, unsigned int level,
                      managed_ref leaf_base, int root) {
  memset(page, 0, sizeof(*page));
  page->header.meta = (unsigned char)(level << MANAGED_META_LEVEL_SHIFT);
  if (root && level == MANAGED_MAX_LEVEL) page->bitmap = 0xff00u;
  if (level == 0 && leaf_base == terminal_leaf_base())
    page->bitmap |= 0x8000u;
}
#endif

static unsigned char first_not_full(uint16_t bitmap) RIFT_MANAGED_FASTCALL {
  unsigned char slot = 0;
  while (bitmap & 1u) {
    bitmap >>= 1;
    slot++;
  }
  return slot;
}

static void page_link_child(managed_page *parent, unsigned char slot,
                            managed_page *child) MANAGED_PRIVATE_CALLEE {
  parent->slots[slot] = child;
  page_set_count(parent, page_count(parent) + 1u);
  if (page_is_full(child)) parent->bitmap |= (uint16_t)(1u << slot);
  child->header.parent = parent;
  child->header.parent_slot = (unsigned char)slot;
}

#ifndef __SDCC
static unsigned int cache_slot(managed_ref id) {
  return (unsigned int)(id & (managed_ref)(MANAGED_CACHE_COUNT - 1u));
}
#endif

#ifdef __SDCC
#define cache_invalidate(ref_value)                                      \
  do {                                                                   \
    managed_cache_entry *invalidated =                                  \
        &managed_state.cache[cache_slot(ref_value)];                     \
    invalidated->id = 0;                                                 \
    invalidated->payload = NULL;                                         \
  } while (0)
#else
static void cache_invalidate(managed_ref id) {
  managed_cache_entry *entry = &managed_state.cache[cache_slot(id)];
  entry->id = 0;
  entry->payload = NULL;
}
#endif

static managed_page *find_leaf(managed_ref id) RIFT_MANAGED_FASTCALL {
  managed_page *page = managed_state.root;
  managed_ref key;
  unsigned char level;
  if (!page || !id || id > MANAGED_REF_DYNAMIC_MAX) return NULL;
  key = (managed_ref)(id - 1u);
  level = page_level(page);
  if (level < MANAGED_MAX_LEVEL &&
      key >= ((managed_ref)1u << (MANAGED_RADIX_BITS * (level + 1u))))
    return NULL;
  while (level > 0) {
    unsigned char slot =
        (unsigned char)((key >> (MANAGED_RADIX_BITS * level)) & 0x0fu);
    MANAGED_TEST_INC(node_visits);
    page = (managed_page *)page->slots[slot];
    if (!page) return NULL;
    level--;
  }
  MANAGED_TEST_INC(node_visits);
  return page;
}

#ifndef __SDCC
static void *lookup_uncached(managed_ref id) {
  managed_page *leaf = find_leaf(id);
  unsigned char slot;
  if (!leaf) return NULL;
  slot = (unsigned char)((id - 1u) & 0x0fu);
  if (!(leaf->bitmap & (uint16_t)(1u << slot))) return NULL;
  return leaf->slots[slot];
}
#endif

static void *lookup(managed_ref id) RIFT_MANAGED_FASTCALL {
  managed_cache_entry *entry = &managed_state.cache[cache_slot(id)];
  void *payload;
  if (entry->id == id && entry->payload) {
    MANAGED_TEST_INC(cache_hits);
    return entry->payload;
  }
  MANAGED_TEST_INC(cache_misses);
#ifdef __SDCC
  {
    managed_page *leaf = find_leaf(id);
    unsigned char slot = (unsigned char)((id - 1u) & 0x0fu);
    payload = leaf && (leaf->bitmap & (uint16_t)(1u << slot))
                  ? leaf->slots[slot]
                  : NULL;
  }
#else
  payload = lookup_uncached(id);
#endif
  if (payload) {
    entry->id = id;
    entry->payload = payload;
  }
  return payload;
}

static void *static_payload(managed_ref ref) RIFT_MANAGED_FASTCALL {
  size_t index = (size_t)(ref & ~MANAGED_REF_STATIC_BIT);
  if (index >= rift_managed_static_count)
    managed_fault(MANAGED_FAULT_INVALID_REF, ref);
  return (void *)rift_managed_static_table[index].payload;
}

static void propagate_full(managed_page *page) RIFT_MANAGED_FASTCALL {
  while (page_is_full(page) && page->header.parent) {
    managed_page *parent = page->header.parent;
    uint16_t bit = (uint16_t)(1u << page->header.parent_slot);
    if (parent->bitmap & bit) break;
    parent->bitmap |= bit;
    page = parent;
  }
}

static void propagate_not_full(managed_page *page) RIFT_MANAGED_FASTCALL {
  while (page->header.parent) {
    managed_page *parent = page->header.parent;
    uint16_t bit = (uint16_t)(1u << page->header.parent_slot);
    parent->bitmap &= (uint16_t)~bit;
    page = parent;
  }
}

static void release_empty_path(managed_page *page) RIFT_MANAGED_FASTCALL {
  while (page_count(page) == 0) {
    managed_page *parent = page->header.parent;
    if (!parent) {
      managed_state.root = NULL;
      private_free(page);
      return;
    }
    {
      unsigned char slot = page->header.parent_slot;
      parent->slots[slot] = NULL;
      page_set_count(parent, page_count(parent) - 1u);
    }
    private_free(page);
    page = parent;
  }
  while (managed_state.root && page_level(managed_state.root) > 0 &&
         page_count(managed_state.root) == 1u &&
         managed_state.root->slots[0]) {
    managed_page *old = managed_state.root;
    managed_state.root = (managed_page *)old->slots[0];
    managed_state.root->header.parent = NULL;
    managed_state.root->header.parent_slot = 0;
    private_free(old);
  }
}

static void finish_detached_path(managed_page *page, unsigned char first_slot,
                                 unsigned char activate)
    MANAGED_PRIVATE_CALLEE {
  while (page) {
    managed_page *next = page_level(page) > 0
                             ? (managed_page *)page->slots[first_slot]
                             : NULL;
    if (activate)
      set_live_tag(page, MANAGED_TAG_LEVEL(page_level(page)));
    else
      private_free(page);
    page = next;
    first_slot = 0;
  }
}

#ifndef __SDCC
static managed_object_prefix *object_prefix(void *payload) {
  return (managed_object_prefix *)((unsigned char *)payload -
                                    sizeof(managed_object_prefix));
}

static void *object_raw_payload(void *payload) {
  return (unsigned char *)payload - sizeof(managed_object_prefix);
}
#endif

static void *require_live(managed_ref ref) RIFT_MANAGED_FASTCALL {
  void *payload;
  managed_object_prefix *prefix;
  payload = lookup(ref);
  if (!payload) managed_fault(MANAGED_FAULT_INVALID_REF, ref);
  prefix = object_prefix(payload);
  if (prefix->state == MANAGED_STATE_DESTROYING)
    managed_fault(MANAGED_FAULT_DESTROYING, ref);
  if (prefix->state != MANAGED_STATE_LIVE)
    managed_fault(MANAGED_FAULT_INTERNAL, ref);
  return payload;
}

static void retain_payload(managed_ref ref, void *payload)
    MANAGED_PRIVATE_CALLEE {
  rift_block_header *header = raw_header(object_raw_payload(payload));
  if (header->refcount >= RIFT_RC_MAX_LIVE)
    managed_fault(MANAGED_FAULT_REFCOUNT_OVERFLOW, ref);
  header->refcount++;
}

void managed_heap_init(const managed_heap_options *options)
    RIFT_MANAGED_FASTCALL {
  if (managed_state.lifecycle == MANAGED_LIFECYCLE_READY)
    managed_fault(MANAGED_FAULT_INTERNAL, 0);
  if (!options || !options->fault) {
    rift_error_text("rift: managed heap requires a fault sink\n");
    exit(1);
  }
  memset(&managed_state, 0, sizeof(managed_state));
  managed_state.destroy = options->destroy;
  managed_state.fault = options->fault;
  managed_state.lifecycle = MANAGED_LIFECYCLE_READY;
#ifdef RIFT_MANAGED_TEST
  memset(&test_stats, 0, sizeof(test_stats));
  test_allocations_before_failure = -1;
#endif
}

void managed_heap_deinit(void) {
  if (managed_state.lifecycle != MANAGED_LIFECYCLE_READY)
    managed_fault(MANAGED_FAULT_INTERNAL, 0);
  if (managed_state.live_count || managed_state.pin_count || managed_state.root)
    managed_fault(MANAGED_FAULT_LEAK, managed_state.live_count);
  memset(&managed_state, 0, sizeof(managed_state));
}

managed_ref managed_heap_alloc(size_t bytes) RIFT_MANAGED_FASTCALL {
  managed_page *parent = NULL;
  managed_page *leaf = NULL;
  managed_page *page;
  managed_page *detached = NULL;
  managed_page *detached_tail = NULL;
  unsigned char parent_slot = 0;
  unsigned char page_count_needed = 0;
  unsigned char first_level = 0;
  unsigned char leaf_slot = 0;
  managed_ref key = 0;
  managed_ref leaf_base;
  managed_ref id;
  void *raw_payload;
  void *payload;
  managed_object_prefix *prefix;
  unsigned char grow_root = 0;

  if (bytes > SIZE_MAX - sizeof(managed_object_prefix))
    managed_fault(MANAGED_FAULT_CAPACITY, bytes);

  page = managed_state.root;
  if (!page) {
    page_count_needed = 1;
  } else if (page_is_full(page)) {
    unsigned char old_level = (unsigned char)page_level(page);
    if (old_level == MANAGED_MAX_LEVEL)
      managed_fault(MANAGED_FAULT_CAPACITY, bytes);
    grow_root = 1;
    first_level = old_level + 1u;
    page_count_needed = first_level + 1u;
    key = (managed_ref)1u << (MANAGED_RADIX_BITS * first_level);
  } else {
    unsigned char level = (unsigned char)page_level(page);
    while (level > 0) {
      unsigned char slot = first_not_full(page->bitmap);
      key = (managed_ref)((key << MANAGED_RADIX_BITS) | slot);
      if (!page->slots[slot]) {
        parent = page;
        parent_slot = slot;
        first_level = level - 1u;
        page_count_needed = level;
        key <<= MANAGED_RADIX_BITS * level;
        break;
      }
      page = (managed_page *)page->slots[slot];
      level--;
    }
    if (level == 0 && page_count_needed == 0) {
      leaf = page;
      leaf_slot = first_not_full(leaf->bitmap);
      key = (managed_ref)((key << MANAGED_RADIX_BITS) | leaf_slot);
    }
  }

  leaf_base = (managed_ref)(key & (managed_ref)~0x0fu);
  if (page_count_needed) {
    unsigned char remaining = page_count_needed;
    unsigned char level = first_level;
    while (remaining) {
      managed_page *reserved =
          (managed_page *)private_try_alloc(sizeof(managed_page));
      if (!reserved) {
        finish_detached_path(detached, grow_root ? 1u : 0u, 0);
        managed_fault(MANAGED_FAULT_CAPACITY, bytes);
      }
      page_init(reserved, level, leaf_base,
                grow_root && !detached);
      set_live_tag(reserved, MANAGED_TAG_RESERVED);
      if (!detached)
        detached = reserved;
      else
        page_link_child(detached_tail,
                        grow_root && detached_tail == detached ? 1u : 0u,
                        reserved);
      detached_tail = reserved;
#ifdef RIFT_MANAGED_TEST
      test_stats.node_bytes += sizeof(managed_page);
#endif
      remaining--;
      if (remaining) level--;
    }
    leaf = detached_tail;
  }

  raw_payload = private_try_alloc(sizeof(managed_object_prefix) + bytes);
  if (!raw_payload) {
    finish_detached_path(detached, grow_root ? 1u : 0u, 0);
    managed_fault(MANAGED_FAULT_CAPACITY, bytes);
  }
  prefix = (managed_object_prefix *)raw_payload;
  prefix->pin_count = 0;
  prefix->state = MANAGED_STATE_LIVE;
  payload = (unsigned char *)raw_payload + sizeof(*prefix);
  id = (managed_ref)(key + 1u);
  set_live_tag(raw_payload, (rift_pool_offset_t)id);

  if (page_count_needed) {
    finish_detached_path(detached, grow_root ? 1u : 0u, 1);
    if (!managed_state.root) {
      managed_state.root = detached;
    } else if (grow_root) {
      managed_page *old_root = managed_state.root;
      page_link_child(detached, 0, old_root);
      managed_state.root = detached;
    } else {
      page_link_child(parent, parent_slot, detached);
    }
    leaf_slot = (unsigned char)(key & 0x0fu);
  }

  leaf->slots[leaf_slot] = payload;
  leaf->bitmap |= (uint16_t)(1u << leaf_slot);
  page_set_count(leaf, page_count(leaf) + 1u);
  propagate_full(leaf);
  managed_state.live_count++;
  {
    managed_cache_entry *entry = &managed_state.cache[cache_slot(id)];
    entry->id = id;
    entry->payload = payload;
  }
  return id;
}

managed_ref managed_heap_retain(managed_ref ref) RIFT_MANAGED_FASTCALL {
  void *payload;
  if (!ref || managed_ref_is_static(ref)) return ref;
  payload = require_live(ref);
  retain_payload(ref, payload);
  return ref;
}

static void *directory_remove_destroying(managed_ref ref)
    RIFT_MANAGED_FASTCALL {
  managed_page *leaf = find_leaf(ref);
  unsigned char slot = (unsigned char)((ref - 1u) & 0x0fu);
  void *payload;
  if (!leaf || !(leaf->bitmap & (uint16_t)(1u << slot)))
    managed_fault(MANAGED_FAULT_INTERNAL, ref);
  payload = leaf->slots[slot];
  if (object_prefix(payload)->state != MANAGED_STATE_DESTROYING)
    managed_fault(MANAGED_FAULT_INTERNAL, ref);
  leaf->slots[slot] = NULL;
  leaf->bitmap &= (uint16_t)~(uint16_t)(1u << slot);
  page_set_count(leaf, page_count(leaf) - 1u);
  cache_invalidate(ref);
  propagate_not_full(leaf);
  release_empty_path(leaf);
  return payload;
}

void managed_heap_release_typed(managed_ref ref, managed_type_id type)
    RIFT_MANAGED_CALLEE {
  void *payload;
  managed_object_prefix *prefix;
  void *raw_payload;
  rift_block_header *header;
  if (!ref || managed_ref_is_static(ref)) return;
  payload = require_live(ref);
  prefix = object_prefix(payload);
  raw_payload = object_raw_payload(payload);
  header = raw_header(raw_payload);
  if (header->refcount == 0 || header->refcount > RIFT_RC_MAX_LIVE)
    managed_fault(MANAGED_FAULT_INTERNAL, ref);
  if (header->refcount > 1u) {
    header->refcount--;
    return;
  }
  if (prefix->pin_count != 0)
    managed_fault(MANAGED_FAULT_INTERNAL, ref);
  prefix->state = MANAGED_STATE_DESTROYING;
  header->refcount = RIFT_RC_DESTROYING;
  if (managed_state.destroy) managed_state.destroy(type, payload);
  payload = directory_remove_destroying(ref);
  raw_payload = object_raw_payload(payload);
  managed_state.live_count--;
  private_free(raw_payload);
}

#ifdef __SDCC
void managed_heap_access_begin(managed_ref ref, managed_access_token *token)
    RIFT_MANAGED_CALLEE __naked {
  (void)ref;
  (void)token;
  __asm
    pop af
    pop hl
    pop bc
    push af

    ld a,h
    or l
    jr z,004$
    bit 7,h
    jr nz,003$

    push hl
    ld a,l
    and #0x07
    add a,a
    add a,a
    ld e,a
    ld d,#0
    ld hl,#_managed_state
    add hl,de
    pop de

    ld a,(hl)
    cp e
    jr nz,002$
    inc hl
    ld a,(hl)
    cp d
    jr nz,002$
    inc hl
    ld a,(hl)
    inc hl
    ld h,(hl)
    ld l,a
    ld a,h
    or l
    jr z,002$
    dec hl
    ld a,(hl)
    inc hl
    cp #0x51
    jr z,005$

002$:
    push bc
    ex de,hl
    call _require_live
    pop bc
    jr 005$

003$:
    push bc
    call _static_payload
    pop bc
    jr 005$

004$:
    ld hl,#0

005$:
    ld a,l
    ld (bc),a
    inc bc
    ld a,h
    ld (bc),a
    ret
  __endasm;
}
#else
void managed_heap_access_begin(managed_ref ref, managed_access_token *token)
    RIFT_MANAGED_CALLEE {
  if (!ref)
    token->payload = NULL;
  else if (managed_ref_is_static(ref))
    token->payload = static_payload(ref);
  else
    token->payload = require_live(ref);
}
#endif

void managed_heap_pin_typed(managed_ref ref, managed_type_id type,
                            managed_pin_token *token) RIFT_MANAGED_CALLEE {
  void *payload;
  managed_object_prefix *prefix;
  if (!ref) {
    payload = NULL;
    goto publish;
  }
  if (managed_ref_is_static(ref)) {
    payload = static_payload(ref);
    goto publish;
  }
  payload = require_live(ref);
  prefix = object_prefix(payload);
  if (prefix->pin_count == 0xffu)
    managed_fault(MANAGED_FAULT_PIN_OVERFLOW, ref);
  retain_payload(ref, payload);
  prefix->pin_count++;
  managed_state.pin_count++;

publish:
  token->payload = payload;
  token->ref = ref;
  token->type = type;
}

static void pin_token_clear(managed_pin_token *token) RIFT_MANAGED_FASTCALL {
  token->payload = NULL;
  token->ref = 0;
  token->type = 0;
}

void managed_heap_unpin_typed(managed_pin_token *token) RIFT_MANAGED_FASTCALL {
  managed_ref ref;
  managed_type_id type;
  void *payload;
  managed_object_prefix *prefix;
  if (!token || !token->ref)
    managed_fault(MANAGED_FAULT_INVALID_REF, 0);
  if (managed_ref_is_static(token->ref)) {
    pin_token_clear(token);
    return;
  }
  ref = token->ref;
  type = token->type;
  payload = token->payload;
  if (!payload)
    managed_fault(MANAGED_FAULT_INVALID_REF, ref);
  prefix = object_prefix(payload);
  if (prefix->state != MANAGED_STATE_LIVE || prefix->pin_count == 0)
    managed_fault(MANAGED_FAULT_INVALID_REF, ref);
  pin_token_clear(token);
  prefix->pin_count--;
  managed_state.pin_count--;
  managed_heap_release_typed(ref, type);
}

size_t managed_heap_live_count(void) { return managed_state.live_count; }
size_t managed_heap_pin_count(void) { return managed_state.pin_count; }

#ifdef RIFT_MANAGED_TEST
managed_heap_test_stats managed_heap_test_stats_get(void) {
  managed_heap_test_stats result = test_stats;
  result.tree_height = managed_state.root ? page_level(managed_state.root) + 1u
                                          : 0;
  return result;
}

size_t managed_heap_test_digest(void) {
  size_t digest = (size_t)managed_state.live_count * 131u +
                  (size_t)managed_state.pin_count * 17u;
  managed_ref id;
  for (id = 1; id <= managed_state.live_count + 32u && id != 0; id++) {
    void *payload = lookup_uncached(id);
    if (payload) digest = digest * 33u + id;
  }
#ifdef RIFT_ALLOCATOR_TEST
  digest ^= rift_heap_test_digest();
#endif
  return digest;
}

void managed_heap_test_fail_after(int successful_allocations) {
  test_allocations_before_failure = successful_allocations;
}

uint16_t managed_heap_test_refcount(managed_ref ref) {
  void *payload = lookup_uncached(ref);
  return payload ? raw_header(object_raw_payload(payload))->refcount : 0;
}

unsigned int managed_heap_test_pin_count(managed_ref ref) {
  void *payload = lookup_uncached(ref);
  return payload ? object_prefix(payload)->pin_count : 0;
}

void managed_heap_test_set_counts(managed_ref ref, uint16_t refcount,
                                  unsigned int pin_count) {
  void *payload = lookup_uncached(ref);
  managed_object_prefix *prefix = object_prefix(payload);
  raw_header(object_raw_payload(payload))->refcount = refcount;
  managed_state.pin_count -= prefix->pin_count;
  prefix->pin_count = (unsigned char)pin_count;
  managed_state.pin_count += prefix->pin_count;
}
#endif
