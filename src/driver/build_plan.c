#include "build_plan.h"
#include "paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *copy_string(const char *value) {
  size_t size = strlen(value) + 1;
  char *result = malloc(size);
  if (result) memcpy(result, value, size);
  return result;
}

int driver_string_list_push(driver_string_list *list, const char *value) {
  for (int i = 0; i < list->count; i++)
    if (strcmp(list->items[i], value) == 0) return 1;
  if (list->count == list->capacity) {
    int capacity = list->capacity ? list->capacity * 2 : 16;
    char **items = realloc(list->items, sizeof(char *) * (size_t)capacity);
    if (!items) return 0;
    list->items = items;
    list->capacity = capacity;
  }
  list->items[list->count] = copy_string(value);
  if (!list->items[list->count]) return 0;
  list->count++;
  return 1;
}

static int append_component_paths(driver_string_list *list,
                                  const char *runtime_root,
                                  const char *paths) {
  int count = component_parameter_count(paths);
  for (int i = 0; i < count; i++) {
    char relative[512];
    if (!component_parameter_at(paths, i, relative, sizeof(relative))) {
      fprintf(stderr, "build failed: invalid component source list\n");
      return 0;
    }
    char *absolute = driver_path_join(runtime_root, relative);
    if (!absolute || !driver_string_list_push(list, absolute)) {
      free(absolute);
      return 0;
    }
    free(absolute);
  }
  return 1;
}

int driver_make_build_plan(const char *root, const driver_options *options,
                           const driver_requirements *requirements,
                           driver_build_plan *plan) {
  memset(plan, 0, sizeof(*plan));
  plan->startup = 1;
  if (options->target == DRIVER_TARGET_ZXN &&
      options->rtl_mode == DRIVER_RTL_AUTO) {
    switch (requirements->profile) {
    case DRIVER_PROFILE_TINY:
      plan->startup = 31;
      plan->tiny_core = 1;
      plan->tiny_print_direct = 1;
      break;
    case DRIVER_PROFILE_TINY_CONSOLE:
      plan->startup = 31;
      plan->tiny_core = 1;
      plan->tiny_print_direct = 1;
      plan->tiny_print_controls = 1;
      break;
    case DRIVER_PROFILE_CORE:
      plan->startup = 31;
      plan->light_core = 1;
      plan->tiny_print_direct = 1;
      plan->tiny_print_controls = 1;
      break;
    case DRIVER_PROFILE_FULL:
      break;
    }
  }
  char *runtime_root = driver_path_join(root, "src/lib");
  if (!runtime_root) return 0;
  for (int i = 0; i < requirements->component_count; i++) {
    component_spec *component = requirements->components[i];
    const char *c_sources = options->target == DRIVER_TARGET_HOST
                                ? component->host_sources
                                : component->zxn_sources;
    const char *asm_sources = options->target == DRIVER_TARGET_HOST
                                  ? component->host_asm
                                  : component->zxn_asm;
    if (!append_component_paths(&plan->c_sources, runtime_root, c_sources) ||
        !append_component_paths(&plan->asm_sources, runtime_root, asm_sources)) {
      free(runtime_root);
      driver_free_build_plan(plan);
      return 0;
    }
  }
  free(runtime_root);
  return 1;
}

void driver_free_build_plan(driver_build_plan *plan) {
  for (int i = 0; i < plan->c_sources.count; i++)
    free(plan->c_sources.items[i]);
  for (int i = 0; i < plan->asm_sources.count; i++)
    free(plan->asm_sources.items[i]);
  free(plan->c_sources.items);
  free(plan->asm_sources.items);
  memset(plan, 0, sizeof(*plan));
}
