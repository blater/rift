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
            !options.memory_max_set && !options.memory_min_set &&
            !options.memory_reserve_set,
        "unqualified builds automatically share available ZX Next memory");

  char *gcc_argv[] = {"rift", "--target=gcc", "program.rift"};
  check(driver_parse_options(3, gcc_argv, &options) &&
            options.target == DRIVER_TARGET_HOST,
        "--target=gcc explicitly selects the host target");

  char *short_gcc_argv[] = {"rift", "-t", "gcc", "program.rift"};
  check(driver_parse_options(4, short_gcc_argv, &options) &&
            options.target == DRIVER_TARGET_HOST,
        "-t gcc explicitly selects the host target");

  char *hints_argv[] = {"rift", "--memory-max=16384",
                         "--memory-min=8192", "--memory-reserve=4096",
                         "program.rift"};
  check(driver_parse_options(5, hints_argv, &options) &&
            options.memory_max_set && options.memory_max == 16384 &&
            options.memory_min_set && options.memory_min == 8192 &&
            options.memory_reserve_set && options.memory_reserve == 4096,
        "purpose-level memory bounds are parsed without allocator details");

  char *zero_argv[] = {"rift", "--memory-min=0", "--memory-reserve=0",
                        "program.rift"};
  check(driver_parse_options(4, zero_argv, &options) &&
            options.memory_min_set && options.memory_min == 0 &&
            options.memory_reserve_set && options.memory_reserve == 0,
        "literal zero hints remain distinct from absent automatic defaults");

  char *zero_max_argv[] = {"rift", "--memory-max=0", "program.rift"};
  check(!driver_parse_options(3, zero_max_argv, &options),
        "zero maximum is rejected instead of meaning automatic");

  char *bad_relation_argv[] = {"rift", "--memory-max=4096",
                                "--memory-min=8192", "program.rift"};
  check(!driver_parse_options(4, bad_relation_argv, &options),
        "minimum memory cannot exceed the explicit maximum");

  char *removed_preset_argv[] = {"rift", "--memory=compact", "program.rift"};
  check(!driver_parse_options(3, removed_preset_argv, &options),
        "obsolete memory presets are rejected");

  char *removed_pool_argv[] = {"rift", "--zxn-longlived-pool=8192",
                                "program.rift"};
  check(!driver_parse_options(3, removed_pool_argv, &options),
        "allocator-specific pool options are rejected");

  char *removed_profile_argv[] = {"rift", "--memory-profile=zxn",
                                   "program.rift"};
  check(!driver_parse_options(3, removed_profile_argv, &options),
        "obsolete memory profiles are rejected");

  char *host_memory_argv[] = {"rift", "--target=gcc", "--memory-max=8192",
                              "--memory-min=4096", "program.rift"};
  check(driver_parse_options(5, host_memory_argv, &options) &&
            options.memory_max == 8192 && options.memory_min == 4096,
        "host builds accept total managed-memory minimum and maximum hints");

  char *host_reserve_argv[] = {"rift", "--target=gcc",
                               "--memory-reserve=4096", "program.rift"};
  check(!driver_parse_options(4, host_reserve_argv, &options),
        "host builds reject target-address-space reserve hints");

  return failures ? 1 : 0;
}
