// The allocation surface of libcp. Every block the library itself
// owns is obtained through these four macros and released through
// LIBCP_FREE, so an embedder can route the library's memory to its own
// allocator (a pool, a tracking allocator, a freestanding heap).
//
// libcp 的分配接口. 库自身拥有的每个内存块都通过这四个宏获取,
// 并通过 LIBCP_FREE 释放, 因此嵌入方可以把库的内存接到自己的分配器
// (内存池, 跟踪分配器, freestanding 堆).
//
// The choice is one build-time switch, not a per-macro fallback:
//  - LIBCP_DEFAULT_ALLOCATOR defined: install the C library allocator,
//    i.e. the four macros expand to malloc / calloc / realloc / free.
//  - LIBCP_DEFAULT_ALLOCATOR not defined: the embedder supplies all
//    four macros itself, usually through an -include shim header. When
//    libcp is being built (LIBCP_INTERNAL) and any of the four is
//    missing, the #error below stops the build here instead of failing
//    at the first call site.
//
// 选择由单个构建期开关决定, 不做逐宏回退:
//  - 定义了 LIBCP_DEFAULT_ALLOCATOR: 安装 C 库分配器,
//    即四个宏展开为 malloc / calloc / realloc / free.
//  - 未定义 LIBCP_DEFAULT_ALLOCATOR: 嵌入方自己提供全部四个宏,
//    通常通过一个 -include shim 头文件. 当正在构建 libcp
//    (LIBCP_INTERNAL) 且四个宏中缺失任何一个时, 下面的 #error
//    会在此处终止构建, 而不是等到第一个调用点才失败.
//
// The check is limited to the library build: a consumer that only
// includes the public headers never has to choose an allocator.
//
// 该检查只限于库构建: 只包含公共头文件的消费者永远不需要
// 选择分配器.
//
// The macros are compile-time configuration: define them when libcp
// is built (a -include shim header, or the equivalent -D options, in the
// library build's flags). Defining them only in a consumer that links a
// prebuilt library changes nothing - the library was already compiled
// with the defaults.
//
// 这些宏是编译期配置: 在构建 libcp 时定义它们 (库构建参数中的
// -include shim 头文件, 或等价的 -D 选项). 只在链接预编译库的消费者中
// 定义它们不会产生任何效果 - 库在编译时已经使用了默认值.
//
// A replacement must behave like the C library function it replaces:
//  - LIBCP_MALLOC / LIBCP_CALLOC return memory aligned for any
//    object type. LibcpToken and LibcpNode embed `long double`, so x86-64 needs
//    16-byte alignment.
//  - LIBCP_CALLOC zero-fills the block; the library relies on that.
//  - LIBCP_REALLOC follows realloc: a NULL pointer allocates, and on
//    failure it returns NULL leaving the original block valid.
//  - LIBCP_FREE accepts NULL.
// The library does not recover from allocation failure; a replacement
// returning NULL fails the same way the C library default would.
//
// 替代实现必须与被替代的 C 库函数行为一致:
//  - LIBCP_MALLOC / LIBCP_CALLOC 返回对任何对象类型都对齐的内存.
//    LibcpToken 与 LibcpNode 内嵌 `long double`, 因此 x86-64 需要
//    16 字节对齐.
//  - LIBCP_CALLOC 将内存块清零; 库依赖这一行为.
//  - LIBCP_REALLOC 遵循 realloc: 传入 NULL 指针时执行分配,
//    失败时返回 NULL 并保持原内存块有效.
//  - LIBCP_FREE 接受 NULL.
// 库不从分配失败中恢复; 替代实现返回 NULL 时的失败方式与 C 库
// 默认实现相同.
//
// The macros add no symbols: the D7 binary surface and the shared
// library exports do not change.
//
// 这些宏不新增符号: D7 二进制面与共享库导出都不会改变.

#ifndef LIBCP_ALLOCATOR_H
#define LIBCP_ALLOCATOR_H

#ifdef LIBCP_DEFAULT_ALLOCATOR

#include <stdlib.h>

#define LIBCP_MALLOC(size) malloc(size)
#define LIBCP_CALLOC(count, size) calloc(count, size)
#define LIBCP_REALLOC(ptr, size) realloc(ptr, size)
#define LIBCP_FREE(ptr) free(ptr)

#elif defined(LIBCP_INTERNAL)

#if !defined(LIBCP_MALLOC) || !defined(LIBCP_CALLOC) || \
    !defined(LIBCP_REALLOC) || !defined(LIBCP_FREE)
#error "define LIBCP_DEFAULT_ALLOCATOR (C library) or all four LIBCP_* allocator macros"
#endif

#endif

#endif
