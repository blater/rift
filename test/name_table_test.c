#include "ast.h"
#include "lib/alloc.h"
#include "name_table.h"
#include "stringview.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void check(int condition, const char *description) {
  if (condition) {
    printf("PASS: %s\n", description);
  } else {
    printf("FAIL: %s\n", description);
    failures++;
  }
}

static ast_t marker(int value) {
  node_t node = {0};
  node.tag = literal;
  node.data.literal.lit.line = value;
  return new_ast(node);
}

int main(void) {
  init_compiler_stack();
  name_table_t table = new_name_table();
  string_view shared = sv_from_cstr("Shared");
  ast_t type_ref = marker(1);
  ast_t outer_ref = marker(2);
  ast_t inner_ref = marker(3);

  check(!lookup_nt(sv_from_cstr("missing"), table).found,
        "missing lookup has an explicit not-found result");
  check(get_nt_kind(sv_from_cstr("missing"), table) == NT_NOT_FOUND,
        "missing lookup is not reported as a variable");

  push_nt(&table, shared, NT_USER_TYPE, type_ref);
  push_nt(&table, shared, NT_VAR, outer_ref);
  check(get_ref_by_kind(shared, NT_USER_TYPE, table) == type_ref,
        "kind lookup finds equal-spelled type");
  check(get_ref_by_kind(shared, NT_VAR, table) == outer_ref,
        "kind lookup finds equal-spelled value");

  new_nt_scope(&table);
  push_nt(&table, shared, NT_VAR, inner_ref);
  check(get_ref(shared, table) == inner_ref,
        "inner value shadows outer value");
  check(has_nt_in_current_scope(shared, NT_VAR, table),
        "current-scope lookup sees inner value");
  check(!push_nt_unique(&table, shared, NT_VAR, marker(4)),
        "unique insertion rejects a duplicate kind in one scope");
  check(push_nt_unique(&table, shared, NT_FUN, marker(5)),
        "unique insertion permits the same spelling in another namespace");
  end_nt_scope(&table);
  check(get_ref_by_kind(shared, NT_VAR, table) == outer_ref,
        "ending scope restores outer value");
  check(get_ref_by_kind(shared, NT_USER_TYPE, table) == type_ref,
        "ending value scope preserves equal-spelled type");

  for (int i = 0; i < 1100; i++) {
    char text[32];
    snprintf(text, sizeof(text), "entry_%d", i);
    char *owned = allocate_compiler_persistent(strlen(text) + 1);
    strcpy(owned, text);
    push_nt(&table, sv_from_cstr(owned), NT_VAR, marker(i + 10));
  }
  check(lookup_nt(sv_from_cstr("entry_0"), table).found,
        "first entry survives table growth");
  check(lookup_nt(sv_from_cstr("entry_1099"), table).found,
        "last entry survives table growth");
  check(lookup_nt(sv_from_cstr("entry_700"), table).ref->data.literal.lit.line ==
            710,
        "references survive table growth");

  kill_compiler_stack();
  return failures == 0 ? 0 : 1;
}
