// Preprocessing (PLAN L3.1b): a token stream in, a token stream out. The
// macro definitions, conditionals and include state live in the context.
//
// 预处理 (PLAN L3.1b): 输入一个 token 流, 输出一个 token 流. 宏定义,
// 条件与 include 状态都存放在 context 中.
//
// The call consumes the list it is given: it rewrites the input's `next`
// links in place as it splices in expansions. A consumer that needs the
// unexpanded stream or its CST must take it before this call, or tokenize
// the file a second time.
//
// 该调用会消耗传入的链表: 它在拼接展开结果时就地改写输入的 `next`
// 链接. 若消费者需要未展开的流或其 CST, 必须在调用之前取得,
// 或者再次 tokenize 该文件.

#ifndef LIBCP_PREPROCESS_H
#define LIBCP_PREPROCESS_H

#include <libcp/token.h>

LIBCP_API LibcpToken *libcp_preprocess(LibcpContext *ctx, LibcpToken *tok);

#endif
