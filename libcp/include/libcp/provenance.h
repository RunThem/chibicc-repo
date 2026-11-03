// Macro expansion provenance (plans/PLAN-provenance.md): every token the
// preprocessor produces by expanding a macro carries a chain of
// LibcpMacroProv nodes, one per expansion layer, innermost first; a token
// read from the source carries NULL. The records a chain points to are
// built once per expansion, immutable afterwards, and live as long as
// the tokens (arena allocation, never freed). LibcpMacro, LibcpMacroArg and
// LibcpMacroParam stay private to preprocess.c; these accessors are their
// public face.
//
// 宏展开 provenance (plans/PLAN-provenance.md): 预处理器通过展开宏产生的
// 每个 token 都携带一条 LibcpMacroProv 节点链, 每个展开层一个节点,
// 最内层在前; 从源码读入的 token 携带 NULL. 链指向的记录在每次展开时
// 构建一次, 之后不可变, 并与 token 同生命周期 (arena 分配, 从不释放).
// LibcpMacro, LibcpMacroArg 与 LibcpMacroParam 保持为 preprocess.c 私有;
// 这些访问器是它们的公共接口.

#ifndef LIBCP_PROVENANCE_H
#define LIBCP_PROVENANCE_H

#include <stdio.h>

#include <libcp/token.h>

typedef struct libcp_macro_expansion LibcpMacroExpansion;
LIBCP_API LibcpMacroProv *libcp_token_provenance(LibcpToken *tok);
LIBCP_API LibcpMacroProv *libcp_macro_prov_next(LibcpMacroProv *p);
LIBCP_API LibcpMacroExpansion *libcp_macro_prov_expansion(LibcpMacroProv *p);
LIBCP_API int libcp_macro_prov_arg_index(LibcpMacroProv *p);

LIBCP_API LibcpToken *libcp_macro_expansion_name(LibcpMacroExpansion *e);
// rparen; name if none
// 右括号; 没有参数时为宏名
LIBCP_API LibcpToken *libcp_macro_expansion_end(LibcpMacroExpansion *e);
// the macro's spelling
// 宏的拼写
LIBCP_API char *libcp_macro_expansion_macro(LibcpMacroExpansion *e);
// #define name; NULL if none
// #define 名称; 没有则为 NULL
LIBCP_API LibcpToken *libcp_macro_expansion_def(LibcpMacroExpansion *e);
LIBCP_API LibcpToken *libcp_macro_expansion_arg(LibcpMacroExpansion *e, int i);
LIBCP_API int libcp_macro_expansion_arg_count(LibcpMacroExpansion *e);
LIBCP_API bool libcp_macro_expansion_is_builtin(LibcpMacroExpansion *e);

// The read-only provenance channels (plans/PLAN.md 判据 5). The dump prints one
// line per non-EOF token of the preprocessed stream; the verifier checks
// every chain and returns false at the first violation. Neither touches
// the compilation path.
//
// 只读的 provenance 通道 (plans/PLAN.md 判据 5). dump 为预处理流中每个
// 非 EOF token 打印一行; verifier 检查每条链, 并在第一处违规时返回
// false. 两者都不触碰编译路径.
LIBCP_API void libcp_expansion_dump(LibcpContext *ctx, LibcpToken *tok, FILE *out);
LIBCP_API bool libcp_expansion_verify(LibcpContext *ctx, LibcpToken *tok);

#endif
