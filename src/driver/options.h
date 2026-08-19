#ifndef RIFT_DRIVER_OPTIONS_H
#define RIFT_DRIVER_OPTIONS_H

#include <stddef.h>

typedef enum {
  DRIVER_TARGET_HOST,
  DRIVER_TARGET_ZXN
} driver_target;

typedef enum {
  DRIVER_RTL_AUTO,
  DRIVER_RTL_ALL
} driver_rtl_mode;

typedef enum {
  DRIVER_MEMORY_STANDARD,
  DRIVER_MEMORY_COMPACT
} driver_memory_mode;

typedef struct {
  driver_target target;
  driver_rtl_mode rtl_mode;
  driver_memory_mode memory_mode;
  int auto_cast;
  int zxn_test;
  int allocator_stats;
  int memory_profile_zxn;
  int debug;
  int run;
  int zxn_pool_overrides;
  int zxn_bump_pool_override;
  unsigned zxn_bump_pool;
  unsigned zxn_longlived_pool;
  const char *source_arg;
  const char *output_arg;
} driver_options;

void driver_print_usage(const char *program);
int driver_parse_options(int argc, char **argv, driver_options *options);

#endif
