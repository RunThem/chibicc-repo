#include "chibicc.h"

// Lossless concrete syntax tree over the raw token stream (PLAN.md stage
// C1). Every token of the input becomes one leaf, in order; the leaves'
// trivia and spellings rebuild the input file byte for byte.
//
// The C1 skeleton groups delimiters and splits statements. At statement
// level (the top level and inside a block) an item is a ';'-terminated
// CST_STMT, a lone ';' (CST_NULL_STMT), or a '{...}' body (CST_BLOCK).
// Inside a statement, '(' / '[' / '{' start a group whose interior is
// kept as raw groups and leaves: semicolons in a for-header are not
// statement breaks. A group or block without its closing token, and a
// closer that has no opener at this level, become CST_UNPARSED; the
// tokens it already holds stay in the tree.
//
// The lossless contract does not depend on the shape: the parser is
// total, never reports a diagnostic and never exits (RESULT.md I1-I6).
// Declarations and expressions arrive in C2.

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
// raw mode: the interior is not split into statements, only nested
// delimiters are grouped. On a missing closer the node becomes
// CST_UNPARSED but keeps the opener and everything it consumed.
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

// Parse one statement: tokens up to and including a ';' at this level, a
// group per delimiter, and stop before a closer that belongs to an outer
// level. A statement cut short by a closer (or EOF) keeps the tokens it
// has; the skeleton does not guess where it should have ended.
static CstNode *cst_parse_statement(Token **rest, Token *tok) {
  CstNode *stmt = new_cst_node(CST_STMT);

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

static CstNode *cst_parse_block(Token **rest, Token *tok) {
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
      cst_add_child(block, cst_parse_block(&p, p));
      continue;
    }
    cst_add_child(block, cst_parse_statement(&p, p));
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
static void cst_parse_items(CstNode *parent, Token **rest) {
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
      cst_add_child(parent, cst_parse_block(&p, p));
      continue;
    }
    cst_add_child(parent, cst_parse_statement(&p, p));
  }
  *rest = p;
}

// Build the CST for tokenize_file()'s output. The EOF marker has its own
// kind because it carries the file's trailing trivia and no spelling.
CstNode *cst_parse(Token *tok) {
  CstNode *root = new_cst_node(CST_TRANSLATION_UNIT);

  Token *p = tok;
  cst_parse_items(root, &p);

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
  case CST_STMT:             return "STMT";
  case CST_NULL_STMT:        return "NULL_STMT";
  case CST_UNPARSED:         return "UNPARSED";
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
