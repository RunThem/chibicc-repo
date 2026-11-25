// 驱动器 (main.c 与 codegen.c) 共用的本地工具. 它们不属于 libcp:
// 公共面不暴露 printf 风格格式化与致命诊断, 这里只服务仓库内的驱动器,
// 因此只包含公共头与标准头.

#ifndef LIBCP_DRIVER_UTIL_H
#define LIBCP_DRIVER_UTIL_H

#include <libcp/libcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>

#ifndef __GNUC__
#  define __attribute__(x)
#endif

// printf 风格格式化. 结果随进程生命周期, 不进入库的 per-TU 登记链.
static inline char* driver_format(char* fmt, ...) __attribute__((format(printf, 1, 2)));

static inline char* driver_format(char* fmt, ...) {
  va_list ap;
  va_list ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int len = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);

  char* buf = calloc(1, len + 1);
  vsnprintf(buf, len + 1, fmt, ap);
  va_end(ap);
  return buf;
}

// 致命诊断: 与库内部的 error 路径共用同一套格式化与回调交付, 但驱动器
// 不在公共入口内, 出错时没有可以返回的翻译单元, 因此报告后结束进程.
// tok 是锚点 token, 可以为 NULL.
static inline noreturn void driver_error(LibcpContext* ctx, LibcpToken* tok, char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

static inline noreturn void driver_error(LibcpContext* ctx, LibcpToken* tok, char* fmt, ...) {
  va_list ap;
  va_list ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int len = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);

  char* msg = calloc(1, len + 1);
  vsnprintf(msg, len + 1, fmt, ap);
  va_end(ap);

  libcp_diagnostic_report(ctx, tok, LIBCP_DIAG_ERROR, "%s", msg);
  exit(1);
}

// 不可能到达的分支: 文案与锚点保持调用点所在文件的源码位置.
#define driver_unreachable(ctx)                                                                    \
  driver_error((ctx), NULL, "internal error at %s:%d", __FILE__, __LINE__)

#endif
