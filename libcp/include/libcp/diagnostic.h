// Diagnostics (PLAN L3.1b): the message a registered callback receives.
// The library itself never prints through this path; the default handler
// reproduces the driver's byte-for-byte stderr format.

#ifndef LIBCP_DIAGNOSTIC_H
#define LIBCP_DIAGNOSTIC_H

typedef enum libcp_diagnostic_severity {
  LIBCP_DIAG_ERROR = 0,
  LIBCP_DIAG_WARNING = 1,
} LibcpDiagnosticSeverity;

// A diagnostic handed to a registered callback. `filename` and `message`
// are owned by the context and stay valid until the next
// libcp_context_reset; a handler that keeps either past that point
// must copy it. The default handler additionally prints the source line
// and the caret from the location it was given.
//
// `tok` is the anchor token when the diagnostic is token-anchored
// (error_tok / warn_tok; NULL for plain error()), and `prov` is that
// token's macro-expansion chain (NULL for source tokens). A handler can
// walk the chain to the outermost call site (PLAN-l5 W5). The default
// handler ignores both and prints exactly as before.
struct libcp_macro_prov;
struct libcp_token;
typedef struct libcp_diagnostic {
  const char *filename;
  int line;
  int column;
  LibcpDiagnosticSeverity severity;
  const char *message;
  const struct libcp_token *tok;
  const struct libcp_macro_prov *prov;
} LibcpDiagnostic;

typedef void (*LibcpDiagnosticFn)(void *user, const LibcpDiagnostic *);

#endif
