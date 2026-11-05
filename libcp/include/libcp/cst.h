// 第 2 层: 无损具体语法树 (AGENTS.md 第 2 阶段). 它基于 tokenize_file()
// 的直接输出, 在预处理之前构建: 预处理指令是节点, 每个条件分支都保留,
// 每个语法 token 与标点都是显式叶子, 叶子可以逐字节重建输入文件.

#ifndef LIBCP_CST_H
#define LIBCP_CST_H

#include <libcp/token.h>
#include <stdio.h>

typedef enum {
  CST_TRANSLATION_UNIT,
  CST_TOKEN,
  CST_EOF,
  CST_GROUP_PAREN,
  CST_GROUP_BRACKET,
  CST_GROUP_BRACE,
  CST_BLOCK,
  CST_NULL_STMT,
  CST_UNPARSED,
  CST_EXPR_STMT,
  CST_BINARY,
  CST_UNARY,
  CST_POSTFIX,
  CST_CAST,
  CST_COND,
  CST_ASSIGN,
  CST_COMMA,
  CST_PAREN,
  CST_PRIMARY,
  CST_ARG_LIST,
  CST_INIT_LIST,
  CST_COMPOUND_LITERAL,
  CST_DECL,
  CST_DECL_SPECS,
  CST_DECLARATOR,
  CST_INIT_DECLARATOR,
  CST_FUNC_DEF,
  CST_PARAM_LIST,
  CST_PARAM,
  CST_TYPE_NAME,
  CST_INITIALIZER,
  CST_DESIGNATOR,
  CST_IF,
  CST_SWITCH,
  CST_WHILE,
  CST_DO,
  CST_FOR,
  CST_CASE,
  CST_DEFAULT,
  CST_LABEL,
  CST_GOTO,
  CST_BREAK,
  CST_CONTINUE,
  CST_RETURN,
  CST_ASM,
  CST_PP_DIRECTIVE,
  CST_PP_DEFINE,
  CST_PP_UNDEF,
  CST_PP_INCLUDE,
  CST_PP_LINE,
  CST_PP_PRAGMA,
  CST_PP_ERROR,
  CST_PP_NULL,
  CST_PP_IF,
  CST_PP_BRANCH,
  // GNU 语句表达式 "({...})" 与 _Generic 关联参数
  // "type: expr" / "default: expr" (由 PLAN-l5 W1 添加).
  CST_STMT_EXPR,
  CST_GENERIC_ASSOC,
} LibcpCstKind;

typedef struct libcp_cst_node LibcpCstNode;
struct libcp_cst_node {
  LibcpCstKind kind;
  LibcpToken* tok;  // 在叶子上设置 (CST_TOKEN / CST_EOF)
  // `children[0..child_count)` 是有效子节点; 数组容量是 cst.c 私有的.
  LibcpCstNode** children;  // 在内部节点上设置
  int child_count;
};

LIBCP_API LibcpCstNode* libcp_cst_parse(LibcpContext* ctx, LibcpToken* tok);
// 预置一个 typedef 名, 这类名字在预处理后本会来自被包含的头文件:
// CST 在预处理之前构建, 因此它的声明 oracle 看不到这些名字,
// 由消费者提供 (例如 "va_list"). 属于配置: 跨 reset 保留.
LIBCP_API void libcp_cst_seed_typedef(LibcpContext* ctx, char* name);
LIBCP_API void libcp_cst_reconstruct(LibcpContext* ctx, LibcpCstNode* root, FILE* out);
LIBCP_API void libcp_cst_dump(LibcpContext* ctx, LibcpCstNode* root, FILE* out);
LIBCP_API bool libcp_cst_verify(LibcpContext* ctx, LibcpCstNode* root, LibcpToken* tok);

#endif
