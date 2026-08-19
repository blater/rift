#include "input.h"
#include "console.h"
#include "fundefs.h"

string input(void) {
  char buffer[RIFT_INPUT_MAX_LENGTH + 1];
  size_t length = 0;
  string result;
  byte value;
  size_t i;

  for (;;) {
    value = keypress();
    if (value == 10 || value == 13) {
      rift_console_newline();
      break;
    }
    if (value == 8) {
      if (length != 0) {
        length--;
        rift_console_putc('\b');
        rift_console_putc(' ');
        rift_console_putc('\b');
      }
      continue;
    }
    if (value >= 32 && value <= 126 && length < RIFT_INPUT_MAX_LENGTH) {
      buffer[length++] = (char)value;
      rift_console_putc((char)value);
    }
  }

  __rift_make_longlived_string(&result, length);
  for (i = 0; i < length; i++) result.data[i] = buffer[i];
  result.data[length] = 0;
  return result;
}
