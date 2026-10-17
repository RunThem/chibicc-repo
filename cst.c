#include "chibicc.h"

// Lossless concrete syntax tree over the raw token stream (PLAN.md stage
// C1/C2.1). Every token of the input becomes one leaf, in order; the
// leaves' trivia and spellings rebuild the input file byte for byte.
//
// The skeleton groups delimiters and splits statements. At statement
// level (the top level and inside a block) an item is a ';'-terminated
// statement, a lone ';' (CST_NULL_STMT), or a '{...}' body (CST_BLOCK).
// An expression statement whose whole span parses is a CST_EXPR_STMT;
// anything the expression grammar cannot cover becomes CST_UNPARSED,
// holding the raw skeleton children (tokens up to ';' with nested
// delimiters grouped, semicolons in a for-header left as leaves). A
// group or block without its closing token, and a closer that has no
// opener at this level, are CST_UNPARSED too; the tokens they already
// hold stay in the tree.
//
// The expression grammar is the C precedence chain up to and including
// the comma operator. Every operator, parenthesis and bracket is an
// explicit leaf; nodes only group. A '(' whose first token starts a type
// name (a type keyword, one of struct/union/enum/typeof/_Atomic, or a
// typedef name seen earlier in this file) is a cast or a compound
// literal, otherwise a parenthesized expression. There is no name
// lookup, no type computation and no constant folding: a file-local
// typedef set plus spelling is the whole oracle, and a parse that cannot
// decide falls back to the skeleton rather than guessing.
//
// The lossless contract does not depend on the shape: the parser is
// total, never reports a diagnostic and never exits (RESULT.md I1-I6).

static CstNode *new_cst_node(CstKind kind) {
  CstNode *node = calloc(1, sizeof(CstNode));
  node->kind = kind;
  return node;
}

static void cst_add_child(CstNode *node, CstNode *child) {
  if (node->child_count == node->child_cap) {
    node->child_cap = node->child_cap ? node->child_cap * 2 : 8;
    node->children = realloc(node->children,
                             sizeof(CstNode *) * node->child_cap);
  }
  node->children[node->child_count++] = child;
}

static CstNode *cst_add_leaf(CstNode *parent, Token *tok) {
  CstNode *leaf = new_cst_node(tok->kind == TK_EOF ? CST_EOF : CST_TOKEN);
  leaf->tok = tok;
  cst_add_child(parent, leaf);
  return leaf;
}

static bool cst_is_open(Token *tok) {
  return equal(tok, "(") || equal(tok, "[") || equal(tok, "{");
}

static bool cst_is_close(Token *tok) {
  return equal(tok, ")") || equal(tok, "]") || equal(tok, "}");
}

// Parse one delimited group starting at `tok` (one of '(' '[' '{'), in
// raw mode: the interior is not split into statements or expressions,
// only nested delimiters are grouped. On a missing closer the node
// becomes CST_UNPARSED but keeps the opener and everything it consumed.
static CstNode *cst_parse_group(Token **rest, Token *tok) {
  char *closer = equal(tok, "(") ? ")" : equal(tok, "[") ? "]" : "}";
  CstKind kind = equal(tok, "(") ? CST_GROUP_PAREN
               : equal(tok, "[") ? CST_GROUP_BRACKET
               : CST_GROUP_BRACE;

  CstNode *group = new_cst_node(kind);
  cst_add_leaf(group, tok);

  Token *p = tok->next;
  while (p && p->kind != TK_EOF && !equal(p, closer)) {
    if (cst_is_open(p)) {
      cst_add_child(group, cst_parse_group(&p, p));
      continue;
    }
    cst_add_leaf(group, p);
    p = p->next;
  }

  if (p && p->kind != TK_EOF && equal(p, closer)) {
    cst_add_leaf(group, p);
    p = p->next;
  } else {
    group->kind = CST_UNPARSED;
  }
  *rest = p;
  return group;
}

//
// Expression parser (C2.1)
//
// A backtracking recursive descent over the pre-preprocessing token
// stream. `ok` turns false at the first failure and no caller looks at
// `tok` again; the statement layer then rebuilds the span with the raw
// skeleton, which is why a failed attempt may leave unreachable nodes in
// the arena without duplicating a token in the tree.
//

typedef struct {
  StringArray typedefs; // Names introduced by `typedef` earlier in the file
} CstContext;

typedef struct {
  Token *tok;
  bool ok;
  CstContext *ctx;
} CstParser;

static void cst_fail(CstParser *p) {
  p->ok = false;
}

static bool cst_at(CstParser *p, char *op) {
  return p->tok && p->tok->kind != TK_EOF && equal(p->tok, op);
}

static bool cst_in_list(Token *tok, char **list) {
  for (int i = 0; list[i]; i++)
    if (equal(tok, list[i]))
      return true;
  return false;
}

// Words that cannot begin an expression operand: C11 keywords, the
// operator-like keywords, and the common GNU spellings. In the raw
// stream keywords are still TK_IDENT, so this is a spelling test.
static bool cst_is_keyword(Token *tok) {
  char *kw[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if",
    "inline", "int", "long", "register", "restrict", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
    "unsigned", "void", "volatile", "while",
    "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic",
    "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local",
    "typeof", "__typeof__", "__typeof", "alignof", "__alignof__",
    "__alignof", "asm", "__asm__", "__attribute__", "__extension__",
    NULL,
  };
  return cst_in_list(tok, kw);
}

// True if `tok` can start a type name in a cast or sizeof: a type
// specifier / qualifier keyword, or a file-local typedef name.
static bool cst_is_type_start(Token *tok, CstContext *ctx) {
  char *kw[] = {
    "void", "_Bool", "char", "short", "int", "long", "float", "double",
    "signed", "unsigned", "_Complex", "_Imaginary", "const", "volatile",
    "restrict", "struct", "union", "enum", "typeof", "__typeof__",
    "__typeof", "_Atomic", "__signed__", "__const__", "__volatile__",
    "__restrict", "__restrict__",
    NULL,
  };
  if (cst_in_list(tok, kw))
    return true;

  for (int i = 0; i < ctx->typedefs.len; i++)
    if (equal(tok, ctx->typedefs.data[i]))
      return true;
  return false;
}

static void cst_add_typedef(CstContext *ctx, Token *tok) {
  strarray_push(&ctx->typedefs, strndup(tok->loc, tok->len));
}

// A conservative pre-scan for file-local typedef names. Names introduced
// by `typedef int T;`, `typedef int *T;`, `typedef int T[3];`,
// `typedef int A, B;`, `typedef struct S T;` and `typedef int (*T)(int);`
// are collected; parameter names and struct tags are not. This is the
// cast / declaration oracle the plan allows - an include-provided name
// stays unknown and is not guessed.
static void cst_collect_typedefs(CstContext *ctx, Token *tok) {
  Token *p = tok;

  while (p && p->kind != TK_EOF) {
    if (p->kind != TK_IDENT || !equal(p, "typedef")) {
      p = p->next;
      continue;
    }

    int depth = 0;
    Token *prev = p;
    Token *q = p->next;

    for (; q && q->kind != TK_EOF; prev = q, q = q->next) {
      if (equal(q, "(") || equal(q, "[") || equal(q, "{")) {
        depth++;
        continue;
      }
      if (equal(q, ")") || equal(q, "]") || equal(q, "}")) {
        depth--;
        continue;
      }
      if (depth == 0 && equal(q, ";"))
        break;
      if (q->kind != TK_IDENT)
        continue;

      if (depth == 0 &&
          (equal(q->next, ";") || equal(q->next, ",") || equal(q->next, "[")))
        cst_add_typedef(ctx, q);
      else if (depth == 1 && equal(prev, "*") && equal(q->next, ")"))
        cst_add_typedef(ctx, q);
    }

    p = q ? q : p->next;
  }
}

static CstNode *cst_parse_comma(CstParser *p);
static CstNode *cst_parse_assign(CstParser *p);
static CstNode *cst_parse_conditional(CstParser *p);
static CstNode *cst_parse_unary(CstParser *p);
static CstNode *cst_parse_postfix(CstParser *p);
static CstNode *cst_parse_postfix_tail(CstParser *p, CstNode *node);
static CstNode *cst_parse_primary(CstParser *p);
static CstNode *cst_parse_arg_list(CstParser *p);
static CstNode *cst_parse_init_list(CstParser *p);

// Consume one balanced group in raw mode, for a type name in a cast or
// sizeof. An unclosed group fails the expression: the statement falls
// back to the skeleton, where the span becomes UNPARSED.
static CstNode *cst_parse_raw_group(CstParser *p) {
  Token *rest = NULL;
  CstNode *group = cst_parse_group(&rest, p->tok);

  if (group->kind == CST_UNPARSED) {
    cst_fail(p);
    return NULL;
  }
  p->tok = rest;
  return group;
}

// primary = ident | number | char | string+ | "(" expr ")"
static CstNode *cst_parse_primary(CstParser *p) {
  if (!p->ok)
    return NULL;

  Token *tok = p->tok;
  if (!tok || tok->kind == TK_EOF) {
    cst_fail(p);
    return NULL;
  }

  if (equal(tok, "(")) {
    CstNode *node = new_cst_node(CST_PAREN);
    cst_add_leaf(node, tok);

    p->tok = tok->next;
    CstNode *inner = cst_parse_comma(p);
    if (!p->ok || !cst_at(p, ")")) {
      cst_fail(p);
      return NULL;
    }

    cst_add_child(node, inner);
    cst_add_leaf(node, p->tok);
    p->tok = p->tok->next;
    return node;
  }

  if (tok->kind == TK_PUNCT || cst_is_keyword(tok)) {
    cst_fail(p);
    return NULL;
  }

  // Identifiers (keywords are still identifiers here), numbers, char and
  // string literals. Adjacent string literals are one primary; each stays
  // an explicit leaf.
  CstNode *node = new_cst_node(CST_PRIMARY);
  cst_add_leaf(node, tok);
  p->tok = tok->next;

  while (p->tok && p->tok->kind == TK_STR) {
    cst_add_leaf(node, p->tok);
    p->tok = p->tok->next;
  }
  return node;
}

// arg-list = "(" (assign ("," assign)*)? ")"
static CstNode *cst_parse_arg_list(CstParser *p) {
  CstNode *args = new_cst_node(CST_ARG_LIST);
  cst_add_leaf(args, p->tok); // '('
  p->tok = p->tok->next;

  if (!cst_at(p, ")")) {
    for (;;) {
      CstNode *arg = cst_parse_assign(p);
      if (!p->ok)
        return NULL;
      cst_add_child(args, arg);

      if (!cst_at(p, ","))
        break;
      cst_add_leaf(args, p->tok);
      p->tok = p->tok->next;
    }
  }

  if (!cst_at(p, ")")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(args, p->tok);
  p->tok = p->tok->next;
  return args;
}

// init-list = "{" (initializer ("," initializer)* ","?)? "}"
// Designators are C2.2's; here an element is an assignment expression or
// a nested list.
static CstNode *cst_parse_init_list(CstParser *p) {
  CstNode *list = new_cst_node(CST_INIT_LIST);
  cst_add_leaf(list, p->tok); // '{'
  p->tok = p->tok->next;

  if (cst_at(p, "}")) {
    cst_add_leaf(list, p->tok);
    p->tok = p->tok->next;
    return list;
  }

  for (;;) {
    CstNode *elem = cst_at(p, "{") ? cst_parse_init_list(p)
                                   : cst_parse_assign(p);
    if (!p->ok)
      return NULL;
    cst_add_child(list, elem);

    if (!cst_at(p, ","))
      break;
    cst_add_leaf(list, p->tok);
    p->tok = p->tok->next;
    if (cst_at(p, "}"))
      break;
  }

  if (!cst_at(p, "}")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(list, p->tok);
  p->tok = p->tok->next;
  return list;
}

// postfix-tail = ("(" arg-list | "[" expr "]" | "." ident | "->" ident
//                | "++" | "--")*
static CstNode *cst_parse_postfix_tail(CstParser *p, CstNode *node) {
  for (;;) {
    if (!p->ok)
      return NULL;

    if (cst_at(p, "(")) {
      CstNode *args = cst_parse_arg_list(p);
      if (!p->ok)
        return NULL;
      CstNode *call = new_cst_node(CST_POSTFIX);
      cst_add_child(call, node);
      cst_add_child(call, args);
      node = call;
      continue;
    }

    if (cst_at(p, "[")) {
      Token *open = p->tok;
      p->tok = open->next;
      CstNode *idx = cst_parse_comma(p);
      if (!p->ok || !cst_at(p, "]")) {
        cst_fail(p);
        return NULL;
      }

      CstNode *sub = new_cst_node(CST_POSTFIX);
      cst_add_child(sub, node);
      cst_add_leaf(sub, open);
      cst_add_child(sub, idx);
      cst_add_leaf(sub, p->tok);
      p->tok = p->tok->next;
      node = sub;
      continue;
    }

    if (cst_at(p, ".") || cst_at(p, "->")) {
      Token *op = p->tok;
      Token *name = op->next;
      if (!name || name->kind != TK_IDENT) {
        cst_fail(p);
        return NULL;
      }

      CstNode *member = new_cst_node(CST_POSTFIX);
      cst_add_child(member, node);
      cst_add_leaf(member, op);
      cst_add_leaf(member, name);
      p->tok = name->next;
      node = member;
      continue;
    }

    if (cst_at(p, "++") || cst_at(p, "--")) {
      CstNode *inc = new_cst_node(CST_POSTFIX);
      cst_add_child(inc, node);
      cst_add_leaf(inc, p->tok);
      p->tok = p->tok->next;
      node = inc;
      continue;
    }

    return node;
  }
}

// postfix = primary postfix-tail
static CstNode *cst_parse_postfix(CstParser *p) {
  CstNode *node = cst_parse_primary(p);
  if (!p->ok)
    return NULL;
  return cst_parse_postfix_tail(p, node);
}

// unary = ("+" | "-" | "!" | "~" | "*" | "&" | "&&" | "++" | "--") unary
//       | ("sizeof" | "_Alignof" | "typeof") (type-name | unary)
//       | "(" type-name ")" unary                    // cast
//       | "(" type-name ")" "{" init-list "}"        // compound literal
//       | postfix
static CstNode *cst_parse_unary(CstParser *p) {
  if (!p->ok)
    return NULL;

  if (cst_at(p, "sizeof") || cst_at(p, "_Alignof") || cst_at(p, "alignof") ||
      cst_at(p, "__alignof__") || cst_at(p, "__alignof") ||
      cst_at(p, "typeof") || cst_at(p, "__typeof__") || cst_at(p, "__typeof")) {
    Token *op = p->tok;
    p->tok = op->next;

    CstNode *operand = (cst_at(p, "(") && p->tok->next &&
                        cst_is_type_start(p->tok->next, p->ctx))
                         ? cst_parse_raw_group(p)
                         : cst_parse_unary(p);
    if (!p->ok)
      return NULL;

    CstNode *node = new_cst_node(CST_UNARY);
    cst_add_leaf(node, op);
    cst_add_child(node, operand);
    return node;
  }

  if (cst_at(p, "+") || cst_at(p, "-") || cst_at(p, "!") || cst_at(p, "~") ||
      cst_at(p, "*") || cst_at(p, "&") || cst_at(p, "&&") ||
      cst_at(p, "++") || cst_at(p, "--")) {
    Token *op = p->tok;
    p->tok = op->next;

    CstNode *operand = cst_parse_unary(p);
    if (!p->ok)
      return NULL;

    CstNode *node = new_cst_node(CST_UNARY);
    cst_add_leaf(node, op);
    cst_add_child(node, operand);
    return node;
  }

  if (cst_at(p, "(") && p->tok->next &&
      cst_is_type_start(p->tok->next, p->ctx)) {
    CstNode *type = cst_parse_raw_group(p);
    if (!p->ok)
      return NULL;

    if (cst_at(p, "{")) {
      CstNode *init = cst_parse_init_list(p);
      if (!p->ok)
        return NULL;
      CstNode *lit = new_cst_node(CST_COMPOUND_LITERAL);
      cst_add_child(lit, type);
      cst_add_child(lit, init);
      return cst_parse_postfix_tail(p, lit);
    }

    CstNode *operand = cst_parse_unary(p);
    if (!p->ok)
      return NULL;

    CstNode *cast = new_cst_node(CST_CAST);
    cst_add_child(cast, type);
    cst_add_child(cast, operand);
    return cast;
  }

  return cst_parse_postfix(p);
}

// binary-level <name> = <operand-level> (op <operand-level>)*, left
// associative. Each level keeps the operator as an explicit leaf.

static CstNode *cst_parse_mul(CstParser *p) {
  CstNode *node = cst_parse_unary(p);
  while (p->ok && (cst_at(p, "*") || cst_at(p, "/") || cst_at(p, "%"))) {
    CstNode *bin = new_cst_node(CST_BINARY);
    cst_add_child(bin, node);
    cst_add_leaf(bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_unary(p);
    if (!p->ok)
      return NULL;
    cst_add_child(bin, rhs);
    node = bin;
  }
  return node;
}

static CstNode *cst_parse_add(CstParser *p) {
  CstNode *node = cst_parse_mul(p);
  while (p->ok && (cst_at(p, "+") || cst_at(p, "-"))) {
    CstNode *bin = new_cst_node(CST_BINARY);
    cst_add_child(bin, node);
    cst_add_leaf(bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_mul(p);
    if (!p->ok)
      return NULL;
    cst_add_child(bin, rhs);
    node = bin;
  }
  return node;
}

static CstNode *cst_parse_shift(CstParser *p) {
  CstNode *node = cst_parse_add(p);
  while (p->ok && (cst_at(p, "<<") || cst_at(p, ">>"))) {
    CstNode *bin = new_cst_node(CST_BINARY);
    cst_add_child(bin, node);
    cst_add_leaf(bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_add(p);
    if (!p->ok)
      return NULL;
    cst_add_child(bin, rhs);
    node = bin;
  }
  return node;
}

static CstNode *cst_parse_relational(CstParser *p) {
  CstNode *node = cst_parse_shift(p);
  while (p->ok && (cst_at(p, "<") || cst_at(p, "<=") ||
                   cst_at(p, ">") || cst_at(p, ">="))) {
    CstNode *bin = new_cst_node(CST_BINARY);
    cst_add_child(bin, node);
    cst_add_leaf(bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_shift(p);
    if (!p->ok)
      return NULL;
    cst_add_child(bin, rhs);
    node = bin;
  }
  return node;
}

static CstNode *cst_parse_equality(CstParser *p) {
  CstNode *node = cst_parse_relational(p);
  while (p->ok && (cst_at(p, "==") || cst_at(p, "!="))) {
    CstNode *bin = new_cst_node(CST_BINARY);
    cst_add_child(bin, node);
    cst_add_leaf(bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_relational(p);
    if (!p->ok)
      return NULL;
    cst_add_child(bin, rhs);
    node = bin;
  }
  return node;
}

static CstNode *cst_parse_bitand(CstParser *p) {
  CstNode *node = cst_parse_equality(p);
  while (p->ok && cst_at(p, "&")) {
    CstNode *bin = new_cst_node(CST_BINARY);
    cst_add_child(bin, node);
    cst_add_leaf(bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_equality(p);
    if (!p->ok)
      return NULL;
    cst_add_child(bin, rhs);
    node = bin;
  }
  return node;
}

static CstNode *cst_parse_bitxor(CstParser *p) {
  CstNode *node = cst_parse_bitand(p);
  while (p->ok && cst_at(p, "^")) {
    CstNode *bin = new_cst_node(CST_BINARY);
    cst_add_child(bin, node);
    cst_add_leaf(bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_bitand(p);
    if (!p->ok)
      return NULL;
    cst_add_child(bin, rhs);
    node = bin;
  }
  return node;
}

static CstNode *cst_parse_bitor(CstParser *p) {
  CstNode *node = cst_parse_bitxor(p);
  while (p->ok && cst_at(p, "|")) {
    CstNode *bin = new_cst_node(CST_BINARY);
    cst_add_child(bin, node);
    cst_add_leaf(bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_bitxor(p);
    if (!p->ok)
      return NULL;
    cst_add_child(bin, rhs);
    node = bin;
  }
  return node;
}

static CstNode *cst_parse_logand(CstParser *p) {
  CstNode *node = cst_parse_bitor(p);
  while (p->ok && cst_at(p, "&&")) {
    CstNode *bin = new_cst_node(CST_BINARY);
    cst_add_child(bin, node);
    cst_add_leaf(bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_bitor(p);
    if (!p->ok)
      return NULL;
    cst_add_child(bin, rhs);
    node = bin;
  }
  return node;
}

static CstNode *cst_parse_logor(CstParser *p) {
  CstNode *node = cst_parse_logand(p);
  while (p->ok && cst_at(p, "||")) {
    CstNode *bin = new_cst_node(CST_BINARY);
    cst_add_child(bin, node);
    cst_add_leaf(bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_logand(p);
    if (!p->ok)
      return NULL;
    cst_add_child(bin, rhs);
    node = bin;
  }
  return node;
}

// conditional = logor ("?" expr ":" conditional | "?" ":" conditional)?
static CstNode *cst_parse_conditional(CstParser *p) {
  CstNode *cond = cst_parse_logor(p);
  if (!p->ok || !cst_at(p, "?"))
    return cond;

  CstNode *node = new_cst_node(CST_COND);
  cst_add_child(node, cond);
  cst_add_leaf(node, p->tok); // '?'
  p->tok = p->tok->next;

  // [GNU] `a ?: b` has no middle operand.
  if (cst_at(p, ":")) {
    cst_add_leaf(node, p->tok);
    p->tok = p->tok->next;
    CstNode *els = cst_parse_conditional(p);
    if (!p->ok)
      return NULL;
    cst_add_child(node, els);
    return node;
  }

  CstNode *then = cst_parse_comma(p);
  if (!p->ok || !cst_at(p, ":")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_child(node, then);
  cst_add_leaf(node, p->tok); // ':'
  p->tok = p->tok->next;

  CstNode *els = cst_parse_conditional(p);
  if (!p->ok)
    return NULL;
  cst_add_child(node, els);
  return node;
}

static bool cst_at_assign_op(CstParser *p) {
  char *ops[] = {
    "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
    NULL,
  };
  return p->tok && p->tok->kind != TK_EOF && cst_in_list(p->tok, ops);
}

// assign = conditional (assign-op assign)?
static CstNode *cst_parse_assign(CstParser *p) {
  CstNode *lhs = cst_parse_conditional(p);
  if (!p->ok || !cst_at_assign_op(p))
    return lhs;

  CstNode *node = new_cst_node(CST_ASSIGN);
  cst_add_child(node, lhs);
  cst_add_leaf(node, p->tok);
  p->tok = p->tok->next;

  CstNode *rhs = cst_parse_assign(p);
  if (!p->ok)
    return NULL;
  cst_add_child(node, rhs);
  return node;
}

// comma = assign ("," assign)*
static CstNode *cst_parse_comma(CstParser *p) {
  CstNode *node = cst_parse_assign(p);
  while (p->ok && cst_at(p, ",")) {
    CstNode *comma = new_cst_node(CST_COMMA);
    cst_add_child(comma, node);
    cst_add_leaf(comma, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_assign(p);
    if (!p->ok)
      return NULL;
    cst_add_child(comma, rhs);
    node = comma;
  }
  return node;
}

//
// Statement layer
//

// Parse one statement into the fallback shape: tokens up to and
// including a ';' at this level, a raw group per delimiter, and stop
// before a closer that belongs to an outer level. A span cut short by a
// closer (or EOF) keeps the tokens it has; the skeleton does not guess
// where it should have ended. The node is CST_UNPARSED: the expression
// grammar did not explain the span (a declaration is one such span until
// C2.2), and the children are exactly the raw pieces, not a guess.
static CstNode *cst_parse_fallback_statement(Token **rest, Token *tok) {
  CstNode *stmt = new_cst_node(CST_UNPARSED);

  Token *p = tok;
  while (p && p->kind != TK_EOF) {
    if (equal(p, ";")) {
      cst_add_leaf(stmt, p);
      p = p->next;
      break;
    }
    if (cst_is_close(p))
      break;
    if (cst_is_open(p)) {
      cst_add_child(stmt, cst_parse_group(&p, p));
      continue;
    }
    cst_add_leaf(stmt, p);
    p = p->next;
  }
  *rest = p;
  return stmt;
}

// A statement whose whole span is an expression followed by ';' becomes
// CST_EXPR_STMT. On any failure the caller uses the raw skeleton instead;
// the failed attempt's nodes are unreachable, which is allowed.
static CstNode *cst_try_expr_stmt(Token **rest, Token *tok, CstContext *ctx) {
  CstParser p = {tok, true, ctx};

  CstNode *expr = cst_parse_comma(&p);
  if (!p.ok || !equal(p.tok, ";"))
    return NULL;

  CstNode *stmt = new_cst_node(CST_EXPR_STMT);
  cst_add_child(stmt, expr);
  cst_add_leaf(stmt, p.tok);
  *rest = p.tok->next;
  return stmt;
}

static CstNode *cst_parse_statement(Token **rest, Token *tok, CstContext *ctx) {
  CstNode *stmt = cst_try_expr_stmt(rest, tok, ctx);
  if (stmt)
    return stmt;
  return cst_parse_fallback_statement(rest, tok);
}

static CstNode *cst_parse_block(Token **rest, Token *tok, CstContext *ctx) {
  CstNode *block = new_cst_node(CST_BLOCK);
  cst_add_leaf(block, tok); // '{'

  Token *p = tok->next;
  for (;;) {
    if (!p || p->kind == TK_EOF)
      break;
    if (equal(p, "}"))
      break;
    if (cst_is_close(p)) {
      // A closer of another delimiter at this level has no opener here.
      CstNode *unparsed = new_cst_node(CST_UNPARSED);
      cst_add_leaf(unparsed, p);
      cst_add_child(block, unparsed);
      p = p->next;
      continue;
    }
    if (equal(p, ";")) {
      CstNode *null = new_cst_node(CST_NULL_STMT);
      cst_add_leaf(null, p);
      cst_add_child(block, null);
      p = p->next;
      continue;
    }
    if (equal(p, "{")) {
      cst_add_child(block, cst_parse_block(&p, p, ctx));
      continue;
    }
    cst_add_child(block, cst_parse_statement(&p, p, ctx));
  }

  if (p && p->kind != TK_EOF && equal(p, "}")) {
    cst_add_leaf(block, p);
    p = p->next;
  } else {
    block->kind = CST_UNPARSED;
  }
  *rest = p;
  return block;
}

// Split the top level into items. The EOF token is left for cst_parse so
// that it stays the tree's last leaf whatever the items do.
static void cst_parse_items(CstNode *parent, Token **rest, CstContext *ctx) {
  Token *p = *rest;
  for (;;) {
    if (!p || p->kind == TK_EOF)
      break;
    if (cst_is_close(p)) {
      CstNode *unparsed = new_cst_node(CST_UNPARSED);
      cst_add_leaf(unparsed, p);
      cst_add_child(parent, unparsed);
      p = p->next;
      continue;
    }
    if (equal(p, ";")) {
      CstNode *null = new_cst_node(CST_NULL_STMT);
      cst_add_leaf(null, p);
      cst_add_child(parent, null);
      p = p->next;
      continue;
    }
    if (equal(p, "{")) {
      cst_add_child(parent, cst_parse_block(&p, p, ctx));
      continue;
    }
    cst_add_child(parent, cst_parse_statement(&p, p, ctx));
  }
  *rest = p;
}

// Build the CST for tokenize_file()'s output. The EOF marker has its own
// kind because it carries the file's trailing trivia and no spelling.
CstNode *cst_parse(Token *tok) {
  CstContext ctx = {};
  cst_collect_typedefs(&ctx, tok);

  CstNode *root = new_cst_node(CST_TRANSLATION_UNIT);
  Token *p = tok;
  cst_parse_items(root, &p, &ctx);

  if (p && p->kind == TK_EOF)
    cst_add_leaf(root, p);
  return root;
}

static void cst_reconstruct_node(CstNode *node, FILE *out) {
  if (node->tok) {
    fwrite(node->tok->trivia, 1, node->tok->trivia_len, out);
    fwrite(node->tok->raw_loc, 1, node->tok->raw_len, out);
    return;
  }

  for (int i = 0; i < node->child_count; i++)
    cst_reconstruct_node(node->children[i], out);
}

// Write the raw bytes of the tree's leaves in order (trivia then spelling
// for every token). On tokenize_file()'s direct output this equals the
// input file's raw text.
void cst_reconstruct(CstNode *root, FILE *out) {
  cst_reconstruct_node(root, out);
}

static char *cst_kind_name(CstKind kind) {
  switch (kind) {
  case CST_TRANSLATION_UNIT: return "TU";
  case CST_TOKEN:            return "TOKEN";
  case CST_EOF:              return "EOF";
  case CST_GROUP_PAREN:      return "GROUP_PAREN";
  case CST_GROUP_BRACKET:    return "GROUP_BRACKET";
  case CST_GROUP_BRACE:      return "GROUP_BRACE";
  case CST_BLOCK:            return "BLOCK";
  case CST_NULL_STMT:        return "NULL_STMT";
  case CST_UNPARSED:         return "UNPARSED";
  case CST_EXPR_STMT:        return "EXPR_STMT";
  case CST_BINARY:           return "BINARY";
  case CST_UNARY:            return "UNARY";
  case CST_POSTFIX:          return "POSTFIX";
  case CST_CAST:             return "CAST";
  case CST_COND:             return "COND";
  case CST_ASSIGN:           return "ASSIGN";
  case CST_COMMA:            return "COMMA";
  case CST_PAREN:            return "PAREN";
  case CST_PRIMARY:          return "PRIMARY";
  case CST_ARG_LIST:         return "ARG_LIST";
  case CST_INIT_LIST:        return "INIT_LIST";
  case CST_COMPOUND_LITERAL: return "COMPOUND_LITERAL";
  }
  unreachable();
}

// The same escaping as the token dump (main.c): C-style for \n, \t, \r
// and \\, \xHH for everything invisible, including the space so that a
// dump line never ends in whitespace. The spelling never contains a raw
// newline (continuations are part of it and are escaped as \\ + \n), so
// one leaf is one line.
static void cst_print_escaped(FILE *out, char *p, int len) {
  for (int i = 0; i < len; i++) {
    unsigned char c = p[i];

    switch (c) {
    case '\n': fprintf(out, "\\n"); break;
    case '\t': fprintf(out, "\\t"); break;
    case '\r': fprintf(out, "\\r"); break;
    case '\\': fprintf(out, "\\\\"); break;
    default:
      if (c < 0x20 || c >= 0x7f || c == ' ')
        fprintf(out, "\\x%02x", c);
      else
        fputc(c, out);
    }
  }
}

static void cst_dump_node(CstNode *node, FILE *out, int depth) {
  for (int i = 0; i < depth; i++)
    fputs("  ", out);

  fputs(cst_kind_name(node->kind), out);

  if (node->tok && node->tok->raw_len) {
    fputc(' ', out);
    cst_print_escaped(out, node->tok->raw_loc, node->tok->raw_len);
  }
  fputc('\n', out);

  for (int i = 0; i < node->child_count; i++)
    cst_dump_node(node->children[i], out, depth + 1);
}

// Print the tree shape, two spaces per level. Leaf text is the token's
// raw spelling (not its trivia); the dump locks the tree, the round-trip
// locks the bytes.
void cst_dump(CstNode *root, FILE *out) {
  cst_dump_node(root, out, 0);
}

static bool cst_verify_node(CstNode *node, Token **cur) {
  if (node->tok) {
    if (node->child_count || *cur != node->tok)
      return false;
    *cur = (*cur)->next;
    return true;
  }

  for (int i = 0; i < node->child_count; i++)
    if (!cst_verify_node(node->children[i], cur))
      return false;
  return true;
}

// I1 / I3: the leaves in order are exactly the tokens of `tok`, each once
// (pointer equality), and every leaf holds a token while every inner node
// holds none. Returns the result; the caller reports it.
bool cst_verify(CstNode *root, Token *tok) {
  Token *cur = tok;
  return cst_verify_node(root, &cur) && !cur;
}
