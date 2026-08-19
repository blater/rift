#define _XOPEN_SOURCE 700
#include "build_plan.h"
#include "component_manifest.h"
#include "lib/arena.h"
#include "lib/alloc.h"
#include "options.h"
#include "paths.h"
#include "process.h"
#include "sidecar.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  char **items;
  int count;
  int capacity;
} argument_vector;

static int argument_push(argument_vector *arguments, char *value) {
  if (arguments->count + 1 >= arguments->capacity) {
    int capacity = arguments->capacity ? arguments->capacity * 2 : 32;
    char **items = realloc(arguments->items,
                           sizeof(char *) * (size_t)capacity);
    if (!items) return 0;
    arguments->items = items;
    arguments->capacity = capacity;
  }
  arguments->items[arguments->count++] = value;
  arguments->items[arguments->count] = NULL;
  return 1;
}

static const char *path_filename(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static char *copy_without_suffix(const char *path, const char *suffix) {
  size_t length = strlen(path);
  size_t suffix_length = strlen(suffix);
  if (length >= suffix_length &&
      strcmp(path + length - suffix_length, suffix) == 0)
    length -= suffix_length;
  char *result = malloc(length + 1);
  if (!result) return NULL;
  memcpy(result, path, length);
  result[length] = '\0';
  return result;
}

static int regular_file_size(const char *path, off_t *size) {
  struct stat info;
  if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) return 0;
  *size = info.st_size;
  return 1;
}

static int invoke_compiler(const driver_paths *paths,
                           const driver_options *options,
                           const char *manifest_path) {
  char *compiler = driver_path_join(paths->root, "riftc");
  char *manifest_option = NULL;
  size_t manifest_size = strlen(manifest_path) + 22;
  manifest_option = malloc(manifest_size);
  if (!compiler || !manifest_option) {
    free(compiler);
    free(manifest_option);
    return 1;
  }
  snprintf(manifest_option, manifest_size, "--component-manifest=%s",
           manifest_path);
  argument_vector arguments = {0};
  argument_push(&arguments, compiler);
  argument_push(&arguments, paths->source);
  argument_push(&arguments, paths->work_base);
  argument_push(&arguments, options->target == DRIVER_TARGET_ZXN
                                ? "--target=zxn"
                                : "--target=gcc");
  argument_push(&arguments, manifest_option);
  if (options->auto_cast) argument_push(&arguments, "--auto-cast");
  if (options->zxn_test) argument_push(&arguments, "--zxn-test");
  if (options->rtl_mode == DRIVER_RTL_ALL)
    argument_push(&arguments, "--components=all");
  int result = driver_run_process(arguments.items, NULL);
  free(arguments.items);
  free(manifest_option);
  free(compiler);
  return result;
}

static int build_host(const driver_paths *paths,
                      const driver_options *options,
                      const driver_build_plan *plan) {
  char *generated_c = driver_path_with_suffix(paths->work_base, ".c");
  char *include_lib = driver_path_join(paths->root, "src/lib");
  char *include_ext = driver_path_join(paths->root, "src/ext/lib");
  if (!generated_c || !include_lib || !include_ext) {
    free(generated_c);
    free(include_lib);
    free(include_ext);
    return 1;
  }
  argument_vector arguments = {0};
  argument_push(&arguments, "gcc");
  argument_push(&arguments, "-Wall");
  argument_push(&arguments, "-Wno-unused-variable");
  argument_push(&arguments, "-I");
  argument_push(&arguments, include_lib);
  argument_push(&arguments, "-I");
  argument_push(&arguments, include_ext);
  char memory_defines[6][96];
  snprintf(memory_defines[0], sizeof(memory_defines[0]),
           "-DRIFT_MEMORY_MAX_VALUE=%zu", options->memory_max);
  snprintf(memory_defines[1], sizeof(memory_defines[1]),
           "-DRIFT_MEMORY_MAX_PRESENT=%d", options->memory_max_set);
  snprintf(memory_defines[2], sizeof(memory_defines[2]),
           "-DRIFT_MEMORY_MIN_VALUE=%zu", options->memory_min);
  snprintf(memory_defines[3], sizeof(memory_defines[3]),
           "-DRIFT_MEMORY_MIN_PRESENT=%d", options->memory_min_set);
  snprintf(memory_defines[4], sizeof(memory_defines[4]),
           "-DRIFT_MEMORY_RESERVE_VALUE=0");
  snprintf(memory_defines[5], sizeof(memory_defines[5]),
           "-DRIFT_MEMORY_RESERVE_PRESENT=0");
  for (int i = 0; i < 6; i++) argument_push(&arguments, memory_defines[i]);
  if (!plan->bump_required)
    argument_push(&arguments, "-DRIFT_ZXN_NO_BUMP_POOL");
  argument_push(&arguments, "-o");
  argument_push(&arguments, paths->work_base);
  argument_push(&arguments, generated_c);
  for (int i = 0; i < plan->c_sources.count; i++)
    argument_push(&arguments, plan->c_sources.items[i]);
  for (int i = 0; i < plan->asm_sources.count; i++)
    argument_push(&arguments, plan->asm_sources.items[i]);
  argument_push(&arguments, "-lm");
  int result = driver_run_process(arguments.items, NULL);
  free(arguments.items);
  free(generated_c);
  free(include_lib);
  free(include_ext);
  return result;
}

static int read_map_symbol(const char *map_path, const char *wanted,
                           unsigned long *result) {
  FILE *file = fopen(map_path, "r");
  if (!file) return 0;
  char line[4096];
  int found = 0;
  while (fgets(line, sizeof(line), file)) {
    char symbol[256];
    char middle[256];
    char value[256];
    if (sscanf(line, "%255s %255s %255s", symbol, middle, value) == 3 &&
        strcmp(symbol, wanted) == 0) {
      const char *digits = value[0] == '$' ? value + 1 : value;
      char *end = NULL;
      errno = 0;
      unsigned long parsed = strtoul(digits, &end, 16);
      if (!errno && end && *end == '\0') {
        *result = parsed;
        found = 1;
      }
      break;
    }
  }
  fclose(file);
  return found;
}

static int build_zxn(const driver_paths *paths,
                     const driver_options *options,
                     const driver_build_plan *plan,
                     char **nex_path_out, char **code_path_out) {
  char *work_filename = (char *)path_filename(paths->work_base);
  char *work_stem = copy_without_suffix(work_filename, ".exe");
  char *generated_filename = driver_path_with_suffix(work_filename, ".c");
  char *asset_filename = driver_path_with_suffix(work_filename, ".assets.asm");
  char *map_filename = driver_path_with_suffix(work_stem, ".map");
  char *nex_filename = driver_path_with_suffix(work_stem, ".nex");
  char *code_filename = driver_path_with_suffix(work_stem, "_CODE.bin");
  char *asset_path = asset_filename
                         ? driver_path_join(paths->workspace, asset_filename)
                         : NULL;
  char *map_path = map_filename
                       ? driver_path_join(paths->workspace, map_filename)
                       : NULL;
  char *nex_path = nex_filename
                       ? driver_path_join(paths->workspace, nex_filename)
                       : NULL;
  char *code_path = code_filename
                        ? driver_path_join(paths->workspace, code_filename)
                        : NULL;
  char *pragma_path = driver_path_join(paths->root,
                                       "src/lib/zxn/zpragma_zxn.inc");
  char *include_path = driver_path_join(paths->root, "src/lib");
  char *uploader_path = driver_path_join(paths->root,
                                         "src/lib/zxn/sprite_upload.asm");
  char *verifier_path = driver_path_join(paths->root,
                                         "build/verify-zxn-assets");
  if (!work_stem || !generated_filename || !asset_filename || !map_filename ||
      !nex_filename || !code_filename || !asset_path || !map_path ||
      !nex_path || !code_path || !pragma_path || !include_path ||
      !uploader_path || !verifier_path) {
    goto allocation_failure;
  }
  off_t asset_size = 0;
  int has_assets = regular_file_size(asset_path, &asset_size) && asset_size > 0;
  if (has_assets && access(uploader_path, R_OK) != 0) {
    fprintf(stderr, "build failed: missing ZXN sprite asset uploader %s\n",
            uploader_path);
    goto failure;
  }
  char startup[32];
  char memory_defines[6][96];
  char pragma_option[4096];
  char include_option[4096];
  snprintf(startup, sizeof(startup), "-startup=%d", plan->startup);
  snprintf(memory_defines[0], sizeof(memory_defines[0]),
           "-DRIFT_MEMORY_MAX_VALUE=%zu", options->memory_max);
  snprintf(memory_defines[1], sizeof(memory_defines[1]),
           "-DRIFT_MEMORY_MAX_PRESENT=%d", options->memory_max_set);
  snprintf(memory_defines[2], sizeof(memory_defines[2]),
           "-DRIFT_MEMORY_MIN_VALUE=%zu", options->memory_min);
  snprintf(memory_defines[3], sizeof(memory_defines[3]),
           "-DRIFT_MEMORY_MIN_PRESENT=%d", options->memory_min_set);
  snprintf(memory_defines[4], sizeof(memory_defines[4]),
           "-DRIFT_MEMORY_RESERVE_VALUE=%zu", options->memory_reserve);
  snprintf(memory_defines[5], sizeof(memory_defines[5]),
           "-DRIFT_MEMORY_RESERVE_PRESENT=%d", options->memory_reserve_set);
  snprintf(pragma_option, sizeof(pragma_option), "-pragma-include:%s",
           pragma_path);
  snprintf(include_option, sizeof(include_option), "-I%s", include_path);
  char memory_max_text[32];
  if (options->memory_max_set)
    snprintf(memory_max_text, sizeof(memory_max_text), "%zu",
             options->memory_max);
  else
    snprintf(memory_max_text, sizeof(memory_max_text), "auto");
  fprintf(stdout,
          "ZXN profile: startup=%d, memory=auto, memory-max=%s, "
          "memory-min=%zu, memory-reserve=%zu, bump=%s, "
          "tiny-core=%d, light-core=%d\n",
          plan->startup,
          memory_max_text,
          options->memory_min, options->memory_reserve,
          plan->bump_required ? "included" : "omitted",
          plan->tiny_core, plan->light_core);
  argument_vector arguments = {0};
  argument_push(&arguments, "zcc");
  argument_push(&arguments, "+zxn");
  argument_push(&arguments, "-m");
  argument_push(&arguments, "-vn");
  argument_push(&arguments, "-subtype=nex");
  argument_push(&arguments, startup);
  argument_push(&arguments, "-clib=sdcc_iy");
  argument_push(&arguments, "--opt-code-size");
  argument_push(&arguments, "-create-app");
  if (options->zxn_test) argument_push(&arguments, "-DRIFT_ZXN_TEST");
  if (options->allocator_stats)
    argument_push(&arguments, "-DRIFT_ALLOCATOR_STATS");
  if (!plan->pools_required)
    argument_push(&arguments, "-DRIFT_ZXN_NO_POOLS");
  else if (!plan->bump_required)
    argument_push(&arguments, "-DRIFT_ZXN_NO_BUMP_POOL");
  for (int i = 0; i < 6; i++) argument_push(&arguments, memory_defines[i]);
  if (plan->tiny_core) argument_push(&arguments, "-DRIFT_ZXN_TINY_CORE");
  if (plan->tiny_print_direct)
    argument_push(&arguments, "-DRIFT_ZXN_TINY_PRINT_DIRECT");
  if (plan->tiny_print_controls)
    argument_push(&arguments, "-DRIFT_ZXN_TINY_PRINT_CONTROLS");
  if (plan->light_core) argument_push(&arguments, "-DRIFT_ZXN_LIGHT_CORE");
  argument_push(&arguments, pragma_option);
  argument_push(&arguments, include_option);
  argument_push(&arguments, "-lm");
  argument_push(&arguments, "-o");
  argument_push(&arguments, work_filename);
  argument_push(&arguments, generated_filename);
  for (int i = 0; i < plan->c_sources.count; i++)
    argument_push(&arguments, plan->c_sources.items[i]);
  for (int i = 0; i < plan->asm_sources.count; i++)
    argument_push(&arguments, plan->asm_sources.items[i]);
  if (has_assets) {
    argument_push(&arguments, uploader_path);
    argument_push(&arguments, asset_filename);
  }
  int result = driver_run_process(arguments.items, paths->workspace);
  free(arguments.items);
  if (result != 0) goto failure;
  unsigned long bss_end;
  unsigned long stack_top;
  unsigned long stack_size;
  if (!read_map_symbol(map_path, "__BSS_END_tail", &bss_end) ||
      !read_map_symbol(map_path, "REGISTER_SP", &stack_top) ||
      !read_map_symbol(map_path, "CRT_STACK_SIZE", &stack_size)) {
    fprintf(stderr,
            "build failed: link map lacks arena/stack boundary symbols\n");
    goto failure;
  }
  if (stack_size > stack_top) {
    fprintf(stderr, "build failed: linked stack bound underflows address space\n");
    goto failure;
  }
  unsigned long stack_floor = stack_top - stack_size;
  if (options->memory_reserve > stack_floor) {
    fprintf(stderr, "build failed: --memory-reserve exceeds target RAM\n");
    goto failure;
  }
  unsigned long arena_floor = stack_floor - options->memory_reserve;
  arena_floor -= arena_floor % RIFT_ARENA_ALIGNMENT;
  if (bss_end >= arena_floor) {
    fprintf(stderr,
            "build failed: ZXN BSS ends at 0x%04lX, entering reserved high "
            "memory below the 0x%04lX-0x%04lX stack range\n",
            bss_end, stack_floor, stack_top);
    goto failure;
  }
  fprintf(stdout,
          "ZXN layout: BSS ends at 0x%04lX; reserved stack is "
          "0x%04lX-0x%04lX\n",
          bss_end, stack_floor, stack_top);
  unsigned long arena_start =
      (bss_end + RIFT_ARENA_ALIGNMENT - 1ul) &
      ~(unsigned long)(RIFT_ARENA_ALIGNMENT - 1ul);
  if (arena_start >= arena_floor) {
    fprintf(stderr,
            "build failed: aligned ZXN arena start 0x%04lX enters reserved "
            "high memory\n",
            arena_start);
    goto failure;
  }
  unsigned long arena_capacity = arena_floor - arena_start;
  if (options->memory_max_set && arena_capacity > options->memory_max) {
    arena_capacity = options->memory_max;
    arena_capacity -= arena_capacity % RIFT_ARENA_ALIGNMENT;
  }
  if (options->memory_min_set && arena_capacity < options->memory_min) {
    fprintf(stderr,
            "build failed: ZXN managed headroom is %lu bytes, below "
            "--memory-min=%zu\n",
            arena_capacity, options->memory_min);
    goto failure;
  }
  if (plan->pools_required) {
    fprintf(stdout,
            "ZXN managed arena: 0x%04lX-0x%04lX (%lu bytes); "
            "high-memory reserve=%zu bytes\n",
            arena_start, arena_start + arena_capacity, arena_capacity,
            options->memory_reserve);
  }
  if (has_assets) {
    char *verify_argv[] = {verifier_path, "--map", map_path, "--nex",
                           nex_path, "--ram-pages=96", NULL};
    if (driver_run_process(verify_argv, NULL) != 0) {
      unlink(nex_path);
      fprintf(stderr,
              "build failed: asset placement did not match the linked NEX\n");
      goto failure;
    }
  }
  *nex_path_out = nex_path;
  *code_path_out = code_path;
  free(work_stem);
  free(generated_filename);
  free(asset_filename);
  free(map_filename);
  free(nex_filename);
  free(code_filename);
  free(asset_path);
  free(map_path);
  free(pragma_path);
  free(include_path);
  free(uploader_path);
  free(verifier_path);
  return 0;

allocation_failure:
  fprintf(stderr, "error: out of memory while planning ZXN build\n");
failure:
  free(nex_path);
  free(code_path);
  free(work_stem);
  free(generated_filename);
  free(asset_filename);
  free(map_filename);
  free(nex_filename);
  free(code_filename);
  free(asset_path);
  free(map_path);
  free(pragma_path);
  free(include_path);
  free(uploader_path);
  free(verifier_path);
  return 1;
}

static void print_components(const driver_requirements *requirements) {
  fputs("RTL components: ", stdout);
  for (int i = 0; i < requirements->component_count; i++)
    fprintf(stdout, "%s ", requirements->components[i]->id);
  fputc('\n', stdout);
}

int main(int argc, char **argv) {
  driver_options options;
  int parsed = driver_parse_options(argc, argv, &options);
  if (parsed == 2) return 0;
  if (!parsed) return 1;

  driver_paths paths;
  if (!driver_resolve_paths(argv[0], &options, &paths)) return 1;
  if (options.debug) {
    fprintf(stdout, "rift: debug workspace: %s\n", paths.workspace);
    fflush(stdout);
  }

  int status = 1;
  char *manifest_path = driver_path_join(paths.root,
                                         "src/lib/components.manifest");
  char *sidecar_path = driver_path_with_suffix(paths.work_base, ".components");
  driver_build_plan plan = {0};
  char *zxn_nex = NULL;
  char *zxn_code = NULL;
  if (!manifest_path || !sidecar_path) goto cleanup;

  if (invoke_compiler(&paths, &options, manifest_path) != 0) goto cleanup;

  init_compiler_stack();
  component_manifest *manifest = load_component_manifest(manifest_path);
  driver_requirements requirements;
  if (!driver_read_requirements(sidecar_path, manifest, &requirements)) {
    kill_compiler_stack();
    goto cleanup;
  }
  if (!requirements.pools_required &&
      (options.memory_max_set || options.memory_min_set ||
       options.memory_reserve_set)) {
    fprintf(stderr,
            "build failed: memory bounds require a program with managed "
            "allocation\n");
    kill_compiler_stack();
    goto cleanup;
  }
  if (!driver_make_build_plan(paths.root, &options, &requirements, &plan)) {
    kill_compiler_stack();
    goto cleanup;
  }
  print_components(&requirements);
  fprintf(stdout, "compile %s -> %s\n", paths.source, paths.output);
  kill_compiler_stack();

  if (options.target == DRIVER_TARGET_HOST) {
    if (build_host(&paths, &options, &plan) != 0) goto cleanup;
    if (!driver_publish_file(paths.work_base, paths.output, 1)) goto cleanup;
    off_t size;
    if (!regular_file_size(paths.output, &size)) goto cleanup;
    fprintf(stdout, "created %s (EXE: %lld bytes)\n", paths.output,
            (long long)size);
  } else {
    if (build_zxn(&paths, &options, &plan, &zxn_nex, &zxn_code) != 0)
      goto cleanup;
    if (!driver_publish_file(zxn_nex, paths.output, 0)) goto cleanup;
    off_t size;
    off_t code_size;
    if (!regular_file_size(paths.output, &size) ||
        !regular_file_size(zxn_code, &code_size))
      goto cleanup;
    fprintf(stdout,
            "created %s (ZXN: %lld bytes; pre-wrap EXE: %lld bytes)\n",
            paths.output, (long long)size, (long long)code_size);
  }

  if (options.debug) {
    char *generated_c = driver_path_with_suffix(paths.work_base, ".c");
    if (generated_c) {
      char *format_argv[] = {"clang-format", "-i", generated_c, NULL};
      int format_status = driver_run_process(format_argv, NULL);
      if (format_status != 0)
        fprintf(stderr, "rift: warning: clang-format failed; build retained\n");
      free(generated_c);
    }
  }
  status = 0;
  if (options.run) {
    if (options.target == DRIVER_TARGET_HOST) {
      char *run_argv[] = {paths.output, NULL};
      status = driver_run_process(run_argv, NULL);
    } else {
      char *run_argv[] = {"zx", paths.output, NULL};
      status = driver_run_process(run_argv, NULL);
    }
  }

cleanup:
  driver_free_build_plan(&plan);
  free(manifest_path);
  free(sidecar_path);
  free(zxn_nex);
  free(zxn_code);
  if (!options.debug && paths.workspace) {
    if (driver_remove_tree(paths.workspace) != 0 && errno != ENOENT)
      fprintf(stderr, "rift: warning: cannot remove temporary workspace %s\n",
              paths.workspace);
  }
  driver_free_paths(&paths);
  return status;
}
