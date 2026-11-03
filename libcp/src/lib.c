// The library facade (PLAN L2): the context object, the arena, and the
// public entry points. The phases themselves stay in their own files;
// everything here is the boundary the library promises.
//
// 库的 facade (PLAN L2): context 对象, arena 以及公共入口. 各阶段实现
// 留在各自的文件里; 这里是库对外承诺的边界. 主要内容: context 的
// new / free / reset 三态, arena 分配与字符串复制, 诊断边界与输入入口.

#include "libcp.h"

// Every public entry is an error boundary: an error_* longjmps to the
// innermost frame, and the entry returns its failure value. The frame
// belongs to the context in hand; no other state is saved or restored.
// The ENTER/LEAVE macros live in libcp.h; the merged entries (the phase
// functions that carry their public name directly) use them from their
// own translation units.
//
// 每个公共入口都是一个错误边界: error_* 会 longjmp 到最内层 frame,
// 入口随后返回自己的失败值. frame 属于当前 context; 不保存或恢复
// 任何其他状态. ENTER/LEAVE 宏定义在 libcp.h 中; 直接携带公共名的
// 合并入口 (各阶段函数) 在各自的翻译单元里使用它们.

const char* libcp_version(void) {
  return LIBCP_VERSION;
}

// arena 常规块的容量: 64 KiB. 请求比它更大时按请求尺寸放大.
#define ARENA_BLOCK_SIZE (64 * 1024)

#ifdef LIBCP_DEBUG_ARENA
// One allocation per object for the sanitizer gate: the bump arena hides
// intra-block overflows from AddressSanitizer's redzones. test/library.sh
// enables this in its ASan round.
//
// 为 sanitizer 闸门改为每次分配一个对象: bump arena 会让块内溢出
// 落在 AddressSanitizer 红区之外, 从而被漏掉. test/library.sh 的
// ASan 轮次会打开这个开关.
static void* arena_alloc(LibcpContext* ctx, size_t size, size_t align) {
  (void)align;
  assert(ctx);
  if (size == 0)
    size = 1;

  LibcpArenaBlock* b = LIBCP_CALLOC(1, sizeof(LibcpArenaBlock));
  if (!b)
    return NULL;
  b->data = LIBCP_MALLOC(size);
  if (!b->data) {
    LIBCP_FREE(b);
    return NULL;
  }
  b->cap     = size;
  b->used    = size;
  b->next    = ctx->arena;
  ctx->arena = b;
  return b->data;
}
#else
// 默认实现: bump 分配器. 在最新块内把起点向上对齐到 align 后向前
// 推进; 放不下就挂一个新块, 旧块保留到 reset, 不做回收.
static void* arena_alloc(LibcpContext* ctx, size_t size, size_t align) {
  assert(ctx);
  if (size == 0)
    size = 1;
  if (align < 8)
    align = 8;

  LibcpArenaBlock* b = ctx->arena;
  size_t start       = b ? (b->used + align - 1) / align * align : 0;
  // Overflow-safe form of start + size > b->cap. start may round past
  // cap (used can sit on cap with a custom-sized block), and cap - start
  // would then wrap, so that case is decided first.
  //
  // start + size > b->cap 的防溢出写法. start 可能对齐到 cap 之后
  // (自定义大小的块能让 used 正好落在 cap 上), 此时 cap - start 会
  // 回绕, 所以先单独判断这种情况.
  if (!b || start > b->cap || size > b->cap - start) {
    size_t cap = ARENA_BLOCK_SIZE;
    if (size > SIZE_MAX - align || cap < size + align)
      cap = size + align;
    LibcpArenaBlock* nb = LIBCP_CALLOC(1, sizeof(LibcpArenaBlock));
    nb->data            = LIBCP_MALLOC(cap);
    nb->cap             = cap;
    nb->next            = b;
    ctx->arena          = nb;
    b                   = nb;
    start               = 0;
  }

  void* p = b->data + start;
  b->used = start + size;
  return p;
}
#endif

void* libcp_arena_alloc(LibcpContext* ctx, size_t size, size_t align) {
  return arena_alloc(ctx, size, align);
}

void* libcp_arena_calloc(LibcpContext* ctx, size_t count, size_t size) {
  // The arena path multiplies by hand; fail instead of wrapping to a
  // small allocation that memset would then overflow.
  //
  // arena 路径自行做乘法; 溢出时返回失败, 而不是回绕成一个小块再让
  // 随后的 memset 越界.
  if (size && count > SIZE_MAX / size)
    return NULL;
  size_t n = count * size;
  // 16, not 8: LibcpToken and LibcpNode embed a long double, which needs 16-byte
  // alignment on x86-64 (UBSan found this in L3.4c).
  //
  // 用 16 而不是 8: LibcpToken 与 LibcpNode 内嵌 long double, 在 x86-64
  // 上需要 16 字节对齐 (UBSan 在 L3.4c 发现了这一点).
  void* p  = arena_alloc(ctx, n, 16);
  memset(p, 0, n);
  return p;
}

char* libcp_arena_strdup(LibcpContext* ctx, char* s) {
  if (!s)
    return NULL;
  size_t n = strlen(s) + 1;
  char* p  = arena_alloc(ctx, n, 1);
  memcpy(p, s, n);
  return p;
}

char* libcp_arena_strndup(LibcpContext* ctx, char* s, size_t n) {
  size_t len = strnlen(s, n);
  char* p    = arena_alloc(ctx, len + 1, 1);
  memcpy(p, s, len);
  p[len] = '\0';
  return p;
}

// Reallocate in the arena. The old block is not reclaimed - the arena
// frees only when the context is reset - so a geometric growth keeps
// the dead capacity alive and peaks at about twice the final size; that
// is the arena trade-off, not a leak. Every byte past `old_size` in the
// returned block is zeroed, so a caller may rely on a clean tail (C9).
//
// 在 arena 中重新分配. 旧块不会被回收 - arena 只在 context reset 时
// 释放 - 所以几何式增长会让已死的容量继续存活, 峰值约为最终大小的
// 两倍; 这是 arena 的取舍, 不是泄漏. 返回块中超过 `old_size` 的每个
// 字节都被清零, 因此调用方可以依赖干净的尾部 (C9).
void* libcp_arena_realloc(LibcpContext* ctx, void* old, size_t old_size, size_t new_size) {
  size_t kept = old ? (old_size < new_size ? old_size : new_size) : 0;

  void* p = arena_alloc(ctx, new_size, 8);
  if (kept)
    memcpy(p, old, kept);
  if (new_size > kept)
    memset((char*)p + kept, 0, new_size - kept);
  return p;
}

// strdup would take the C library's allocator, but the context frees
// these blocks through LIBCP_FREE; a replaced allocator must own them.
//
// strdup 会使用 C 库的分配器, 但 context 通过 LIBCP_FREE 释放这些块;
// 被替换的分配器必须拥有它们, 否则 free 与分配会对不上.
static char* config_strdup(char* s) {
  size_t n = strlen(s) + 1;
  char* p  = LIBCP_MALLOC(n);
  if (p)
    memcpy(p, s, n);
  return p;
}

// 向配置用的字符串数组追加一项并保持 NULL 结尾. 容量从 8 起翻倍;
// 底层用 LIBCP_REALLOC, 因为这些配置跨 reset 保留, 由 context_free 释放.
static void config_push(char*** arr, int* len, int* cap, char* s) {
  if (*len + 1 >= *cap) {
    int ncap = *cap ? *cap * 2 : 8;
    char** n = LIBCP_REALLOC(*arr, sizeof(char*) * ncap);
    // On OOM keep the old array and capacity; the caller's string is
    // not stored. A void configuration setter has no report path.
    //
    // 内存不足时保留旧数组与旧容量, 调用方的字符串不会被存下.
    // 这类 void 配置 setter 没有上报路径.
    if (!n)
      return;
    *arr = n;
    *cap = ncap;
  }
  (*arr)[(*len)++] = s;
  (*arr)[*len]     = NULL;
}

// 追加一个 #include 搜索目录; 参数字符串被复制, 调用方的缓冲区可以
// 立刻释放. 该配置跨 reset 保留.
void libcp_context_add_include_path(LibcpContext* ctx, char* path) {
  config_push(&ctx->include_paths,
              &ctx->include_paths_len,
              &ctx->include_paths_cap,
              config_strdup(path));
}

// PLAN-l5 W2: a typedef name provided by an included header is invisible
// to the CST's pre-preprocess scan; the consumer, which knows its target
// headers, seeds those names instead of the library guessing.
//
// PLAN-l5 W2: 由被包含头文件提供的 typedef 名对 CST 的预处理前扫描
// 不可见; 消费者知道自己的目标头文件, 因此由它预置这些名字, 而不是
// 让库去猜.
void libcp_cst_seed_typedef(LibcpContext* ctx, char* name) {
  config_push(&ctx->cst_seeds, &ctx->cst_seeds_len, &ctx->cst_seeds_cap, config_strdup(name));
}

// 设置基础文件名, 供 __BASE_FILE__ 内建宏使用. 字符串复制进 arena,
// 随下一次 reset 失效; 未显式设置时, 第一个被词法分析的输入文件充当它.
void libcp_context_set_base_file(LibcpContext* ctx, char* path) {
  ctx->base_file = libcp_arena_strdup(ctx, path);
}

// 解析并执行一条记录形式的宏操作: "D:name=body" 定义宏, "U:name"
// 取消定义. 定义分支会临时把 '=' 改成 NUL 再改回来, 因此传入的字符串
// 必须可写 (调用方总是传 LIBCP_CALLOC 出来的副本).
static void apply_macro_op(LibcpContext* ctx, char* op) {
  if (op[0] == 'D') {
    char* name = op + 2;
    char* eq   = strchr(name, '=');
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

void libcp_context_define(LibcpContext* ctx, char* name, char* buf) {
  size_t n = strlen(name) + strlen(buf) + 4;
  char* s  = LIBCP_CALLOC(1, n);
  snprintf(s, n, "D:%s=%s", name, buf);

  // Its own error boundary: a malformed body raises through
  // tokenize_nested instead of exiting the host process. The frame is
  // hand-written (not ENTER_VOID) so the op string is freed on the way
  // out, and the op is applied before it is recorded because
  // apply_macro_op leaves its '=' NULed when it raises - a half-applied
  // op must not reach the reset replay.
  //
  // 自成一体的错误边界: 非法的宏体会经 tokenize_nested 抛出, 而不是让
  // 宿主进程退出. 这个 frame 是手写的 (不用 ENTER_VOID), 以便返回时
  // 释放 op 字符串; 并且先执行 op 再记录它, 因为 apply_macro_op 抛出时
  // 会把其中的 '=' 留成 NUL - 半执行的 op 不能进入 reset 回放.
  LibcpJmpFrame jf;
  jf.prev      = ctx->jmp_top;
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

// 记录一条 undef 配置并立即执行它; 记录下来的操作会在每次 reset 之后
// 连同 -D 一起按顺序回放.
void libcp_context_undef(LibcpContext* ctx, char* name) {
  size_t n = strlen(name) + 3;
  char* s  = LIBCP_CALLOC(1, n);
  snprintf(s, n, "U:%s", name);
  ENTER_VOID(ctx);
  config_push(&ctx->macro_ops, &ctx->macro_ops_len, &ctx->macro_ops_cap, s);
  apply_macro_op(ctx, s);
  LEAVE(ctx);
}

void libcp_context_set_predefined_macros(LibcpContext* ctx, bool enabled) {
  if (ctx)
    ctx->predefined_macros = enabled;
}

void libcp_context_set_diagnostic(LibcpContext* ctx, LibcpDiagnosticFn fn, void* user) {
  ctx->diag_fn   = fn;
  ctx->diag_user = user;
}

bool libcp_context_failed(LibcpContext* ctx) {
  return ctx->failed;
}

// 释放 arena 的全部块: 每块的 data 与块头都走 LIBCP_FREE, 之后
// ctx->arena 归为 NULL.
static void free_arena(LibcpContext* ctx) {
  for (LibcpArenaBlock* b = ctx->arena; b;) {
    LibcpArenaBlock* next = b->next;
    LIBCP_FREE(b->data);
    LIBCP_FREE(b);
    b = next;
  }
  ctx->arena = NULL;
}

// 清空 per-TU 状态, 只保留配置字段 (include 路径, 宏操作, CST 预置的
// typedef 名, 诊断回调). 必须在 free_arena 之后调用: 这里丢弃的指针
// 都指向刚释放的 arena.
static void clear_state(LibcpContext* ctx) {
  ctx->base_file        = NULL;
  ctx->current_file     = NULL;
  ctx->input_files      = NULL;
  ctx->input_files_len  = 0;
  ctx->input_files_cap  = 0;
  ctx->next_file_no     = 0;
  ctx->stdin_text       = NULL;
  ctx->stdin_len        = 0;
  ctx->at_bol           = false;
  ctx->has_space        = false;
  ctx->macros           = (LibcpHashMap){};
  ctx->predefined       = NULL;
  ctx->predefined_len   = 0;
  ctx->cond_incl        = NULL;
  ctx->pragma_once      = (LibcpHashMap){};
  ctx->search_cache     = (LibcpHashMap){};
  ctx->include_guards   = (LibcpHashMap){};
  ctx->keyword_map      = (LibcpHashMap){};
  ctx->typename_map     = (LibcpHashMap){};
  ctx->counter          = 0;
  ctx->include_next_idx = 0;
  ctx->include_count    = 0;
  ctx->parse_depth      = 0;
  ctx->scope            = NULL;
  libcp_sema_state_clear(ctx);
  ctx->failed  = false;
  ctx->jmp_top = NULL;
}

// 创建 context: 全部字段零初始化, 默认安装预定义宏集合, 尚未绑定任何
// 输入文件.
LibcpContext* libcp_context_new(void) {
  LibcpContext* ctx      = LIBCP_CALLOC(1, sizeof(LibcpContext));
  ctx->predefined_macros = true;
  libcp_init_macros(ctx);
  return ctx;
}

// 三态中的 reset: 释放 per-TU 内存, 清空状态, 重装预定义宏, 再按记录
// 顺序回放 -D/-U. 配置 (include 路径等) 全部保留, 可以接着处理下一个
// 翻译单元.
void libcp_context_reset(LibcpContext* ctx) {
  free_arena(ctx);
  clear_state(ctx);
  libcp_init_macros(ctx);
  for (int i = 0; i < ctx->macro_ops_len; i++)
    apply_macro_op(ctx, ctx->macro_ops[i]);
}

// 销毁 context: 先释放 arena, 再逐个释放配置数组 (它们由 malloc 拥有),
// 最后释放 context 本身. 不做逐对象 free.
void libcp_context_free(LibcpContext* ctx) {
  if (!ctx)
    return;
  free_arena(ctx);
  for (int i = 0; i < ctx->include_paths_len; i++)
    LIBCP_FREE(ctx->include_paths[i]);
  LIBCP_FREE(ctx->include_paths);
  for (int i = 0; i < ctx->macro_ops_len; i++)
    LIBCP_FREE(ctx->macro_ops[i]);
  LIBCP_FREE(ctx->macro_ops);
  for (int i = 0; i < ctx->cst_seeds_len; i++)
    LIBCP_FREE(ctx->cst_seeds[i]);
  LIBCP_FREE(ctx->cst_seeds);
  LIBCP_FREE(ctx);
}

// 对文件做词法分析, 返回第 1 层 token 流. tokenize 入口都在错误边界
// 内运行, 输入有问题会走诊断, 不会静默返回 NULL.
LibcpToken* libcp_tokenize_file(LibcpContext* ctx, char* path) {
  ENTER_RET(ctx, NULL);
  if (!ctx->base_file)
    ctx->base_file = libcp_arena_strdup(ctx, path);
  LibcpToken* tok = libcp_tokenize_file_impl(ctx, path);
  // libcp_tokenize_file_impl returns NULL only when the file cannot be opened; the
  // facades report it, so a library consumer never sees a silent NULL
  // (PLAN D4/L3.4b).
  //
  // libcp_tokenize_file_impl 只在文件无法打开时返回 NULL; facade 在这里
  // 上报它, 因此库的消费者不会遇到静默的 NULL (PLAN D4/L3.4b).
  if (!tok)
    libcp_error(ctx, "cannot open %s: %s", path, strerror(errno));
  LEAVE(ctx);
  return tok;
}

// 词法分析一段内存缓冲区. 文本被复制进 arena 并补上 NUL 结尾, 因此
// 调用方的缓冲区之后可以释放.
LibcpToken* libcp_tokenize_buffer_len(LibcpContext* ctx, char* name, char* contents, int len) {
  ENTER_RET(ctx, NULL);
  if (len < 0)
    libcp_error(ctx, "invalid buffer length");
  if (!ctx->base_file)
    ctx->base_file = libcp_arena_strdup(ctx, name);

  char* copy = libcp_arena_alloc(ctx, (size_t)len + 1, 1);
  memcpy(copy, contents, len);
  copy[len] = '\0';

  LibcpFile* file = libcp_new_file_len(ctx, libcp_arena_strdup(ctx, name), 0, copy, len);
  char* nul       = memchr(copy, 0, len);
  // 内嵌 NUL 视为非法输入, 而不是静默截断; 先设置 current_file 让诊断
  // 能带上文件名与行号.
  if (nul) {
    ctx->current_file = file;
    libcp_error_at(ctx, nul, "NUL byte in source file");
  }

  LibcpToken* tok = libcp_tokenize(ctx, file);
  LEAVE(ctx);
  return tok;
}

LibcpToken* libcp_tokenize_buffer(LibcpContext* ctx, char* name, char* contents) {
  return libcp_tokenize_buffer_len(ctx, name, contents, (int)strlen(contents));
}

// 把 token 流写回原始文本: 逐个 token 先写 trivia 再写拼写, 因此
// 对 tokenize 的直接输出可以逐字节重建输入文件 (见 token.h).
void libcp_reconstruct(LibcpContext* ctx, LibcpToken* tok, FILE* out) {
  ENTER_VOID(ctx);
  for (; tok; tok = tok->next) {
    fwrite(tok->trivia, 1, tok->trivia_len, out);
    fwrite(tok->raw_loc, 1, tok->raw_len, out);
  }
  LEAVE(ctx);
}
