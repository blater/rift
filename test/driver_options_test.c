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
            options.target == DRIVER_TARGET_ZXN &&
            options.memory_mode == DRIVER_MEMORY_STANDARD &&
            options.zxn_bump_pool == 1024 &&
            options.zxn_longlived_pool == 6144,
        "unqualified builds use standard ZX Next memory");

  char *gcc_argv[] = {"rift", "--target=gcc", "program.rift"};
  check(driver_parse_options(3, gcc_argv, &options) &&
            options.target == DRIVER_TARGET_HOST,
        "--target=gcc explicitly selects the host target");

  char *short_gcc_argv[] = {"rift", "-t", "gcc", "program.rift"};
  check(driver_parse_options(4, short_gcc_argv, &options) &&
            options.target == DRIVER_TARGET_HOST,
        "-t gcc explicitly selects the host target");

  char *compact_argv[] = {"rift", "--memory=compact", "program.rift"};
  check(driver_parse_options(3, compact_argv, &options) &&
            options.memory_mode == DRIVER_MEMORY_COMPACT &&
            options.zxn_bump_pool == 256 &&
            options.zxn_longlived_pool == 1024 &&
            !options.zxn_pool_overrides &&
            !options.zxn_bump_pool_override,
        "compact memory selects example-sized capacities");

  char *standard_argv[] = {"rift", "--memory=standard", "program.rift"};
  check(driver_parse_options(3, standard_argv, &options) &&
            options.memory_mode == DRIVER_MEMORY_STANDARD &&
            options.zxn_bump_pool == 1024 &&
            options.zxn_longlived_pool == 6144,
        "standard memory explicitly selects the default capacities");

  char *custom_after_argv[] = {"rift", "--memory=compact",
                               "--zxn-longlived-pool=2048", "program.rift"};
  check(driver_parse_options(4, custom_after_argv, &options) &&
            options.zxn_bump_pool == 256 &&
            options.zxn_longlived_pool == 2048 &&
            options.zxn_pool_overrides &&
            !options.zxn_bump_pool_override,
        "expert capacity overrides the corresponding compact value");

  char *custom_before_argv[] = {"rift", "--zxn-bump-pool=512",
                                "--memory=compact", "program.rift"};
  check(driver_parse_options(4, custom_before_argv, &options) &&
            options.zxn_bump_pool == 512 &&
            options.zxn_longlived_pool == 1024 &&
            options.zxn_pool_overrides &&
            options.zxn_bump_pool_override,
        "expert capacity precedence is independent of argument order");

  char *invalid_argv[] = {"rift", "--memory=small", "program.rift"};
  check(!driver_parse_options(3, invalid_argv, &options),
        "unknown memory preset is rejected");

  char *missing_argv[] = {"rift", "--memory", "program.rift"};
  check(!driver_parse_options(3, missing_argv, &options),
        "missing memory preset is rejected");

  char *host_memory_argv[] = {"rift", "--target=gcc", "--memory=compact",
                              "program.rift"};
  check(!driver_parse_options(4, host_memory_argv, &options),
        "ZX Next memory preset is rejected for host builds");

  return failures ? 1 : 0;
}
