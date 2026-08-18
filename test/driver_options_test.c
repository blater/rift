#include "driver/options.h"
#include <stdio.h>

static int failures;

static void check(int condition, const char *message) {
  if (condition)
    printf("PASS: %s\n", message);
  else {
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
  }
}

int main(void) {
  driver_options options;
  char *default_argv[] = {"rift", "program.rift"};
  check(driver_parse_options(2, default_argv, &options) &&
            options.target == DRIVER_TARGET_ZXN,
        "unqualified builds default to ZX Next");

  char *gcc_argv[] = {"rift", "--target=gcc", "program.rift"};
  check(driver_parse_options(3, gcc_argv, &options) &&
            options.target == DRIVER_TARGET_HOST,
        "--target=gcc explicitly selects the host target");

  char *short_gcc_argv[] = {"rift", "-t", "gcc", "program.rift"};
  check(driver_parse_options(4, short_gcc_argv, &options) &&
            options.target == DRIVER_TARGET_HOST,
        "-t gcc explicitly selects the host target");

  return failures ? 1 : 0;
}
