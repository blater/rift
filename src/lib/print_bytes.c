#include "fundefs.h"

void rock_print_bytes(const char *data, size_t length) {
  for (size_t i = 0; i < length; i++)
    putchar((unsigned char)data[i]);
  fflush(stdout);
}
