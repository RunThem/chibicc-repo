// The context object (PLAN L2/L3.1b): one context is one translation unit.
// The context is opaque; its fields are an implementation detail.
// Different contexts may live in different threads; one context must not
// be used concurrently from two threads. Internally every function that
// touches the arena, diagnostics, or context state takes the context as
// its first parameter - there is no thread-local current context, so a
// diagnostic callback may create, use, and free another context without
// disturbing the outer call.
//
// context 对象 (PLAN L2/L3.1b): 一个 context 就是一个翻译单元.
// context 是不透明的; 它的字段属于实现细节. 不同的 context 可以位于
// 不同线程; 同一个 context 不得被两个线程并发使用. 在内部, 每个
// 触碰 arena, 诊断或 context 状态的函数都以 context 作为首参 -
// 不存在线程局部的当前 context, 因此诊断回调可以创建, 使用并释放
// 另一个 context, 而不会干扰外层调用.

#ifndef LIBCP_CONTEXT_H
#define LIBCP_CONTEXT_H

#include <stdbool.h>

#include <libcp/diagnostic.h>

// Marks a D7 function as exported from the shared library; the library
// itself is built with -fvisibility=hidden (PLAN L3.3b). Every header
// that declares functions includes this one. chibicc defines
// __chibicc__ and does not parse a leading visibility attribute; it
// only builds the static archive, where the marker is not needed.
//
// 把 D7 函数标记为从共享库导出; 库本身以 -fvisibility=hidden
// 构建 (PLAN L3.3b). 每个声明函数的头文件都包含本头文件. chibicc
// 定义了 __chibicc__, 且不解析前置的 visibility 属性; 它只构建静态库,
// 那里不需要该标记.
#if defined(__chibicc__)
#define LIBCP_API
#elif defined(__GNUC__) || defined(__clang__)
#define LIBCP_API __attribute__((visibility("default")))
#else
#define LIBCP_API
#endif

typedef struct LibcpContext LibcpContext;

LIBCP_API LibcpContext *libcp_context_new(void);
LIBCP_API void libcp_context_free(LibcpContext *ctx);

// Releases every per-TU allocation and clears the per-TU state, keeping
// the configuration (include paths, macro operations, diagnostic
// callback). Every pointer returned by this context is invalid
// afterwards. An error leaves the context inconsistent: reset before
// reusing it.
//
// 释放所有 per-TU 分配并清空 per-TU 状态, 同时保留配置
// (include 路径, 宏操作, 诊断回调). 此后该 context 返回的所有指针
// 都失效. 发生错误会让 context 处于不一致状态: 再次使用前先 reset.
LIBCP_API void libcp_context_reset(LibcpContext *ctx);

LIBCP_API void libcp_context_set_diagnostic(LibcpContext *ctx,
                                    LibcpDiagnosticFn fn, void *user);
LIBCP_API bool libcp_context_failed(LibcpContext *ctx);

LIBCP_API void libcp_context_add_include_path(LibcpContext *ctx, char *path);
LIBCP_API void libcp_context_define(LibcpContext *ctx, char *name, char *buf);
LIBCP_API void libcp_context_undef(LibcpContext *ctx, char *name);

// A predefined macro in the context's set. `value` is the replacement
// text; it is NULL for token-producing builtins and for the dynamic
// __DATE__/__TIME__ macros. The strings are owned by the context and
// invalidated by libcp_context_reset.
//
// context 集合中的一个预定义宏. `value` 是替换文本; 对于产生 token 的
// 内建宏以及动态的 __DATE__/__TIME__ 宏, 它为 NULL. 这些字符串归
// context 所有, 并会被 libcp_context_reset 失效.
typedef struct libcp_predefined_macro {
  const char *name;
  const char *value;
} LibcpPredefinedMacro;

// The predefined macro set is installed by default. The setter takes
// effect at the next libcp_context_reset (disabling it yields a clean
// environment; the recorded -D/-U operations still apply). The count and
// at functions enumerate the set installed for the current translation
// unit (0 entries when it is disabled).
//
// 预定义宏集合默认安装. setter 在下一次 libcp_context_reset 时生效
// (禁用它会产生干净的环境; 已记录的 -D/-U 操作仍然生效). count 与 at
// 函数枚举当前翻译单元已安装的集合 (禁用时为 0 项).
LIBCP_API void libcp_context_set_predefined_macros(LibcpContext *ctx, bool enabled);
LIBCP_API int libcp_context_predefined_count(LibcpContext *ctx);
LIBCP_API LibcpPredefinedMacro libcp_context_predefined_at(LibcpContext *ctx, int index);

#endif
