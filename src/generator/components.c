#include "components.h"
#include "stringview.h"
#include "type_info.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int component_index(generator_t *g, const char *id) {
  if (!g->components || !id) return -1;
  for (int i = 0; i < g->components->component_count; i++)
    if (strcmp(g->components->components[i].id, id) == 0) return i;
  return -1;
}

void record_component(generator_t *g, const char *id) {
  int index = component_index(g, id);
  if (index < 0) {
    fprintf(stderr, "%s: error: required component '%s' is not declared\n",
            g->components->path, id);
    exit(1);
  }
  g->direct_components[index] = 1;
}

void record_fundef_component(generator_t *g, ast_t ref) {
  if (ref && ref->tag == fundef && ref->data.fundef.component_id)
    record_component(g, ref->data.fundef.component_id);
}

static void mark_opaque_type_usage(generator_t *g, ast_t type_node) {
  if (!type_node || type_node->tag != type) return;
  ast_type declared = type_node->data.type;
  char *name = string_of_sv(declared.name.lexeme);
  for (int i = 0; i < g->components->interface_count; i++) {
    component_interface_spec *entry = &g->components->interfaces[i];
    if (entry->kind == COMPONENT_OPAQUE && strcmp(entry->owner, name) == 0) {
      g->opaque_value_used[i] = 1;
      if (declared.is_array) g->opaque_array_used[i] = 1;
      return;
    }
  }
}

static void collect_component_uses(generator_t *g, ast_t node) {
  if (!node) return;
  switch (node->tag) {
  case program:
    for (int i = 0; i < node->data.program.prog.length; i++)
      collect_component_uses(g, node->data.program.prog.data[i]);
    return;
  case fundef:
    mark_opaque_type_usage(g, node->data.fundef.ret_type);
    for (int i = 0; i < node->data.fundef.types.length; i++)
      mark_opaque_type_usage(g, node->data.fundef.types.data[i]);
    collect_component_uses(g, node->data.fundef.body);
    return;
  case compound:
    for (int i = 0; i < node->data.compound.stmts.length; i++)
      collect_component_uses(g, node->data.compound.stmts.data[i]);
    return;
  case funcall: {
    ast_funcall call = node->data.funcall;
    record_fundef_component(g, call.resolved_target);
    if ((!call.resolved_target || call.resolved_target->tag != fundef ||
         call.resolved_target->data.fundef.body == NULL) &&
        (svcmp(call.name.lexeme, sv_from_cstr("putchar")) == 0 ||
         (svcmp(call.name.lexeme, sv_from_cstr("printf")) == 0 &&
          (call.args.length != 1 ||
           !expr_returns_string(call.args.data[0], g->table)))))
      g->zxn_light_core_eligible = 0;
    for (int i = 0; i < call.args.length; i++)
      collect_component_uses(g, call.args.data[i]);
    return;
  }
  case method_call:
    record_fundef_component(g, node->data.method_call.resolved_target);
    collect_component_uses(g, node->data.method_call.receiver);
    for (int i = 0; i < node->data.method_call.args.length; i++)
      collect_component_uses(g, node->data.method_call.args.data[i]);
    return;
  case vardef:
    mark_opaque_type_usage(g, node->data.vardef.type);
    collect_component_uses(g, node->data.vardef.expr);
    return;
  case assign:
    collect_component_uses(g, node->data.assign.target);
    collect_component_uses(g, node->data.assign.expr);
    return;
  case op:
    collect_component_uses(g, node->data.op.left);
    collect_component_uses(g, node->data.op.right);
    return;
  case unary_op:
    collect_component_uses(g, node->data.unary_op.operand);
    return;
  case ret:
    collect_component_uses(g, node->data.ret.expr);
    return;
  case ifstmt:
    collect_component_uses(g, node->data.ifstmt.expression);
    collect_component_uses(g, node->data.ifstmt.body);
    collect_component_uses(g, node->data.ifstmt.elsestmt);
    return;
  case while_loop:
    collect_component_uses(g, node->data.while_loop.condition);
    collect_component_uses(g, node->data.while_loop.statement);
    return;
  case loop:
    collect_component_uses(g, node->data.loop.start);
    collect_component_uses(g, node->data.loop.end);
    collect_component_uses(g, node->data.loop.statement);
    return;
  case iter_loop:
    collect_component_uses(g, node->data.iter_loop.iterable);
    collect_component_uses(g, node->data.iter_loop.statement);
    return;
  case sub:
    collect_component_uses(g, node->data.sub.receiver);
    collect_component_uses(g, node->data.sub.expr);
    return;
  case arr_index:
    collect_component_uses(g, node->data.arr_index.array);
    collect_component_uses(g, node->data.arr_index.index);
    collect_component_uses(g, node->data.arr_index.field_expr);
    return;
  case record_expr:
    for (int i = 0; i < node->data.record_expr.exprs.length; i++)
      collect_component_uses(g, node->data.record_expr.exprs.data[i]);
    return;
  case match:
    collect_component_uses(g, node->data.match.expr);
    for (int i = 0; i < node->data.match.cases.length; i++)
      collect_component_uses(g, node->data.match.cases.data[i]);
    return;
  case matchcase:
    collect_component_uses(g, node->data.matchcase.expr);
    collect_component_uses(g, node->data.matchcase.body);
    return;
  case tdef:
    for (int i = 0; i < node->data.tdef.module_fields.length; i++)
      collect_component_uses(g, node->data.tdef.module_fields.data[i]);
    for (int i = 0; i < node->data.tdef.constructors.length; i++) {
      ast_t constructor = node->data.tdef.constructors.data[i];
      if (constructor && constructor->tag == cons)
        mark_opaque_type_usage(g, constructor->data.cons.type);
    }
    return;
  case embed:
    g->zxn_light_core_eligible = 0;
    return;
  default:
    return;
  }
}

void generator_collect_component_uses(generator_t *g, ast_t program) {
  collect_component_uses(g, program);
}

static void close_component(generator_t *g, int index,
                            unsigned char *visiting) {
  if (g->closed_components[index]) return;
  if (visiting[index]) {
    fprintf(stderr, "%s: error: component dependency cycle includes '%s'\n",
            g->components->path, g->components->components[index].id);
    exit(1);
  }
  visiting[index] = 1;
  component_spec *component = &g->components->components[index];
  int dependency_count = component_parameter_count(component->dependencies);
  for (int i = 0; i < dependency_count; i++) {
    char dependency[128];
    component_parameter_at(component->dependencies, i, dependency,
                           sizeof(dependency));
    int dependency_index = component_index(g, dependency);
    if (dependency_index < 0) {
      fprintf(stderr, "%s: error: unknown component dependency '%s'\n",
              g->components->path, dependency);
      exit(1);
    }
    close_component(g, dependency_index, visiting);
  }
  visiting[index] = 0;
  g->closed_components[index] = 1;
  g->component_order[g->component_order_count++] = index;
}

void generator_compute_component_closure(generator_t *g) {
  unsigned char visiting[COMPONENT_MANIFEST_MAX_COMPONENTS] = {0};
  for (int i = 0; i < g->components->component_count; i++) {
    int always = g->components->components[i].always;
    if (g->zxn_tiny_eligible &&
        strcmp(g->components->components[i].id, "core") == 0)
      always = 0;
    if (g->select_all_components || always)
      g->direct_components[i] = 1;
  }
  for (int i = 0; i < g->components->component_count; i++)
    if (g->direct_components[i]) close_component(g, i, visiting);
}

void generator_write_component_output(generator_t *g) {
  FILE *file = fopen(g->component_output_path, "wb");
  if (!file) {
    fprintf(stderr, "error: cannot write component output '%s'\n",
            g->component_output_path);
    exit(1);
  }
  fprintf(file, "RIFT_COMPONENTS_V1\n");
  fprintf(file, "@profile=%s\n",
          g->zxn_tiny_eligible
              ? (g->zxn_tiny_uses_stdout
                     ? (g->zxn_tiny_simple_stdout ? "tiny-31"
                                                  : "tiny-console-31")
                     : "tiny-31")
              : (g->zxn_light_core_eligible ? "core-31" : "full"));
  for (int i = 0; i < g->component_order_count; i++)
    fprintf(file, "%s\n",
            g->components->components[g->component_order[i]].id);
  fclose(file);
}

void generator_emit_manifest_headers(generator_t *g) {
  for (int i = 0; i < g->components->component_count; i++) {
    component_spec *component = &g->components->components[i];
    int header_count = component_parameter_count(component->headers);
    for (int j = 0; j < header_count; j++) {
      char header[256];
      component_parameter_at(component->headers, j, header, sizeof(header));
      fprintf(g->f, "#include \"%s\"\n", header);
    }
  }
}

void generator_emit_component_init(generator_t *g) {
  for (int i = 0; i < g->component_order_count; i++) {
    component_spec *component =
        &g->components->components[g->component_order[i]];
    if (component->init_hook[0] != '\0')
      fprintf(g->f, "%s();\n", component->init_hook);
  }
}

void generator_emit_component_shutdown(generator_t *g) {
  for (int i = g->component_order_count - 1; i >= 0; i--) {
    component_spec *component =
        &g->components->components[g->component_order[i]];
    if (component->shutdown_hook[0] != '\0')
      fprintf(g->f, "%s();\n", component->shutdown_hook);
  }
}
