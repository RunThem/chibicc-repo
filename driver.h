// 仓库内驱动器 (main.c 与 codegen.c) 共用的声明. 它们不属于 libcp:
// 驱动器只包含公共伞头, 库子目录可以整体搬到另一个项目.

#ifndef LIBCP_DRIVER_H
#define LIBCP_DRIVER_H

#include <libcp/libcp.h>

void codegen(LibcpContext* ctx, LibcpObj* prog, FILE* out);

extern bool opt_fpic;
extern bool opt_fcommon;

#endif
