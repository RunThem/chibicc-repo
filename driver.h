// In-tree driver declarations shared by main.c and codegen.c. They are
// not part of libchibicc: libchibicc/src/chibicc.h declares only what
// the library defines or consumes, so the library subtree can be moved
// to another project as is.

#ifndef CHIBICC_DRIVER_H
#define CHIBICC_DRIVER_H

#include "chibicc.h"

void codegen(ChibiccContext *ctx, Obj *prog, FILE *out);

extern bool opt_fpic;
extern bool opt_fcommon;

#endif
