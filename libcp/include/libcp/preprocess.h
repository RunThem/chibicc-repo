// Preprocessing (PLAN L3.1b): a token stream in, a token stream out. The
// macro definitions, conditionals and include state live in the context.
//
// The call consumes the list it is given: it rewrites the input's `next`
// links in place as it splices in expansions. A consumer that needs the
// unexpanded stream or its CST must take it before this call, or tokenize
// the file a second time.

#ifndef LIBCP_PREPROCESS_H
#define LIBCP_PREPROCESS_H

#include <libcp/token.h>

LIBCP_API LibcpToken *libcp_preprocess(LibcpContext *ctx, LibcpToken *tok);

#endif
