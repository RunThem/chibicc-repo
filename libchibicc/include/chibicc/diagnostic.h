// Diagnostics (PLAN L3.1b): the message a registered callback receives.
// The library itself never prints through this path; the default handler
// reproduces the driver's byte-for-byte stderr format.

#ifndef CHIBICC_DIAGNOSTIC_H
#define CHIBICC_DIAGNOSTIC_H

// A diagnostic handed to a registered callback. `message` is the
// formatted text; the default handler additionally prints the source
// line and the caret from the location it was given.
typedef struct {
  const char *filename;
  int line;
  int column;
  int severity; // 0 = error, 1 = warning
  const char *message;
} ChibiccDiagnostic;

typedef void (*ChibiccDiagnosticFn)(void *user, const ChibiccDiagnostic *);

#endif
