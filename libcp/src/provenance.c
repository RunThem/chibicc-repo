// Read-only macro-expansion provenance channels (plans/PLAN.md, 判据 5).
//
// -dump-expansions prints one line per non-EOF token of the preprocessed
// stream:
//
//   T <file>:<phys-line|@synth> <kind> <spelling> <chain>
//
// <chain> is "-" for a token read from the source. Otherwise it lists the
// token's MacroProv layers, innermost first, joined by ">"; a layer reads
//
//   <macro>@<call-file>:<call-line>:<body|argN>:<def-file>:<def-line>
//
// with argN 1-based and "?:?" when the macro has no #define token
// (builtins, -D definitions). A pasted macro name makes a call position
// synthetic; it prints as @synth like any other synthetic token.
//
// -verify-expansions checks the chain invariants of plans/RESULT.md (I1-I3)
// and prints at most one message on stderr: exit 0 on success, 1 on the
// first violation. It never calls error() (判据 4), so the channel can
// run over a broken stream without taking the process down. This is the
// only consumer of the accessors besides the dump. Neither channel touches
// the compilation path.

#include "libcp.h"

// A synthetic buffer (paste, stringize, builtin values) is its own raw
// text; a real file's raw text is a separate allocation.
static bool is_synthetic(Token *tok) {
  return tok->file->raw == tok->file->contents;
}

// Print a token's position as <file>:<line>, or <file>:@synth when the
// position is not a line of a real file. phys-line = line_no -
// line_delta, because preprocess() adds the #line offset to every
// token's line number at the end.
static void print_pos(FILE *out, Token *tok) {
  if (is_synthetic(tok))
    fprintf(out, "%s:@synth", tok->file->name);
  else
    fprintf(out, "%s:%d", tok->file->name, tok->line_no - tok->line_delta);
}

void expansion_dump(LibcpContext *ctx, Token *tok, FILE *out) {
  for (; tok->kind != TK_EOF; tok = tok->next) {
    fputs("T ", out);
    print_pos(out, tok);
    fprintf(out, " %s ", token_kind_name(ctx, tok->kind));
    print_escaped(out, tok->loc, tok->len);

    MacroProv *prov = token_provenance(tok);
    if (!prov) {
      fputs(" -\n", out);
      continue;
    }

    fputc(' ', out);
    for (MacroProv *p = prov; p; p = macro_prov_next(p)) {
      if (p != prov)
        fputc('>', out);

      MacroExpansion *e = macro_prov_expansion(p);
      fprintf(out, "%s@", macro_expansion_macro(e));
      print_pos(out, macro_expansion_name(e));

      int arg_index = macro_prov_arg_index(p);
      if (arg_index < 0)
        fputs(":body:", out);
      else
        fprintf(out, ":arg%d:", arg_index + 1);

      Token *def = macro_expansion_def(e);
      if (def)
        print_pos(out, def);
      else
        fputs("?:?", out);
    }
    fputc('\n', out);
  }
}

// A chain deeper than this is treated as corruption rather than as a real
// expansion: recursive expansion is bounded by hidesets, so real chains are
// short, and the bound keeps a broken chain from looping.
#define MAX_PROV_DEPTH 1000

// An argument list runs from its first token to an EOF marker (an empty
// argument is just the marker). Bounded so that a malformed list cannot
// loop here.
static bool verify_arg_list(Token *tok) {
  int n = 0;
  for (; tok; tok = tok->next) {
    if (tok->kind == TK_EOF)
      return true;
    if (++n > 1 << 20)
      return false;
  }
  return false;
}

static void report(Token *tok, char *what) {
  fputs("chibicc: ", stderr);
  print_pos(stderr, tok);
  fprintf(stderr, ": invalid macro provenance: %s\n", what);
}

// I1: the head of a token's chain is the expansion that produced it, its
// layer body or a valid argument index. I2: an argument layer names an
// existing argument of a function-like, non-builtin expansion, and every
// argument list is well-formed. I3: no node appears twice (chains are
// prepend-only and share immutable tails).
static bool verify_chain(Token *tok) {
  MacroProv *head = token_provenance(tok);
  int depth = 0;

  for (MacroProv *p = head; p; p = macro_prov_next(p)) {
    if (++depth > MAX_PROV_DEPTH) {
      report(tok, "chain is too deep");
      return false;
    }

    int seen = 0;
    int steps = 0;
    for (MacroProv *q = head; q; q = macro_prov_next(q)) {
      if (q == p)
        seen++;
      if (++steps > MAX_PROV_DEPTH) {
        report(tok, "chain is too deep");
        return false;
      }
    }
    if (seen > 1) {
      report(tok, "chain node appears twice");
      return false;
    }

    MacroExpansion *e = macro_prov_expansion(p);
    if (!e) {
      report(tok, "layer without an expansion");
      return false;
    }

    Token *name = macro_expansion_name(e);
    char *macro = macro_expansion_macro(e);
    if (!name || !macro) {
      report(tok, "expansion without a name");
      return false;
    }

    if (strlen(macro) != (size_t)name->len ||
        strncmp(macro, name->loc, name->len)) {
      report(tok, "macro name does not match the call token");
      return false;
    }

    int arg_index = macro_prov_arg_index(p);
    int arg_count = macro_expansion_arg_count(e);
    if (arg_index < -1 || arg_index >= arg_count) {
      report(tok, "argument index out of range");
      return false;
    }

    for (int i = 0; i < arg_count; i++)
      if (!verify_arg_list(macro_expansion_arg(e, i))) {
        report(tok, "malformed argument list");
        return false;
      }

    if (arg_index >= 0 &&
        (macro_expansion_end(e) == name || macro_expansion_is_builtin(e))) {
      report(tok, "argument layer on a non-function-like expansion");
      return false;
    }
  }
  return true;
}

bool expansion_verify(Token *tok) {
  for (; tok->kind != TK_EOF; tok = tok->next)
    if (token_provenance(tok) && !verify_chain(tok))
      return false;
  return true;
}
