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

typedef struct {
  driver_target target;
  driver_rtl_mode rtl_mode;
  int auto_cast;
  int zxn_test;
  int allocator_stats;
  int debug;
  int run;
  int memory_max_set;
  int memory_min_set;
  int memory_reserve_set;
  size_t memory_max;
  size_t memory_min;
  size_t memory_reserve;
  const char *source_arg;
  const char *output_arg;
} driver_options;

void driver_print_usage(const char *program);
int driver_parse_options(int argc, char **argv, driver_options *options);

#endif
