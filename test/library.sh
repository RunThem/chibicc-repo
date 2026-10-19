#!/bin/sh
# The library acceptance test (PLAN L3.4): a host-compiled consumer that
# includes chibicc.h, links libchibicc.a, and uses the public context /
# phase functions without the driver or codegen.

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/consumer.c" <<'EOF'
#include <chibicc/chibicc.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__SANITIZE_ADDRESS__)
#define HAVE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HAVE_ASAN 1
#endif
#endif

#ifdef HAVE_ASAN
// AddressSanitizer's allocator interface: the number of live bytes in
// the process. It is deterministic, unlike LeakSanitizer (which under
// Rosetta reports leaks only intermittently).
extern size_t __sanitizer_get_current_allocated_bytes(void);
#endif

static int errors;

static void diag(void *user, const ChibiccDiagnostic *d) {
  (void)user;
  if (!d->message || !*d->message)
    exit(2);
  if (d->severity == 0)
    errors++;
}

static ChibiccNode *compile(ChibiccContext *ctx, char *src, ChibiccObj **out) {
  ChibiccToken *tok = chibicc_tokenize_buffer(ctx, "<libtest>", src);
  if (!tok)
    return NULL;
  tok = chibicc_preprocess(ctx, tok);
  if (!tok)
    return NULL;
  ChibiccNode *ast = chibicc_parse(ctx, tok);
  if (chibicc_context_failed(ctx))
    return NULL;
  ChibiccObj *prog = chibicc_sema(ctx, ast);
  if (chibicc_context_failed(ctx))
    return NULL;
  *out = prog;
  return ast;
}

int main(void) {
  char *src =
    "typedef struct P { int x; } P;\n"
    "int f(int a) { P p = {a}; return p.x + 1; }\n";

  ChibiccContext *ctx = chibicc_context_new();
  chibicc_context_set_diagnostic(ctx, diag, NULL);

  ChibiccObj *prog = NULL;
  ChibiccNode *ast = compile(ctx, src, &prog);
  if (!ast || !prog)
    return 1;

  // The raw stream remains available for the CST channel.
  ChibiccToken *raw = chibicc_tokenize_buffer(ctx, "<libtest>", src);
  ChibiccCstNode *root = chibicc_cst_parse(ctx, raw);
  if (!root || !chibicc_cst_verify(ctx, root, raw))
    return 1;

  // A bad input reaches the callback and unwinds instead of exiting.
  ChibiccToken *bad = chibicc_tokenize_buffer(ctx, "<libtest>", "int x = ;\n");
  if (!bad)
    return 1;
  bad = chibicc_preprocess(ctx, bad);
  if (!bad)
    return 1;
  chibicc_parse(ctx, bad);
  if (!chibicc_context_failed(ctx) || errors != 1)
    return 1;

  // Reset releases the arena and the per-TU state; the context is reusable.
  chibicc_context_reset(ctx);
  prog = NULL;
  ast = compile(ctx, src, &prog);
  if (!ast || !prog || chibicc_context_failed(ctx) || errors != 1)
    return 1;

  // Every CstNode and every children array is arena memory, so a reset
  // releases the whole tree (PLAN L3.0). Under ASan the live-byte count
  // must not grow from iteration to iteration: the pre-fix children
  // arrays (a bare realloc) leaked about 1.6 KiB per iteration on this
  // source, while the arena version ends each iteration empty.
  chibicc_context_reset(ctx);
#ifdef HAVE_ASAN
  size_t before = __sanitizer_get_current_allocated_bytes();
#endif
  for (int i = 0; i < 64; i++) {
    ChibiccToken *t = chibicc_tokenize_buffer(ctx, "<libtest>", src);
    if (!t)
      return 1;
    ChibiccCstNode *r = chibicc_cst_parse(ctx, t);
    if (!r || !chibicc_cst_verify(ctx, r, t))
      return 1;
    chibicc_context_reset(ctx);
  }
#ifdef HAVE_ASAN
  size_t after = __sanitizer_get_current_allocated_bytes();
  if (after > before + 32 * 1024) {
    fprintf(stderr, "library: CST -> reset grew live bytes by %zu\n",
            after - before);
    return 1;
  }
#endif

  chibicc_context_free(ctx);
  puts("library: ok");
  return 0;
}
EOF

"$cc" -std=c11 -I"$root/libchibicc/include" "$tmp/consumer.c" "$root/libchibicc.a" -o "$tmp/consumer"
"$tmp/consumer"

# The same consumer under AddressSanitizer (PLAN L3.0). The reset loop
# asserts that live bytes do not grow: any CST allocation left outside
# the arena accumulates there. LeakSanitizer is deliberately not the
# gate - it reports only intermittently under Rosetta.
"$cc" -std=c11 -g -fsanitize=address -fno-omit-frame-pointer \
  -I"$root/libchibicc/include" "$tmp/consumer.c" "$root/libchibicc.a" \
  -o "$tmp/consumer-asan"
"$tmp/consumer-asan"
