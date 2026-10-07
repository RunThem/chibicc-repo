#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <libgen.h>
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

#define MAX(x, y) ((x) < (y) ? (y) : (x))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#ifndef __GNUC__
# define __attribute__(x)
#endif

typedef struct Type Type;
typedef struct Node Node;
typedef struct Member Member;
typedef struct Relocation Relocation;
typedef struct Hideset Hideset;

//
// strings.c
//

typedef struct {
  char **data;
  int capacity;
  int len;
} StringArray;

void strarray_push(StringArray *arr, char *s);
char *format(char *fmt, ...) __attribute__((format(printf, 1, 2)));

//
// tokenize.c
//

// Token
typedef enum {
  TK_IDENT,   // Identifiers
  TK_PUNCT,   // Punctuators
  TK_KEYWORD, // Keywords
  TK_STR,     // String literals
  TK_NUM,     // Numeric literals
  TK_PP_NUM,  // Preprocessing numbers
  TK_EOF,     // End-of-file markers
} TokenKind;

typedef struct {
  char *name;
  int file_no;
  char *contents;

  // For #line directive
  char *display_name;
  int line_delta;
} File;

// Token type
typedef struct Token Token;
struct Token {
  TokenKind kind;   // Token kind
  Token *next;      // Next token
  int64_t val;      // If kind is TK_NUM, its value
  long double fval; // If kind is TK_NUM, its value
  char *loc;        // Token location
  int len;          // Token length
  Type *ty;         // Used if TK_NUM or TK_STR
  char *str;        // String literal contents including terminating '\0'

  File *file;       // Source location
  char *filename;   // Filename
  int line_no;      // Line number
  int line_delta;   // Line number
  bool at_bol;      // True if this token is at beginning of line
  bool has_space;   // True if this token follows a space character
  Hideset *hideset; // For macro expansion
  Token *origin;    // If this is expanded from a macro, the original token
};

noreturn void error(char *fmt, ...) __attribute__((format(printf, 1, 2)));
noreturn void error_at(char *loc, char *fmt, ...) __attribute__((format(printf, 2, 3)));
noreturn void error_tok(Token *tok, char *fmt, ...) __attribute__((format(printf, 2, 3)));
void warn_tok(Token *tok, char *fmt, ...) __attribute__((format(printf, 2, 3)));
bool equal(Token *tok, char *op);
Token *skip(Token *tok, char *op);
bool consume(Token **rest, Token *tok, char *str);
void convert_pp_tokens(Token *tok);
File **get_input_files(void);
File *new_file(char *name, int file_no, char *contents);
Token *tokenize_string_literal(Token *tok, Type *basety);
Token *tokenize(File *file);
Token *tokenize_file(char *filename);

#define unreachable() \
  error("internal error at %s:%d", __FILE__, __LINE__)

//
// preprocess.c
//

char *search_include_paths(char *filename);
void init_macros(void);
void define_macro(char *name, char *buf);
void undef_macro(char *name);
Token *preprocess(Token *tok);

//
// parse.c
//

// Variable or function
typedef struct Obj Obj;
struct Obj {
  Obj *next;
  char *name;    // Variable name
  Type *ty;      // Type
  Token *tok;    // representative token
  bool is_local; // local or global/function
  int align;     // alignment

  // Local variable
  int offset;

  // Global variable or function
  bool is_function;
  bool is_definition;
  bool is_static;

  // Global variable
  bool is_tentative;
  bool is_tls;
  char *init_data;
  Relocation *rel;

  // Function
  bool is_inline;
  Obj *params;
  Node *body;
  Obj *locals;
  Obj *va_area;
  Obj *alloca_bottom;
  int stack_size;

  // Per function: the goto and [GNU] label-value references of this
  // body, chained via goto_next. sema's check pass collects them (a
  // label value can sit outside the statement tree, in a block-scope
  // static initializer, which its own descent cannot reach) and pairs
  // them by name for the undeclared-label check; codegen's shaping
  // pass resolves them against the labels it allocates. Written by
  // sema, read once by codegen.
  Node *label_gotos;

  // Static inline function
  bool is_live;
  bool is_root;
  StringArray refs;
};

// Global variable can be initialized either by a constant expression
// or a pointer to another global variable. This struct represents the
// latter.
typedef struct Relocation Relocation;
struct Relocation {
  Relocation *next;
  int offset;
  char **label;
  long addend;
};

// Declaration attributes collected by declspec: storage class and
// _Alignas. The parser fills this in and copies it onto the declaration
// record nodes; sema consumes it when it declares the object.
typedef struct {
  bool is_typedef;
  bool is_static;
  bool is_extern;
  bool is_inline;
  bool is_tls;
  int align;
  // A `_Alignas(<expr>)` argument, of which `align_ty` is the
  // `_Alignas(<type>)` form. Whichever comes last is the one that
  // counts; sema reads the type form's alignment off a completed type.
  Node *align_expr;
  Type *align_ty;
} VarAttr;

// Faithful initializer record (parse.c): the syntactic shape of an
// initializer as written - a braced item sequence, a string literal or
// a single expression, with designators recorded unevaluated. sema
// resolves designators, applies brace elision and lowers the record
// when it types the carrying node (ND_DECL, compound literal) or when
// a global variable definition is parsed.
typedef struct Initializer Initializer;
typedef struct InitItem InitItem;
typedef struct InitDesig InitDesig;

enum { INIT_LIST, INIT_EXPR, INIT_STR };

struct Initializer {
  int kind;     // INIT_LIST, INIT_EXPR or INIT_STR
  Token *tok;   // first token of the initializer source
  InitItem *items;  // INIT_LIST: braced element sequence
  Node *expr;       // INIT_EXPR: scalar initialization expression
  Token *str_tok;   // INIT_STR: string literal token

  // Only for the top-level record of an ND_DECL: the `=` that
  // introduced it, which is where sema anchors a diagnostic that
  // rejects the declaration as a whole.
  Token *eq_tok;
};

// One element of a braced initializer list.
struct InitItem {
  InitItem *next;
  Token *comma_tok;    // the `,` before this element (NULL for the first)
  InitDesig *desigs;   // designator chain as written (may be NULL)
  Initializer *init;   // the element value
};

// One designator as written: `[expr]`, `[begin ... end]` or `.name`.
// Index expressions are recorded unevaluated; sema evaluates them.
struct InitDesig {
  InitDesig *next;
  Token *tok;           // `[` or `.`
  Token *after_begin;   // token after the begin expression (`...` or `]`)
  Token *rbracket;      // the `]` token
  Node *begin;          // array designator index
  Node *end;            // range end (NULL unless `[begin ... end]`)
  Token *name;          // member designator name
};

// AST node
typedef enum {
  ND_NULL_EXPR, // Do nothing
  ND_ADD,       // +
  ND_SUB,       // -
  ND_MUL,       // *
  ND_DIV,       // /
  ND_NEG,       // unary -
  ND_MOD,       // %
  ND_BITAND,    // &
  ND_BITOR,     // |
  ND_BITXOR,    // ^
  ND_SHL,       // <<
  ND_SHR,       // >>
  ND_EQ,        // ==
  ND_NE,        // !=
  ND_LT,        // <
  ND_LE,        // <=
  ND_GT,        // >
  ND_GE,        // >=
  ND_ASSIGN,    // =
  ND_INCDEC,    // "++" and "--"
  ND_SUBSCRIPT, // x[y]
  ND_COND,      // ?:
  ND_COMMA,     // ,
  ND_MEMBER,    // . (struct member access)
  ND_ADDR,      // unary &
  ND_DEREF,     // unary *
  ND_NOT,       // !
  ND_BITNOT,    // ~
  ND_LOGAND,    // &&
  ND_LOGOR,     // ||
  ND_RETURN,    // "return"
  ND_IF,        // "if"
  ND_FOR,       // "for" (also the lowered shape of "while")
  ND_WHILE,     // "while"; sema lowers it to ND_FOR
  ND_DO,        // "do"
  ND_SWITCH,    // "switch"
  ND_CASE,      // "case"
  ND_BLOCK,     // { ... }
  ND_GOTO,      // "goto"
  ND_BREAK,     // "break"; sema lowers it to a jump to the break label
  ND_CONTINUE,  // "continue"; sema lowers it to a jump to the continue label
  ND_GOTO_EXPR, // "goto" labels-as-values
  ND_LABEL,     // Labeled statement
  ND_LABEL_VAL, // [GNU] Labels-as-values
  ND_FUNCALL,   // Function call
  ND_EXPR_STMT, // Expression statement
  ND_STMT_EXPR, // Statement expression
  ND_IDENT,     // An unresolved name; sema binds it to a variable,
                // a function or an enum constant
  ND_VAR,       // Variable
  ND_VLA_PTR,   // VLA designator
  ND_NUM,       // Integer
  ND_STRING,    // String literal; sema lowers it to an anonymous global
  ND_SIZEOF,    // "sizeof"; sema folds it to its value
  ND_ALIGNOF,   // "_Alignof"; sema folds it to its value
  ND_GENERIC,   // "_Generic"; sema selects an association and becomes it
  ND_GENERIC_ASSOC, // One association of an ND_GENERIC: the type name in
                    // ty_op (NULL for "default:"), the result expression in
                    // lhs. A carrier only; it never reaches codegen.
  ND_TYPES_COMPATIBLE, // "__builtin_types_compatible_p"; sema folds it to 0 or 1
  ND_REG_CLASS,        // "__builtin_reg_class"; sema folds it to its class
  ND_CAST,      // Type cast. The parser leaves an explicit cast untyped
                // with its target type in ty_op; sema resolves and types
                // the node. Casts sema inserts are fully typed at birth
                // and marked is_implicit.
  ND_MEMZERO,   // Zero-clear a stack variable
  ND_DECL,      // Declaration of a local variable: the name token on its
                // type record, the attributes in `attr` and the faithful
                // initializer record. sema declares the variable (pass 1)
                // and lowers the node to statements (pass 2).
  ND_GVAR_DECL, // Declaration record of a global variable (file scope, or
                // an `extern`/tentative declaration inside a block). sema
                // declares the variable, serializes its initializer and
                // removes the record from any statement chain.
  ND_FUNCDEF,   // Declaration record of a function; `body` is non-NULL for
                // a definition. sema declares the function, runs both
                // passes over the body and removes the record from any
                // statement chain (a block-scope prototype or a [GNU]
                // nested definition).
  ND_TYPEDEF,   // Typedef declaration record; sema resolves its type and
                // removes it from any statement chain.
  ND_ENUM_CONST, // Enum constant declaration (member name + optional value
                // expression). sema evaluates and registers the name, and
                // removes the record from any statement chain.
  ND_COMPOUND_LITERAL, // "(type){...}": the type in ty_op, the faithful
                // initializer record in decl_init. sema materializes the
                // hidden variable and lowers the node.
  ND_ASM,       // "asm"
  ND_CAS,       // Atomic compare-and-swap
  ND_EXCH,      // Atomic exchange
} NodeKind;

// AST node type
struct Node {
  NodeKind kind; // Node kind
  Node *next;    // Next node
  Type *ty;      // Type, e.g. int or pointer to int
  Token *tok;    // Representative token

  Node *lhs;     // Left-hand side
  Node *rhs;     // Right-hand side

  // Compound assignment operator for ND_ASSIGN ("+=", "-=", ...).
  // 0 means a plain "=".
  NodeKind op;

  // Increment/decrement for ND_INCDEC: postfix (`i++`) or prefix
  // (`++i`), with an addend of +1 or -1. sema lowers the node to a
  // compound assignment.
  bool is_post;
  int addend;

  // "if" or "for" statement
  Node *cond;
  Node *then;
  Node *els;
  Node *init;
  Node *inc;

  // "break" and "continue" labels, allocated by sema's analyze pass
  // when it descends into the loop or the switch.
  char *brk_label;
  char *cont_label;

  // Block or statement expression
  Node *body;

  // ND_BLOCK: true for a compound-statement block, which is a lexical
  // scope; the empty statement and the block sema folds a `for`
  // init-declaration chain into are ND_BLOCK nodes without one. sema's
  // resolve pass derives its scope stack from this flag.
  bool is_scope_block;

  // Struct member access. The parser leaves it unbound: an unresolved
  // ND_MEMBER carries the member name in `tok`, and sema's add_type
  // looks the name up. `arrow_tok` is the `->` token of a pointer
  // access and NULL for a plain `.`; it is the anchor of the checks on
  // the operand, which is why the token itself is kept. sema binds the
  // member and leaves the marker on the innermost link of a flattened
  // chain; codegen's shaping pass inserts the dereference it stands
  // for there and clears it, and the evaluator reads it in constant
  // expressions.
  Member *member;
  Token *arrow_tok;

  // Function call
  Type *func_ty;
  Node *args;
  bool pass_by_stack;
  Obj *ret_buffer;

  // Goto or labeled statement, or labels-as-values
  char *label;
  char *unique_label;
  Node *goto_next;

  // Switch. sema's analyze pass links the case labels of a body into
  // the switch that encloses them.
  Node *case_next;
  Node *default_case;

  // [GNU] `a ?: b` conditional. sema lowers it to
  // `tmp = a, tmp ? tmp : b`.
  bool is_elvis;

  // ND_SIZEOF/ND_ALIGNOF: the operand type for the `sizeof(type)`
  // form. The `sizeof expr` form carries its unevaluated operand in
  // `lhs` instead. sema folds the node to its value. ND_CAST (from the
  // parser), ND_GENERIC_ASSOC, ND_TYPES_COMPATIBLE, ND_REG_CLASS,
  // ND_DECL, ND_GVAR_DECL, ND_FUNCDEF and ND_COMPOUND_LITERAL carry
  // their declared/target/operand type here as well.
  Type *ty_op;

  // ND_TYPES_COMPATIBLE: the second type operand, the first being
  // ty_op. The type predicates take two types and no subexpression.
  Type *ty_op2;

  // ND_GENERIC: the result expression of the association the controlling
  // expression selects - the conclusion of the selection, recorded on the
  // node so a consumer can read it instead of the node having to *become*
  // it. Written by sema's select_generic. Nothing reads it yet: today the
  // node is still rewritten to the selected expression as well, and the
  // rewrite is what the evaluator and codegen consume. PLAN A9.1 drops the
  // rewrite and switches both consumers to this field.
  Node *generic_sel;

  // ND_CAST: true for a cast sema inserted to carry out an implicit
  // conversion, false for one the source wrote. The parser never sets
  // it, new_cast always does, and the rewrite sites that copy a node's
  // shape along with it - so a source-to-source tool can tell the two
  // kinds of cast apart. codegen does not read the flag.
  bool is_implicit;

  // Declaration record nodes (ND_DECL, ND_GVAR_DECL, ND_FUNCDEF): the
  // storage-class and alignment attributes as the parser read them.
  VarAttr attr;

  // Declaration record nodes (ND_DECL, ND_GVAR_DECL, ND_FUNCDEF): the
  // declared identifier. The declarator also parks it on the type's
  // `name`, but types are shared (basic-type singletons, tag and
  // typedef types) and that slot is overwritten by the next
  // declarator, long before sema reads the record.
  Token *name_tok;

  // Enum-constant declaration records that became visible while an
  // expression-context type was parsed (e.g. `sizeof(enum E { A })`).
  // sema's resolve pass evaluates and registers them at this node's
  // position, since the record has no statement-chain slot of its own.
  Node *spec_decls;

  // ND_DECL: the faithful initializer record, or NULL if the
  // declarator has no initializer. sema resolves and lowers it to the
  // MEMZERO + assignment comma chain.
  Initializer *decl_init;

  // ND_DECL, ND_GVAR_DECL and ND_COMPOUND_LITERAL: an opaque payload
  // sema's resolve pass parks here - the initializer record resolved
  // against the declared type, which also completes the type - for
  // the annotation pass to consume. Owned by sema.c.
  void *init_resolved;

  // Case
  long begin;
  long end;
  bool is_default; // `default:` rather than `case <expr>:`

  // ND_CASE: the operands of `case <expr>:` and of the [GNU] range form
  // `case <expr> ... <expr>:`, recorded unevaluated; sema's add_type
  // fills in `begin`/`end` from them. `colon_tok` is the `:` that ends
  // the label, where the range check is anchored.
  Node *begin_expr;
  Node *end_expr;
  Token *colon_tok;

  // "asm" string literal
  char *asm_str;

  // Atomic compare-and-swap
  Node *cas_addr;
  Node *cas_old;
  Node *cas_new;

  // Atomic op= operators
  Obj *atomic_addr;
  Node *atomic_expr;

  // Variable
  Obj *var;

  // Numeric literal
  int64_t val;
  long double fval;
};

// Constructors for the faithful-tree nodes. Defined in parse.c; sema
// uses them to build the nodes its lowerings introduce. The shapes that
// only a lowering produces - a bound name, a VLA designator, a numeric
// literal typed at birth - sema constructs itself.
Node *new_node(NodeKind kind, Token *tok);
Node *new_binary(NodeKind kind, Node *lhs, Node *rhs, Token *tok);
Node *new_unary(NodeKind kind, Node *expr, Token *tok);
Node *new_num(int64_t val, Token *tok);

// Identifier spelling, used by sema when it declares the objects the
// parser's declaration records name.
char *get_ident(Token *tok);

// The parser builds the faithful syntax tree and returns the top-level
// declaration record chain; it makes no semantic decision beyond the
// typedef/tag classification oracle the C grammar requires. sema()
// consumes the chain.
Node *parse(Token *tok);

//
// sema.c
//

// Runs semantic analysis over the parser's output: name resolution,
// type annotation, constant evaluation, aggregate layout, lowering, and
// control-flow binding. Returns the list of global objects for codegen.
Obj *sema(Node *toplevel);

// Parses and evaluates a constant expression. Used by the
// preprocessor for `#if`; not part of the parse/sema pipeline below.
int64_t const_expr(Token **rest, Token *tok);

// The parser's conditional-expression entry point (parse.c), which
// const_expr uses to read the expression it evaluates.
Node *conditional(Token **rest, Token *tok);

// The type-level tools a consumer needs in order to build nodes that
// arrive already typed: the annotation entry point, the constructors for
// the shapes only sema produces, and the conversions that carry sema's
// decisions. A consumer (codegen, and later the other users of this
// library) calls these; it does not re-implement a type decision of its
// own. That is what keeps the shaping a consumer does - the pointer
// scaling, the read-modify-write loop, the statement reordering - free of
// semantics: the shape is the consumer's, every conversion inside it is
// sema's, obtained from here.
//
// Some of these have no caller outside sema.c yet. They are exported now
// because the lowerings PLAN A3.1-A9.1 move into codegen call them, and
// exporting them up front keeps those steps pure moves.
void add_type(Node *node);
Node *new_arith(NodeKind kind, Node *lhs, Node *rhs, Token *tok);
void usual_arith_conv(Node **lhs, Node **rhs);
Type *get_common_type(Type *ty1, Type *ty2);
Node *new_cast(Node *expr, Type *ty);
Node *new_long(int64_t val, Token *tok);
Node *new_ulong(long val, Token *tok);
Node *new_var_node(Obj *var, Token *tok);
Node *new_vla_ptr(Obj *var, Token *tok);
Node *new_alloca(Node *sz);

// Temporary exports (PLAN contract 2): the lowerings still living in
// sema.c that the shaping pass calls while they are on their way out.
// The step that moves a callee into codegen deletes its declaration
// here; A9.2 checks the list is empty. new_add and new_sub leave sema
// at A4.1.
Node *new_add(Node *lhs, Node *rhs, Token *tok);
Node *new_sub(Node *lhs, Node *rhs, Token *tok);

// The next value the anonymous-name counter would hand out, without
// consuming it. A consumer that allocates its own `.L..%d` control
// labels continues its own sequence from here, so the two ranges
// never collide; the counter itself stays sema's and only ever names
// the objects the language says exist.
int unique_name_next(void);

//
// type.c
//

typedef enum {
  TY_VOID,
  TY_BOOL,
  TY_CHAR,
  TY_SHORT,
  TY_INT,
  TY_LONG,
  TY_FLOAT,
  TY_DOUBLE,
  TY_LDOUBLE,
  TY_ENUM,
  TY_PTR,
  TY_FUNC,
  TY_ARRAY,
  TY_VLA, // variable-length array
  TY_TYPEOF, // [GNU] `typeof(expr)`, whose operand type sema has not
             // annotated yet
  TY_STRUCT,
  TY_UNION,
} TypeKind;

struct Type {
  TypeKind kind;
  int size;           // sizeof() value
  int align;          // alignment
  bool is_unsigned;   // unsigned or signed
  bool is_atomic;     // true if _Atomic
  Type *origin;       // for type compatibility check

  // Pointer-to or array-of type. We intentionally use the same member
  // to represent pointer/array duality in C.
  //
  // In many contexts in which a pointer is expected, we examine this
  // member instead of "kind" member to determine whether a type is a
  // pointer or not. That means in many contexts "array of T" is
  // naturally handled as if it were "pointer to T", as required by
  // the C spec.
  Type *base;

  // Declaration
  Token *name;
  Token *name_pos;

  // Array
  int array_len;

  // Records the parser's declarator layer leaves for sema to complete
  // (see resolve_type): an array dimension expression that has not been
  // evaluated, so that neither the length nor the fixed-or-variable
  // question is answered in the parser; a `typeof(expr)` operand whose
  // type has not been annotated; and the argument of an `aligned`
  // attribute. Each is cleared once consumed.
  Node *dim_len;
  Node *typeof_expr;
  Node *align_expr;

  // Variable-length array
  Node *vla_len; // # of elements
  Obj *vla_size; // sizeof() value

  // Struct
  Member *members;
  bool is_flexible;
  bool is_packed;
  // The member list is complete but sema has not placed the members
  // yet; resolve_type lays the type out on first sight (an incomplete
  // forward-declared type never carries this).
  bool layout_pending;

  // Function type
  Type *return_ty;
  Type *params;
  bool is_variadic;
  // Enum-constant declaration records a parameter's declspec produced
  // (e.g. `int f(enum E { A } x)`); sema's resolve pass registers them
  // when it completes this function type.
  Node *spec_decls;
  Type *next;
};

// Struct member
struct Member {
  Member *next;
  Type *ty;
  Token *tok; // for error message
  Token *name;
  int idx;
  int align;
  int offset;

  // A `_Alignas` on the member's declspec: the `_Alignas(<type>)` form
  // is recorded in `align_ty` and the `_Alignas(<expr>)` form in
  // `align_expr`; sema settles the value when the aggregate is laid
  // out. Zero means no `_Alignas`, and the type's own alignment counts.
  Type *align_ty;
  Node *align_expr;

  // Bitfield
  bool is_bitfield;
  int bit_offset;
  int bit_width;
  // The recorded width expression of a bitfield, unevaluated; sema
  // evaluates it into bit_width when the aggregate is laid out.
  Node *width_expr;
};

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
Type *copy_type(Type *ty);
Type *pointer_to(Type *base);
Type *func_type(Type *return_ty);
Type *array_of(Type *base, int size);
Type *vla_of(Type *base, Node *expr);
Type *array_of_dim(Type *base, Node *dim);
Type *typeof_placeholder(void);
Type *enum_type(void);
Type *struct_type(void);

//
// codegen.c
//

void codegen(Obj *prog, FILE *out);
int align_to(int n, int align);

//
// unicode.c
//

int encode_utf8(char *buf, uint32_t c);
uint32_t decode_utf8(char **new_pos, char *p);
bool is_ident1(uint32_t c);
bool is_ident2(uint32_t c);
int display_width(char *p, int len);

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
  int used;
} HashMap;

void *hashmap_get(HashMap *map, char *key);
void *hashmap_get2(HashMap *map, char *key, int keylen);
void hashmap_put(HashMap *map, char *key, void *val);
void hashmap_put2(HashMap *map, char *key, int keylen, void *val);
void hashmap_delete(HashMap *map, char *key);
void hashmap_delete2(HashMap *map, char *key, int keylen);
void hashmap_test(void);

//
// main.c
//

bool file_exists(char *path);

extern StringArray include_paths;
extern bool opt_fpic;
extern bool opt_fcommon;
extern char *base_file;
