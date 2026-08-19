#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include "paths.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static char *copy_string(const char *value) {
  size_t size = strlen(value) + 1;
  char *result = malloc(size);
  if (result) memcpy(result, value, size);
  return result;
}

char *driver_path_join(const char *left, const char *right) {
  size_t left_length = strlen(left);
  size_t right_length = strlen(right);
  int slash = left_length > 0 && left[left_length - 1] != '/';
  char *result = malloc(left_length + (size_t)slash + right_length + 1);
  if (!result) return NULL;
  memcpy(result, left, left_length);
  if (slash) result[left_length++] = '/';
  memcpy(result + left_length, right, right_length + 1);
  return result;
}

char *driver_path_with_suffix(const char *base, const char *suffix) {
  size_t base_length = strlen(base);
  size_t suffix_length = strlen(suffix);
  char *result = malloc(base_length + suffix_length + 1);
  if (!result) return NULL;
  memcpy(result, base, base_length);
  memcpy(result + base_length, suffix, suffix_length + 1);
  return result;
}

static char *path_directory(const char *path) {
  const char *slash = strrchr(path, '/');
  if (!slash) return copy_string(".");
  if (slash == path) return copy_string("/");
  size_t length = (size_t)(slash - path);
  char *result = malloc(length + 1);
  if (!result) return NULL;
  memcpy(result, path, length);
  result[length] = '\0';
  return result;
}

static const char *path_filename(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static char *strip_known_suffix(const char *filename) {
  const char *suffixes[] = {".rift", ".rft", ".exe", ".nex"};
  size_t length = strlen(filename);
  for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
    size_t suffix_length = strlen(suffixes[i]);
    if (length > suffix_length &&
        strcmp(filename + length - suffix_length, suffixes[i]) == 0) {
      length -= suffix_length;
      break;
    }
  }
  char *result = malloc(length + 1);
  if (!result) return NULL;
  memcpy(result, filename, length);
  result[length] = '\0';
  return result;
}

static char *resolve_from_cwd(const char *cwd, const char *path) {
  char *candidate = path[0] == '/' ? copy_string(path)
                                  : driver_path_join(cwd, path);
  if (!candidate) return NULL;
  char *resolved = realpath(candidate, NULL);
  free(candidate);
  return resolved;
}

static char *resolve_output(const char *cwd, const char *source,
                            const driver_options *options,
                            char **output_dir_out, char **stem_out) {
  char *raw_dir = NULL;
  char *stem = NULL;
  if (options->output_arg) {
    char *candidate = options->output_arg[0] == '/'
                          ? copy_string(options->output_arg)
                          : driver_path_join(cwd, options->output_arg);
    if (!candidate) return NULL;
    raw_dir = path_directory(candidate);
    stem = strip_known_suffix(path_filename(candidate));
    free(candidate);
  } else {
    raw_dir = path_directory(source);
    stem = strip_known_suffix(path_filename(source));
  }
  if (!raw_dir || !stem || !stem[0]) {
    free(raw_dir);
    free(stem);
    return NULL;
  }
  char *resolved_dir = realpath(raw_dir, NULL);
  free(raw_dir);
  if (!resolved_dir) {
    fprintf(stderr, "error: cannot resolve output directory: %s\n",
            strerror(errno));
    free(stem);
    return NULL;
  }
  struct stat info;
  if (stat(resolved_dir, &info) != 0 || !S_ISDIR(info.st_mode)) {
    fprintf(stderr, "error: output parent is not a directory\n");
    free(resolved_dir);
    free(stem);
    return NULL;
  }
  const char *suffix = options->target == DRIVER_TARGET_ZXN ? ".nex" : ".exe";
  char *filename = driver_path_with_suffix(stem, suffix);
  char *output = filename ? driver_path_join(resolved_dir, filename) : NULL;
  free(filename);
  if (!output) {
    free(resolved_dir);
    free(stem);
    return NULL;
  }
  *output_dir_out = resolved_dir;
  *stem_out = stem;
  return output;
}

static char *find_in_path(const char *name) {
  const char *path = getenv("PATH");
  if (!path) return NULL;
  char *copy = copy_string(path);
  if (!copy) return NULL;
  char *save = NULL;
  for (char *part = strtok_r(copy, ":", &save); part;
       part = strtok_r(NULL, ":", &save)) {
    char *candidate = driver_path_join(part[0] ? part : ".", name);
    if (candidate && access(candidate, X_OK) == 0) {
      char *resolved = realpath(candidate, NULL);
      free(candidate);
      free(copy);
      return resolved;
    }
    free(candidate);
  }
  free(copy);
  return NULL;
}

static char *resolve_executable(const char *argv0) {
#ifdef __APPLE__
  uint32_t size = 0;
  _NSGetExecutablePath(NULL, &size);
  char *buffer = malloc(size);
  if (buffer && _NSGetExecutablePath(buffer, &size) == 0) {
    char *resolved = realpath(buffer, NULL);
    free(buffer);
    if (resolved) return resolved;
  } else {
    free(buffer);
  }
#elif defined(__linux__)
  char buffer[PATH_MAX];
  ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length > 0) {
    buffer[length] = '\0';
    char *resolved = realpath(buffer, NULL);
    if (resolved) return resolved;
  }
#endif
  if (strchr(argv0, '/')) return realpath(argv0, NULL);
  return find_in_path(argv0);
}

int driver_resolve_paths(const char *argv0, const driver_options *options,
                         driver_paths *paths) {
  memset(paths, 0, sizeof(*paths));
  char cwd[PATH_MAX];
  if (!getcwd(cwd, sizeof(cwd))) {
    fprintf(stderr, "error: cannot determine current directory: %s\n",
            strerror(errno));
    return 0;
  }
  char *executable = resolve_executable(argv0);
  if (!executable) {
    fprintf(stderr, "error: cannot resolve driver executable '%s'\n", argv0);
    return 0;
  }
  paths->root = path_directory(executable);
  free(executable);
  paths->source = resolve_from_cwd(cwd, options->source_arg);
  if (!paths->source) {
    fprintf(stderr, "error: cannot resolve source '%s': %s\n",
            options->source_arg, strerror(errno));
    driver_free_paths(paths);
    return 0;
  }
  paths->output = resolve_output(cwd, paths->source, options,
                                 &paths->output_dir, &paths->output_stem);
  if (!paths->output) {
    driver_free_paths(paths);
    return 0;
  }
  if (strcmp(paths->source, paths->output) == 0) {
    fprintf(stderr, "error: source and output paths are the same\n");
    driver_free_paths(paths);
    return 0;
  }
  char workspace_template[] = "/tmp/rift-build-XXXXXX";
  paths->workspace = copy_string(workspace_template);
  if (!paths->workspace || !mkdtemp(paths->workspace)) {
    fprintf(stderr, "error: cannot create temporary build directory: %s\n",
            strerror(errno));
    driver_free_paths(paths);
    return 0;
  }
  chmod(paths->workspace, 0700);
  char *work_filename = driver_path_with_suffix(paths->output_stem, ".exe");
  paths->work_base = work_filename
                         ? driver_path_join(paths->workspace, work_filename)
                         : NULL;
  free(work_filename);
  if (!paths->work_base) {
    driver_free_paths(paths);
    return 0;
  }
  return 1;
}

static int remove_tree_contents(const char *path) {
  DIR *directory = opendir(path);
  if (!directory) return errno == ENOENT ? 0 : -1;
  int result = 0;
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    char *child = driver_path_join(path, entry->d_name);
    struct stat info;
    if (!child || lstat(child, &info) != 0) {
      result = -1;
      free(child);
      continue;
    }
    if (S_ISDIR(info.st_mode)) {
      if (remove_tree_contents(child) != 0 || rmdir(child) != 0) result = -1;
    } else if (unlink(child) != 0) {
      result = -1;
    }
    free(child);
  }
  closedir(directory);
  return result;
}

int driver_remove_tree(const char *path) {
  if (!path) return 0;
  if (remove_tree_contents(path) != 0) return -1;
  return rmdir(path);
}

int driver_publish_file(const char *source, const char *destination,
                        int executable) {
  int input = open(source, O_RDONLY);
  if (input < 0) {
    fprintf(stderr, "error: cannot open build artifact '%s': %s\n", source,
            strerror(errno));
    return 0;
  }
  char *directory = path_directory(destination);
  char publish_template[PATH_MAX];
  int written = snprintf(publish_template, sizeof(publish_template),
                         "%s/.rift-publish-XXXXXX", directory);
  free(directory);
  if (written < 0 || written >= (int)sizeof(publish_template)) {
    close(input);
    return 0;
  }
  int output = mkstemp(publish_template);
  if (output < 0) {
    close(input);
    fprintf(stderr, "error: cannot stage final output: %s\n", strerror(errno));
    return 0;
  }
  char buffer[65536];
  int ok = 1;
  for (;;) {
    ssize_t count = read(input, buffer, sizeof(buffer));
    if (count == 0) break;
    if (count < 0) {
      ok = 0;
      break;
    }
    ssize_t offset = 0;
    while (offset < count) {
      ssize_t copied = write(output, buffer + offset, (size_t)(count - offset));
      if (copied <= 0) {
        ok = 0;
        break;
      }
      offset += copied;
    }
    if (!ok) break;
  }
  if (executable && fchmod(output, 0755) != 0) ok = 0;
  if (fsync(output) != 0) ok = 0;
  if (close(input) != 0) ok = 0;
  if (close(output) != 0) ok = 0;
  if (ok && rename(publish_template, destination) != 0) ok = 0;
  if (!ok) {
    fprintf(stderr, "error: cannot publish '%s': %s\n", destination,
            strerror(errno));
    unlink(publish_template);
  }
  return ok;
}

void driver_free_paths(driver_paths *paths) {
  free(paths->root);
  free(paths->source);
  free(paths->output);
  free(paths->output_dir);
  free(paths->output_stem);
  free(paths->workspace);
  free(paths->work_base);
  memset(paths, 0, sizeof(*paths));
}
