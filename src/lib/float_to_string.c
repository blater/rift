#include "float_format.h"
#include "fundefs.h"

void __to_string_float(string *out, float value) {
  char buffer[20];
  size_t length = rift_format_float(buffer, value);
  size_t i;
  __rift_make_longlived_string(out, length);
  for (i = 0; i < length; i++) out->data[i] = buffer[i];
  out->data[length] = 0;
}
