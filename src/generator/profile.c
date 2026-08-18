#include "profile.h"
#include "stringview.h"

static int stdout_literal_is_simple(token_t tok) {
  string_view text = tok.lexeme;
  if (text.length < 2) return 0;
  for (size_t i = 1; i + 1 < text.length; i++) {
    unsigned char c = (unsigned char)text.data[i];
    if (c == '\\' || c < 32 || c > 126) return 0;
  }
  return 1;
}

static void analyse_node(ast_t node, generator_profile_analysis *analysis) {
  if (!node || !analysis->eligible) return;

  switch (node->tag) {
  case program: {
    ast_array_t nodes = node->data.program.prog;
    for (int i = 0; i < nodes.length; i++)
      analyse_node(nodes.data[i], analysis);
    return;
  }
  case fundef:
    if (node->data.fundef.method_kind != METHOD_NONE ||
        svcmp(node->data.fundef.name.lexeme, sv_from_cstr("main")) != 0) {
      analysis->eligible = 0;
      return;
    }
    analyse_node(node->data.fundef.body, analysis);
    return;
  case compound: {
    ast_array_t nodes = node->data.compound.stmts;
    for (int i = 0; i < nodes.length; i++)
      analyse_node(nodes.data[i], analysis);
    return;
  }
  case funcall: {
    ast_funcall call = node->data.funcall;
    if (svcmp(call.name.lexeme, sv_from_cstr("Sprite")) == 0) {
      if (call.args.length == 1)
        analyse_node(call.args.data[0], analysis);
      else
        analysis->eligible = 0;
      return;
    }
    if (svcmp(call.name.lexeme, sv_from_cstr("print")) != 0 ||
        call.args.length != 1 || call.args.data[0]->tag != literal ||
        call.args.data[0]->data.literal.lit.type != TOK_STR_LIT) {
      analysis->eligible = 0;
      return;
    }
    analysis->uses_stdout = 1;
    if (!stdout_literal_is_simple(call.args.data[0]->data.literal.lit))
      analysis->simple_stdout = 0;
    return;
  }
  case method_call: {
    ast_method_call call = node->data.method_call;
    int tiny_sprite_method =
        call.is_resolved &&
        svcmp(call.resolved_owner, sv_from_cstr("Sprite")) == 0 &&
        (svcmp(call.method.lexeme, sv_from_cstr("position")) == 0 ||
         svcmp(call.method.lexeme, sv_from_cstr("frame")) == 0 ||
         svcmp(call.method.lexeme, sv_from_cstr("show")) == 0 ||
         svcmp(call.method.lexeme, sv_from_cstr("hide")) == 0 ||
         (call.resolved_kind == METHOD_TYPE_LEVEL &&
          svcmp(call.method.lexeme, sv_from_cstr("hideall")) == 0));
    if (!tiny_sprite_method) {
      analysis->eligible = 0;
      return;
    }
    for (int i = 0; i < call.args.length; i++)
      analyse_node(call.args.data[i], analysis);
    return;
  }
  case op:
    analyse_node(node->data.op.left, analysis);
    analyse_node(node->data.op.right, analysis);
    return;
  case unary_op:
    analyse_node(node->data.unary_op.operand, analysis);
    return;
  case literal:
    if (node->data.literal.lit.type == TOK_STR_LIT ||
        node->data.literal.lit.type == TOK_ARR_DECL)
      analysis->eligible = 0;
    return;
  case identifier:
  case asset_decl:
    return;
  case vardef: {
    ast_vardef var = node->data.vardef;
    if (var.is_rec || var.type->data.type.is_array ||
        svcmp(var.type->data.type.name.lexeme, sv_from_cstr("string")) == 0) {
      analysis->eligible = 0;
      return;
    }
    analyse_node(var.expr, analysis);
    return;
  }
  case assign:
    analyse_node(node->data.assign.target, analysis);
    analyse_node(node->data.assign.expr, analysis);
    return;
  case ifstmt:
    analyse_node(node->data.ifstmt.expression, analysis);
    analyse_node(node->data.ifstmt.body, analysis);
    analyse_node(node->data.ifstmt.elsestmt, analysis);
    return;
  case loop:
    analyse_node(node->data.loop.start, analysis);
    analyse_node(node->data.loop.end, analysis);
    analyse_node(node->data.loop.statement, analysis);
    return;
  case while_loop:
    analyse_node(node->data.while_loop.condition, analysis);
    analyse_node(node->data.while_loop.statement, analysis);
    return;
  case ret:
    analyse_node(node->data.ret.expr, analysis);
    return;
  default:
    analysis->eligible = 0;
    return;
  }
}

generator_profile_analysis generator_analyse_profile(target_t target,
                                                      ast_t program) {
  generator_profile_analysis analysis = {1, 0, 1};
  if (target != TARGET_ZXN) {
    analysis.eligible = 0;
    return analysis;
  }
  analyse_node(program, &analysis);
  return analysis;
}
