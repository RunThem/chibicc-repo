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

// 按 (file, offset) 判断 a 是否位于 b 之前; 文件指针比较结果稳定,
// 但不同合成缓冲区之间的先后是任意的.
static int pos_lt(LibcpSourcePos a, LibcpSourcePos b) {
  if (a.file != b.file)
    return a.file < b.file;
  return a.offset < b.offset;
}

// 用一个锚点 token 更新当前的起止极值; NULL 锚点直接忽略.
static void consider(LibcpToken* tok, LibcpToken** start, LibcpToken** end) {
  if (!tok)
    return;
  LibcpSourcePos pos = libcp_token_source_pos(tok);
  if (!*start || pos_lt(pos, libcp_token_source_pos(*start)))
    *start = tok;
  if (!*end || pos_lt(libcp_token_source_pos(*end), pos))
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

  // 子节点顺序与枚举一致; 列表字段只取头节点 (is_list 在这里不重要,
  // 兄弟不是子节点). generic_sel 是某个关联结果的别名, 枚举不返回它,
  // 因此这里不再重复下降.
  for (int i = 0, count = libcp_node_child_count(node); i < count; i++)
    walk(libcp_node_child_at(node, i).node, start, end);
}

// 计算 node 子树的源码范围, 通过 *start / *end 返回首尾锚点 token.
// 空子树时两者都为 NULL.
void libcp_node_source_range(const LibcpNode* node, LibcpToken** start, LibcpToken** end) {
  *start = NULL;
  *end   = NULL;
  walk(node, start, end);
}
