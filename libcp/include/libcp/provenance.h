// Macro expansion provenance (plans/PLAN-provenance.md): every token the
// preprocessor produces by expanding a macro carries a chain of
// LibcpMacroProv nodes, one per expansion layer, innermost first; a token
// read from the source carries NULL. The records a chain points to are
// built once per expansion, immutable afterwards, and live as long as
// the tokens (arena allocation, never freed). LibcpMacro, LibcpMacroArg and
// LibcpMacroParam stay private to preprocess.c; these accessors are their
// public face.

#ifndef LIBCP_PROVENANCE_H
#define LIBCP_PROVENANCE_H

#include <stdio.h>

#include <libcp/token.h>

typedef struct libcp_macro_expansion LibcpMacroExpansion;
LIBCP_API LibcpMacroProv *libcp_token_provenance(LibcpToken *tok);
LIBCP_API LibcpMacroProv *libcp_macro_prov_next(LibcpMacroProv *p);
LIBCP_API LibcpMacroExpansion *libcp_macro_prov_expansion(LibcpMacroProv *p);
LIBCP_API int libcp_macro_prov_arg_index(LibcpMacroProv *p);

LIBCP_API LibcpToken *libcp_macro_expansion_name(LibcpMacroExpansion *e);
LIBCP_API LibcpToken *libcp_macro_expansion_end(LibcpMacroExpansion *e); // rparen; name if none
LIBCP_API char *libcp_macro_expansion_macro(LibcpMacroExpansion *e);       // the macro's spelling
LIBCP_API LibcpToken *libcp_macro_expansion_def(LibcpMacroExpansion *e); // #define name; NULL if none
LIBCP_API LibcpToken *libcp_macro_expansion_arg(LibcpMacroExpansion *e, int i);
LIBCP_API int libcp_macro_expansion_arg_count(LibcpMacroExpansion *e);
LIBCP_API bool libcp_macro_expansion_is_builtin(LibcpMacroExpansion *e);

// The read-only provenance channels (plans/PLAN.md 判据 5). The dump prints one
// line per non-EOF token of the preprocessed stream; the verifier checks
// every chain and returns false at the first violation. Neither touches
// the compilation path.
LIBCP_API void libcp_expansion_dump(LibcpContext *ctx, LibcpToken *tok, FILE *out);
LIBCP_API bool libcp_expansion_verify(LibcpContext *ctx, LibcpToken *tok);

#endif
