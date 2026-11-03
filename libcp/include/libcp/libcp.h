// libcp: a reusable C front end (tokenize / preprocess / CST /
// parse / sema) for tools other than the compiler. The compiler in this
// repository is one consumer of the library; its code generator and
// driver are not part of it.
//
// libcp 是一个可复用的 C 前端 (tokenize / preprocess / CST /
// parse / sema), 面向编译器之外的工具. 本仓库中的编译器是该库的一个
// 消费者; 它的代码生成器与驱动器不属于本库.
//
// Include this umbrella header; the layer headers next to it are
// included here and may also be included on their own.
//
// 包含这个总头文件; 与它相邻的各层头文件在这里被包含,
// 也可以单独包含.

#ifndef LIBCP_H
#define LIBCP_H

#include <libcp/allocator.h>
#include <libcp/ast.h>
#include <libcp/context.h>
#include <libcp/cst.h>
#include <libcp/diagnostic.h>
#include <libcp/preprocess.h>
#include <libcp/provenance.h>
#include <libcp/sema.h>
#include <libcp/token.h>

// The library version (PLAN L3.3c). Only source compatibility is
// promised: a change to a public struct is a version bump. Keep this in
// sync with VERSION in the Makefile (test/install.sh checks it).
//
// 库版本 (PLAN L3.3c). 这里只承诺源码兼容: 修改公共结构体
// 就相当于版本升级. 该值必须与 Makefile 中的 VERSION 保持同步
// (test/install.sh 会检查).
#define LIBCP_VERSION "0.1.0"
LIBCP_API const char* libcp_version(void);

#endif
