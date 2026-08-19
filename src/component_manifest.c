#include "component_manifest.h"
#include "lib/alloc.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MANIFEST_LINE_MAX 4096
#define MANIFEST_FIELDS_MAX 12

static char *manifest_copy(const char *text) {
  size_t size = strlen(text) + 1;
  char *copy = allocate_compiler_persistent(size);
  memcpy(copy, text, size);
  return copy;
}

static int split_fields(char *line, char **fields, int capacity) {
  int count = 0;
  char *start = line;
  for (char *cursor = line;; cursor++) {
    if (*cursor == '|' || *cursor == '\0') {
      if (count >= capacity) return -1;
      fields[count++] = start;
      if (*cursor == '\0') break;
      *cursor = '\0';
      start = cursor + 1;
    }
  }
  return count;
}

static void manifest_error(const char *path, int line, const char *message) {
  if (line > 0)
    fprintf(stderr, "%s:%d: error: component manifest %s\n", path, line,
            message);
  else
    fprintf(stderr, "%s: error: component manifest %s\n", path, message);
  exit(1);
}

static int valid_identifier(const char *value, int lowercase_only) {
  if (!value || !value[0] || !(isalpha((unsigned char)value[0]) || value[0] == '_'))
    return 0;
  for (const char *cursor = value + 1; *cursor; cursor++) {
    if (!(isalnum((unsigned char)*cursor) || *cursor == '_')) return 0;
  }
  if (lowercase_only) {
    for (const char *cursor = value; *cursor; cursor++)
      if (isupper((unsigned char)*cursor)) return 0;
  }
  return 1;
}

static int valid_manifest_path(const char *path) {
  if (!path || !path[0] || path[0] == '/' || strstr(path, "..")) return 0;
  for (const char *cursor = path; *cursor; cursor++)
    if (!(isalnum((unsigned char)*cursor) || *cursor == '_' ||
          *cursor == '-' || *cursor == '.' || *cursor == '/'))
      return 0;
  return 1;
}

static void validate_path_list(component_manifest *manifest, const char *list) {
  char item[512];
  int count = component_parameter_count(list);
  for (int i = 0; i < count; i++)
    if (!component_parameter_at(list, i, item, sizeof(item)) ||
        !valid_manifest_path(item))
      manifest_error(manifest->path, 0, "contains an unsafe component path");
}

static int list_contains(const char *list, const char *wanted) {
  char item[512];
  int count = component_parameter_count(list);
  for (int i = 0; i < count; i++)
    if (component_parameter_at(list, i, item, sizeof(item)) &&
        strcmp(item, wanted) == 0)
      return 1;
  return 0;
}

static int primitive_interface_type(const char *name, int allow_void) {
  const char *primitives[] = {"boolean", "bool", "byte", "char", "int",
                              "word", "dword", "float", "string"};
  if (allow_void && strcmp(name, "void") == 0) return 1;
  for (size_t i = 0; i < sizeof(primitives) / sizeof(primitives[0]); i++)
    if (strcmp(name, primitives[i]) == 0) return 1;
  return 0;
}

static int valid_interface_type(component_manifest *manifest,
                                const char *type_name, int allow_void) {
  size_t length = strlen(type_name);
  char base[128];
  if (length == 0 || length >= sizeof(base)) return 0;
  memcpy(base, type_name, length + 1);
  if (length >= 2 && base[length - 2] == '[' && base[length - 1] == ']')
    base[length - 2] = '\0';
  if (primitive_interface_type(base, allow_void)) return 1;
  if (find_opaque_interface(manifest, base) != NULL) return 1;
  return 0;
}

static int asset_category_type(component_manifest *manifest,
                               const char *type_name) {
  size_t length = strlen(type_name);
  char base[128];
  if (length == 0 || length >= sizeof(base)) return 0;
  memcpy(base, type_name, length + 1);
  if (length >= 2 && base[length - 2] == '[' && base[length - 1] == ']')
    base[length - 2] = '\0';
  for (int i = 0; i < manifest->asset_kind_count; i++)
    if (strcmp(manifest->asset_kinds[i].category, base) == 0) return 1;
  return 0;
}

static int valid_asset_consumer_parameter(component_manifest *manifest,
                                          component_interface_spec *entry,
                                          int index,
                                          const char *type_name) {
  if (entry->kind != COMPONENT_INSTANCE_METHOD ||
      strcmp(entry->owner, "Sprite") != 0 ||
      strcmp(entry->name, "frame") != 0 || index != 0 ||
      component_parameter_count(entry->parameters) != 2)
    return 0;
  for (int i = 0; i < manifest->asset_kind_count; i++) {
    component_asset_kind_spec *asset = &manifest->asset_kinds[i];
    if (strcmp(asset->category, type_name) == 0 &&
        strcmp(asset->component_id, entry->component_id) == 0)
      return 1;
  }
  return 0;
}

static void validate_unique_path_column(component_manifest *manifest,
                                        int column) {
  char item[512];
  for (int i = 0; i < manifest->component_count; i++) {
    component_spec *left = &manifest->components[i];
    const char *left_list = column == 0 ? left->host_sources
                            : column == 1 ? left->zxn_sources
                            : column == 2 ? left->host_asm
                                          : left->zxn_asm;
    int count = component_parameter_count(left_list);
    for (int j = 0; j < count; j++) {
      if (!component_parameter_at(left_list, j, item, sizeof(item)))
        manifest_error(manifest->path, 0, "has an invalid component path list");
      for (int k = 0; k < j; k++) {
        char prior[512];
        component_parameter_at(left_list, k, prior, sizeof(prior));
        if (strcmp(item, prior) == 0)
          manifest_error(manifest->path, 0,
                         "contains a duplicate source in one component");
      }
      for (int k = 0; k < i; k++) {
        component_spec *right = &manifest->components[k];
        const char *right_list = column == 0 ? right->host_sources
                                  : column == 1 ? right->zxn_sources
                                  : column == 2 ? right->host_asm
                                                : right->zxn_asm;
        if (list_contains(right_list, item))
          manifest_error(manifest->path, 0,
                         "assigns one source to multiple components");
      }
    }
  }
}

static void validate_component_cycle(component_manifest *manifest, int index,
                                     unsigned char *visiting,
                                     unsigned char *visited) {
  if (visited[index]) return;
  if (visiting[index])
    manifest_error(manifest->path, 0, "contains a component dependency cycle");
  visiting[index] = 1;
  component_spec *component = &manifest->components[index];
  char dependency[128];
  int count = component_parameter_count(component->dependencies);
  for (int i = 0; i < count; i++) {
    component_parameter_at(component->dependencies, i, dependency,
                           sizeof(dependency));
    component_spec *target = find_component(manifest, dependency);
    validate_component_cycle(manifest, (int)(target - manifest->components),
                             visiting, visited);
  }
  visiting[index] = 0;
  visited[index] = 1;
}

int component_parameter_count(const char *parameters) {
  if (parameters == NULL || parameters[0] == '\0') return 0;
  int count = 1;
  for (const char *cursor = parameters; *cursor; cursor++)
    if (*cursor == ',') count++;
  return count;
}

const char *component_parameter_at(const char *parameters, int index,
                                   char *buffer, int buffer_size) {
  if (parameters == NULL || index < 0 || buffer_size <= 0) return NULL;
  const char *start = parameters;
  for (int current = 0;; current++) {
    const char *end = strchr(start, ',');
    if (current == index) {
      size_t length = end ? (size_t)(end - start) : strlen(start);
      if (length == 0 || length >= (size_t)buffer_size) return NULL;
      memcpy(buffer, start, length);
      buffer[length] = '\0';
      return buffer;
    }
    if (!end) return NULL;
    start = end + 1;
  }
}

component_spec *find_component(component_manifest *manifest, const char *id) {
  if (!manifest || !id) return NULL;
  for (int i = 0; i < manifest->component_count; i++)
    if (strcmp(manifest->components[i].id, id) == 0)
      return &manifest->components[i];
  return NULL;
}

component_interface_spec *find_opaque_interface(component_manifest *manifest,
                                                const char *owner) {
  if (!manifest || !owner) return NULL;
  for (int i = 0; i < manifest->interface_count; i++) {
    component_interface_spec *entry = &manifest->interfaces[i];
    if (entry->kind == COMPONENT_OPAQUE && strcmp(entry->owner, owner) == 0)
      return entry;
  }
  return NULL;
}

component_interface_spec *find_builtin_interface(component_manifest *manifest,
                                                 const char *name, int arity) {
  if (!manifest || !name) return NULL;
  for (int i = 0; i < manifest->interface_count; i++) {
    component_interface_spec *entry = &manifest->interfaces[i];
    int callable = entry->kind == COMPONENT_BUILTIN ||
                   entry->kind == COMPONENT_LOWERED_BUILTIN ||
                   entry->kind == COMPONENT_OPAQUE;
    if (callable && strcmp(entry->name, name) == 0 &&
        component_parameter_count(entry->parameters) == arity)
      return entry;
  }
  return NULL;
}

component_interface_spec *find_method_interface(component_manifest *manifest,
                                                const char *owner,
                                                const char *name,
                                                int type_level, int arity) {
  if (!manifest || !owner || !name) return NULL;
  component_interface_kind wanted =
      type_level ? COMPONENT_TYPE_METHOD : COMPONENT_INSTANCE_METHOD;
  for (int i = 0; i < manifest->interface_count; i++) {
    component_interface_spec *entry = &manifest->interfaces[i];
    if (entry->kind == wanted && strcmp(entry->owner, owner) == 0 &&
        strcmp(entry->name, name) == 0 &&
        component_parameter_count(entry->parameters) == arity)
      return entry;
  }
  return NULL;
}

component_namespace_spec *find_namespace(component_manifest *manifest,
                                         const char *owner) {
  if (!manifest || !owner) return NULL;
  for (int i = 0; i < manifest->namespace_count; i++)
    if (strcmp(manifest->namespaces[i].owner, owner) == 0)
      return &manifest->namespaces[i];
  return NULL;
}

component_asset_kind_spec *find_asset_kind(component_manifest *manifest,
                                           const char *kind) {
  if (!manifest || !kind) return NULL;
  for (int i = 0; i < manifest->asset_kind_count; i++)
    if (strcmp(manifest->asset_kinds[i].kind, kind) == 0)
      return &manifest->asset_kinds[i];
  return NULL;
}

static void validate_manifest(component_manifest *manifest) {
  if (manifest->component_count == 0)
    manifest_error(manifest->path, 0, "contains no components");
  for (int i = 0; i < manifest->component_count; i++) {
    component_spec *component = &manifest->components[i];
    if (!valid_identifier(component->id, 1))
      manifest_error(manifest->path, 0, "has an invalid component ID");
    validate_path_list(manifest, component->headers);
    validate_path_list(manifest, component->host_sources);
    validate_path_list(manifest, component->zxn_sources);
    validate_path_list(manifest, component->host_asm);
    validate_path_list(manifest, component->zxn_asm);
    if ((component->init_hook[0] &&
         !valid_identifier(component->init_hook, 0)) ||
        (component->shutdown_hook[0] &&
         !valid_identifier(component->shutdown_hook, 0)))
      manifest_error(manifest->path, 0, "has an invalid lifecycle hook");
    for (int j = 0; j < i; j++) {
      if (strcmp(component->id, manifest->components[j].id) == 0)
        manifest_error(manifest->path, 0, "contains a duplicate component ID");
      if ((component->init_hook[0] &&
           strcmp(component->init_hook, manifest->components[j].init_hook) == 0) ||
          (component->shutdown_hook[0] &&
           strcmp(component->shutdown_hook,
                  manifest->components[j].shutdown_hook) == 0))
        manifest_error(manifest->path, 0, "contains a duplicate lifecycle hook");
    }
    char dependency[128];
    int dependency_count = component_parameter_count(component->dependencies);
    for (int j = 0; j < dependency_count; j++) {
      if (!component_parameter_at(component->dependencies, j, dependency,
                                  sizeof(dependency)) ||
          !find_component(manifest, dependency))
        manifest_error(manifest->path, 0,
                       "references an unknown component dependency");
    }
  }
  validate_unique_path_column(manifest, 0);
  validate_unique_path_column(manifest, 1);
  validate_unique_path_column(manifest, 2);
  validate_unique_path_column(manifest, 3);
  unsigned char visiting[COMPONENT_MANIFEST_MAX_COMPONENTS] = {0};
  unsigned char visited[COMPONENT_MANIFEST_MAX_COMPONENTS] = {0};
  for (int i = 0; i < manifest->component_count; i++)
    validate_component_cycle(manifest, i, visiting, visited);
  for (int i = 0; i < manifest->namespace_count; i++) {
    component_namespace_spec *entry = &manifest->namespaces[i];
    if (!valid_identifier(entry->owner, 0) ||
        !find_component(manifest, entry->component_id))
      manifest_error(manifest->path, 0, "has an invalid namespace row");
    for (int j = 0; j < i; j++)
      if (strcmp(entry->owner, manifest->namespaces[j].owner) == 0)
        manifest_error(manifest->path, 0, "contains a duplicate namespace owner");
  }
  for (int i = 0; i < manifest->asset_kind_count; i++) {
    component_asset_kind_spec *entry = &manifest->asset_kinds[i];
    if (!valid_identifier(entry->kind, 0) ||
        !valid_identifier(entry->category, 0) ||
        !find_component(manifest, entry->component_id))
      manifest_error(manifest->path, 0, "has an invalid asset row");
    if (primitive_interface_type(entry->category, 1) ||
        find_opaque_interface(manifest, entry->category) ||
        find_namespace(manifest, entry->category))
      manifest_error(manifest->path, 0,
                     "asset category collides with a runtime or namespace type");
    if ((strcmp(entry->kind, "sprite4") != 0 &&
         strcmp(entry->kind, "sprite8") != 0) ||
        strcmp(entry->category, "SpritePattern") != 0)
      manifest_error(manifest->path, 0,
                     "sprite assets support only sprite4/sprite8 SpritePattern assets");
    for (int j = 0; j < i; j++) {
      component_asset_kind_spec *prior = &manifest->asset_kinds[j];
      if (strcmp(entry->kind, prior->kind) == 0)
        manifest_error(manifest->path, 0, "contains a duplicate asset kind");
    }
  }
  for (int i = 0; i < manifest->interface_count; i++) {
    component_interface_spec *entry = &manifest->interfaces[i];
    if (!find_component(manifest, entry->component_id))
      manifest_error(manifest->path, 0,
                     "interface references an unknown component");
    if (component_parameter_count(entry->parameters) >
        COMPONENT_MANIFEST_MAX_PARAMS)
      manifest_error(manifest->path, 0,
                     "interface has too many parameters");
    if ((entry->kind == COMPONENT_INSTANCE_METHOD ||
         entry->kind == COMPONENT_TYPE_METHOD) &&
        !find_opaque_interface(manifest, entry->owner) &&
        !find_namespace(manifest, entry->owner))
      manifest_error(manifest->path, 0,
                     "method owner is not a declared opaque interface or namespace");
    if (entry->kind == COMPONENT_OPAQUE) {
      char expected[256];
      int written = snprintf(expected, sizeof(expected), "%s_new", entry->owner);
      if (written < 0 || written >= (int)sizeof(expected) ||
          strcmp(entry->constructor, expected) != 0)
        manifest_error(manifest->path, 0,
                       "opaque constructor must be named Owner_new");
    }
    if (!valid_identifier(entry->name, 0) ||
        (entry->owner[0] && !valid_identifier(entry->owner, 0)) ||
        !valid_identifier(entry->c_symbol, 0))
      manifest_error(manifest->path, 0, "has an invalid interface identifier");
    if (asset_category_type(manifest, entry->return_type))
      manifest_error(manifest->path, 0,
                     "asset category may not be an interface return type");
    if (!valid_interface_type(manifest, entry->return_type, 1))
      manifest_error(manifest->path, 0, "has an unknown interface return type");
    char parameter[128];
    int parameter_count = component_parameter_count(entry->parameters);
    for (int j = 0; j < parameter_count; j++) {
      if (!component_parameter_at(entry->parameters, j, parameter,
                                  sizeof(parameter)))
        manifest_error(manifest->path, 0,
                       "has an unknown interface parameter type");
      if (strcmp(parameter, "printable") == 0) {
        if (entry->kind != COMPONENT_LOWERED_BUILTIN)
          manifest_error(manifest->path, 0,
                         "printable parameters require a lowered builtin");
      } else if (asset_category_type(manifest, parameter)) {
        if (!valid_asset_consumer_parameter(manifest, entry, j, parameter))
          manifest_error(manifest->path, 0,
                         "asset category is only valid in its registered consumer parameter");
      } else if (!valid_interface_type(manifest, parameter, 0)) {
        manifest_error(manifest->path, 0,
                       "has an unknown interface parameter type");
      }
    }
    for (int j = 0; j < i; j++) {
      component_interface_spec *prior = &manifest->interfaces[j];
      if (entry->kind == COMPONENT_OPAQUE &&
          prior->kind == COMPONENT_OPAQUE &&
          strcmp(entry->owner, prior->owner) == 0)
        manifest_error(manifest->path, 0,
                       "contains a duplicate opaque owner");
      if (entry->kind == prior->kind &&
          strcmp(entry->owner, prior->owner) == 0 &&
          strcmp(entry->name, prior->name) == 0 &&
          (entry->kind == COMPONENT_INSTANCE_METHOD ||
           component_parameter_count(entry->parameters) ==
               component_parameter_count(prior->parameters)))
        manifest_error(manifest->path, 0, "contains a duplicate interface");
      if (strcmp(entry->c_symbol, prior->c_symbol) == 0)
        manifest_error(manifest->path, 0, "contains a duplicate native symbol");
    }
  }
  int has_sprite4 = 0;
  int has_sprite8 = 0;
  for (int i = 0; i < manifest->asset_kind_count; i++) {
    component_asset_kind_spec *asset = &manifest->asset_kinds[i];
    if (strcmp(asset->kind, "sprite4") == 0) has_sprite4 = 1;
    if (strcmp(asset->kind, "sprite8") == 0) has_sprite8 = 1;
    component_interface_spec *consumer =
        find_method_interface(manifest, "Sprite", "frame", 0, 2);
    char category[128];
    if (!consumer || strcmp(consumer->component_id, asset->component_id) != 0 ||
        !component_parameter_at(consumer->parameters, 0, category,
                                sizeof(category)) ||
        strcmp(category, asset->category) != 0)
      manifest_error(manifest->path, 0,
                     "sprite assets require Sprite.frame argument 1 to be SpritePattern in the same component");
  }
  if (manifest->asset_kind_count > 0 && (!has_sprite4 || !has_sprite8))
    manifest_error(manifest->path, 0,
                   "sprite assets require both sprite4 and sprite8 SpritePattern asset kinds");
}

component_manifest *load_component_manifest(const char *path) {
  FILE *file = fopen(path, "r");
  if (!file) {
    fprintf(stderr, "error: cannot open component manifest '%s'\n", path);
    exit(1);
  }
  component_manifest *manifest =
      allocate_compiler_persistent(sizeof(component_manifest));
  memset(manifest, 0, sizeof(*manifest));
  manifest->path = manifest_copy(path);

  char line[MANIFEST_LINE_MAX];
  int line_number = 0;
  while (fgets(line, sizeof(line), file)) {
    line_number++;
    size_t length = strlen(line);
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
      line[--length] = '\0';
    if (line[0] == '\0' || line[0] == '#') continue;
    char *fields[MANIFEST_FIELDS_MAX] = {0};
    int count = split_fields(line, fields, MANIFEST_FIELDS_MAX);
    if (count < 0) manifest_error(path, line_number, "has too many fields");

    if (strcmp(fields[0], "component") == 0) {
      if (count != 11 && count != 12)
        manifest_error(path, line_number,
                       "component row needs 11 or 12 fields");
      if (manifest->component_count >= COMPONENT_MANIFEST_MAX_COMPONENTS)
        manifest_error(path, line_number, "has too many components");
      component_spec *entry = &manifest->components[manifest->component_count++];
      entry->id = manifest_copy(fields[1]);
      entry->dependencies = manifest_copy(fields[2]);
      entry->headers = manifest_copy(fields[3]);
      entry->host_sources = manifest_copy(fields[4]);
      entry->zxn_sources = manifest_copy(fields[5]);
      entry->host_asm = manifest_copy(fields[6]);
      entry->zxn_asm = manifest_copy(fields[7]);
      entry->init_hook = manifest_copy(fields[8]);
      entry->shutdown_hook = manifest_copy(fields[9]);
      entry->always = strcmp(fields[10], "always") == 0;
      entry->zxn_startup31_safe =
          count == 12 && list_contains(fields[11], "startup31");
      entry->zxn_pools_required =
          count == 12 && list_contains(fields[11], "pools");
      if (entry->id[0] == '\0') manifest_error(path, line_number, "has an empty component ID");
      if (fields[10][0] && strcmp(fields[10], "always") != 0)
        manifest_error(path, line_number, "component selection must be empty or always");
      if (count == 12) {
        char capability[128];
        int capability_count = component_parameter_count(fields[11]);
        for (int i = 0; i < capability_count; i++) {
          if (!component_parameter_at(fields[11], i, capability,
                                      sizeof(capability)) ||
              (strcmp(capability, "startup31") != 0 &&
               strcmp(capability, "pools") != 0))
            manifest_error(
                path, line_number,
                "component ZXN capabilities must contain only startup31 or pools");
        }
      }
      continue;
    }

    if (strcmp(fields[0], "namespace") == 0) {
      if (count != 3) manifest_error(path, line_number, "namespace row needs 3 fields");
      if (manifest->namespace_count >= COMPONENT_MANIFEST_MAX_NAMESPACES)
        manifest_error(path, line_number, "has too many namespaces");
      component_namespace_spec *entry =
          &manifest->namespaces[manifest->namespace_count++];
      entry->owner = manifest_copy(fields[1]);
      entry->component_id = manifest_copy(fields[2]);
      continue;
    }

    if (strcmp(fields[0], "asset") == 0) {
      if (count != 4) manifest_error(path, line_number, "asset row needs 4 fields");
      if (manifest->asset_kind_count >= COMPONENT_MANIFEST_MAX_ASSET_KINDS)
        manifest_error(path, line_number, "has too many asset kinds");
      component_asset_kind_spec *entry =
          &manifest->asset_kinds[manifest->asset_kind_count++];
      entry->kind = manifest_copy(fields[1]);
      entry->category = manifest_copy(fields[2]);
      entry->component_id = manifest_copy(fields[3]);
      continue;
    }

    if (manifest->interface_count >= COMPONENT_MANIFEST_MAX_INTERFACES)
      manifest_error(path, line_number, "has too many interfaces");
    component_interface_spec *entry =
        &manifest->interfaces[manifest->interface_count++];
    if (strcmp(fields[0], "builtin") == 0 ||
        strcmp(fields[0], "lowered") == 0) {
      if (count != 7) manifest_error(path, line_number, "builtin row needs 7 fields");
      entry->kind = strcmp(fields[0], "builtin") == 0
                        ? COMPONENT_BUILTIN
                        : COMPONENT_LOWERED_BUILTIN;
      entry->name = manifest_copy(fields[1]);
      entry->component_id = manifest_copy(fields[2]);
      entry->return_type = manifest_copy(fields[3]);
      entry->parameters = manifest_copy(fields[4]);
      entry->c_symbol = manifest_copy(fields[5]);
      entry->owner = manifest_copy(fields[6]);
    } else if (strcmp(fields[0], "opaque") == 0) {
      if (count != 5) manifest_error(path, line_number, "opaque row needs 5 fields");
      entry->kind = COMPONENT_OPAQUE;
      entry->owner = manifest_copy(fields[1]);
      entry->component_id = manifest_copy(fields[2]);
      entry->constructor = manifest_copy(fields[3]);
      entry->c_symbol = manifest_copy(fields[4]);
      entry->name = manifest_copy(fields[3]);
      entry->return_type = manifest_copy(fields[1]);
      entry->parameters = manifest_copy("");
    } else if (strcmp(fields[0], "method") == 0) {
      if (count != 9) manifest_error(path, line_number, "method row needs 9 fields");
      if (strcmp(fields[2], "instance") == 0)
        entry->kind = COMPONENT_INSTANCE_METHOD;
      else if (strcmp(fields[2], "type") == 0)
        entry->kind = COMPONENT_TYPE_METHOD;
      else
        manifest_error(path, line_number, "method dispatch must be instance or type");
      entry->owner = manifest_copy(fields[1]);
      entry->name = manifest_copy(fields[3]);
      entry->component_id = manifest_copy(fields[4]);
      entry->return_type = manifest_copy(fields[5]);
      entry->parameters = manifest_copy(fields[6]);
      entry->c_symbol = manifest_copy(fields[7]);
      entry->constructor = manifest_copy(fields[8]);
    } else {
      manifest_error(path, line_number, "has an unknown row kind");
    }
  }
  fclose(file);
  validate_manifest(manifest);
  return manifest;
}
