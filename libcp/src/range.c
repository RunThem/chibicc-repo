// LibcpNode source ranges (PLAN-l5 W5): the anchor-token extremes of a
// subtree. Every node carries a representative token; this walk collects
// the smallest and largest anchors by (file, offset), so a consumer can
// map a subtree back to a source span without rewalking the tokens.
//
// Scope notes, deliberate for the MVP:
// - `next` chains are siblings, not children: the range covers the
//   subtree rooted at the argument only.
// - LibcpInitializer trees are not descended; a
//   declaration's range covers its node anchors.
// - Tokens from synthetic buffers (macro bodies, pastes) compare by
//   file pointer first, which is stable but arbitrary across buffers.

#include "libcp.h"

typedef struct {
  LibcpFile *file;
  long off;
} LibcpPos;

static LibcpPos pos_of(LibcpToken *tok) {
  return (LibcpPos){tok->file, tok->loc - tok->file->contents};
}

static int pos_lt(LibcpPos a, LibcpPos b) {
  if (a.file != b.file)
    return a.file < b.file;
  return a.off < b.off;
}

static void consider(LibcpToken *tok, LibcpToken **start, LibcpToken **end) {
  if (!tok)
    return;
  if (!*start || pos_lt(pos_of(tok), pos_of(*start)))
    *start = tok;
  if (!*end || pos_lt(pos_of(*end), pos_of(tok)))
    *end = tok;
}

static void walk(const LibcpNode *node, LibcpToken **start, LibcpToken **end) {
  if (!node)
    return;

  consider(node->tok, start, end);
  consider(node->name_tok, start, end);
  consider(node->arrow_tok, start, end);
  consider(node->colon_tok, start, end);

  walk(node->lhs, start, end);
  walk(node->rhs, start, end);
  walk(node->cond, start, end);
  walk(node->then, start, end);
  walk(node->els, start, end);
  walk(node->init, start, end);
  walk(node->inc, start, end);
  walk(node->body, start, end);
  walk(node->args, start, end);
  walk(node->generic_sel, start, end);
  walk(node->spec_decls, start, end);
  walk(node->begin_expr, start, end);
  walk(node->end_expr, start, end);
  walk(node->cas_addr, start, end);
  walk(node->cas_old, start, end);
  walk(node->cas_new, start, end);
  walk(node->atomic_expr, start, end);
}

void libcp_node_source_range(const LibcpNode *node, LibcpToken **start,
                             LibcpToken **end) {
  *start = NULL;
  *end = NULL;
  walk(node, start, end);
}
