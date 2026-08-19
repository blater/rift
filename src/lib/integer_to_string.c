#include "fundefs.h"
#include "number_format.h"

static void string_from_buffer(string *out, const char *buffer,
                               size_t length) {
  size_t i;
  __rift_make_longlived_string(out, length);
  for (i = 0; i < length; i++) out->data[i] = buffer[i];
  out->data[length] = 0;
}

void __to_string_byte(string *out, byte value) {
  char buffer[4];
  string_from_buffer(out, buffer, rift_format_byte(buffer, value));
}

void __to_string_word(string *out, word value) {
  char buffer[6];
  string_from_buffer(out, buffer, rift_format_word(buffer, value));
}

void __to_string_int(string *out, int value) {
  char buffer[12];
  string_from_buffer(out, buffer, rift_format_int(buffer, value));
}

void __to_string_boolean(string *out, boolean value) {
  char buffer[6];
  string_from_buffer(out, buffer, rift_format_boolean(buffer, value));
}

void __to_string_char(string *out, char value) {
  string_from_buffer(out, &value, 1);
}

void __to_string_string(string *out, string value) {
  size_t length = value.data ? value.length : 0;
  string_from_buffer(out, value.data ? value.data : "", length);
}
