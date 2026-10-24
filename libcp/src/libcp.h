// Internal umbrella (PLAN L3.1). The public types and declarations live
// under libcp/include/libcp/; this header is what the
// implementation includes. Everything below the public include is
// internal to the library: the context layout, the arena, the helper
// containers, and the phase functions the facade wraps.
#define LIBCP_INTERNAL 1

#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <libgen.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "libcp/libcp.h"

// The public surface uses the Libcp-prefixed names only (PLAN L3.2a);
// the implementation keeps the short spellings through these aliases.
typedef LibcpToken Token;
typedef LibcpFile File;
typedef LibcpTokenKind TokenKind;
typedef LibcpHideset Hideset;
typedef LibcpMacroProv MacroProv;
typedef LibcpMacroExpansion MacroExpansion;
typedef LibcpCstNode CstNode;
typedef LibcpCstKind CstKind;
typedef LibcpStringArray StringArray;
typedef LibcpObj Obj;
typedef LibcpRelocation Relocation;
typedef LibcpVarAttr VarAttr;
typedef LibcpInitializer Initializer;
typedef LibcpInitItem InitItem;
typedef LibcpInitDesig InitDesig;
typedef LibcpInitTree InitTree;
typedef LibcpInitPath InitPath;
typedef LibcpType Type;
typedef LibcpMember Member;
typedef LibcpNode Node;
typedef LibcpNodeKind NodeKind;
typedef LibcpTypeKind TypeKind;

#define MAX(x, y) ((x) < (y) ? (y) : (x))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#ifndef __GNUC__
# define __attribute__(x)
#endif

//
// strings.c
//

typedef struct {
  Node **data;
  int capacity;
  int len;
} NodeArray;

void libcp_strarray_push(LibcpContext *ctx, StringArray *arr, char *s);
void libcp_nodearray_push(LibcpContext *ctx, NodeArray *arr, Node *node);
char *libcp_format(LibcpContext *ctx, char *fmt, ...) __attribute__((format(printf, 2, 3)));

//
// tokenize.c
//

noreturn void libcp_error(LibcpContext *ctx, char *fmt, ...) __attribute__((format(printf, 2, 3)));
noreturn void libcp_error_at(LibcpContext *ctx, char *loc, char *fmt, ...) __attribute__((format(printf, 3, 4)));
noreturn void libcp_error_tok(LibcpContext *ctx, Token *tok, char *fmt, ...) __attribute__((format(printf, 3, 4)));
void libcp_warn_tok(LibcpContext *ctx, Token *tok, char *fmt, ...) __attribute__((format(printf, 3, 4)));
bool libcp_equal(Token *tok, char *op);
Token *libcp_skip(LibcpContext *ctx, Token *tok, char *op);
bool libcp_consume(Token **rest, Token *tok, char *str);
void libcp_convert_pp_tokens(LibcpContext *ctx, Token *tok);
File **libcp_get_input_files(LibcpContext *ctx);
File *libcp_new_file(LibcpContext *ctx, char *name, int file_no, char *contents);
File *libcp_new_file_len(LibcpContext *ctx, char *name, int file_no, char *contents, int len);
bool libcp_is_line_break(char *p);
int libcp_line_break_len(char *p);
Token *libcp_tokenize_string_literal(LibcpContext *ctx, Token *tok, Type *basety);
Token *libcp_tokenize(LibcpContext *ctx, File *file);
Token *libcp_tokenize_file_impl(LibcpContext *ctx, char *filename);

#define unreachable() \
  libcp_error(ctx, "internal error at %s:%d", __FILE__, __LINE__)

//
// preprocess.c
// The CST channel entries (libcp_cst_parse / _reconstruct / _dump /
// _verify), the pipeline entries (libcp_preprocess / libcp_parse /
// libcp_sema), the expansion channels (libcp_expansion_dump / _verify)
// and the provenance accessors carry their public names directly and are
// declared in the public headers (PLAN-pratt-default S2: the facade
// wrappers merged into the implementations).
//

char *libcp_search_include_paths(LibcpContext *ctx, char *filename);
void libcp_init_macros(LibcpContext *ctx);
void libcp_define_macro(LibcpContext *ctx, char *name, char *buf);
void libcp_undef_macro(LibcpContext *ctx, char *name);

//
// parse.c
//

// Constructors for the faithful-tree nodes. Defined in parse.c; sema
// uses them to build the nodes its lowerings introduce. The shapes that
// only a lowering produces - a bound name, a VLA designator, a numeric
// literal typed at birth - sema constructs itself.
Node *libcp_new_node(LibcpContext *ctx, NodeKind kind, Token *tok);
Node *libcp_new_binary(LibcpContext *ctx, NodeKind kind, Node *lhs, Node *rhs, Token *tok);
Node *libcp_new_unary(LibcpContext *ctx, NodeKind kind, Node *expr, Token *tok);
Node *libcp_new_num(LibcpContext *ctx, int64_t val, Token *tok);

// Identifier spelling, used by sema when it declares the objects the
// parser's declaration records name.
char *libcp_get_ident(LibcpContext *ctx, Token *tok);

void libcp_parse_init(LibcpContext *ctx);
void libcp_sema_store_label_gotos(LibcpContext *ctx, Obj *fn, NodeArray gotos);
NodeArray *libcp_sema_label_gotos(LibcpContext *ctx, Obj *fn);
void libcp_sema_state_clear(LibcpContext *ctx);

//
// sema.c
//

// Parses and evaluates a constant expression. Used by the
// preprocessor for `#if`; not part of the parse/sema pipeline below.
int64_t libcp_const_expr(LibcpContext *ctx, Token **rest, Token *tok);

// The parser's conditional-expression entry point (parse.c), which
// libcp_const_expr uses to read the expression it evaluates.
Node *libcp_conditional(LibcpContext *ctx, Token **rest, Token *tok);

// The type-level tools a consumer needs in order to build nodes that
// arrive already typed: the annotation entry point, the constructors for
// the shapes only sema produces, and the conversions that carry sema's
// decisions.
void libcp_add_type(LibcpContext *ctx, Node *node);
Node *libcp_new_arith(LibcpContext *ctx, NodeKind kind, Node *lhs, Node *rhs, Token *tok);
void libcp_usual_arith_conv(LibcpContext *ctx, Node **lhs, Node **rhs);
Type *libcp_get_common_type(LibcpContext *ctx, Type *ty1, Type *ty2);
Node *libcp_new_cast(LibcpContext *ctx, Node *expr, Type *ty);
Node *libcp_new_long(LibcpContext *ctx, int64_t val, Token *tok);
Node *libcp_new_ulong(LibcpContext *ctx, long val, Token *tok);
Node *libcp_new_var_node(LibcpContext *ctx, Obj *var, Token *tok);
Node *libcp_new_vla_ptr(LibcpContext *ctx, Obj *var, Token *tok);
Node *libcp_new_alloca(LibcpContext *ctx, Node *sz);

// The next value the anonymous-name counter would hand out, without
// consuming it.
int libcp_unique_name_next(LibcpContext *ctx);

//
// type.c
//

extern Type *libcp_ty_void;
extern Type *libcp_ty_bool;

extern Type *libcp_ty_char;
extern Type *libcp_ty_short;
extern Type *libcp_ty_int;
extern Type *libcp_ty_long;

extern Type *libcp_ty_uchar;
extern Type *libcp_ty_ushort;
extern Type *libcp_ty_uint;
extern Type *libcp_ty_ulong;

extern Type *libcp_ty_float;
extern Type *libcp_ty_double;
extern Type *libcp_ty_ldouble;

bool libcp_is_integer(Type *ty);
bool libcp_is_flonum(Type *ty);
bool libcp_is_numeric(Type *ty);
bool libcp_is_compatible(Type *t1, Type *t2);
Type *libcp_copy_type(LibcpContext *ctx, Type *ty);
Type *libcp_pointer_to(LibcpContext *ctx, Type *base);
Type *libcp_func_type(LibcpContext *ctx, Type *return_ty);
Type *libcp_array_of(LibcpContext *ctx, Type *base, int size);
Type *libcp_vla_of(LibcpContext *ctx, Type *base, Node *expr);
Type *libcp_array_of_dim(LibcpContext *ctx, Type *base, Node *dim);
Type *libcp_typeof_placeholder(LibcpContext *ctx);
Type *libcp_enum_type(LibcpContext *ctx);
Type *libcp_struct_type(LibcpContext *ctx);

//
// strings.c
//

int libcp_align_to(int n, int align);

//
// unicode.c
//

int libcp_encode_utf8(char *buf, uint32_t c);
uint32_t libcp_decode_utf8(LibcpContext *ctx, char **new_pos, char *p);
bool libcp_is_ident1(uint32_t c);
bool libcp_is_ident2(uint32_t c);
int libcp_display_width(LibcpContext *ctx, char *p, int len);

//
// hashmap.c
//

typedef struct {
  char *key;
  int keylen;
  void *val;
} HashEntry;

typedef struct {
  HashEntry *buckets;
  int capacity;
  int used;     // live entries plus tombstones
  int ndeleted; // tombstones in `used`
} HashMap;

void *libcp_hashmap_get(LibcpContext *ctx, HashMap *map, char *key);
void *libcp_hashmap_get2(LibcpContext *ctx, HashMap *map, char *key, int keylen);
void libcp_hashmap_put(LibcpContext *ctx, HashMap *map, char *key, void *val);
void libcp_hashmap_put2(LibcpContext *ctx, HashMap *map, char *key, int keylen, void *val);
void libcp_hashmap_delete(LibcpContext *ctx, HashMap *map, char *key);
void libcp_hashmap_delete2(LibcpContext *ctx, HashMap *map, char *key, int keylen);

//
// Shared helpers (strings.c / tokenize.c)
//

bool libcp_file_exists(char *path);
void libcp_print_escaped(FILE *out, char *p, int len);
char *libcp_token_kind_name(LibcpContext *ctx, TokenKind kind);

//
// context / arena / diagnostics (PLAN L2)
//

typedef struct JmpFrame JmpFrame;
typedef struct ArenaBlock ArenaBlock;

// The error boundary: every public entry pushes one, and error_* longjmps
// to the innermost frame. The driver pushes one around codegen too.
struct JmpFrame {
  JmpFrame *prev;
  jmp_buf buf;
};

// The boundary prologue / epilogue for a public entry. ENTER_RET returns
// `fail` from the enclosing function on error; ENTER_VOID returns void.
// The frame belongs to the context in hand; no other state is saved or
// restored. Used by lib.c's facade entries and by the phase functions
// that carry their public name directly (PLAN-pratt-default S2).
#define ENTER_RET(ctx, fail)                                              \
  JmpFrame _jf;                                                           \
  _jf.prev = (ctx)->jmp_top;                                              \
  (ctx)->jmp_top = &_jf;                                                  \
  if (setjmp(_jf.buf)) {                                                  \
    (ctx)->jmp_top = _jf.prev;                                            \
    return fail;                                                          \
  }

#define ENTER_VOID(ctx)                                                   \
  JmpFrame _jf;                                                           \
  _jf.prev = (ctx)->jmp_top;                                              \
  (ctx)->jmp_top = &_jf;                                                  \
  if (setjmp(_jf.buf)) {                                                  \
    (ctx)->jmp_top = _jf.prev;                                            \
    return;                                                               \
  }

#define LEAVE(ctx)                                                        \
  do {                                                                    \
    (ctx)->jmp_top = _jf.prev;                                            \
  } while (0)

struct ArenaBlock {
  ArenaBlock *next;
  size_t used;
  size_t cap;
  char *data;
};

// The goto / label-value references sema's check pass collects, keyed by
// the function they belong to. The handoff lives in the context, not on
// the public Obj (PLAN L3.1b).
typedef struct LabelGotoList LabelGotoList;
struct LabelGotoList {
  LabelGotoList *next;
  Obj *fn;
  NodeArray refs;
};

struct CondIncl;
struct ParseScope;
struct SemaScope;

struct LibcpContext {
  // Arena for everything this translation unit allocates.
  ArenaBlock *arena;

  // Configuration. Survives reset, owned by malloc, freed by
  // libcp_context_free.
  char **include_paths;
  int include_paths_len;
  int include_paths_cap;
  // Command-line macro operations in order: "D:name=body" or "U:name".
  char **macro_ops;
  int macro_ops_len;
  int macro_ops_cap;
  bool predefined_macros;
  LibcpDiagnosticFn diag_fn;
  void *diag_user;
  JmpFrame *jmp_top;
  bool failed;

  // Per-TU state (PLAN L2.2-L2.5). These were file-scope statics.
  char *base_file;
  File *current_file;
  File **input_files;
  int input_files_len;
  int input_files_cap;
  int next_file_no;

  // The text of the first "-" read, so a second tokenize of stdin sees
  // the same input instead of the sticky EOF's empty stream (C12).
  char *stdin_text;
  size_t stdin_len;
  bool at_bol;
  bool has_space;

  HashMap macros;
  LibcpPredefinedMacro *predefined;
  int predefined_len;
  struct CondIncl *cond_incl;
  HashMap pragma_once;
  HashMap search_cache;
  HashMap include_guards;
  HashMap keyword_map;
  HashMap typename_map;
  int counter;
  int include_next_idx;
  int include_count;
  int parse_depth;

  struct ParseScope *scope;
  struct SemaScope *sema_scope;

  Obj *locals;
  Obj *globals;
  Obj *sema_fn;
  Obj *builtin_alloca;
  NodeArray gotos;
  NodeArray labels;
  LabelGotoList *label_gotos;
  int loop_depth;
  int switch_depth;
  int unique_name_id;
  bool resolving_body;
};

// The base file name is per-TU state, not configuration: it is set from
// the input path when tokenizing starts and cleared by reset, unlike the
// include paths and macro operations, which survive reset.
void libcp_context_set_base_file(LibcpContext *ctx, char *path);

void *libcp_arena_alloc(LibcpContext *ctx, size_t size, size_t align);
void *libcp_arena_calloc(LibcpContext *ctx, size_t count, size_t size);
char *libcp_arena_strdup(LibcpContext *ctx, char *s);
char *libcp_arena_strndup(LibcpContext *ctx, char *s, size_t n);
void *libcp_arena_realloc(LibcpContext *ctx, void *old, size_t old_size, size_t new_size);
