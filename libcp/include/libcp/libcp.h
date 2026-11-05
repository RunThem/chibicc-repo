// libcp 是一个可复用的 C 前端 (tokenize / preprocess / CST /
// parse / sema), 面向编译器之外的工具. 本仓库中的编译器是该库的一个
// 消费者; 它的代码生成器与驱动器不属于本库.
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

// 库版本 (PLAN L3.3c). 这里只承诺源码兼容: 修改公共结构体
// 就相当于版本升级. 该值必须与 Makefile 中的 VERSION 保持同步
// (test/install.sh 会检查).
#define LIBCP_VERSION "0.1.0"
LIBCP_API const char* libcp_version(void);

#endif
