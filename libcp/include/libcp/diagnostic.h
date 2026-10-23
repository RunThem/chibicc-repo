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
typedef struct libcp_diagnostic {
  const char *filename;
  int line;
  int column;
  LibcpDiagnosticSeverity severity;
  const char *message;
} LibcpDiagnostic;

typedef void (*LibcpDiagnosticFn)(void *user, const LibcpDiagnostic *);

#endif
