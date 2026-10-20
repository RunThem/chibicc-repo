// Diagnostics (PLAN L3.1b): the message a registered callback receives.
// The library itself never prints through this path; the default handler
// reproduces the driver's byte-for-byte stderr format.

#ifndef CHIBICC_DIAGNOSTIC_H
#define CHIBICC_DIAGNOSTIC_H

typedef enum chibicc_diagnostic_severity {
  CHIBICC_DIAG_ERROR = 0,
  CHIBICC_DIAG_WARNING = 1,
} ChibiccDiagnosticSeverity;

// A diagnostic handed to a registered callback. `filename` and `message`
// are owned by the context and stay valid until the next
// chibicc_context_reset; a handler that keeps either past that point
// must copy it. The default handler additionally prints the source line
// and the caret from the location it was given.
typedef struct chibicc_diagnostic {
  const char *filename;
  int line;
  int column;
  ChibiccDiagnosticSeverity severity;
  const char *message;
} ChibiccDiagnostic;

typedef void (*ChibiccDiagnosticFn)(void *user, const ChibiccDiagnostic *);

#endif
