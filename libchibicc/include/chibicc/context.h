// The context object (PLAN L2/L3.1b): one context is one translation unit.
// The context is opaque; its fields are an implementation detail.
// Different contexts may live in different threads; one context must not
// be used concurrently from two threads.

#ifndef CHIBICC_CONTEXT_H
#define CHIBICC_CONTEXT_H

#include <stdbool.h>

#include <chibicc/diagnostic.h>

typedef struct ChibiccContext ChibiccContext;

ChibiccContext *chibicc_context_new(void);
void chibicc_context_free(ChibiccContext *ctx);

// Releases every per-TU allocation and clears the per-TU state, keeping
// the configuration (include paths, macro operations, diagnostic
// callback). Every pointer returned by this context is invalid
// afterwards. An error leaves the context inconsistent: reset before
// reusing it.
void chibicc_context_reset(ChibiccContext *ctx);

void chibicc_context_set_diagnostic(ChibiccContext *ctx,
                                    ChibiccDiagnosticFn fn, void *user);
bool chibicc_context_failed(ChibiccContext *ctx);

void chibicc_context_add_include_path(ChibiccContext *ctx, char *path);
void chibicc_context_define(ChibiccContext *ctx, char *name, char *buf);
void chibicc_context_undef(ChibiccContext *ctx, char *name);

#endif
