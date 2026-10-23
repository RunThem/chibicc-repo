#!/bin/sh
# The allocator-replacement gate: libchibicc obtains every block it owns
# through the public CHIBICC_MALLOC / CHIBICC_CALLOC / CHIBICC_REALLOC
# macros and releases it through CHIBICC_FREE (chibicc/allocator.h). The
# library is rebuilt here without CHIBICC_DEFAULT_ALLOCATOR and with
# replacement macros that count, so the test proves two things: the
# replacement is actually used for the context, its configuration and the
# arena, and chibicc_context_free returns every block to it (live blocks
# reach zero). A leaked arena block or a config string freed with the
# wrong allocator fails the count.

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/alloc_shim.h" <<'EOF'
#include <stddef.h>

/* No CHIBICC_DEFAULT_ALLOCATOR: allocator.h leaves the four macros to
 * us and checks that all of them are present. */
void *alloc_test_malloc(size_t size);
void *alloc_test_calloc(size_t count, size_t size);
void *alloc_test_realloc(void *ptr, size_t size);
void alloc_test_free(void *ptr);

#define CHIBICC_MALLOC(size) alloc_test_malloc(size)
#define CHIBICC_CALLOC(count, size) alloc_test_calloc(count, size)
#define CHIBICC_REALLOC(ptr, size) alloc_test_realloc(ptr, size)
#define CHIBICC_FREE(ptr) alloc_test_free(ptr)
EOF

cat > "$tmp/alloc_shim.c" <<'EOF'
#include <stdlib.h>

#include "alloc_shim.h"

static long live;
static long allocs;

void *alloc_test_malloc(size_t size) {
  void *p = malloc(size);
  if (!p)
    return NULL;
  live++;
  allocs++;
  return p;
}

void *alloc_test_calloc(size_t count, size_t size) {
  void *p = calloc(count, size);
  if (!p)
    return NULL;
  live++;
  allocs++;
  return p;
}

void *alloc_test_realloc(void *ptr, size_t size) {
  void *p = realloc(ptr, size);
  if (!p)
    return NULL;
  if (!ptr) {
    live++;
    allocs++;
  }
  return p;
}

void alloc_test_free(void *ptr) {
  if (!ptr)
    return;
  live--;
  free(ptr);
}

long alloc_test_live(void) {
  return live;
}

long alloc_test_allocs(void) {
  return allocs;
}
EOF

cat > "$tmp/consumer.c" <<'EOF'
#include <stdio.h>
#include <string.h>

#include <chibicc/chibicc.h>

long alloc_test_live(void);
long alloc_test_allocs(void);

/* Tokenize to sema, build a CST, reset (which must free every arena
 * block) and free the context (which must free the context object and
 * its configuration). */
static int one_round(void) {
  char *src = "int g = 1; int f(int a) { return a + g; }\n";

  ChibiccContext *ctx = chibicc_context_new();
  if (!ctx)
    return 1;

  chibicc_context_add_include_path(ctx, "inc");
  chibicc_context_define(ctx, "ALLOC_TEST", "1");

  ChibiccToken *tok = chibicc_tokenize_buffer(ctx, "<allocator>", src);
  if (!tok)
    return 1;
  tok = chibicc_preprocess(ctx, tok);
  if (!tok)
    return 1;
  ChibiccNode *ast = chibicc_parse(ctx, tok);
  if (!ast)
    return 1;
  ChibiccObj *prog = chibicc_sema(ctx, ast);
  if (!prog || chibicc_context_failed(ctx))
    return 1;

  ChibiccToken *raw = chibicc_tokenize_buffer(ctx, "<allocator>", src);
  ChibiccCstNode *root = chibicc_cst_parse(ctx, raw);
  if (!root || !chibicc_cst_verify(ctx, root, raw))
    return 1;

  chibicc_context_reset(ctx);
  if (alloc_test_live() <= 0) {
    fprintf(stderr, "allocator: reset released the context itself\n");
    return 1;
  }

  chibicc_context_free(ctx);
  return 0;
}

int main(void) {
  for (int i = 0; i < 3; i++)
    if (one_round())
      return 1;

  if (alloc_test_allocs() == 0) {
    fprintf(stderr, "allocator: the replacement was never called\n");
    return 1;
  }
  if (alloc_test_live() != 0) {
    fprintf(stderr, "allocator: %ld blocks still live after context_free\n",
            alloc_test_live());
    return 1;
  }

  return 0;
}
EOF

# The master switch also guards against a silent fallback: when the
# library is built without CHIBICC_DEFAULT_ALLOCATOR and a macro is
# missing, the header must fail right here.
cat > "$tmp/missing.c" <<'EOF'
#define CHIBICC_INTERNAL
#include <chibicc/allocator.h>
EOF
if "$cc" -std=c11 -I"$root/libchibicc/include" -fsyntax-only "$tmp/missing.c" \
    >/dev/null 2>&1; then
  echo "allocator: the missing-macro check did not fail" >&2
  exit 1
fi

objs=
for src in "$root"/libchibicc/src/*.c; do
  base=$(basename "$src" .c)
  "$cc" -std=c11 -g -Wall -Wno-switch \
    -I"$root/libchibicc/include" -I"$root/libchibicc/src" \
    -include "$tmp/alloc_shim.h" -c "$src" -o "$tmp/$base.o"
  objs="$objs $tmp/$base.o"
done
ar rcs "$tmp/libchibicc-alloc.a" $objs

"$cc" -std=c11 -g -Wall -I"$root/libchibicc/include" \
  "$tmp/consumer.c" "$tmp/alloc_shim.c" "$tmp/libchibicc-alloc.a" \
  -o "$tmp/consumer"
"$tmp/consumer"

echo "allocator: ok"
