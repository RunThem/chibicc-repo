// Layer 3: the faithful syntax tree (plans/PLAN-faithful.md). The parser keeps
// every written form distinct and makes no semantic decision; layer 4
// (sema.h) binds names, computes types and records its conclusions on
// these nodes.

#ifndef LIBCP_AST_H
#define LIBCP_AST_H

#include <stdbool.h>
#include <stdint.h>

#include <libcp/token.h>

typedef struct libcp_type LibcpType;
typedef struct libcp_node LibcpNode;
typedef struct libcp_obj LibcpObj;
typedef struct libcp_member LibcpMember;
typedef struct libcp_relocation LibcpRelocation;
typedef struct libcp_initializer LibcpInitializer;
typedef struct libcp_init_item LibcpInitItem;
typedef struct libcp_init_desig LibcpInitDesig;
typedef struct libcp_init_tree LibcpInitTree;
typedef struct libcp_init_path LibcpInitPath;

// A growable string array. The library uses it internally; it is part of
// the public type set because LibcpObj.refs exposes one.
typedef struct libcp_string_array {
  char **data;
  int capacity;
  int len;
} LibcpStringArray;

// Variable or function
struct libcp_obj {
  LibcpObj *next;
  char *name;    // Variable name
  LibcpType *ty;      // LibcpType
  LibcpToken *tok;    // representative token
  bool is_local; // local or global/function
  int align;     // alignment

  // Global variable or function
  bool is_function;
  bool is_definition;
  bool is_static;

  // Global variable
  bool is_tentative;
  bool is_tls;
  char *init_data;
  LibcpRelocation *rel;

  // Function
  bool is_inline;
  LibcpObj *params;
  LibcpNode *body;
  LibcpObj *locals;
  LibcpObj *va_area;
  LibcpObj *alloca_bottom;

  // Static inline function
  bool is_live;
  bool is_root;
  LibcpStringArray refs;
};

// Global variable can be initialized either by a constant expression
// or a pointer to another global variable. This struct represents the
// latter.
struct libcp_relocation {
  LibcpRelocation *next;
  int offset;

  // The target of an address constant. A named symbol (a global, a
  // function, a string literal's anonymous global, a file-scope
  // compound literal's object) is known at serialization time and
  // lives in `label`. A [GNU] `&&label` reference has no name yet:
  // codegen allocates the `.L` name during shaping, so the node is
  // kept here and resolved at emission.
  char *label;
  LibcpNode *label_node;

  long addend;
};

// Declaration attributes collected by declspec: storage class and
// _Alignas. The parser fills this in and copies it onto the declaration
// record nodes; sema consumes it when it declares the object.
typedef struct libcp_var_attr {
  bool is_typedef;
  bool is_static;
  bool is_extern;
  bool is_inline;
  bool is_tls;
  int align;
  // A `_Alignas(<expr>)` argument, of which `align_ty` is the
  // `_Alignas(<type>)` form. Whichever comes last is the one that
  // counts; sema reads the type form's alignment off a completed type.
  LibcpNode *align_expr;
  LibcpType *align_ty;
} LibcpVarAttr;

// Faithful initializer record (parse.c): the syntactic shape of an
// initializer as written - a braced item sequence, a string literal or
// a single expression, with designators recorded unevaluated. sema
// resolves designators, applies brace elision and lowers the record
// when it types the carrying node (ND_DECL, compound literal) or when
// a global variable definition is parsed.
enum { INIT_LIST, INIT_EXPR, INIT_STR };

struct libcp_initializer {
  int kind;     // INIT_LIST, INIT_EXPR or INIT_STR
  LibcpToken *tok;   // first token of the initializer source
  LibcpInitItem *items;  // INIT_LIST: braced element sequence
  LibcpNode *expr;       // INIT_EXPR: scalar initialization expression
  LibcpToken *str_tok;   // INIT_STR: string literal token

  // Only for the top-level record of an ND_DECL: the `=` that
  // introduced it, which is where sema anchors a diagnostic that
  // rejects the declaration as a whole.
  LibcpToken *eq_tok;
};

// One element of a braced initializer list.
struct libcp_init_item {
  LibcpInitItem *next;
  LibcpToken *comma_tok;    // the `,` before this element (NULL for the first)
  LibcpInitDesig *desigs;   // designator chain as written (may be NULL)
  LibcpInitializer *init;   // the element value
};

// One designator as written: `[expr]`, `[begin ... end]` or `.name`.
// Index expressions are recorded unevaluated; sema evaluates them.
struct libcp_init_desig {
  LibcpInitDesig *next;
  LibcpToken *tok;           // `[` or `.`
  LibcpToken *after_begin;   // token after the begin expression (`...` or `]`)
  LibcpToken *rbracket;      // the `]` token
  LibcpNode *begin;          // array designator index
  LibcpNode *end;            // range end (NULL unless `[begin ... end]`)
  LibcpToken *name;          // member designator name
};

// The resolved initializer tree (PLAN A8.1): what a faithful
// initializer record becomes once designators are evaluated, member
// names are bound, brace elision is applied and flexible arrays are
// sized. sema's resolve pass builds it; the consumer's declaration
// expansion walks it to build the assignment chain, and sema's
// file-scope serializer walks it into .data bytes. Since initializers
// can be nested (e.g. `int x[2][2] = {{1, 2}, {3, 4}}`), it is a tree.
struct libcp_init_tree {
  LibcpType *ty;
  bool is_flexible;

  // If it's not an aggregate type and has an initializer,
  // `expr` has an initialization expression.
  LibcpNode *expr;

  // If it's an initializer for an aggregate type (e.g. array or struct),
  // `children` has initializers for its children.
  LibcpInitTree **children;

  // Only one member can be initialized for a union.
  // `mem` is used to clarify which member is initialized.
  LibcpMember *mem;
};

// Designator chain describing the position of an element within a
// local variable initializer (e.g. `x[1].y[2]`), walked by the
// consumer's expansion to build the target expression of each
// assignment.
struct libcp_init_path {
  LibcpInitPath *next;
  int idx;
  LibcpMember *member;
  LibcpObj *var;
};

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
} LibcpTypeKind;

struct libcp_type {
  LibcpTypeKind kind;
  int size;           // sizeof() value
  int align;          // alignment
  bool is_unsigned;   // unsigned or signed
  bool is_atomic;     // true if _Atomic
  LibcpType *origin;       // for type compatibility check

  // Pointer-to or array-of type. We intentionally use the same member
  // to represent pointer/array duality in C.
  //
  // In many contexts in which a pointer is expected, we examine this
  // member instead of "kind" member to determine whether a type is a
  // pointer or not. That means in many contexts "array of T" is
  // naturally handled as if it were "pointer to T", as required by
  // the C spec.
  LibcpType *base;

  // Declaration. A declarator name is never written into a shared type
  // record (F1); these fields are filled only on the per-parameter copy
  // inside a function type, where sema reads them.
  LibcpToken *name;
  LibcpToken *name_pos;

  // Array
  int array_len;

  // Records the parser's declarator layer leaves for sema to complete
  // (see resolve_type): an array dimension expression that has not been
  // evaluated, so that neither the length nor the fixed-or-variable
  // question is answered in the parser; a `typeof(expr)` operand whose
  // type has not been annotated; and the argument of an `aligned`
  // attribute. Each is cleared once consumed.
  LibcpNode *dim_len;
  LibcpNode *typeof_expr;
  LibcpNode *align_expr;

  // Variable-length array. The runtime size slot codegen creates for
  // it (A7.1) is kept in codegen's private per-type table (PLAN L1).
  LibcpNode *vla_len; // # of elements

  // Struct
  LibcpMember *members;
  bool is_flexible;
  bool is_packed;
  // The member list is complete but sema has not placed the members
  // yet; resolve_type lays the type out on first sight (an incomplete
  // forward-declared type never carries this).
  bool layout_pending;

  // Function type
  LibcpType *return_ty;
  LibcpType *params;
  bool is_variadic;
  // Enum-constant declaration records a parameter's declspec produced
  // (e.g. `int f(enum E { A } x)`); sema's resolve pass registers them
  // when it completes this function type.
  LibcpNode *spec_decls;
  LibcpType *next;
};

// Struct member
struct libcp_member {
  LibcpMember *next;
  LibcpType *ty;
  LibcpToken *tok; // for error message
  LibcpToken *name;
  int idx;
  int align;
  int offset;

  // A `_Alignas` on the member's declspec: the `_Alignas(<type>)` form
  // is recorded in `align_ty` and the `_Alignas(<expr>)` form in
  // `align_expr`; sema settles the value when the aggregate is laid
  // out. Zero means no `_Alignas`, and the type's own alignment counts.
  LibcpType *align_ty;
  LibcpNode *align_expr;

  // Bitfield
  bool is_bitfield;
  int bit_offset;
  int bit_width;
  // The recorded width expression of a bitfield, unevaluated; sema
  // evaluates it into bit_width when the aggregate is laid out.
  LibcpNode *width_expr;
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
  ND_CAST,      // LibcpType cast. The parser leaves an explicit cast untyped
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
} LibcpNodeKind;

// AST node type
struct libcp_node {
  LibcpNodeKind kind; // LibcpNode kind
  LibcpNode *next;    // Next node
  LibcpType *ty;      // LibcpType, e.g. int or pointer to int
  LibcpToken *tok;    // Representative token

  LibcpNode *lhs;     // Left-hand side
  LibcpNode *rhs;     // Right-hand side

  // Compound assignment operator for ND_ASSIGN ("+=", "-=", ...).
  // 0 means a plain "=".
  LibcpNodeKind op;

  // Increment/decrement for ND_INCDEC: postfix (`i++`) or prefix
  // (`++i`), with an addend of +1 or -1. sema lowers the node to a
  // compound assignment.
  bool is_post;
  int addend;

  // "if" or "for" statement
  LibcpNode *cond;
  LibcpNode *then;
  LibcpNode *els;
  LibcpNode *init;
  LibcpNode *inc;

  // Block or statement expression
  LibcpNode *body;

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
  // chain (clearing it on the dissolved outer node); the evaluator
  // reads it in constant expressions, and codegen's gen_addr and
  // to_assign read it as the dereference the `->` stands for (PLAN
  // B1.1b - the marker stays on the node, nothing rewrites it away).
  LibcpMember *member;
  LibcpToken *arrow_tok;

  // Function call. func_ty and the conversions on the arguments are
  // sema's (the annotation). The call's codegen-side state - the
  // pass-by-stack marks and the caller-owned return buffer - lives in
  // codegen's private per-node table (PLAN L1).
  LibcpType *func_ty;
  LibcpNode *args;

  // Goto or labeled statement, or labels-as-values. `label` is the
  // spelled name the parser recorded and the goto/label pairing check
  // compares; the `.L..` names codegen allocates for goto, label and
  // case nodes live in its private per-node table (PLAN L1).
  char *label;

  // [GNU] `a ?: b` conditional. sema types it; gen_expr emits it from
  // its own fields without a slot - the test value stays in the
  // register (PLAN B1.1e) - and the evaluator and is_const_expr treat
  // it as a runtime value through this flag.
  bool is_elvis;

  // ND_SIZEOF/ND_ALIGNOF: the operand type for the `sizeof(type)`
  // form. The `sizeof expr` form carries its unevaluated operand in
  // `lhs` instead. sema records the size or alignment in `val`; a
  // VLA operand is not a constant and codegen's shaping pass builds
  // the runtime size expression for it (A9.1/A7.1). ND_CAST (from the
  // parser), ND_GENERIC_ASSOC, ND_TYPES_COMPATIBLE, ND_REG_CLASS,
  // ND_DECL, ND_GVAR_DECL, ND_FUNCDEF and ND_COMPOUND_LITERAL carry
  // their declared/target/operand type here as well.
  LibcpType *ty_op;

  // ND_TYPES_COMPATIBLE: the second type operand, the first being
  // ty_op. The type predicates take two types and no subexpression.
  LibcpType *ty_op2;

  // ND_GENERIC: the result expression of the association the controlling
  // expression selects - the conclusion of the selection, recorded on the
  // node so a consumer can read it instead of the node having to *become*
  // it. Written by sema's select_generic; the evaluator, gen_expr and
  // gen_addr read it (A9.1). The node keeps its kind.
  LibcpNode *generic_sel;

  // ND_CAST: true for a cast sema inserted to carry out an implicit
  // conversion, false for one the source wrote. The parser never sets
  // it, new_cast always does, and the rewrite sites that copy a node's
  // shape along with it - so a source-to-source tool can tell the two
  // kinds of cast apart. codegen does not read the flag.
  bool is_implicit;

  // Declaration record nodes (ND_DECL, ND_GVAR_DECL, ND_FUNCDEF): the
  // storage-class and alignment attributes as the parser read them.
  LibcpVarAttr attr;

  // Declaration record nodes (ND_DECL, ND_GVAR_DECL, ND_FUNCDEF): the
  // declared identifier. The declarator also parks it on the type's
  // `name`, but types are shared (basic-type singletons, tag and
  // typedef types) and that slot is overwritten by the next
  // declarator, long before sema reads the record.
  LibcpToken *name_tok;

  // Enum-constant declaration records that became visible while an
  // expression-context type was parsed (e.g. `sizeof(enum E { A })`).
  // sema's resolve pass evaluates and registers them at this node's
  // position, since the record has no statement-chain slot of its own.
  LibcpNode *spec_decls;

  // ND_DECL: the faithful initializer record, or NULL if the
  // declarator has no initializer. sema resolves and lowers it to the
  // MEMZERO + assignment comma chain.
  LibcpInitializer *decl_init;

  // ND_DECL, ND_GVAR_DECL and ND_COMPOUND_LITERAL: an opaque payload
  // private to sema.c. The resolve pass parks the initializer record
  // resolved against the declared type here for the annotation pass; it
  // may be NULL outside a sema run, and a consumer must neither read nor
  // write it. The public tree does not interpret it.
  void *init_resolved;

  // Case
  long begin;
  long end;
  bool is_default; // `default:` rather than `case <expr>:`

  // ND_CASE: the operands of `case <expr>:` and of the [GNU] range form
  // `case <expr> ... <expr>:`, recorded unevaluated; sema's add_type
  // fills in `begin`/`end` from them. `colon_tok` is the `:` that ends
  // the label, where the range check is anchored.
  LibcpNode *begin_expr;
  LibcpNode *end_expr;
  LibcpToken *colon_tok;

  // "asm" string literal
  char *asm_str;

  // Atomic compare-and-swap
  LibcpNode *cas_addr;
  LibcpNode *cas_old;
  LibcpNode *cas_new;

  // Atomic op= operators
  LibcpObj *atomic_addr;
  LibcpNode *atomic_expr;

  // Variable
  LibcpObj *var;

  // Numeric literal
  int64_t val;
  long double fval;
};

// The parser builds the faithful syntax tree and returns the top-level
// declaration record chain; it makes no semantic decision beyond the
// typedef/tag classification oracle the C grammar requires.
LIBCP_API LibcpNode *libcp_parse(LibcpContext *ctx, LibcpToken *tok);

#endif
