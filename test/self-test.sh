#!/bin/sh
# The library's unit self-tests (hashmap, arena realloc contract, sema
# state reset across two TUs). The test code lives in test/self/self-test.c
# and links the archive; it is not part of libcp.
#
# LIBCP_LIB selects the archive; the test targets point it at the
# stage2 / Pratt / stage2-Pratt archives as well as the default one.
# LIBCP_PRATT_PARSE_FEATURE must match the parser the archive was
# built with.

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
lib=${LIBCP_LIB:-$root/libcp.a}
pratt=${LIBCP_PRATT_PARSE_FEATURE:-0}
pratt_flag="-DLIBCP_PRATT_PARSE_FEATURE=$pratt"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

if [ ! -f "$lib" ]; then
  echo "self-test: $lib is missing; run make libcp.a" >&2
  exit 1
fi

# The test includes the internal umbrella, so it sees the allocator
# check; it is compiled for the default allocator, like the archive.
"$cc" -std=c11 -g -Wall -Wno-switch $pratt_flag -DLIBCP_DEFAULT_ALLOCATOR \
  -I"$root/libcp/include" -I"$root/libcp/src" \
  "$root/test/self/self-test.c" "$lib" -o "$tmp/self-test"
"$tmp/self-test"
