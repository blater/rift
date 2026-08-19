#include "component_manifest.h"
#include "driver/sidecar.h"
#include "lib/alloc.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void check(int condition, const char *message) {
  if (condition)
    printf("PASS: %s\n", message);
  else {
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
  }
}

int main(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr, "usage: driver_sidecar_test MANIFEST VALID BAD DUPLICATE\n");
    return 2;
  }
  init_compiler_stack();
  component_manifest *manifest = load_component_manifest(argv[1]);
  driver_requirements requirements;
  check(driver_read_requirements(argv[2], manifest, &requirements),
        "valid authoritative sidecar is accepted");
  check(requirements.profile == DRIVER_PROFILE_TINY,
        "tiny compiler profile is preserved");
  check(!requirements.pools_required,
        "pool requirement is preserved independently of startup profile");
  check(requirements.component_count == 1 &&
            strcmp(requirements.components[0]->id, "sprite") == 0,
        "selected component order is preserved");
  check(!driver_read_requirements(argv[3], manifest, &requirements),
        "unknown compiler profile is rejected");
  check(!driver_read_requirements(argv[4], manifest, &requirements),
        "duplicate component is rejected");
  kill_compiler_stack();
  return failures ? 1 : 0;
}
