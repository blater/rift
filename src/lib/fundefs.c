#include "fundefs.h"
#include "fundefs_internal.h"
#include "pools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// SIMPLE UTILITIES (safe for sccz80)
// ============================================================================

char *string_to_cstr(string s) { return s.data; }

void __rock_make_string(string *out, const char *data, size_t length) {
  out->data = (char *)data;
  out->length = length;
  out->capacity = 0;       /* read-only view by default */
  out->backing = NULL;     /* no refcounted backing (Phase E will populate) */
}

/* ADR-0003 §7.1: allocate a fresh writable string in the longlived pool.
 * Returns through `out` with backing pointing at the rock_block_header
 * (refcount = 1 from rock_longlived_alloc). The caller writes `length+1`
 * bytes into out->data (including the null terminator). */
void __rock_make_longlived_string(string *out, size_t length) {
  char *payload = (char *)rock_longlived_alloc(length + 1);
  out->data     = payload;
  out->length   = length;
  out->capacity = length;
  out->backing  = ((rock_block_header *)payload) - 1;
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

// ============================================================================
// STRING OPERATIONS (available everywhere with out-param convention)
// ============================================================================

void print(string s) {
  if (s.data == NULL) {
    printf("NULL");
    fflush(stdout);
    return;
  }
  rock_print_bytes(s.data, s.length);
}

void cstr_to_string(string *out, char *cstr) {
  __rock_make_string(out, cstr, strlen(cstr));
}

void __concat_char(string *out, string s, char c) {
  if (s.data == NULL) {
    __rock_make_longlived_string(out, 1);
    out->data[0] = c;
    out->data[1] = 0;
    return;
  }
  __rock_make_longlived_string(out, s.length + 1);
  memcpy(out->data, s.data, s.length);
  out->data[s.length] = c;
  out->data[s.length + 1] = 0;
}

void __concat_str(string *out, string s1, string s2) {
  size_t len1 = (s1.data == NULL) ? 0 : s1.length;
  size_t len2 = (s2.data == NULL) ? 0 : s2.length;
  __rock_make_longlived_string(out, len1 + len2);
  if (s1.data != NULL)
    memcpy(out->data, s1.data, len1);
  if (s2.data != NULL)
    memcpy(&out->data[len1], s2.data, len2);
  out->data[len1 + len2] = 0;
}

void new_string(string *out, string s) {
  __rock_make_longlived_string(out, s.length);
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
    printf("setCharAt: cannot mutate read-only string view "
           "(literal or substring; capacity == 0)\n");
    exit_rocker(1);
  }
  if (n < 0 || (size_t)n >= s.length) {
    printf("setCharAt: index %d out of bounds (length=%zu)\n", n, s.length);
    exit_rocker(1);
  }
  s.data[n] = c;
}

// ============================================================================
// FILE I/O (host-only, requires POSIX APIs)
// ============================================================================

#ifdef __SDCC
void __read_file_impl(string *out, string filename) {
  (void)filename;
  __rock_make_string(out, "", 0);
}

void write_string_to_file(string s, string filename) {
  (void)s;
  (void)filename;
}

void __get_abs_path_impl(string *out, string path) {
  (void)path;
  __rock_make_string(out, "", 0);
}
#else
void __read_file_impl(string *out, string filename) {
  FILE *f = fopen(string_to_cstr(filename), "r");
  if (f == NULL) {
    printf("Unable to open file \"%s\" for reading: ",
           string_to_cstr(filename));
    perror("");
    exit_rocker(1);
  }
  fseek(f, 0, SEEK_END);
  size_t length = ftell(f);
  fseek(f, 0, SEEK_SET);
  __rock_make_longlived_string(out, length);
  fread(out->data, 1, length, f);
  out->data[length] = 0;
  fclose(f);
}

void write_string_to_file(string s, string filename) {
  /* ADR-0003 §13: length-aware write. Substring views are not
   * null-terminated, so fprintf("%s", s.data) would over-read into the
   * source's bytes. fwrite uses the descriptor's length explicitly. */
  char *fname = string_to_cstr(filename);
  FILE *f = fopen(fname, "wb");
  if (f == NULL) {
    printf("Unable to open file \"%s\" for writing: ",
           string_to_cstr(filename));
    perror("");
    exit_rocker(1);
  }
  if (s.data != NULL && s.length > 0) {
    fwrite(s.data, 1, s.length, f);
  }
  fclose(f);
}

void __get_abs_path_impl(string *out, string path) {
  char *abs_path_tmp = realpath(path.data, NULL);
  if (abs_path_tmp == NULL) {
    printf("Could not get absolute path of file \'%s\'\n", path.data);
    exit(1);
  }
  size_t abs_len = strlen(abs_path_tmp);
  __rock_make_longlived_string(out, abs_len);
  memcpy(out->data, abs_path_tmp, abs_len + 1);
  free(abs_path_tmp);
}
#endif

/* Retain/release on string backing. NULL backing denotes a borrowed view;
 * static literal backing is immortal; all produced mutable strings live in
 * the long-lived pool. */

void __string_retain(string s) {
  if (s.backing == NULL) return;
  if (s.backing->refcount == ROCK_RC_STATIC) return;
  if (s.backing->refcount == ROCK_RC_FREE || s.backing->refcount == ROCK_RC_MAGAZINE) {
    fprintf(stderr, "rock: __string_retain on freed block\n");
    exit(1);
  }
  if (s.backing->refcount >= ROCK_RC_FREE - 1) {
    fprintf(stderr, "rock: __string_retain refcount overflow\n");
    exit(1);
  }
  s.backing->refcount++;
}

void __string_release(string s) {
  if (s.backing == NULL) return;
  if (s.backing->refcount == ROCK_RC_STATIC) return;
  if (s.backing->refcount == ROCK_RC_FREE || s.backing->refcount == ROCK_RC_MAGAZINE) {
    fprintf(stderr, "rock: __string_release on already-freed block\n");
    exit(1);
  }
  if (--s.backing->refcount == 0) {
    /* Payload pointer is just past the header. */
    void *payload = (void *)((char *)s.backing + sizeof(rock_block_header));
    rock_longlived_free(payload);
  }
}

/* ADR-0003 §7.6: return materialisation. See header for case breakdown.
 * Always returns a descriptor with caller-owned ownership semantics —
 * caller transfers (no inc, no dec) into the destination slot. */
string __return_string(string s) {
  if (s.backing == NULL) {
    /* Bump source (or NULL data). Allocate a fresh longlived block and
     * copy the bytes. The new descriptor is owned (rc=1 from
     * rock_longlived_alloc). */
    string out;
    __rock_make_longlived_string(&out, s.length);
    if (s.length > 0 && s.data != NULL) {
      memcpy(out.data, s.data, s.length);
    }
    out.data[s.length] = 0;
    return out;
  }
  if (s.backing->refcount == ROCK_RC_STATIC) {
    /* Static source — eternal lifetime, no inc needed. The caller's
     * transfer is sound because static blocks can't be released. */
    return s;
  }
  /* Longlived source — inc refcount so the caller has an owned reference
   * independent of any other live alias. */
  __string_retain(s);
  return s;
}

void *__handle_retain(void *payload) {
  if (!payload) return payload;
  rock_block_header *h = ((rock_block_header *)payload) - 1;
  if (h->refcount == ROCK_RC_STATIC) return payload;
  if (h->refcount == ROCK_RC_FREE || h->refcount == ROCK_RC_MAGAZINE) {
    fprintf(stderr, "rock: __handle_retain on already-freed block at %p\n", payload);
    exit(1);
  }
  if (h->refcount >= ROCK_RC_FREE - 1) {
    fprintf(stderr, "rock: __handle_retain refcount overflow\n");
    exit(1);
  }
  h->refcount++;
  return payload;
}

void __handle_release(void *payload) {
  if (!payload) return;
  rock_block_header *h = ((rock_block_header *)payload) - 1;
  if (h->refcount == ROCK_RC_STATIC) return;
  if (h->refcount == ROCK_RC_FREE || h->refcount == ROCK_RC_MAGAZINE) {
    fprintf(stderr, "rock: __handle_release on already-freed block at %p\n", payload);
    exit(1);
  }
  if (--h->refcount == 0) {
    rock_longlived_free(payload);
  }
}

// ============================================================================
// STRING SLICING
// ============================================================================

int __rock_substr_index_base = 1;

void set_string_index_base(int base) { __rock_substr_index_base = base; }
int  get_string_index_base(void)     { return __rock_substr_index_base; }

static int __normalize_substr_idx(int idx, int length) {
  if (idx < 0) return length + idx;
  return idx - __rock_substr_index_base;
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
    printf("substring: start index out of bounds (start=%d, length=%d)\n",
           start, (int)s.length);
    exit_rocker(1);
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
    printf("substring: start index out of bounds (start=%d, length=%d)\n",
           start, (int)s.length);
    exit_rocker(1);
  }
  if (c_end < c_start || c_end >= (int)s.length) {
    printf("substring: end index out of bounds (end=%d, length=%d)\n",
           end, (int)s.length);
    exit_rocker(1);
  }
  int len = c_end - c_start + 1;
  out->data     = s.data + c_start;
  out->length   = (size_t)len;
  out->capacity = 0;
  out->backing  = s.backing;
  __string_retain(*out);
}

// ============================================================================
// TYPE CONVERSIONS
// ============================================================================

int __to_int_byte(byte b) {
  return (int)b;
}

byte __to_byte_int(int n) {
  return (byte)n;
}

void __to_string_byte(string *out, byte b) {
  char buf[4];  // max "255\0"
  int len = snprintf(buf, sizeof(buf), "%u", (unsigned int)b);
  __rock_make_longlived_string(out, (size_t)len);
  memcpy(out->data, buf, len + 1);
}

void __to_string_int(string *out, int n) {
  char buf[24];
  int len = snprintf(buf, sizeof(buf), "%d", n);
  __rock_make_longlived_string(out, (size_t)len);
  memcpy(out->data, buf, len + 1);
}

int  __to_int_word(word w)  { return (int)w; }
word __to_word_int(int n)   { return (word)n; }

void __to_string_word(string *out, word w) {
  char buf[6];  // max "65535\0"
  int len = snprintf(buf, sizeof(buf), "%u", (unsigned int)w);
  __rock_make_longlived_string(out, (size_t)len);
  memcpy(out->data, buf, len + 1);
}

int   __to_int_dword(dword d)  { return (int)d; }
dword __to_dword_int(int n)    { return (dword)n; }
int   __to_int_float(float f)  { return (int)f; }

void __to_string_dword(string *out, dword d) {
  char buf[11];  // max "4294967295\0"
  int len = snprintf(buf, sizeof(buf), "%lu", (unsigned long)d);
  __rock_make_longlived_string(out, (size_t)len);
  memcpy(out->data, buf, len + 1);
}

void __to_string_float(string *out, float f) {
  char buf[24];
  int len = snprintf(buf, sizeof(buf), "%g", (double)f);
  __rock_make_longlived_string(out, (size_t)len);
  memcpy(out->data, buf, len + 1);
}

// ============================================================================
// ALWAYS-AVAILABLE CASTING FUNCTIONS (safe for sccz80)
// ============================================================================

byte __to_byte_word(word w) { return (byte)w; }
word __to_word_byte(byte b) { return (word)b; }
