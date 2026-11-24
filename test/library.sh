#!/bin/sh
# The library acceptance test (PLAN L3.4): a host-compiled consumer that
# includes libcp.h, links libcp.a, and uses the public context /
# phase functions without the driver or codegen.

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
# LIBCP_LIB selects the archive to link; test-stage2 points it at the
# stage2 objects (PLAN L3.3c).
lib=${LIBCP_LIB:-$root/libcp.a}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

if [ ! -f "$lib" ]; then
  echo "library: $lib is missing; run make libcp.a" >&2
  exit 1
fi

# Inputs for the tokenize_file + include-path check: the header lives in
# its own directory, so only the context include path finds it.
mkdir -p "$tmp/inc"
printf '#define H 7\n' > "$tmp/inc/header.h"
printf '#include "header.h"\nint x = H;\n' > "$tmp/input.c"
printf 'int a;\000int b;\n' > "$tmp/nul.c"
printf '#include "self.h"\n' > "$tmp/self.h"
printf 'int stdin_a;\nint stdin_b;\n' > "$tmp/stdin.c"

cat > "$tmp/consumer.c" <<'EOF'
#include <libcp/libcp.h>
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
static const char *last_message;

static void diag(void *user, const LibcpDiagnostic *d) {
  (void)user;
  if (!d->message || !*d->message)
    diag_fields_ok = 0;
  last_message = d->message;
  if (d->filename) {
    if (d->line <= 0 || d->column <= 0)
      diag_fields_ok = 0;
    diag_located++;
  }
  if (d->severity == LIBCP_DIAG_ERROR)
    errors++;
}

static LibcpNode *compile(LibcpContext *ctx, char *src, LibcpObj **out) {
  LibcpToken *tok = libcp_tokenize_buffer(ctx, "<libtest>", src);
  if (!tok)
    return NULL;
  tok = libcp_preprocess(ctx, tok);
  if (!tok)
    return NULL;
  LibcpNode *ast = libcp_parse(ctx, tok);
  if (libcp_context_failed(ctx))
    return NULL;
  LibcpObj *prog = libcp_sema(ctx, ast);
  if (libcp_context_failed(ctx))
    return NULL;
  *out = prog;
  return ast;
}

static LibcpToken *find_num(LibcpToken *tok, long val) {
  for (; tok && tok->kind != TK_EOF; tok = tok->next)
    if (tok->kind == TK_NUM && tok->val == val)
      return tok;
  return NULL;
}

// A declared object's type record carries its own name. Shared builtin,
// tag and typedef records are never written; sema gives each named
// object a per-object copy after completing the type (audit F1).
static int check_type_names(LibcpContext *ctx) {
  LibcpObj *prog = NULL;
  if (!compile(ctx, "int x; int y;\n", &prog) || !prog)
    return 1;

  LibcpObj *x = NULL, *y = NULL;
  for (LibcpObj *o = prog; o; o = o->next) {
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

  // Functions, parameters and locals get their own named copies too.
  libcp_context_reset(ctx);
  prog = NULL;
  if (!compile(ctx, "int f(int p) { int l; return p + l; }\n", &prog) || !prog)
    return 1;

  LibcpObj *fn = NULL;
  for (LibcpObj *o = prog; o; o = o->next)
    if (o->name && strcmp(o->name, "f") == 0)
      fn = o;
  if (!fn || !fn->is_function || !fn->ty->name ||
      fn->ty->name->len != 1 || fn->ty->name->loc[0] != 'f')
    return 1;
  if (!fn->params || !fn->params->ty->name ||
      fn->params->ty->name->len != 1 || fn->params->ty->name->loc[0] != 'p')
    return 1;

  LibcpObj *local = NULL;
  for (LibcpObj *o = fn->locals; o; o = o->next)
    if (o->name && strcmp(o->name, "l") == 0)
      local = o;
  if (!local || !local->ty->name || local->ty->name->len != 1 ||
      local->ty->name->loc[0] != 'l')
    return 1;
  return 0;
}

// A NUL byte is diagnosed, not silently truncated: once with an explicit
// length and once through a real file.
static int check_nul_buffer(LibcpContext *ctx) {
  int before = errors;
  if (libcp_tokenize_buffer_len(ctx, "<nul>", "int a;\0int b;", 13) != NULL)
    return 1;
  if (!libcp_context_failed(ctx) || errors != before + 1)
    return 1;
  libcp_context_reset(ctx);
  return 0;
}

static int check_nul_file(LibcpContext *ctx, char *path) {
  int before = errors;
  if (libcp_tokenize_file(ctx, path) != NULL)
    return 1;
  if (!libcp_context_failed(ctx) || errors != before + 1)
    return 1;
  libcp_context_reset(ctx);
  return 0;
}

// A file that includes itself through differently spelled paths defeats
// the guard/pragma-once maps; the depth bound stops it.
static int check_include_depth(LibcpContext *ctx, char *path) {
  int before = errors;
  LibcpToken *tok = libcp_tokenize_file(ctx, path);
  if (!tok)
    return 1;
  if (libcp_preprocess(ctx, tok) != NULL)
    return 1;
  if (!libcp_context_failed(ctx) || errors != before + 1)
    return 1;
  libcp_context_reset(ctx);
  return 0;
}

// The first "-" read is cached: a second tokenize of stdin sees the same
// text instead of the sticky EOF's empty stream (C12).
static int check_stdin_twice(LibcpContext *ctx, const char *path) {
  if (!freopen(path, "r", stdin))
    return 1;

  LibcpToken *a = libcp_tokenize_file(ctx, "-");
  LibcpToken *b = libcp_tokenize_file(ctx, "-");
  if (!a || !b)
    return 1;

  int n = 0;
  while (a->kind != TK_EOF && b->kind != TK_EOF &&
         a->len == b->len && !memcmp(a->loc, b->loc, a->len)) {
    a = a->next;
    b = b->next;
    n++;
  }

  int ok = a->kind == TK_EOF && b->kind == TK_EOF && n > 0;
  libcp_context_reset(ctx);
  return ok ? 0 : 1;
}

// Pathological nesting must fail with a diagnostic instead of
// overflowing the parser stack.
static int check_parse_depth(LibcpContext *ctx) {
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
  LibcpToken *tok = libcp_tokenize_buffer(ctx, "<deep>", src);
  free(src);
  if (!tok)
    return 1;
  tok = libcp_preprocess(ctx, tok);
  if (!tok)
    return 1;
  if (libcp_parse(ctx, tok) != NULL)
    return 1;
  if (!libcp_context_failed(ctx) || errors != before + 1)
    return 1;
  libcp_context_reset(ctx);
  return 0;
}

// The CST parser is total: a pathologically deep span falls back to
// unparsed, flat groups and still verifies leaf-for-leaf against the
// token stream.
static int cst_depth_case(LibcpContext *ctx, char *src) {
  LibcpToken *raw = libcp_tokenize_buffer(ctx, "<cst-deep>", src);
  if (!raw)
    return 1;
  LibcpCstNode *root = libcp_cst_parse(ctx, raw);
  if (!root || !libcp_cst_verify(ctx, root, raw) ||
      libcp_context_failed(ctx))
    return 1;
  libcp_context_reset(ctx);
  return 0;
}

// PLAN-l5 W2: an include-provided typedef name is invisible to the
// pre-preprocess scan; seeding it lets the declaration oracle see it,
// and the seed survives reset as configuration.
static int cst_unparsed_count(LibcpCstNode *n) {
  int count = (n->kind == CST_UNPARSED) ? 1 : 0;
  for (int i = 0; i < n->child_count; i++)
    count += cst_unparsed_count(n->children[i]);
  return count;
}

static int check_cst_seed(LibcpContext *ctx) {
  char *src = "va_list ap;\n";

  LibcpToken *raw = libcp_tokenize_buffer(ctx, "<cst-seed>", src);
  if (!raw)
    return 1;
  LibcpCstNode *root = libcp_cst_parse(ctx, raw);
  if (!root || !libcp_cst_verify(ctx, root, raw))
    return 1;
  if (cst_unparsed_count(root) != 1)
    return 1;

  libcp_cst_seed_typedef(ctx, "va_list");
  raw = libcp_tokenize_buffer(ctx, "<cst-seed>", src);
  root = libcp_cst_parse(ctx, raw);
  if (!root || !libcp_cst_verify(ctx, root, raw) ||
      cst_unparsed_count(root) != 0)
    return 1;

  libcp_context_reset(ctx);
  raw = libcp_tokenize_buffer(ctx, "<cst-seed>", src);
  root = libcp_cst_parse(ctx, raw);
  if (!root || cst_unparsed_count(root) != 0)
    return 1;
  return 0;
}

// PLAN-l5 W5: a subtree maps back to a source span through its anchor
// tokens, and a macro-internal diagnostic carries the provenance chain
// to the call site.
static int check_source_range(LibcpContext *ctx) {
  char *src = "int f(void) { return 1+2; }\n";

  LibcpToken *tok = libcp_tokenize_buffer(ctx, "<range>", src);
  if (!tok)
    return 1;
  tok = libcp_preprocess(ctx, tok);
  if (!tok)
    return 1;
  LibcpNode *prog = libcp_parse(ctx, tok);
  if (!prog || prog->kind != ND_FUNCDEF)
    return 1;

  // f's body block -> return stmt -> the "+" node.
  LibcpNode *ret = prog->decl.body->decl.body;
  if (!ret || ret->kind != ND_RETURN)
    return 1;
  LibcpNode *add = ret->lhs;
  if (!add || add->kind != ND_ADD)
    return 1;

  LibcpToken *start = NULL, *end = NULL;
  libcp_node_source_range(add, &start, &end);
  if (!start || !end)
    return 1;
  if (start->raw_len != 1 || start->raw_loc[0] != '1')
    return 1;
  if (end->raw_len != 1 || end->raw_loc[0] != '2')
    return 1;

  // The whole function: its representative anchors run from the name
  // (a FUNCDEF's own tok is the body brace) to the last token.
  libcp_node_source_range(prog, &start, &end);
  if (!start || !end || start->raw_len != 1 || start->raw_loc[0] != 'f')
    return 1;
  if (end->raw_len != 1 || end->raw_loc[0] != '2')
    return 1;
  return 0;
}

// The backend support face (issue #17): a consumer that sees only the
// public headers builds every node a backend needs, reads back kind and
// fields, and gets sema's type conclusions through libcp_node_annotate_type.
static int check_backend_support(LibcpContext *ctx) {
  LibcpObj *prog = NULL;
  LibcpNode *ast = compile(ctx, "int g; int f(int a) { return a; }\n", &prog);
  if (!ast || !prog)
    return 1;

  LibcpObj *g = NULL;
  for (LibcpObj *o = prog; o; o = o->next)
    if (o->name && strcmp(o->name, "g") == 0)
      g = o;
  if (!g || !g->ty || g->ty->kind != TY_INT)
    return 1;

  LibcpToken *tok = ast->tok;
  if (!tok)
    return 1;

  // All 13 builtin kinds map to the shared singleton; a second call
  // returns the same record.
  static const struct {
    LibcpBuiltinType bt;
    LibcpTypeKind kind;
    int size;
    int align;
    int is_unsigned;
  } builtins[] = {
    {BT_VOID, TY_VOID, 1, 1, 0},
    {BT_BOOL, TY_BOOL, 1, 1, 0},
    {BT_CHAR, TY_CHAR, 1, 1, 0},
    {BT_UCHAR, TY_CHAR, 1, 1, 1},
    {BT_SHORT, TY_SHORT, 2, 2, 0},
    {BT_USHORT, TY_SHORT, 2, 2, 1},
    {BT_INT, TY_INT, 4, 4, 0},
    {BT_UINT, TY_INT, 4, 4, 1},
    {BT_LONG, TY_LONG, 8, 8, 0},
    {BT_ULONG, TY_LONG, 8, 8, 1},
    {BT_FLOAT, TY_FLOAT, 4, 4, 0},
    {BT_DOUBLE, TY_DOUBLE, 8, 8, 0},
    {BT_LDOUBLE, TY_LDOUBLE, 16, 16, 0},
  };
  if (sizeof(builtins) / sizeof(builtins[0]) != 13)
    return 1;
  for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
    LibcpType *ty = libcp_type_builtin(builtins[i].bt);
    if (!ty || ty->kind != builtins[i].kind || ty->size != builtins[i].size ||
        ty->align != builtins[i].align ||
        ty->is_unsigned != (builtins[i].is_unsigned != 0))
      return 1;
    if (libcp_type_builtin(builtins[i].bt) != ty)
      return 1;
  }

  LibcpType *ptr = libcp_type_pointer_to(ctx, libcp_type_builtin(BT_INT));
  if (!ptr || ptr->kind != TY_PTR || ptr->size != 8 || ptr->align != 8 ||
      !ptr->is_unsigned || ptr->base != libcp_type_builtin(BT_INT))
    return 1;

  // libcp_node_new leaves the payload zeroed and records kind and tok.
  LibcpNode *plain = libcp_node_new(ctx, ND_EXPR_STMT, tok);
  if (!plain || plain->kind != ND_EXPR_STMT || plain->tok != tok || plain->ty ||
      plain->lhs || plain->rhs || plain->next)
    return 1;

  LibcpNode *num = libcp_node_num(ctx, 42, tok);
  if (!num || num->kind != ND_NUM || num->value.val != 42 || num->tok != tok)
    return 1;

  LibcpNode *lng = libcp_node_long(ctx, 7, tok);
  if (!lng || lng->kind != ND_NUM || lng->value.val != 7 ||
      lng->ty != libcp_type_builtin(BT_LONG))
    return 1;

  LibcpNode *operand = libcp_node_num(ctx, 9, tok);
  LibcpNode *un = libcp_node_unary(ctx, ND_NEG, operand, tok);
  if (!un || un->kind != ND_NEG || un->lhs != operand || un->rhs ||
      un->tok != tok)
    return 1;

  LibcpNode *blhs = libcp_node_num(ctx, 1, tok);
  LibcpNode *brhs = libcp_node_num(ctx, 2, tok);
  LibcpNode *bin = libcp_node_binary(ctx, ND_ADD, blhs, brhs, tok);
  if (!bin || bin->kind != ND_ADD || bin->lhs != blhs || bin->rhs != brhs ||
      bin->tok != tok)
    return 1;
  libcp_node_annotate_type(ctx, bin);
  if (bin->ty != libcp_type_builtin(BT_INT) ||
      blhs->ty != libcp_type_builtin(BT_INT) ||
      brhs->ty != libcp_type_builtin(BT_INT))
    return 1;

  LibcpNode *ai = libcp_node_num(ctx, 3, tok);
  LibcpNode *al = libcp_node_long(ctx, 4, tok);
  // The usual arithmetic conversions wrap both operands in casts to the
  // common result type; each cast carries a copy whose origin is the
  // shared long type, and the node takes the left operand's copy.
  LibcpNode *arith = libcp_node_arith(ctx, ND_ADD, ai, al, tok);
  if (!arith || arith->kind != ND_ADD || !arith->ty ||
      arith->ty->kind != TY_LONG ||
      arith->ty->origin != libcp_type_builtin(BT_LONG) ||
      arith->lhs->ty != arith->ty || arith->rhs->ty->kind != TY_LONG ||
      arith->rhs->ty->origin != libcp_type_builtin(BT_LONG))
    return 1;

  LibcpNode *var = libcp_node_var(ctx, g, tok);
  if (!var || var->kind != ND_VAR || var->decl.var != g || var->tok != tok)
    return 1;
  libcp_node_annotate_type(ctx, var);
  if (var->ty != g->ty)
    return 1;

  LibcpNode *vla = libcp_node_vla_ptr(ctx, g, tok);
  if (!vla || vla->kind != ND_VLA_PTR || vla->decl.var != g || vla->tok != tok)
    return 1;

  LibcpNode *size = libcp_node_num(ctx, 8, tok);
  LibcpNode *alloc = libcp_node_alloca(ctx, size);
  if (!alloc || alloc->kind != ND_FUNCALL || alloc->ctrl.args != size ||
      !alloc->ctrl.func_ty || alloc->ty != alloc->ctrl.func_ty->return_ty)
    return 1;
  if (!alloc->lhs || alloc->lhs->kind != ND_VAR || !alloc->lhs->decl.var ||
      !alloc->lhs->decl.var->name ||
      strcmp(alloc->lhs->decl.var->name, "alloca") != 0)
    return 1;

  LibcpNode *src = libcp_node_num(ctx, 5, tok);
  LibcpType *target = libcp_type_builtin(BT_ULONG);
  LibcpNode *cast = libcp_node_cast(ctx, src, target);
  if (!cast || cast->kind != ND_CAST || cast->lhs != src || cast->tok != tok ||
      !cast->value.is_implicit || !cast->ty || cast->ty->origin != target ||
      cast->ty->kind != TY_LONG || !cast->ty->is_unsigned)
    return 1;

  return 0;
}

// The label references a backend reads (issue #18): after sema, a
// function's goto and [GNU] label-value references are available by
// count / at without touching the library's internal arrays.
static int check_backend_labels(LibcpContext *ctx) {
  LibcpObj *prog = NULL;
  LibcpNode *ast = compile(ctx,
                           "int f(int x) {\n"
                           "  if (x) goto done;\n"
                           "  goto done;\n"
                           "done:\n"
                           "  return 0;\n"
                           "}\n"
                           "int g(void) { return 1; }\n"
                           "int h;\n",
                           &prog);
  if (!ast || !prog)
    return 1;

  LibcpObj *f = NULL, *g = NULL, *h = NULL;
  for (LibcpObj *o = prog; o; o = o->next) {
    if (o->name && strcmp(o->name, "f") == 0)
      f = o;
    if (o->name && strcmp(o->name, "g") == 0)
      g = o;
    if (o->name && strcmp(o->name, "h") == 0)
      h = o;
  }
  if (!f || !g || !h)
    return 1;

  // Two gotos, both naming `done`; the array only carries references, so
  // the label itself is not among them.
  if (libcp_label_ref_count(ctx, f) != 2)
    return 1;
  for (int i = 0; i < 2; i++) {
    LibcpNode *ref = libcp_label_ref_at(ctx, f, i);
    if (!ref || ref->kind != ND_GOTO || !ref->jump.label ||
        strcmp(ref->jump.label, "done") != 0)
      return 1;
  }
  // Out-of-range indexes and functions without references give NULL.
  if (libcp_label_ref_at(ctx, f, 2) || libcp_label_ref_at(ctx, f, -1))
    return 1;
  if (libcp_label_ref_count(ctx, g) != 0 || libcp_label_ref_at(ctx, g, 0))
    return 1;
  // A non-function object never collected references.
  if (libcp_label_ref_count(ctx, h) != 0 || libcp_label_ref_at(ctx, h, 0))
    return 1;

  return 0;
}

// Unique names (issue #18): the counter behind sema's anonymous `.L..N`
// objects is exposed as take. Every call consumes a fresh id, so a
// consumer's labels never collide with the library's.
static int check_backend_unique_name(LibcpContext *ctx) {
  LibcpObj *prog = NULL;
  if (!compile(ctx, "char *s = \"x\";\n", &prog) || !prog)
    return 1;

  // The string literal already consumed `.L..0`, so the next id must be
  // past every suffix sema minted in this translation unit.
  int max_seen = -1;
  for (LibcpObj *o = prog; o; o = o->next) {
    if (o->name && strncmp(o->name, ".L..", 4) == 0) {
      int v = atoi(o->name + 4);
      if (v > max_seen)
        max_seen = v;
    }
  }
  if (max_seen < 0)
    return 1;

  int first = libcp_context_take_unique_name(ctx);
  int second = libcp_context_take_unique_name(ctx);
  int third = libcp_context_take_unique_name(ctx);
  if (first <= max_seen || second == first || third == first ||
      third == second)
    return 1;
  return 0;
}

static int rep_hits;
static LibcpDiagnosticSeverity rep_severity;
static const char *rep_filename;
static const char *rep_message;
static int rep_line;
static int rep_column;
static const LibcpToken *rep_tok;
static const struct libcp_macro_prov *rep_prov;

static void rep_diag(void *user, const LibcpDiagnostic *d) {
  (void)user;
  rep_hits++;
  rep_severity = d->severity;
  rep_filename = d->filename;
  rep_message = d->message;
  rep_line = d->line;
  rep_column = d->column;
  rep_tok = d->tok;
  rep_prov = d->prov;
}

// The returning diagnostic entry (issue #18): report never longjmps,
// still calls the callback, and only errors mark the context failed.
// The error wrappers built on it keep the old unwinding behavior.
static int check_diagnostic_report(LibcpContext *ctx) {
  LibcpToken *tok = libcp_tokenize_buffer(ctx, "<report>", "int x;\n");
  if (!tok)
    return 1;

  libcp_context_set_diagnostic(ctx, rep_diag, NULL);

  int before = rep_hits;
  libcp_diagnostic_report(ctx, tok, LIBCP_DIAG_WARNING, "warn %d", 7);
  if (rep_hits != before + 1 || libcp_context_failed(ctx))
    return 1;
  if (rep_severity != LIBCP_DIAG_WARNING || !rep_message ||
      strcmp(rep_message, "warn 7") != 0)
    return 1;
  if (!rep_filename || strcmp(rep_filename, "<report>") != 0 ||
      rep_line != 1 || rep_column != 1 || rep_tok != tok || rep_prov)
    return 1;

  // An error report returns too, but marks the context failed and
  // carries no location when it has no token.
  libcp_diagnostic_report(ctx, NULL, LIBCP_DIAG_ERROR, "boom");
  if (rep_hits != before + 2 || !libcp_context_failed(ctx))
    return 1;
  if (rep_severity != LIBCP_DIAG_ERROR || rep_filename || rep_line != 0 ||
      rep_column != 0 || rep_tok || !rep_message ||
      strcmp(rep_message, "boom") != 0)
    return 1;

  // The error wrappers still longjmp: a parse error unwinds the public
  // entry (it returns NULL) instead of continuing.
  libcp_context_reset(ctx);
  libcp_context_set_diagnostic(ctx, diag, NULL);
  LibcpToken *bad = libcp_tokenize_buffer(ctx, "<report>", "int y = ;\n");
  if (!bad)
    return 1;
  bad = libcp_preprocess(ctx, bad);
  if (!bad)
    return 1;
  int errors_before = errors;
  if (libcp_parse(ctx, bad) != NULL || !libcp_context_failed(ctx) ||
      errors != errors_before + 1)
    return 1;
  libcp_context_reset(ctx);
  return 0;
}

static LibcpToken *exp_diag_tok;
static const struct libcp_macro_prov *exp_diag_prov;

static void exp_diag(void *user, const LibcpDiagnostic *d) {
  (void)user;
  exp_diag_tok = (LibcpToken *)d->tok;
  exp_diag_prov = d->prov;
}

static int check_diag_expansion(LibcpContext *ctx) {
  libcp_context_set_diagnostic(ctx, exp_diag, NULL);
  int rc = 1;

  // A plain source error: anchored, no provenance.
  LibcpToken *tok = libcp_tokenize_buffer(ctx, "<exp>", "int x = ;\n");
  if (!tok)
    goto out;
  tok = libcp_preprocess(ctx, tok);
  if (!tok)
    goto out;
  libcp_parse(ctx, tok);
  if (!libcp_context_failed(ctx) || !exp_diag_tok || exp_diag_prov)
    goto out;
  libcp_context_reset(ctx);

  // An error inside a macro body: the diagnostic carries the chain; the
  // outermost expansion names the macro at the call site.
  tok = libcp_tokenize_buffer(ctx, "<exp>",
                              "#define M(x) ((x)+1)\n"
                              "int y = M(1+);\n");
  if (!tok)
    goto out;
  tok = libcp_preprocess(ctx, tok);
  if (!tok)
    goto out;
  libcp_parse(ctx, tok);
  if (!libcp_context_failed(ctx) || !exp_diag_tok || !exp_diag_prov)
    goto out;

  LibcpMacroProv *p = (LibcpMacroProv *)exp_diag_prov;
  while (libcp_macro_prov_next(p))
    p = libcp_macro_prov_next(p);
  LibcpToken *name = libcp_macro_expansion_name(libcp_macro_prov_expansion(p));
  if (!name || name->len != 1 || name->loc[0] != 'M')
    goto out;
  rc = 0;

out:
  libcp_context_reset(ctx);
  libcp_context_set_diagnostic(ctx, diag, NULL);
  return rc;
}

static int check_cst_depth(LibcpContext *ctx) {
  size_t n = 2000, i;
  char *src = malloc(2 * n + 64);
  if (!src)
    return 1;
  i = 0;
  memcpy(src + i, "int f(void) { return ", 21);
  i += 21;
  for (size_t j = 0; j < n; j++)
    src[i++] = '(';
  src[i++] = '1';
  for (size_t j = 0; j < n; j++)
    src[i++] = ')';
  src[i++] = ';';
  src[i++] = '}';
  src[i++] = '\n';
  src[i] = '\0';
  int rc = cst_depth_case(ctx, src);

  if (rc == 0) {
    i = 0;
    memcpy(src + i, "int f(void) {", 13);
    i += 13;
    for (size_t j = 0; j < n; j++)
      src[i++] = '{';
    for (size_t j = 0; j < n; j++)
      src[i++] = '}';
    src[i++] = '}';
    src[i++] = '\n';
    src[i] = '\0';
    rc = cst_depth_case(ctx, src);
  }
  free(src);
  return rc;
}

// decode_utf8 rejects overlong forms, surrogates and code points above
// U+10FFFF, and accepts the shortest-form boundaries on both sides.
static int check_utf8(LibcpContext *ctx) {
  struct {
    unsigned char bytes[8];
    int len;
  } bad[] = {
    {{'u', '"', 0xC0, 0x80, '"'}, 5},
    {{'u', '"', 0xE0, 0x80, 0x80, '"'}, 6},
    {{'u', '"', 0xF0, 0x80, 0x80, 0x80, '"'}, 7},
    {{'u', '"', 0xED, 0xA0, 0x80, '"'}, 6},
    {{'u', '"', 0xF4, 0x90, 0x80, 0x80, '"'}, 7},
    {{'u', '"', 0xF5, 0x80, 0x80, 0x80, '"'}, 7},
    {{'u', '"', 0xF8, 0xBF, 0xBF, 0xBF, '"'}, 7},
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
    int before = errors;
    LibcpToken *tok = libcp_tokenize_buffer_len(
        ctx, "<bad-utf8>", (char *)bad[i].bytes, bad[i].len);
    if (tok || !libcp_context_failed(ctx) || errors != before + 1 ||
        !last_message || strcmp(last_message, "invalid UTF-8 sequence") != 0)
      return 1;
    libcp_context_reset(ctx);
  }

  struct {
    unsigned char bytes[8];
    int len;
  } good[] = {
    {{'u', '"', 0xC2, 0x80, '"'}, 5},                // U+0080
    {{'u', '"', 0xDF, 0xBF, '"'}, 5},                // U+07FF
    {{'u', '"', 0xE0, 0xA0, 0x80, '"'}, 6},          // U+0800
    {{'u', '"', 0xED, 0x9F, 0xBF, '"'}, 6},          // U+D7FF
    {{'u', '"', 0xEE, 0x80, 0x80, '"'}, 6},          // U+E000
    {{'u', '"', 0xEF, 0xBF, 0xBF, '"'}, 6},          // U+FFFF
    {{'u', '"', 0xF0, 0x90, 0x80, 0x80, '"'}, 7},    // U+10000
    {{'u', '"', 0xF4, 0x8F, 0xBF, 0xBF, '"'}, 7},    // U+10FFFF
  };
  for (size_t i = 0; i < sizeof(good) / sizeof(*good); i++) {
    int before = errors;
    LibcpToken *tok = libcp_tokenize_buffer_len(
        ctx, "<ok-utf8>", (char *)good[i].bytes, good[i].len);
    if (!tok || libcp_context_failed(ctx) || errors != before)
      return 1;
    libcp_context_reset(ctx);
  }
  return 0;
}

#ifdef LIBCP_TEST_BUILTINS
// A builtin handler that produces two tokens must keep both (B8).
static int check_multi_builtin(LibcpContext *ctx) {
  LibcpToken *tok =
      libcp_tokenize_buffer(ctx, "<multi>", "int x = __LIBCP_MULTI__;");
  if (!tok)
    return 1;
  tok = libcp_preprocess(ctx, tok);
  if (!tok || !find_num(tok, 1) || !find_num(tok, 2) ||
      libcp_context_failed(ctx))
    return 1;
  libcp_context_reset(ctx);
  return 0;
}
#endif

// The predefined set can be enumerated and disabled; __SIZEOF_* comes
// from the type records.
static int has_predefined(LibcpContext *ctx, char *name, char *value) {
  int n = libcp_context_predefined_count(ctx);
  for (int i = 0; i < n; i++) {
    LibcpPredefinedMacro m = libcp_context_predefined_at(ctx, i);
    if (m.name && strcmp(m.name, name) == 0)
      return value ? (m.value && strcmp(m.value, value) == 0)
                   : m.value == NULL;
  }
  return 0;
}

static int check_predefined_macros(LibcpContext *ctx) {
  if (libcp_context_predefined_count(ctx) <= 0)
    return 1;
  if (!has_predefined(ctx, "__STDC__", "1") ||
      !has_predefined(ctx, "__SIZEOF_LONG_DOUBLE__", "16") ||
      !has_predefined(ctx, "__SIZEOF_INT__", "4") ||
      !has_predefined(ctx, "__FILE__", NULL) ||
      !has_predefined(ctx, "__DATE__", NULL))
    return 1;

  libcp_context_set_predefined_macros(ctx, false);
  libcp_context_reset(ctx);
  if (libcp_context_predefined_count(ctx) != 0)
    return 1;

  LibcpToken *tok =
      libcp_tokenize_buffer(ctx, "<predef>", "int x = __STDC__;");
  if (!tok)
    return 1;
  tok = libcp_preprocess(ctx, tok);
  if (!tok || find_num(tok, 1) || libcp_context_failed(ctx))
    return 1;
  int found = 0;
  for (LibcpToken *t = tok; t && t->kind != TK_EOF; t = t->next)
    if (t->kind == TK_IDENT && t->len == 8 &&
        !memcmp(t->loc, "__STDC__", 8))
      found = 1;
  if (!found)
    return 1;

  libcp_context_set_predefined_macros(ctx, true);
  libcp_context_reset(ctx);
  tok = libcp_tokenize_buffer(ctx, "<predef>", "int x = __STDC__;");
  if (!tok)
    return 1;
  tok = libcp_preprocess(ctx, tok);
  if (!tok || !find_num(tok, 1) || libcp_context_failed(ctx))
    return 1;

  libcp_context_reset(ctx);
  return 0;
}

// libcp_reconstruct rebuilds the tokenized bytes exactly.
static int check_reconstruct(LibcpContext *ctx) {
  char *text = "int  x =  1;  /* c */\n\tchar *s = \"a\";\n";
  LibcpToken *tok = libcp_tokenize_buffer(ctx, "<reconstruct>", text);
  FILE *f = tmpfile();
  if (!tok || !f)
    return 1;
  libcp_reconstruct(ctx, tok, f);
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
static int check_provenance(LibcpContext *ctx) {
  LibcpToken *tok =
    libcp_tokenize_buffer(ctx, "<prov>", "#define F(x) x+1\nint a = F(2);\n");
  if (!tok)
    return 1;
  tok = libcp_preprocess(ctx, tok);
  if (!tok)
    return 1;

  LibcpToken *two = find_num(tok, 2);
  if (!two)
    return 1;
  LibcpMacroProv *p = libcp_token_provenance(two);
  if (!p)
    return 1;
  LibcpMacroExpansion *e = libcp_macro_prov_expansion(p);
  if (!e || !libcp_macro_expansion_name(e) ||
      strcmp(libcp_macro_expansion_macro(e), "F") != 0 ||
      libcp_macro_expansion_arg_count(e) != 1 ||
      libcp_macro_prov_arg_index(p) != 0 ||
      libcp_macro_expansion_is_builtin(e) ||
      libcp_macro_prov_next(p) != NULL)
    return 1;
  LibcpToken *arg = libcp_macro_expansion_arg(e, 0);
  if (!arg || arg->len != 1 || arg->loc[0] != '2')
    return 1;

  tok = libcp_tokenize_buffer(ctx, "<prov>", "int b = __LINE__;\n");
  if (!tok)
    return 1;
  tok = libcp_preprocess(ctx, tok);
  if (!tok)
    return 1;
  LibcpToken *one = find_num(tok, 1);
  if (!one)
    return 1;
  p = libcp_token_provenance(one);
  if (!p || !libcp_macro_prov_expansion(p) ||
      !libcp_macro_expansion_is_builtin(libcp_macro_prov_expansion(p)))
    return 1;
  return 0;
}

// tokenize_file + the context's include path.
static int check_include(LibcpContext *ctx, char *path, char *dir) {
  libcp_context_add_include_path(ctx, dir);
  LibcpToken *tok = libcp_tokenize_file(ctx, path);
  if (!tok)
    return 1;
  tok = libcp_preprocess(ctx, tok);
  if (!tok)
    return 1;
  return find_num(tok, 7) ? 0 : 1;
}

// The recorded macro operations: define applies immediately, undef
// removes the name.
static int check_define(LibcpContext *ctx) {
  libcp_context_define(ctx, "LIBFOO", "42");
  LibcpToken *tok = libcp_tokenize_buffer(ctx, "<def>", "int y = LIBFOO;\n");
  if (!tok)
    return 1;
  tok = libcp_preprocess(ctx, tok);
  if (!tok || !find_num(tok, 42))
    return 1;

  libcp_context_undef(ctx, "LIBFOO");
  tok = libcp_tokenize_buffer(ctx, "<undef>", "int y = LIBFOO;\n");
  if (!tok)
    return 1;
  tok = libcp_preprocess(ctx, tok);
  if (!tok || find_num(tok, 42))
    return 1;
  return 0;
}

// A malformed macro body in libcp_context_define raises to the callback
// and returns instead of exiting the host process; the failed op is not
// recorded, so the reset replay stays valid and the context keeps
// working (CodeReview 2026-10-24 A1).
static int check_define_bad_body(LibcpContext *ctx) {
  int before = errors;
  libcp_context_define(ctx, "BAD_BODY", "\"oops");
  if (!libcp_context_failed(ctx) || errors != before + 1)
    return 1;
  libcp_context_reset(ctx);

  // The name stays an identifier: the failed op never entered the
  // replay (a recorded half-applied op would silently define it as 1).
  LibcpToken *tok =
    libcp_tokenize_buffer(ctx, "<def>", "int y = BAD_BODY;\n");
  if (!tok)
    return 1;
  tok = libcp_preprocess(ctx, tok);
  if (!tok || libcp_context_failed(ctx))
    return 1;
  int ident = 0;
  for (LibcpToken *t = tok; t && t->kind != TK_EOF; t = t->next)
    if (t->kind == TK_IDENT && t->len == 8 &&
        !memcmp(t->loc, "BAD_BODY", 8))
      ident = 1;
  if (!ident)
    return 1;

  // A define after the failure still records and replays across a reset.
  libcp_context_define(ctx, "AFTER", "7");
  libcp_context_reset(ctx);
  tok = libcp_tokenize_buffer(ctx, "<def>", "int y = AFTER;\n");
  if (!tok)
    return 1;
  tok = libcp_preprocess(ctx, tok);
  if (!tok || !find_num(tok, 7) || libcp_context_failed(ctx))
    return 1;
  return 0;
}

// Every public entry unwinds to the callback and returns its failure
// value without exiting (PLAN L3.4b).
static int check_errors(LibcpContext *ctx) {
  int before;

  // Missing file.
  before = errors;
  if (libcp_tokenize_file(ctx, "/no/such/chibicc-libtest.c") != NULL)
    return 1;
  if (!libcp_context_failed(ctx) || errors != before + 1)
    return 1;
  libcp_context_reset(ctx);

  // Bad directive.
  before = errors;
  LibcpToken *t = libcp_tokenize_buffer(ctx, "<bad>", "#error boom\n");
  if (!t || libcp_preprocess(ctx, t) != NULL)
    return 1;
  if (!libcp_context_failed(ctx) || errors != before + 1)
    return 1;
  libcp_context_reset(ctx);

  // Bad expression.
  before = errors;
  t = libcp_tokenize_buffer(ctx, "<bad>", "int x = ;\n");
  if (!t)
    return 1;
  t = libcp_preprocess(ctx, t);
  if (!t || libcp_parse(ctx, t) != NULL)
    return 1;
  if (!libcp_context_failed(ctx) || errors != before + 1)
    return 1;
  libcp_context_reset(ctx);

  // Bad type (an undeclared name fails sema).
  before = errors;
  t = libcp_tokenize_buffer(ctx, "<bad>", "int f(void) { return missing; }\n");
  if (!t)
    return 1;
  t = libcp_preprocess(ctx, t);
  LibcpNode *ast = t ? libcp_parse(ctx, t) : NULL;
  if (!ast || libcp_sema(ctx, ast) != NULL)
    return 1;
  if (!libcp_context_failed(ctx) || errors != before + 1)
    return 1;
  libcp_context_reset(ctx);
  return 0;
}

// Two contexts in one thread: a diagnostic callback of one context runs a
// public entry of the other while the first is unwinding (PLAN L3.4b).
static LibcpContext *nested_target;
static int nested_ok;

static void nested_diag(void *user, const LibcpDiagnostic *d) {
  (void)user;
  if (!d->message || !*d->message)
    nested_ok = 0;
  if (!nested_target)
    return;
  LibcpContext *a = nested_target;
  nested_target = NULL;
  LibcpToken *t = libcp_tokenize_buffer(a, "<nested>", "int n;\n");
  if (t)
    t = libcp_preprocess(a, t);
  nested_ok = (t != NULL);
}

static int check_nested(LibcpContext *a) {
  LibcpContext *b = libcp_context_new();
  nested_target = a;
  nested_ok = 1;
  libcp_context_set_diagnostic(b, nested_diag, NULL);

  LibcpToken *t = libcp_tokenize_buffer(b, "<nest>", "int x = ;\n");
  if (!t)
    return 1;
  t = libcp_preprocess(b, t);
  if (!t)
    return 1;
  (void)libcp_parse(b, t);
  int ok = nested_ok && libcp_context_failed(b);
  libcp_context_free(b);

  LibcpObj *prog = NULL;
  if (!compile(a, "int z;\n", &prog) || !prog)
    ok = 0;
  return ok ? 0 : 1;
}

// A diagnostic callback creates, configures, uses, and frees another
// context; the outer error must still unwind to the outer context's own
// frame, and the outer context must keep working afterwards
// (PLAN-explicit-context E2.3). With the old thread-local accessor the
// inner free left the active pointer NULL and the outer error exited.
static int scope_ok;

static void scope_diag(void *user, const LibcpDiagnostic *d) {
  (void)user;
  if (!d->message || !*d->message)
    scope_ok = 0;

  LibcpContext *inner = libcp_context_new();
  if (!inner) {
    scope_ok = 0;
    return;
  }
  libcp_context_define(inner, "SCOPED", "1");
  LibcpToken *t = libcp_tokenize_buffer(inner, "<scope>", "int SCOPED;\n");
  if (t)
    t = libcp_preprocess(inner, t);
  if (!t || libcp_context_failed(inner))
    scope_ok = 0;
  libcp_context_free(inner);
}

static int check_context_scope(LibcpContext *a) {
  LibcpContext *b = libcp_context_new();
  scope_ok = 1;
  libcp_context_set_diagnostic(b, scope_diag, NULL);

  LibcpToken *t = libcp_tokenize_buffer(b, "<scope>", "int x = ;\n");
  if (!t)
    return 1;
  t = libcp_preprocess(b, t);
  if (!t)
    return 1;
  (void)libcp_parse(b, t);
  int ok = scope_ok && libcp_context_failed(b);
  libcp_context_free(b);

  LibcpObj *prog = NULL;
  if (!compile(a, "int scope_z;\n", &prog) || !prog)
    ok = 0;
  return ok ? 0 : 1;
}

// Two contexts in two threads (PLAN L3.4b). Each pipeline passes its
// own context explicitly and contexts carry no shared state, so the two
// never see each other.
static void *thread_main(void *arg) {
  char *src = arg;
  LibcpContext *ctx = libcp_context_new();
  LibcpToken *t = libcp_tokenize_buffer(ctx, "<thread>", src);
  if (t)
    t = libcp_preprocess(ctx, t);
  LibcpNode *ast = t ? libcp_parse(ctx, t) : NULL;
  LibcpObj *prog = ast ? libcp_sema(ctx, ast) : NULL;
  int ok = prog && !libcp_context_failed(ctx);
  libcp_context_free(ctx);
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
  LibcpContext *c = libcp_context_new();
  LibcpToken *t = libcp_tokenize_buffer(c, "<free>", src);
  if (t)
    t = libcp_preprocess(c, t);
  LibcpNode *ast = t ? libcp_parse(c, t) : NULL;
  LibcpObj *prog = ast ? libcp_sema(c, ast) : NULL;
  int ok = prog != NULL;

  LibcpToken *raw = libcp_tokenize_buffer(c, "<free>", src);
  LibcpCstNode *root = libcp_cst_parse(c, raw);
  if (!root || !libcp_cst_verify(c, root, raw))
    ok = 0;

  libcp_context_free(c);
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

  LibcpContext *ctx = libcp_context_new();
  libcp_context_set_diagnostic(ctx, diag, NULL);

  LibcpObj *prog = NULL;
  LibcpNode *ast = compile(ctx, src, &prog);
  if (!ast || !prog)
    return 1;

  // The raw stream remains available for the CST channel.
  LibcpToken *raw = libcp_tokenize_buffer(ctx, "<libtest>", src);
  LibcpCstNode *root = libcp_cst_parse(ctx, raw);
  if (!root || !libcp_cst_verify(ctx, root, raw))
    return 1;

  // A bad input reaches the callback and unwinds instead of exiting.
  LibcpToken *bad = libcp_tokenize_buffer(ctx, "<libtest>", "int x = ;\n");
  if (!bad)
    return 1;
  bad = libcp_preprocess(ctx, bad);
  if (!bad)
    return 1;
  libcp_parse(ctx, bad);
  if (!libcp_context_failed(ctx) || errors != 1)
    return 1;

  // Reset releases every per-TU allocation and clears the state; the
  // context is reusable.
  libcp_context_reset(ctx);
  prog = NULL;
  ast = compile(ctx, src, &prog);
  if (!ast || !prog || libcp_context_failed(ctx) || errors != 1)
    return 1;

  libcp_context_reset(ctx);
  if (check_type_names(ctx))
    return 1;
  libcp_context_reset(ctx);

  if (check_nul_buffer(ctx))
    return 1;
  if (argc > 3 && check_nul_file(ctx, argv[3]))
    return 1;
  if (argc > 4 && check_include_depth(ctx, argv[4]))
    return 1;
  if (argc > 5 && check_stdin_twice(ctx, argv[5]))
    return 1;
  if (check_parse_depth(ctx))
    return 1;
  if (check_cst_depth(ctx))
    return 1;
  if (check_cst_seed(ctx))
    return 1;
  if (check_source_range(ctx))
    return 1;
  if (check_backend_support(ctx))
    return 1;
  if (check_backend_labels(ctx))
    return 1;
  if (check_backend_unique_name(ctx))
    return 1;
  if (check_diagnostic_report(ctx))
    return 1;
  libcp_context_reset(ctx);
  if (check_diag_expansion(ctx))
    return 1;
  if (check_utf8(ctx))
    return 1;
#ifdef LIBCP_TEST_BUILTINS
  if (check_multi_builtin(ctx))
    return 1;
#endif
  if (check_predefined_macros(ctx))
    return 1;

  // The layer-1 reconstruct channel, the provenance accessors, the
  // tokenize_file path with a context include path, and the recorded
  // macro operations (PLAN L3.4a).
  if (check_reconstruct(ctx))
    return 1;
  if (check_provenance(ctx))
    return 1;
  libcp_context_reset(ctx);
  if (argc > 2 && check_include(ctx, argv[1], argv[2]))
    return 1;
  libcp_context_reset(ctx);
  if (check_define(ctx))
    return 1;
  libcp_context_reset(ctx);
  if (check_define_bad_body(ctx))
    return 1;
  libcp_context_reset(ctx);
  if (check_errors(ctx))
    return 1;
  if (check_nested(ctx))
    return 1;
  if (check_context_scope(ctx))
    return 1;
  if (check_threads())
    return 1;
  if (!diag_fields_ok || diag_located == 0)
    return 1;

  // Every CstNode and every children array is a per-TU allocation, so a
  // reset releases the whole tree (PLAN L3.0). Under ASan the live-byte
  // count must not grow from iteration to iteration: the pre-fix children
  // arrays (a bare realloc) leaked about 1.6 KiB per iteration on this
  // source, while the per-TU version ends each iteration empty.
  libcp_context_reset(ctx);
#ifdef HAVE_ASAN
  size_t before = __sanitizer_get_current_allocated_bytes();
#endif
  for (int i = 0; i < 64; i++) {
    LibcpToken *t = libcp_tokenize_buffer(ctx, "<libtest>", src);
    if (!t)
      return 1;
    LibcpCstNode *r = libcp_cst_parse(ctx, t);
    if (!r || !libcp_cst_verify(ctx, r, t))
      return 1;
    libcp_context_reset(ctx);
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

  libcp_context_free(ctx);
  puts("library: ok");
  return 0;
}
EOF

"$cc" -std=c11 -pthread -I"$root/libcp/include" "$tmp/consumer.c" "$lib" -o "$tmp/consumer"
"$tmp/consumer" "$tmp/input.c" "$tmp/inc" "$tmp/nul.c" "$tmp/self.h" "$tmp/stdin.c"

# The sanitizer gate (PLAN L3.0/L3.4c). The library sources themselves are
# rebuilt with ASan and UBSan, so the coverage reaches CST, parse, sema and
# reset; the reset loop also asserts that live bytes do not grow. LSan is
# deliberately not the gate - it reports only intermittently under Rosetta.
mkdir -p "$tmp/asan"
asan_objs=
for src in "$root"/libcp/src/*.c; do
  base=$(basename "$src" .c)
  "$cc" -std=c11 -g -Wall -Wno-switch -DLIBCP_DEFAULT_ALLOCATOR -DLIBCP_TEST_BUILTINS -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$root/libcp/include" -I"$root/libcp/src" -c "$src" -o "$tmp/asan/$base.o"
  asan_objs="$asan_objs $tmp/asan/$base.o"
done
ar rcs "$tmp/libcp-asan.a" $asan_objs
"$cc" -std=c11 -pthread -g -DLIBCP_TEST_BUILTINS -fsanitize=address,undefined \
  -fno-omit-frame-pointer -I"$root/libcp/include" "$tmp/consumer.c" \
  "$tmp/libcp-asan.a" -o "$tmp/consumer-asan"
ASAN_OPTIONS=detect_leaks=0 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  "$tmp/consumer-asan" "$tmp/input.c" "$tmp/inc" "$tmp/nul.c" "$tmp/self.h" "$tmp/stdin.c"

# The shared library (PLAN L3.3b, Linux only): the same consumer links
# against it and runs.
if [ -z "${LIBCP_LIB:-}" ] && [ -f "$root/libcp.so" ]; then
  "$cc" -std=c11 -pthread -I"$root/libcp/include" "$tmp/consumer.c" \
    -L"$root" -lcp -Wl,-rpath,"$root" -o "$tmp/consumer-so"
  "$tmp/consumer-so" "$tmp/input.c" "$tmp/inc" "$tmp/nul.c" "$tmp/self.h" "$tmp/stdin.c"
fi
