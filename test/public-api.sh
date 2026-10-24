#!/bin/sh
# The public-API gate (PLAN L3.1d): every public header compiles on its
# own, a consumer that sees nothing but the public include tree links
# against libcp.a and runs, and the consumer object references no
# internal symbol. The full nm whitelist for the library itself arrives
# in L3.2b, after the .a symbol policy is decided.

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
# LIBCP_LIB selects the archive to inspect; test-stage2 points it at the
# stage2 archive, whose public surface must be the same.
lib=${LIBCP_LIB:-$root/libcp.a}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

if [ ! -f "$lib" ]; then
  echo "public-api: $lib is missing; run make libcp.a" >&2
  exit 1
fi

# Copy only the public include tree: an internal header is then not on
# the search path at all, so any accidental dependency fails to compile.
mkdir -p "$tmp/pubinc"
cp -R "$root/libcp/include/libcp" "$tmp/pubinc/"

for h in allocator diagnostic context token preprocess provenance cst ast sema libcp; do
  cat > "$tmp/h.c" <<EOF
#include <libcp/$h.h>
int main(void) { return 0; }
EOF
  "$cc" -std=c11 -fsyntax-only -Wall -Wextra -I"$tmp/pubinc" "$tmp/h.c"
  echo "public-api: header $h ok"
done

# A consumer that includes <stdio.h> first as well: the public headers
# must not depend on being included before anything else.
cat > "$tmp/stdio_first.c" <<'EOF'
#include <stdio.h>
#include <libcp/libcp.h>
int main(void) { return 0; }
EOF
"$cc" -std=c11 -fsyntax-only -Wall -Wextra -I"$tmp/pubinc" "$tmp/stdio_first.c"

cat > "$tmp/consumer.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include <libcp/libcp.h>

int main(void) {
  char *src = "int x = 1;\n";
  LibcpContext *ctx = libcp_context_new();
  if (!ctx || strcmp(libcp_version(), LIBCP_VERSION) != 0)
    return 1;

  LibcpToken *tok = libcp_tokenize_buffer(ctx, "<public-api>", src);
  if (!tok)
    return 1;
  tok = libcp_preprocess(ctx, tok);
  if (!tok)
    return 1;
  LibcpNode *ast = libcp_parse(ctx, tok);
  if (!ast || libcp_context_failed(ctx))
    return 1;
  LibcpObj *prog = libcp_sema(ctx, ast);
  if (!prog || libcp_context_failed(ctx))
    return 1;

  LibcpToken *raw = libcp_tokenize_buffer(ctx, "<public-api>", src);
  LibcpCstNode *root = libcp_cst_parse(ctx, raw);
  if (!root || !libcp_cst_verify(ctx, root, raw))
    return 1;

  // libcp_reconstruct rebuilds the tokenized bytes exactly.
  char *text = "int  x =  1;  /* c */\n\tchar *s = \"a\";\n";
  LibcpToken *t = libcp_tokenize_buffer(ctx, "<public-api>", text);
  FILE *f = tmpfile();
  if (!t || !f)
    return 1;
  libcp_reconstruct(ctx, t, f);
  fflush(f);
  rewind(f);
  char buf[128];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  if (strcmp(buf, text) != 0)
    return 1;

  libcp_context_free(ctx);
  puts("public-api: ok");
  return 0;
}
EOF

"$cc" -std=c11 -I"$tmp/pubinc" -c "$tmp/consumer.c" -o "$tmp/consumer.o"

# The object may reference libc and D7; an internal phase or helper name
# means the public surface leaked. Symbols differ by one leading
# underscore on Darwin. Since the symbol-prefix line every internal
# symbol carries the libcp_ prefix too, the deny list is anchored on it.
nm -u "$tmp/consumer.o" | awk '{print $NF}' > "$tmp/undef"
bad=$(grep -E '^_?(libcp_(tokenize_file_impl|preprocess2|parse_init|const_expr|add_type|new_cast|new_node|new_binary|new_unary|new_num|error|error_at|error_tok|warn_tok|strarray_push|nodearray_push|format|ctx|conditional|context_activate|context_set_base_file)|libcp_(hashmap|arena|sema|ty)_[a-z_0-9]+)$' "$tmp/undef" || true)
if [ -n "$bad" ]; then
  echo "public-api: the consumer references internal symbols:" >&2
  echo "$bad" >&2
  exit 1
fi

"$cc" -std=c11 -I"$tmp/pubinc" "$tmp/consumer.c" "$lib" \
  -o "$tmp/consumer"
"$tmp/consumer"

# PLAN L3.2b, revised by the symbol-prefix line (PLAN-pratt-default S2):
# the archive's internal symbols carry the libcp_ prefix too, but they
# are still not API - the public headers do not declare them and
# consumers must not call them. What is checked here is the other half:
# every D7 function is present. The strict export whitelist belongs to
# the shared library (L3.3b, -fvisibility=hidden).
d7="libcp_context_new
libcp_context_free
libcp_context_reset
libcp_context_set_diagnostic
libcp_context_failed
libcp_context_add_include_path
libcp_context_define
libcp_context_undef
libcp_context_set_predefined_macros
libcp_context_predefined_count
libcp_context_predefined_at
libcp_tokenize_file
libcp_tokenize_buffer
libcp_tokenize_buffer_len
libcp_reconstruct
libcp_preprocess
libcp_cst_parse
libcp_cst_seed_typedef
libcp_cst_reconstruct
libcp_cst_dump
libcp_cst_verify
libcp_parse
libcp_node_source_range
libcp_sema
libcp_token_provenance
libcp_macro_prov_next
libcp_macro_prov_expansion
libcp_macro_prov_arg_index
libcp_macro_expansion_name
libcp_macro_expansion_end
libcp_macro_expansion_macro
libcp_macro_expansion_def
libcp_macro_expansion_arg
libcp_macro_expansion_arg_count
libcp_macro_expansion_is_builtin
libcp_expansion_dump
libcp_expansion_verify
libcp_version"

nm "$lib" | awk '$2 == "T" { s = $3; sub(/^_/, "", s); print s }' \
  | sort -u > "$tmp/libT"
for sym in $d7; do
  if ! grep -qx "$sym" "$tmp/libT"; then
    echo "public-api: D7 symbol missing from libcp.a: $sym" >&2
    exit 1
  fi
done
echo "public-api: D7 symbols present in libcp.a"

# PLAN L3.3b: the shared library must export exactly D7. Only built on
# Linux, so only inspected there.
if [ -z "${LIBCP_LIB:-}" ] && [ -f "$root/libcp.so" ]; then
  nm -D --defined-only "$root/libcp.so" | awk '{print $NF}' \
    | sort -u > "$tmp/soT"
  printf '%s\n' "$d7" | sort -u > "$tmp/d7"
  if ! diff -u "$tmp/d7" "$tmp/soT"; then
    echo "public-api: libcp.so exports differ from D7" >&2
    exit 1
  fi
  echo "public-api: libcp.so exports exactly D7"
fi
