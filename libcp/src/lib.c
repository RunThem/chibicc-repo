// The library facade (PLAN L2): the context object, the arena, and the
// public entry points. The phases themselves stay in their own files;
// everything here is the boundary the library promises.

#include "libcp.h"

// Every public entry is an error boundary: an error_* longjmps to the
// innermost frame, and the entry returns its failure value. The frame
// belongs to the context in hand; no other state is saved or restored.
// The ENTER/LEAVE macros live in libcp.h; the merged entries (the phase
// functions that carry their public name directly) use them from their
// own translation units.

const char *libcp_version(void) {
  return LIBCP_VERSION;
}

#define ARENA_BLOCK_SIZE (64 * 1024)

#ifdef LIBCP_DEBUG_ARENA
// One allocation per object for the sanitizer gate: the bump arena hides
// intra-block overflows from AddressSanitizer's redzones. test/library.sh
// enables this in its ASan round.
static void *arena_alloc(LibcpContext *ctx, size_t size, size_t align) {
  (void)align;
  assert(ctx);
  if (size == 0)
    size = 1;

  ArenaBlock *b = LIBCP_CALLOC(1, sizeof(ArenaBlock));
  if (!b)
    return NULL;
  b->data = LIBCP_MALLOC(size);
  if (!b->data) {
    LIBCP_FREE(b);
    return NULL;
  }
  b->cap = size;
  b->used = size;
  b->next = ctx->arena;
  ctx->arena = b;
  return b->data;
}
#else
static void *arena_alloc(LibcpContext *ctx, size_t size, size_t align) {
  assert(ctx);
  if (size == 0)
    size = 1;
  if (align < 8)
    align = 8;

  ArenaBlock *b = ctx->arena;
  size_t start = b ? (b->used + align - 1) / align * align : 0;
  // Overflow-safe form of start + size > b->cap. start may round past
  // cap (used can sit on cap with a custom-sized block), and cap - start
  // would then wrap, so that case is decided first.
  if (!b || start > b->cap || size > b->cap - start) {
    size_t cap = ARENA_BLOCK_SIZE;
    if (size > SIZE_MAX - align || cap < size + align)
      cap = size + align;
    ArenaBlock *nb = LIBCP_CALLOC(1, sizeof(ArenaBlock));
    nb->data = LIBCP_MALLOC(cap);
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

void *libcp_arena_alloc(LibcpContext *ctx, size_t size, size_t align) {
  return arena_alloc(ctx, size, align);
}

void *libcp_arena_calloc(LibcpContext *ctx, size_t count, size_t size) {
  // The arena path multiplies by hand; fail instead of wrapping to a
  // small allocation that memset would then overflow.
  if (size && count > SIZE_MAX / size)
    return NULL;
  size_t n = count * size;
  // 16, not 8: Token and Node embed a long double, which needs 16-byte
  // alignment on x86-64 (UBSan found this in L3.4c).
  void *p = arena_alloc(ctx, n, 16);
  memset(p, 0, n);
  return p;
}

char *libcp_arena_strdup(LibcpContext *ctx, char *s) {
  if (!s)
    return NULL;
  size_t n = strlen(s) + 1;
  char *p = arena_alloc(ctx, n, 1);
  memcpy(p, s, n);
  return p;
}

char *libcp_arena_strndup(LibcpContext *ctx, char *s, size_t n) {
  size_t len = strnlen(s, n);
  char *p = arena_alloc(ctx, len + 1, 1);
  memcpy(p, s, len);
  p[len] = '\0';
  return p;
}

// Reallocate in the arena. The old block is not reclaimed - the arena
// frees only when the context is reset - so a geometric growth keeps
// the dead capacity alive and peaks at about twice the final size; that
// is the arena trade-off, not a leak. Every byte past `old_size` in the
// returned block is zeroed, so a caller may rely on a clean tail (C9).
void *libcp_arena_realloc(LibcpContext *ctx, void *old, size_t old_size, size_t new_size) {
  size_t kept = old ? (old_size < new_size ? old_size : new_size) : 0;

  void *p = arena_alloc(ctx, new_size, 8);
  if (kept)
    memcpy(p, old, kept);
  if (new_size > kept)
    memset((char *)p + kept, 0, new_size - kept);
  return p;
}

// strdup would take the C library's allocator, but the context frees
// these blocks through LIBCP_FREE; a replaced allocator must own them.
static char *config_strdup(char *s) {
  size_t n = strlen(s) + 1;
  char *p = LIBCP_MALLOC(n);
  if (p)
    memcpy(p, s, n);
  return p;
}

static void config_push(char ***arr, int *len, int *cap, char *s) {
  if (*len + 1 >= *cap) {
    int ncap = *cap ? *cap * 2 : 8;
    char **n = LIBCP_REALLOC(*arr, sizeof(char *) * ncap);
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

void libcp_context_add_include_path(LibcpContext *ctx, char *path) {
  config_push(&ctx->include_paths, &ctx->include_paths_len,
              &ctx->include_paths_cap, config_strdup(path));
}

void libcp_context_set_base_file(LibcpContext *ctx, char *path) {
  ctx->base_file = libcp_arena_strdup(ctx, path);
}

static void apply_macro_op(LibcpContext *ctx, char *op) {
  if (op[0] == 'D') {
    char *name = op + 2;
    char *eq = strchr(name, '=');
    if (eq) {
      *eq = '\0';
      libcp_define_macro(ctx, name, eq + 1);
      *eq = '=';
    } else {
      libcp_define_macro(ctx, name, "1");
    }
  } else {
    libcp_undef_macro(ctx, op + 2);
  }
}

void libcp_context_define(LibcpContext *ctx, char *name, char *buf) {
  size_t n = strlen(name) + strlen(buf) + 4;
  char *s = LIBCP_CALLOC(1, n);
  snprintf(s, n, "D:%s=%s", name, buf);

  // Its own error boundary: a malformed body raises through
  // tokenize_nested instead of exiting the host process. The frame is
  // hand-written (not ENTER_VOID) so the op string is freed on the way
  // out, and the op is applied before it is recorded because
  // apply_macro_op leaves its '=' NULed when it raises - a half-applied
  // op must not reach the reset replay.
  JmpFrame jf;
  jf.prev = ctx->jmp_top;
  ctx->jmp_top = &jf;
  if (setjmp(jf.buf)) {
    ctx->jmp_top = jf.prev;
    LIBCP_FREE(s);
    return;
  }

  apply_macro_op(ctx, s);
  config_push(&ctx->macro_ops, &ctx->macro_ops_len, &ctx->macro_ops_cap, s);
  ctx->jmp_top = jf.prev;
}

void libcp_context_undef(LibcpContext *ctx, char *name) {
  size_t n = strlen(name) + 3;
  char *s = LIBCP_CALLOC(1, n);
  snprintf(s, n, "U:%s", name);
  ENTER_VOID(ctx);
  config_push(&ctx->macro_ops, &ctx->macro_ops_len, &ctx->macro_ops_cap, s);
  apply_macro_op(ctx, s);
  LEAVE(ctx);
}

void libcp_context_set_predefined_macros(LibcpContext *ctx, bool enabled) {
  if (ctx)
    ctx->predefined_macros = enabled;
}

void libcp_context_set_diagnostic(LibcpContext *ctx,
                                    LibcpDiagnosticFn fn, void *user) {
  ctx->diag_fn = fn;
  ctx->diag_user = user;
}

bool libcp_context_failed(LibcpContext *ctx) {
  return ctx->failed;
}

static void free_arena(LibcpContext *ctx) {
  for (ArenaBlock *b = ctx->arena; b;) {
    ArenaBlock *next = b->next;
    LIBCP_FREE(b->data);
    LIBCP_FREE(b);
    b = next;
  }
  ctx->arena = NULL;
}

static void clear_state(LibcpContext *ctx) {
  ctx->base_file = NULL;
  ctx->current_file = NULL;
  ctx->input_files = NULL;
  ctx->input_files_len = 0;
  ctx->input_files_cap = 0;
  ctx->next_file_no = 0;
  ctx->stdin_text = NULL;
  ctx->stdin_len = 0;
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
  libcp_sema_state_clear(ctx);
  ctx->failed = false;
  ctx->jmp_top = NULL;
}

LibcpContext *libcp_context_new(void) {
  LibcpContext *ctx = LIBCP_CALLOC(1, sizeof(LibcpContext));
  ctx->predefined_macros = true;
  libcp_init_macros(ctx);
  return ctx;
}

void libcp_context_reset(LibcpContext *ctx) {
  free_arena(ctx);
  clear_state(ctx);
  libcp_init_macros(ctx);
  for (int i = 0; i < ctx->macro_ops_len; i++)
    apply_macro_op(ctx, ctx->macro_ops[i]);
}

void libcp_context_free(LibcpContext *ctx) {
  if (!ctx)
    return;
  free_arena(ctx);
  for (int i = 0; i < ctx->include_paths_len; i++)
    LIBCP_FREE(ctx->include_paths[i]);
  LIBCP_FREE(ctx->include_paths);
  for (int i = 0; i < ctx->macro_ops_len; i++)
    LIBCP_FREE(ctx->macro_ops[i]);
  LIBCP_FREE(ctx->macro_ops);
  LIBCP_FREE(ctx);
}

Token *libcp_tokenize_file(LibcpContext *ctx, char *path) {
  ENTER_RET(ctx, NULL);
  if (!ctx->base_file)
    ctx->base_file = libcp_arena_strdup(ctx, path);
  Token *tok = libcp_tokenize_file_impl(ctx, path);
  // libcp_tokenize_file_impl returns NULL only when the file cannot be opened; the
  // facades report it, so a library consumer never sees a silent NULL
  // (PLAN D4/L3.4b).
  if (!tok)
    libcp_error(ctx, "cannot open %s: %s", path, strerror(errno));
  LEAVE(ctx);
  return tok;
}

Token *libcp_tokenize_buffer_len(LibcpContext *ctx, char *name,
                                   char *contents, int len) {
  ENTER_RET(ctx, NULL);
  if (len < 0)
    libcp_error(ctx, "invalid buffer length");
  if (!ctx->base_file)
    ctx->base_file = libcp_arena_strdup(ctx, name);

  char *copy = libcp_arena_alloc(ctx, (size_t)len + 1, 1);
  memcpy(copy, contents, len);
  copy[len] = '\0';

  File *file = libcp_new_file_len(ctx, libcp_arena_strdup(ctx, name), 0, copy, len);
  char *nul = memchr(copy, 0, len);
  if (nul) {
    ctx->current_file = file;
    libcp_error_at(ctx, nul, "NUL byte in source file");
  }

  Token *tok = libcp_tokenize(ctx, file);
  LEAVE(ctx);
  return tok;
}

Token *libcp_tokenize_buffer(LibcpContext *ctx, char *name, char *contents) {
  return libcp_tokenize_buffer_len(ctx, name, contents, (int)strlen(contents));
}

void libcp_reconstruct(LibcpContext *ctx, Token *tok, FILE *out) {
  ENTER_VOID(ctx);
  for (; tok; tok = tok->next) {
    fwrite(tok->trivia, 1, tok->trivia_len, out);
    fwrite(tok->raw_loc, 1, tok->raw_len, out);
  }
  LEAVE(ctx);
}

