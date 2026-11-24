// 内部 include 模块 (issue #23): include 搜索的单一 owner.
//
// 本文件独占路径表 (path + origin), 搜索顺序, 搜索缓存, 系统目录判定,
// per-file #include_next 续查与 reset / free 语义. preprocess.c 只提交
// include 请求, 不再拼路径或直接查文件; 驱动器只按 origin 注册路径, 并
// 用 public 的系统目录判定做 -MMD 分类.
//
// 搜索顺序固定为 USER -> AFTER -> SYSTEM, 桶内保持插入顺序. 路径表按
// 桶连续存放, 所以数组本身就是搜索顺序. 这与 GCC 不同: GCC 把
// -idirafter 排在系统目录之后, 本库排在系统目录之前.
//
// 路径表是配置, 跨 reset 保留; 搜索缓存与 per-file 续查表是 per-TU
// 状态, 随 reset 清空. 全部状态都在 LibcpContext 内, 没有静态全局.
//
// #include_next 的续查位置按文件记录 (issue #24): 解析命中时把命中下标
// 记在目标文件上, 续查从该文件自己的下标 + 1 开始. 不是从路径表命中的
// 文件 (引号形式命中当前目录, 绝对路径, 主文件) 记为 -1, 续查从 0 开始,
// 等价于普通 #include. 搜索缓存按 (filename, start_index) 键控, 因此
// 缓存命中不会把续查位置换回别的起点.
#include "libcp.h"

// origin 的搜索桶序号: USER 最前, AFTER 次之, SYSTEM 最后. 未知值按
// SYSTEM 处理, 这样错误的来源不会意外获得更高优先级.
static int origin_rank(LibcpIncludeOrigin origin) {
  switch (origin) {
    case LIBCP_INCLUDE_USER: return 0;
    case LIBCP_INCLUDE_AFTER: return 1;
    case LIBCP_INCLUDE_SYSTEM:
    default: return 2;
  }
}

// path 是否位于目录 dir 之下. 只做字符串前缀匹配, 与 -MMD 的分类一致;
// 不做 realpath 或 inode 归一.
static bool path_under(const char* dir, const char* path) {
  size_t len = strlen(dir);
  return strncmp(dir, path, len) == 0 && path[len] == '/';
}

// 把路径复制进配置分配 (LIBCP_MALLOC 拥有, 跨 reset 保留). 内存不足时
// 返回 NULL, 与该 setter 的旧行为一致: 路径不被记录.
static char* config_strdup(char* s) {
  size_t n = strlen(s) + 1;
  char* p  = LIBCP_MALLOC(n);
  if (p)
    memcpy(p, s, n);
  return p;
}

// 丢弃 per-TU 的搜索结果与续查位置. 路径表改变会移动已有命中下标, 所以
// 两者都不能保留; 路径是配置, 应在处理任何翻译单元之前注册完毕.
static void clear_search_state(LibcpContext* ctx) {
  ctx->include_search.cache     = (LibcpHashMap){};
  ctx->include_search.files     = NULL;
  ctx->include_search.files_len = 0;
  ctx->include_search.files_cap = 0;
}

// 按 origin 把路径插入它所属的桶尾. 路径表始终保持按桶排序, 因此
// 直接遍历就是搜索顺序.
void libcp_include_add_path(LibcpContext* ctx, char* path, LibcpIncludeOrigin origin) {
  LibcpIncludeSearch* s = &ctx->include_search;

  if (s->len == s->cap) {
    int cap             = s->cap ? s->cap * 2 : 8;
    LibcpIncludePath* p = LIBCP_REALLOC(s->paths, sizeof(LibcpIncludePath) * cap);
    if (!p)
      return;
    s->paths = p;
    s->cap   = cap;
  }

  char* copy = config_strdup(path);
  if (!copy)
    return;

  int rank = origin_rank(origin);
  int pos  = s->len;
  while (pos > 0 && origin_rank(s->paths[pos - 1].origin) > rank)
    pos--;
  memmove(s->paths + pos + 1, s->paths + pos, sizeof(LibcpIncludePath) * (s->len - pos));
  s->paths[pos].path   = copy;
  s->paths[pos].origin = origin;
  s->len++;

  clear_search_state(ctx);
}

// 搜索结果缓存的键: 文件名与搜索起点一起决定结果, 所以两者都进键.
// 十进制下标不含 '#', 因此 (filename, start) 与键一一对应.
static char* cache_key(LibcpContext* ctx, char* filename, int start) {
  return libcp_format(ctx, "%s#%d", filename, start);
}

// 记录 file 自己的命中下标, 供它内部的 #include_next 续查. 同一路径
// 再次命中时覆盖旧值; 路径表变动会整体清空这张表.
static void record_file_index(LibcpContext* ctx, char* file, int index) {
  LibcpIncludeSearch* s = &ctx->include_search;

  for (int i = 0; i < s->files_len; i++) {
    if (!strcmp(s->files[i].file, file)) {
      s->files[i].index = index;
      return;
    }
  }

  if (s->files_len == s->files_cap) {
    int cap      = s->files_cap ? s->files_cap * 2 : 8;
    s->files     = libcp_tu_realloc(ctx,
                                    s->files,
                                    sizeof(LibcpIncludeFileIndex) * s->files_cap,
                                    sizeof(LibcpIncludeFileIndex) * cap);
    s->files_cap = cap;
  }

  s->files[s->files_len].file  = libcp_tu_strdup(ctx, file);
  s->files[s->files_len].index = index;
  s->files_len++;
}

// file 自己的命中下标; 没有记录 (主文件或未从路径表命中的文件) 返回 -1.
static int file_index(LibcpContext* ctx, char* file) {
  LibcpIncludeSearch* s = &ctx->include_search;
  for (int i = 0; i < s->files_len; i++)
    if (!strcmp(s->files[i].file, file))
      return s->files[i].index;
  return -1;
}

// 从第 start 项起按搜索顺序查找 filename. 命中时填写 hit; 未命中时
// hit->path 为 NULL. 本函数不更新任何续查状态.
static bool search_paths(LibcpContext* ctx, int start, char* filename, LibcpIncludeHit* hit) {
  LibcpIncludeSearch* s = &ctx->include_search;

  if (start < 0)
    start = 0;

  for (int i = start; i < s->len; i++) {
    char* path = libcp_format(ctx, "%s/%s", s->paths[i].path, filename);
    if (!libcp_file_exists(path))
      continue;
    hit->path  = path;
    hit->index = i;
    return true;
  }

  hit->path  = NULL;
  hit->index = -1;
  return false;
}

// 路径表搜索, 命中结果按 (filename, start) 缓存在 per-TU 搜索缓存里.
// 缓存保存命中下标, 因此缓存命中也会把正确的续查位置记到目标文件上.
static LibcpIncludeHit resolve_paths(LibcpContext* ctx, char* filename, int start) {
  LibcpIncludeSearch* s = &ctx->include_search;
  LibcpIncludeHit hit   = {};

  if (start < 0)
    start = 0;

  char* key               = cache_key(ctx, filename, start);
  LibcpIncludeHit* cached = libcp_hashmap_get(ctx, &s->cache, key);
  if (cached) {
    hit = *cached;
  } else if (search_paths(ctx, start, filename, &hit)) {
    LibcpIncludeHit* entry = libcp_tu_alloc(ctx, sizeof(LibcpIncludeHit));
    *entry                 = hit;
    libcp_hashmap_put(ctx, &s->cache, key, entry);
  }

  if (hit.path)
    record_file_index(ctx, hit.path, hit.index);
  return hit;
}

// 绝对路径绕过路径表, 与旧实现一致. 它不是从路径表命中的, 所以自己的
// 续查下标记为 -1.
static LibcpIncludeHit absolute_hit(LibcpContext* ctx, char* filename) {
  LibcpIncludeHit hit = {filename, -1};
  record_file_index(ctx, hit.path, hit.index);
  return hit;
}

// 引号形式: 先查当前文件所在目录, 再走路径表. 当前目录命中不是从路径表
// 命中的, 续查下标记为 -1.
LibcpIncludeHit
    libcp_include_resolve_quoted(LibcpContext* ctx, LibcpFile* current, char* filename) {
  if (filename[0] == '/')
    return absolute_hit(ctx, filename);

  char* path = libcp_format(ctx, "%s/%s", dirname(libcp_tu_strdup(ctx, current->name)), filename);
  if (libcp_file_exists(path)) {
    LibcpIncludeHit hit = {path, -1};
    record_file_index(ctx, hit.path, hit.index);
    return hit;
  }
  return resolve_paths(ctx, filename, 0);
}

// 尖括号形式: 绝对路径直接使用, 其余只走路径表.
LibcpIncludeHit libcp_include_resolve_angled(LibcpContext* ctx, char* filename) {
  if (filename[0] == '/')
    return absolute_hit(ctx, filename);
  return resolve_paths(ctx, filename, 0);
}

// #include_next: 从当前文件自己的命中下标 + 1 继续查路径表. 当前文件
// 没有记录 (主文件, 引号当前目录命中, 绝对路径) 时从 0 开始. 未命中时
// 返回空结果, 由预处理按字面文件名再试一次并沿用 cannot open 诊断.
LibcpIncludeHit libcp_include_resolve_next(LibcpContext* ctx, LibcpFile* current, char* filename) {
  int start = 0;
  if (current)
    start = file_index(ctx, current->name) + 1;
  return resolve_paths(ctx, filename, start);
}

// 既有入口保留原签名, 等同 USER 来源.
void libcp_context_add_include_path(LibcpContext* ctx, char* path) {
  libcp_include_add_path(ctx, path, LIBCP_INCLUDE_USER);
}

void libcp_context_add_include_path_with_origin(LibcpContext* ctx,
                                                char* path,
                                                LibcpIncludeOrigin origin) {
  libcp_include_add_path(ctx, path, origin);
}

// 公共解析入口: 先按原样查文件 (cwd 相对或绝对), 再走路径表. 返回
// per-TU 路径; 绝对路径找不到时直接 NULL, 不与路径表拼接.
char* libcp_context_resolve_include(LibcpContext* ctx, char* filename) {
  ENTER_RET(ctx, NULL);

  char* path;
  if (libcp_file_exists(filename)) {
    path = libcp_tu_strdup(ctx, filename);
    record_file_index(ctx, path, -1);
  } else if (filename[0] == '/') {
    path = NULL;
  } else {
    path = resolve_paths(ctx, filename, 0).path;
  }

  LEAVE(ctx);
  return path;
}

// SYSTEM 与 AFTER 视为系统目录; USER 与表外路径不算.
bool libcp_context_include_is_system(LibcpContext* ctx, char* path) {
  if (!ctx || !path)
    return false;

  LibcpIncludeSearch* s = &ctx->include_search;
  for (int i = 0; i < s->len; i++)
    if (origin_rank(s->paths[i].origin) > 0 && path_under(s->paths[i].path, path))
      return true;
  return false;
}

// 清空 per-TU 状态, 保留路径表.
void libcp_include_reset(LibcpContext* ctx) {
  clear_search_state(ctx);
}

// 释放配置路径表; 只由 libcp_context_free 调用. per-file 续查表是 per-TU
// 分配, 已由 free_tu_allocs 沿登记链归还, 这里不碰.
void libcp_include_free(LibcpContext* ctx) {
  LibcpIncludeSearch* s = &ctx->include_search;
  for (int i = 0; i < s->len; i++)
    LIBCP_FREE(s->paths[i].path);
  LIBCP_FREE(s->paths);
  s->paths = NULL;
  s->len   = 0;
  s->cap   = 0;
}
