// The allocation surface of libcp. Every block the library itself
// owns is obtained through these four macros and released through
// LIBCP_FREE, so an embedder can route the library's memory to its own
// allocator (a pool, a tracking allocator, a freestanding heap).
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
// The check is limited to the library build: a consumer that only
// includes the public headers never has to choose an allocator.
//
// The macros are compile-time configuration: define them when libcp
// is built (a -include shim header, or the equivalent -D options, in the
// library build's flags). Defining them only in a consumer that links a
// prebuilt library changes nothing - the library was already compiled
// with the defaults.
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
// The macros add no symbols: the D7 binary surface and the shared
// library exports do not change.

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
