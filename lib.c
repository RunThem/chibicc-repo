// The library facade (PLAN L2): the context object, the arena, and the
// public entry points. The phases themselves stay in their own files;
// everything here is the boundary the library promises.

#include "chibicc.h"

_Thread_local ChibiccContext *chibicc_active_ctx;

ChibiccContext *chibicc_ctx(void) {
  return chibicc_active_ctx;
}

struct ArenaBlock {
  ArenaBlock *next;
  size_t used;
  size_t cap;
  char *data;
};

#define ARENA_BLOCK_SIZE (64 * 1024)

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

void *chibicc_arena_alloc(size_t size, size_t align) {
  return arena_alloc(chibicc_active_ctx, size, align);
}

void *chibicc_arena_calloc(size_t count, size_t size) {
  size_t n = count * size;
  if (!chibicc_active_ctx)
    return calloc(count, size);
  void *p = arena_alloc(chibicc_active_ctx, n, 8);
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

void chibicc_context_set_diagnostic(ChibiccContext *ctx,
                                    ChibiccDiagnosticFn fn, void *user) {
  ctx->diag_fn = fn;
  ctx->diag_user = user;
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
  ctx->current_file = NULL;
  ctx->input_files = NULL;
  ctx->input_files_len = 0;
  ctx->input_files_cap = 0;
  ctx->at_bol = false;
  ctx->has_space = false;
  ctx->macros = (HashMap){};
  ctx->cond_incl = NULL;
  ctx->pragma_once = (HashMap){};
  ctx->search_cache = (HashMap){};
  ctx->include_next_idx = 0;
  ctx->scope = NULL;
  ctx->locals = NULL;
  ctx->globals = NULL;
  ctx->sema_fn = NULL;
  ctx->builtin_alloca = NULL;
  ctx->gotos = (NodeArray){};
  ctx->labels = (NodeArray){};
  ctx->loop_depth = 0;
  ctx->switch_depth = 0;
  ctx->unique_name_id = 0;
  ctx->resolving_body = false;
  ctx->failed = false;
  ctx->jmp_top = NULL;
}

ChibiccContext *chibicc_context_new(void) {
  ChibiccContext *ctx = calloc(1, sizeof(ChibiccContext));
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
  chibicc_active_ctx = ctx;
  return tokenize_file(path);
}

Token *chibicc_tokenize_buffer(ChibiccContext *ctx, char *name, char *contents) {
  chibicc_active_ctx = ctx;
  char *copy = chibicc_arena_strdup(contents);
  File *file = new_file(chibicc_arena_strdup(name), 0, copy);
  return tokenize(file);
}

Token *chibicc_preprocess(ChibiccContext *ctx, Token *tok) {
  chibicc_active_ctx = ctx;
  return preprocess(tok);
}

Node *chibicc_parse(ChibiccContext *ctx, Token *tok) {
  chibicc_active_ctx = ctx;
  return parse(tok);
}

Obj *chibicc_sema(ChibiccContext *ctx, Node *toplevel) {
  chibicc_active_ctx = ctx;
  return sema(toplevel);
}

CstNode *chibicc_cst_parse(ChibiccContext *ctx, Token *tok) {
  chibicc_active_ctx = ctx;
  return cst_parse(tok);
}

void chibicc_cst_reconstruct(ChibiccContext *ctx, CstNode *root, FILE *out) {
  chibicc_active_ctx = ctx;
  cst_reconstruct(root, out);
}

void chibicc_cst_dump(ChibiccContext *ctx, CstNode *root, FILE *out) {
  chibicc_active_ctx = ctx;
  cst_dump(root, out);
}

bool chibicc_cst_verify(ChibiccContext *ctx, CstNode *root, Token *tok) {
  chibicc_active_ctx = ctx;
  return cst_verify(root, tok);
}

void chibicc_expansion_dump(ChibiccContext *ctx, Token *tok, FILE *out) {
  chibicc_active_ctx = ctx;
  expansion_dump(tok, out);
}

bool chibicc_expansion_verify(ChibiccContext *ctx, Token *tok) {
  chibicc_active_ctx = ctx;
  return expansion_verify(tok);
}
