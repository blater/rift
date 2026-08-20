#include "number_parse.h"

#include "error_sink.h"
#include <stdlib.h>

#define RIFT_FLOAT_MAX 3.4028234e38f

static void invalid_number(void) {
  rift_error_text("Invalid numeric input\n");
  exit(1);
}

static int is_digit(char value) { return value >= '0' && value <= '9'; }

float rift_parse_float_checked(string text) {
  size_t index = 0;
  int negative = 0;
  int saw_digit = 0;
  int saw_nonzero = 0;
  int after_decimal = 0;
  int decimal_exponent = 0;
  int explicit_exponent = 0;
  int exponent_negative = 0;
  byte significant_digits = 0;
  dword significand = 0;
  float result;

  while (index < text.length && text.data[index] == ' ')
    index++;
  if (index < text.length &&
      (text.data[index] == '+' || text.data[index] == '-')) {
    negative = text.data[index] == '-';
    index++;
  }

  while (index < text.length) {
    char value = text.data[index];
    if (is_digit(value)) {
      int digit = value - '0';
      saw_digit = 1;
      if (!saw_nonzero && digit == 0) {
        if (after_decimal)
          decimal_exponent--;
      } else {
        saw_nonzero = 1;
        if (significant_digits < 9) {
          significand = significand * 10u + (dword)digit;
          significant_digits++;
          if (after_decimal)
            decimal_exponent--;
        } else if (!after_decimal) {
          decimal_exponent++;
        }
      }
      index++;
      continue;
    }
    if (value == '.' && !after_decimal) {
      after_decimal = 1;
      index++;
      continue;
    }
    break;
  }

  if (!saw_digit)
    invalid_number();

  if (index < text.length &&
      (text.data[index] == 'e' || text.data[index] == 'E')) {
    int saw_exponent_digit = 0;
    index++;
    if (index < text.length &&
        (text.data[index] == '+' || text.data[index] == '-')) {
      exponent_negative = text.data[index] == '-';
      index++;
    }
    while (index < text.length && is_digit(text.data[index])) {
      int digit = text.data[index] - '0';
      saw_exponent_digit = 1;
      if (explicit_exponent > 100 || (explicit_exponent == 100 && digit != 0))
        explicit_exponent = 1000;
      else
        explicit_exponent = explicit_exponent * 10 + digit;
      index++;
    }
    if (!saw_exponent_digit)
      invalid_number();
  }

  while (index < text.length && text.data[index] == ' ')
    index++;
  if (index != text.length)
    invalid_number();
  if (!saw_nonzero)
    return negative ? -0.0f : 0.0f;

  if (exponent_negative)
    decimal_exponent -= explicit_exponent;
  else
    decimal_exponent += explicit_exponent;

  result = (float)significand;
  while (decimal_exponent > 0) {
    if (result > RIFT_FLOAT_MAX / 10.0f)
      invalid_number();
    result *= 10.0f;
    decimal_exponent--;
  }
  while (decimal_exponent < 0) {
    float smaller = result / 10.0f;
    if (smaller == 0.0f)
      invalid_number();
    result = smaller;
    decimal_exponent++;
  }

  return negative ? 0.0f - result : result;
}
