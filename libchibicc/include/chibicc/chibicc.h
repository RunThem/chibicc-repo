// libchibicc: a reusable C front end (tokenize / preprocess / CST /
// parse / sema) for tools other than the compiler. The compiler in this
// repository is one consumer of the library; its code generator and
// driver are not part of it.
//
// Include this umbrella header; the layer headers next to it are
// included here and may also be included on their own.

#ifndef CHIBICC_H
#define CHIBICC_H

#include <chibicc/allocator.h>
#include <chibicc/diagnostic.h>
#include <chibicc/context.h>
#include <chibicc/token.h>
#include <chibicc/preprocess.h>
#include <chibicc/provenance.h>
#include <chibicc/cst.h>
#include <chibicc/ast.h>
#include <chibicc/sema.h>

// The library version (PLAN L3.3c). Only source compatibility is
// promised: a change to a public struct is a version bump. Keep this in
// sync with VERSION in the Makefile (test/install.sh checks it).
#define CHIBICC_VERSION "0.1.0"
CHIBICC_API const char *chibicc_version(void);

#endif
