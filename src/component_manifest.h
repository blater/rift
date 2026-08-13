#ifndef COMPONENT_MANIFEST_H
#define COMPONENT_MANIFEST_H

#define COMPONENT_MANIFEST_MAX_COMPONENTS 64
#define COMPONENT_MANIFEST_MAX_INTERFACES 256
#define COMPONENT_MANIFEST_MAX_PARAMS 8

typedef enum component_interface_kind {
  COMPONENT_BUILTIN,
  COMPONENT_LOWERED_BUILTIN,
  COMPONENT_OPAQUE,
  COMPONENT_INSTANCE_METHOD,
  COMPONENT_TYPE_METHOD,
} component_interface_kind;

typedef struct component_spec {
  char *id;
  char *dependencies;
  char *headers;
  char *host_sources;
  char *zxn_sources;
  char *host_asm;
  char *zxn_asm;
  char *init_hook;
  char *shutdown_hook;
  int always;
} component_spec;

typedef struct component_interface_spec {
  component_interface_kind kind;
  char *owner;
  char *name;
  char *component_id;
  char *return_type;
  char *parameters;
  char *c_symbol;
  char *constructor;
} component_interface_spec;

typedef struct component_manifest {
  component_spec components[COMPONENT_MANIFEST_MAX_COMPONENTS];
  int component_count;
  component_interface_spec interfaces[COMPONENT_MANIFEST_MAX_INTERFACES];
  int interface_count;
  char *path;
} component_manifest;

component_manifest *load_component_manifest(const char *path);
component_spec *find_component(component_manifest *manifest, const char *id);
component_interface_spec *find_opaque_interface(component_manifest *manifest,
                                                const char *owner);
component_interface_spec *find_builtin_interface(component_manifest *manifest,
                                                 const char *name, int arity);
component_interface_spec *find_method_interface(component_manifest *manifest,
                                                const char *owner,
                                                const char *name,
                                                int type_level, int arity);
int component_parameter_count(const char *parameters);
const char *component_parameter_at(const char *parameters, int index,
                                   char *buffer, int buffer_size);

#endif
