// This file contains a recursive descent parser for C.
//
// Most functions in this file are named after the symbols they are
// supposed to read from an input token list. For example, stmt() is
// responsible for reading a statement from a token list. The function
// then construct an AST node representing that statement.
//
// Each function conceptually returns two values, an AST node and
// remaining part of the input tokens. Since C doesn't support
// multiple return values, the remaining tokens are returned to the
// caller via a pointer argument.
//
// Input tokens are represented by a linked list. Unlike many recursive
// descent parsers, we don't have the notion of the "input token stream".
// Most parsing functions don't change the global state of the parser.
// So it is very easy to lookahead arbitrary number of tokens in this
// parser.
//
// The tree this parser builds is faithful to the source and carries no
// semantic decision: expressions are untyped, names are unbound,
// constant expressions are unevaluated, and declarations (variables,
// functions, typedefs, enum constants) are emitted as records for sema
// to declare and lower. The only semantic feedback the parser takes is
// the one the C grammar demands - whether an identifier is a typedef
// name and what a struct/union/enum tag refers to - plus the
// "to-be-completed" type records the declarator layer builds (array
// dimensions, typeof operands, alignment and bitfield-width
// expressions), which sema resolves.
//
// That feedback comes from the scope stack at the top of this file,
// which is the parser's own and holds nothing else. sema keeps a
// separate stack for the objects and enum constants it declares.

#include "libcp.h"

static LibcpType *find_typedef(LibcpContext *ctx, LibcpToken *tok);
static LibcpType *find_tag(LibcpContext *ctx, LibcpToken *tok);
static LibcpType *find_current_tag(LibcpContext *ctx, LibcpToken *tok);
static void push_tag_scope(LibcpContext *ctx, LibcpToken *tok, LibcpType *ty);
static void add_typedef(LibcpContext *ctx, LibcpNode *node);
static void add_declared_name(LibcpContext *ctx, LibcpToken *tok);
static void enter_scope(LibcpContext *ctx);
static void leave_scope(LibcpContext *ctx);

// Bound the parser's recursion: pathological nesting must fail with a
// diagnostic, not a stack overflow.
#define MAX_PARSE_DEPTH 512

static bool is_typename(LibcpContext *ctx, LibcpToken *tok);
static LibcpToken *attribute_list(LibcpContext *ctx, LibcpToken *tok, LibcpType *ty);
static LibcpType *declspec(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpVarAttr *attr, LibcpNode **specs);
static LibcpType *typename(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpNode **specs);
static LibcpType *enum_specifier(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpNode **specs);
static LibcpType *typeof_specifier(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpNode **specs);
static LibcpType *type_suffix(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *ty);

// The built-in types are shared by every context (type.c), and a tag or
// typedef type is shared by every declaration that names it. A
// declarator therefore never writes its name into the type record: the
// name travels in this per-declaration carrier instead (F1).
typedef struct {
  LibcpToken *name;     // NULL when the declarator has none
  LibcpToken *name_pos; // where the name would go, for "name omitted"
} LibcpDeclName;

static LibcpType *declarator(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *ty, LibcpDeclName *decl);
static LibcpNode *declaration(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *basety, LibcpVarAttr *attr);
static LibcpInitializer *init_record(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok);
static LibcpNode *compound_stmt(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok);
static LibcpNode *stmt(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok);
static LibcpNode *expr_stmt(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok);
static LibcpNode *expr(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok);
static LibcpNode *assign(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok);
static LibcpType *struct_decl(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpNode **specs);
static LibcpType *union_decl(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpNode **specs);
static LibcpNode *funcall(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpNode *node);
static LibcpNode *unary(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok);
// The body behind each depth-guarded entry (the wrapper increments
// parse_depth before calling it).
static LibcpType *declarator1(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *ty, LibcpDeclName *decl);
static LibcpType *abstract_declarator(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *ty);
static LibcpType *abstract_declarator1(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *ty);
static LibcpInitializer *init_record1(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok);
static LibcpNode *primary(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok);
static LibcpNode *parse_typedef(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *basety);
static bool is_function(LibcpContext *ctx, LibcpToken *tok);
static LibcpNode *function_def(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *basety, LibcpVarAttr *attr);
static LibcpNode *global_variable(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *basety, LibcpVarAttr *attr);

LibcpNode *libcp_new_node(LibcpContext *ctx, LibcpNodeKind kind, LibcpToken *tok) {
  LibcpNode *node = libcp_arena_calloc(ctx, 1, sizeof(LibcpNode));
  node->kind = kind;
  node->tok = tok;
  return node;
}

LibcpNode *libcp_new_binary(LibcpContext *ctx, LibcpNodeKind kind, LibcpNode *lhs, LibcpNode *rhs, LibcpToken *tok) {
  LibcpNode *node = libcp_new_node(ctx, kind, tok);
  node->lhs = lhs;
  node->rhs = rhs;
  return node;
}

LibcpNode *libcp_new_unary(LibcpContext *ctx, LibcpNodeKind kind, LibcpNode *expr, LibcpToken *tok) {
  LibcpNode *node = libcp_new_node(ctx, kind, tok);
  node->lhs = expr;
  return node;
}

LibcpNode *libcp_new_num(LibcpContext *ctx, int64_t val, LibcpToken *tok) {
  LibcpNode *node = libcp_new_node(ctx, ND_NUM, tok);
  node->val = val;
  return node;
}

char *libcp_get_ident(LibcpContext *ctx, LibcpToken *tok) {
  if (tok->kind != TK_IDENT)
    libcp_error_tok(ctx, tok, "expected an identifier");
  return libcp_arena_strndup(ctx, tok->loc, tok->len);
}

//
// The parser's scope stack
//
// C's grammar is not context-free: whether `t * x;` declares a pointer
// or multiplies two variables depends on whether `t` is a typedef name,
// and `struct T` has to be resolvable to know whether a tag was seen
// before. The parser therefore keeps its own block scope stack, and it
// holds nothing else - no object, no enum constant, no type annotation.
// sema derives its own scope stack from the tree when it resolves names.

// An identifier the parser has seen. `type_def` is non-NULL for a
// typedef name; a plain declared name gets an empty entry, which is how
// a variable shadows a typedef of the same spelling from its point of
// declaration on.
typedef struct {
  LibcpType *type_def;
} LibcpNameEntry;

struct libcp_parse_scope {
  LibcpParseScope *next;

  // C has two block scopes; one is for ordinary names and the other is
  // for struct/union/enum tags.
  LibcpHashMap names;
  LibcpHashMap tags;
};

void libcp_parse_init(LibcpContext *ctx) {
  ctx->scope = libcp_arena_calloc(ctx, 1, sizeof(LibcpParseScope));
}

// The parser's scope stack lives in LibcpContext (PLAN L2.4).
#define scope (ctx->scope)

static void enter_scope(LibcpContext *ctx) {
  LibcpParseScope *sc = libcp_arena_calloc(ctx, 1, sizeof(LibcpParseScope));
  sc->next = scope;
  scope = sc;
}

static void leave_scope(LibcpContext *ctx) {
  scope = scope->next;
}

// The parser's typedef-name oracle: the C grammar needs to know whether
// an identifier names a type before it can parse a declaration.
static LibcpType *find_typedef(LibcpContext *ctx, LibcpToken *tok) {
  if (tok->kind != TK_IDENT)
    return NULL;

  for (LibcpParseScope *sc = scope; sc; sc = sc->next) {
    LibcpNameEntry *e = libcp_hashmap_get2(ctx, &sc->names, tok->loc, tok->len);
    if (e)
      return e->type_def;
  }
  return NULL;
}

// Declares a typedef name in the current scope. The name must be
// visible immediately: the oracle has to classify the very next
// identifier. sema completes the type when it reaches the ND_TYPEDEF
// record this declaration also emits.
static void add_typedef(LibcpContext *ctx, LibcpNode *node) {
  LibcpNameEntry *e = libcp_arena_calloc(ctx, 1, sizeof(LibcpNameEntry));
  e->type_def = node->ty;
  libcp_hashmap_put(ctx, &scope->names, libcp_get_ident(ctx, node->tok), e);
}

// Records a name a declarator just declared (a variable, function,
// parameter or enum constant), so that it shadows a typedef of the same
// spelling for the rest of the parse.
static void add_declared_name(LibcpContext *ctx, LibcpToken *tok) {
  libcp_hashmap_put(ctx, &scope->names, libcp_get_ident(ctx, tok), libcp_arena_calloc(ctx, 1, sizeof(LibcpNameEntry)));
}

// Find a struct/union/enum tag by name, in any enclosing scope.
static LibcpType *find_tag(LibcpContext *ctx, LibcpToken *tok) {
  for (LibcpParseScope *sc = scope; sc; sc = sc->next) {
    LibcpType *ty = libcp_hashmap_get2(ctx, &sc->tags, tok->loc, tok->len);
    if (ty)
      return ty;
  }
  return NULL;
}

// Find a tag declared in the current scope only; used when a tag is
// (re)defined, so that a previous forward declaration in the same scope
// keeps its identity.
static LibcpType *find_current_tag(LibcpContext *ctx, LibcpToken *tok) {
  return libcp_hashmap_get2(ctx, &scope->tags, tok->loc, tok->len);
}

static void push_tag_scope(LibcpContext *ctx, LibcpToken *tok, LibcpType *ty) {
  libcp_hashmap_put2(ctx, &scope->tags, tok->loc, tok->len, ty);
}

// Appends a chain of nodes to the chain `cur` points into and returns
// its new tail.
static LibcpNode *chain_append(LibcpNode *cur, LibcpNode *chain) {
  for (; chain; chain = chain->next)
    cur = cur->next = chain;
  return cur;
}

// Returns `specs` with `chain` appended after it (or `chain` if there
// are no specs yet). Used where declaration records produced by a
// declspec have to precede the records the declaration itself emits.
static LibcpNode *specs_then(LibcpNode *specs, LibcpNode *chain) {
  if (!specs)
    return chain;
  LibcpNode *cur = specs;
  while (cur->next)
    cur = cur->next;
  cur->next = chain;
  return specs;
}

// Appends one declaration record (an enum constant) to a spec list.
static void add_spec(LibcpNode **specs, LibcpNode *node) {
  node->next = NULL;
  if (!*specs) {
    *specs = node;
    return;
  }
  LibcpNode *cur = *specs;
  while (cur->next)
    cur = cur->next;
  cur->next = node;
}

// declspec = ("void" | "_Bool" | "char" | "short" | "int" | "long"
//             | "typedef" | "static" | "extern" | "inline"
//             | "_Thread_local" | "__thread"
//             | "signed" | "unsigned"
//             | struct-decl | union-decl | typedef-name
//             | enum-specifier | typeof-specifier
//             | "const" | "volatile" | "auto" | "register" | "restrict"
//             | "__restrict" | "__restrict__" | "_Noreturn")+
//
// The order of typenames in a type-specifier doesn't matter. For
// example, `int long static` means the same as `static long int`.
// That can also be written as `static long` because you can omit
// `int` if `long` or `short` is specified. However, something like
// `char int` is not a valid type specifier. We have to accept only a
// limited combinations of the typenames.
//
// In this function, we count the number of occurrences of each typename
// while keeping the "current" type object that the typenames up
// until that point represent. When we reach a non-typename token,
// we returns the current type object.
//
// Enum constants an enum-specifier defines along the way are declaration
// records with no statement of their own; they are appended to `*specs`
// for the caller to place at this position of its chain or node.
static LibcpType *declspec(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpVarAttr *attr, LibcpNode **specs) {
  // We use a single integer as counters for all typenames.
  // For example, bits 0 and 1 represents how many times we saw the
  // keyword "void" so far. With this, we can use a switch statement
  // as you can see below.
  enum {
    VOID     = 1 << 0,
    BOOL     = 1 << 2,
    CHAR     = 1 << 4,
    SHORT    = 1 << 6,
    INT      = 1 << 8,
    LONG     = 1 << 10,
    FLOAT    = 1 << 12,
    DOUBLE   = 1 << 14,
    OTHER    = 1 << 16,
    SIGNED   = 1 << 17,
    UNSIGNED = 1 << 18,
  };

  LibcpType *ty = libcp_ty_int;
  int counter = 0;
  bool is_atomic = false;

  while (is_typename(ctx, tok) || libcp_equal(tok, "__attribute__")) {
    // Declaration and function attributes are accepted in any specifier
    // position; the ones without a type meaning are ignored.
    if (libcp_equal(tok, "__attribute__")) {
      tok = attribute_list(ctx, tok, NULL);
      continue;
    }

    // Handle storage class specifiers.
    if (libcp_equal(tok, "typedef") || libcp_equal(tok, "static") || libcp_equal(tok, "extern") ||
        libcp_equal(tok, "inline") || libcp_equal(tok, "_Thread_local") || libcp_equal(tok, "__thread")) {
      if (!attr)
        libcp_error_tok(ctx, tok, "storage class specifier is not allowed in this context");

      if (libcp_equal(tok, "typedef"))
        attr->is_typedef = true;
      else if (libcp_equal(tok, "static"))
        attr->is_static = true;
      else if (libcp_equal(tok, "extern"))
        attr->is_extern = true;
      else if (libcp_equal(tok, "inline"))
        attr->is_inline = true;
      else
        attr->is_tls = true;

      if (attr->is_typedef &&
          attr->is_static + attr->is_extern + attr->is_inline + attr->is_tls > 1)
        libcp_error_tok(ctx, tok, "typedef may not be used together with static,"
                  " extern, inline, __thread or _Thread_local");
      tok = tok->next;
      continue;
    }

    // These keywords are recognized but ignored.
    if (libcp_consume(&tok, tok, "const") || libcp_consume(&tok, tok, "volatile") ||
        libcp_consume(&tok, tok, "auto") || libcp_consume(&tok, tok, "register") ||
        libcp_consume(&tok, tok, "restrict") || libcp_consume(&tok, tok, "__restrict") ||
        libcp_consume(&tok, tok, "__restrict__") || libcp_consume(&tok, tok, "_Noreturn"))
      continue;

    if (libcp_equal(tok, "_Atomic")) {
      tok = tok->next;
      if (libcp_equal(tok , "(")) {
        ty = typename(ctx, &tok, tok->next, specs);
        tok = libcp_skip(ctx, tok, ")");
      }
      is_atomic = true;
      continue;
    }

    if (libcp_equal(tok, "_Alignas")) {
      if (!attr)
        libcp_error_tok(ctx, tok, "_Alignas is not allowed in this context");
      tok = libcp_skip(ctx, tok->next, "(");

      // The `_Alignas(type)` form asks for a property of a type that
      // may not be complete yet, and the `_Alignas(expr)` form is a
      // constant expression; both are recorded for sema to settle.
      // Whichever comes last is the one that counts, as before.
      if (is_typename(ctx, tok)) {
        attr->align_ty = typename(ctx, &tok, tok, specs);
        attr->align_expr = NULL;
      } else {
        attr->align_ty = NULL;
        attr->align_expr = libcp_conditional(ctx, &tok, tok);
      }
      tok = libcp_skip(ctx, tok, ")");
      continue;
    }

    // Handle user-defined types.
    LibcpType *ty2 = find_typedef(ctx, tok);
    if (libcp_equal(tok, "struct") || libcp_equal(tok, "union") || libcp_equal(tok, "enum") ||
        libcp_equal(tok, "typeof") || ty2) {
      if (counter)
        break;

      if (libcp_equal(tok, "struct")) {
        ty = struct_decl(ctx, &tok, tok->next, specs);
      } else if (libcp_equal(tok, "union")) {
        ty = union_decl(ctx, &tok, tok->next, specs);
      } else if (libcp_equal(tok, "enum")) {
        ty = enum_specifier(ctx, &tok, tok->next, specs);
      } else if (libcp_equal(tok, "typeof")) {
        ty = typeof_specifier(ctx, &tok, tok->next, specs);
      } else {
        ty = ty2;
        tok = tok->next;
      }

      counter += OTHER;
      continue;
    }

    // Handle built-in types.
    if (libcp_equal(tok, "void"))
      counter += VOID;
    else if (libcp_equal(tok, "_Bool"))
      counter += BOOL;
    else if (libcp_equal(tok, "char"))
      counter += CHAR;
    else if (libcp_equal(tok, "short"))
      counter += SHORT;
    else if (libcp_equal(tok, "int"))
      counter += INT;
    else if (libcp_equal(tok, "long"))
      counter += LONG;
    else if (libcp_equal(tok, "float"))
      counter += FLOAT;
    else if (libcp_equal(tok, "double"))
      counter += DOUBLE;
    else if (libcp_equal(tok, "signed"))
      counter |= SIGNED;
    else if (libcp_equal(tok, "unsigned"))
      counter |= UNSIGNED;
    else
      unreachable();

    switch (counter) {
    case VOID:
      ty = libcp_ty_void;
      break;
    case BOOL:
      ty = libcp_ty_bool;
      break;
    case CHAR:
    case SIGNED + CHAR:
      ty = libcp_ty_char;
      break;
    case UNSIGNED + CHAR:
      ty = libcp_ty_uchar;
      break;
    case SHORT:
    case SHORT + INT:
    case SIGNED + SHORT:
    case SIGNED + SHORT + INT:
      ty = libcp_ty_short;
      break;
    case UNSIGNED + SHORT:
    case UNSIGNED + SHORT + INT:
      ty = libcp_ty_ushort;
      break;
    case INT:
    case SIGNED:
    case SIGNED + INT:
      ty = libcp_ty_int;
      break;
    case UNSIGNED:
    case UNSIGNED + INT:
      ty = libcp_ty_uint;
      break;
    case LONG:
    case LONG + INT:
    case LONG + LONG:
    case LONG + LONG + INT:
    case SIGNED + LONG:
    case SIGNED + LONG + INT:
    case SIGNED + LONG + LONG:
    case SIGNED + LONG + LONG + INT:
      ty = libcp_ty_long;
      break;
    case UNSIGNED + LONG:
    case UNSIGNED + LONG + INT:
    case UNSIGNED + LONG + LONG:
    case UNSIGNED + LONG + LONG + INT:
      ty = libcp_ty_ulong;
      break;
    case FLOAT:
      ty = libcp_ty_float;
      break;
    case DOUBLE:
      ty = libcp_ty_double;
      break;
    case LONG + DOUBLE:
      ty = libcp_ty_ldouble;
      break;
    default:
      libcp_error_tok(ctx, tok, "invalid type");
    }

    tok = tok->next;
  }

  if (is_atomic) {
    ty = libcp_copy_type(ctx, ty);
    ty->is_atomic = true;
  }

  *rest = tok;
  return ty;
}

// func-params = ("void" | param ("," param)* ("," "...")?)? ")"
// param       = declspec declarator
static LibcpType *func_params(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *ty) {
  if (libcp_equal(tok, "void") && libcp_equal(tok->next, ")")) {
    *rest = tok->next->next;
    return libcp_func_type(ctx, ty);
  }

  LibcpType head = {};
  LibcpType *cur = &head;
  bool is_variadic = false;
  LibcpNode *specs = NULL;

  while (!libcp_equal(tok, ")")) {
    if (cur != &head)
      tok = libcp_skip(ctx, tok, ",");

    if (libcp_equal(tok, "...")) {
      is_variadic = true;
      tok = tok->next;
      libcp_skip(ctx, tok, ")");
      break;
    }

    LibcpDeclName decl = {};
    LibcpType *ty2 = declspec(ctx, &tok, tok, NULL, &specs);
    ty2 = declarator(ctx, &tok, tok, ty2, &decl);

    if (ty2->kind == TY_ARRAY) {
      // "array of T" is converted to "pointer to T" only in the parameter
      // context. For example, *argv[] is converted to **argv by this.
      ty2 = libcp_pointer_to(ctx, ty2->base);
    } else if (ty2->kind == TY_FUNC) {
      // Likewise, a function is converted to a pointer to a function
      // only in the parameter context.
      ty2 = libcp_pointer_to(ctx, ty2);
    }

    // The parameter record is copied into the function type; its name
    // lives on that per-parameter copy, where sema reads it.
    LibcpType *param = libcp_copy_type(ctx, ty2);
    param->name = decl.name;
    param->name_pos = decl.name_pos;
    cur = cur->next = param;
  }

  if (cur == &head)
    is_variadic = true;

  ty = libcp_func_type(ctx, ty);
  ty->params = head.next;
  ty->is_variadic = is_variadic;
  // Enum constants a parameter's declspec defined (e.g.
  // `int f(enum E { A } x)`) ride on the function type; sema registers
  // them when it completes the type.
  ty->spec_decls = specs;
  *rest = tok->next;
  return ty;
}

// array-dimensions = ("static" | "restrict")* const-expr? "]" type-suffix
static LibcpType *array_dimensions(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *ty) {
  while (libcp_equal(tok, "static") || libcp_equal(tok, "restrict"))
    tok = tok->next;

  if (libcp_equal(tok, "]")) {
    ty = type_suffix(ctx, rest, tok->next, ty);
    return libcp_array_of(ctx, ty, -1);
  }

  LibcpNode *expr = libcp_conditional(ctx, &tok, tok);
  tok = libcp_skip(ctx, tok, "]");
  ty = type_suffix(ctx, rest, tok, ty);

  // Whether the dimension denotes a fixed-length array or a VLA depends
  // on whether the length expression is a constant expression - a
  // semantic question. The expression is recorded here and sema decides
  // the type when it resolves the declarator (see resolve_type).
  return libcp_array_of_dim(ctx, ty, expr);
}

// type-suffix = "(" func-params
//             | "[" array-dimensions
//             | ε
static LibcpType *type_suffix(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *ty) {
  if (libcp_equal(tok, "("))
    return func_params(ctx, rest, tok->next, ty);

  if (libcp_equal(tok, "["))
    return array_dimensions(ctx, rest, tok->next, ty);

  *rest = tok;
  return ty;
}

// pointers = ("*" ("const" | "volatile" | "restrict")*)*
static LibcpType *pointers(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *ty) {
  while (libcp_consume(&tok, tok, "*")) {
    ty = libcp_pointer_to(ctx, ty);
    while (libcp_equal(tok, "const") || libcp_equal(tok, "volatile") || libcp_equal(tok, "restrict") ||
           libcp_equal(tok, "__restrict") || libcp_equal(tok, "__restrict__"))
      tok = tok->next;
  }
  *rest = tok;
  return ty;
}

// declarator = pointers ("(" ident ")" | "(" declarator ")" | ident) type-suffix
//
// The declarator layer builds the type shape and leaves everything it
// cannot decide syntactically (array dimensions, typeof operands) as
// records for sema to complete.
static LibcpType *declarator(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *ty, LibcpDeclName *decl) {
  if (++ctx->parse_depth > MAX_PARSE_DEPTH)
    libcp_error_tok(ctx, tok, "nesting too deep");
  LibcpType *node = declarator1(ctx, rest, tok, ty, decl);
  ctx->parse_depth--;
  return node;
}

static LibcpType *declarator1(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *ty, LibcpDeclName *decl) {
  ty = pointers(ctx, &tok, tok, ty);

  if (libcp_equal(tok, "(")) {
    LibcpToken *start = tok;
    LibcpType dummy = {};
    declarator(ctx, &tok, start->next, &dummy, NULL);
    tok = libcp_skip(ctx, tok, ")");
    ty = type_suffix(ctx, rest, tok, ty);
    return declarator(ctx, &tok, start->next, ty, decl);
  }

  LibcpToken *name = NULL;
  LibcpToken *name_pos = tok;

  if (tok->kind == TK_IDENT) {
    name = tok;
    tok = tok->next;
  }

  ty = type_suffix(ctx, rest, tok, ty);
  // A named declaration accepts trailing attributes here. The type
  // meaning is ignored for a named declaration, so a shared type record
  // is never mutated.
  tok = *rest;
  tok = attribute_list(ctx, tok, NULL);
  *rest = tok;
  if (decl) {
    decl->name = name;
    decl->name_pos = name_pos;
  }
  return ty;
}

// abstract-declarator = pointers ("(" abstract-declarator ")")? type-suffix
static LibcpType *abstract_declarator(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *ty) {
  if (++ctx->parse_depth > MAX_PARSE_DEPTH)
    libcp_error_tok(ctx, tok, "nesting too deep");
  LibcpType *node = abstract_declarator1(ctx, rest, tok, ty);
  ctx->parse_depth--;
  return node;
}

static LibcpType *abstract_declarator1(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *ty) {
  ty = pointers(ctx, &tok, tok, ty);

  if (libcp_equal(tok, "(")) {
    LibcpToken *start = tok;
    LibcpType dummy = {};
    abstract_declarator(ctx, &tok, start->next, &dummy);
    tok = libcp_skip(ctx, tok, ")");
    ty = type_suffix(ctx, rest, tok, ty);
    return abstract_declarator(ctx, &tok, start->next, ty);
  }

  ty = type_suffix(ctx, rest, tok, ty);
  tok = *rest;
  tok = attribute_list(ctx, tok, NULL);
  *rest = tok;
  return ty;
}

// type-name = declspec abstract-declarator
static LibcpType *typename(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpNode **specs) {
  LibcpType *ty = declspec(ctx, &tok, tok, NULL, specs);
  return abstract_declarator(ctx, rest, tok, ty);
}

static bool consume_end(LibcpToken **rest, LibcpToken *tok) {
  if (libcp_equal(tok, "}")) {
    *rest = tok->next;
    return true;
  }

  if (libcp_equal(tok, ",") && libcp_equal(tok->next, "}")) {
    *rest = tok->next->next;
    return true;
  }

  return false;
}

// enum-specifier = ident? "{" enum-list? "}"
//                | ident ("{" enum-list? "}")?
//
// enum-list      = ident ("=" num)? ("," ident ("=" num)?)* ","?
static LibcpType *enum_specifier(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpNode **specs) {
  LibcpType *ty = libcp_enum_type(ctx);

  // Read a struct tag.
  LibcpToken *tag = NULL;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }
  if (tag)
    ty->tag_tok = tag;

  if (tag && !libcp_equal(tok, "{")) {
    LibcpType *ty = find_tag(ctx, tag);
    if (!ty)
      libcp_error_tok(ctx, tag, "unknown enum type");
    if (ty->kind != TY_ENUM)
      libcp_error_tok(ctx, tag, "not an enum tag");
    *rest = tok;
    return ty;
  }

  tok = libcp_skip(ctx, tok, "{");

  // Read an enum-list. Each member becomes a declaration record node
  // carrying the name, the optional explicit value expression and the
  // enum type; sema evaluates the values and registers the names at
  // this position of the enclosing chain.
  int i = 0;
  while (!consume_end(rest, tok)) {
    if (i++ > 0)
      tok = libcp_skip(ctx, tok, ",");

    LibcpToken *name = tok;
    tok = tok->next;
    add_declared_name(ctx, name);

    LibcpNode *node = libcp_new_node(ctx, ND_ENUM_CONST, name);
    if (libcp_equal(tok, "="))
      node->lhs = libcp_conditional(ctx, &tok, tok->next);
    node->ty_op = ty;

    add_spec(specs, node);
  }

  if (tag)
    push_tag_scope(ctx, tag, ty);
  return ty;
}

// typeof-specifier = "(" (expr | typename) ")"
static LibcpType *typeof_specifier(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpNode **specs) {
  tok = libcp_skip(ctx, tok, "(");

  LibcpType *ty;
  if (is_typename(ctx, tok)) {
    ty = typename(ctx, &tok, tok, specs);
  } else {
    // What type an expression has is not a syntax question, so the
    // operand is recorded in a placeholder the declarator builds on;
    // sema annotates the expression and fills the record in.
    ty = libcp_typeof_placeholder(ctx);
    ty->typeof_expr = expr(ctx, &tok, tok);
  }
  *rest = libcp_skip(ctx, tok, ")");
  return ty;
}

// declaration = declspec (declarator ("=" initializer)? ("," declarator ("=" initializer)?)*)? ";"
//
// Each declarator emits an ND_DECL record anchored at the name it
// declares: the name comes from the declarator's local record, the
// storage class and alignment from `attr`, and the initializer stays a
// faithful record.
// The records go straight into the enclosing statement chain - a
// declaration is not a block in the source, so the block that carries
// them is the one the source wrote. sema declares the object when it
// resolves the record and lowers the node when it types it - including
// the VLA-size sibling statement and the block-scope static's data
// image, whose shapes it reproduces exactly.
static LibcpNode *declaration(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *basety, LibcpVarAttr *attr) {
  LibcpNode head = {};
  LibcpNode *cur = &head;
  int i = 0;

  while (!libcp_equal(tok, ";")) {
    if (i++ > 0)
      tok = libcp_skip(ctx, tok, ",");

    LibcpDeclName decl_name = {};
    LibcpType *ty = declarator(ctx, &tok, tok, basety, &decl_name);
    if (!decl_name.name)
      libcp_error_tok(ctx, decl_name.name_pos, "variable name omitted");

    // The declared name shadows a typedef of the same name for the rest
    // of the parse - before the initializer is parsed, as the old
    // declaration site did.
    LibcpToken *name = decl_name.name;
    add_declared_name(ctx, name);

    LibcpInitializer *init = NULL;
    if (libcp_equal(tok, "=")) {
      LibcpToken *eq = tok;
      init = init_record(ctx, &tok, tok->next);
      init->eq_tok = eq;
    }

    LibcpNode *decl = libcp_new_node(ctx, ND_DECL, name);
    decl->ty_op = ty;
    decl->name_tok = name;
    if (attr)
      decl->attr = *attr;
    decl->decl_init = init;
    cur = cur->next = decl;
  }

  *rest = tok->next;
  return head.next;
}

// designator = "[" conditional-expr ("]" | "..." conditional-expr "]")
//            | "." ident
//
// C99 added the designated initializer to the language, which allows
// programmers to move the "cursor" of an initializer to any element.
// The syntax looks like this:
//
//   int x[10] = { 1, 2, [5]=3, 4, 5, 6, 7 };
//
// `[5]` moves the cursor to the 5th element, so the 5th element of x
// is set to 3. Initialization then continues forward in order, so
// 6th, 7th, 8th and 9th elements are initialized with 4, 5 and 6 and 7,
// respectively. Unspecified elements (in this case, 3rd and 4th
// elements) are initialized with zero.
//
// Nesting is allowed, so the following initializer is valid:
//
//   int x[5][10] = { [5][8]=1, 2, 3 };
//
// It sets x[5][8], x[5][9] and x[6][0] to 1, 2 and 3, respectively.
//
// Use `.fieldname` to move the cursor for a struct initializer. E.g.
//
//   struct { int a, b, c; } x = { .c=5 };
//
// The above initializer sets x.c to 5.
//
// Designators are recorded as written: index expressions stay
// unevaluated and member names stay unbound; sema evaluates and
// applies them against the target type.
static LibcpInitDesig *designators(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok) {
  LibcpInitDesig head = {};
  LibcpInitDesig *cur = &head;

  while (1) {
    if (libcp_equal(tok, "[")) {
      LibcpInitDesig *desig = libcp_arena_calloc(ctx, 1, sizeof(LibcpInitDesig));
      desig->tok = tok;
      desig->begin = libcp_conditional(ctx, &tok, tok->next);
      desig->after_begin = tok;
      if (libcp_equal(tok, "..."))
        desig->end = libcp_conditional(ctx, &tok, tok->next);
      desig->rbracket = tok;
      tok = libcp_skip(ctx, tok, "]");
      cur = cur->next = desig;
      continue;
    }

    if (libcp_equal(tok, ".")) {
      LibcpInitDesig *desig = libcp_arena_calloc(ctx, 1, sizeof(LibcpInitDesig));
      desig->tok = tok;
      tok = tok->next;
      if (tok->kind != TK_IDENT)
        libcp_error_tok(ctx, tok, "expected a field designator");
      desig->name = tok;
      tok = tok->next;
      cur = cur->next = desig;
      continue;
    }

    break;
  }

  *rest = tok;
  return head.next;
}

// initializer-record = brace-list | string-literal | assign-expression
// brace-list = "{" item ("," item)* ","? "}"
// item = designator* "="? initializer-record
//
// The record is faithful: elements keep their written shape (braced
// sublist, string literal or expression) and no decision that needs
// the target type is made here. Brace elision, string expansion,
// designator application and flexible-array sizing all happen in sema.
static LibcpInitializer *init_record(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok) {
  if (++ctx->parse_depth > MAX_PARSE_DEPTH)
    libcp_error_tok(ctx, tok, "nesting too deep");
  LibcpInitializer *node = init_record1(ctx, rest, tok);
  ctx->parse_depth--;
  return node;
}

static LibcpInitializer *init_record1(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok) {
  LibcpInitializer *rec = libcp_arena_calloc(ctx, 1, sizeof(LibcpInitializer));
  rec->tok = tok;

  if (libcp_equal(tok, "{")) {
    rec->kind = INIT_LIST;
    tok = tok->next;

    LibcpInitItem head = {};
    LibcpInitItem *cur = &head;
    bool first = true;

    while (!consume_end(&tok, tok)) {
      LibcpInitItem *item = libcp_arena_calloc(ctx, 1, sizeof(LibcpInitItem));
      if (!first) {
        item->comma_tok = tok;
        tok = libcp_skip(ctx, tok, ",");
      }
      first = false;

      item->desigs = designators(ctx, &tok, tok);
      if (item->desigs && libcp_equal(tok, "="))
        tok = tok->next;
      item->init = init_record(ctx, &tok, tok);
      cur = cur->next = item;
    }

    rec->items = head.next;
    *rest = tok;
    return rec;
  }

  // A string literal is a string initializer only when it is the whole
  // initializer. If an operator or a postfix follows, the literal starts
  // an ordinary expression (`"ab" + 1`, `"8.6" ? 0 : 1`, `"ab"[0]`).
  if (tok->kind == TK_STR && (libcp_equal(tok->next, ",") ||
                              libcp_equal(tok->next, "}") ||
                              libcp_equal(tok->next, ";") ||
                              tok->next->kind == TK_EOF)) {
    rec->kind = INIT_STR;
    rec->str_tok = tok;
    *rest = tok->next;
    return rec;
  }

  rec->kind = INIT_EXPR;
  rec->expr = assign(ctx, rest, tok);
  return rec;
}

// Returns true if a given token represents a type.
static bool is_typename(LibcpContext *ctx, LibcpToken *tok) {
  LibcpHashMap *map = &ctx->typename_map;

  if (map->capacity == 0) {
    static char *kw[] = {
      "void", "_Bool", "char", "short", "int", "long", "struct", "union",
      "typedef", "enum", "static", "extern", "_Alignas", "signed", "unsigned",
      "const", "volatile", "auto", "register", "restrict", "__restrict",
      "__restrict__", "_Noreturn", "float", "double", "typeof", "inline",
      "_Thread_local", "__thread", "_Atomic",
    };

    for (int i = 0; i < sizeof(kw) / sizeof(*kw); i++)
      libcp_hashmap_put(ctx, map, kw[i], (void *)1);
  }

  return libcp_hashmap_get2(ctx, map, tok->loc, tok->len) || find_typedef(ctx, tok);
}

// asm-stmt = "asm" ("volatile" | "inline")* "(" string-literal ")"
static LibcpNode *asm_stmt(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok) {
  LibcpNode *node = libcp_new_node(ctx, ND_ASM, tok);
  tok = tok->next;

  while (libcp_equal(tok, "volatile") || libcp_equal(tok, "inline"))
    tok = tok->next;

  tok = libcp_skip(ctx, tok, "(");
  if (tok->kind != TK_STR || tok->ty->base->kind != TY_CHAR)
    libcp_error_tok(ctx, tok, "expected string literal");
  node->asm_str = tok->str;
  *rest = libcp_skip(ctx, tok->next, ")");
  return node;
}

// stmt = "return" expr? ";"
//      | "if" "(" expr ")" stmt ("else" stmt)?
//      | "switch" "(" expr ")" stmt
//      | "case" const-expr ("..." const-expr)? ":" stmt
//      | "default" ":" stmt
//      | "for" "(" expr-stmt expr? ";" expr? ")" stmt
//      | "while" "(" expr ")" stmt
//      | "do" stmt "while" "(" expr ")" ";"
//      | "asm" asm-stmt
//      | "goto" (ident | "*" expr) ";"
//      | "break" ";"
//      | "continue" ";"
//      | ident ":" stmt
//      | "{" compound-stmt
//      | expr-stmt
static LibcpNode *stmt(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok) {
  if (libcp_equal(tok, "return")) {
    // The implicit conversion to the return type is sema's: the parser
    // does not know the enclosing function.
    LibcpNode *node = libcp_new_node(ctx, ND_RETURN, tok);
    if (libcp_consume(rest, tok->next, ";"))
      return node;

    node->lhs = expr(ctx, &tok, tok->next);
    *rest = libcp_skip(ctx, tok, ";");
    return node;
  }

  if (libcp_equal(tok, "if")) {
    LibcpNode *node = libcp_new_node(ctx, ND_IF, tok);
    tok = libcp_skip(ctx, tok->next, "(");
    node->cond = expr(ctx, &tok, tok);
    tok = libcp_skip(ctx, tok, ")");
    node->then = stmt(ctx, &tok, tok);
    if (libcp_equal(tok, "else"))
      node->els = stmt(ctx, &tok, tok->next);
    *rest = tok;
    return node;
  }

  if (libcp_equal(tok, "switch")) {
    LibcpNode *node = libcp_new_node(ctx, ND_SWITCH, tok);
    tok = libcp_skip(ctx, tok->next, "(");
    node->cond = expr(ctx, &tok, tok);
    tok = libcp_skip(ctx, tok, ")");
    node->then = stmt(ctx, rest, tok);
    return node;
  }

  if (libcp_equal(tok, "case")) {
    // The values a case label stands for are constant expressions, so
    // they are recorded unevaluated; sema evaluates them into begin/end
    // when it resolves the node. `:` is where the range check is
    // anchored.
    LibcpNode *node = libcp_new_node(ctx, ND_CASE, tok);
    node->begin_expr = libcp_conditional(ctx, &tok, tok->next);

    if (libcp_equal(tok, "...")) {
      // [GNU] Case ranges, e.g. "case 1 ... 5:"
      node->end_expr = libcp_conditional(ctx, &tok, tok->next);
    }

    node->colon_tok = tok;
    tok = libcp_skip(ctx, tok, ":");
    node->lhs = stmt(ctx, rest, tok);
    return node;
  }

  if (libcp_equal(tok, "default")) {
    LibcpNode *node = libcp_new_node(ctx, ND_CASE, tok);
    node->is_default = true;
    tok = libcp_skip(ctx, tok->next, ":");
    node->lhs = stmt(ctx, rest, tok);
    return node;
  }

  if (libcp_equal(tok, "for")) {
    LibcpNode *node = libcp_new_node(ctx, ND_FOR, tok);
    tok = libcp_skip(ctx, tok->next, "(");

    enter_scope(ctx);

    if (is_typename(ctx, tok)) {
      LibcpNode *specs = NULL;
      LibcpType *basety = declspec(ctx, &tok, tok, NULL, &specs);
      // Enum records a for-init declspec defined share the init's
      // scope, so they ride at the head of the init chain. The init is
      // a chain of declaration records like any other declaration; sema
      // folds it into one statement once the records are lowered,
      // because codegen's `for` has a single-statement init slot.
      node->init = specs_then(specs, declaration(ctx, &tok, tok, basety, NULL));
    } else {
      node->init = expr_stmt(ctx, &tok, tok);
    }

    if (!libcp_equal(tok, ";"))
      node->cond = expr(ctx, &tok, tok);
    tok = libcp_skip(ctx, tok, ";");

    if (!libcp_equal(tok, ")"))
      node->inc = expr(ctx, &tok, tok);
    tok = libcp_skip(ctx, tok, ")");

    node->then = stmt(ctx, rest, tok);

    leave_scope(ctx);
    return node;
  }

  if (libcp_equal(tok, "while")) {
    LibcpNode *node = libcp_new_node(ctx, ND_WHILE, tok);
    tok = libcp_skip(ctx, tok->next, "(");
    node->cond = expr(ctx, &tok, tok);
    tok = libcp_skip(ctx, tok, ")");
    node->then = stmt(ctx, rest, tok);
    return node;
  }

  if (libcp_equal(tok, "do")) {
    LibcpNode *node = libcp_new_node(ctx, ND_DO, tok);
    node->then = stmt(ctx, &tok, tok->next);

    tok = libcp_skip(ctx, tok, "while");
    tok = libcp_skip(ctx, tok, "(");
    node->cond = expr(ctx, &tok, tok);
    tok = libcp_skip(ctx, tok, ")");
    *rest = libcp_skip(ctx, tok, ";");
    return node;
  }

  if (libcp_equal(tok, "asm"))
    return asm_stmt(ctx, rest, tok);

  if (libcp_equal(tok, "goto")) {
    if (libcp_equal(tok->next, "*")) {
      // [GNU] `goto *ptr` jumps to the address specified by `ptr`.
      LibcpNode *node = libcp_new_node(ctx, ND_GOTO_EXPR, tok);
      node->lhs = expr(ctx, &tok, tok->next->next);
      *rest = libcp_skip(ctx, tok, ";");
      return node;
    }

    LibcpNode *node = libcp_new_node(ctx, ND_GOTO, tok);
    node->label = libcp_get_ident(ctx, tok->next);
    *rest = libcp_skip(ctx, tok->next->next, ";");
    return node;
  }

  if (libcp_equal(tok, "break")) {
    *rest = libcp_skip(ctx, tok->next, ";");
    return libcp_new_node(ctx, ND_BREAK, tok);
  }

  if (libcp_equal(tok, "continue")) {
    *rest = libcp_skip(ctx, tok->next, ";");
    return libcp_new_node(ctx, ND_CONTINUE, tok);
  }

  if (tok->kind == TK_IDENT && libcp_equal(tok->next, ":")) {
    LibcpNode *node = libcp_new_node(ctx, ND_LABEL, tok);
    node->label = libcp_arena_strndup(ctx, tok->loc, tok->len);
    node->lhs = stmt(ctx, rest, tok->next->next);
    return node;
  }

  if (libcp_equal(tok, "{"))
    return compound_stmt(ctx, rest, tok->next);

  return expr_stmt(ctx, rest, tok);
}

// compound-stmt = (typedef | declaration | stmt)* "}"
//
// Declaration records that produce no statement of their own (typedefs,
// enum constants, block-scope extern declarations and [GNU] nested
// function definitions) enter the chain at their source position; sema
// removes them again before codegen sees the chain.
static LibcpNode *compound_stmt(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok) {
  LibcpNode *node = libcp_new_node(ctx, ND_BLOCK, tok);
  node->is_scope_block = true;
  LibcpNode head = {};
  LibcpNode *cur = &head;

  enter_scope(ctx);

  while (!libcp_equal(tok, "}")) {
    if (is_typename(ctx, tok) && !libcp_equal(tok->next, ":")) {
      LibcpVarAttr attr = {};
      LibcpNode *specs = NULL;
      LibcpType *basety = declspec(ctx, &tok, tok, &attr, &specs);
      cur = chain_append(cur, specs);

      if (attr.is_typedef) {
        cur = chain_append(cur, parse_typedef(ctx, &tok, tok, basety));
        continue;
      }

      if (is_function(ctx, tok)) {
        cur = cur->next = function_def(ctx, &tok, tok, basety, &attr);
        continue;
      }

      if (attr.is_extern) {
        cur = chain_append(cur, global_variable(ctx, &tok, tok, basety, &attr));
        continue;
      }

      cur = chain_append(cur, declaration(ctx, &tok, tok, basety, &attr));
    } else {
      cur = cur->next = stmt(ctx, &tok, tok);
    }
  }

  leave_scope(ctx);

  node->body = head.next;
  *rest = tok->next;
  return node;
}

// expr-stmt = expr? ";"
static LibcpNode *expr_stmt(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok) {
  if (libcp_equal(tok, ";")) {
    *rest = tok->next;
    return libcp_new_node(ctx, ND_BLOCK, tok);
  }

  LibcpNode *node = libcp_new_node(ctx, ND_EXPR_STMT, tok);
  node->lhs = expr(ctx, &tok, tok);
  *rest = libcp_skip(ctx, tok, ";");
  return node;
}



// struct-members = (declspec declarator (","  declarator)* ";")*
//
// LibcpMember placement (offsets, size, alignment, bitfield widths) and the
// flexible-array-member conversion are sema's; the member list here is
// the syntax shape, with `_Alignas` and bitfield widths recorded
// unevaluated.
static void struct_members(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *ty, LibcpNode **specs) {
  LibcpMember head = {};
  LibcpMember *cur = &head;
  int idx = 0;

  while (!libcp_equal(tok, "}")) {
    LibcpVarAttr attr = {};
    LibcpToken *decl_start = tok;
    LibcpType *basety = declspec(ctx, &tok, tok, &attr, specs);
    bool first = true;

    // An anonymous member is recognized by the kind of the base type.
    // A `typeof(expr)` base has not settled its kind yet, so the
    // placeholder counts as a candidate too; sema rejects a nameless
    // member that turns out not to be an aggregate.
    if ((basety->kind == TY_STRUCT || basety->kind == TY_UNION ||
         basety->kind == TY_TYPEOF) &&
        libcp_consume(&tok, tok, ";")) {
      LibcpMember *mem = libcp_arena_calloc(ctx, 1, sizeof(LibcpMember));
      mem->tok = decl_start;
      mem->ty = basety;
      mem->idx = idx++;
      mem->align_ty = attr.align_ty;
      mem->align_expr = attr.align_expr;
      cur = cur->next = mem;
      continue;
    }

    // Regular struct members
    while (!libcp_consume(&tok, tok, ";")) {
      if (!first)
        tok = libcp_skip(ctx, tok, ",");
      first = false;

      LibcpMember *mem = libcp_arena_calloc(ctx, 1, sizeof(LibcpMember));
      mem->tok = tok;
      LibcpDeclName decl = {};
      mem->ty = declarator(ctx, &tok, tok, basety, &decl);
      mem->name = decl.name;
      mem->idx = idx++;
      mem->align_ty = attr.align_ty;
      mem->align_expr = attr.align_expr;

      if (libcp_consume(&tok, tok, ":")) {
        mem->is_bitfield = true;
        // The width is a constant expression; it is recorded
        // unevaluated and sema evaluates it when the aggregate is
        // laid out.
        mem->width_expr = libcp_conditional(ctx, &tok, tok);
      }

      cur = cur->next = mem;
    }
  }

  *rest = tok->next;
  ty->members = head.next;
}

// attribute = ("__attribute__" "(" "(" attribute ("," attribute)* ")" ")")*
//
// `packed` and `aligned` are applied when a type record is given; the
// known declaration/function attributes are recognized and ignored.
static LibcpToken *skip_attribute_args(LibcpContext *ctx, LibcpToken *tok) {
  if (!libcp_equal(tok, "("))
    return tok;

  int depth = 0;
  for (; tok->kind != TK_EOF; tok = tok->next) {
    if (libcp_equal(tok, "(")) {
      depth++;
    } else if (libcp_equal(tok, ")")) {
      if (--depth == 0)
        return tok->next;
    }
  }
  libcp_error_tok(ctx, tok, "unclosed attribute");
  return tok;
}

static bool is_ignored_attribute(LibcpToken *tok) {
  static char *names[] = {
    "unused", "used", "noreturn", "format", "format_arg", "visibility",
    "deprecated", "warn_unused_result", "nonnull", "returns_nonnull",
    "malloc", "pure", "const", "always_inline", "noinline", "section",
    "weak", "alias", "constructor", "destructor", "may_alias", "mode",
    "fallthrough", "nodiscard", "access", "alloc_size", "hot", "cold",
    "gnu_inline", "sentinel", "nothrow", "leaf", "returns_twice",
    "artificial", "error", "warning",
    NULL,
  };
  for (int i = 0; names[i]; i++)
    if (libcp_equal(tok, names[i]))
      return true;
  return false;
}

static LibcpToken *attribute_list(LibcpContext *ctx, LibcpToken *tok, LibcpType *ty) {
  while (libcp_consume(&tok, tok, "__attribute__")) {
    tok = libcp_skip(ctx, tok, "(");
    tok = libcp_skip(ctx, tok, "(");

    bool first = true;

    while (!libcp_consume(&tok, tok, ")")) {
      if (!first)
        tok = libcp_skip(ctx, tok, ",");
      first = false;

      if (libcp_consume(&tok, tok, "packed")) {
        if (ty)
          ty->is_packed = true;
        continue;
      }

      if (libcp_consume(&tok, tok, "aligned")) {
        // The alignment is a constant expression. It is applied only when
        // the attribute is attached to a type definition; declaration
        // positions pass NULL so the shared type records stay read-only.
        if (libcp_consume(&tok, tok, "(")) {
          if (ty)
            ty->align_expr = libcp_conditional(ctx, &tok, tok);
          else
            (void)libcp_conditional(ctx, &tok, tok);
          tok = libcp_skip(ctx, tok, ")");
        }
        continue;
      }

      if (libcp_equal(tok, "cleanup")) {
        libcp_warn_tok(ctx, tok, "cleanup attribute is ignored");
        tok = skip_attribute_args(ctx, tok->next);
        continue;
      }

      if (is_ignored_attribute(tok)) {
        tok = skip_attribute_args(ctx, tok->next);
        continue;
      }

      libcp_error_tok(ctx, tok, "unknown attribute");
    }

    tok = libcp_skip(ctx, tok, ")");
  }

  return tok;
}

// struct-union-decl = attribute? ident? ("{" struct-members)?
static LibcpType *struct_union_decl(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpNode **specs) {
  LibcpType *ty = libcp_struct_type(ctx);
  tok = attribute_list(ctx, tok, ty);

  // Read a tag.
  LibcpToken *tag = NULL;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }
  if (tag)
    ty->tag_tok = tag;

  if (tag && !libcp_equal(tok, "{")) {
    *rest = tok;

    LibcpType *ty2 = find_tag(ctx, tag);
    if (ty2)
      return ty2;

    ty->size = -1;
    push_tag_scope(ctx, tag, ty);
    return ty;
  }

  tok = libcp_skip(ctx, tok, "{");

  // Construct a struct object. The member list is syntax; placing the
  // members is sema's job, which it does when the type is completed -
  // at the definition for a bare one, or at the first declaration that
  // resolves a type referring to it.
  struct_members(ctx, &tok, tok, ty, specs);
  *rest = attribute_list(ctx, tok, ty);
  ty->layout_pending = true;

  LibcpType *ret = ty;
  if (tag) {
    // If this is a redefinition, overwrite a previous type.
    // Otherwise, register the struct type.
    LibcpType *ty2 = find_current_tag(ctx, tag);
    if (ty2) {
      // A forward declaration (size -1) is completed here. A second
      // definition of the same tag in the same scope is a constraint
      // violation (C11 6.7.2.3p1), not a silent replacement.
      if (ty2->size >= 0)
        libcp_error_tok(ctx, tag, "redefinition of tag");
      *ty2 = *ty;
      ret = ty2;
    } else {
      push_tag_scope(ctx, tag, ty);
    }
  }

  return ret;
}

// struct-decl = struct-union-decl
static LibcpType *struct_decl(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpNode **specs) {
  LibcpType *ty = struct_union_decl(ctx, rest, tok, specs);
  ty->kind = TY_STRUCT;
  return ty;
}

// union-decl = struct-union-decl
static LibcpType *union_decl(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpNode **specs) {
  LibcpType *ty = struct_union_decl(ctx, rest, tok, specs);
  ty->kind = TY_UNION;
  return ty;
}

// Create a node representing a struct member access, such as foo.bar
// where foo is a struct and bar is a member name.
//
// C has a feature called "anonymous struct" which allows a struct to
// have another unnamed struct as a member like this:
//
//   struct { struct { int a; }; int b; } x;
//
// The members of an anonymous struct belong to the outer struct's
// member namespace. Therefore, in the above example, you can access
// member "a" of the anonymous struct as "x.a".
//
// Which member a name denotes is not a syntax question, so the node is
// left unbound: it carries the member name token and, for `x->y`, the
// `->` token in `arrow_tok`. sema finds the member (flattening the
// anonymous ones in between), checks the operand and re-inserts the
// dereference.
static LibcpNode *struct_ref(LibcpContext *ctx, LibcpNode *node, LibcpToken *tok, LibcpToken *arrow) {
  LibcpNode *mem = libcp_new_unary(ctx, ND_MEMBER, node, tok);
  mem->arrow_tok = arrow;
  return mem;
}


// funcall = (assign ("," assign)*)? ")"
//
// The callee check, the argument conversions and the struct-return
// buffer are sema's; the node stays faithful, anchored at the closing
// paren where the argument-count diagnostics point.
static LibcpNode *funcall(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpNode *fn) {
  LibcpNode head = {};
  LibcpNode *cur = &head;

  while (!libcp_equal(tok, ")")) {
    if (cur != &head)
      tok = libcp_skip(ctx, tok, ",");

    cur = cur->next = assign(ctx, &tok, tok);
  }

  *rest = libcp_skip(ctx, tok, ")");

  LibcpNode *node = libcp_new_unary(ctx, ND_FUNCALL, fn, tok);
  node->args = head.next;
  return node;
}

// generic-selection = "(" assign "," generic-assoc ("," generic-assoc)* ")"
//
// generic-assoc = type-name ":" assign
//               | "default" ":" assign
//
// Which association the controlling expression's type selects is not a
// syntax question, so the selection is left to sema: the node keeps the
// controlling expression in `cond` and one ND_GENERIC_ASSOC per
// association in `args`.
static LibcpNode *generic_selection(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok) {
  LibcpNode *node = libcp_new_node(ctx, ND_GENERIC, tok);

  tok = libcp_skip(ctx, tok, "(");
  node->cond = assign(ctx, &tok, tok);

  LibcpNode head = {};
  LibcpNode *cur = &head;

  while (!libcp_consume(rest, tok, ")")) {
    tok = libcp_skip(ctx, tok, ",");

    LibcpNode *assoc = libcp_new_node(ctx, ND_GENERIC_ASSOC, tok);

    // A "default:" association is the one that carries no type name.
    if (libcp_equal(tok, "default")) {
      tok = libcp_skip(ctx, tok->next, ":");
    } else {
      LibcpNode *specs = NULL;
      assoc->ty_op = typename(ctx, &tok, tok, &specs);
      assoc->spec_decls = specs;
      tok = libcp_skip(ctx, tok, ":");
    }
    assoc->lhs = assign(ctx, &tok, tok);

    cur = cur->next = assoc;
  }

  node->args = head.next;
  return node;
}

// primary = "(" "{" stmt+ "}" ")"
//         | "(" expr ")"
//         | "sizeof" "(" type-name ")"
//         | "sizeof" unary
//         | "_Alignof" "(" type-name ")"
//         | "_Alignof" unary
//         | "_Generic" generic-selection
//         | "__builtin_types_compatible_p" "(" type-name, type-name, ")"
//         | "__builtin_reg_class" "(" type-name ")"
//         | ident
//         | str
//         | num
static LibcpNode *primary(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok) {
  LibcpToken *start = tok;

  if (libcp_equal(tok, "(") && libcp_equal(tok->next, "{")) {
    // This is a GNU statement expresssion.
    LibcpNode *node = libcp_new_node(ctx, ND_STMT_EXPR, tok);
    node->body = compound_stmt(ctx, &tok, tok->next->next)->body;
    *rest = libcp_skip(ctx, tok, ")");
    return node;
  }

  if (libcp_equal(tok, "(")) {
    LibcpNode *node = expr(ctx, &tok, tok->next);
    *rest = libcp_skip(ctx, tok, ")");
    return node;
  }

  if (libcp_equal(tok, "sizeof") && libcp_equal(tok->next, "(") && is_typename(ctx, tok->next->next)) {
    LibcpNode *specs = NULL;
    LibcpType *ty = typename(ctx, &tok, tok->next->next, &specs);
    *rest = libcp_skip(ctx, tok, ")");

    // Keep sizeof faithful; sema folds it to its value.
    LibcpNode *node = libcp_new_node(ctx, ND_SIZEOF, start);
    node->ty_op = ty;
    node->spec_decls = specs;
    return node;
  }

  if (libcp_equal(tok, "sizeof")) {
    // Keep sizeof faithful; sema folds it to its value.
    LibcpNode *node = libcp_new_node(ctx, ND_SIZEOF, tok);
    node->lhs = unary(ctx, rest, tok->next);
    return node;
  }

  if (libcp_equal(tok, "_Alignof") && libcp_equal(tok->next, "(") && is_typename(ctx, tok->next->next)) {
    LibcpNode *specs = NULL;
    LibcpType *ty = typename(ctx, &tok, tok->next->next, &specs);
    *rest = libcp_skip(ctx, tok, ")");

    LibcpNode *node = libcp_new_node(ctx, ND_ALIGNOF, start);
    node->ty_op = ty;
    node->spec_decls = specs;
    return node;
  }

  if (libcp_equal(tok, "_Alignof")) {
    LibcpNode *node = libcp_new_node(ctx, ND_ALIGNOF, tok);
    node->lhs = unary(ctx, rest, tok->next);
    return node;
  }

  if (libcp_equal(tok, "_Generic"))
    return generic_selection(ctx, rest, tok->next);

  if (libcp_equal(tok, "__builtin_types_compatible_p")) {
    // Both type operands are recorded; sema folds the node to 0 or 1.
    LibcpNode *node = libcp_new_node(ctx, ND_TYPES_COMPATIBLE, start);
    LibcpNode *specs = NULL;
    tok = libcp_skip(ctx, tok->next, "(");
    node->ty_op = typename(ctx, &tok, tok, &specs);
    tok = libcp_skip(ctx, tok, ",");
    node->ty_op2 = typename(ctx, &tok, tok, &specs);
    node->spec_decls = specs;
    *rest = libcp_skip(ctx, tok, ")");
    return node;
  }

  if (libcp_equal(tok, "__builtin_reg_class")) {
    // sema folds the node to the register class of its type operand.
    LibcpNode *node = libcp_new_node(ctx, ND_REG_CLASS, start);
    LibcpNode *specs = NULL;
    tok = libcp_skip(ctx, tok->next, "(");
    node->ty_op = typename(ctx, &tok, tok, &specs);
    node->spec_decls = specs;
    *rest = libcp_skip(ctx, tok, ")");
    return node;
  }

  if (libcp_equal(tok, "__builtin_compare_and_swap")) {
    LibcpNode *node = libcp_new_node(ctx, ND_CAS, tok);
    tok = libcp_skip(ctx, tok->next, "(");
    node->cas_addr = assign(ctx, &tok, tok);
    tok = libcp_skip(ctx, tok, ",");
    node->cas_old = assign(ctx, &tok, tok);
    tok = libcp_skip(ctx, tok, ",");
    node->cas_new = assign(ctx, &tok, tok);
    *rest = libcp_skip(ctx, tok, ")");
    return node;
  }

  if (libcp_equal(tok, "__builtin_atomic_exchange")) {
    LibcpNode *node = libcp_new_node(ctx, ND_EXCH, tok);
    tok = libcp_skip(ctx, tok->next, "(");
    node->lhs = assign(ctx, &tok, tok);
    tok = libcp_skip(ctx, tok, ",");
    node->rhs = assign(ctx, &tok, tok);
    *rest = libcp_skip(ctx, tok, ")");
    return node;
  }

  if (tok->kind == TK_IDENT) {
    // A reference to a variable, a function or an enum constant. Which
    // one it denotes is not a syntax question, so the reference stays
    // faithful and sema's resolve pass binds it, using the scope stack
    // the tree structure gives it.
    LibcpNode *node = libcp_new_node(ctx, ND_IDENT, tok);
    *rest = tok->next;
    return node;
  }

  if (tok->kind == TK_STR) {
    // Keep the literal faithful; sema lowers it to a reference to an
    // anonymous global when it types the node.
    LibcpNode *node = libcp_new_node(ctx, ND_STRING, tok);
    *rest = tok->next;
    return node;
  }

  if (tok->kind == TK_NUM) {
    LibcpNode *node;
    if (libcp_is_flonum(tok->ty)) {
      node = libcp_new_node(ctx, ND_NUM, tok);
      node->fval = tok->fval;
    } else {
      node = libcp_new_num(ctx, tok->val, tok);
    }

    node->ty = tok->ty;
    *rest = tok->next;
    return node;
  }

  libcp_error_tok(ctx, tok, "expected an expression");
}

//
// The Pratt expression parser (PLAN-pratt-parse).
//
// One loop replaces the per-precedence cascade: infix_bp() turns the
// current token into an operator and a binding power, and expr_bp() keeps
// consuming while the operator binds tighter than min_bp (left
// associative: rbp == lbp; right associative: rbp == lbp - 1). The three
// entry points keep the old names and meanings: expr allows comma, assign
// allows assignment, libcp_conditional allows neither. The prefix forms are in
// expr_nud; the postfix forms are the led section of the loop below.
enum {
  BP_COMMA = 1,
  BP_ASSIGN = 2,
  BP_COND = 3,
  BP_LOGOR = 4,
  BP_LOGAND = 5,
  BP_BITOR = 6,
  BP_BITXOR = 7,
  BP_BITAND = 8,
  BP_EQUALITY = 9,
  BP_RELATIONAL = 10,
  BP_SHIFT = 11,
  BP_ADD = 12,
  BP_MUL = 13,
  BP_CAST = 14,
  BP_POSTFIX = 15,
};

// The binding-power table: one row per infix spelling. The comma row is
// left-associative (rbp == lbp), assignment rows are right-associative
// (rbp == lbp - 1), and ?: has its own branch in expr_bp. A linear scan
// is enough here; there are fewer than forty spellings and this is not a
// hot loop.
typedef struct {
  char *op;
  LibcpNodeKind kind;
  LibcpNodeKind assign_op;
  int lbp;
  int rbp;
} LibcpInfixOp;

static const LibcpInfixOp infix_ops[] = {
  {",",   ND_COMMA,  ND_NULL_EXPR, BP_COMMA,      BP_COMMA},
  {"=",   ND_ASSIGN, ND_NULL_EXPR, BP_ASSIGN,     BP_ASSIGN - 1},
  {"+=",  ND_ASSIGN, ND_ADD,       BP_ASSIGN,     BP_ASSIGN - 1},
  {"-=",  ND_ASSIGN, ND_SUB,       BP_ASSIGN,     BP_ASSIGN - 1},
  {"*=",  ND_ASSIGN, ND_MUL,       BP_ASSIGN,     BP_ASSIGN - 1},
  {"/=",  ND_ASSIGN, ND_DIV,       BP_ASSIGN,     BP_ASSIGN - 1},
  {"%=",  ND_ASSIGN, ND_MOD,       BP_ASSIGN,     BP_ASSIGN - 1},
  {"&=",  ND_ASSIGN, ND_BITAND,    BP_ASSIGN,     BP_ASSIGN - 1},
  {"|=",  ND_ASSIGN, ND_BITOR,     BP_ASSIGN,     BP_ASSIGN - 1},
  {"^=",  ND_ASSIGN, ND_BITXOR,    BP_ASSIGN,     BP_ASSIGN - 1},
  {"<<=", ND_ASSIGN, ND_SHL,       BP_ASSIGN,     BP_ASSIGN - 1},
  {">>=", ND_ASSIGN, ND_SHR,       BP_ASSIGN,     BP_ASSIGN - 1},
  {"?",   ND_COND,   ND_NULL_EXPR, BP_COND,       BP_COND - 1},
  {"||",  ND_LOGOR,  ND_NULL_EXPR, BP_LOGOR,      BP_LOGOR},
  {"&&",  ND_LOGAND, ND_NULL_EXPR, BP_LOGAND,     BP_LOGAND},
  {"|",   ND_BITOR,  ND_NULL_EXPR, BP_BITOR,      BP_BITOR},
  {"^",   ND_BITXOR, ND_NULL_EXPR, BP_BITXOR,     BP_BITXOR},
  {"&",   ND_BITAND, ND_NULL_EXPR, BP_BITAND,     BP_BITAND},
  {"==",  ND_EQ,     ND_NULL_EXPR, BP_EQUALITY,   BP_EQUALITY},
  {"!=",  ND_NE,     ND_NULL_EXPR, BP_EQUALITY,   BP_EQUALITY},
  {"<",   ND_LT,     ND_NULL_EXPR, BP_RELATIONAL, BP_RELATIONAL},
  {"<=",  ND_LE,     ND_NULL_EXPR, BP_RELATIONAL, BP_RELATIONAL},
  {">",   ND_GT,     ND_NULL_EXPR, BP_RELATIONAL, BP_RELATIONAL},
  {">=",  ND_GE,     ND_NULL_EXPR, BP_RELATIONAL, BP_RELATIONAL},
  {"<<",  ND_SHL,    ND_NULL_EXPR, BP_SHIFT,      BP_SHIFT},
  {">>",  ND_SHR,    ND_NULL_EXPR, BP_SHIFT,      BP_SHIFT},
  {"+",   ND_ADD,    ND_NULL_EXPR, BP_ADD,        BP_ADD},
  {"-",   ND_SUB,    ND_NULL_EXPR, BP_ADD,        BP_ADD},
  {"*",   ND_MUL,    ND_NULL_EXPR, BP_MUL,        BP_MUL},
  {"/",   ND_DIV,    ND_NULL_EXPR, BP_MUL,        BP_MUL},
  {"%",   ND_MOD,    ND_NULL_EXPR, BP_MUL,        BP_MUL},
};

// The infix operator at tok, if any: its node kind, the compound
// assignment operator (ND_NULL_EXPR for a plain "=" or a non-assignment),
// and its binding powers.
static bool infix_bp(LibcpToken *tok, LibcpNodeKind *kind, LibcpNodeKind *assign_op,
                     int *lbp, int *rbp) {
  int n = sizeof(infix_ops) / sizeof(*infix_ops);

  for (int i = 0; i < n; i++) {
    if (!libcp_equal(tok, infix_ops[i].op))
      continue;
    *kind = infix_ops[i].kind;
    *assign_op = infix_ops[i].assign_op;
    *lbp = infix_ops[i].lbp;
    *rbp = infix_ops[i].rbp;
    return true;
  }
  return false;
}

static LibcpNode *expr_bp(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, int min_bp);

// The prefix (nud) forms: a cast or compound literal (the `(` type-name
// lookahead), the unary operators, labels-as-values, then the shared
// primary parser for everything atomic. Unary `+` is transparent and
// builds no node, exactly as the cascade did.
static LibcpNode *expr_nud(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok) {
  if (libcp_equal(tok, "(") && is_typename(ctx, tok->next)) {
    LibcpToken *start = tok;
    LibcpNode *specs = NULL;
    LibcpType *ty = typename(ctx, &tok, tok->next, &specs);
    tok = libcp_skip(ctx, tok, ")");

    // A compound literal. Kept faithful, with the type and the
    // initializer record on the node; sema materializes the object.
    if (libcp_equal(tok, "{")) {
      LibcpNode *node = libcp_new_node(ctx, ND_COMPOUND_LITERAL, start);
      node->ty_op = ty;
      node->spec_decls = specs;
      node->decl_init = init_record(ctx, rest, tok);
      return node;
    }

    // A cast the source wrote: untyped, with the target type recorded
    // for sema. is_implicit stays false (libcp_new_cast sets it for the casts
    // sema inserts).
    LibcpNode *node = libcp_new_node(ctx, ND_CAST, start);
    node->lhs = expr_bp(ctx, rest, tok, BP_CAST);
    node->ty_op = ty;
    node->spec_decls = specs;
    return node;
  }

  if (libcp_equal(tok, "+"))
    return expr_bp(ctx, rest, tok->next, BP_CAST);

  if (libcp_equal(tok, "-"))
    return libcp_new_unary(ctx, ND_NEG, expr_bp(ctx, rest, tok->next, BP_CAST), tok);

  if (libcp_equal(tok, "&"))
    return libcp_new_unary(ctx, ND_ADDR, expr_bp(ctx, rest, tok->next, BP_CAST), tok);

  if (libcp_equal(tok, "*"))
    return libcp_new_unary(ctx, ND_DEREF, expr_bp(ctx, rest, tok->next, BP_CAST), tok);

  if (libcp_equal(tok, "!"))
    return libcp_new_unary(ctx, ND_NOT, expr_bp(ctx, rest, tok->next, BP_CAST), tok);

  if (libcp_equal(tok, "~"))
    return libcp_new_unary(ctx, ND_BITNOT, expr_bp(ctx, rest, tok->next, BP_CAST), tok);

  if (libcp_equal(tok, "++")) {
    LibcpNode *node = libcp_new_node(ctx, ND_INCDEC, tok);
    node->lhs = expr_bp(ctx, rest, tok->next, BP_CAST);
    node->addend = 1;
    return node;
  }

  if (libcp_equal(tok, "--")) {
    LibcpNode *node = libcp_new_node(ctx, ND_INCDEC, tok);
    node->lhs = expr_bp(ctx, rest, tok->next, BP_CAST);
    node->addend = -1;
    return node;
  }

  // [GNU] labels-as-values. Checked before any infix interpretation, so
  // `&&label` is not a binary `&&`.
  if (libcp_equal(tok, "&&")) {
    LibcpNode *node = libcp_new_node(ctx, ND_LABEL_VAL, tok);
    node->label = libcp_get_ident(ctx, tok->next);
    *rest = tok->next->next;
    return node;
  }

  return primary(ctx, rest, tok);
}

// The one expression loop. The depth gate counts here and only here; the
// entry wrappers below do not add a frame.
static LibcpNode *expr_bp(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, int min_bp) {
  if (++ctx->parse_depth > MAX_PARSE_DEPTH)
    libcp_error_tok(ctx, tok, "nesting too deep");

  LibcpNode *node = expr_nud(ctx, rest, tok);

  for (;;) {
    LibcpToken *op = *rest;
    LibcpNodeKind kind;
    LibcpNodeKind assign_op;
    int lbp;
    int rbp;

    // The postfix (led) forms bind tighter than every infix operator:
    // call, subscript, member access and postfix increment/decrement.
    if (min_bp < BP_POSTFIX) {
      if (libcp_equal(op, "(")) {
        node = funcall(ctx, rest, op->next, node);
        continue;
      }

      if (libcp_equal(op, "[")) {
        LibcpToken *idx = op->next;
        LibcpNode *subscript = libcp_new_node(ctx, ND_SUBSCRIPT, op);
        subscript->lhs = node;
        subscript->rhs = expr(ctx, &idx, idx);
        *rest = libcp_skip(ctx, idx, "]");
        node = subscript;
        continue;
      }

      if (libcp_equal(op, ".")) {
        node = struct_ref(ctx, node, op->next, NULL);
        *rest = op->next->next;
        continue;
      }

      if (libcp_equal(op, "->")) {
        node = struct_ref(ctx, node, op->next, op);
        *rest = op->next->next;
        continue;
      }

      if (libcp_equal(op, "++")) {
        LibcpNode *incdec = libcp_new_node(ctx, ND_INCDEC, op);
        incdec->lhs = node;
        incdec->is_post = true;
        incdec->addend = 1;
        node = incdec;
        *rest = op->next;
        continue;
      }

      if (libcp_equal(op, "--")) {
        LibcpNode *incdec = libcp_new_node(ctx, ND_INCDEC, op);
        incdec->lhs = node;
        incdec->is_post = true;
        incdec->addend = -1;
        node = incdec;
        *rest = op->next;
        continue;
      }
    }

    if (!infix_bp(op, &kind, &assign_op, &lbp, &rbp) || lbp <= min_bp)
      break;

    if (kind == ND_COND) {
      LibcpNode *cond = libcp_new_node(ctx, ND_COND, op);

      if (libcp_equal(op->next, ":")) {
        // [GNU] `a ?: b`: only cond and els, with is_elvis set.
        cond->is_elvis = true;
        cond->cond = node;
        cond->els = expr_bp(ctx, rest, op->next->next, BP_COND - 1);
        node = cond;
        continue;
      }

      LibcpToken *mid = NULL;
      cond->cond = node;
      cond->then = expr_bp(ctx, &mid, op->next, 0);
      LibcpToken *colon = libcp_skip(ctx, mid, ":");
      cond->els = expr_bp(ctx, rest, colon, BP_COND - 1);
      node = cond;
      continue;
    }

    if (kind == ND_ASSIGN) {
      LibcpNode *rhs = expr_bp(ctx, rest, op->next, rbp);
      LibcpNode *assign = libcp_new_binary(ctx, ND_ASSIGN, node, rhs, op);
      assign->op = assign_op;
      node = assign;
      continue;
    }

    node = libcp_new_binary(ctx, kind, node, expr_bp(ctx, rest, op->next, rbp), op);
  }

  ctx->parse_depth--;
  return node;
}

// primary's `sizeof expr` and `_Alignof expr` forms parse their operand
// through the `unary` name; here that is the cast layer.
static LibcpNode *unary(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok) {
  return expr_bp(ctx, rest, tok, BP_CAST);
}

static LibcpNode *expr(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok) {
  return expr_bp(ctx, rest, tok, 0);
}

static LibcpNode *assign(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok) {
  return expr_bp(ctx, rest, tok, BP_ASSIGN - 1);
}

LibcpNode *libcp_conditional(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok) {
  return expr_bp(ctx, rest, tok, BP_COND - 1);
}

// Parses typedef declarators into ND_TYPEDEF records. The name also
// goes into the typedef-name oracle immediately: the grammar needs it
// to classify the very next identifier.
static LibcpNode *parse_typedef(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *basety) {
  LibcpNode head = {};
  LibcpNode *cur = &head;
  bool first = true;

  while (!libcp_consume(&tok, tok, ";")) {
    if (!first)
      tok = libcp_skip(ctx, tok, ",");
    first = false;

    LibcpDeclName decl = {};
    LibcpType *ty = declarator(ctx, &tok, tok, basety, &decl);
    if (!decl.name)
      libcp_error_tok(ctx, decl.name_pos, "typedef name omitted");

    LibcpNode *node = libcp_new_node(ctx, ND_TYPEDEF, decl.name);
    node->ty = ty;
    add_typedef(ctx, node);
    cur = cur->next = node;
  }

  *rest = tok;
  return head.next;
}

// Parses a function declarator, and its body if one follows, into an
// ND_FUNCDEF record. sema declares the function (checking it against a
// previous declaration, anchored at `tok`) and analyzes the body.
static LibcpNode *function_def(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *basety, LibcpVarAttr *attr) {
  LibcpDeclName decl = {};
  LibcpType *ty = declarator(ctx, &tok, tok, basety, &decl);
  if (!decl.name)
    libcp_error_tok(ctx, decl.name_pos, "function name omitted");

  LibcpNode *node = libcp_new_node(ctx, ND_FUNCDEF, tok);
  node->ty_op = ty;
  node->name_tok = decl.name;
  node->attr = *attr;
  add_declared_name(ctx, decl.name);

  if (libcp_consume(&tok, tok, ";")) {
    *rest = tok;
    return node;
  }

  // The parameters are visible in the whole body, so they shadow
  // typedefs of the same name from here on, one scope above the body's
  // own - where the old begin_function registered them.
  enter_scope(ctx);
  for (LibcpType *p = ty->params; p; p = p->next)
    if (p->name)
      add_declared_name(ctx, p->name);

  tok = libcp_skip(ctx, tok, "{");
  node->body = compound_stmt(ctx, &tok, tok);
  leave_scope(ctx);
  *rest = tok;
  return node;
}

// Parses global-variable declarators (at file scope, or `extern` ones
// inside a block) into ND_GVAR_DECL records; sema declares the objects
// and serializes their initializers.
static LibcpNode *global_variable(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *basety, LibcpVarAttr *attr) {
  LibcpNode head = {};
  LibcpNode *cur = &head;
  bool first = true;

  while (!libcp_consume(&tok, tok, ";")) {
    if (!first)
      tok = libcp_skip(ctx, tok, ",");
    first = false;

    LibcpDeclName decl = {};
    LibcpType *ty = declarator(ctx, &tok, tok, basety, &decl);
    if (!decl.name)
      libcp_error_tok(ctx, decl.name_pos, "variable name omitted");

    LibcpToken *name = decl.name;
    add_declared_name(ctx, name);

    LibcpNode *node = libcp_new_node(ctx, ND_GVAR_DECL, tok);
    node->ty_op = ty;
    node->name_tok = name;
    node->attr = *attr;
    if (libcp_equal(tok, "="))
      node->decl_init = init_record(ctx, &tok, tok->next);
    cur = cur->next = node;
  }

  *rest = tok;
  return head.next;
}

// Lookahead tokens and returns true if a given token is a start
// of a function definition or declaration.
static bool is_function(LibcpContext *ctx, LibcpToken *tok) {
  if (libcp_equal(tok, ";"))
    return false;

  LibcpType dummy = {};
  LibcpType *ty = declarator(ctx, &tok, tok, &dummy, NULL);
  return ty->kind == TY_FUNC;
}

// program = (typedef | function-definition | global-variable)*
//
// The top-level declaration-record chain this returns is what sema
// walks, in source order, to declare the objects and analyze the
// function bodies.
LibcpNode *libcp_parse(LibcpContext *ctx, LibcpToken *tok) {
  ENTER_RET(ctx, NULL);
  libcp_parse_init(ctx);
  LibcpNode head = {};
  LibcpNode *cur = &head;

  while (tok->kind != TK_EOF) {
    LibcpVarAttr attr = {};
    LibcpNode *specs = NULL;
    LibcpType *basety = declspec(ctx, &tok, tok, &attr, &specs);
    cur = chain_append(cur, specs);

    // Typedef
    if (attr.is_typedef) {
      cur = chain_append(cur, parse_typedef(ctx, &tok, tok, basety));
      continue;
    }

    // Function
    if (is_function(ctx, tok)) {
      cur = cur->next = function_def(ctx, &tok, tok, basety, &attr);
      continue;
    }

    // Global variable
    cur = chain_append(cur, global_variable(ctx, &tok, tok, basety, &attr));
  }

  LEAVE(ctx);
  return head.next;
}
