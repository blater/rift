#ifndef RIFT_DRIVER_SIDECAR_H
#define RIFT_DRIVER_SIDECAR_H

#include "component_manifest.h"

typedef enum {
  DRIVER_PROFILE_FULL,
  DRIVER_PROFILE_CORE,
  DRIVER_PROFILE_TINY,
  DRIVER_PROFILE_TINY_CONSOLE
} driver_profile;

typedef struct {
  driver_profile profile;
  int pools_required;
  int bump_required;
  component_spec *components[COMPONENT_MANIFEST_MAX_COMPONENTS];
  int component_count;
} driver_requirements;

int driver_read_requirements(const char *path, component_manifest *manifest,
                             driver_requirements *requirements);

#endif
