// Layer 4: semantic analysis (AGENTS.md stage 4). Names are resolved,
// types annotated, constants evaluated, aggregates laid out, and the
// conclusions the language requires are recorded on the layer-3 tree.

#ifndef LIBCP_SEMA_H
#define LIBCP_SEMA_H

#include <libcp/ast.h>

// Runs semantic analysis over the parser's output: name resolution,
// type annotation, constant evaluation, aggregate layout, and
// control-flow binding. Returns the list of global objects for codegen.
LIBCP_API LibcpObj *libcp_sema(LibcpContext *ctx, LibcpNode *toplevel);

#endif
