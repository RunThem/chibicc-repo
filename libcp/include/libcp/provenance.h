// 宏展开 provenance (plans/PLAN-provenance.md): 预处理器通过展开宏产生的
// 每个 token 都携带一条 LibcpMacroProv 节点链, 每个展开层一个节点,
// 最内层在前; 从源码读入的 token 携带 NULL. 链指向的记录在每次展开时
// 构建一次, 之后不可变, 并与 token 同生命周期 (arena 分配, 从不释放).
// LibcpMacro, LibcpMacroArg 与 LibcpMacroParam 保持为 preprocess.c 私有;
// 这些访问器是它们的公共接口.

#ifndef LIBCP_PROVENANCE_H
#define LIBCP_PROVENANCE_H

#include <libcp/token.h>
#include <stdio.h>

typedef struct libcp_macro_expansion LibcpMacroExpansion;
LIBCP_API LibcpMacroProv* libcp_token_provenance(LibcpToken* tok);
LIBCP_API LibcpMacroProv* libcp_macro_prov_next(LibcpMacroProv* p);
LIBCP_API LibcpMacroExpansion* libcp_macro_prov_expansion(LibcpMacroProv* p);
LIBCP_API int libcp_macro_prov_arg_index(LibcpMacroProv* p);

LIBCP_API LibcpToken* libcp_macro_expansion_name(LibcpMacroExpansion* e);
// 右括号; 没有参数时为宏名
LIBCP_API LibcpToken* libcp_macro_expansion_end(LibcpMacroExpansion* e);
// 宏的拼写
LIBCP_API char* libcp_macro_expansion_macro(LibcpMacroExpansion* e);
// #define 名称; 没有则为 NULL
LIBCP_API LibcpToken* libcp_macro_expansion_def(LibcpMacroExpansion* e);
LIBCP_API LibcpToken* libcp_macro_expansion_arg(LibcpMacroExpansion* e, int i);
LIBCP_API int libcp_macro_expansion_arg_count(LibcpMacroExpansion* e);
LIBCP_API bool libcp_macro_expansion_is_builtin(LibcpMacroExpansion* e);

// 只读的 provenance 通道 (plans/PLAN.md 判据 5). dump 为预处理流中每个
// 非 EOF token 打印一行; verifier 检查每条链, 并在第一处违规时返回
// false. 两者都不触碰编译路径.
LIBCP_API void libcp_expansion_dump(LibcpContext* ctx, LibcpToken* tok, FILE* out);
LIBCP_API bool libcp_expansion_verify(LibcpContext* ctx, LibcpToken* tok);

#endif
