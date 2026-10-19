// Macro expansion provenance (PLAN-provenance.md): every token the
// preprocessor produces by expanding a macro carries a chain of
// ChibiccMacroProv nodes, one per expansion layer, innermost first; a token
// read from the source carries NULL. The records a chain points to are
// built once per expansion, immutable afterwards, and live as long as
// the tokens (arena allocation, never freed). Macro, MacroArg and
// MacroParam stay private to preprocess.c; these accessors are their
// public face.

#ifndef CHIBICC_PROVENANCE_H
#define CHIBICC_PROVENANCE_H

#include <stdio.h>

#include <chibicc/token.h>

typedef struct chibicc_macro_expansion ChibiccMacroExpansion;
ChibiccMacroProv *chibicc_token_provenance(ChibiccToken *tok);
ChibiccMacroProv *chibicc_macro_prov_next(ChibiccMacroProv *p);
ChibiccMacroExpansion *chibicc_macro_prov_expansion(ChibiccMacroProv *p);
int chibicc_macro_prov_arg_index(ChibiccMacroProv *p);

ChibiccToken *chibicc_macro_expansion_name(ChibiccMacroExpansion *e);
ChibiccToken *chibicc_macro_expansion_end(ChibiccMacroExpansion *e); // rparen; name if none
char *chibicc_macro_expansion_macro(ChibiccMacroExpansion *e);       // the macro's spelling
ChibiccToken *chibicc_macro_expansion_def(ChibiccMacroExpansion *e); // #define name; NULL if none
ChibiccToken *chibicc_macro_expansion_arg(ChibiccMacroExpansion *e, int i);
int chibicc_macro_expansion_arg_count(ChibiccMacroExpansion *e);
bool chibicc_macro_expansion_is_builtin(ChibiccMacroExpansion *e);

// The read-only provenance channels (PLAN.md 判据 5). The dump prints one
// line per non-EOF token of the preprocessed stream; the verifier checks
// every chain and returns false at the first violation. Neither touches
// the compilation path.
void chibicc_expansion_dump(ChibiccContext *ctx, ChibiccToken *tok, FILE *out);
bool chibicc_expansion_verify(ChibiccContext *ctx, ChibiccToken *tok);

#endif
