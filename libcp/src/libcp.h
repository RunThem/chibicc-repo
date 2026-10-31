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

// Every type the library defines carries the Libcp prefix (2026-11-01);
// the implementation uses those names directly, with no short aliases.

#define MAX(x, y) ((x) < (y) ? (y) : (x))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#ifndef __GNUC__
# define __attribute__(x)
#endif

//
// strings.c
//

typedef struct {
  LibcpNode **data;
  int capacity;
  int len;
} LibcpNodeArray;

void libcp_strarray_push(LibcpContext *ctx, LibcpStringArray *arr, char *s);
void libcp_nodearray_push(LibcpContext *ctx, LibcpNodeArray *arr, LibcpNode *node);
char *libcp_format(LibcpContext *ctx, char *fmt, ...) __attribute__((format(printf, 2, 3)));

//
// tokenize.c
//

noreturn void libcp_error(LibcpContext *ctx, char *fmt, ...) __attribute__((format(printf, 2, 3)));
noreturn void libcp_error_at(LibcpContext *ctx, char *loc, char *fmt, ...) __attribute__((format(printf, 3, 4)));
noreturn void libcp_error_tok(LibcpContext *ctx, LibcpToken *tok, char *fmt, ...) __attribute__((format(printf, 3, 4)));
void libcp_warn_tok(LibcpContext *ctx, LibcpToken *tok, char *fmt, ...) __attribute__((format(printf, 3, 4)));
bool libcp_equal(LibcpToken *tok, char *op);
LibcpToken *libcp_skip(LibcpContext *ctx, LibcpToken *tok, char *op);
bool libcp_consume(LibcpToken **rest, LibcpToken *tok, char *str);
void libcp_convert_pp_tokens(LibcpContext *ctx, LibcpToken *tok);
LibcpFile **libcp_get_input_files(LibcpContext *ctx);
LibcpFile *libcp_new_file(LibcpContext *ctx, char *name, int file_no, char *contents);
LibcpFile *libcp_new_file_len(LibcpContext *ctx, char *name, int file_no, char *contents, int len);
bool libcp_is_line_break(char *p);
int libcp_line_break_len(char *p);
LibcpToken *libcp_tokenize_string_literal(LibcpContext *ctx, LibcpToken *tok, LibcpType *basety);
LibcpToken *libcp_tokenize(LibcpContext *ctx, LibcpFile *file);
LibcpToken *libcp_tokenize_file_impl(LibcpContext *ctx, char *filename);

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
LibcpNode *libcp_new_node(LibcpContext *ctx, LibcpNodeKind kind, LibcpToken *tok);
LibcpNode *libcp_new_binary(LibcpContext *ctx, LibcpNodeKind kind, LibcpNode *lhs, LibcpNode *rhs, LibcpToken *tok);
LibcpNode *libcp_new_unary(LibcpContext *ctx, LibcpNodeKind kind, LibcpNode *expr, LibcpToken *tok);
LibcpNode *libcp_new_num(LibcpContext *ctx, int64_t val, LibcpToken *tok);

// Identifier spelling, used by sema when it declares the objects the
// parser's declaration records name.
char *libcp_get_ident(LibcpContext *ctx, LibcpToken *tok);

void libcp_parse_init(LibcpContext *ctx);
void libcp_sema_store_label_gotos(LibcpContext *ctx, LibcpObj *fn, LibcpNodeArray gotos);
LibcpNodeArray *libcp_sema_label_gotos(LibcpContext *ctx, LibcpObj *fn);
void libcp_sema_state_clear(LibcpContext *ctx);

//
// sema.c
//

// Parses and evaluates a constant expression. Used by the
// preprocessor for `#if`; not part of the parse/sema pipeline below.
int64_t libcp_const_expr(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok);

// The parser's conditional-expression entry point (parse.c), which
// libcp_const_expr uses to read the expression it evaluates.
LibcpNode *libcp_conditional(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok);

// The type-level tools a consumer needs in order to build nodes that
// arrive already typed: the annotation entry point, the constructors for
// the shapes only sema produces, and the conversions that carry sema's
// decisions.
void libcp_add_type(LibcpContext *ctx, LibcpNode *node);
LibcpNode *libcp_new_arith(LibcpContext *ctx, LibcpNodeKind kind, LibcpNode *lhs, LibcpNode *rhs, LibcpToken *tok);
void libcp_usual_arith_conv(LibcpContext *ctx, LibcpNode **lhs, LibcpNode **rhs);
LibcpType *libcp_get_common_type(LibcpContext *ctx, LibcpType *ty1, LibcpType *ty2);
LibcpNode *libcp_new_cast(LibcpContext *ctx, LibcpNode *expr, LibcpType *ty);
LibcpNode *libcp_new_long(LibcpContext *ctx, int64_t val, LibcpToken *tok);
LibcpNode *libcp_new_ulong(LibcpContext *ctx, long val, LibcpToken *tok);
LibcpNode *libcp_new_var_node(LibcpContext *ctx, LibcpObj *var, LibcpToken *tok);
LibcpNode *libcp_new_vla_ptr(LibcpContext *ctx, LibcpObj *var, LibcpToken *tok);
LibcpNode *libcp_new_alloca(LibcpContext *ctx, LibcpNode *sz);

// The next value the anonymous-name counter would hand out, without
// consuming it.
int libcp_unique_name_next(LibcpContext *ctx);

//
// type.c
//

extern LibcpType *libcp_ty_void;
extern LibcpType *libcp_ty_bool;

extern LibcpType *libcp_ty_char;
extern LibcpType *libcp_ty_short;
extern LibcpType *libcp_ty_int;
extern LibcpType *libcp_ty_long;

extern LibcpType *libcp_ty_uchar;
extern LibcpType *libcp_ty_ushort;
extern LibcpType *libcp_ty_uint;
extern LibcpType *libcp_ty_ulong;

extern LibcpType *libcp_ty_float;
extern LibcpType *libcp_ty_double;
extern LibcpType *libcp_ty_ldouble;

bool libcp_is_integer(LibcpType *ty);
bool libcp_is_flonum(LibcpType *ty);
bool libcp_is_numeric(LibcpType *ty);
bool libcp_is_compatible(LibcpType *t1, LibcpType *t2);
LibcpType *libcp_copy_type(LibcpContext *ctx, LibcpType *ty);
LibcpType *libcp_pointer_to(LibcpContext *ctx, LibcpType *base);
LibcpType *libcp_func_type(LibcpContext *ctx, LibcpType *return_ty);
LibcpType *libcp_array_of(LibcpContext *ctx, LibcpType *base, int size);
LibcpType *libcp_vla_of(LibcpContext *ctx, LibcpType *base, LibcpNode *expr);
LibcpType *libcp_array_of_dim(LibcpContext *ctx, LibcpType *base, LibcpNode *dim);
LibcpType *libcp_typeof_placeholder(LibcpContext *ctx);
LibcpType *libcp_enum_type(LibcpContext *ctx);
LibcpType *libcp_struct_type(LibcpContext *ctx);

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
} LibcpHashEntry;

typedef struct {
  LibcpHashEntry *buckets;
  int capacity;
  int used;     // live entries plus tombstones
  int ndeleted; // tombstones in `used`
} LibcpHashMap;

void *libcp_hashmap_get(LibcpContext *ctx, LibcpHashMap *map, char *key);
void *libcp_hashmap_get2(LibcpContext *ctx, LibcpHashMap *map, char *key, int keylen);
void libcp_hashmap_put(LibcpContext *ctx, LibcpHashMap *map, char *key, void *val);
void libcp_hashmap_put2(LibcpContext *ctx, LibcpHashMap *map, char *key, int keylen, void *val);
void libcp_hashmap_delete(LibcpContext *ctx, LibcpHashMap *map, char *key);
void libcp_hashmap_delete2(LibcpContext *ctx, LibcpHashMap *map, char *key, int keylen);

//
// Shared helpers (strings.c / tokenize.c)
//

bool libcp_file_exists(char *path);
void libcp_print_escaped(FILE *out, char *p, int len);
char *libcp_token_kind_name(LibcpContext *ctx, LibcpTokenKind kind);

//
// context / arena / diagnostics (PLAN L2)
//

typedef struct libcp_jmp_frame LibcpJmpFrame;
typedef struct libcp_arena_block LibcpArenaBlock;

// The error boundary: every public entry pushes one, and error_* longjmps
// to the innermost frame. The driver pushes one around codegen too.
struct libcp_jmp_frame {
  LibcpJmpFrame *prev;
  jmp_buf buf;
};

// The boundary prologue / epilogue for a public entry. ENTER_RET returns
// `fail` from the enclosing function on error; ENTER_VOID returns void.
// The frame belongs to the context in hand; no other state is saved or
// restored. Used by lib.c's facade entries and by the phase functions
// that carry their public name directly (PLAN-pratt-default S2).
#define ENTER_RET(ctx, fail)                                              \
  LibcpJmpFrame _jf;                                                      \
  _jf.prev = (ctx)->jmp_top;                                              \
  (ctx)->jmp_top = &_jf;                                                  \
  if (setjmp(_jf.buf)) {                                                  \
    (ctx)->jmp_top = _jf.prev;                                            \
    return fail;                                                          \
  }

#define ENTER_VOID(ctx)                                                   \
  LibcpJmpFrame _jf;                                                      \
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

struct libcp_arena_block {
  LibcpArenaBlock *next;
  size_t used;
  size_t cap;
  char *data;
};

// The goto / label-value references sema's check pass collects, keyed by
// the function they belong to. The handoff lives in the context, not on
// the public LibcpObj (PLAN L3.1b).
typedef struct libcp_label_goto_list LibcpLabelGotoList;
struct libcp_label_goto_list {
  LibcpLabelGotoList *next;
  LibcpObj *fn;
  LibcpNodeArray refs;
};

typedef struct libcp_cond_incl LibcpCondIncl;
typedef struct libcp_parse_scope LibcpParseScope;
typedef struct libcp_sema_scope LibcpSemaScope;

struct LibcpContext {
  // Arena for everything this translation unit allocates.
  LibcpArenaBlock *arena;

  // Configuration. Survives reset, owned by malloc, freed by
  // libcp_context_free.
  char **include_paths;
  int include_paths_len;
  int include_paths_cap;
  // Command-line macro operations in order: "D:name=body" or "U:name".
  char **macro_ops;
  int macro_ops_len;
  int macro_ops_cap;
  // Typedef names the consumer seeded for the CST oracle (PLAN-l5 W2):
  // names that arrive through included headers, which the
  // pre-preprocess typedef scan cannot see. Survives reset like the
  // other configuration.
  char **cst_seeds;
  int cst_seeds_len;
  int cst_seeds_cap;
  bool predefined_macros;
  LibcpDiagnosticFn diag_fn;
  void *diag_user;
  LibcpJmpFrame *jmp_top;
  bool failed;

  // Per-TU state (PLAN L2.2-L2.5). These were file-scope statics.
  char *base_file;
  LibcpFile *current_file;
  LibcpFile **input_files;
  int input_files_len;
  int input_files_cap;
  int next_file_no;

  // The text of the first "-" read, so a second tokenize of stdin sees
  // the same input instead of the sticky EOF's empty stream (C12).
  char *stdin_text;
  size_t stdin_len;
  bool at_bol;
  bool has_space;

  LibcpHashMap macros;
  LibcpPredefinedMacro *predefined;
  int predefined_len;
  LibcpCondIncl *cond_incl;
  LibcpHashMap pragma_once;
  LibcpHashMap search_cache;
  LibcpHashMap include_guards;
  LibcpHashMap keyword_map;
  LibcpHashMap typename_map;
  int counter;
  int include_next_idx;
  int include_count;
  int parse_depth;

  LibcpParseScope *scope;
  LibcpSemaScope *sema_scope;

  LibcpObj *locals;
  LibcpObj *globals;
  LibcpObj *sema_fn;
  LibcpObj *builtin_alloca;
  LibcpNodeArray gotos;
  LibcpNodeArray labels;
  LibcpLabelGotoList *label_gotos;
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
