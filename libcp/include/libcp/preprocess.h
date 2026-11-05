// 预处理 (PLAN L3.1b): 输入一个 token 流, 输出一个 token 流. 宏定义,
// 条件与 include 状态都存放在 context 中.
//
// 该调用会消耗传入的链表: 它在拼接展开结果时就地改写输入的 `next`
// 链接. 若消费者需要未展开的流或其 CST, 必须在调用之前取得,
// 或者再次 tokenize 该文件.

#ifndef LIBCP_PREPROCESS_H
#define LIBCP_PREPROCESS_H

#include <libcp/token.h>

LIBCP_API LibcpToken* libcp_preprocess(LibcpContext* ctx, LibcpToken* tok);

#endif
