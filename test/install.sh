#!/bin/sh
# The packaging gate (PLAN L3.3c): make install puts exactly the public
# headers, the library and libchibicc.pc under the prefix, pkg-config
# reports the same version as CHIBICC_VERSION, and a consumer builds and
# runs with pkg-config's flags.

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
prefix="$tmp/prefix"

make -C "$root" install PREFIX="$prefix" >/dev/null

count=$(ls "$prefix/include/chibicc" | wc -l | tr -d ' ')
if [ "$count" -ne 9 ]; then
  echo "install: expected 9 public headers, found $count" >&2
  ls "$prefix/include/chibicc" >&2
  exit 1
fi
for h in chibicc context diagnostic token preprocess provenance cst ast sema; do
  if [ ! -f "$prefix/include/chibicc/$h.h" ]; then
    echo "install: missing header $h.h" >&2
    exit 1
  fi
done
[ -f "$prefix/lib/libchibicc.a" ] || { echo "install: missing libchibicc.a" >&2; exit 1; }
[ -f "$prefix/lib/pkgconfig/libchibicc.pc" ] || { echo "install: missing libchibicc.pc" >&2; exit 1; }
if [ "$(uname -s)" = "Linux" ]; then
  [ -f "$prefix/lib/libchibicc.so.0" ] || { echo "install: missing libchibicc.so.0" >&2; exit 1; }
  [ -L "$prefix/lib/libchibicc.so" ] || { echo "install: missing libchibicc.so symlink" >&2; exit 1; }
fi

if ! command -v pkg-config >/dev/null 2>&1; then
  echo "install: pkg-config not found; the .pc checks were skipped" >&2
  echo "install: ok"
  exit 0
fi

export PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
cflags=$(pkg-config --cflags libchibicc)
libs=$(pkg-config --libs libchibicc)
pcver=$(pkg-config --modversion libchibicc)
hdrver=$(sed -n 's/^#define CHIBICC_VERSION "\(.*\)"/\1/p' \
  "$root/libchibicc/include/chibicc/chibicc.h")
if [ "$pcver" != "$hdrver" ]; then
  echo "install: pkg-config version $pcver != CHIBICC_VERSION $hdrver" >&2
  exit 1
fi
echo "install: pkg-config: $cflags | $libs | $pcver"

cat > "$tmp/consumer.c" <<'EOF'
#include <string.h>
#include <chibicc/chibicc.h>

int main(void) {
  if (strcmp(chibicc_version(), CHIBICC_VERSION) != 0)
    return 1;
  ChibiccContext *ctx = chibicc_context_new();
  ChibiccToken *tok = chibicc_tokenize_buffer(ctx, "<install>", "int x;\n");
  if (!tok)
    return 1;
  tok = chibicc_preprocess(ctx, tok);
  if (!tok)
    return 1;
  ChibiccNode *ast = chibicc_parse(ctx, tok);
  if (!ast || chibicc_context_failed(ctx))
    return 1;
  if (!chibicc_sema(ctx, ast))
    return 1;
  chibicc_context_free(ctx);
  return 0;
}
EOF

"$cc" -std=c11 $cflags "$tmp/consumer.c" $libs -o "$tmp/consumer"
if [ -f "$prefix/lib/libchibicc.so" ]; then
  LD_LIBRARY_PATH="$prefix/lib" "$tmp/consumer"
else
  "$tmp/consumer"
fi

echo "install: ok"
