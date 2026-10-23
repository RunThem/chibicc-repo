// Preprocessing (PLAN L3.1b): a token stream in, a token stream out. The
// macro definitions, conditionals and include state live in the context.
//
// The call consumes the list it is given: it rewrites the input's `next`
// links in place as it splices in expansions. A consumer that needs the
// unexpanded stream or its CST must take it before this call, or tokenize
// the file a second time.

#ifndef CHIBICC_PREPROCESS_H
#define CHIBICC_PREPROCESS_H

#include <chibicc/token.h>

CHIBICC_API ChibiccToken *chibicc_preprocess(ChibiccContext *ctx, ChibiccToken *tok);

#endif
