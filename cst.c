#include "chibicc.h"

// Lossless concrete syntax tree over the raw token stream (PLAN.md stage
// C1.1). The tree at this stage is a flat spine: every token of the input
// becomes one leaf under the translation unit, with the EOF token last.
// Structural kinds arrive in C1.2 and later; the lossless contract does
// not depend on them.
//
// Invariants (RESULT.md I1-I6): the leaves in order are the token stream's
// tokens, each exactly once; a leaf's text is its trivia followed by its
// raw spelling, so the leaves rebuild the input file byte for byte. The
// parser is total: it never reports a diagnostic and never exits.

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

// Build the CST for tokenize_file()'s output. Every token becomes a leaf;
// the EOF marker has its own kind because it carries the file's trailing
// trivia and no spelling.
CstNode *cst_parse(Token *tok) {
  CstNode *root = new_cst_node(CST_TRANSLATION_UNIT);

  for (; tok; tok = tok->next) {
    CstNode *leaf = new_cst_node(tok->kind == TK_EOF ? CST_EOF : CST_TOKEN);
    leaf->tok = tok;
    cst_add_child(root, leaf);
  }
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
