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
          "  --memory-max=BYTES  cap the automatic managed arena\n"
          "  --memory-min=BYTES  require at least this much managed memory\n"
          "  --memory-reserve=BYTES  leave high memory outside the arena\n"
          "  --rtl=auto|all\n"
          "\n"
          "  --debug\n"
          "  --help\n",
          program);
}

static int parse_decimal(const char *option, const char *text,
                         size_t *result) {
  if (!text[0]) {
    fprintf(stderr, "%s requires a decimal byte count\n", option);
    return 0;
  }
  if (text[0] == '0' && text[1]) {
    fprintf(stderr, "memory sizes must be canonical decimal without leading zeros\n");
    return 0;
  }
  for (const char *cursor = text; *cursor; cursor++) {
    if (*cursor < '0' || *cursor > '9') {
      fprintf(stderr, "%s requires a decimal byte count\n", option);
      return 0;
    }
  }
  errno = 0;
  char *end = NULL;
  unsigned long long value = strtoull(text, &end, 10);
  if (errno || !end || *end || value > (unsigned long long)SIZE_MAX) {
    fprintf(stderr, "%s exceeds this host's addressable size\n", option);
    return 0;
  }
  *result = (size_t)value;
  return 1;
}

static int validate_options(const driver_options *options) {
  if (options->zxn_test && options->target != DRIVER_TARGET_ZXN) {
    fprintf(stderr, "--zxn-test requires --target=zxn\n");
    return 0;
  }
  if (options->target == DRIVER_TARGET_HOST && options->memory_reserve_set) {
    fprintf(stderr, "--memory-reserve is only meaningful for --target=zxn\n");
    return 0;
  }
  if (options->memory_max_set && options->memory_max == 0) {
    fprintf(stderr, "--memory-max must be greater than zero\n");
    return 0;
  }
  if (options->target == DRIVER_TARGET_ZXN &&
      ((options->memory_max_set && options->memory_max > 65534u) ||
       (options->memory_min_set && options->memory_min > 65534u) ||
       (options->memory_reserve_set && options->memory_reserve > 65534u))) {
    fprintf(stderr,
            "ZXN memory sizes must fit the target's 16-bit address space "
            "(0..65534)\n");
    return 0;
  }
  if (options->memory_max_set &&
      options->memory_min > options->memory_max) {
    fprintf(stderr, "--memory-min cannot exceed --memory-max\n");
    return 0;
  }
  return 1;
}

int driver_parse_options(int argc, char **argv, driver_options *options) {
  *options = (driver_options){
      .target = DRIVER_TARGET_ZXN,
      .rtl_mode = DRIVER_RTL_AUTO,
  };
  int positional_only = 0;
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
    if (!positional_only && strncmp(arg, "--memory-max=", 13) == 0) {
      if (!parse_decimal("--memory-max", arg + 13, &options->memory_max))
        return 0;
      options->memory_max_set = 1;
      continue;
    }
    if (!positional_only && strncmp(arg, "--memory-min=", 13) == 0) {
      if (!parse_decimal("--memory-min", arg + 13, &options->memory_min))
        return 0;
      options->memory_min_set = 1;
      continue;
    }
    if (!positional_only && strncmp(arg, "--memory-reserve=", 17) == 0) {
      if (!parse_decimal("--memory-reserve", arg + 17,
                         &options->memory_reserve))
        return 0;
      options->memory_reserve_set = 1;
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
  return validate_options(options);
}
