#ifndef RIFT_DRIVER_PATHS_H
#define RIFT_DRIVER_PATHS_H

#include "options.h"
#include <stddef.h>

typedef struct {
  char *root;
  char *source;
  char *output;
  char *output_dir;
  char *output_stem;
  char *workspace;
  char *work_base;
} driver_paths;

int driver_resolve_paths(const char *argv0, const driver_options *options,
                         driver_paths *paths);
void driver_free_paths(driver_paths *paths);
char *driver_path_join(const char *left, const char *right);
char *driver_path_with_suffix(const char *base, const char *suffix);
int driver_remove_tree(const char *path);
int driver_publish_file(const char *source, const char *destination,
                        int executable);

#endif
