// The context object (PLAN L2/L3.1b): one context is one translation unit.
// The context is opaque; its fields are an implementation detail.
// Different contexts may live in different threads; one context must not
// be used concurrently from two threads. Internally every function that
// touches the arena, diagnostics, or context state takes the context as
// its first parameter - there is no thread-local current context, so a
// diagnostic callback may create, use, and free another context without
// disturbing the outer call.

#ifndef CHIBICC_CONTEXT_H
#define CHIBICC_CONTEXT_H

#include <stdbool.h>

#include <chibicc/diagnostic.h>

// Marks a D7 function as exported from the shared library; the library
// itself is built with -fvisibility=hidden (PLAN L3.3b). Every header
// that declares functions includes this one. chibicc defines
// __chibicc__ and does not parse a leading visibility attribute; it
// only builds the static archive, where the marker is not needed.
#if defined(__chibicc__)
#define CHIBICC_API
#elif defined(__GNUC__) || defined(__clang__)
#define CHIBICC_API __attribute__((visibility("default")))
#else
#define CHIBICC_API
#endif

typedef struct ChibiccContext ChibiccContext;

CHIBICC_API ChibiccContext *chibicc_context_new(void);
CHIBICC_API void chibicc_context_free(ChibiccContext *ctx);

// Releases every per-TU allocation and clears the per-TU state, keeping
// the configuration (include paths, macro operations, diagnostic
// callback). Every pointer returned by this context is invalid
// afterwards. An error leaves the context inconsistent: reset before
// reusing it.
CHIBICC_API void chibicc_context_reset(ChibiccContext *ctx);

CHIBICC_API void chibicc_context_set_diagnostic(ChibiccContext *ctx,
                                    ChibiccDiagnosticFn fn, void *user);
CHIBICC_API bool chibicc_context_failed(ChibiccContext *ctx);

CHIBICC_API void chibicc_context_add_include_path(ChibiccContext *ctx, char *path);
CHIBICC_API void chibicc_context_define(ChibiccContext *ctx, char *name, char *buf);
CHIBICC_API void chibicc_context_undef(ChibiccContext *ctx, char *name);

// A predefined macro in the context's set. `value` is the replacement
// text; it is NULL for token-producing builtins and for the dynamic
// __DATE__/__TIME__ macros. The strings are owned by the context and
// invalidated by chibicc_context_reset.
typedef struct chibicc_predefined_macro {
  const char *name;
  const char *value;
} ChibiccPredefinedMacro;

// The predefined macro set is installed by default. The setter takes
// effect at the next chibicc_context_reset (disabling it yields a clean
// environment; the recorded -D/-U operations still apply). The count and
// at functions enumerate the set installed for the current translation
// unit (0 entries when it is disabled).
CHIBICC_API void chibicc_context_set_predefined_macros(ChibiccContext *ctx, bool enabled);
CHIBICC_API int chibicc_context_predefined_count(ChibiccContext *ctx);
CHIBICC_API ChibiccPredefinedMacro chibicc_context_predefined_at(ChibiccContext *ctx, int index);

#endif
