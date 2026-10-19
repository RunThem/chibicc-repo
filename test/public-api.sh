#!/bin/sh
# The public-API gate (PLAN L3.1d): every public header compiles on its
# own, a consumer that sees nothing but the public include tree links
# against libchibicc.a and runs, and the consumer object references no
# internal symbol. The full nm whitelist for the library itself arrives
# in L3.2b, after the .a symbol policy is decided.

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

if [ ! -f "$root/libchibicc.a" ]; then
  echo "public-api: $root/libchibicc.a is missing; run make libchibicc.a" >&2
  exit 1
fi

# Copy only the public include tree: an internal header is then not on
# the search path at all, so any accidental dependency fails to compile.
mkdir -p "$tmp/pubinc"
cp -R "$root/libchibicc/include/chibicc" "$tmp/pubinc/"

for h in diagnostic context token preprocess provenance cst ast sema chibicc; do
  cat > "$tmp/h.c" <<EOF
#include <chibicc/$h.h>
int main(void) { return 0; }
EOF
  "$cc" -std=c11 -fsyntax-only -Wall -Wextra -I"$tmp/pubinc" "$tmp/h.c"
  echo "public-api: header $h ok"
done

# A consumer that includes <stdio.h> first as well: the public headers
# must not depend on being included before anything else.
cat > "$tmp/stdio_first.c" <<'EOF'
#include <stdio.h>
#include <chibicc/chibicc.h>
int main(void) { return 0; }
EOF
"$cc" -std=c11 -fsyntax-only -Wall -Wextra -I"$tmp/pubinc" "$tmp/stdio_first.c"

cat > "$tmp/consumer.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include <chibicc/chibicc.h>

int main(void) {
  char *src = "int x = 1;\n";
  ChibiccContext *ctx = chibicc_context_new();

  ChibiccToken *tok = chibicc_tokenize_buffer(ctx, "<public-api>", src);
  if (!tok)
    return 1;
  tok = chibicc_preprocess(ctx, tok);
  if (!tok)
    return 1;
  ChibiccNode *ast = chibicc_parse(ctx, tok);
  if (!ast || chibicc_context_failed(ctx))
    return 1;
  ChibiccObj *prog = chibicc_sema(ctx, ast);
  if (!prog || chibicc_context_failed(ctx))
    return 1;

  ChibiccToken *raw = chibicc_tokenize_buffer(ctx, "<public-api>", src);
  ChibiccCstNode *root = chibicc_cst_parse(ctx, raw);
  if (!root || !chibicc_cst_verify(ctx, root, raw))
    return 1;

  // chibicc_reconstruct rebuilds the tokenized bytes exactly.
  char *text = "int  x =  1;  /* c */\n\tchar *s = \"a\";\n";
  ChibiccToken *t = chibicc_tokenize_buffer(ctx, "<public-api>", text);
  FILE *f = tmpfile();
  if (!t || !f)
    return 1;
  chibicc_reconstruct(ctx, t, f);
  fflush(f);
  rewind(f);
  char buf[128];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  if (strcmp(buf, text) != 0)
    return 1;

  chibicc_context_free(ctx);
  puts("public-api: ok");
  return 0;
}
EOF

"$cc" -std=c11 -I"$tmp/pubinc" -c "$tmp/consumer.c" -o "$tmp/consumer.o"

# The object may reference libc and D7; an internal phase or helper name
# means the public surface leaked. Symbols differ by one leading
# underscore on Darwin.
nm -u "$tmp/consumer.o" | awk '{print $NF}' > "$tmp/undef"
bad=$(grep -E '^_?(tokenize|tokenize_file|convert_pp_tokens|preprocess|parse|parse_init|sema|const_expr|add_type|new_cast|new_node|new_binary|new_unary|new_num|error|error_at|error_tok|warn_tok|hashmap_[a-z_0-9]+|strarray_push|nodearray_push|format|chibicc_arena_[a-z_0-9]+|chibicc_ctx|chibicc_context_activate|chibicc_context_set_base_file|cst_parse|cst_dump|cst_verify|cst_reconstruct|expansion_dump|expansion_verify)$' "$tmp/undef" || true)
if [ -n "$bad" ]; then
  echo "public-api: the consumer references internal symbols:" >&2
  echo "$bad" >&2
  exit 1
fi

"$cc" -std=c11 -I"$tmp/pubinc" "$tmp/consumer.c" "$root/libchibicc.a" \
  -o "$tmp/consumer"
"$tmp/consumer"
