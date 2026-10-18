// Read-only macro-expansion provenance channels (PLAN.md, 判据 5).
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
// -verify-expansions checks the chain invariants and prints at most one
// message on stderr: exit 0 on success, 1 on the first violation. This
// skeleton (M1.1) accepts every chain; M2.2 fills in the checks. Neither
// channel touches the compilation path.

#include "chibicc.h"

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

void expansion_dump(Token *tok, FILE *out) {
  for (; tok->kind != TK_EOF; tok = tok->next) {
    fputs("T ", out);
    print_pos(out, tok);
    fprintf(out, " %s ", token_kind_name(tok->kind));
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

bool expansion_verify(Token *tok) {
  (void)tok;
  return true;
}
