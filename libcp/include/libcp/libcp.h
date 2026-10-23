// libcp: a reusable C front end (tokenize / preprocess / CST /
// parse / sema) for tools other than the compiler. The compiler in this
// repository is one consumer of the library; its code generator and
// driver are not part of it.
//
// Include this umbrella header; the layer headers next to it are
// included here and may also be included on their own.

#ifndef LIBCP_H
#define LIBCP_H

#include <libcp/allocator.h>
#include <libcp/diagnostic.h>
#include <libcp/context.h>
#include <libcp/token.h>
#include <libcp/preprocess.h>
#include <libcp/provenance.h>
#include <libcp/cst.h>
#include <libcp/ast.h>
#include <libcp/sema.h>

// The library version (PLAN L3.3c). Only source compatibility is
// promised: a change to a public struct is a version bump. Keep this in
// sync with VERSION in the Makefile (test/install.sh checks it).
#define LIBCP_VERSION "0.1.0"
LIBCP_API const char *libcp_version(void);

#endif
