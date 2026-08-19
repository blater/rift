#include "fundefs_internal.h"

static int global_argc;
static char **global_argv;

void fill_cmd_args(int argc, char **argv) {
  global_argc = argc;
  global_argv = argv;
}

__internal_dynamic_array_t get_args(void) {
  __internal_dynamic_array_t cmd_args = string_make_array();
  for (int i = 1; i < global_argc; i++) {
    string arg;
    cstr_to_string(&arg, global_argv[i]);
    string_push_array(cmd_args, arg);
  }
  return cmd_args;
}
