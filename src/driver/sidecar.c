#include "sidecar.h"
#include <stdio.h>
#include <string.h>

static int set_profile(const char *value, driver_profile *profile) {
  if (strcmp(value, "full") == 0)
    *profile = DRIVER_PROFILE_FULL;
  else if (strcmp(value, "core-31") == 0)
    *profile = DRIVER_PROFILE_CORE;
  else if (strcmp(value, "tiny-31") == 0)
    *profile = DRIVER_PROFILE_TINY;
  else if (strcmp(value, "tiny-console-31") == 0)
    *profile = DRIVER_PROFILE_TINY_CONSOLE;
  else
    return 0;
  return 1;
}

int driver_read_requirements(const char *path, component_manifest *manifest,
                             driver_requirements *requirements) {
  memset(requirements, 0, sizeof(*requirements));
  /* V1 sidecars predate the explicit pool and bump directives. Treat missing
   * requirements conservatively so older compiler output can never cause the
   * driver to omit allocator storage that the program may need. */
  requirements->pools_required = 1;
  requirements->bump_required = 1;
  FILE *file = fopen(path, "r");
  if (!file) {
    fprintf(stderr,
            "build failed: missing or invalid compiler component output %s\n",
            path);
    return 0;
  }
  char line[4096];
  int line_number = 0;
  int profile_seen = 0;
  int pools_seen = 0;
  int bump_seen = 0;
  while (fgets(line, sizeof(line), file)) {
    line_number++;
    size_t length = strlen(line);
    while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
      line[--length] = '\0';
    if (line_number == 1) {
      if (strcmp(line, "RIFT_COMPONENTS_V1") != 0) {
        fprintf(stderr, "build failed: unsupported component sidecar '%s'\n",
                line);
        fclose(file);
        return 0;
      }
      continue;
    }
    if (!line[0]) continue;
    if (strncmp(line, "@profile=", 9) == 0) {
      if (profile_seen || !set_profile(line + 9, &requirements->profile)) {
        fprintf(stderr, "build failed: invalid component profile '%s'\n",
                line + 9);
        fclose(file);
        return 0;
      }
      profile_seen = 1;
      continue;
    }
    if (strncmp(line, "@pools=", 7) == 0) {
      const char *value = line + 7;
      if (pools_seen ||
          (strcmp(value, "none") != 0 && strcmp(value, "required") != 0)) {
        fprintf(stderr, "build failed: invalid pool requirement '%s'\n",
                value);
        fclose(file);
        return 0;
      }
      requirements->pools_required = strcmp(value, "required") == 0;
      pools_seen = 1;
      continue;
    }
    if (strncmp(line, "@bump=", 6) == 0) {
      const char *value = line + 6;
      if (bump_seen ||
          (strcmp(value, "none") != 0 && strcmp(value, "required") != 0)) {
        fprintf(stderr, "build failed: invalid bump requirement '%s'\n",
                value);
        fclose(file);
        return 0;
      }
      requirements->bump_required = strcmp(value, "required") == 0;
      bump_seen = 1;
      continue;
    }
    if (line[0] == '@') {
      fprintf(stderr, "build failed: unknown component directive '%s'\n", line);
      fclose(file);
      return 0;
    }
    component_spec *component = find_component(manifest, line);
    if (!component) {
      fprintf(stderr, "unknown selected component '%s'\n", line);
      fclose(file);
      return 0;
    }
    for (int i = 0; i < requirements->component_count; i++) {
      if (requirements->components[i] == component) {
        fprintf(stderr, "build failed: duplicate selected component '%s'\n",
                line);
        fclose(file);
        return 0;
      }
    }
    if (requirements->component_count >= COMPONENT_MANIFEST_MAX_COMPONENTS) {
      fprintf(stderr, "build failed: too many selected components\n");
      fclose(file);
      return 0;
    }
    requirements->components[requirements->component_count++] = component;
  }
  fclose(file);
  if (line_number == 0 || !profile_seen) {
    fprintf(stderr, "build failed: component sidecar has no profile\n");
    return 0;
  }
  return 1;
}
