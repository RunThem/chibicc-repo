// In-tree driver declarations shared by main.c and codegen.c. They are
// not part of libcp: libcp/src/libcp.h declares only what
// the library defines or consumes, so the library subtree can be moved
// to another project as is.

#ifndef LIBCP_DRIVER_H
#define LIBCP_DRIVER_H

#include "libcp.h"

void codegen(LibcpContext *ctx, LibcpObj *prog, FILE *out);

extern bool opt_fpic;
extern bool opt_fcommon;

#endif
