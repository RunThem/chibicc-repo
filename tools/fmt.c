// tools/fmt.c - the CST formatter MVP (PLAN-l5 W4).
//
// The library's first real consumer: it tokenizes and builds the CST
// through the public API only (layers 1/2), then walks the tree and
// re-emits the source with normalized whitespace. The policy is
// deliberately conservative:
//
// - line-preserving: every newline in the trivia is kept, so `__LINE__`
//   semantics and line counts do not move (the -S equivalence gate
//   depends on this);
// - comments and backslash-newline continuations are emitted
//   byte-for-byte;
// - indentation is recomputed from the tree: BLOCK / GROUP bodies sit
//   one level deeper and closers return to their opener's level
//   (2 spaces per level);
// - every other whitespace run between two tokens collapses to one
//   space, or none if the source had none;
// - spellings are never touched.
//
// Usage: fmt [-s typedef]... file.c   (the result goes to stdout)

#include <stdio.h>
#include <string.h>

#include <libcp/libcp.h>

typedef struct {
  FILE *out;
  int at_line_start;  // nothing emitted on the current line yet
  int had_space;      // collapsible whitespace seen since the last token
  int paren_depth;    // open ( [ since the line's block indent base
  int errors;
} Emitter;

static void emit_indent(Emitter *e, int depth) {
  for (int i = 0; i < 2 * depth + 2 * e->paren_depth; i++)
    fputc(' ', e->out);
}

// A separator before an inline comment. Comments themselves are never
// re-wrapped.
static void comment_prefix(Emitter *e, int depth) {
  if (e->at_line_start) {
    emit_indent(e, depth);
    e->at_line_start = 0;
  } else if (e->had_space) {
    fputc(' ', e->out);
  }
}

static void newline(Emitter *e) {
  e->at_line_start = 1;
  e->had_space = 0;
  e->paren_depth = 0;
}

// Walk the token's trivia: comments and continuations verbatim,
// newlines kept (each triggers re-indent), other whitespace collapses.
static void emit_trivia(Emitter *e, LibcpToken *tok, int depth) {
  char *p = tok->trivia;
  char *end = p + tok->trivia_len;

  while (p < end) {
    // Block comment: through the closing */ (or the end of the file).
    if (p + 1 < end && p[0] == '/' && p[1] == '*') {
      char *q = p + 2;
      for (;;) {
        while (q < end && q[0] != '*')
          q++;
        if (q + 1 >= end || q[1] == '/') {
          if (q + 1 < end)
            q++;
          break;
        }
        q++;
      }
      comment_prefix(e, depth);
      fwrite(p, 1, q + 1 - p, e->out);
      p = q + 1;
      e->had_space = 1;
      continue;
    }
    // Line comment: through (not including) the newline.
    if (p + 1 < end && p[0] == '/' && p[1] == '/') {
      char *q = p;
      while (q < end && q[0] != '\n')
        q++;
      comment_prefix(e, depth);
      fwrite(p, 1, q - p, e->out);
      p = q;
      e->had_space = 1;
      continue;
    }
    // Backslash-newline continuation: whitespace between the backslash
    // and the newline stays as written; the next line restarts the
    // indent (directives continue at their level).
    if (p[0] == '\\') {
      char *q = p + 1;
      while (q < end && (q[0] == ' ' || q[0] == '\t' || q[0] == '\r'))
        q++;
      if (q < end && q[0] == '\n') {
        fwrite(p, 1, q + 1 - p, e->out);
        p = q + 1;
        newline(e);
        continue;
      }
      e->had_space = 1;
      p++;
      continue;
    }
    if (p[0] == '\n') {
      fputc('\n', e->out);
      p++;
      newline(e);
      continue;
    }
    if (p[0] == '\r') {
      fputc('\r', e->out);
      p++;
      if (p < end && p[0] == '\n') {
        fputc('\n', e->out);
        p++;
        newline(e);
      }
      continue;
    }
    e->had_space = 1;
    p++;
  }
}

static void emit_token(Emitter *e, LibcpToken *tok, int depth) {
  emit_trivia(e, tok, depth);

  char *s = tok->raw_loc;
  int len = tok->raw_len;
  if (len == 0)
    return; // EOF: trailing trivia only, no spelling

  int is_open = (len == 1 && (s[0] == '(' || s[0] == '['));
  int is_close = (len == 1 && (s[0] == ')' || s[0] == ']'));

  if (e->at_line_start) {
    emit_indent(e, depth);
    e->at_line_start = 0;
  } else if (e->had_space) {
    fputc(' ', e->out);
  }
  e->had_space = 0;

  if (is_close && e->paren_depth > 0)
    e->paren_depth--;
  fwrite(s, 1, len, e->out);
  if (is_open)
    e->paren_depth++;
}

// The CST walk. BLOCK, statement expressions and delimiter groups
// indent their bodies one level deeper; the final closer returns to the
// opener's level. Every other node is a transparent container - PP
// directives included: they sit at the surrounding level, their bodies
// too (the common convention for #if blocks).
static void emit_node(Emitter *e, LibcpCstNode *node, int depth) {
  if (node->kind == CST_TOKEN || node->kind == CST_EOF) {
    emit_token(e, node->tok, depth);
    return;
  }

  int structured = node->kind == CST_BLOCK || node->kind == CST_STMT_EXPR ||
                   node->kind == CST_GROUP_PAREN ||
                   node->kind == CST_GROUP_BRACKET ||
                   node->kind == CST_GROUP_BRACE ||
                   node->kind == CST_INIT_LIST;

  for (int i = 0; i < node->child_count; i++) {
    LibcpCstNode *child = node->children[i];
    int d = depth;
    if (structured) {
      LibcpCstNode *last = node->children[node->child_count - 1];
      int closer = last->kind == CST_TOKEN && last->tok->raw_len == 1 &&
                   strchr(")]}", last->tok->raw_loc[0]);
      if (i > 0 && closer && child == last)
        d = depth;
      else if (i > 0)
        d = depth + 1;
    }
    emit_node(e, child, d);
  }
}

static void on_diag(void *user, const LibcpDiagnostic *d) {
  Emitter *e = user;
  e->errors++;
  fprintf(stderr, "fmt: %s:%d: %s\n", d->filename ? d->filename : "<fmt>",
          d->line, d->message);
}

int main(int argc, char **argv) {
  const char *path = NULL;

  Emitter emitter = {stdout, 1, 0, 0, 0};
  LibcpContext *ctx = libcp_context_new();
  if (!ctx) {
    fprintf(stderr, "fmt: out of memory\n");
    return 1;
  }
  libcp_context_set_diagnostic(ctx, on_diag, &emitter);

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-s") && i + 1 < argc) {
      libcp_cst_seed_typedef(ctx, argv[++i]);
      continue;
    }
    if (argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "fmt: unknown option: %s\n", argv[i]);
      return 1;
    }
    path = argv[i];
  }
  if (!path) {
    fprintf(stderr, "usage: fmt [-s typedef]... file.c\n");
    return 1;
  }

  LibcpToken *tok = libcp_tokenize_file(ctx, (char *)path);
  if (!tok)
    return 1;
  LibcpCstNode *root = libcp_cst_parse(ctx, tok);
  if (!root)
    return 1;

  emit_node(&emitter, root, 0);
  libcp_context_free(ctx);

  if (fflush(stdout) != 0 || emitter.errors) {
    fprintf(stderr, "fmt: failed to format %s\n", path);
    return 1;
  }
  return 0;
}
