// The context object (PLAN L2/L3.1b): one context is one translation unit.
// The context is opaque; its fields are an implementation detail.
// Different contexts may live in different threads; one context must not
// be used concurrently from two threads. Internally every function that
// touches the arena, diagnostics, or context state takes the context as
// its first parameter - there is no thread-local current context, so a
// diagnostic callback may create, use, and free another context without
// disturbing the outer call.

#ifndef LIBCP_CONTEXT_H
#define LIBCP_CONTEXT_H

#include <stdbool.h>

#include <libcp/diagnostic.h>

// Marks a D7 function as exported from the shared library; the library
// itself is built with -fvisibility=hidden (PLAN L3.3b). Every header
// that declares functions includes this one. chibicc defines
// __chibicc__ and does not parse a leading visibility attribute; it
// only builds the static archive, where the marker is not needed.
#if defined(__chibicc__)
#define LIBCP_API
#elif defined(__GNUC__) || defined(__clang__)
#define LIBCP_API __attribute__((visibility("default")))
#else
#define LIBCP_API
#endif

typedef struct LibcpContext LibcpContext;

LIBCP_API LibcpContext *libcp_context_new(void);
LIBCP_API void libcp_context_free(LibcpContext *ctx);

// Releases every per-TU allocation and clears the per-TU state, keeping
// the configuration (include paths, macro operations, diagnostic
// callback). Every pointer returned by this context is invalid
// afterwards. An error leaves the context inconsistent: reset before
// reusing it.
LIBCP_API void libcp_context_reset(LibcpContext *ctx);

LIBCP_API void libcp_context_set_diagnostic(LibcpContext *ctx,
                                    LibcpDiagnosticFn fn, void *user);
LIBCP_API bool libcp_context_failed(LibcpContext *ctx);

LIBCP_API void libcp_context_add_include_path(LibcpContext *ctx, char *path);
LIBCP_API void libcp_context_define(LibcpContext *ctx, char *name, char *buf);
LIBCP_API void libcp_context_undef(LibcpContext *ctx, char *name);

// A predefined macro in the context's set. `value` is the replacement
// text; it is NULL for token-producing builtins and for the dynamic
// __DATE__/__TIME__ macros. The strings are owned by the context and
// invalidated by libcp_context_reset.
typedef struct libcp_predefined_macro {
  const char *name;
  const char *value;
} LibcpPredefinedMacro;

// The predefined macro set is installed by default. The setter takes
// effect at the next libcp_context_reset (disabling it yields a clean
// environment; the recorded -D/-U operations still apply). The count and
// at functions enumerate the set installed for the current translation
// unit (0 entries when it is disabled).
LIBCP_API void libcp_context_set_predefined_macros(LibcpContext *ctx, bool enabled);
LIBCP_API int libcp_context_predefined_count(LibcpContext *ctx);
LIBCP_API LibcpPredefinedMacro libcp_context_predefined_at(LibcpContext *ctx, int index);

#endif
