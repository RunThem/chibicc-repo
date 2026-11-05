// 第 4 层: 语义分析 (AGENTS.md 第 4 阶段). 完成名字解析, 类型标注,
// 常量求值, 聚合体布局, 并把语言要求的语义结论记录到第 3 层树上.

#ifndef LIBCP_SEMA_H
#define LIBCP_SEMA_H

#include <libcp/ast.h>

// 对解析器输出运行语义分析: 名字解析, 类型标注, 常量求值, 聚合体布局,
// 以及控制流绑定. 返回供 codegen 使用的全局对象链表.
LIBCP_API LibcpObj* libcp_sema(LibcpContext* ctx, LibcpNode* toplevel);

#endif
