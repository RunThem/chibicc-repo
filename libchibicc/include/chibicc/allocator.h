// The allocation surface of libchibicc. Every block the library itself
// owns is obtained through these four macros and released through
// CHIBICC_FREE, so an embedder can route the library's memory to its own
// allocator (a pool, a tracking allocator, a freestanding heap).
//
// The choice is one build-time switch, not a per-macro fallback:
//  - CHIBICC_DEFAULT_ALLOCATOR defined: install the C library allocator,
//    i.e. the four macros expand to malloc / calloc / realloc / free.
//  - CHIBICC_DEFAULT_ALLOCATOR not defined: the embedder supplies all
//    four macros itself, usually through an -include shim header. When
//    libchibicc is being built (CHIBICC_INTERNAL) and any of the four is
//    missing, the #error below stops the build here instead of failing
//    at the first call site.
//
// The check is limited to the library build: a consumer that only
// includes the public headers never has to choose an allocator.
//
// The macros are compile-time configuration: define them when libchibicc
// is built (a -include shim header, or the equivalent -D options, in the
// library build's flags). Defining them only in a consumer that links a
// prebuilt library changes nothing - the library was already compiled
// with the defaults.
//
// A replacement must behave like the C library function it replaces:
//  - CHIBICC_MALLOC / CHIBICC_CALLOC return memory aligned for any
//    object type. Token and Node embed `long double`, so x86-64 needs
//    16-byte alignment.
//  - CHIBICC_CALLOC zero-fills the block; the library relies on that.
//  - CHIBICC_REALLOC follows realloc: a NULL pointer allocates, and on
//    failure it returns NULL leaving the original block valid.
//  - CHIBICC_FREE accepts NULL.
// The library does not recover from allocation failure; a replacement
// returning NULL fails the same way the C library default would.
//
// The macros add no symbols: the D7 binary surface and the shared
// library exports do not change.

#ifndef CHIBICC_ALLOCATOR_H
#define CHIBICC_ALLOCATOR_H

#ifdef CHIBICC_DEFAULT_ALLOCATOR

#include <stdlib.h>

#define CHIBICC_MALLOC(size) malloc(size)
#define CHIBICC_CALLOC(count, size) calloc(count, size)
#define CHIBICC_REALLOC(ptr, size) realloc(ptr, size)
#define CHIBICC_FREE(ptr) free(ptr)

#elif defined(CHIBICC_INTERNAL)

#if !defined(CHIBICC_MALLOC) || !defined(CHIBICC_CALLOC) || \
    !defined(CHIBICC_REALLOC) || !defined(CHIBICC_FREE)
#error "define CHIBICC_DEFAULT_ALLOCATOR (C library) or all four CHIBICC_* allocator macros"
#endif

#endif

#endif
