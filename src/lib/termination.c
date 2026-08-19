#include "fundefs_internal.h"

#include <stdlib.h>

void init_rift(int argc, char **argv) {
  (void)argc;
  (void)argv;
}

void end_rift(void) {
}

void exit_rift(int status) {
  end_rift();
  exit(status);
}

void halt(byte code) {
  exit_rift((int)code);
}
