#!/bin/sh
# The library acceptance test (PLAN L3.4): a host-compiled consumer that
# includes chibicc.h, links libchibicc.a, and uses the public context /
# phase functions without the driver or codegen.

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
# CHIBICC_LIB selects the archive to link; test-stage2 points it at the
# stage2 objects (PLAN L3.3c).
lib=${CHIBICC_LIB:-$root/libchibicc.a}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

if [ ! -f "$lib" ]; then
  echo "library: $lib is missing; run make libchibicc.a" >&2
  exit 1
fi

# Inputs for the tokenize_file + include-path check: the header lives in
# its own directory, so only the context include path finds it.
mkdir -p "$tmp/inc"
printf '#define H 7\n' > "$tmp/inc/header.h"
printf '#include "header.h"\nint x = H;\n' > "$tmp/input.c"
printf 'int a;\000int b;\n' > "$tmp/nul.c"
printf '#include "self.h"\n' > "$tmp/self.h"

cat > "$tmp/consumer.c" <<'EOF'
#include <chibicc/chibicc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static int diag_fields_ok = 1;
static int diag_located;

static void diag(void *user, const ChibiccDiagnostic *d) {
  (void)user;
  if (!d->message || !*d->message)
    diag_fields_ok = 0;
  if (d->filename) {
    if (d->line <= 0 || d->column <= 0)
      diag_fields_ok = 0;
    diag_located++;
  }
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

static ChibiccToken *find_num(ChibiccToken *tok, long val) {
  for (; tok && tok->kind != TK_EOF; tok = tok->next)
    if (tok->kind == TK_NUM && tok->val == val)
      return tok;
  return NULL;
}

// A declaration's type record carries its own name. The built-in types
// are shared by every context, so the parser must copy one instead of
// writing the name into the singleton (audit F1).
static int check_type_names(ChibiccContext *ctx) {
  ChibiccObj *prog = NULL;
  if (!compile(ctx, "int x; int y;\n", &prog) || !prog)
    return 1;

  ChibiccObj *x = NULL, *y = NULL;
  for (ChibiccObj *o = prog; o; o = o->next) {
    if (o->name && strcmp(o->name, "x") == 0)
      x = o;
    if (o->name && strcmp(o->name, "y") == 0)
      y = o;
  }
  if (!x || !y)
    return 1;
  if (!x->ty->name || x->ty->name->len != 1 || x->ty->name->loc[0] != 'x')
    return 1;
  if (!y->ty->name || y->ty->name->len != 1 || y->ty->name->loc[0] != 'y')
    return 1;
  return 0;
}

// A NUL byte is diagnosed, not silently truncated: once with an explicit
// length and once through a real file.
static int check_nul_buffer(ChibiccContext *ctx) {
  int before = errors;
  if (chibicc_tokenize_buffer_len(ctx, "<nul>", "int a;\0int b;", 13) != NULL)
    return 1;
  if (!chibicc_context_failed(ctx) || errors != before + 1)
    return 1;
  chibicc_context_reset(ctx);
  return 0;
}

static int check_nul_file(ChibiccContext *ctx, char *path) {
  int before = errors;
  if (chibicc_tokenize_file(ctx, path) != NULL)
    return 1;
  if (!chibicc_context_failed(ctx) || errors != before + 1)
    return 1;
  chibicc_context_reset(ctx);
  return 0;
}

// A file that includes itself through differently spelled paths defeats
// the guard/pragma-once maps; the depth bound stops it.
static int check_include_depth(ChibiccContext *ctx, char *path) {
  int before = errors;
  ChibiccToken *tok = chibicc_tokenize_file(ctx, path);
  if (!tok)
    return 1;
  if (chibicc_preprocess(ctx, tok) != NULL)
    return 1;
  if (!chibicc_context_failed(ctx) || errors != before + 1)
    return 1;
  chibicc_context_reset(ctx);
  return 0;
}

// Pathological nesting must fail with a diagnostic instead of
// overflowing the parser stack.
static int check_parse_depth(ChibiccContext *ctx) {
  char *prefix = "int f(void) { return ";
  size_t n = 2000, plen = strlen(prefix), i = plen;
  char *src = malloc(plen + 2 * n + 8);
  if (!src)
    return 1;
  memcpy(src, prefix, plen);
  for (size_t j = 0; j < n; j++)
    src[i++] = '(';
  src[i++] = '1';
  for (size_t j = 0; j < n; j++)
    src[i++] = ')';
  src[i++] = ';';
  src[i++] = '}';
  src[i++] = '\n';
  src[i] = '\0';

  int before = errors;
  ChibiccToken *tok = chibicc_tokenize_buffer(ctx, "<deep>", src);
  free(src);
  if (!tok)
    return 1;
  tok = chibicc_preprocess(ctx, tok);
  if (!tok)
    return 1;
  if (chibicc_parse(ctx, tok) != NULL)
    return 1;
  if (!chibicc_context_failed(ctx) || errors != before + 1)
    return 1;
  chibicc_context_reset(ctx);
  return 0;
}

// chibicc_reconstruct rebuilds the tokenized bytes exactly.
static int check_reconstruct(ChibiccContext *ctx) {
  char *text = "int  x =  1;  /* c */\n\tchar *s = \"a\";\n";
  ChibiccToken *tok = chibicc_tokenize_buffer(ctx, "<reconstruct>", text);
  FILE *f = tmpfile();
  if (!tok || !f)
    return 1;
  chibicc_reconstruct(ctx, tok, f);
  fflush(f);
  rewind(f);
  char buf[128];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  return strcmp(buf, text) != 0;
}

// The provenance accessors (PLAN L3.4a): an argument expansion and a
// builtin expansion.
static int check_provenance(ChibiccContext *ctx) {
  ChibiccToken *tok =
    chibicc_tokenize_buffer(ctx, "<prov>", "#define F(x) x+1\nint a = F(2);\n");
  if (!tok)
    return 1;
  tok = chibicc_preprocess(ctx, tok);
  if (!tok)
    return 1;

  ChibiccToken *two = find_num(tok, 2);
  if (!two)
    return 1;
  ChibiccMacroProv *p = chibicc_token_provenance(two);
  if (!p)
    return 1;
  ChibiccMacroExpansion *e = chibicc_macro_prov_expansion(p);
  if (!e || !chibicc_macro_expansion_name(e) ||
      strcmp(chibicc_macro_expansion_macro(e), "F") != 0 ||
      chibicc_macro_expansion_arg_count(e) != 1 ||
      chibicc_macro_prov_arg_index(p) != 0 ||
      chibicc_macro_expansion_is_builtin(e) ||
      chibicc_macro_prov_next(p) != NULL)
    return 1;
  ChibiccToken *arg = chibicc_macro_expansion_arg(e, 0);
  if (!arg || arg->len != 1 || arg->loc[0] != '2')
    return 1;

  tok = chibicc_tokenize_buffer(ctx, "<prov>", "int b = __LINE__;\n");
  if (!tok)
    return 1;
  tok = chibicc_preprocess(ctx, tok);
  if (!tok)
    return 1;
  ChibiccToken *one = find_num(tok, 1);
  if (!one)
    return 1;
  p = chibicc_token_provenance(one);
  if (!p || !chibicc_macro_prov_expansion(p) ||
      !chibicc_macro_expansion_is_builtin(chibicc_macro_prov_expansion(p)))
    return 1;
  return 0;
}

// tokenize_file + the context's include path.
static int check_include(ChibiccContext *ctx, char *path, char *dir) {
  chibicc_context_add_include_path(ctx, dir);
  ChibiccToken *tok = chibicc_tokenize_file(ctx, path);
  if (!tok)
    return 1;
  tok = chibicc_preprocess(ctx, tok);
  if (!tok)
    return 1;
  return find_num(tok, 7) ? 0 : 1;
}

// The recorded macro operations: define applies immediately, undef
// removes the name.
static int check_define(ChibiccContext *ctx) {
  chibicc_context_define(ctx, "LIBFOO", "42");
  ChibiccToken *tok = chibicc_tokenize_buffer(ctx, "<def>", "int y = LIBFOO;\n");
  if (!tok)
    return 1;
  tok = chibicc_preprocess(ctx, tok);
  if (!tok || !find_num(tok, 42))
    return 1;

  chibicc_context_undef(ctx, "LIBFOO");
  tok = chibicc_tokenize_buffer(ctx, "<undef>", "int y = LIBFOO;\n");
  if (!tok)
    return 1;
  tok = chibicc_preprocess(ctx, tok);
  if (!tok || find_num(tok, 42))
    return 1;
  return 0;
}

// Every public entry unwinds to the callback and returns its failure
// value without exiting (PLAN L3.4b).
static int check_errors(ChibiccContext *ctx) {
  int before;

  // Missing file.
  before = errors;
  if (chibicc_tokenize_file(ctx, "/no/such/chibicc-libtest.c") != NULL)
    return 1;
  if (!chibicc_context_failed(ctx) || errors != before + 1)
    return 1;
  chibicc_context_reset(ctx);

  // Bad directive.
  before = errors;
  ChibiccToken *t = chibicc_tokenize_buffer(ctx, "<bad>", "#error boom\n");
  if (!t || chibicc_preprocess(ctx, t) != NULL)
    return 1;
  if (!chibicc_context_failed(ctx) || errors != before + 1)
    return 1;
  chibicc_context_reset(ctx);

  // Bad expression.
  before = errors;
  t = chibicc_tokenize_buffer(ctx, "<bad>", "int x = ;\n");
  if (!t)
    return 1;
  t = chibicc_preprocess(ctx, t);
  if (!t || chibicc_parse(ctx, t) != NULL)
    return 1;
  if (!chibicc_context_failed(ctx) || errors != before + 1)
    return 1;
  chibicc_context_reset(ctx);

  // Bad type (an undeclared name fails sema).
  before = errors;
  t = chibicc_tokenize_buffer(ctx, "<bad>", "int f(void) { return missing; }\n");
  if (!t)
    return 1;
  t = chibicc_preprocess(ctx, t);
  ChibiccNode *ast = t ? chibicc_parse(ctx, t) : NULL;
  if (!ast || chibicc_sema(ctx, ast) != NULL)
    return 1;
  if (!chibicc_context_failed(ctx) || errors != before + 1)
    return 1;
  chibicc_context_reset(ctx);
  return 0;
}

// Two contexts in one thread: a diagnostic callback of one context runs a
// public entry of the other while the first is unwinding (PLAN L3.4b).
static ChibiccContext *nested_target;
static int nested_ok;

static void nested_diag(void *user, const ChibiccDiagnostic *d) {
  (void)user;
  if (!d->message || !*d->message)
    nested_ok = 0;
  if (!nested_target)
    return;
  ChibiccContext *a = nested_target;
  nested_target = NULL;
  ChibiccToken *t = chibicc_tokenize_buffer(a, "<nested>", "int n;\n");
  if (t)
    t = chibicc_preprocess(a, t);
  nested_ok = (t != NULL);
}

static int check_nested(ChibiccContext *a) {
  ChibiccContext *b = chibicc_context_new();
  nested_target = a;
  nested_ok = 1;
  chibicc_context_set_diagnostic(b, nested_diag, NULL);

  ChibiccToken *t = chibicc_tokenize_buffer(b, "<nest>", "int x = ;\n");
  if (!t)
    return 1;
  t = chibicc_preprocess(b, t);
  if (!t)
    return 1;
  (void)chibicc_parse(b, t);
  int ok = nested_ok && chibicc_context_failed(b);
  chibicc_context_free(b);

  ChibiccObj *prog = NULL;
  if (!compile(a, "int z;\n", &prog) || !prog)
    ok = 0;
  return ok ? 0 : 1;
}

// Two contexts in two threads (PLAN L3.4b). The active context is
// thread-local, so the two pipelines never see each other.
static void *thread_main(void *arg) {
  char *src = arg;
  ChibiccContext *ctx = chibicc_context_new();
  ChibiccToken *t = chibicc_tokenize_buffer(ctx, "<thread>", src);
  if (t)
    t = chibicc_preprocess(ctx, t);
  ChibiccNode *ast = t ? chibicc_parse(ctx, t) : NULL;
  ChibiccObj *prog = ast ? chibicc_sema(ctx, ast) : NULL;
  int ok = prog && !chibicc_context_failed(ctx);
  chibicc_context_free(ctx);
  return ok ? NULL : (void *)1;
}

static int check_threads(void) {
  pthread_t t1, t2;
  char *src = "int f(int a) { return a + 1; }\n";
  if (pthread_create(&t1, NULL, thread_main, src) != 0)
    return 1;
  if (pthread_create(&t2, NULL, thread_main, src) != 0)
    return 1;
  void *r1 = NULL, *r2 = NULL;
  pthread_join(t1, &r1);
  pthread_join(t2, &r2);
  return (r1 == NULL && r2 == NULL) ? 0 : 1;
}

// A fresh context that runs one full pipeline and is freed returns the
// process to its pre-context live-byte count (PLAN L3.4c). Measuring
// around this context keeps stdio / pthread one-time allocations out.
static int check_context_free(void) {
#ifdef HAVE_ASAN
  size_t base = __sanitizer_get_current_allocated_bytes();
#endif
  char *src = "int g = 1; int f(int a) { return a + g; }\n";
  ChibiccContext *c = chibicc_context_new();
  ChibiccToken *t = chibicc_tokenize_buffer(c, "<free>", src);
  if (t)
    t = chibicc_preprocess(c, t);
  ChibiccNode *ast = t ? chibicc_parse(c, t) : NULL;
  ChibiccObj *prog = ast ? chibicc_sema(c, ast) : NULL;
  int ok = prog != NULL;

  ChibiccToken *raw = chibicc_tokenize_buffer(c, "<free>", src);
  ChibiccCstNode *root = chibicc_cst_parse(c, raw);
  if (!root || !chibicc_cst_verify(c, root, raw))
    ok = 0;

  chibicc_context_free(c);
#ifdef HAVE_ASAN
  if (__sanitizer_get_current_allocated_bytes() > base + 4096)
    ok = 0;
#endif
  return ok ? 0 : 1;
}

int main(int argc, char **argv) {
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

  chibicc_context_reset(ctx);
  if (check_type_names(ctx))
    return 1;
  chibicc_context_reset(ctx);

  if (check_nul_buffer(ctx))
    return 1;
  if (argc > 3 && check_nul_file(ctx, argv[3]))
    return 1;
  if (argc > 4 && check_include_depth(ctx, argv[4]))
    return 1;
  if (check_parse_depth(ctx))
    return 1;

  // The layer-1 reconstruct channel, the provenance accessors, the
  // tokenize_file path with a context include path, and the recorded
  // macro operations (PLAN L3.4a).
  if (check_reconstruct(ctx))
    return 1;
  if (check_provenance(ctx))
    return 1;
  chibicc_context_reset(ctx);
  if (argc > 2 && check_include(ctx, argv[1], argv[2]))
    return 1;
  chibicc_context_reset(ctx);
  if (check_define(ctx))
    return 1;
  chibicc_context_reset(ctx);
  if (check_errors(ctx))
    return 1;
  if (check_nested(ctx))
    return 1;
  if (check_threads())
    return 1;
  if (!diag_fields_ok || diag_located == 0)
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

  if (check_context_free())
    return 1;

  chibicc_context_free(ctx);
  puts("library: ok");
  return 0;
}
EOF

"$cc" -std=c11 -pthread -I"$root/libchibicc/include" "$tmp/consumer.c" "$lib" -o "$tmp/consumer"
"$tmp/consumer" "$tmp/input.c" "$tmp/inc" "$tmp/nul.c" "$tmp/self.h"

# The sanitizer gate (PLAN L3.0/L3.4c). The library sources themselves are
# rebuilt with ASan and UBSan, so the coverage reaches CST, parse, sema and
# reset; the reset loop also asserts that live bytes do not grow. LSan is
# deliberately not the gate - it reports only intermittently under Rosetta.
mkdir -p "$tmp/asan"
asan_objs=
for src in "$root"/*.c; do
  base=$(basename "$src" .c)
  case $base in main|codegen) continue;; esac
  "$cc" -std=c11 -g -Wall -Wno-switch -DCHIBICC_DEBUG_ARENA -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$root/libchibicc/include" -c "$src" -o "$tmp/asan/$base.o"
  asan_objs="$asan_objs $tmp/asan/$base.o"
done
ar rcs "$tmp/libchibicc-asan.a" $asan_objs
"$cc" -std=c11 -pthread -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer -I"$root/libchibicc/include" "$tmp/consumer.c" \
  "$tmp/libchibicc-asan.a" -o "$tmp/consumer-asan"
ASAN_OPTIONS=detect_leaks=0 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  "$tmp/consumer-asan" "$tmp/input.c" "$tmp/inc" "$tmp/nul.c" "$tmp/self.h"

# The shared library (PLAN L3.3b, Linux only): the same consumer links
# against it and runs.
if [ -z "${CHIBICC_LIB:-}" ] && [ -f "$root/libchibicc.so" ]; then
  "$cc" -std=c11 -pthread -I"$root/libchibicc/include" "$tmp/consumer.c" \
    -L"$root" -lchibicc -Wl,-rpath,"$root" -o "$tmp/consumer-so"
  "$tmp/consumer-so" "$tmp/input.c" "$tmp/inc" "$tmp/nul.c" "$tmp/self.h"
fi
