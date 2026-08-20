#include "fundefs.h"
#include "fundefs_internal.h"
#include "error_sink.h"
#include "pools.h"
#include <stdlib.h>
#include <string.h>

// ============================================================================
// SIMPLE UTILITIES (safe for sccz80)
// ============================================================================

char *string_to_cstr(string s) { return s.data; }

void __rift_make_string(string *out, const char *data, size_t length) {
  out->data = (char *)data;
  out->length = length;
  out->capacity = 0;       /* read-only view by default */
  out->backing = NULL;     /* no refcounted backing (Phase E will populate) */
}

/* ADR-0003 §7.1: allocate a fresh writable string in the longlived pool.
 * Returns through `out` with backing pointing at the rift_block_header
 * (refcount = 1 from rift_longlived_alloc). The caller writes `length+1`
 * bytes into out->data (including the null terminator). */
void __rift_make_longlived_string(string *out, size_t length) {
  char *payload;
  if (length == SIZE_MAX) {
    rift_error_text("string: length exceeds addressable storage\n");
    exit_rift(1);
  }
  payload = (char *)rift_longlived_alloc(length + 1);
  if (payload == NULL) {
    /* A custom OOM observer is allowed to return. String construction cannot
     * produce a valid descriptor without backing, so fail deterministically
     * rather than dereferencing a missing allocation. */
    rift_error_text("string: allocation failed\n");
    exit_rift(1);
  }
  out->data     = payload;
  out->length   = length;
  out->backing  = ((rift_block_header *)payload) - 1;
  /* Alignment may provide more payload than the requested length.
   * Record that already-paid space so unique concat assignments can grow
   * without another allocation. */
  out->capacity = (size_t)out->backing->size - 1;
}

char charAt(string s, int n) {
  if (s.data == NULL || n >= s.length)
    return 0;
  return s.data[n];
}

int equals(string s1, string s2) {
  if (s1.data == NULL && s2.data == NULL)
    return 1;
  if (s1.data == NULL || s2.data == NULL)
    return 0;
  if (s1.length != s2.length)
    return 0;
  return memcmp(s1.data, s2.data, s1.length) == 0;
}

size_t __length_string(string s) { return s.length; }

// ============================================================================
// STRING OPERATIONS (available everywhere with out-param convention)
// ============================================================================

void print(string s) {
  if (s.data == NULL) {
    rift_print_bytes("NULL", 4);
    return;
  }
  rift_print_bytes(s.data, s.length);
}

void cstr_to_string(string *out, char *cstr) {
  __rift_make_string(out, cstr, strlen(cstr));
}

void __concat_char(string *out, string s, char c) {
  if (s.data == NULL) {
    __rift_make_longlived_string(out, 1);
    out->data[0] = c;
    out->data[1] = 0;
    return;
  }
  __rift_make_longlived_string(out, __concat_checked_add(s.length, 1));
  memcpy(out->data, s.data, s.length);
  out->data[s.length] = c;
  out->data[s.length + 1] = 0;
}

void __concat_str(string *out, string s1, string s2) {
  size_t len1 = (s1.data == NULL) ? 0 : s1.length;
  size_t len2 = (s2.data == NULL) ? 0 : s2.length;
  size_t total = __concat_checked_add(len1, len2);
  __rift_make_longlived_string(out, total);
  if (s1.data != NULL)
    memcpy(out->data, s1.data, len1);
  if (s2.data != NULL)
    memcpy(&out->data[len1], s2.data, len2);
  out->data[total] = 0;
}

size_t __concat_checked_add(size_t total, size_t addition) {
  if (addition > (size_t)-1 - total) {
    rift_print_bytes("concat: result is too large\n", 28);
    exit_rift(1);
  }
  return total + addition;
}

size_t __concat_append_bytes(string out, size_t offset,
                             const char *data, size_t length) {
  if (offset > out.length || length > out.length - offset) {
    rift_error_text("concat: append exceeds allocated result\n");
    exit_rift(1);
  }
  if (data != NULL && length != 0) memcpy(out.data + offset, data, length);
  return offset + length;
}

void __concat_append_owned(string *value, const char *data, size_t length,
                           unsigned int owned_refcount) {
  size_t total = __concat_checked_add(value->length, length);
  size_t backing_capacity = 0;
  int can_reuse = 0;

  if (length != 0 && data == NULL) {
    rift_error_text("concat: non-empty source has no data\n");
    exit_rift(1);
  }

  if (value->backing != NULL &&
      value->backing->refcount == owned_refcount &&
      value->backing->refcount != RIFT_RC_STATIC &&
      value->backing->refcount != RIFT_RC_FREE &&
      value->data == (char *)(value->backing + 1) &&
      value->backing->size != 0) {
    backing_capacity = (size_t)value->backing->size - 1;
    can_reuse = value->capacity == backing_capacity && total <= backing_capacity;
  }

  if (can_reuse) {
    if (length != 0)
      memmove(value->data + value->length, data, length);
    value->length = total;
    value->data[total] = 0;
    return;
  }

  {
    string replacement;
    size_t replacement_capacity = total;
    if (backing_capacity != 0 && backing_capacity < total &&
        backing_capacity <= (SIZE_MAX - 1) / 2) {
      replacement_capacity = backing_capacity * 2;
      if (replacement_capacity < total) replacement_capacity = total;
    }
    __rift_make_longlived_string(&replacement, replacement_capacity);
    replacement.length = total;
    if (value->data != NULL && value->length != 0)
      memcpy(replacement.data, value->data, value->length);
    if (length != 0)
      memcpy(replacement.data + value->length, data, length);
    replacement.data[total] = 0;
    __string_release(*value);
    *value = replacement;
  }
}

void new_string(string *out, string s) {
  __rift_make_longlived_string(out, s.length);
  for (size_t i = 0; i < out->length; i++)
    out->data[i] = s.data[i];
  out->data[out->length] = 0;
}

void setCharAt(string s, int n, char c) {
  /* ADR-0003 §7.3: setCharAt requires writable backing. capacity == 0
   * means the descriptor is a read-only view (string literal, substring
   * of any source). Mutation through such a descriptor would corrupt
   * shared bytes (literal in rodata; the source of a substring). */
  if (s.capacity == 0) {
    rift_error_text("setCharAt: cannot mutate read-only string view "
                    "(literal or substring; capacity == 0)\n");
    exit_rift(1);
  }
  if (n < 0 || (size_t)n >= s.length) {
    rift_error_text("setCharAt: index ");
    rift_error_int(n);
    rift_error_text(" out of bounds (length=");
    rift_error_size(s.length);
    rift_error_text(")\n");
    exit_rift(1);
  }
  s.data[n] = c;
}

/* Retain/release on string backing. NULL backing denotes a borrowed view;
 * static literal backing is immortal; all produced mutable strings live in
 * the long-lived pool. */

void __string_retain(string s) {
  if (s.backing == NULL) return;
  if (s.backing->refcount == RIFT_RC_STATIC) return;
  if (s.backing->refcount == RIFT_RC_FREE) {
    rift_error_text("rift: __string_retain on freed block\n");
    exit(1);
  }
  if (s.backing->refcount >= RIFT_RC_FREE - 1) {
    rift_error_text("rift: __string_retain refcount overflow\n");
    exit(1);
  }
  s.backing->refcount++;
}

void __string_release(string s) {
  if (s.backing == NULL) return;
  if (s.backing->refcount == RIFT_RC_STATIC) return;
  if (s.backing->refcount == RIFT_RC_FREE) {
    rift_error_text("rift: __string_release on already-freed block\n");
    exit(1);
  }
  if (--s.backing->refcount == 0) {
    /* Payload pointer is just past the header. */
    void *payload = (void *)((char *)s.backing + sizeof(rift_block_header));
    rift_longlived_free(payload);
  }
}

/* ADR-0003 §7.6: return materialisation. See header for case breakdown.
 * Always returns a descriptor with caller-owned ownership semantics —
 * caller transfers (no inc, no dec) into the destination slot. */
string __return_string(string s) {
  if (s.backing == NULL) {
    /* Bump source (or NULL data). Allocate a fresh longlived block and
     * copy the bytes. The new descriptor is owned (rc=1 from
     * rift_longlived_alloc). */
    string out;
    __rift_make_longlived_string(&out, s.length);
    if (s.length > 0 && s.data != NULL) {
      memcpy(out.data, s.data, s.length);
    }
    out.data[s.length] = 0;
    return out;
  }
  if (s.backing->refcount == RIFT_RC_STATIC) {
    /* Static source — eternal lifetime, no inc needed. The caller's
     * transfer is sound because static blocks can't be released. */
    return s;
  }
  /* Longlived source — inc refcount so the caller has an owned reference
   * independent of any other live alias. */
  __string_retain(s);
  return s;
}

// ============================================================================
// STRING SLICING
// ============================================================================

int __rift_substr_index_base = 1;

void set_string_index_base(int base) { __rift_substr_index_base = base; }
int  get_string_index_base(void)     { return __rift_substr_index_base; }

static int __normalize_substr_idx(int idx, int length) {
  if (idx < 0) return length + idx;
  return idx - __rift_substr_index_base;
}

/* ADR-0003 §7.5: substring is a view. The result descriptor points into
 * the source's bytes with capacity = 0 (read-only) and inherits the
 * source's `backing`. For longlived sources, the inherited backing's
 * refcount is incremented so the source survives as long as any view
 * does. For static sources (sentinel) and bump sources (NULL), the
 * retain is a no-op. */
void __substring_from(string *out, string s, int start) {
  int c_start = __normalize_substr_idx(start, (int)s.length);
  if (c_start < 0 || c_start > (int)s.length) {
    rift_error_text("substring: start index out of bounds\n");
    exit_rift(1);
  }
  int len = (int)s.length - c_start;
  out->data     = s.data + c_start;
  out->length   = (size_t)len;
  out->capacity = 0;             /* view: read-only */
  out->backing  = s.backing;     /* inherit source's lifetime anchor */
  __string_retain(*out);
}

void __substring_range(string *out, string s, int start, int end) {
  int c_start = __normalize_substr_idx(start, (int)s.length);
  int c_end   = __normalize_substr_idx(end,   (int)s.length);
  if (c_start < 0 || c_start >= (int)s.length) {
    rift_error_text("substring: start index out of bounds\n");
    exit_rift(1);
  }
  if (c_end < c_start || c_end >= (int)s.length) {
    rift_error_text("substring: end index out of bounds\n");
    exit_rift(1);
  }
  int len = c_end - c_start + 1;
  out->data     = s.data + c_start;
  out->length   = (size_t)len;
  out->capacity = 0;
  out->backing  = s.backing;
  __string_retain(*out);
}
