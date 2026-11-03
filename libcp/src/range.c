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
//
// LibcpNode 的源码范围 (PLAN-l5 W5): 子树的锚点 token 极值. 每个节点
// 都携带一个代表 token; 本次遍历按 (file, offset) 收集最小与最大的
// 锚点, 因此消费者可以把子树映射回源码区间, 而不必重新遍历 token.
//
// 范围说明, 这些都是 MVP 的有意取舍:
// - `next` 链是兄弟节点, 不是子节点: 范围只覆盖以参数为根的子树.
// - 不下降进 LibcpInitializer 树; 声明的范围覆盖它自身的节点锚点.
// - 来自合成缓冲区 (宏体, 拼接) 的 token 先按 file 指针比较,
//   这是稳定的, 但跨缓冲区比较时顺序是任意的.

#include "libcp.h"

// token 位置: 所在文件加文件内偏移, 比较先后时用.
typedef struct {
  LibcpFile* file;
  long off;
} LibcpPos;

// 取 token 的 (file, offset) 位置.
static LibcpPos pos_of(LibcpToken* tok) {
  return (LibcpPos){tok->file, tok->loc - tok->file->contents};
}

// 按 (file, offset) 判断 a 是否位于 b 之前; 文件指针比较结果稳定,
// 但不同合成缓冲区之间的先后是任意的.
static int pos_lt(LibcpPos a, LibcpPos b) {
  if (a.file != b.file)
    return a.file < b.file;
  return a.off < b.off;
}

// 用一个锚点 token 更新当前的起止极值; NULL 锚点直接忽略.
static void consider(LibcpToken* tok, LibcpToken** start, LibcpToken** end) {
  if (!tok)
    return;
  if (!*start || pos_lt(pos_of(tok), pos_of(*start)))
    *start = tok;
  if (!*end || pos_lt(pos_of(*end), pos_of(tok)))
    *end = tok;
}

// 递归遍历子树: 先收集节点自带的锚点 token, 再下降进各个子节点.
// 各字段可能为 NULL, consider 与递归都做空判断.
static void walk(const LibcpNode* node, LibcpToken** start, LibcpToken** end) {
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

// 计算 node 子树的源码范围, 通过 *start / *end 返回首尾锚点 token.
// 空子树时两者都为 NULL.
void libcp_node_source_range(const LibcpNode* node, LibcpToken** start, LibcpToken** end) {
  *start = NULL;
  *end   = NULL;
  walk(node, start, end);
}
