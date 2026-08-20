#ifndef RIFT_MANAGED_REF_H
#define RIFT_MANAGED_REF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __SDCC
typedef uint16_t managed_ref;
typedef uint16_t managed_type_id;
#define MANAGED_REF_STATIC_BIT ((managed_ref)0x8000u)
#define MANAGED_REF_DYNAMIC_MAX ((managed_ref)0x7fffu)
#else
typedef uint32_t managed_ref;
typedef uint32_t managed_type_id;
#define MANAGED_REF_STATIC_BIT ((managed_ref)0x80000000u)
#define MANAGED_REF_DYNAMIC_MAX ((managed_ref)0x7fffffffu)
#endif

typedef struct managed_static_entry {
  const void *payload;
  size_t size;
} managed_static_entry;

typedef struct managed_access_token {
  void *payload;
} managed_access_token;

typedef struct managed_pin_token {
  void *payload;
  managed_ref ref;
  managed_type_id type;
} managed_pin_token;

extern const managed_static_entry rift_managed_static_table[];
extern const size_t rift_managed_static_count;

#ifndef RIFT_MANAGED_DYNAMIC
#define RIFT_MANAGED_DYNAMIC 1
#endif

#ifdef __SDCC
#define RIFT_MANAGED_FASTCALL __z88dk_fastcall
#define RIFT_MANAGED_CALLEE __z88dk_callee
#else
#define RIFT_MANAGED_FASTCALL
#define RIFT_MANAGED_CALLEE
#endif

#if RIFT_MANAGED_DYNAMIC
managed_ref managed_heap_alloc(size_t bytes) RIFT_MANAGED_FASTCALL;
managed_ref managed_heap_retain(managed_ref ref) RIFT_MANAGED_FASTCALL;
void managed_heap_release_typed(managed_ref ref, managed_type_id type)
    RIFT_MANAGED_CALLEE;
void managed_heap_access_begin(managed_ref ref, managed_access_token *token)
    RIFT_MANAGED_CALLEE;
void managed_heap_pin_typed(managed_ref ref, managed_type_id type,
                            managed_pin_token *token) RIFT_MANAGED_CALLEE;
void managed_heap_unpin_typed(managed_pin_token *token) RIFT_MANAGED_FASTCALL;
#endif

#define managed_ref_is_static(ref) (((ref) & MANAGED_REF_STATIC_BIT) != 0)

#if defined(RIFT_MANAGED_IMPLEMENTATION)

#elif RIFT_MANAGED_DYNAMIC

#define managed_alloc(bytes) managed_heap_alloc(bytes)
#define managed_retain(ref) managed_heap_retain(ref)
#define managed_release_typed(ref, type) \
  managed_heap_release_typed((ref), (type))
#define managed_access_begin(ref, token) \
  managed_heap_access_begin((ref), (token))
#define managed_access_ptr(token) ((token) ? (token)->payload : NULL)
#define managed_access_end(token) \
  do {                            \
    if (token) (token)->payload = NULL; \
  } while (0)
#define managed_pin_typed(ref, type, token) \
  managed_heap_pin_typed((ref), (type), (token))
#define managed_pin_ptr(token) ((token) ? (token)->payload : NULL)
#define managed_unpin_typed(token) managed_heap_unpin_typed(token)

#else

static inline const managed_static_entry *managed_static_lookup(
    managed_ref ref) {
  size_t index = (size_t)(ref & ~MANAGED_REF_STATIC_BIT);
  if (!managed_ref_is_static(ref) || index >= rift_managed_static_count)
    return NULL;
  return &rift_managed_static_table[index];
}

static inline managed_ref managed_alloc(size_t bytes) {
  (void)bytes;
  return (managed_ref)0;
}

static inline managed_ref managed_retain(managed_ref ref) {
  if (!ref || managed_ref_is_static(ref)) return ref;
  return (managed_ref)0;
}

static inline void managed_release_typed(managed_ref ref,
                                         managed_type_id type) {
  if (!ref || managed_ref_is_static(ref)) return;
  (void)type;
}

static inline void managed_access_begin(managed_ref ref,
                                        managed_access_token *token) {
  const managed_static_entry *entry;
  if (!token) return;
  token->payload = NULL;
  if (!ref) return;
  if (managed_ref_is_static(ref)) {
    entry = managed_static_lookup(ref);
    token->payload = entry ? (void *)entry->payload : NULL;
    return;
  }
}

static inline void *managed_access_ptr(const managed_access_token *token) {
  return token ? token->payload : NULL;
}

static inline void managed_access_end(managed_access_token *token) {
  if (!token) return;
  token->payload = NULL;
}

static inline void managed_pin_typed(managed_ref ref, managed_type_id type,
                                     managed_pin_token *token) {
  const managed_static_entry *entry;
  if (!token) return;
  token->payload = NULL;
  token->ref = 0;
  token->type = type;
  if (!ref) return;
  if (managed_ref_is_static(ref)) {
    entry = managed_static_lookup(ref);
    if (entry) {
      token->payload = (void *)entry->payload;
      token->ref = ref;
    }
    return;
  }
}

static inline void *managed_pin_ptr(const managed_pin_token *token) {
  return token ? token->payload : NULL;
}

static inline void managed_unpin_typed(managed_pin_token *token) {
  if (!token || !token->ref) return;
  if (managed_ref_is_static(token->ref)) {
    token->payload = NULL;
    token->ref = 0;
    token->type = 0;
    return;
  }
}
#endif

#endif
