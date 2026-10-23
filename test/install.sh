#!/bin/sh
# The packaging gate (PLAN L3.3c): make install puts exactly the public
# headers, the library and libcp.pc under the prefix, pkg-config
# reports the same version as LIBCP_VERSION, and a consumer builds and
# runs with pkg-config's flags.

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
prefix="$tmp/prefix"

make -C "$root" install PREFIX="$prefix" >/dev/null

count=$(ls "$prefix/include/libcp" | wc -l | tr -d ' ')
if [ "$count" -ne 10 ]; then
  echo "install: expected 10 public headers, found $count" >&2
  ls "$prefix/include/libcp" >&2
  exit 1
fi
for h in libcp allocator context diagnostic token preprocess provenance cst ast sema; do
  if [ ! -f "$prefix/include/libcp/$h.h" ]; then
    echo "install: missing header $h.h" >&2
    exit 1
  fi
done
[ -f "$prefix/lib/libcp.a" ] || { echo "install: missing libcp.a" >&2; exit 1; }
[ -f "$prefix/lib/pkgconfig/libcp.pc" ] || { echo "install: missing libcp.pc" >&2; exit 1; }
if [ "$(uname -s)" = "Linux" ]; then
  [ -f "$prefix/lib/libcp.so.0" ] || { echo "install: missing libcp.so.0" >&2; exit 1; }
  [ -L "$prefix/lib/libcp.so" ] || { echo "install: missing libcp.so symlink" >&2; exit 1; }
fi

if ! command -v pkg-config >/dev/null 2>&1; then
  echo "install: pkg-config not found; the .pc checks were skipped" >&2
  echo "install: ok"
  exit 0
fi

export PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
cflags=$(pkg-config --cflags libcp)
libs=$(pkg-config --libs libcp)
pcver=$(pkg-config --modversion libcp)
hdrver=$(sed -n 's/^#define LIBCP_VERSION "\(.*\)"/\1/p' \
  "$root/libcp/include/libcp/libcp.h")
if [ "$pcver" != "$hdrver" ]; then
  echo "install: pkg-config version $pcver != LIBCP_VERSION $hdrver" >&2
  exit 1
fi
echo "install: pkg-config: $cflags | $libs | $pcver"

cat > "$tmp/consumer.c" <<'EOF'
#include <string.h>
#include <libcp/libcp.h>

int main(void) {
  if (strcmp(libcp_version(), LIBCP_VERSION) != 0)
    return 1;
  LibcpContext *ctx = libcp_context_new();
  LibcpToken *tok = libcp_tokenize_buffer(ctx, "<install>", "int x;\n");
  if (!tok)
    return 1;
  tok = libcp_preprocess(ctx, tok);
  if (!tok)
    return 1;
  LibcpNode *ast = libcp_parse(ctx, tok);
  if (!ast || libcp_context_failed(ctx))
    return 1;
  if (!libcp_sema(ctx, ast))
    return 1;
  libcp_context_free(ctx);
  return 0;
}
EOF

"$cc" -std=c11 $cflags "$tmp/consumer.c" $libs -o "$tmp/consumer"
if [ -f "$prefix/lib/libcp.so" ]; then
  LD_LIBRARY_PATH="$prefix/lib" "$tmp/consumer"
else
  "$tmp/consumer"
fi

echo "install: ok"
