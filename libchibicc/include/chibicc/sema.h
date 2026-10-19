// Layer 4: semantic analysis (AGENTS.md stage 4). Names are resolved,
// types annotated, constants evaluated, aggregates laid out, and the
// conclusions the language requires are recorded on the layer-3 tree.

#ifndef CHIBICC_SEMA_H
#define CHIBICC_SEMA_H

#include <chibicc/ast.h>

// Runs semantic analysis over the parser's output: name resolution,
// type annotation, constant evaluation, aggregate layout, and
// control-flow binding. Returns the list of global objects for codegen.
CHIBICC_API ChibiccObj *chibicc_sema(ChibiccContext *ctx, ChibiccNode *toplevel);

#endif
