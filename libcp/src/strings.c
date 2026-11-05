// 通用小工具: 字符串数组与节点数组的增长, 对齐取整, 转义打印,
// 文件存在性检查, 以及 arena 内的 printf 风格格式化. 这些函数被各层共用.

#include "libcp.h"

// 向字符串数组追加一项; 首次追加时分配 8 项容量, 满时容量翻倍.
void libcp_strarray_push(LibcpContext* ctx, LibcpStringArray* arr, char* s) {
  if (!arr->data) {
    arr->data     = libcp_arena_calloc(ctx, 8, sizeof(char*));
    arr->capacity = 8;
  }

  if (arr->capacity == arr->len) {
    int oldcap = arr->capacity;
    arr->data =
        libcp_arena_realloc(ctx, arr->data, sizeof(char*) * oldcap, sizeof(char*) * oldcap * 2);
    arr->capacity = oldcap * 2;
  }

  arr->data[arr->len++] = s;
}

// 向节点数组追加一项; 增长策略与 libcp_strarray_push 相同.
void libcp_nodearray_push(LibcpContext* ctx, LibcpNodeArray* arr, LibcpNode* node) {
  if (!arr->data) {
    arr->data     = libcp_arena_calloc(ctx, 8, sizeof(LibcpNode*));
    arr->capacity = 8;
  }

  if (arr->capacity == arr->len) {
    int oldcap    = arr->capacity;
    arr->data     = libcp_arena_realloc(ctx,
                                    arr->data,
                                    sizeof(LibcpNode*) * oldcap,
                                    sizeof(LibcpNode*) * oldcap * 2);
    arr->capacity = oldcap * 2;
  }

  arr->data[arr->len++] = node;
}

// 把非负的 `n` 向上取整到 `align` 的最近倍数. 例如:
// libcp_align_to(5, 8) 返回 8, libcp_align_to(11, 8) 返回 16.
int libcp_align_to(int n, int align) {
  return (n + align - 1) / align * align;
}

// 转义写出 p 的前 len 个字节: \n, \t, \r 打印为 C 转义序列, 反斜杠
// 打印为 \\; 其余控制字符, 0x7f 及以上的字节与空格都打印为 \xHH.
// 空格也转义, 这样 dump 行不会以可能被工具去掉的空白结尾.
void libcp_print_escaped(FILE* out, char* p, int len) {
  for (int i = 0; i < len; i++) {
    unsigned char c = p[i];

    switch (c) {
      case '\n': fprintf(out, "\\n"); break;
      case '\t': fprintf(out, "\\t"); break;
      case '\r': fprintf(out, "\\r"); break;
      case '\\': fprintf(out, "\\\\"); break;
      default:
        if (c < 0x20 || c >= 0x7f || c == ' ')
          fprintf(out, "\\x%02x", c);
        else
          fputc(c, out);
    }
  }
}

// 若文件存在则返回 true. 预处理器与驱动器都会用到; 它放在库里,
// 这样消费者不必依赖 main.c.
bool libcp_file_exists(char* path) {
  struct stat st;
  return !stat(path, &st);
}

// 接受 printf 风格的格式串, 返回分配在 context arena 中的格式化字符串.
char* libcp_format(LibcpContext* ctx, char* fmt, ...) {
  va_list ap;
  va_list ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int len = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);

  char* buf = libcp_arena_alloc(ctx, len + 1, 1);
  vsnprintf(buf, len + 1, fmt, ap);
  va_end(ap);
  return buf;
}
