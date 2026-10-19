// libchibicc: a reusable C front end (tokenize / preprocess / CST /
// parse / sema) for tools other than the compiler. The compiler in this
// repository is one consumer of the library; its code generator and
// driver are not part of it.
//
// Include this umbrella header; the layer headers next to it are
// included here and may also be included on their own.

#ifndef CHIBICC_H
#define CHIBICC_H

#include <chibicc/diagnostic.h>
#include <chibicc/context.h>
#include <chibicc/token.h>
#include <chibicc/preprocess.h>
#include <chibicc/provenance.h>
#include <chibicc/cst.h>
#include <chibicc/ast.h>
#include <chibicc/sema.h>

#endif
