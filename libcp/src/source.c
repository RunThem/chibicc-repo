// 内部源坐标模块 (issue #21): LibcpFile 与扫描缓冲区的唯一 owner.
//
// 本文件独占 LibcpFile 的构造, raw 文本到扫描文本的 contents / map /
// lines 构造, raw 偏移 / 扫描偏移 / 物理行之间的换算, file 级 synthetic
// 判定, 以及 input file 表的登记. tokenize.c 只负责切词; range 与
// provenance 共用这里的换算, 不再各自做 loc - contents 或
// line_no - line_delta.
#include "libcp.h"

// raw 文本到扫描文本的构造, 定义在本文件末尾.
static char*
    make_scan_buffer(LibcpContext* ctx, char* raw, int len, int** map_out, int** lines_out);

// 把一个十六进制数字字符转成数值; 调用方保证 c 是十六进制数字.
static int from_hex(char c) {
  if ('0' <= c && c <= '9')
    return c - '0';
  if ('a' <= c && c <= 'f')
    return c - 'a' + 10;
  return c - 'A' + 10;
}

// 解析 \u / \U 之后的 len 个十六进制数字; 遇到非十六进制数字时返回 0.
static uint32_t read_universal_char(char* p, int len) {
  uint32_t c = 0;
  for (int i = 0; i < len; i++) {
    if (!isxdigit((unsigned char)p[i]))
      return 0;
    c = (c << 4) | from_hex(p[i]);
  }
  return c;
}

// 若 p 处是一个换行则为 true: '\n', '\r\n' 或 '\r'.
bool libcp_is_line_break(const char* p) {
  return *p == '\n' || *p == '\r';
}

// p 处换行的长度.
int libcp_line_break_len(const char* p) {
  return (*p == '\r' && p[1] == '\n') ? 2 : 1;
}

// 扫描缓冲区中某个位置的 raw 偏移 (PLAN T2.1): 在续行, '\r' 或
// universal character name 被改写的地方, 两份文本并不相同.
// 对合成缓冲区来说两者完全一致.
int libcp_file_raw_offset(const LibcpFile* file, const char* p) {
  return file->map[p - file->contents];
}

// token 拼写结束之后的 raw 偏移: 下一个 token 前面被跳过的字节
// 从这里开始.
int libcp_token_raw_end(const LibcpToken* tok) {
  return tok->raw_loc - tok->file->raw + tok->raw_len;
}

// 产生扫描偏移 `off` 的那些 raw 字节之后的 raw 偏移: 换行与
// universal character name 对应多个 raw 字节, 其余情况只对应一个.
int libcp_file_raw_end_of(const LibcpFile* file, int off) {
  int lo  = file->map[off];
  char* p = file->raw + lo;

  if (libcp_is_line_break(p))
    return lo + libcp_line_break_len(p);

  if (p[0] == '\\' && (p[1] == 'u' || p[1] == 'U')) {
    int digs = p[1] == 'u' ? 4 : 8;
    if (read_universal_char(p + 2, digs))
      return lo + digs + 2;
  }
  return lo + 1;
}

// raw 偏移对应的物理行号.
int libcp_file_physical_line(const LibcpFile* file, int raw_offset) {
  return file->lines[raw_offset];
}

// token 的物理行号: 从文件的 lines / map 表取, 不依赖 token 的
// line_no 与 line_delta (后者会被 #line 与重扫改写).
int libcp_token_physical_line(const LibcpToken* tok) {
  return libcp_file_physical_line(tok->file, libcp_file_raw_offset(tok->file, tok->loc));
}

// token 的源位置: 所属文件加扫描缓冲区内的偏移, 供跨 token 比较先后.
LibcpSourcePos libcp_token_source_pos(const LibcpToken* tok) {
  LibcpSourcePos pos = {tok->file, tok->loc - tok->file->contents};
  return pos;
}

// 合成缓冲区的 raw 就是它自身的文本; 真实文件的 raw 则是另一块分配.
bool libcp_file_is_synthetic(const LibcpFile* file) {
  return file->raw == file->contents;
}

// 公共的 file 级 synthetic 判定 (issue #21). 内存输入也满足它;
// 宏展开产物的 @synth 标记按 token 的 provenance 判断, 是另一回事.
bool libcp_token_is_synthetic(const LibcpToken* tok) {
  return tok && libcp_file_is_synthetic(tok->file);
}

// 枚举当前翻译单元已读入的文件, 顺序是首次读取顺序, 主文件通常是
// 索引 0. count 在 ctx 为 NULL 时为 0; at 越界或 ctx 为 NULL 时为 NULL.
int libcp_context_input_file_count(LibcpContext* ctx) {
  return ctx ? ctx->input_files_len : 0;
}

LibcpFile* libcp_context_input_file_at(LibcpContext* ctx, int index) {
  if (!ctx || index < 0 || index >= ctx->input_files_len)
    return NULL;
  return ctx->input_files[index];
}

// 把一个顶层输入文件登记进 context 的输入文件表. 表的顺序就是登记
// 顺序; 末尾始终留一个 NULL 终止项.
void libcp_source_register_input_file(LibcpContext* ctx, LibcpFile* file) {
  int index = ctx->input_files_len;
  if (ctx->input_files_cap < index + 2) {
    size_t old           = sizeof(LibcpFile*) * ctx->input_files_cap;
    int cap              = index + 2;
    ctx->input_files     = libcp_tu_realloc(ctx, ctx->input_files, old, sizeof(LibcpFile*) * cap);
    ctx->input_files_cap = cap;
  }
  ctx->input_files[index]     = file;
  ctx->input_files[index + 1] = NULL;
  ctx->input_files_len        = index + 1;
}

// 合成缓冲区 (内存输入, 宏体, 拼接, 内建值) 的文件构造: 它自身就是 raw
// 文本, map 是恒等映射, 行号就是简单的换行计数.
LibcpFile* libcp_new_file_len(LibcpContext* ctx, char* name, int file_no, char* contents, int len) {
  LibcpFile* file    = libcp_tu_calloc(ctx, 1, sizeof(LibcpFile));
  file->name         = name;
  file->display_name = name;
  file->file_no      = file_no;
  file->contents     = contents;

  file->raw   = contents;
  file->map   = libcp_tu_calloc(ctx, len + 2, sizeof(int));
  file->lines = libcp_tu_calloc(ctx, len + 2, sizeof(int));

  int line = 1;
  for (int i = 0; i <= len; i++) {
    file->map[i]   = i;
    file->lines[i] = line;
    if (i < len && contents[i] == '\n')
      line++;
  }
  return file;
}

LibcpFile* libcp_new_file(LibcpContext* ctx, char* name, int file_no, char* contents) {
  return libcp_new_file_len(ctx, name, file_no, contents, strlen(contents));
}

// 由 raw 文本为真实文件构建 LibcpFile: make_scan_buffer 建出扫描文本与
// 两张坐标表, 文件编号按 context 的递增序号分配 (从 1 开始).
LibcpFile* libcp_new_file_from_raw(LibcpContext* ctx, char* name, char* raw, int len) {
  int* map;
  int* lines;
  char* contents = make_scan_buffer(ctx, raw, len, &map, &lines);

  LibcpFile* file = libcp_new_file(ctx, name, ++ctx->next_file_no, contents);
  // 扫描缓冲区替换掉 new_file 建出的恒等映射表.
  file->raw       = raw;
  file->map       = map;
  file->lines     = lines;
  return file;
}

// 由 raw 文本为真实文件构建词法分析器扫描的缓冲区 (PLAN T2.1). 扫描文本
// 与过去三个前置 pass 的产物一致 - 换行规范化为 '\n', 换行前的反斜杠连同
// 换行一起删除, universal character name 改写为 UTF-8, 跳过开头的 BOM,
// 并保证末尾有换行 - 这样每个消费者看到的文本与以前相同. `map` 记录每个
// 扫描偏移来自哪个 raw 偏移, `lines` 记录每个 raw 偏移的行号: 两者合起来
// 让 token 流仍可抵达 raw 坐标. 行号是这些 pass 唯一的可观察结果, 并被
// 精确复现 - 续行内部的换行记在下一个真实换行处 (被删除的 pass 把那些
// 换行下移), 合成的末尾换行则记在最后.
static char*
    make_scan_buffer(LibcpContext* ctx, char* raw, int len, int** map_out, int** lines_out) {
  int* map   = libcp_tu_calloc(ctx, len + 2, sizeof(int));
  int* lines = libcp_tu_calloc(ctx, len + 2, sizeof(int));
  char* buf  = libcp_tu_calloc(ctx, 1, len + 2);

  // 前置 pass 过去看到的文本: raw 文本, 若不以换行结尾则补一个末尾换行.
  char* text = libcp_tu_calloc(ctx, 1, len + 2);
  memcpy(text, raw, len);
  if (len == 0 || raw[len - 1] != '\n')
    text[len] = '\n';
  // raw 长度, 再加上合成的末尾换行. 绝不能用 strlen: raw 文本可能含有 NUL,
  // 这种情况由 libcp_tokenize_file_impl 诊断.
  int n = len;
  if (len == 0 || raw[len - 1] != '\n')
    n++;

  int i       = 0;  // `text` 中的位置
  int j       = 0;  // `buf` 中的位置
  int mark    = 0;  // 第一个还没有行号的 raw 偏移
  int line    = 1;  // `i` 处的行号
  int pending = 0;  // 尚未计入的续行换行数

  // 开头的 BOM 会被跳过, 并成为第一个 token 的前导 trivia 的一部分.
  if (!strncmp(text, "\xef\xbb\xbf", 3))
    i = 3;
  map[0] = i;

  while (i < n) {
    // 遍历经过的每个 raw 偏移都取它所在位置的行号; 续行内部的偏移属于
    // 它们所在的那一行.
    for (; mark <= i && mark <= len; mark++)
      lines[mark] = line;

    // 换行前的反斜杠让两行连接起来: 反斜杠与换行一起被丢弃, 该换行对
    // 词法分析器来说不算换行.
    if (text[i] == '\\' && i + 1 < n && libcp_is_line_break(text + i + 1)) {
      pending++;
      i += 1 + libcp_line_break_len(text + i + 1);
      continue;
    }

    // 换行被规范化为单个 '\n'.
    if (libcp_is_line_break(text + i)) {
      map[j]   = i;
      buf[j++] = '\n';
      i += libcp_line_break_len(text + i);
      line += 1 + pending;
      pending = 0;
      continue;
    }

    // universal character name 被改写为 UTF-8; `raw` 中保留原来的转义写法.
    if (text[i] == '\\' && (text[i + 1] == 'u' || text[i + 1] == 'U')) {
      int digs   = text[i + 1] == 'u' ? 4 : 8;
      uint32_t c = read_universal_char(text + i + 2, digs);
      if (c) {
        int k = libcp_encode_utf8(buf + j, c);
        for (int e = 0; e < k; e++)
          map[j + e] = i;
        j += k;
        i += digs + 2;
        continue;
      }
    }

    // 其它反斜杠与它后面的一个字节一起原样复制, 以免把 "\\u" 中转义过的
    // "u" 当成转义序列的开头.
    if (text[i] == '\\' && i + 1 < len) {
      map[j]   = i;
      buf[j++] = text[i++];
    }

    map[j]   = i;
    buf[j++] = text[i++];
  }

  // 词法分析器要求缓冲区以换行结尾: 行注释要一直跑到下一个换行. 过去的
  // pass 在末尾续行吞掉合成换行时会保留它, 办法就是在最后补出尚未计入的
  // 换行.
  if (j == 0 || buf[j - 1] != '\n') {
    map[j]   = len;
    buf[j++] = '\n';
  }

  // 计入那些后面从未遇到真实换行的续行, 并给文件末尾 (EOF token 所在处)
  // 赋上最终的行号.
  line += pending;
  for (; mark <= len; mark++)
    lines[mark] = line;
  lines[len] = line;

  map[j]     = len;
  *map_out   = map;
  *lines_out = lines;
  return buf;
}
