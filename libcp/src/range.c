// LibcpNode 的源码范围 (PLAN-l5 W5): 子树的锚点 token 极值. 每个节点
// 都携带一个代表 token; 本次遍历按 (file, offset) 收集最小与最大的
// 锚点, 因此消费者可以把子树映射回源码区间, 而不必重新遍历 token.
//
// 范围说明, 这些都是 MVP 的有意取舍:
// - `next` 链是兄弟节点, 不是子节点: 范围只覆盖以参数为根的子树.
// - 不下降进 LibcpInitializer 树; 声明的范围覆盖它自身的节点锚点.
// - 来自合成缓冲区 (宏体, 拼接) 的 token 先按 file 指针比较,
//   这是稳定的, 但跨缓冲区比较时顺序是任意的.
//
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
  LibcpNodeFamily fam = libcp_node_family(node->kind);
  if (fam == LIBCP_NF_DECL)
    consider(node->decl.name_tok, start, end);
  if (fam == LIBCP_NF_ACCESS)
    consider(node->access.arrow_tok, start, end);
  if (fam == LIBCP_NF_CASE)
    consider(node->case_.colon_tok, start, end);

  walk(node->lhs, start, end);
  walk(node->rhs, start, end);
  if (fam == LIBCP_NF_CTRL) {
    walk(node->ctrl.cond, start, end);
    walk(node->ctrl.then, start, end);
    walk(node->ctrl.els, start, end);
    walk(node->ctrl.init, start, end);
    walk(node->ctrl.inc, start, end);
    walk(node->ctrl.args, start, end);
    walk(node->ctrl.generic_sel, start, end);
  }
  if (fam == LIBCP_NF_DECL)
    walk(node->decl.body, start, end);
  walk(node->spec_decls, start, end);
  if (fam == LIBCP_NF_CASE) {
    walk(node->case_.begin_expr, start, end);
    walk(node->case_.end_expr, start, end);
  }
  if (fam == LIBCP_NF_CAS) {
    walk(node->cas.cas_addr, start, end);
    walk(node->cas.cas_old, start, end);
    walk(node->cas.cas_new, start, end);
  }
}

// 计算 node 子树的源码范围, 通过 *start / *end 返回首尾锚点 token.
// 空子树时两者都为 NULL.
void libcp_node_source_range(const LibcpNode* node, LibcpToken** start, LibcpToken** end) {
  *start = NULL;
  *end   = NULL;
  walk(node, start, end);
}
