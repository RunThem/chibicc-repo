// Preprocessing (PLAN L3.1b): a token stream in, a token stream out. The
// macro definitions, conditionals and include state live in the context.

#ifndef CHIBICC_PREPROCESS_H
#define CHIBICC_PREPROCESS_H

#include <chibicc/token.h>

CHIBICC_API ChibiccToken *chibicc_preprocess(ChibiccContext *ctx, ChibiccToken *tok);

#endif
