#ifndef RIFT_FUNDEFS
#define RIFT_FUNDEFS

#include "typedefs.h"
#include "asm_interop.h"
#include "console.h"
#ifndef __SDCC
#include <stdio.h>
#endif

void print(string s);
char *string_to_cstr(string s);
void cstr_to_string(string *out, char *cstr);
char charAt(string s, int n);
void __concat_char(string *out, string s, char c);
void __concat_str(string *out, string s1, string s2);
size_t __concat_checked_add(size_t total, size_t addition);
size_t __concat_append_bytes(string out, size_t offset,
                             const char *data, size_t length);
void setCharAt(string s, int n, char c);
void __read_file_impl(string *out, string filename);
void new_string(string *out, string s);
void write_string_to_file(string s, string filename);
int equals(string s1, string s2);
void __get_abs_path_impl(string *out, string path);

/* ADR-0003 §7.6 — refcount on long-lived string backing.
 *
 * Three lifetime classes per the descriptor's `backing` field:
 *   - backing == NULL                  bump or pre-Phase-H string; no-op
 *   - backing->refcount == 0xFFFF      static (literal); no-op
 *   - backing->refcount  < 0xFFFF      longlived; inc/dec
 *
 * On release dropping refcount to zero, the block returns to its
 * address-ordered free list via rift_longlived_free.
 *
 * These helpers are call-site-safe at every site listed in §7.6:
 *   variable initialiser, slot write, scope exit, parameter entry/exit,
 *   return capture. Currently no generator path emits them — Phase E.b
 *   wires the call sites; until then they are unreferenced exports
 *   exercised only by C-level unit tests. */
void __string_retain(string s);
void __string_release(string s);

/* ADR-0003 §7.6 — return materialisation.
 *
 * Every non-scalar function return must materialise as an owned producer
 * in `longlived` so the caller's transfer-into-destination is sound under
 * a single LIFO bump pointer. Three runtime cases:
 *   - static source        return unchanged (eternal; no inc needed)
 *   - longlived source     inc refcount; return same descriptor
 *   - bump source (NULL)   allocate fresh longlived block; copy bytes;
 *                          return new descriptor
 *
 * Caller side: the returned descriptor is treated as a producer
 * (rc-1 owned). The caller's destination capture is a transfer
 * (no inc, no dec). */
string __return_string(string s);

/* Refcount on aggregate handles (records, unions, modules). Universal
 * block header sits at `(rift_block_header *)payload - 1`. NULL payloads
 * are no-ops; static handles (RIFT_RC_STATIC) pass through. */
void *__handle_retain(void *payload);
void  __handle_release(void *payload);

/* Return materialisation for aggregate handles. Behaviourally identical
 * to __handle_retain — kept as a distinct symbol so ADR-0003 §16.3
 * structural greps can pick out return sites from generic retains. */
#if defined(__SDCC) && defined(RIFT_ZXN_TINY_CORE)
#define __return_handle(payload) (payload)
#else
static inline void *__return_handle(void *payload) {
  return __handle_retain(payload);
}
#endif

// Configurable string index base (0 = zero-indexed, 1 = one-indexed)
extern int __rift_substr_index_base;
void set_string_index_base(int base);
int  get_string_index_base(void);
void __substring_from(string *out, string s, int start);
void __substring_range(string *out, string s, int start, int end);

// Helper for string construction (replaces C99 compound literals)
void __rift_make_string(string *out, const char *data, size_t length);

/* ADR-0003 §7.1: allocate writable backing in the longlived pool and
 * populate the descriptor's `backing` field. Caller writes `length+1`
 * bytes into out->data (including the null terminator). */
void __rift_make_longlived_string(string *out, size_t length);

// byte/word/dword/float casting
int    __to_int_byte(byte b);
byte   __to_byte_int(int n);
void __to_string_byte(string *out, byte b);
int    __to_int_word(word w);
word   __to_word_int(int n);
void __to_string_word(string *out, word w);
int    __to_int_dword(dword d);
dword  __to_dword_int(int n);
void __to_string_dword(string *out, dword d);
void __to_string_boolean(string *out, boolean value);
void __to_string_char(string *out, char value);
void __to_string_string(string *out, string value);
void __to_string_int(string *out, int n);
int    __to_int_float(float f);
void __to_string_float(string *out, float f);

// Host-only wrappers for file I/O (returns by value)
#ifndef __SDCC

static inline string read_file(string filename) {
  string res;
  __read_file_impl(&res, filename);
  return res;
}

static inline string get_abs_path(string path) {
  string res;
  __get_abs_path_impl(&res, path);
  return res;
}

#else

// Stubs for ZXN (file I/O not available on ZXN)
static inline string read_file(string filename) {
  (void)filename;
  string res = {NULL, 0, 0};
  return res;
}

static inline string get_abs_path(string path) {
  (void)path;
  string res = {NULL, 0, 0};
  return res;
}

#endif

#ifndef __SDCC
#define to_int(x)    _Generic((x), byte: __to_int_byte, word: __to_int_word, dword: __to_int_dword, float: __to_int_float, default: (int)(x))(x)
#define to_byte(x)   _Generic((x), int:  __to_byte_int, default: (byte)(x))(x)
#define to_word(x)   _Generic((x), int:  __to_word_int, default: (word)(x))(x)
#define to_dword(x)  _Generic((x), int:  __to_dword_int, default: (dword)(x))(x)
#else
#define to_int(x)    ((int)(x))
#define to_byte(x)   ((byte)(x))
#define to_word(x)   ((word)(x))
#define to_dword(x)  ((dword)(x))
#endif
#define to_float(x)  ((float)(x))

#endif // RIFT_FUNDEFS
