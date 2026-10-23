// Layer 2: the lossless concrete syntax tree (AGENTS.md stage 2). It is
// built from tokenize_file()'s direct output, before preprocessing:
// preprocessor directives are nodes, every conditional branch is kept,
// every grammar token and punctuator is an explicit leaf, and the leaves
// rebuild the input file byte for byte.

#ifndef LIBCP_CST_H
#define LIBCP_CST_H

#include <stdio.h>

#include <libcp/token.h>

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
} LibcpCstKind;

typedef struct libcp_cst_node LibcpCstNode;
struct libcp_cst_node {
  LibcpCstKind kind;
  LibcpToken *tok;          // Set on leaves (CST_TOKEN / CST_EOF)
  // `children[0..child_count)` are the live children; the array's
  // capacity is private to cst.c.
  LibcpCstNode **children;  // Set on inner nodes
  int child_count;
};

LIBCP_API LibcpCstNode *libcp_cst_parse(LibcpContext *ctx, LibcpToken *tok);
LIBCP_API void libcp_cst_reconstruct(LibcpContext *ctx, LibcpCstNode *root, FILE *out);
LIBCP_API void libcp_cst_dump(LibcpContext *ctx, LibcpCstNode *root, FILE *out);
LIBCP_API bool libcp_cst_verify(LibcpContext *ctx, LibcpCstNode *root, LibcpToken *tok);

#endif
