#include "options.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void driver_print_usage(const char *program) {
  fprintf(stderr,
          "usage: %s [run] [-t gcc|zxn] [options] SOURCE [OUTPUT]\n"
          "\n"
          "Targets:\n"
          "  -t, --target=gcc|zxn  (default: zxn)\n"
          "\n"
          "Options:\n"
          "  --auto-cast\n"
          "  --zxn-test\n"
          "  --allocator-stats\n"
          "  --memory-profile=zxn\n"
          "  --rtl=auto|all\n"
          "  --zxn-bump-pool=BYTES\n"
          "  --zxn-longlived-pool=BYTES\n"
          "  --debug\n"
          "  --help\n",
          program);
}

static int parse_decimal(const char *option, const char *text,
                         unsigned *result) {
  if (!text[0]) {
    fprintf(stderr, "%s requires a decimal byte count\n", option);
    return 0;
  }
  if (text[0] == '0' && text[1]) {
    fprintf(stderr,
            "ZXN pool capacities must be canonical decimal values without "
            "leading zeros\n");
    return 0;
  }
  for (const char *cursor = text; *cursor; cursor++) {
    if (*cursor < '0' || *cursor > '9') {
      fprintf(stderr, "%s requires a decimal byte count\n", option);
      return 0;
    }
  }
  if (strlen(text) > 5) {
    fprintf(stderr,
            "ZXN pool capacities must fit the target's 16-bit pool offsets "
            "(0..65534)\n");
    return 0;
  }
  errno = 0;
  char *end = NULL;
  unsigned long value = strtoul(text, &end, 10);
  if (errno || !end || *end || value > 65534ul) {
    fprintf(stderr,
            "ZXN pool capacities must fit the target's 16-bit pool offsets "
            "(0..65534)\n");
    return 0;
  }
  *result = (unsigned)value;
  return 1;
}

static int validate_options(const driver_options *options,
                            int pool_options_set) {
  if (options->zxn_test && options->target != DRIVER_TARGET_ZXN) {
    fprintf(stderr, "--zxn-test requires --target=zxn\n");
    return 0;
  }
  if (pool_options_set && options->target != DRIVER_TARGET_ZXN) {
    fprintf(stderr, "ZXN pool capacity options require --target=zxn\n");
    return 0;
  }
  unsigned longlived = options->zxn_longlived_pool;
  if (longlived < 16 || longlived > 6144 || longlived % 16 != 0) {
    fprintf(stderr,
            "--zxn-longlived-pool must be a multiple of 16 in the range "
            "16..6144\n");
    return 0;
  }
  unsigned units = longlived / 16;
  unsigned roots = 0;
  while (units) {
    roots += units & 1u;
    units >>= 1;
  }
  if (roots > 2) {
    fprintf(stderr,
            "--zxn-longlived-pool must decompose into at most two "
            "power-of-two buddy roots\n");
    return 0;
  }
  return 1;
}

int driver_parse_options(int argc, char **argv, driver_options *options) {
  *options = (driver_options){
      .target = DRIVER_TARGET_ZXN,
      .rtl_mode = DRIVER_RTL_AUTO,
      .zxn_bump_pool = 1024,
      .zxn_longlived_pool = 6144,
  };
  int positional_only = 0;
  int pool_options_set = 0;
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (!positional_only && strcmp(arg, "--") == 0) {
      positional_only = 1;
      continue;
    }
    if (!positional_only && strcmp(arg, "--help") == 0) {
      driver_print_usage(argv[0]);
      return 2;
    }
    if (!positional_only && strcmp(arg, "run") == 0 &&
        options->source_arg == NULL) {
      options->run = 1;
      continue;
    }
    if (!positional_only && strcmp(arg, "-t") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "-t requires gcc or zxn\n");
        return 0;
      }
      arg = argv[i];
      if (strcmp(arg, "gcc") == 0)
        options->target = DRIVER_TARGET_HOST;
      else if (strcmp(arg, "zxn") == 0)
        options->target = DRIVER_TARGET_ZXN;
      else {
        fprintf(stderr, "unknown target '%s'\n", arg);
        return 0;
      }
      continue;
    }
    if (!positional_only && strncmp(arg, "--target=", 9) == 0) {
      const char *value = arg + 9;
      if (strcmp(value, "gcc") == 0)
        options->target = DRIVER_TARGET_HOST;
      else if (strcmp(value, "zxn") == 0)
        options->target = DRIVER_TARGET_ZXN;
      else {
        fprintf(stderr, "unknown target '%s'\n", value);
        return 0;
      }
      continue;
    }
    if (!positional_only && strcmp(arg, "--auto-cast") == 0) {
      options->auto_cast = 1;
      continue;
    }
    if (!positional_only && strcmp(arg, "--zxn-test") == 0) {
      options->zxn_test = 1;
      continue;
    }
    if (!positional_only && strcmp(arg, "--allocator-stats") == 0) {
      options->allocator_stats = 1;
      continue;
    }
    if (!positional_only && strcmp(arg, "--memory-profile=zxn") == 0) {
      options->memory_profile_zxn = 1;
      continue;
    }
    if (!positional_only && strcmp(arg, "--rtl=auto") == 0) {
      options->rtl_mode = DRIVER_RTL_AUTO;
      continue;
    }
    if (!positional_only && strcmp(arg, "--rtl=all") == 0) {
      options->rtl_mode = DRIVER_RTL_ALL;
      continue;
    }
    if (!positional_only && strcmp(arg, "--debug") == 0) {
      options->debug = 1;
      continue;
    }
    if (!positional_only && strncmp(arg, "--zxn-bump-pool=", 16) == 0) {
      if (!parse_decimal("--zxn-bump-pool", arg + 16,
                         &options->zxn_bump_pool))
        return 0;
      pool_options_set = 1;
      continue;
    }
    if (!positional_only &&
        strncmp(arg, "--zxn-longlived-pool=", 21) == 0) {
      if (!parse_decimal("--zxn-longlived-pool", arg + 21,
                         &options->zxn_longlived_pool))
        return 0;
      pool_options_set = 1;
      continue;
    }
    if (!positional_only && arg[0] == '-') {
      fprintf(stderr, "unknown option '%s'\n", arg);
      return 0;
    }
    if (!options->source_arg)
      options->source_arg = arg;
    else if (!options->output_arg)
      options->output_arg = arg;
    else {
      fprintf(stderr, "unexpected argument '%s'\n", arg);
      return 0;
    }
  }
  if (!options->source_arg) {
    driver_print_usage(argv[0]);
    return 0;
  }
  return validate_options(options, pool_options_set);
}
