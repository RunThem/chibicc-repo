// Layer 2: the lossless concrete syntax tree (AGENTS.md stage 2). It is
// built from tokenize_file()'s direct output, before preprocessing:
// preprocessor directives are nodes, every conditional branch is kept,
// every grammar token and punctuator is an explicit leaf, and the leaves
// rebuild the input file byte for byte.

#ifndef CHIBICC_CST_H
#define CHIBICC_CST_H

#include <stdio.h>

#include <chibicc/token.h>

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
} CstKind;

typedef struct CstNode CstNode;
struct CstNode {
  CstKind kind;
  Token *tok;          // Set on leaves (CST_TOKEN / CST_EOF)
  CstNode **children;  // Set on inner nodes
  int child_count;
  int child_cap;
};

typedef CstNode ChibiccCstNode;
typedef CstKind ChibiccCstKind;

CstNode *chibicc_cst_parse(ChibiccContext *ctx, Token *tok);
void chibicc_cst_reconstruct(ChibiccContext *ctx, CstNode *root, FILE *out);
void chibicc_cst_dump(ChibiccContext *ctx, CstNode *root, FILE *out);
bool chibicc_cst_verify(ChibiccContext *ctx, CstNode *root, Token *tok);

#endif
