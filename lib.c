// The library facade (PLAN L2): the context object, the arena, and the
// public entry points. The phases themselves stay in their own files;
// everything here is the boundary the library promises.

#include "chibicc.h"

_Thread_local ChibiccContext *chibicc_active_ctx;

const char *chibicc_version(void) {
  return CHIBICC_VERSION;
}

ChibiccContext *chibicc_ctx(void) {
  return chibicc_active_ctx;
}

// Every public entry is an error boundary: an error_* longjmps to the
// innermost frame, and the entry returns its failure value.
#define ENTER_RET(ctx, fail)                                              \
  ChibiccContext *_prev = chibicc_active_ctx;                             \
  JmpFrame _jf;                                                           \
  _jf.prev = (ctx)->jmp_top;                                              \
  (ctx)->jmp_top = &_jf;                                                  \
  chibicc_active_ctx = (ctx);                                             \
  if (setjmp(_jf.buf)) {                                                  \
    (ctx)->jmp_top = _jf.prev;                                            \
    chibicc_active_ctx = _prev;                                           \
    return fail;                                                          \
  }

#define ENTER_VOID(ctx)                                                   \
  ChibiccContext *_prev = chibicc_active_ctx;                             \
  JmpFrame _jf;                                                           \
  _jf.prev = (ctx)->jmp_top;                                              \
  (ctx)->jmp_top = &_jf;                                                  \
  chibicc_active_ctx = (ctx);                                             \
  if (setjmp(_jf.buf)) {                                                  \
    (ctx)->jmp_top = _jf.prev;                                            \
    chibicc_active_ctx = _prev;                                           \
    return;                                                               \
  }

#define LEAVE(ctx)                                                        \
  do {                                                                    \
    (ctx)->jmp_top = _jf.prev;                                            \
    chibicc_active_ctx = _prev;                                           \
  } while (0)

#define ARENA_BLOCK_SIZE (64 * 1024)

#ifdef CHIBICC_DEBUG_ARENA
// One allocation per object for the sanitizer gate: the bump arena hides
// intra-block overflows from AddressSanitizer's redzones. test/library.sh
// enables this in its ASan round.
static void *arena_alloc(ChibiccContext *ctx, size_t size, size_t align) {
  (void)align;
  if (!ctx)
    return calloc(1, size ? size : 1);
  if (size == 0)
    size = 1;

  ArenaBlock *b = calloc(1, sizeof(ArenaBlock));
  if (!b)
    return NULL;
  b->data = malloc(size);
  if (!b->data) {
    free(b);
    return NULL;
  }
  b->cap = size;
  b->used = size;
  b->next = ctx->arena;
  ctx->arena = b;
  return b->data;
}
#else
static void *arena_alloc(ChibiccContext *ctx, size_t size, size_t align) {
  if (!ctx)
    return calloc(1, size ? size : 1);

  if (size == 0)
    size = 1;
  if (align < 8)
    align = 8;

  ArenaBlock *b = ctx->arena;
  size_t start = b ? (b->used + align - 1) / align * align : 0;
  if (!b || start + size > b->cap) {
    size_t cap = ARENA_BLOCK_SIZE;
    if (cap < size + align)
      cap = size + align;
    ArenaBlock *nb = calloc(1, sizeof(ArenaBlock));
    nb->data = malloc(cap);
    nb->cap = cap;
    nb->next = b;
    ctx->arena = nb;
    b = nb;
    start = 0;
  }

  void *p = b->data + start;
  b->used = start + size;
  return p;
}
#endif

void *chibicc_arena_alloc(size_t size, size_t align) {
  return arena_alloc(chibicc_active_ctx, size, align);
}

void *chibicc_arena_calloc(size_t count, size_t size) {
  // The arena path multiplies by hand; fail instead of wrapping to a
  // small allocation that memset would then overflow.
  if (size && count > SIZE_MAX / size)
    return NULL;
  size_t n = count * size;
  if (!chibicc_active_ctx)
    return calloc(count, size);
  // 16, not 8: Token and Node embed a long double, which needs 16-byte
  // alignment on x86-64 (UBSan found this in L3.4c).
  void *p = arena_alloc(chibicc_active_ctx, n, 16);
  memset(p, 0, n);
  return p;
}

char *chibicc_arena_strdup(char *s) {
  if (!s)
    return NULL;
  size_t n = strlen(s) + 1;
  char *p = arena_alloc(chibicc_active_ctx, n, 1);
  memcpy(p, s, n);
  return p;
}

char *chibicc_arena_strndup(char *s, size_t n) {
  size_t len = strnlen(s, n);
  char *p = arena_alloc(chibicc_active_ctx, len + 1, 1);
  memcpy(p, s, len);
  p[len] = '\0';
  return p;
}

void *chibicc_arena_realloc(void *old, size_t old_size, size_t new_size) {
  if (!chibicc_active_ctx)
    return realloc(old, new_size);
  void *p = arena_alloc(chibicc_active_ctx, new_size, 8);
  if (old && old_size)
    memcpy(p, old, old_size < new_size ? old_size : new_size);
  return p;
}

static void config_push(char ***arr, int *len, int *cap, char *s) {
  if (*len + 1 >= *cap) {
    int ncap = *cap ? *cap * 2 : 8;
    char **n = realloc(*arr, sizeof(char *) * ncap);
    // On OOM keep the old array and capacity; the caller's string is
    // not stored. A void configuration setter has no report path.
    if (!n)
      return;
    *arr = n;
    *cap = ncap;
  }
  (*arr)[(*len)++] = s;
  (*arr)[*len] = NULL;
}

void chibicc_context_add_include_path(ChibiccContext *ctx, char *path) {
  config_push(&ctx->include_paths, &ctx->include_paths_len,
              &ctx->include_paths_cap, strdup(path));
}

void chibicc_context_set_base_file(ChibiccContext *ctx, char *path) {
  chibicc_active_ctx = ctx;
  ctx->base_file = chibicc_arena_strdup(path);
}

static void apply_macro_op(ChibiccContext *ctx, char *op) {
  chibicc_active_ctx = ctx;
  if (op[0] == 'D') {
    char *name = op + 2;
    char *eq = strchr(name, '=');
    if (eq) {
      *eq = '\0';
      define_macro(name, eq + 1);
      *eq = '=';
    } else {
      define_macro(name, "1");
    }
  } else {
    undef_macro(op + 2);
  }
}

void chibicc_context_define(ChibiccContext *ctx, char *name, char *buf) {
  size_t n = strlen(name) + strlen(buf) + 4;
  char *s = calloc(1, n);
  snprintf(s, n, "D:%s=%s", name, buf);
  config_push(&ctx->macro_ops, &ctx->macro_ops_len, &ctx->macro_ops_cap, s);
  apply_macro_op(ctx, s);
}

void chibicc_context_undef(ChibiccContext *ctx, char *name) {
  size_t n = strlen(name) + 3;
  char *s = calloc(1, n);
  snprintf(s, n, "U:%s", name);
  config_push(&ctx->macro_ops, &ctx->macro_ops_len, &ctx->macro_ops_cap, s);
  apply_macro_op(ctx, s);
}

void chibicc_context_set_predefined_macros(ChibiccContext *ctx, bool enabled) {
  if (ctx)
    ctx->predefined_macros = enabled;
}

void chibicc_context_set_diagnostic(ChibiccContext *ctx,
                                    ChibiccDiagnosticFn fn, void *user) {
  ctx->diag_fn = fn;
  ctx->diag_user = user;
}

bool chibicc_context_failed(ChibiccContext *ctx) {
  return ctx->failed;
}

void chibicc_context_activate(ChibiccContext *ctx) {
  chibicc_active_ctx = ctx;
}

static void free_arena(ChibiccContext *ctx) {
  for (ArenaBlock *b = ctx->arena; b;) {
    ArenaBlock *next = b->next;
    free(b->data);
    free(b);
    b = next;
  }
  ctx->arena = NULL;
}

static void clear_state(ChibiccContext *ctx) {
  ctx->base_file = NULL;
  ctx->current_file = NULL;
  ctx->input_files = NULL;
  ctx->input_files_len = 0;
  ctx->input_files_cap = 0;
  ctx->next_file_no = 0;
  ctx->at_bol = false;
  ctx->has_space = false;
  ctx->macros = (HashMap){};
  ctx->predefined = NULL;
  ctx->predefined_len = 0;
  ctx->cond_incl = NULL;
  ctx->pragma_once = (HashMap){};
  ctx->search_cache = (HashMap){};
  ctx->include_guards = (HashMap){};
  ctx->keyword_map = (HashMap){};
  ctx->typename_map = (HashMap){};
  ctx->counter = 0;
  ctx->include_next_idx = 0;
  ctx->include_count = 0;
  ctx->parse_depth = 0;
  ctx->scope = NULL;
  ctx->sema_scope = NULL;
  ctx->locals = NULL;
  ctx->globals = NULL;
  ctx->sema_fn = NULL;
  ctx->builtin_alloca = NULL;
  ctx->gotos = (NodeArray){};
  ctx->labels = (NodeArray){};
  ctx->label_gotos = NULL;
  ctx->loop_depth = 0;
  ctx->switch_depth = 0;
  ctx->unique_name_id = 0;
  ctx->resolving_body = false;
  ctx->failed = false;
  ctx->jmp_top = NULL;
}

ChibiccContext *chibicc_context_new(void) {
  ChibiccContext *ctx = calloc(1, sizeof(ChibiccContext));
  ctx->predefined_macros = true;
  chibicc_active_ctx = ctx;
  init_macros();
  return ctx;
}

void chibicc_context_reset(ChibiccContext *ctx) {
  free_arena(ctx);
  clear_state(ctx);
  chibicc_active_ctx = ctx;
  init_macros();
  for (int i = 0; i < ctx->macro_ops_len; i++)
    apply_macro_op(ctx, ctx->macro_ops[i]);
}

void chibicc_context_free(ChibiccContext *ctx) {
  if (!ctx)
    return;
  free_arena(ctx);
  for (int i = 0; i < ctx->include_paths_len; i++)
    free(ctx->include_paths[i]);
  free(ctx->include_paths);
  for (int i = 0; i < ctx->macro_ops_len; i++)
    free(ctx->macro_ops[i]);
  free(ctx->macro_ops);
  if (chibicc_active_ctx == ctx)
    chibicc_active_ctx = NULL;
  free(ctx);
}

Token *chibicc_tokenize_file(ChibiccContext *ctx, char *path) {
  ENTER_RET(ctx, NULL);
  if (!ctx->base_file)
    ctx->base_file = chibicc_arena_strdup(path);
  Token *tok = tokenize_file(path);
  // tokenize_file returns NULL only when the file cannot be opened; the
  // facades report it, so a library consumer never sees a silent NULL
  // (PLAN D4/L3.4b).
  if (!tok)
    error("cannot open %s: %s", path, strerror(errno));
  LEAVE(ctx);
  return tok;
}

Token *chibicc_tokenize_buffer_len(ChibiccContext *ctx, char *name,
                                   char *contents, int len) {
  ENTER_RET(ctx, NULL);
  if (len < 0)
    error("invalid buffer length");
  if (!ctx->base_file)
    ctx->base_file = chibicc_arena_strdup(name);

  char *copy = chibicc_arena_alloc((size_t)len + 1, 1);
  memcpy(copy, contents, len);
  copy[len] = '\0';

  File *file = new_file_len(chibicc_arena_strdup(name), 0, copy, len);
  char *nul = memchr(copy, 0, len);
  if (nul) {
    ctx->current_file = file;
    error_at(nul, "NUL byte in source file");
  }

  Token *tok = tokenize(file);
  LEAVE(ctx);
  return tok;
}

Token *chibicc_tokenize_buffer(ChibiccContext *ctx, char *name, char *contents) {
  return chibicc_tokenize_buffer_len(ctx, name, contents, (int)strlen(contents));
}

void chibicc_reconstruct(ChibiccContext *ctx, Token *tok, FILE *out) {
  ENTER_VOID(ctx);
  for (; tok; tok = tok->next) {
    fwrite(tok->trivia, 1, tok->trivia_len, out);
    fwrite(tok->raw_loc, 1, tok->raw_len, out);
  }
  LEAVE(ctx);
}

Token *chibicc_preprocess(ChibiccContext *ctx, Token *tok) {
  ENTER_RET(ctx, NULL);
  Token *r = preprocess(tok);
  LEAVE(ctx);
  return r;
}

Node *chibicc_parse(ChibiccContext *ctx, Token *tok) {
  ENTER_RET(ctx, NULL);
  Node *r = parse(tok);
  LEAVE(ctx);
  return r;
}

Obj *chibicc_sema(ChibiccContext *ctx, Node *toplevel) {
  ENTER_RET(ctx, NULL);
  Obj *r = sema(toplevel);
  LEAVE(ctx);
  return r;
}

CstNode *chibicc_cst_parse(ChibiccContext *ctx, Token *tok) {
  ENTER_RET(ctx, NULL);
  CstNode *r = cst_parse(tok);
  LEAVE(ctx);
  return r;
}

void chibicc_cst_reconstruct(ChibiccContext *ctx, CstNode *root, FILE *out) {
  ENTER_VOID(ctx);
  cst_reconstruct(root, out);
  LEAVE(ctx);
}

void chibicc_cst_dump(ChibiccContext *ctx, CstNode *root, FILE *out) {
  ENTER_VOID(ctx);
  cst_dump(root, out);
  LEAVE(ctx);
}

bool chibicc_cst_verify(ChibiccContext *ctx, CstNode *root, Token *tok) {
  ENTER_RET(ctx, false);
  bool r = cst_verify(root, tok);
  LEAVE(ctx);
  return r;
}

void chibicc_expansion_dump(ChibiccContext *ctx, Token *tok, FILE *out) {
  ENTER_VOID(ctx);
  expansion_dump(tok, out);
  LEAVE(ctx);
}

bool chibicc_expansion_verify(ChibiccContext *ctx, Token *tok) {
  ENTER_RET(ctx, false);
  bool r = expansion_verify(tok);
  LEAVE(ctx);
  return r;
}

// The provenance accessors (PLAN D7). They read immutable records and
// allocate nothing, so they need no error boundary.
ChibiccMacroProv *chibicc_token_provenance(ChibiccToken *tok) {
  return token_provenance(tok);
}

ChibiccMacroProv *chibicc_macro_prov_next(ChibiccMacroProv *p) {
  return macro_prov_next(p);
}

ChibiccMacroExpansion *chibicc_macro_prov_expansion(ChibiccMacroProv *p) {
  return macro_prov_expansion(p);
}

int chibicc_macro_prov_arg_index(ChibiccMacroProv *p) {
  return macro_prov_arg_index(p);
}

ChibiccToken *chibicc_macro_expansion_name(ChibiccMacroExpansion *e) {
  return macro_expansion_name(e);
}

ChibiccToken *chibicc_macro_expansion_end(ChibiccMacroExpansion *e) {
  return macro_expansion_end(e);
}

char *chibicc_macro_expansion_macro(ChibiccMacroExpansion *e) {
  return macro_expansion_macro(e);
}

ChibiccToken *chibicc_macro_expansion_def(ChibiccMacroExpansion *e) {
  return macro_expansion_def(e);
}

ChibiccToken *chibicc_macro_expansion_arg(ChibiccMacroExpansion *e, int i) {
  return macro_expansion_arg(e, i);
}

int chibicc_macro_expansion_arg_count(ChibiccMacroExpansion *e) {
  return macro_expansion_arg_count(e);
}

bool chibicc_macro_expansion_is_builtin(ChibiccMacroExpansion *e) {
  return macro_expansion_is_builtin(e);
}

