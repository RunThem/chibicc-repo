#!/bin/sh
# The implicit-context gate (PLAN-explicit-context). Every internal
# function that touches the arena, diagnostics, or context state takes
# `ChibiccContext *ctx` as its first parameter; there is no thread-local
# accessor, and codegen receives the driver's ctx explicitly.
#
# Report mode (default) prints what it finds and exits 0, so the gate can
# land before the line is converted. `--strict` turns every finding into
# a failure; `make check-context` and `make check-context-pratt` run the
# strict form. CHIBICC_LIB selects the archive to inspect.

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
lib=${CHIBICC_LIB:-$root/libchibicc.a}
mode=${1:-report}
rc=0

if [ "$mode" = "--strict" ]; then
  strict=1
else
  strict=0
fi

note() {
  if [ "$strict" -eq 1 ]; then
    echo "context-audit: $1" >&2
    rc=1
  else
    echo "context-audit: $1"
  fi
}

# 1. The accessor names must not exist in the library sources or headers.
hits=$(grep -RIn 'chibicc_ctx\|chibicc_active_ctx\|chibicc_context_activate' \
         "$root/libchibicc/src" "$root/libchibicc/include" 2>/dev/null || true)
if [ -n "$hits" ]; then
  echo "$hits"
  note "accessor references in libchibicc/ (want 0)"
else
  echo "context-audit: accessors in libchibicc/: 0"
fi

# 2. No thread-local storage anywhere in the library. The compiler's
#    keyword tables spell the two keywords as string literals, so only
#    declaration-looking lines count.
hits=$(grep -RInE '^[[:space:]]*(extern[[:space:]]+|static[[:space:]]+)?(_Thread_local|__thread)[[:space:]]+[A-Za-z_]' \
         "$root/libchibicc/src" "$root/libchibicc/include" 2>/dev/null || true)
if [ -n "$hits" ]; then
  echo "$hits"
  note "thread-local storage in libchibicc/ (want 0)"
else
  echo "context-audit: thread-local in libchibicc/: 0"
fi

# 3. The consumer side: no accessor and no equivalent file-scope context
#    pointer in codegen; main.c keeps its driver-level ctx.
for f in "$root/codegen.c" "$root/main.c"; do
  hits=$(grep -n 'chibicc_ctx\|chibicc_active_ctx\|chibicc_context_activate' "$f" || true)
  if [ -n "$hits" ]; then
    echo "$hits"
    note "accessor references in $(basename "$f") (want 0)"
  else
    echo "context-audit: accessors in $(basename "$f"): 0"
  fi
done
hits=$(grep -n '\bcg_ctx\b' "$root/codegen.c" || true)
if [ -n "$hits" ]; then
  echo "$hits"
  note "cg_ctx in codegen.c (want 0)"
else
  echo "context-audit: cg_ctx in codegen.c: 0"
fi
# No file-scope context pointer under any other name either. main.c is
# the driver and keeps its one context; codegen receives it as a
# parameter.
hits=$(grep -nE '^[[:space:]]*(static[[:space:]]+)?ChibiccContext[[:space:]]*\*' "$root/codegen.c" || true)
if [ -n "$hits" ]; then
  echo "$hits"
  note "file-scope context pointer in codegen.c (want 0)"
else
  echo "context-audit: file-scope context pointer in codegen.c: 0"
fi

# 4. The accessors must not survive in the archive either. Darwin nm
#    prefixes C symbols with an underscore.
if [ ! -f "$lib" ]; then
  note "$lib is missing; run make libchibicc.a"
else
  syms=$(nm -g "$lib" 2>/dev/null | awk '{print $NF}' | sed 's/^_//')
  found=$(printf '%s\n' "$syms" | grep -E '^(chibicc_ctx|chibicc_active_ctx|chibicc_context_activate)$' || true)
  if [ -n "$found" ]; then
    echo "$found"
    note "accessor symbols in $lib (want 0)"
  else
    echo "context-audit: accessor symbols in $(basename "$lib"): 0"
  fi
fi

exit $rc
