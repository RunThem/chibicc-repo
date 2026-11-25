// 后端支持面: 消费者后端构造第 4 层节点所需的入口. 这不是库自带的
// 第 5 层; 它只把"读取 sema 标注树并降级"所需的最小构造与类型工具
// 公开出来, 构造规则本身仍由库拥有.
//
// 节点构造器把对象分配在当前 context 的 per-TU 登记链上: 对象归
// context 所有, 与同一翻译单元的其余分配一起活到 libcp_context_reset /
// libcp_context_free 为止, 不提供逐对象 free. 需要已定型的节点时,
// 调用 libcp_node_annotate_type 让 sema 的规则填充 node->ty 并插入
// 标准要求的隐式转换.
//
// libcp_type_builtin 返回的类型记录是跨 context 共享的只读单例;
// 消费者不得修改它们.

#ifndef LIBCP_BACKEND_H
#define LIBCP_BACKEND_H

#include <libcp/ast.h>
#include <libcp/context.h>
#include <stdint.h>

// 内建类型. libcp_type_builtin 把每一项映射到 type.c 中共享的类型
// 单例; 该查询不触碰 per-TU 状态, 因此不带 context.
typedef enum {
  BT_VOID,
  BT_BOOL,
  BT_CHAR,
  BT_UCHAR,
  BT_SHORT,
  BT_USHORT,
  BT_INT,
  BT_UINT,
  BT_LONG,
  BT_ULONG,
  BT_FLOAT,
  BT_DOUBLE,
  BT_LDOUBLE,
} LibcpBuiltinType;

LIBCP_API LibcpType* libcp_type_builtin(LibcpBuiltinType kind);

// 节点构造. 分配自当前 context; 返回的节点归 context 所有.
LIBCP_API LibcpNode* libcp_node_new(LibcpContext* ctx, LibcpNodeKind kind, LibcpToken* tok);
LIBCP_API LibcpNode* libcp_node_binary(LibcpContext* ctx,
                                       LibcpNodeKind kind,
                                       LibcpNode* lhs,
                                       LibcpNode* rhs,
                                       LibcpToken* tok);
LIBCP_API LibcpNode*
    libcp_node_unary(LibcpContext* ctx, LibcpNodeKind kind, LibcpNode* expr, LibcpToken* tok);
LIBCP_API LibcpNode* libcp_node_num(LibcpContext* ctx, int64_t val, LibcpToken* tok);
LIBCP_API LibcpNode* libcp_node_long(LibcpContext* ctx, int64_t val, LibcpToken* tok);

// 已定型的二元算术节点: 先标注两个操作数, 再施加常规算术转换, 返回节点
// 的类型就是结果类型.
LIBCP_API LibcpNode* libcp_node_arith(LibcpContext* ctx,
                                      LibcpNodeKind kind,
                                      LibcpNode* lhs,
                                      LibcpNode* rhs,
                                      LibcpToken* tok);

// 已绑定名字的变量节点与 VLA 指示符.
LIBCP_API LibcpNode* libcp_node_var(LibcpContext* ctx, LibcpObj* var, LibcpToken* tok);
LIBCP_API LibcpNode* libcp_node_vla_ptr(LibcpContext* ctx, LibcpObj* var, LibcpToken* tok);

// 内建 alloca 的调用节点. 只在 sema 安装好内建声明之后可用, 即同一个
// context 至少跑过一次 libcp_sema.
LIBCP_API LibcpNode* libcp_node_alloca(LibcpContext* ctx, LibcpNode* sz);

// 已定型的隐式转换节点; 目标类型被浅拷贝, 副本的 origin 指向 ty.
LIBCP_API LibcpNode* libcp_node_cast(LibcpContext* ctx, LibcpNode* expr, LibcpType* ty);

// 用 sema 的规则标注一棵子树: 填充 node->ty, 插入标准要求的隐式转换,
// 并运行需要已定型树的检查. 幂等: 已经带 ty 的节点直接返回.
LIBCP_API void libcp_node_annotate_type(LibcpContext* ctx, LibcpNode* node);

// 从基础类型构造指向它的指针类型 (8 字节大小与对齐, 标记无符号).
LIBCP_API LibcpType* libcp_type_pointer_to(LibcpContext* ctx, LibcpType* base);

// 函数体内的标签引用. sema 为每个定义函数收集 goto 语句与 [GNU]
// 标签值 (`&&label`) 节点, 顺序是标注遍历的访问顺序; count 返回个数,
// 没有记录时为 0, at 取第 index 个, 越界 (含负数) 返回 NULL. 数组归
// context 所有, 活到 reset. 引用是节点链的一部分, 不要释放或改写.
LIBCP_API int libcp_label_ref_count(LibcpContext* ctx, LibcpObj* fn);
LIBCP_API LibcpNode* libcp_label_ref_at(LibcpContext* ctx, LibcpObj* fn, int index);

// 取一个新的唯一编号并消耗它: 同一个号不会再从同一个 context 发出.
// 编号与库为字符串字面量全局对象, 块作用域 static, 复合字面量与
// 控制流标签所用的编号来自同一个计数器, 所以消费者按返回的号构造
// 自己的名字 (例如 `.L.<id>`) 不会与库生成的对象冲突. 计数器随
// libcp_context_reset 回到起点.
LIBCP_API int libcp_context_take_unique_name(LibcpContext* ctx);

// 返回式诊断. 与库内部的 error / warn 路径共用同一套格式化与回调交付,
// 但它不 longjmp: 调用者拿到控制权后自行决定如何处理. 错误把 context
// 标记为失败 (libcp_context_failed 为 true), 警告不标记. tok 为锚点
// token, 可以为 NULL, 此时诊断没有文件与行列, 回调的 filename 为 NULL.
// fmt 走 printf 格式检查.
LIBCP_API void libcp_diagnostic_report(LibcpContext* ctx,
                                       LibcpToken* tok,
                                       LibcpDiagnosticSeverity severity,
                                       const char* fmt,
                                       ...) __attribute__((format(printf, 4, 5)));

#endif
