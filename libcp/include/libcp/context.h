// context 对象 (PLAN L2/L3.1b): 一个 context 就是一个翻译单元.
// context 是不透明的; 它的字段属于实现细节. 不同的 context 可以位于
// 不同线程; 同一个 context 不得被两个线程并发使用. 在内部, 每个
// 触碰 per-TU 分配, 诊断或 context 状态的函数都以 context 作为首参 -
// 不存在线程局部的当前 context, 因此诊断回调可以创建, 使用并释放
// 另一个 context, 而不会干扰外层调用.

#ifndef LIBCP_CONTEXT_H
#define LIBCP_CONTEXT_H

#include <libcp/diagnostic.h>
#include <stdbool.h>

// 把 D7 函数标记为从共享库导出; 库本身以 -fvisibility=hidden
// 构建 (PLAN L3.3b). 每个声明函数的头文件都包含本头文件. chibicc
// 定义了 __chibicc__, 且不解析前置的 visibility 属性; 它只构建静态库,
// 那里不需要该标记.
#if defined(__chibicc__)
#  define LIBCP_API
#elif defined(__GNUC__) || defined(__clang__)
#  define LIBCP_API __attribute__((visibility("default")))
#else
#  define LIBCP_API
#endif

typedef struct LibcpContext LibcpContext;

LIBCP_API LibcpContext* libcp_context_new(void);
LIBCP_API void libcp_context_free(LibcpContext* ctx);

// 释放所有 per-TU 分配并清空 per-TU 状态, 同时保留配置
// (include 路径, 宏操作, 诊断回调). 此后该 context 返回的所有指针
// 都失效. 发生错误会让 context 处于不一致状态: 再次使用前先 reset.
LIBCP_API void libcp_context_reset(LibcpContext* ctx);

LIBCP_API void libcp_context_set_diagnostic(LibcpContext* ctx, LibcpDiagnosticFn fn, void* user);
LIBCP_API bool libcp_context_failed(LibcpContext* ctx);

// include 搜索路径的来源. 路径按来源分桶, 搜索顺序固定为 USER ->
// AFTER -> SYSTEM, 同一桶内保持注册顺序. 这与 GCC 不同: GCC 把
// -idirafter 排在系统目录之后, 本库排在系统目录之前.
typedef enum {
  LIBCP_INCLUDE_USER,    // -I: 用户目录
  LIBCP_INCLUDE_SYSTEM,  // 内建或默认系统目录
  LIBCP_INCLUDE_AFTER,   // -idirafter: 用户目录之后, 系统目录之前
} LibcpIncludeOrigin;

// 追加一个 include 搜索目录; 等同于 origin 为 LIBCP_INCLUDE_USER 的
// 版本. 字符串被复制, 调用方的缓冲区可以立刻释放. 该配置跨 reset 保留.
LIBCP_API void libcp_context_add_include_path(LibcpContext* ctx, char* path);

// 按来源追加一个 include 搜索目录. 命令行上 -I 与 -idirafter 的先后
// 不影响结果: 搜索总是先走完 USER 桶, 再走 AFTER 桶, 最后走 SYSTEM 桶.
// 字符串被复制, 该配置跨 reset 保留.
LIBCP_API void libcp_context_add_include_path_with_origin(LibcpContext* ctx,
                                                          char* path,
                                                          LibcpIncludeOrigin origin);

// 解析一个 include 文件名: 先按原样查文件 (cwd 相对或绝对), 再按上述
// 顺序遍历路径表. 返回 per-TU 分配的路径, 其有效期到下一次 reset 或
// free; 找不到时返回 NULL. 绝对路径不会与路径表拼接. 返回的路径也会
// 更新内部搜索状态, 与预处理器里的 #include 命中一致.
LIBCP_API char* libcp_context_resolve_include(LibcpContext* ctx, char* filename);

// 判断一个文件路径是否落在系统目录下: 路径表前缀匹配, SYSTEM 与
// AFTER 来源算系统目录, USER 不算. 用于 -MMD 一类"省略系统头"的过滤;
// 不做 realpath 或 inode 归一.
LIBCP_API bool libcp_context_include_is_system(LibcpContext* ctx, char* path);

// 设置 __BASE_FILE__ 的路径. 这是 per-TU 状态: reset 清空; 未设置时,
// 第一个被词法分析的输入文件充当它. 字符串被复制进 per-TU 分配.
LIBCP_API void libcp_context_set_base_file(LibcpContext* ctx, char* path);

LIBCP_API void libcp_context_define(LibcpContext* ctx, char* name, char* buf);
LIBCP_API void libcp_context_undef(LibcpContext* ctx, char* name);

// context 集合中的一个预定义宏. `value` 是替换文本; 对于产生 token 的
// 内建宏以及动态的 __DATE__/__TIME__ 宏, 它为 NULL. `name` 与固定的
// `value` 指向静态表项, 不随 reset 失效; 只有 per-TU 构造的 __SIZEOF_*
// 替换文本由 context 分配, 会被 libcp_context_reset 失效.
typedef struct libcp_predefined_macro {
  const char* name;
  const char* value;
} LibcpPredefinedMacro;

// 预定义宏集合默认安装. setter 在下一次 libcp_context_reset 时生效
// (禁用它会产生干净的环境; 已记录的 -D/-U 操作仍然生效). count 与 at
// 函数枚举当前翻译单元已安装的集合 (禁用时为 0 项).
LIBCP_API void libcp_context_set_predefined_macros(LibcpContext* ctx, bool enabled);
LIBCP_API int libcp_context_predefined_count(LibcpContext* ctx);
LIBCP_API LibcpPredefinedMacro libcp_context_predefined_at(LibcpContext* ctx, int index);

#endif
