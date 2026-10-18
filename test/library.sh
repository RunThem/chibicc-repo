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

static int errors;

static void diag(void *user, const ChibiccDiagnostic *d) {
  (void)user;
  if (!d->message || !*d->message)
    exit(2);
  if (d->severity == 0)
    errors++;
}

static Node *compile(ChibiccContext *ctx, char *src, Obj **out) {
  Token *tok = chibicc_tokenize_buffer(ctx, "<libtest>", src);
  if (!tok)
    return NULL;
  tok = chibicc_preprocess(ctx, tok);
  if (!tok)
    return NULL;
  Node *ast = chibicc_parse(ctx, tok);
  if (chibicc_context_failed(ctx))
    return NULL;
  Obj *prog = chibicc_sema(ctx, ast);
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

  Obj *prog = NULL;
  Node *ast = compile(ctx, src, &prog);
  if (!ast || !prog)
    return 1;

  // The raw stream remains available for the CST channel.
  Token *raw = chibicc_tokenize_buffer(ctx, "<libtest>", src);
  CstNode *root = chibicc_cst_parse(ctx, raw);
  if (!root || !chibicc_cst_verify(ctx, root, raw))
    return 1;

  // A bad input reaches the callback and unwinds instead of exiting.
  Token *bad = chibicc_tokenize_buffer(ctx, "<libtest>", "int x = ;\n");
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

  chibicc_context_free(ctx);
  puts("library: ok");
  return 0;
}
EOF

"$cc" -std=c11 -I"$root/libchibicc/include" "$tmp/consumer.c" "$root/libchibicc.a" -o "$tmp/consumer"
"$tmp/consumer"
