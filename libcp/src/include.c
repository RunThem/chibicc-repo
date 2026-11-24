// 内部 include 模块 (issue #23): include 搜索的单一 owner.
//
// 本文件独占路径表 (path + origin), 搜索顺序, 搜索缓存, 系统目录判定与
// reset / free 语义. preprocess.c 只提交 include 请求, 不再拼路径或直接
// 查文件; 驱动器只按 origin 注册路径, 并用 public 的系统目录判定做
// -MMD 分类.
//
// 搜索顺序固定为 USER -> AFTER -> SYSTEM, 桶内保持插入顺序. 路径表按
// 桶连续存放, 所以数组本身就是搜索顺序. 这与 GCC 不同: GCC 把
// -idirafter 排在系统目录之后, 本库排在系统目录之前.
//
// 路径表是配置, 跨 reset 保留; 搜索缓存与 #include_next 游标是 per-TU
// 状态, 随 reset 清空. 全部状态都在 LibcpContext 内, 没有静态全局.
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

  // 新路径可能改变此前命中的结果, 丢弃 per-TU 搜索缓存. 表项本身是
  // per-TU 分配, reset 时随登记链一起归还, 这里只清空索引.
  s->cache = (LibcpHashMap){};
}

// 从第 start 项起按搜索顺序查找 filename; 命中时通过 index_out 返回
// 路径表下标. 本函数不更新 #include_next 游标, 由调用方决定.
static char* search_paths(LibcpContext* ctx, int start, char* filename, int* index_out) {
  LibcpIncludeSearch* s = &ctx->include_search;

  for (int i = start; i < s->len; i++) {
    char* path = libcp_format(ctx, "%s/%s", s->paths[i].path, filename);
    if (!libcp_file_exists(path))
      continue;
    *index_out = i;
    return path;
  }
  *index_out = -1;
  return NULL;
}

// 路径表搜索, 命中结果按 filename 缓存在 per-TU 搜索缓存里. 命中同时把
// #include_next 游标移到命中项之后; 缓存命中不移动游标, 与旧实现一致.
static char* resolve_paths(LibcpContext* ctx, char* filename) {
  LibcpIncludeSearch* s = &ctx->include_search;

  char* cached = libcp_hashmap_get(ctx, &s->cache, filename);
  if (cached)
    return cached;

  int index;
  char* path = search_paths(ctx, 0, filename, &index);
  if (!path)
    return NULL;
  libcp_hashmap_put(ctx, &s->cache, filename, path);
  s->next_idx = index + 1;
  return path;
}

// 绝对路径绕过路径表, 与旧实现一致.
char* libcp_include_resolve_angled(LibcpContext* ctx, char* filename) {
  if (filename[0] == '/')
    return filename;
  return resolve_paths(ctx, filename);
}

// 引号形式: 先查当前文件所在目录, 再走路径表. 当前目录命中不移动
// #include_next 游标.
char* libcp_include_resolve_quoted(LibcpContext* ctx, LibcpFile* current, char* filename) {
  if (filename[0] == '/')
    return filename;

  char* path = libcp_format(ctx, "%s/%s", dirname(libcp_tu_strdup(ctx, current->name)), filename);
  if (libcp_file_exists(path))
    return path;
  return resolve_paths(ctx, filename);
}

// #include_next: 从上次命中项之后继续. 命中后游标停在命中项 (旧实现的
// 全局游标语义); 表耗尽时游标停在表尾. per-file 续查是 issue #24.
char* libcp_include_resolve_next(LibcpContext* ctx, char* filename) {
  LibcpIncludeSearch* s = &ctx->include_search;

  int index;
  char* path = search_paths(ctx, s->next_idx, filename, &index);
  if (path)
    return path;
  s->next_idx = s->len;
  return NULL;
}

// 任何来源的路径表前缀匹配. 只服务本 ticket 的第一阶段: 在 -MMD 分类
// 修复之前保留旧行为 (所有注册过的目录都算"标准目录"). 下一提交删除.
bool libcp_include_in_path_table(LibcpContext* ctx, char* path) {
  LibcpIncludeSearch* s = &ctx->include_search;
  for (int i = 0; i < s->len; i++)
    if (path_under(s->paths[i].path, path))
      return true;
  return false;
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
  if (libcp_file_exists(filename))
    path = libcp_tu_strdup(ctx, filename);
  else if (filename[0] == '/')
    path = NULL;
  else
    path = resolve_paths(ctx, filename);

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
  ctx->include_search.cache    = (LibcpHashMap){};
  ctx->include_search.next_idx = 0;
}

// 释放配置路径表; 只由 libcp_context_free 调用.
void libcp_include_free(LibcpContext* ctx) {
  LibcpIncludeSearch* s = &ctx->include_search;
  for (int i = 0; i < s->len; i++)
    LIBCP_FREE(s->paths[i].path);
  LIBCP_FREE(s->paths);
  s->paths = NULL;
  s->len   = 0;
  s->cap   = 0;
}
