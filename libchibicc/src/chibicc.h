// Internal umbrella (PLAN L3.1). The public types and declarations live
// under libchibicc/include/chibicc/; this header is what the
// implementation includes. Everything below the public include is
// internal to the library: the context layout, the arena, the helper
// containers, and the phase functions the facade wraps.
#define CHIBICC_INTERNAL 1

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

#include "chibicc/chibicc.h"

// The public surface uses the Chibicc-prefixed names only (PLAN L3.2a);
// the implementation keeps the short spellings through these aliases.
typedef ChibiccToken Token;
typedef ChibiccFile File;
typedef ChibiccTokenKind TokenKind;
typedef ChibiccHideset Hideset;
typedef ChibiccMacroProv MacroProv;
typedef ChibiccMacroExpansion MacroExpansion;
typedef ChibiccCstNode CstNode;
typedef ChibiccCstKind CstKind;
typedef ChibiccStringArray StringArray;
typedef ChibiccObj Obj;
typedef ChibiccRelocation Relocation;
typedef ChibiccVarAttr VarAttr;
typedef ChibiccInitializer Initializer;
typedef ChibiccInitItem InitItem;
typedef ChibiccInitDesig InitDesig;
typedef ChibiccInitTree InitTree;
typedef ChibiccInitPath InitPath;
typedef ChibiccType Type;
typedef ChibiccMember Member;
typedef ChibiccNode Node;
typedef ChibiccNodeKind NodeKind;
typedef ChibiccTypeKind TypeKind;

// The Pratt expression parser (PLAN-pratt-parse). 0 keeps the cascaded
// expression functions; 1 compiles the Pratt implementation instead.
// The default build is the old path; `make chibicc-pratt` passes 1.
#ifndef CHIBICC_PRATT_PARSE_FEATURE
#define CHIBICC_PRATT_PARSE_FEATURE 0
#endif

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

void strarray_push(ChibiccContext *ctx, StringArray *arr, char *s);
void nodearray_push(ChibiccContext *ctx, NodeArray *arr, Node *node);
char *format(ChibiccContext *ctx, char *fmt, ...) __attribute__((format(printf, 2, 3)));

//
// tokenize.c
//

noreturn void error(ChibiccContext *ctx, char *fmt, ...) __attribute__((format(printf, 2, 3)));
noreturn void error_at(ChibiccContext *ctx, char *loc, char *fmt, ...) __attribute__((format(printf, 3, 4)));
noreturn void error_tok(ChibiccContext *ctx, Token *tok, char *fmt, ...) __attribute__((format(printf, 3, 4)));
void warn_tok(ChibiccContext *ctx, Token *tok, char *fmt, ...) __attribute__((format(printf, 3, 4)));
bool equal(Token *tok, char *op);
Token *skip(ChibiccContext *ctx, Token *tok, char *op);
bool consume(Token **rest, Token *tok, char *str);
void convert_pp_tokens(ChibiccContext *ctx, Token *tok);
File **get_input_files(ChibiccContext *ctx);
File *new_file(ChibiccContext *ctx, char *name, int file_no, char *contents);
File *new_file_len(ChibiccContext *ctx, char *name, int file_no, char *contents, int len);
bool is_line_break(char *p);
int line_break_len(char *p);
Token *tokenize_string_literal(ChibiccContext *ctx, Token *tok, Type *basety);
Token *tokenize(ChibiccContext *ctx, File *file);
Token *tokenize_file(ChibiccContext *ctx, char *filename);

#define unreachable() \
  error(chibicc_ctx(), "internal error at %s:%d", __FILE__, __LINE__)

//
// cst.c
//

CstNode *cst_parse(ChibiccContext *ctx, Token *tok);
void cst_reconstruct(CstNode *root, FILE *out);
void cst_dump(ChibiccContext *ctx, CstNode *root, FILE *out);
bool cst_verify(CstNode *root, Token *tok);

//
// preprocess.c
//

char *search_include_paths(ChibiccContext *ctx, char *filename);
void init_macros(ChibiccContext *ctx);
void define_macro(ChibiccContext *ctx, char *name, char *buf);
void undef_macro(ChibiccContext *ctx, char *name);
Token *preprocess(ChibiccContext *ctx, Token *tok);

MacroProv *token_provenance(Token *tok);
MacroProv *macro_prov_next(MacroProv *p);
MacroExpansion *macro_prov_expansion(MacroProv *p);
int macro_prov_arg_index(MacroProv *p);

Token *macro_expansion_name(MacroExpansion *e);
Token *macro_expansion_end(MacroExpansion *e);   // rparen; name if none
char *macro_expansion_macro(MacroExpansion *e);  // the macro's spelling
Token *macro_expansion_def(MacroExpansion *e);   // #define name; NULL if none
Token *macro_expansion_arg(MacroExpansion *e, int i);
int macro_expansion_arg_count(MacroExpansion *e);
bool macro_expansion_is_builtin(MacroExpansion *e);

//
// provenance.c
//

void expansion_dump(ChibiccContext *ctx, Token *tok, FILE *out);
bool expansion_verify(Token *tok);

//
// parse.c
//

// Constructors for the faithful-tree nodes. Defined in parse.c; sema
// uses them to build the nodes its lowerings introduce. The shapes that
// only a lowering produces - a bound name, a VLA designator, a numeric
// literal typed at birth - sema constructs itself.
Node *new_node(ChibiccContext *ctx, NodeKind kind, Token *tok);
Node *new_binary(ChibiccContext *ctx, NodeKind kind, Node *lhs, Node *rhs, Token *tok);
Node *new_unary(ChibiccContext *ctx, NodeKind kind, Node *expr, Token *tok);
Node *new_num(ChibiccContext *ctx, int64_t val, Token *tok);

// Identifier spelling, used by sema when it declares the objects the
// parser's declaration records name.
char *get_ident(ChibiccContext *ctx, Token *tok);

Node *parse(ChibiccContext *ctx, Token *tok);
void parse_init(ChibiccContext *ctx);
void sema_store_label_gotos(ChibiccContext *ctx, Obj *fn, NodeArray gotos);
NodeArray *sema_label_gotos(ChibiccContext *ctx, Obj *fn);
void sema_state_clear(ChibiccContext *ctx);

//
// sema.c
//

Obj *sema(ChibiccContext *ctx, Node *toplevel);

// Parses and evaluates a constant expression. Used by the
// preprocessor for `#if`; not part of the parse/sema pipeline below.
int64_t const_expr(ChibiccContext *ctx, Token **rest, Token *tok);

// The parser's conditional-expression entry point (parse.c), which
// const_expr uses to read the expression it evaluates.
Node *conditional(ChibiccContext *ctx, Token **rest, Token *tok);

// The type-level tools a consumer needs in order to build nodes that
// arrive already typed: the annotation entry point, the constructors for
// the shapes only sema produces, and the conversions that carry sema's
// decisions.
void add_type(ChibiccContext *ctx, Node *node);
Node *new_arith(ChibiccContext *ctx, NodeKind kind, Node *lhs, Node *rhs, Token *tok);
void usual_arith_conv(ChibiccContext *ctx, Node **lhs, Node **rhs);
Type *get_common_type(ChibiccContext *ctx, Type *ty1, Type *ty2);
Node *new_cast(ChibiccContext *ctx, Node *expr, Type *ty);
Node *new_long(ChibiccContext *ctx, int64_t val, Token *tok);
Node *new_ulong(ChibiccContext *ctx, long val, Token *tok);
Node *new_var_node(ChibiccContext *ctx, Obj *var, Token *tok);
Node *new_vla_ptr(ChibiccContext *ctx, Obj *var, Token *tok);
Node *new_alloca(ChibiccContext *ctx, Node *sz);

// The next value the anonymous-name counter would hand out, without
// consuming it.
int unique_name_next(ChibiccContext *ctx);

//
// type.c
//

extern Type *ty_void;
extern Type *ty_bool;

extern Type *ty_char;
extern Type *ty_short;
extern Type *ty_int;
extern Type *ty_long;

extern Type *ty_uchar;
extern Type *ty_ushort;
extern Type *ty_uint;
extern Type *ty_ulong;

extern Type *ty_float;
extern Type *ty_double;
extern Type *ty_ldouble;

bool is_integer(Type *ty);
bool is_flonum(Type *ty);
bool is_numeric(Type *ty);
bool is_compatible(Type *t1, Type *t2);
Type *copy_type(ChibiccContext *ctx, Type *ty);
Type *pointer_to(ChibiccContext *ctx, Type *base);
Type *func_type(ChibiccContext *ctx, Type *return_ty);
Type *array_of(ChibiccContext *ctx, Type *base, int size);
Type *vla_of(ChibiccContext *ctx, Type *base, Node *expr);
Type *array_of_dim(ChibiccContext *ctx, Type *base, Node *dim);
Type *typeof_placeholder(ChibiccContext *ctx);
Type *enum_type(ChibiccContext *ctx);
Type *struct_type(ChibiccContext *ctx);

//
// strings.c
//

int align_to(int n, int align);

//
// unicode.c
//

int encode_utf8(char *buf, uint32_t c);
uint32_t decode_utf8(ChibiccContext *ctx, char **new_pos, char *p);
bool is_ident1(uint32_t c);
bool is_ident2(uint32_t c);
int display_width(ChibiccContext *ctx, char *p, int len);

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

void *hashmap_get(HashMap *map, char *key);
void *hashmap_get2(HashMap *map, char *key, int keylen);
void hashmap_put(ChibiccContext *ctx, HashMap *map, char *key, void *val);
void hashmap_put2(ChibiccContext *ctx, HashMap *map, char *key, int keylen, void *val);
void hashmap_delete(ChibiccContext *ctx, HashMap *map, char *key);
void hashmap_delete2(ChibiccContext *ctx, HashMap *map, char *key, int keylen);

//
// Shared helpers (strings.c / tokenize.c)
//

bool file_exists(char *path);
void print_escaped(FILE *out, char *p, int len);
char *token_kind_name(ChibiccContext *ctx, TokenKind kind);

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

struct ChibiccContext {
  // Arena for everything this translation unit allocates.
  ArenaBlock *arena;

  // Configuration. Survives reset, owned by malloc, freed by
  // chibicc_context_free.
  char **include_paths;
  int include_paths_len;
  int include_paths_cap;
  // Command-line macro operations in order: "D:name=body" or "U:name".
  char **macro_ops;
  int macro_ops_len;
  int macro_ops_cap;
  bool predefined_macros;
  ChibiccDiagnosticFn diag_fn;
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
  ChibiccPredefinedMacro *predefined;
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

extern _Thread_local ChibiccContext *chibicc_active_ctx;
ChibiccContext *chibicc_ctx(void);

void chibicc_context_activate(ChibiccContext *ctx);
void chibicc_context_set_base_file(ChibiccContext *ctx, char *path);

void *chibicc_arena_alloc(ChibiccContext *ctx, size_t size, size_t align);
void *chibicc_arena_calloc(ChibiccContext *ctx, size_t count, size_t size);
char *chibicc_arena_strdup(ChibiccContext *ctx, char *s);
char *chibicc_arena_strndup(ChibiccContext *ctx, char *s, size_t n);
void *chibicc_arena_realloc(ChibiccContext *ctx, void *old, size_t old_size, size_t new_size);
