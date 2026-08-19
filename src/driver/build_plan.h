#ifndef RIFT_DRIVER_BUILD_PLAN_H
#define RIFT_DRIVER_BUILD_PLAN_H

#include "options.h"
#include "sidecar.h"

typedef struct {
  char **items;
  int count;
  int capacity;
} driver_string_list;

typedef struct {
  driver_string_list c_sources;
  driver_string_list asm_sources;
  int startup;
  int tiny_core;
  int light_core;
  int tiny_print_direct;
  int tiny_print_controls;
  int pools_required;
} driver_build_plan;

int driver_make_build_plan(const char *root, const driver_options *options,
                           const driver_requirements *requirements,
                           driver_build_plan *plan);
void driver_free_build_plan(driver_build_plan *plan);
int driver_string_list_push(driver_string_list *list, const char *value);

#endif
