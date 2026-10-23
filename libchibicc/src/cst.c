#include "chibicc.h"

// Lossless concrete syntax tree over the raw token stream (plans/PLAN.md stage
// C1/C2/C3). Every token of the input becomes one leaf, in order; the
// leaves' trivia and spellings rebuild the input file byte for byte.
//
// The tree groups delimiters, splits statements and builds declarations.
// A declaration is CST_DECL (specs + init-declarators + ';') or
// CST_FUNC_DEF (specs + declarator + body block); a parameter is a
// CST_PARAM inside CST_PARAM_LIST; a type name (cast, sizeof, compound
// literal) is CST_TYPE_NAME. Statements cover the C statement forms with
// every keyword, parenthesis, bracket and ';' explicit. An expression
// statement is CST_EXPR_STMT.
//
// Anything the grammars cannot cover becomes CST_UNPARSED, holding the
// raw skeleton children (tokens up to ';' with nested delimiters
// grouped). A group or block without its closing token, and a closer
// that has no opener at this level, are CST_UNPARSED too; the tokens
// they already hold stay in the tree.
//
// A preprocessing directive is a node of its own (CST_PP_*). The line
// runs from a '#' at the beginning of a logical line to the token before
// the next one; its tokens stay leaves. Conditional groups keep every
// branch: CST_PP_IF holds one CST_PP_BRANCH per #if / #elif / #else and
// the closing #endif, and the branch bodies are built with the same item
// grammar as their surroundings. Conditions are never evaluated and
// macros are never expanded.
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
// total, never reports a diagnostic and never exits (plans/RESULT.md I1-I6).

// Bound the CST parser's recursion. A deeper span becomes a flat,
// unparsed group; every token stays, so reconstruction remains lossless.
#define MAX_CST_DEPTH 512

static CstNode *new_cst_node(ChibiccContext *ctx, CstKind kind) {
  CstNode *node = chibicc_arena_calloc(ctx, 1, sizeof(CstNode));
  node->kind = kind;
  return node;
}

static void cst_add_child(ChibiccContext *ctx, CstNode *node, CstNode *child) {
  // The capacity lives in a size_t header just before `children`; the
  // public node exposes only child_count and the live entries.
  size_t cap = node->children ? ((size_t *)node->children)[-1] : 0;
  if ((size_t)node->child_count == cap) {
    cap = cap ? cap * 2 : 8;
    size_t bytes = sizeof(size_t) + cap * sizeof(CstNode *);
    char *p = chibicc_arena_alloc(ctx, bytes, 8);
    *(size_t *)p = cap;
    CstNode **children = (CstNode **)(p + sizeof(size_t));
    if (node->children)
      memcpy(children, node->children,
             (size_t)node->child_count * sizeof(CstNode *));
    node->children = children;
  }
  node->children[node->child_count++] = child;
}

static CstNode *cst_add_leaf(ChibiccContext *ctx, CstNode *parent, Token *tok) {
  CstNode *leaf = new_cst_node(ctx, tok->kind == TK_EOF ? CST_EOF : CST_TOKEN);
  leaf->tok = tok;
  cst_add_child(ctx, parent, leaf);
  return leaf;
}

static bool cst_is_open(Token *tok) {
  return equal(tok, "(") || equal(tok, "[") || equal(tok, "{");
}

static bool cst_is_close(Token *tok) {
  return equal(tok, ")") || equal(tok, "]") || equal(tok, "}");
}

// The parser's per-file state: the typedef-name oracle and the live
// recursion depth (F5b). A depth overrun turns the current span into raw,
// unparsed children; no token is dropped.
typedef struct {
  StringArray typedefs;
  int depth;
} CstContext;

// Parse one delimited group starting at `tok` (one of '(' '[' '{'), in
// raw mode: the interior is not split into statements or expressions,
// only nested delimiters are grouped. On a missing closer or a depth
// overrun the node becomes CST_UNPARSED but keeps the opener and
// everything it consumed.
static CstNode *cst_parse_group(ChibiccContext *ctx, CstContext *cst, Token **rest, Token *tok) {
  char *closer = equal(tok, "(") ? ")" : equal(tok, "[") ? "]" : "}";
  CstKind kind = equal(tok, "(") ? CST_GROUP_PAREN
               : equal(tok, "[") ? CST_GROUP_BRACKET
               : CST_GROUP_BRACE;

  CstNode *group = new_cst_node(ctx, kind);
  cst_add_leaf(ctx, group, tok);

  Token *p = tok->next;
  if (++cst->depth > MAX_CST_DEPTH) {
    // Depth fallback: keep the rest of the group as flat leaves. The
    // group is an unparsed span; the token order (and so the
    // reconstruction) is unchanged.
    group->kind = CST_UNPARSED;
    while (p && p->kind != TK_EOF && !equal(p, closer)) {
      cst_add_leaf(ctx, group, p);
      p = p->next;
    }
    if (p && p->kind != TK_EOF && equal(p, closer)) {
      cst_add_leaf(ctx, group, p);
      p = p->next;
    }
    cst->depth--;
    *rest = p;
    return group;
  }

  while (p && p->kind != TK_EOF && !equal(p, closer)) {
    if (cst_is_open(p)) {
      cst_add_child(ctx, group, cst_parse_group(ctx, cst, &p, p));
      continue;
    }
    cst_add_leaf(ctx, group, p);
    p = p->next;
  }

  if (p && p->kind != TK_EOF && equal(p, closer)) {
    cst_add_leaf(ctx, group, p);
    p = p->next;
  } else {
    group->kind = CST_UNPARSED;
  }
  cst->depth--;
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
  Token *tok;
  bool ok;
  CstContext *cst;
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

static bool cst_is_typedef_name(CstContext *cst, Token *tok) {
  if (!tok || tok->kind != TK_IDENT)
    return false;
  for (int i = 0; i < cst->typedefs.len; i++)
    if (equal(tok, cst->typedefs.data[i]))
      return true;
  return false;
}

// True if `tok` can start a type name in a cast or sizeof: a type
// specifier / qualifier keyword, or a file-local typedef name.
static bool cst_is_type_start(Token *tok, CstContext *cst) {
  char *kw[] = {
    "void", "_Bool", "char", "short", "int", "long", "float", "double",
    "signed", "unsigned", "_Complex", "_Imaginary", "const", "volatile",
    "restrict", "struct", "union", "enum", "typeof", "__typeof__",
    "__typeof", "_Atomic", "__signed__", "__const__", "__volatile__",
    "__restrict", "__restrict__",
    NULL,
  };
  return cst_in_list(tok, kw) || cst_is_typedef_name(cst, tok);
}

// True if `tok` can start a declaration. Storage classes and the
// declaration-only keywords are included; statement keywords are not.
static bool cst_is_decl_start(Token *tok, CstContext *cst) {
  char *kw[] = {
    "typedef", "static", "extern", "auto", "register", "inline",
    "_Thread_local", "__thread", "_Noreturn", "_Alignas", "const",
    "volatile", "restrict", "__restrict", "__restrict__", "__attribute__",
    "__extension__", "struct", "union", "enum", "typeof", "__typeof__",
    "__typeof", "_Atomic", "void", "_Bool", "char", "short", "int",
    "long", "float", "double", "signed", "unsigned", "_Complex",
    "_Imaginary", "__signed__", "__const__", "__volatile__",
    NULL,
  };
  return cst_in_list(tok, kw) || cst_is_typedef_name(cst, tok);
}

static void cst_add_typedef(ChibiccContext *ctx, CstContext *cst, Token *tok) {
  strarray_push(ctx, &cst->typedefs, chibicc_arena_strndup(ctx, tok->loc, tok->len));
}

// A conservative pre-scan for file-local typedef names. Names introduced
// by `typedef int T;`, `typedef int *T;`, `typedef int T[3];`,
// `typedef int A, B;`, `typedef struct S T;` and `typedef int (*T)(int);`
// are collected; parameter names and struct tags are not. This is the
// cast / declaration oracle the plan allows - an include-provided name
// stays unknown and is not guessed.
static void cst_collect_typedefs(ChibiccContext *ctx, CstContext *cst, Token *tok) {
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
        cst_add_typedef(ctx, cst, q);
      else if (depth == 1 && equal(prev, "*") && equal(q->next, ")"))
        cst_add_typedef(ctx, cst, q);
    }

    p = q ? q : p->next;
  }
}

static CstNode *cst_parse_comma(ChibiccContext *ctx, CstParser *p);
static CstNode *cst_parse_assign(ChibiccContext *ctx, CstParser *p);
static CstNode *cst_parse_conditional(ChibiccContext *ctx, CstParser *p);
static CstNode *cst_parse_unary(ChibiccContext *ctx, CstParser *p);
static CstNode *cst_parse_postfix(ChibiccContext *ctx, CstParser *p);
static CstNode *cst_parse_postfix_tail(ChibiccContext *ctx, CstParser *p, CstNode *node);
static CstNode *cst_parse_primary(ChibiccContext *ctx, CstParser *p);
static CstNode *cst_parse_arg_list(ChibiccContext *ctx, CstParser *p);
static CstNode *cst_parse_init_list(ChibiccContext *ctx, CstParser *p);
static CstNode *cst_parse_type_name(ChibiccContext *ctx, CstParser *p);
static CstNode *cst_parse_declarator(ChibiccContext *ctx, CstParser *p, bool allow_abstract);
static CstNode *cst_parse_decl(ChibiccContext *ctx, CstParser *p);
// The body behind each depth-guarded expression/declarator entry.
static CstNode *cst_parse_comma1(ChibiccContext *ctx, CstParser *p);
static CstNode *cst_parse_assign1(ChibiccContext *ctx, CstParser *p);
static CstNode *cst_parse_conditional1(ChibiccContext *ctx, CstParser *p);
static CstNode *cst_parse_unary1(ChibiccContext *ctx, CstParser *p);
static CstNode *cst_parse_init_list1(ChibiccContext *ctx, CstParser *p);
static CstNode *cst_parse_declarator1(ChibiccContext *ctx, CstParser *p, bool allow_abstract);

// Consume one balanced group in raw mode, for a type name in a cast or
// sizeof. An unclosed group fails the expression: the statement falls
// back to the skeleton, where the span becomes UNPARSED.
static CstNode *cst_parse_raw_group(ChibiccContext *ctx, CstParser *p) {
  Token *rest = NULL;
  CstNode *group = cst_parse_group(ctx, p->cst, &rest, p->tok);

  if (group->kind == CST_UNPARSED) {
    cst_fail(p);
    return NULL;
  }
  p->tok = rest;
  return group;
}

// primary = ident | number | char | string+ | "(" expr ")"
static CstNode *cst_parse_primary(ChibiccContext *ctx, CstParser *p) {
  if (!p->ok)
    return NULL;

  Token *tok = p->tok;
  if (!tok || tok->kind == TK_EOF) {
    cst_fail(p);
    return NULL;
  }

  if (equal(tok, "(")) {
    CstNode *node = new_cst_node(ctx, CST_PAREN);
    cst_add_leaf(ctx, node, tok);

    p->tok = tok->next;
    CstNode *inner = cst_parse_comma(ctx, p);
    if (!p->ok || !cst_at(p, ")")) {
      cst_fail(p);
      return NULL;
    }

    cst_add_child(ctx, node, inner);
    cst_add_leaf(ctx, node, p->tok);
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
  CstNode *node = new_cst_node(ctx, CST_PRIMARY);
  cst_add_leaf(ctx, node, tok);
  p->tok = tok->next;

  while (p->tok && p->tok->kind == TK_STR) {
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;
  }
  return node;
}

// arg-list = "(" (assign ("," assign)*)? ")"
static CstNode *cst_parse_arg_list(ChibiccContext *ctx, CstParser *p) {
  CstNode *args = new_cst_node(ctx, CST_ARG_LIST);
  cst_add_leaf(ctx, args, p->tok); // '('
  p->tok = p->tok->next;

  if (!cst_at(p, ")")) {
    for (;;) {
      CstNode *arg = cst_parse_assign(ctx, p);
      if (!p->ok)
        return NULL;
      cst_add_child(ctx, args, arg);

      if (!cst_at(p, ","))
        break;
      cst_add_leaf(ctx, args, p->tok);
      p->tok = p->tok->next;
    }
  }

  if (!cst_at(p, ")")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, args, p->tok);
  p->tok = p->tok->next;
  return args;
}

// designator = "[" conditional ("..." conditional)? "]" | "." ident
static CstNode *cst_parse_designator(ChibiccContext *ctx, CstParser *p) {
  CstNode *desig = new_cst_node(ctx, CST_DESIGNATOR);

  if (cst_at(p, "[")) {
    cst_add_leaf(ctx, desig, p->tok); // '['
    p->tok = p->tok->next;

    CstNode *begin = cst_parse_conditional(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, desig, begin);

    if (cst_at(p, "...")) {
      cst_add_leaf(ctx, desig, p->tok);
      p->tok = p->tok->next;
      CstNode *end = cst_parse_conditional(ctx, p);
      if (!p->ok)
        return NULL;
      cst_add_child(ctx, desig, end);
    }

    if (!cst_at(p, "]")) {
      cst_fail(p);
      return NULL;
    }
    cst_add_leaf(ctx, desig, p->tok);
    p->tok = p->tok->next;
    return desig;
  }

  if (cst_at(p, ".")) {
    Token *name = p->tok->next;
    if (!name || name->kind != TK_IDENT) {
      cst_fail(p);
      return NULL;
    }
    cst_add_leaf(ctx, desig, p->tok);
    cst_add_leaf(ctx, desig, name);
    p->tok = name->next;
    return desig;
  }

  cst_fail(p);
  return NULL;
}

// initializer = brace-list | string+ | assign
static CstNode *cst_parse_initializer(ChibiccContext *ctx, CstParser *p) {
  if (cst_at(p, "{"))
    return cst_parse_init_list(ctx, p);

  if (p->tok && p->tok->kind == TK_STR) {
    CstNode *node = new_cst_node(ctx, CST_PRIMARY);
    while (p->tok && p->tok->kind == TK_STR) {
      cst_add_leaf(ctx, node, p->tok);
      p->tok = p->tok->next;
    }
    return node;
  }

  return cst_parse_assign(ctx, p);
}

// init-list = "{" (init-item ("," init-item)* ","?)? "}"
// init-item = designator* "="? initializer
//
// The written shape is kept: designators are explicit nodes, a braced
// sublist is a nested CST_INIT_LIST, and no target-type decision is made
// here (brace elision belongs to a later consumer).
static CstNode *cst_parse_init_list(ChibiccContext *ctx, CstParser *p) {
  if (++p->cst->depth > MAX_CST_DEPTH) {
    p->cst->depth--;
    cst_fail(p);
    return NULL;
  }
  CstNode *node = cst_parse_init_list1(ctx, p);
  p->cst->depth--;
  return node;
}

static CstNode *cst_parse_init_list1(ChibiccContext *ctx, CstParser *p) {
  CstNode *list = new_cst_node(ctx, CST_INIT_LIST);
  cst_add_leaf(ctx, list, p->tok); // '{'
  p->tok = p->tok->next;

  if (cst_at(p, "}")) {
    cst_add_leaf(ctx, list, p->tok);
    p->tok = p->tok->next;
    return list;
  }

  for (;;) {
    CstNode *item = new_cst_node(ctx, CST_INITIALIZER);

    while (cst_at(p, "[") || cst_at(p, ".")) {
      CstNode *desig = cst_parse_designator(ctx, p);
      if (!p->ok)
        return NULL;
      cst_add_child(ctx, item, desig);
    }

    if (cst_at(p, "=")) {
      cst_add_leaf(ctx, item, p->tok);
      p->tok = p->tok->next;
    }

    CstNode *value = cst_parse_initializer(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, item, value);
    cst_add_child(ctx, list, item);

    if (!cst_at(p, ","))
      break;
    cst_add_leaf(ctx, list, p->tok);
    p->tok = p->tok->next;
    if (cst_at(p, "}"))
      break;
  }

  if (!cst_at(p, "}")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, list, p->tok);
  p->tok = p->tok->next;
  return list;
}

// postfix-tail = ("(" arg-list | "[" expr "]" | "." ident | "->" ident
//                | "++" | "--")*
static CstNode *cst_parse_postfix_tail(ChibiccContext *ctx, CstParser *p, CstNode *node) {
  for (;;) {
    if (!p->ok)
      return NULL;

    if (cst_at(p, "(")) {
      CstNode *args = cst_parse_arg_list(ctx, p);
      if (!p->ok)
        return NULL;
      CstNode *call = new_cst_node(ctx, CST_POSTFIX);
      cst_add_child(ctx, call, node);
      cst_add_child(ctx, call, args);
      node = call;
      continue;
    }

    if (cst_at(p, "[")) {
      Token *open = p->tok;
      p->tok = open->next;
      CstNode *idx = cst_parse_comma(ctx, p);
      if (!p->ok || !cst_at(p, "]")) {
        cst_fail(p);
        return NULL;
      }

      CstNode *sub = new_cst_node(ctx, CST_POSTFIX);
      cst_add_child(ctx, sub, node);
      cst_add_leaf(ctx, sub, open);
      cst_add_child(ctx, sub, idx);
      cst_add_leaf(ctx, sub, p->tok);
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

      CstNode *member = new_cst_node(ctx, CST_POSTFIX);
      cst_add_child(ctx, member, node);
      cst_add_leaf(ctx, member, op);
      cst_add_leaf(ctx, member, name);
      p->tok = name->next;
      node = member;
      continue;
    }

    if (cst_at(p, "++") || cst_at(p, "--")) {
      CstNode *inc = new_cst_node(ctx, CST_POSTFIX);
      cst_add_child(ctx, inc, node);
      cst_add_leaf(ctx, inc, p->tok);
      p->tok = p->tok->next;
      node = inc;
      continue;
    }

    return node;
  }
}

// postfix = primary postfix-tail
static CstNode *cst_parse_postfix(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = cst_parse_primary(ctx, p);
  if (!p->ok)
    return NULL;
  return cst_parse_postfix_tail(ctx, p, node);
}

// unary = ("+" | "-" | "!" | "~" | "*" | "&" | "&&" | "++" | "--") unary
//       | ("sizeof" | "_Alignof" | "typeof") (type-name | unary)
//       | "(" type-name ")" unary                    // cast
//       | "(" type-name ")" "{" init-list "}"        // compound literal
//       | postfix
static CstNode *cst_parse_unary(ChibiccContext *ctx, CstParser *p) {
  if (++p->cst->depth > MAX_CST_DEPTH) {
    p->cst->depth--;
    cst_fail(p);
    return NULL;
  }
  CstNode *node = cst_parse_unary1(ctx, p);
  p->cst->depth--;
  return node;
}

static CstNode *cst_parse_unary1(ChibiccContext *ctx, CstParser *p) {
  if (!p->ok)
    return NULL;

  if (cst_at(p, "sizeof") || cst_at(p, "_Alignof") || cst_at(p, "alignof") ||
      cst_at(p, "__alignof__") || cst_at(p, "__alignof") ||
      cst_at(p, "typeof") || cst_at(p, "__typeof__") || cst_at(p, "__typeof")) {
    Token *op = p->tok;
    p->tok = op->next;

    CstNode *operand = (cst_at(p, "(") && p->tok->next &&
                        cst_is_type_start(p->tok->next, p->cst))
                         ? cst_parse_type_name(ctx, p)
                         : cst_parse_unary(ctx, p);
    if (!p->ok)
      return NULL;

    CstNode *node = new_cst_node(ctx, CST_UNARY);
    cst_add_leaf(ctx, node, op);
    cst_add_child(ctx, node, operand);
    return node;
  }

  if (cst_at(p, "+") || cst_at(p, "-") || cst_at(p, "!") || cst_at(p, "~") ||
      cst_at(p, "*") || cst_at(p, "&") || cst_at(p, "&&") ||
      cst_at(p, "++") || cst_at(p, "--")) {
    Token *op = p->tok;
    p->tok = op->next;

    CstNode *operand = cst_parse_unary(ctx, p);
    if (!p->ok)
      return NULL;

    CstNode *node = new_cst_node(ctx, CST_UNARY);
    cst_add_leaf(ctx, node, op);
    cst_add_child(ctx, node, operand);
    return node;
  }

  if (cst_at(p, "(") && p->tok->next &&
      cst_is_type_start(p->tok->next, p->cst)) {
    CstNode *type = cst_parse_type_name(ctx, p);
    if (!p->ok)
      return NULL;

    if (cst_at(p, "{")) {
      CstNode *init = cst_parse_init_list(ctx, p);
      if (!p->ok)
        return NULL;
      CstNode *lit = new_cst_node(ctx, CST_COMPOUND_LITERAL);
      cst_add_child(ctx, lit, type);
      cst_add_child(ctx, lit, init);
      return cst_parse_postfix_tail(ctx, p, lit);
    }

    CstNode *operand = cst_parse_unary(ctx, p);
    if (!p->ok)
      return NULL;

    CstNode *cast = new_cst_node(ctx, CST_CAST);
    cst_add_child(ctx, cast, type);
    cst_add_child(ctx, cast, operand);
    return cast;
  }

  return cst_parse_postfix(ctx, p);
}

// binary-level <name> = <operand-level> (op <operand-level>)*, left
// associative. Each level keeps the operator as an explicit leaf.

static CstNode *cst_parse_mul(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = cst_parse_unary(ctx, p);
  while (p->ok && (cst_at(p, "*") || cst_at(p, "/") || cst_at(p, "%"))) {
    CstNode *bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_unary(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

static CstNode *cst_parse_add(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = cst_parse_mul(ctx, p);
  while (p->ok && (cst_at(p, "+") || cst_at(p, "-"))) {
    CstNode *bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_mul(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

static CstNode *cst_parse_shift(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = cst_parse_add(ctx, p);
  while (p->ok && (cst_at(p, "<<") || cst_at(p, ">>"))) {
    CstNode *bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_add(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

static CstNode *cst_parse_relational(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = cst_parse_shift(ctx, p);
  while (p->ok && (cst_at(p, "<") || cst_at(p, "<=") ||
                   cst_at(p, ">") || cst_at(p, ">="))) {
    CstNode *bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_shift(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

static CstNode *cst_parse_equality(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = cst_parse_relational(ctx, p);
  while (p->ok && (cst_at(p, "==") || cst_at(p, "!="))) {
    CstNode *bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_relational(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

static CstNode *cst_parse_bitand(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = cst_parse_equality(ctx, p);
  while (p->ok && cst_at(p, "&")) {
    CstNode *bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_equality(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

static CstNode *cst_parse_bitxor(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = cst_parse_bitand(ctx, p);
  while (p->ok && cst_at(p, "^")) {
    CstNode *bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_bitand(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

static CstNode *cst_parse_bitor(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = cst_parse_bitxor(ctx, p);
  while (p->ok && cst_at(p, "|")) {
    CstNode *bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_bitxor(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

static CstNode *cst_parse_logand(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = cst_parse_bitor(ctx, p);
  while (p->ok && cst_at(p, "&&")) {
    CstNode *bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_bitor(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

static CstNode *cst_parse_logor(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = cst_parse_logand(ctx, p);
  while (p->ok && cst_at(p, "||")) {
    CstNode *bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_logand(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

// conditional = logor ("?" expr ":" conditional | "?" ":" conditional)?
static CstNode *cst_parse_conditional(ChibiccContext *ctx, CstParser *p) {
  if (++p->cst->depth > MAX_CST_DEPTH) {
    p->cst->depth--;
    cst_fail(p);
    return NULL;
  }
  CstNode *node = cst_parse_conditional1(ctx, p);
  p->cst->depth--;
  return node;
}

static CstNode *cst_parse_conditional1(ChibiccContext *ctx, CstParser *p) {
  CstNode *cond = cst_parse_logor(ctx, p);
  if (!p->ok || !cst_at(p, "?"))
    return cond;

  CstNode *node = new_cst_node(ctx, CST_COND);
  cst_add_child(ctx, node, cond);
  cst_add_leaf(ctx, node, p->tok); // '?'
  p->tok = p->tok->next;

  // [GNU] `a ?: b` has no middle operand.
  if (cst_at(p, ":")) {
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;
    CstNode *els = cst_parse_conditional(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, node, els);
    return node;
  }

  CstNode *then = cst_parse_comma(ctx, p);
  if (!p->ok || !cst_at(p, ":")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_child(ctx, node, then);
  cst_add_leaf(ctx, node, p->tok); // ':'
  p->tok = p->tok->next;

  CstNode *els = cst_parse_conditional(ctx, p);
  if (!p->ok)
    return NULL;
  cst_add_child(ctx, node, els);
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
static CstNode *cst_parse_assign(ChibiccContext *ctx, CstParser *p) {
  if (++p->cst->depth > MAX_CST_DEPTH) {
    p->cst->depth--;
    cst_fail(p);
    return NULL;
  }
  CstNode *node = cst_parse_assign1(ctx, p);
  p->cst->depth--;
  return node;
}

static CstNode *cst_parse_assign1(ChibiccContext *ctx, CstParser *p) {
  CstNode *lhs = cst_parse_conditional(ctx, p);
  if (!p->ok || !cst_at_assign_op(p))
    return lhs;

  CstNode *node = new_cst_node(ctx, CST_ASSIGN);
  cst_add_child(ctx, node, lhs);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  CstNode *rhs = cst_parse_assign(ctx, p);
  if (!p->ok)
    return NULL;
  cst_add_child(ctx, node, rhs);
  return node;
}

// comma = assign ("," assign)*
static CstNode *cst_parse_comma(ChibiccContext *ctx, CstParser *p) {
  if (++p->cst->depth > MAX_CST_DEPTH) {
    p->cst->depth--;
    cst_fail(p);
    return NULL;
  }
  CstNode *node = cst_parse_comma1(ctx, p);
  p->cst->depth--;
  return node;
}

static CstNode *cst_parse_comma1(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = cst_parse_assign(ctx, p);
  while (p->ok && cst_at(p, ",")) {
    CstNode *comma = new_cst_node(ctx, CST_COMMA);
    cst_add_child(ctx, comma, node);
    cst_add_leaf(ctx, comma, p->tok);
    p->tok = p->tok->next;

    CstNode *rhs = cst_parse_assign(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, comma, rhs);
    node = comma;
  }
  return node;
}

//
// Declaration layer (C2.2)
//

static void cst_parse_compound(ChibiccContext *ctx, CstParser *p, CstNode *block);
static CstNode *cst_parse_param_list(ChibiccContext *ctx, CstParser *p);

// Consume one balanced group in raw mode from `start`, resetting the
// parser on failure. Used where a structured attempt did not fit and the
// tokens must still stay in the tree (K&R parameter lists, array
// dimensions, attributes).
static CstNode *cst_parse_raw_group_at(ChibiccContext *ctx, CstParser *p, Token *start) {
  p->tok = start;
  p->ok = true;

  Token *rest = NULL;
  CstNode *group = cst_parse_group(ctx, p->cst, &rest, start);
  if (group->kind == CST_UNPARSED) {
    cst_fail(p);
    return NULL;
  }
  p->tok = rest;
  return group;
}

// decl-specs = (storage-class | qualifier | type-specifier | ...)+
//
// Shapes only: `struct` / `union` / `enum` bodies, typeof operands,
// _Atomic / _Alignas arguments and attributes stay raw groups. A
// user-defined type name is a specifier only while no other specifier
// has been seen, so `T T;` leaves the second T to the declarator.
static CstNode *cst_parse_decl_specs(ChibiccContext *ctx, CstParser *p) {
  CstNode *specs = new_cst_node(ctx, CST_DECL_SPECS);
  bool has_type = false;

  for (;;) {
    Token *tok = p->tok;
    if (!p->ok || !tok || tok->kind == TK_EOF)
      break;

    char *plain[] = {
      "typedef", "static", "extern", "auto", "register", "inline",
      "_Thread_local", "__thread", "_Noreturn", "const", "volatile",
      "restrict", "__restrict", "__restrict__",
      NULL,
    };
    if (cst_in_list(tok, plain)) {
      cst_add_leaf(ctx, specs, tok);
      p->tok = tok->next;
      continue;
    }

    char *builtin[] = {
      "void", "_Bool", "char", "short", "int", "long", "float", "double",
      "signed", "unsigned", "_Complex", "_Imaginary",
      "__signed__", "__const__", "__volatile__",
      NULL,
    };
    if (cst_in_list(tok, builtin)) {
      cst_add_leaf(ctx, specs, tok);
      p->tok = tok->next;
      has_type = true;
      continue;
    }

    if (equal(tok, "struct") || equal(tok, "union")) {
      if (has_type)
        break;
      cst_add_leaf(ctx, specs, tok);
      p->tok = tok->next;
      if (p->tok && p->tok->kind == TK_IDENT && !cst_is_keyword(p->tok)) {
        cst_add_leaf(ctx, specs, p->tok);
        p->tok = p->tok->next;
      }
      if (cst_at(p, "{")) {
        Token *rest = NULL;
        CstNode *body = cst_parse_group(ctx, p->cst, &rest, p->tok);
        if (body->kind == CST_UNPARSED) {
          cst_fail(p);
          return NULL;
        }
        p->tok = rest;
        cst_add_child(ctx, specs, body);
      }
      has_type = true;
      continue;
    }

    if (equal(tok, "enum")) {
      if (has_type)
        break;
      cst_add_leaf(ctx, specs, tok);
      p->tok = tok->next;
      if (p->tok && p->tok->kind == TK_IDENT && !cst_is_keyword(p->tok)) {
        cst_add_leaf(ctx, specs, p->tok);
        p->tok = p->tok->next;
      }
      if (cst_at(p, "{")) {
        Token *rest = NULL;
        CstNode *body = cst_parse_group(ctx, p->cst, &rest, p->tok);
        if (body->kind == CST_UNPARSED) {
          cst_fail(p);
          return NULL;
        }
        p->tok = rest;
        cst_add_child(ctx, specs, body);
      }
      has_type = true;
      continue;
    }

    if (equal(tok, "typeof") || equal(tok, "__typeof__") ||
        equal(tok, "__typeof")) {
      if (has_type)
        break;
      cst_add_leaf(ctx, specs, tok);
      p->tok = tok->next;
      if (cst_at(p, "(")) {
        CstNode *operand = cst_parse_raw_group(ctx, p);
        if (!p->ok)
          return NULL;
        cst_add_child(ctx, specs, operand);
      }
      has_type = true;
      continue;
    }

    if (equal(tok, "_Atomic")) {
      cst_add_leaf(ctx, specs, tok);
      p->tok = tok->next;
      if (cst_at(p, "(")) {
        CstNode *operand = cst_parse_raw_group(ctx, p);
        if (!p->ok)
          return NULL;
        cst_add_child(ctx, specs, operand);
        has_type = true;
      }
      continue;
    }

    if (equal(tok, "_Alignas")) {
      cst_add_leaf(ctx, specs, tok);
      p->tok = tok->next;
      if (cst_at(p, "(")) {
        CstNode *operand = cst_parse_raw_group(ctx, p);
        if (!p->ok)
          return NULL;
        cst_add_child(ctx, specs, operand);
      }
      continue;
    }

    if (equal(tok, "__attribute__") || equal(tok, "__extension__")) {
      bool attr = equal(tok, "__attribute__");
      cst_add_leaf(ctx, specs, tok);
      p->tok = tok->next;
      if (attr && cst_at(p, "(")) {
        CstNode *group = cst_parse_raw_group(ctx, p);
        if (!p->ok)
          return NULL;
        cst_add_child(ctx, specs, group);
      }
      continue;
    }

    if (cst_is_typedef_name(p->cst, tok)) {
      if (has_type)
        break;
      cst_add_leaf(ctx, specs, tok);
      p->tok = tok->next;
      has_type = true;
      continue;
    }

    break;
  }

  if (specs->child_count == 0) {
    cst_fail(p);
    return NULL;
  }
  return specs;
}

// Consume `__attribute__((...))` pairs after a declarator, keeping them
// on the declarator node.
static bool cst_parse_attributes(ChibiccContext *ctx, CstParser *p, CstNode *node) {
  while (cst_at(p, "__attribute__")) {
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;
    if (cst_at(p, "(")) {
      CstNode *group = cst_parse_raw_group(ctx, p);
      if (!p->ok)
        return false;
      cst_add_child(ctx, node, group);
    }
  }
  return true;
}

// array-dimension = "[" ("static" | "restrict" | "*")* expr? "]"
// The expression is parsed when it fits; otherwise the bracket keeps its
// raw tokens.
static CstNode *cst_parse_dimension(ChibiccContext *ctx, CstParser *p) {
  Token *start = p->tok;
  CstNode *bracket = new_cst_node(ctx, CST_GROUP_BRACKET);
  cst_add_leaf(ctx, bracket, p->tok); // '['
  p->tok = p->tok->next;

  while (cst_at(p, "static") || cst_at(p, "restrict") ||
         cst_at(p, "const") || cst_at(p, "volatile")) {
    cst_add_leaf(ctx, bracket, p->tok);
    p->tok = p->tok->next;
  }

  if (cst_at(p, "]")) {
    cst_add_leaf(ctx, bracket, p->tok);
    p->tok = p->tok->next;
    return bracket;
  }

  if (cst_at(p, "*")) {
    cst_add_leaf(ctx, bracket, p->tok);
    p->tok = p->tok->next;
  } else {
    CstNode *expr = cst_parse_assign(ctx, p);
    if (!p->ok)
      return cst_parse_raw_group_at(ctx, p, start);
    cst_add_child(ctx, bracket, expr);
  }

  if (!cst_at(p, "]"))
    return cst_parse_raw_group_at(ctx, p, start);
  cst_add_leaf(ctx, bracket, p->tok);
  p->tok = p->tok->next;
  return bracket;
}

// declarator = "*"* ("(" declarator ")" | ident)? ("(" params ")" | "[" dim "]")*
//
// The written grouping is kept: pointer stars, an inner declarator in a
// GROUP_PAREN, the name, and the suffixes in source order. No type is
// built and no name is bound.
static CstNode *cst_parse_declarator(ChibiccContext *ctx, CstParser *p, bool allow_abstract) {
  if (++p->cst->depth > MAX_CST_DEPTH) {
    p->cst->depth--;
    cst_fail(p);
    return NULL;
  }
  CstNode *node = cst_parse_declarator1(ctx, p, allow_abstract);
  p->cst->depth--;
  return node;
}

static CstNode *cst_parse_declarator1(ChibiccContext *ctx, CstParser *p, bool allow_abstract) {
  CstNode *decl = new_cst_node(ctx, CST_DECLARATOR);

  while (cst_at(p, "*")) {
    cst_add_leaf(ctx, decl, p->tok);
    p->tok = p->tok->next;
    while (cst_at(p, "const") || cst_at(p, "volatile") ||
           cst_at(p, "restrict") || cst_at(p, "__restrict") ||
           cst_at(p, "__restrict__")) {
      cst_add_leaf(ctx, decl, p->tok);
      p->tok = p->tok->next;
    }
  }

  if (cst_at(p, "(")) {
    Token *open = p->tok;
    p->tok = open->next;

    CstNode *inner = cst_parse_declarator(ctx, p, allow_abstract);
    if (!p->ok || !cst_at(p, ")")) {
      cst_fail(p);
      return NULL;
    }

    CstNode *group = new_cst_node(ctx, CST_GROUP_PAREN);
    cst_add_leaf(ctx, group, open);
    cst_add_child(ctx, group, inner);
    cst_add_leaf(ctx, group, p->tok);
    p->tok = p->tok->next;
    cst_add_child(ctx, decl, group);
  } else if (p->tok && p->tok->kind == TK_IDENT && !cst_is_keyword(p->tok)) {
    cst_add_leaf(ctx, decl, p->tok);
    p->tok = p->tok->next;
  } else if (!allow_abstract) {
    cst_fail(p);
    return NULL;
  }

  for (;;) {
    if (cst_at(p, "(")) {
      CstNode *params = cst_parse_param_list(ctx, p);
      if (!p->ok)
        return NULL;
      cst_add_child(ctx, decl, params);
      continue;
    }
    if (cst_at(p, "[")) {
      CstNode *dim = cst_parse_dimension(ctx, p);
      if (!p->ok)
        return NULL;
      cst_add_child(ctx, decl, dim);
      continue;
    }
    break;
  }

  if (!cst_parse_attributes(ctx, p, decl))
    return NULL;
  return decl;
}

// parameter = decl-specs declarator?
static CstNode *cst_parse_param(ChibiccContext *ctx, CstParser *p) {
  CstNode *specs = cst_parse_decl_specs(ctx, p);
  if (!p->ok)
    return NULL;

  CstNode *decl = cst_parse_declarator(ctx, p, true);
  if (!p->ok)
    return NULL;

  CstNode *param = new_cst_node(ctx, CST_PARAM);
  cst_add_child(ctx, param, specs);
  if (decl->child_count)
    cst_add_child(ctx, param, decl);
  return param;
}

// param-list = "(" (param ("," param)* | "..." | raw)? ")"
// A parameter list the declaration grammar cannot explain keeps its raw
// tokens in a GROUP_PAREN (K&R identifier lists are the common case).
static CstNode *cst_parse_param_list(ChibiccContext *ctx, CstParser *p) {
  Token *start = p->tok;
  CstNode *list = new_cst_node(ctx, CST_PARAM_LIST);
  cst_add_leaf(ctx, list, p->tok); // '('
  p->tok = p->tok->next;

  if (cst_at(p, ")")) {
    cst_add_leaf(ctx, list, p->tok);
    p->tok = p->tok->next;
    return list;
  }

  if (cst_at(p, "...")) {
    cst_add_leaf(ctx, list, p->tok);
    p->tok = p->tok->next;
  } else {
    for (;;) {
      CstNode *param = cst_parse_param(ctx, p);
      if (!param)
        return cst_parse_raw_group_at(ctx, p, start);
      cst_add_child(ctx, list, param);

      if (!cst_at(p, ","))
        break;
      cst_add_leaf(ctx, list, p->tok);
      p->tok = p->tok->next;
    }
  }

  if (!cst_at(p, ")"))
    return cst_parse_raw_group_at(ctx, p, start);
  cst_add_leaf(ctx, list, p->tok);
  p->tok = p->tok->next;
  return list;
}

// type-name = "(" decl-specs abstract-declarator? ")"
static CstNode *cst_parse_type_name(ChibiccContext *ctx, CstParser *p) {
  Token *open = p->tok;
  CstNode *node = new_cst_node(ctx, CST_TYPE_NAME);
  cst_add_leaf(ctx, node, open); // '('
  p->tok = open->next;

  CstNode *specs = cst_parse_decl_specs(ctx, p);
  if (!p->ok)
    return NULL;
  cst_add_child(ctx, node, specs);

  CstNode *decl = cst_parse_declarator(ctx, p, true);
  if (!p->ok)
    return NULL;
  if (decl->child_count)
    cst_add_child(ctx, node, decl);

  if (!cst_at(p, ")")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;
  return node;
}

// declaration = decl-specs init-declarator ("," init-declarator)* ";"
//             | decl-specs ";"
// function-definition = decl-specs declarator compound-stmt
static CstNode *cst_parse_decl(ChibiccContext *ctx, CstParser *p) {
  CstNode *specs = cst_parse_decl_specs(ctx, p);
  if (!p->ok)
    return NULL;

  // A tag or specifier with no declarator: `struct S {...};`, `int;`.
  if (cst_at(p, ";")) {
    CstNode *decl = new_cst_node(ctx, CST_DECL);
    cst_add_child(ctx, decl, specs);
    cst_add_leaf(ctx, decl, p->tok);
    p->tok = p->tok->next;
    return decl;
  }

  CstNode *declr = cst_parse_declarator(ctx, p, false);
  if (!p->ok)
    return NULL;

  // `declarator {` is a function definition. GNU nested functions are
  // allowed too; nothing here checks the scope.
  if (cst_at(p, "{")) {
    CstNode *body = new_cst_node(ctx, CST_BLOCK);
    cst_parse_compound(ctx, p, body);
    if (!p->ok)
      return NULL;

    CstNode *fn = new_cst_node(ctx, CST_FUNC_DEF);
    cst_add_child(ctx, fn, specs);
    cst_add_child(ctx, fn, declr);
    cst_add_child(ctx, fn, body);
    return fn;
  }

  CstNode *decl = new_cst_node(ctx, CST_DECL);
  cst_add_child(ctx, decl, specs);

  for (;;) {
    if (cst_at(p, "=")) {
      CstNode *init_decl = new_cst_node(ctx, CST_INIT_DECLARATOR);
      cst_add_child(ctx, init_decl, declr);
      cst_add_leaf(ctx, init_decl, p->tok); // '='
      p->tok = p->tok->next;

      CstNode *init = cst_parse_initializer(ctx, p);
      if (!p->ok)
        return NULL;
      cst_add_child(ctx, init_decl, init);
      cst_add_child(ctx, decl, init_decl);
    } else {
      cst_add_child(ctx, decl, declr);
    }

    if (!cst_at(p, ","))
      break;
    cst_add_leaf(ctx, decl, p->tok);
    p->tok = p->tok->next;

    declr = cst_parse_declarator(ctx, p, false);
    if (!p->ok)
      return NULL;
  }

  if (!cst_at(p, ";")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, decl, p->tok);
  p->tok = p->tok->next;
  return decl;
}

//
// Statement layer
//

static bool cst_is_directive(Token *tok);
static bool cst_is_pp_terminator(Token *tok);
static CstNode *cst_parse_directive_line(ChibiccContext *ctx, CstParser *p);
static CstNode *cst_parse_pp_if(ChibiccContext *ctx, CstParser *p);
static CstNode *cst_parse_item(ChibiccContext *ctx, CstParser *p);
static CstNode *cst_parse_item1(ChibiccContext *ctx, CstParser *p);

// Parse one statement into the fallback shape: tokens up to and
// including a ';' at this level, a raw group per delimiter, and stop
// before a closer that belongs to an outer level. A span cut short by a
// closer (or EOF) keeps the tokens it has; the skeleton does not guess
// where it should have ended. The node is CST_UNPARSED: the expression
// grammar did not explain the span (a declaration is one such span until
// C2.2), and the children are exactly the raw pieces, not a guess.
static CstNode *cst_parse_fallback_statement(ChibiccContext *ctx, CstContext *cst, Token **rest,
                                             Token *tok) {
  CstNode *stmt = new_cst_node(ctx, CST_UNPARSED);

  Token *p = tok;
  while (p && p->kind != TK_EOF) {
    // A directive at the start of a line begins the next item; a
    // malformed span must not swallow it. The span's own first token is
    // never a directive (the item parser handles those).
    if (p != tok && cst_is_directive(p))
      break;
    if (equal(p, ";")) {
      cst_add_leaf(ctx, stmt, p);
      p = p->next;
      break;
    }
    if (cst_is_close(p))
      break;
    if (cst_is_open(p)) {
      cst_add_child(ctx, stmt, cst_parse_group(ctx, cst, &p, p));
      continue;
    }
    cst_add_leaf(ctx, stmt, p);
    p = p->next;
  }
  *rest = p;
  return stmt;
}

// A statement whose whole span is an expression followed by ';' becomes
// CST_EXPR_STMT. On any failure the caller uses the raw skeleton instead;
// the failed attempt's nodes are unreachable, which is allowed.
static CstNode *cst_try_expr_stmt(ChibiccContext *ctx, Token **rest, Token *tok, CstContext *cst) {
  CstParser p = {tok, true, cst};

  CstNode *expr = cst_parse_comma(ctx, &p);
  if (!p.ok || !equal(p.tok, ";"))
    return NULL;

  CstNode *stmt = new_cst_node(ctx, CST_EXPR_STMT);
  cst_add_child(ctx, stmt, expr);
  cst_add_leaf(ctx, stmt, p.tok);
  *rest = p.tok->next;
  return stmt;
}

static CstNode *cst_parse_statement(ChibiccContext *ctx, CstParser *p);
static CstNode *cst_parse_statement1(ChibiccContext *ctx, CstParser *p);

// statement = "return" expr? ";"
//           | "if" "(" expr ")" statement ("else" statement)?
//           | "switch" "(" expr ")" statement
//           | "while" "(" expr ")" statement
//           | "do" statement "while" "(" expr ")" ";"
//           | "for" "(" (decl | expr)? ";" expr? ";" expr? ")" statement
//           | "case" conditional ("..." conditional)? ":" statement
//           | "default" ":" statement
//           | "goto" (ident | "*" expr) ";"
//           | "break" ";" | "continue" ";"
//           | "asm" qualifiers* "(" raw ")" ";"
//           | ident ":" statement
//           | compound-stmt | declaration | expr-stmt
static CstNode *cst_parse_simple_stmt(ChibiccContext *ctx, CstParser *p, CstKind kind) {
  CstNode *node = new_cst_node(ctx, kind);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, ";")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;
  return node;
}

static CstNode *cst_parse_return(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = new_cst_node(ctx, CST_RETURN);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, ";")) {
    CstNode *expr = cst_parse_comma(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, node, expr);
  }

  if (!cst_at(p, ";")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;
  return node;
}

static CstNode *cst_parse_if(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = new_cst_node(ctx, CST_IF);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, "(")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  CstNode *cond = cst_parse_comma(ctx, p);
  if (!p->ok || !cst_at(p, ")")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_child(ctx, node, cond);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  CstNode *then = cst_parse_statement(ctx, p);
  if (!then)
    return NULL;
  cst_add_child(ctx, node, then);

  if (cst_at(p, "else")) {
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;

    CstNode *els = cst_parse_statement(ctx, p);
    if (!els)
      return NULL;
    cst_add_child(ctx, node, els);
  }
  return node;
}

static CstNode *cst_parse_switch(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = new_cst_node(ctx, CST_SWITCH);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, "(")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  CstNode *cond = cst_parse_comma(ctx, p);
  if (!p->ok || !cst_at(p, ")")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_child(ctx, node, cond);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  CstNode *body = cst_parse_statement(ctx, p);
  if (!body)
    return NULL;
  cst_add_child(ctx, node, body);
  return node;
}

static CstNode *cst_parse_while(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = new_cst_node(ctx, CST_WHILE);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, "(")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  CstNode *cond = cst_parse_comma(ctx, p);
  if (!p->ok || !cst_at(p, ")")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_child(ctx, node, cond);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  CstNode *body = cst_parse_statement(ctx, p);
  if (!body)
    return NULL;
  cst_add_child(ctx, node, body);
  return node;
}

static CstNode *cst_parse_do(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = new_cst_node(ctx, CST_DO);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  CstNode *body = cst_parse_statement(ctx, p);
  if (!body)
    return NULL;
  cst_add_child(ctx, node, body);

  if (!cst_at(p, "while")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, "(")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  CstNode *cond = cst_parse_comma(ctx, p);
  if (!p->ok || !cst_at(p, ")")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_child(ctx, node, cond);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, ";")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;
  return node;
}

static CstNode *cst_parse_for(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = new_cst_node(ctx, CST_FOR);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, "(")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  // Init: a declaration owns its terminating ';', an expression does not.
  if (cst_at(p, ";")) {
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;
  } else if (cst_is_decl_start(p->tok, p->cst)) {
    CstNode *decl = cst_parse_decl(ctx, p);
    if (!decl)
      return NULL;
    cst_add_child(ctx, node, decl);
  } else {
    CstNode *init = cst_parse_comma(ctx, p);
    if (!p->ok || !cst_at(p, ";")) {
      cst_fail(p);
      return NULL;
    }
    cst_add_child(ctx, node, init);
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;
  }

  if (!cst_at(p, ";")) {
    CstNode *cond = cst_parse_comma(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, node, cond);
  }
  if (!cst_at(p, ";")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, ")")) {
    CstNode *inc = cst_parse_comma(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, node, inc);
  }
  if (!cst_at(p, ")")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  CstNode *body = cst_parse_statement(ctx, p);
  if (!body)
    return NULL;
  cst_add_child(ctx, node, body);
  return node;
}

static CstNode *cst_parse_case(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = new_cst_node(ctx, CST_CASE);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  CstNode *begin = cst_parse_conditional(ctx, p);
  if (!p->ok)
    return NULL;
  cst_add_child(ctx, node, begin);

  if (cst_at(p, "...")) {
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;

    CstNode *end = cst_parse_conditional(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, node, end);
  }

  if (!cst_at(p, ":")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  CstNode *body = cst_parse_statement(ctx, p);
  if (!body)
    return NULL;
  cst_add_child(ctx, node, body);
  return node;
}

static CstNode *cst_parse_default(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = new_cst_node(ctx, CST_DEFAULT);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, ":")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  CstNode *body = cst_parse_statement(ctx, p);
  if (!body)
    return NULL;
  cst_add_child(ctx, node, body);
  return node;
}

static CstNode *cst_parse_label(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = new_cst_node(ctx, CST_LABEL);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  cst_add_leaf(ctx, node, p->tok); // ':'
  p->tok = p->tok->next;

  CstNode *body = cst_parse_statement(ctx, p);
  if (!body)
    return NULL;
  cst_add_child(ctx, node, body);
  return node;
}

static CstNode *cst_parse_goto(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = new_cst_node(ctx, CST_GOTO);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (cst_at(p, "*")) {
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;

    CstNode *expr = cst_parse_comma(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, node, expr);
  } else {
    if (!p->tok || p->tok->kind != TK_IDENT) {
      cst_fail(p);
      return NULL;
    }
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;
  }

  if (!cst_at(p, ";")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;
  return node;
}

static CstNode *cst_parse_asm(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = new_cst_node(ctx, CST_ASM);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  while (cst_at(p, "volatile") || cst_at(p, "inline")) {
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;
  }

  if (!cst_at(p, "(")) {
    cst_fail(p);
    return NULL;
  }
  CstNode *operand = cst_parse_raw_group(ctx, p);
  if (!p->ok)
    return NULL;
  cst_add_child(ctx, node, operand);

  if (!cst_at(p, ";")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;
  return node;
}

static CstNode *cst_parse_statement(ChibiccContext *ctx, CstParser *p) {
  if (++p->cst->depth > MAX_CST_DEPTH) {
    p->cst->depth--;
    cst_fail(p);
    return NULL;
  }
  CstNode *node = cst_parse_statement1(ctx, p);
  p->cst->depth--;
  return node;
}

static CstNode *cst_parse_statement1(ChibiccContext *ctx, CstParser *p) {
  if (!p->ok || !p->tok || p->tok->kind == TK_EOF)
    return NULL;

  Token *tok = p->tok;

  if (equal(tok, "return"))
    return cst_parse_return(ctx, p);
  if (equal(tok, "if"))
    return cst_parse_if(ctx, p);
  if (equal(tok, "switch"))
    return cst_parse_switch(ctx, p);
  if (equal(tok, "while"))
    return cst_parse_while(ctx, p);
  if (equal(tok, "do"))
    return cst_parse_do(ctx, p);
  if (equal(tok, "for"))
    return cst_parse_for(ctx, p);
  if (equal(tok, "case"))
    return cst_parse_case(ctx, p);
  if (equal(tok, "default"))
    return cst_parse_default(ctx, p);
  if (equal(tok, "goto"))
    return cst_parse_goto(ctx, p);
  if (equal(tok, "break"))
    return cst_parse_simple_stmt(ctx, p, CST_BREAK);
  if (equal(tok, "continue"))
    return cst_parse_simple_stmt(ctx, p, CST_CONTINUE);
  if (equal(tok, "asm") || equal(tok, "__asm__"))
    return cst_parse_asm(ctx, p);

  if (tok->kind == TK_IDENT && equal(tok->next, ":"))
    return cst_parse_label(ctx, p);

  if (equal(tok, "{")) {
    CstNode *block = new_cst_node(ctx, CST_BLOCK);
    cst_parse_compound(ctx, p, block);
    return p->ok ? block : NULL;
  }

  if (equal(tok, ";")) {
    CstNode *null = new_cst_node(ctx, CST_NULL_STMT);
    cst_add_leaf(ctx, null, tok);
    p->tok = tok->next;
    return null;
  }

  // A declaration is tried only when the first token can start one; an
  // unknown type (`size_t x;`) therefore fails the expression path and
  // becomes UNPARSED instead of being guessed.
  if (cst_is_decl_start(tok, p->cst)) {
    CstNode *decl = cst_parse_decl(ctx, p);
    if (decl)
      return decl;
    cst_fail(p);
    return NULL;
  }

  Token *next = NULL;
  CstNode *stmt = cst_try_expr_stmt(ctx, &next, tok, p->cst);
  if (stmt) {
    p->tok = next;
    return stmt;
  }

  cst_fail(p);
  return NULL;
}

//
// Preprocessor directive layer (C3.1)
//
// A directive line starts at a '#' with at_bol - the same test as
// preprocess.c's is_hash - and ends before the next at_bol token. The
// scan buffer has already removed continuations and a newline inside a
// block comment does not set at_bol, so a multi-line directive is one
// line here too. The tokens of a directive stay leaves; the tree only
// groups them by line and, for conditionals, by branch. Conditions are
// not evaluated, macros are not expanded and no file is looked up.
//

static bool cst_is_directive(Token *tok) {
  return tok && tok->kind != TK_EOF && tok->at_bol && equal(tok, "#");
}

// The directive word after '#'. NULL means an empty directive (`#` alone
// on its logical line).
static Token *cst_directive_name(Token *tok) {
  Token *name = tok->next;
  if (!name || name->kind == TK_EOF || name->at_bol)
    return NULL;
  return name;
}

// #elif / #else / #endif close a branch body; they belong to the
// enclosing CST_PP_IF rather than to the body's items.
static bool cst_is_pp_terminator(Token *tok) {
  if (!cst_is_directive(tok))
    return false;
  Token *name = cst_directive_name(tok);
  return name && (equal(name, "elif") || equal(name, "else") ||
                  equal(name, "endif"));
}

static bool cst_is_pp_if(Token *tok) {
  if (!cst_is_directive(tok))
    return false;
  Token *name = cst_directive_name(tok);
  return name && (equal(name, "if") || equal(name, "ifdef") ||
                  equal(name, "ifndef"));
}

// The first token after this directive line: the next token with at_bol,
// or EOF. Never NULL on tokenize_file()'s output.
static Token *cst_directive_end(Token *tok) {
  Token *p = tok->next;
  while (p && p->kind != TK_EOF && !p->at_bol)
    p = p->next;
  return p;
}

// The directive's kind by spelling. A number after '#' is a GNU line
// marker (`# 123 "file"`); an unrecognized word stays CST_PP_DIRECTIVE
// and an empty line is CST_PP_NULL. #if / #elif / #else / #endif never
// reach this function as part of a well-formed conditional.
static CstKind cst_pp_kind(Token *tok) {
  Token *name = cst_directive_name(tok);
  if (!name)
    return CST_PP_NULL;
  if (name->kind == TK_PP_NUM)
    return CST_PP_LINE;
  if (equal(name, "define"))
    return CST_PP_DEFINE;
  if (equal(name, "undef"))
    return CST_PP_UNDEF;
  if (equal(name, "include"))
    return CST_PP_INCLUDE;
  if (equal(name, "line"))
    return CST_PP_LINE;
  if (equal(name, "pragma"))
    return CST_PP_PRAGMA;
  if (equal(name, "error"))
    return CST_PP_ERROR;
  return CST_PP_DIRECTIVE;
}

// A function-like macro's parameter list: '(' through the matching ')',
// not past the end of the line. Parameters do not nest in valid input,
// so the group is flat; a missing ')' stops at the line's end and turns
// the group into UNPARSED.
static CstNode *cst_parse_pp_params(ChibiccContext *ctx, Token **rest, Token *tok, Token *end) {
  CstNode *group = new_cst_node(ctx, CST_GROUP_PAREN);
  cst_add_leaf(ctx, group, tok);

  int depth = 1;
  Token *p = tok->next;
  while (p != end) {
    if (equal(p, "(")) {
      depth++;
    } else if (equal(p, ")")) {
      depth--;
      cst_add_leaf(ctx, group, p);
      p = p->next;
      if (depth == 0) {
        *rest = p;
        return group;
      }
      continue;
    }
    cst_add_leaf(ctx, group, p);
    p = p->next;
  }

  group->kind = CST_UNPARSED;
  *rest = p;
  return group;
}

// Consume exactly one directive line. All of its tokens become leaves of
// the directive node. For #define the macro name is followed by a
// GROUP_PAREN only when '(' comes with no space - the object / function
// macro boundary of preprocess.c - so the dump records the distinction.
static CstNode *cst_parse_directive_line(ChibiccContext *ctx, CstParser *p) {
  Token *hash = p->tok;
  Token *end = cst_directive_end(hash);
  CstNode *node = new_cst_node(ctx, cst_pp_kind(hash));
  cst_add_leaf(ctx, node, hash);

  Token *tok = hash->next;
  if (node->kind == CST_PP_DEFINE && tok != end && tok->kind == TK_IDENT) {
    cst_add_leaf(ctx, node, tok); // "define"
    tok = tok->next;

    if (tok != end && tok->kind == TK_IDENT) {
      cst_add_leaf(ctx, node, tok); // macro name
      tok = tok->next;

      if (tok != end && equal(tok, "(") && !tok->has_space) {
        Token *rest = NULL;
        cst_add_child(ctx, node, cst_parse_pp_params(ctx, &rest, tok, end));
        tok = rest;
      }
    }
  }

  while (tok != end) {
    cst_add_leaf(ctx, node, tok);
    tok = tok->next;
  }

  p->tok = end;
  return node;
}

// #if / #ifdef / #ifndef open a conditional group: one CST_PP_BRANCH per
// #if / #elif / #else and the closing #endif as the group's last child.
// A branch body uses the same item grammar as its surroundings, so
// nesting recurses through cst_parse_item and every branch keeps its
// tokens. An unterminated group ends at EOF or at an enclosing closer;
// an orphan #elif / #else / #endif is an ordinary directive line.
static CstNode *cst_parse_pp_if(ChibiccContext *ctx, CstParser *p) {
  CstNode *node = new_cst_node(ctx, CST_PP_IF);
  CstNode *branch = new_cst_node(ctx, CST_PP_BRANCH);
  cst_add_child(ctx, branch, cst_parse_directive_line(ctx, p));
  cst_add_child(ctx, node, branch);

  for (;;) {
    while (p->ok && p->tok && p->tok->kind != TK_EOF &&
           !cst_is_pp_terminator(p->tok)) {
      // A closer belongs to the enclosing block; leave it there and let
      // the group end unterminated instead of swallowing it.
      if (cst_is_close(p->tok))
        return node;
      cst_add_child(ctx, branch, cst_parse_item(ctx, p));
    }

    if (!p->ok || !p->tok || p->tok->kind == TK_EOF ||
        !cst_is_directive(p->tok))
      return node;

    Token *name = cst_directive_name(p->tok);
    if (equal(name, "endif")) {
      cst_add_child(ctx, node, cst_parse_directive_line(ctx, p));
      return node;
    }

    branch = new_cst_node(ctx, CST_PP_BRANCH);
    cst_add_child(ctx, branch, cst_parse_directive_line(ctx, p));
    cst_add_child(ctx, node, branch);
  }
}

// One item at statement level: a directive line (conditionals recurse),
// or a statement / declaration with the raw fallback. Every path
// consumes at least one token when one is available.
static CstNode *cst_parse_item(ChibiccContext *ctx, CstParser *p) {
  if (++p->cst->depth > MAX_CST_DEPTH) {
    p->cst->depth--;
    Token *start = p->tok;
    p->ok = true;
    Token *next = NULL;
    CstNode *item = cst_parse_fallback_statement(ctx, p->cst, &next, start);
    p->tok = next;
    return item;
  }
  CstNode *item = cst_parse_item1(ctx, p);
  p->cst->depth--;
  return item;
}

static CstNode *cst_parse_item1(ChibiccContext *ctx, CstParser *p) {
  if (cst_is_directive(p->tok)) {
    if (cst_is_pp_if(p->tok))
      return cst_parse_pp_if(ctx, p);
    return cst_parse_directive_line(ctx, p);
  }

  Token *start = p->tok;
  CstNode *item = cst_parse_statement(ctx, p);
  if (item)
    return item;

  p->tok = start;
  p->ok = true;
  Token *next = NULL;
  item = cst_parse_fallback_statement(ctx, p->cst, &next, start);
  p->tok = next;
  return item;
}

// compound-stmt = "{" (declaration | statement)* "}"
//
// A failed item is rebuilt from its first token with the raw fallback, so
// one malformed construct does not cost the rest of the block. A missing
// '}' turns the block itself into UNPARSED; parsing continues at EOF.
static void cst_parse_compound(ChibiccContext *ctx, CstParser *p, CstNode *block) {
  cst_add_leaf(ctx, block, p->tok); // '{'
  p->tok = p->tok->next;

  while (p->ok && p->tok && p->tok->kind != TK_EOF && !equal(p->tok, "}")) {
    if (cst_is_close(p->tok)) {
      CstNode *unparsed = new_cst_node(ctx, CST_UNPARSED);
      cst_add_leaf(ctx, unparsed, p->tok);
      cst_add_child(ctx, block, unparsed);
      p->tok = p->tok->next;
      continue;
    }

    cst_add_child(ctx, block, cst_parse_item(ctx, p));
  }

  if (p->ok && p->tok && p->tok->kind != TK_EOF && equal(p->tok, "}")) {
    cst_add_leaf(ctx, block, p->tok);
    p->tok = p->tok->next;
  } else {
    block->kind = CST_UNPARSED;
  }
}

// Split the top level into items. The EOF token is left for cst_parse so
// that it stays the tree's last leaf whatever the items do.
static void cst_parse_items(ChibiccContext *ctx, CstNode *parent, CstParser *p) {
  while (p->ok && p->tok && p->tok->kind != TK_EOF) {
    if (cst_is_close(p->tok)) {
      CstNode *unparsed = new_cst_node(ctx, CST_UNPARSED);
      cst_add_leaf(ctx, unparsed, p->tok);
      cst_add_child(ctx, parent, unparsed);
      p->tok = p->tok->next;
      continue;
    }

    cst_add_child(ctx, parent, cst_parse_item(ctx, p));
  }
}

// Build the CST for tokenize_file()'s output. The EOF marker has its own
// kind because it carries the file's trailing trivia and no spelling.
CstNode *cst_parse(ChibiccContext *ctx, Token *tok) {
  CstContext cst = {};
  cst_collect_typedefs(ctx, &cst, tok);

  CstParser p = {tok, true, &cst};
  CstNode *root = new_cst_node(ctx, CST_TRANSLATION_UNIT);
  cst_parse_items(ctx, root, &p);

  if (p.tok && p.tok->kind == TK_EOF)
    cst_add_leaf(ctx, root, p.tok);
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

static char *cst_kind_name(ChibiccContext *ctx, CstKind kind) {
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
  case CST_DECL:             return "DECL";
  case CST_DECL_SPECS:       return "DECL_SPECS";
  case CST_DECLARATOR:       return "DECLARATOR";
  case CST_INIT_DECLARATOR:  return "INIT_DECLARATOR";
  case CST_FUNC_DEF:         return "FUNC_DEF";
  case CST_PARAM_LIST:       return "PARAM_LIST";
  case CST_PARAM:            return "PARAM";
  case CST_TYPE_NAME:        return "TYPE_NAME";
  case CST_INITIALIZER:      return "INITIALIZER";
  case CST_DESIGNATOR:       return "DESIGNATOR";
  case CST_IF:               return "IF";
  case CST_SWITCH:           return "SWITCH";
  case CST_WHILE:            return "WHILE";
  case CST_DO:               return "DO";
  case CST_FOR:              return "FOR";
  case CST_CASE:             return "CASE";
  case CST_DEFAULT:          return "DEFAULT";
  case CST_LABEL:            return "LABEL";
  case CST_GOTO:             return "GOTO";
  case CST_BREAK:            return "BREAK";
  case CST_CONTINUE:         return "CONTINUE";
  case CST_RETURN:           return "RETURN";
  case CST_ASM:              return "ASM";
  case CST_PP_DIRECTIVE:     return "PP_DIRECTIVE";
  case CST_PP_DEFINE:        return "PP_DEFINE";
  case CST_PP_UNDEF:         return "PP_UNDEF";
  case CST_PP_INCLUDE:       return "PP_INCLUDE";
  case CST_PP_LINE:          return "PP_LINE";
  case CST_PP_PRAGMA:        return "PP_PRAGMA";
  case CST_PP_ERROR:         return "PP_ERROR";
  case CST_PP_NULL:          return "PP_NULL";
  case CST_PP_IF:            return "PP_IF";
  case CST_PP_BRANCH:        return "PP_BRANCH";
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

static void cst_dump_node(ChibiccContext *ctx, CstNode *node, FILE *out, int depth) {
  for (int i = 0; i < depth; i++)
    fputs("  ", out);

  fputs(cst_kind_name(ctx, node->kind), out);

  if (node->tok && node->tok->raw_len) {
    fputc(' ', out);
    cst_print_escaped(out, node->tok->raw_loc, node->tok->raw_len);
  }
  fputc('\n', out);

  for (int i = 0; i < node->child_count; i++)
    cst_dump_node(ctx, node->children[i], out, depth + 1);
}

// Print the tree shape, two spaces per level. Leaf text is the token's
// raw spelling (not its trivia); the dump locks the tree, the round-trip
// locks the bytes.
void cst_dump(ChibiccContext *ctx, CstNode *root, FILE *out) {
  cst_dump_node(ctx, root, out, 0);
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
