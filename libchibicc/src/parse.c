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

#include "chibicc.h"

static Type *find_typedef(ChibiccContext *ctx, Token *tok);
static Type *find_tag(ChibiccContext *ctx, Token *tok);
static Type *find_current_tag(ChibiccContext *ctx, Token *tok);
static void push_tag_scope(ChibiccContext *ctx, Token *tok, Type *ty);
static void add_typedef(ChibiccContext *ctx, Node *node);
static void add_declared_name(ChibiccContext *ctx, Token *tok);
static void enter_scope(ChibiccContext *ctx);
static void leave_scope(ChibiccContext *ctx);

// Bound the parser's recursion: pathological nesting must fail with a
// diagnostic, not a stack overflow.
#define MAX_PARSE_DEPTH 512

static bool is_typename(ChibiccContext *ctx, Token *tok);
static Token *attribute_list(ChibiccContext *ctx, Token *tok, Type *ty);
static Type *declspec(ChibiccContext *ctx, Token **rest, Token *tok, VarAttr *attr, Node **specs);
static Type *typename(ChibiccContext *ctx, Token **rest, Token *tok, Node **specs);
static Type *enum_specifier(ChibiccContext *ctx, Token **rest, Token *tok, Node **specs);
static Type *typeof_specifier(ChibiccContext *ctx, Token **rest, Token *tok, Node **specs);
static Type *type_suffix(ChibiccContext *ctx, Token **rest, Token *tok, Type *ty);

// The built-in types are shared by every context (type.c), and a tag or
// typedef type is shared by every declaration that names it. A
// declarator therefore never writes its name into the type record: the
// name travels in this per-declaration carrier instead (F1).
typedef struct {
  Token *name;     // NULL when the declarator has none
  Token *name_pos; // where the name would go, for "name omitted"
} DeclName;

static Type *declarator(ChibiccContext *ctx, Token **rest, Token *tok, Type *ty, DeclName *decl);
static Node *declaration(ChibiccContext *ctx, Token **rest, Token *tok, Type *basety, VarAttr *attr);
static Initializer *init_record(ChibiccContext *ctx, Token **rest, Token *tok);
static Node *compound_stmt(ChibiccContext *ctx, Token **rest, Token *tok);
static Node *stmt(ChibiccContext *ctx, Token **rest, Token *tok);
static Node *expr_stmt(ChibiccContext *ctx, Token **rest, Token *tok);
static Node *expr(ChibiccContext *ctx, Token **rest, Token *tok);
static Node *assign(ChibiccContext *ctx, Token **rest, Token *tok);
#if !CHIBICC_PRATT_PARSE_FEATURE
static Node *logor(ChibiccContext *ctx, Token **rest, Token *tok);
static Node *logand(ChibiccContext *ctx, Token **rest, Token *tok);
static Node *bitor(ChibiccContext *ctx, Token **rest, Token *tok);
static Node *bitxor(ChibiccContext *ctx, Token **rest, Token *tok);
static Node *bitand(ChibiccContext *ctx, Token **rest, Token *tok);
static Node *equality(ChibiccContext *ctx, Token **rest, Token *tok);
static Node *relational(ChibiccContext *ctx, Token **rest, Token *tok);
static Node *shift(ChibiccContext *ctx, Token **rest, Token *tok);
static Node *add(ChibiccContext *ctx, Token **rest, Token *tok);
static Node *mul(ChibiccContext *ctx, Token **rest, Token *tok);
#endif
#if !CHIBICC_PRATT_PARSE_FEATURE
static Node *cast(ChibiccContext *ctx, Token **rest, Token *tok);
#endif
static Type *struct_decl(ChibiccContext *ctx, Token **rest, Token *tok, Node **specs);
static Type *union_decl(ChibiccContext *ctx, Token **rest, Token *tok, Node **specs);
#if !CHIBICC_PRATT_PARSE_FEATURE
static Node *postfix(ChibiccContext *ctx, Token **rest, Token *tok);
#endif
static Node *funcall(ChibiccContext *ctx, Token **rest, Token *tok, Node *node);
static Node *unary(ChibiccContext *ctx, Token **rest, Token *tok);
// The body behind each depth-guarded entry (the wrapper increments
// parse_depth before calling it).
static Type *declarator1(ChibiccContext *ctx, Token **rest, Token *tok, Type *ty, DeclName *decl);
static Type *abstract_declarator(ChibiccContext *ctx, Token **rest, Token *tok, Type *ty);
static Type *abstract_declarator1(ChibiccContext *ctx, Token **rest, Token *tok, Type *ty);
static Initializer *init_record1(ChibiccContext *ctx, Token **rest, Token *tok);
#if !CHIBICC_PRATT_PARSE_FEATURE
static Node *expr1(ChibiccContext *ctx, Token **rest, Token *tok);
static Node *assign1(ChibiccContext *ctx, Token **rest, Token *tok);
static Node *conditional1(ChibiccContext *ctx, Token **rest, Token *tok);
#endif
#if !CHIBICC_PRATT_PARSE_FEATURE
static Node *cast1(ChibiccContext *ctx, Token **rest, Token *tok);
static Node *unary1(ChibiccContext *ctx, Token **rest, Token *tok);
#endif
static Node *primary(ChibiccContext *ctx, Token **rest, Token *tok);
static Node *parse_typedef(ChibiccContext *ctx, Token **rest, Token *tok, Type *basety);
static bool is_function(ChibiccContext *ctx, Token *tok);
static Node *function_def(ChibiccContext *ctx, Token **rest, Token *tok, Type *basety, VarAttr *attr);
static Node *global_variable(ChibiccContext *ctx, Token **rest, Token *tok, Type *basety, VarAttr *attr);

Node *new_node(ChibiccContext *ctx, NodeKind kind, Token *tok) {
  Node *node = chibicc_arena_calloc(ctx, 1, sizeof(Node));
  node->kind = kind;
  node->tok = tok;
  return node;
}

Node *new_binary(ChibiccContext *ctx, NodeKind kind, Node *lhs, Node *rhs, Token *tok) {
  Node *node = new_node(ctx, kind, tok);
  node->lhs = lhs;
  node->rhs = rhs;
  return node;
}

Node *new_unary(ChibiccContext *ctx, NodeKind kind, Node *expr, Token *tok) {
  Node *node = new_node(ctx, kind, tok);
  node->lhs = expr;
  return node;
}

Node *new_num(ChibiccContext *ctx, int64_t val, Token *tok) {
  Node *node = new_node(ctx, ND_NUM, tok);
  node->val = val;
  return node;
}

char *get_ident(ChibiccContext *ctx, Token *tok) {
  if (tok->kind != TK_IDENT)
    error_tok(ctx, tok, "expected an identifier");
  return chibicc_arena_strndup(ctx, tok->loc, tok->len);
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
  Type *type_def;
} NameEntry;

typedef struct ParseScope ParseScope;
struct ParseScope {
  ParseScope *next;

  // C has two block scopes; one is for ordinary names and the other is
  // for struct/union/enum tags.
  HashMap names;
  HashMap tags;
};

void parse_init(ChibiccContext *ctx) {
  ctx->scope = chibicc_arena_calloc(ctx, 1, sizeof(ParseScope));
}

// The parser's scope stack lives in ChibiccContext (PLAN L2.4).
#define scope (ctx->scope)

static void enter_scope(ChibiccContext *ctx) {
  ParseScope *sc = chibicc_arena_calloc(ctx, 1, sizeof(ParseScope));
  sc->next = scope;
  scope = sc;
}

static void leave_scope(ChibiccContext *ctx) {
  scope = scope->next;
}

// The parser's typedef-name oracle: the C grammar needs to know whether
// an identifier names a type before it can parse a declaration.
static Type *find_typedef(ChibiccContext *ctx, Token *tok) {
  if (tok->kind != TK_IDENT)
    return NULL;

  for (ParseScope *sc = scope; sc; sc = sc->next) {
    NameEntry *e = hashmap_get2(ctx, &sc->names, tok->loc, tok->len);
    if (e)
      return e->type_def;
  }
  return NULL;
}

// Declares a typedef name in the current scope. The name must be
// visible immediately: the oracle has to classify the very next
// identifier. sema completes the type when it reaches the ND_TYPEDEF
// record this declaration also emits.
static void add_typedef(ChibiccContext *ctx, Node *node) {
  NameEntry *e = chibicc_arena_calloc(ctx, 1, sizeof(NameEntry));
  e->type_def = node->ty;
  hashmap_put(ctx, &scope->names, get_ident(ctx, node->tok), e);
}

// Records a name a declarator just declared (a variable, function,
// parameter or enum constant), so that it shadows a typedef of the same
// spelling for the rest of the parse.
static void add_declared_name(ChibiccContext *ctx, Token *tok) {
  hashmap_put(ctx, &scope->names, get_ident(ctx, tok), chibicc_arena_calloc(ctx, 1, sizeof(NameEntry)));
}

// Find a struct/union/enum tag by name, in any enclosing scope.
static Type *find_tag(ChibiccContext *ctx, Token *tok) {
  for (ParseScope *sc = scope; sc; sc = sc->next) {
    Type *ty = hashmap_get2(ctx, &sc->tags, tok->loc, tok->len);
    if (ty)
      return ty;
  }
  return NULL;
}

// Find a tag declared in the current scope only; used when a tag is
// (re)defined, so that a previous forward declaration in the same scope
// keeps its identity.
static Type *find_current_tag(ChibiccContext *ctx, Token *tok) {
  return hashmap_get2(ctx, &scope->tags, tok->loc, tok->len);
}

static void push_tag_scope(ChibiccContext *ctx, Token *tok, Type *ty) {
  hashmap_put2(ctx, &scope->tags, tok->loc, tok->len, ty);
}

// Appends a chain of nodes to the chain `cur` points into and returns
// its new tail.
static Node *chain_append(Node *cur, Node *chain) {
  for (; chain; chain = chain->next)
    cur = cur->next = chain;
  return cur;
}

// Returns `specs` with `chain` appended after it (or `chain` if there
// are no specs yet). Used where declaration records produced by a
// declspec have to precede the records the declaration itself emits.
static Node *specs_then(Node *specs, Node *chain) {
  if (!specs)
    return chain;
  Node *cur = specs;
  while (cur->next)
    cur = cur->next;
  cur->next = chain;
  return specs;
}

// Appends one declaration record (an enum constant) to a spec list.
static void add_spec(Node **specs, Node *node) {
  node->next = NULL;
  if (!*specs) {
    *specs = node;
    return;
  }
  Node *cur = *specs;
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
static Type *declspec(ChibiccContext *ctx, Token **rest, Token *tok, VarAttr *attr, Node **specs) {
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

  Type *ty = ty_int;
  int counter = 0;
  bool is_atomic = false;

  while (is_typename(ctx, tok) || equal(tok, "__attribute__")) {
    // Declaration and function attributes are accepted in any specifier
    // position; the ones without a type meaning are ignored.
    if (equal(tok, "__attribute__")) {
      tok = attribute_list(ctx, tok, NULL);
      continue;
    }

    // Handle storage class specifiers.
    if (equal(tok, "typedef") || equal(tok, "static") || equal(tok, "extern") ||
        equal(tok, "inline") || equal(tok, "_Thread_local") || equal(tok, "__thread")) {
      if (!attr)
        error_tok(ctx, tok, "storage class specifier is not allowed in this context");

      if (equal(tok, "typedef"))
        attr->is_typedef = true;
      else if (equal(tok, "static"))
        attr->is_static = true;
      else if (equal(tok, "extern"))
        attr->is_extern = true;
      else if (equal(tok, "inline"))
        attr->is_inline = true;
      else
        attr->is_tls = true;

      if (attr->is_typedef &&
          attr->is_static + attr->is_extern + attr->is_inline + attr->is_tls > 1)
        error_tok(ctx, tok, "typedef may not be used together with static,"
                  " extern, inline, __thread or _Thread_local");
      tok = tok->next;
      continue;
    }

    // These keywords are recognized but ignored.
    if (consume(&tok, tok, "const") || consume(&tok, tok, "volatile") ||
        consume(&tok, tok, "auto") || consume(&tok, tok, "register") ||
        consume(&tok, tok, "restrict") || consume(&tok, tok, "__restrict") ||
        consume(&tok, tok, "__restrict__") || consume(&tok, tok, "_Noreturn"))
      continue;

    if (equal(tok, "_Atomic")) {
      tok = tok->next;
      if (equal(tok , "(")) {
        ty = typename(ctx, &tok, tok->next, specs);
        tok = skip(ctx, tok, ")");
      }
      is_atomic = true;
      continue;
    }

    if (equal(tok, "_Alignas")) {
      if (!attr)
        error_tok(ctx, tok, "_Alignas is not allowed in this context");
      tok = skip(ctx, tok->next, "(");

      // The `_Alignas(type)` form asks for a property of a type that
      // may not be complete yet, and the `_Alignas(expr)` form is a
      // constant expression; both are recorded for sema to settle.
      // Whichever comes last is the one that counts, as before.
      if (is_typename(ctx, tok)) {
        attr->align_ty = typename(ctx, &tok, tok, specs);
        attr->align_expr = NULL;
      } else {
        attr->align_ty = NULL;
        attr->align_expr = conditional(ctx, &tok, tok);
      }
      tok = skip(ctx, tok, ")");
      continue;
    }

    // Handle user-defined types.
    Type *ty2 = find_typedef(ctx, tok);
    if (equal(tok, "struct") || equal(tok, "union") || equal(tok, "enum") ||
        equal(tok, "typeof") || ty2) {
      if (counter)
        break;

      if (equal(tok, "struct")) {
        ty = struct_decl(ctx, &tok, tok->next, specs);
      } else if (equal(tok, "union")) {
        ty = union_decl(ctx, &tok, tok->next, specs);
      } else if (equal(tok, "enum")) {
        ty = enum_specifier(ctx, &tok, tok->next, specs);
      } else if (equal(tok, "typeof")) {
        ty = typeof_specifier(ctx, &tok, tok->next, specs);
      } else {
        ty = ty2;
        tok = tok->next;
      }

      counter += OTHER;
      continue;
    }

    // Handle built-in types.
    if (equal(tok, "void"))
      counter += VOID;
    else if (equal(tok, "_Bool"))
      counter += BOOL;
    else if (equal(tok, "char"))
      counter += CHAR;
    else if (equal(tok, "short"))
      counter += SHORT;
    else if (equal(tok, "int"))
      counter += INT;
    else if (equal(tok, "long"))
      counter += LONG;
    else if (equal(tok, "float"))
      counter += FLOAT;
    else if (equal(tok, "double"))
      counter += DOUBLE;
    else if (equal(tok, "signed"))
      counter |= SIGNED;
    else if (equal(tok, "unsigned"))
      counter |= UNSIGNED;
    else
      unreachable();

    switch (counter) {
    case VOID:
      ty = ty_void;
      break;
    case BOOL:
      ty = ty_bool;
      break;
    case CHAR:
    case SIGNED + CHAR:
      ty = ty_char;
      break;
    case UNSIGNED + CHAR:
      ty = ty_uchar;
      break;
    case SHORT:
    case SHORT + INT:
    case SIGNED + SHORT:
    case SIGNED + SHORT + INT:
      ty = ty_short;
      break;
    case UNSIGNED + SHORT:
    case UNSIGNED + SHORT + INT:
      ty = ty_ushort;
      break;
    case INT:
    case SIGNED:
    case SIGNED + INT:
      ty = ty_int;
      break;
    case UNSIGNED:
    case UNSIGNED + INT:
      ty = ty_uint;
      break;
    case LONG:
    case LONG + INT:
    case LONG + LONG:
    case LONG + LONG + INT:
    case SIGNED + LONG:
    case SIGNED + LONG + INT:
    case SIGNED + LONG + LONG:
    case SIGNED + LONG + LONG + INT:
      ty = ty_long;
      break;
    case UNSIGNED + LONG:
    case UNSIGNED + LONG + INT:
    case UNSIGNED + LONG + LONG:
    case UNSIGNED + LONG + LONG + INT:
      ty = ty_ulong;
      break;
    case FLOAT:
      ty = ty_float;
      break;
    case DOUBLE:
      ty = ty_double;
      break;
    case LONG + DOUBLE:
      ty = ty_ldouble;
      break;
    default:
      error_tok(ctx, tok, "invalid type");
    }

    tok = tok->next;
  }

  if (is_atomic) {
    ty = copy_type(ctx, ty);
    ty->is_atomic = true;
  }

  *rest = tok;
  return ty;
}

// func-params = ("void" | param ("," param)* ("," "...")?)? ")"
// param       = declspec declarator
static Type *func_params(ChibiccContext *ctx, Token **rest, Token *tok, Type *ty) {
  if (equal(tok, "void") && equal(tok->next, ")")) {
    *rest = tok->next->next;
    return func_type(ctx, ty);
  }

  Type head = {};
  Type *cur = &head;
  bool is_variadic = false;
  Node *specs = NULL;

  while (!equal(tok, ")")) {
    if (cur != &head)
      tok = skip(ctx, tok, ",");

    if (equal(tok, "...")) {
      is_variadic = true;
      tok = tok->next;
      skip(ctx, tok, ")");
      break;
    }

    DeclName decl = {};
    Type *ty2 = declspec(ctx, &tok, tok, NULL, &specs);
    ty2 = declarator(ctx, &tok, tok, ty2, &decl);

    if (ty2->kind == TY_ARRAY) {
      // "array of T" is converted to "pointer to T" only in the parameter
      // context. For example, *argv[] is converted to **argv by this.
      ty2 = pointer_to(ctx, ty2->base);
    } else if (ty2->kind == TY_FUNC) {
      // Likewise, a function is converted to a pointer to a function
      // only in the parameter context.
      ty2 = pointer_to(ctx, ty2);
    }

    // The parameter record is copied into the function type; its name
    // lives on that per-parameter copy, where sema reads it.
    Type *param = copy_type(ctx, ty2);
    param->name = decl.name;
    param->name_pos = decl.name_pos;
    cur = cur->next = param;
  }

  if (cur == &head)
    is_variadic = true;

  ty = func_type(ctx, ty);
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
static Type *array_dimensions(ChibiccContext *ctx, Token **rest, Token *tok, Type *ty) {
  while (equal(tok, "static") || equal(tok, "restrict"))
    tok = tok->next;

  if (equal(tok, "]")) {
    ty = type_suffix(ctx, rest, tok->next, ty);
    return array_of(ctx, ty, -1);
  }

  Node *expr = conditional(ctx, &tok, tok);
  tok = skip(ctx, tok, "]");
  ty = type_suffix(ctx, rest, tok, ty);

  // Whether the dimension denotes a fixed-length array or a VLA depends
  // on whether the length expression is a constant expression - a
  // semantic question. The expression is recorded here and sema decides
  // the type when it resolves the declarator (see resolve_type).
  return array_of_dim(ctx, ty, expr);
}

// type-suffix = "(" func-params
//             | "[" array-dimensions
//             | ε
static Type *type_suffix(ChibiccContext *ctx, Token **rest, Token *tok, Type *ty) {
  if (equal(tok, "("))
    return func_params(ctx, rest, tok->next, ty);

  if (equal(tok, "["))
    return array_dimensions(ctx, rest, tok->next, ty);

  *rest = tok;
  return ty;
}

// pointers = ("*" ("const" | "volatile" | "restrict")*)*
static Type *pointers(ChibiccContext *ctx, Token **rest, Token *tok, Type *ty) {
  while (consume(&tok, tok, "*")) {
    ty = pointer_to(ctx, ty);
    while (equal(tok, "const") || equal(tok, "volatile") || equal(tok, "restrict") ||
           equal(tok, "__restrict") || equal(tok, "__restrict__"))
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
static Type *declarator(ChibiccContext *ctx, Token **rest, Token *tok, Type *ty, DeclName *decl) {
  if (++ctx->parse_depth > MAX_PARSE_DEPTH)
    error_tok(ctx, tok, "nesting too deep");
  Type *node = declarator1(ctx, rest, tok, ty, decl);
  ctx->parse_depth--;
  return node;
}

static Type *declarator1(ChibiccContext *ctx, Token **rest, Token *tok, Type *ty, DeclName *decl) {
  ty = pointers(ctx, &tok, tok, ty);

  if (equal(tok, "(")) {
    Token *start = tok;
    Type dummy = {};
    declarator(ctx, &tok, start->next, &dummy, NULL);
    tok = skip(ctx, tok, ")");
    ty = type_suffix(ctx, rest, tok, ty);
    return declarator(ctx, &tok, start->next, ty, decl);
  }

  Token *name = NULL;
  Token *name_pos = tok;

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
static Type *abstract_declarator(ChibiccContext *ctx, Token **rest, Token *tok, Type *ty) {
  if (++ctx->parse_depth > MAX_PARSE_DEPTH)
    error_tok(ctx, tok, "nesting too deep");
  Type *node = abstract_declarator1(ctx, rest, tok, ty);
  ctx->parse_depth--;
  return node;
}

static Type *abstract_declarator1(ChibiccContext *ctx, Token **rest, Token *tok, Type *ty) {
  ty = pointers(ctx, &tok, tok, ty);

  if (equal(tok, "(")) {
    Token *start = tok;
    Type dummy = {};
    abstract_declarator(ctx, &tok, start->next, &dummy);
    tok = skip(ctx, tok, ")");
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
static Type *typename(ChibiccContext *ctx, Token **rest, Token *tok, Node **specs) {
  Type *ty = declspec(ctx, &tok, tok, NULL, specs);
  return abstract_declarator(ctx, rest, tok, ty);
}

static bool consume_end(Token **rest, Token *tok) {
  if (equal(tok, "}")) {
    *rest = tok->next;
    return true;
  }

  if (equal(tok, ",") && equal(tok->next, "}")) {
    *rest = tok->next->next;
    return true;
  }

  return false;
}

// enum-specifier = ident? "{" enum-list? "}"
//                | ident ("{" enum-list? "}")?
//
// enum-list      = ident ("=" num)? ("," ident ("=" num)?)* ","?
static Type *enum_specifier(ChibiccContext *ctx, Token **rest, Token *tok, Node **specs) {
  Type *ty = enum_type(ctx);

  // Read a struct tag.
  Token *tag = NULL;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }

  if (tag && !equal(tok, "{")) {
    Type *ty = find_tag(ctx, tag);
    if (!ty)
      error_tok(ctx, tag, "unknown enum type");
    if (ty->kind != TY_ENUM)
      error_tok(ctx, tag, "not an enum tag");
    *rest = tok;
    return ty;
  }

  tok = skip(ctx, tok, "{");

  // Read an enum-list. Each member becomes a declaration record node
  // carrying the name, the optional explicit value expression and the
  // enum type; sema evaluates the values and registers the names at
  // this position of the enclosing chain.
  int i = 0;
  while (!consume_end(rest, tok)) {
    if (i++ > 0)
      tok = skip(ctx, tok, ",");

    Token *name = tok;
    tok = tok->next;
    add_declared_name(ctx, name);

    Node *node = new_node(ctx, ND_ENUM_CONST, name);
    if (equal(tok, "="))
      node->lhs = conditional(ctx, &tok, tok->next);
    node->ty_op = ty;

    add_spec(specs, node);
  }

  if (tag)
    push_tag_scope(ctx, tag, ty);
  return ty;
}

// typeof-specifier = "(" (expr | typename) ")"
static Type *typeof_specifier(ChibiccContext *ctx, Token **rest, Token *tok, Node **specs) {
  tok = skip(ctx, tok, "(");

  Type *ty;
  if (is_typename(ctx, tok)) {
    ty = typename(ctx, &tok, tok, specs);
  } else {
    // What type an expression has is not a syntax question, so the
    // operand is recorded in a placeholder the declarator builds on;
    // sema annotates the expression and fills the record in.
    ty = typeof_placeholder(ctx);
    ty->typeof_expr = expr(ctx, &tok, tok);
  }
  *rest = skip(ctx, tok, ")");
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
static Node *declaration(ChibiccContext *ctx, Token **rest, Token *tok, Type *basety, VarAttr *attr) {
  Node head = {};
  Node *cur = &head;
  int i = 0;

  while (!equal(tok, ";")) {
    if (i++ > 0)
      tok = skip(ctx, tok, ",");

    DeclName decl_name = {};
    Type *ty = declarator(ctx, &tok, tok, basety, &decl_name);
    if (!decl_name.name)
      error_tok(ctx, decl_name.name_pos, "variable name omitted");

    // The declared name shadows a typedef of the same name for the rest
    // of the parse - before the initializer is parsed, as the old
    // declaration site did.
    Token *name = decl_name.name;
    add_declared_name(ctx, name);

    Initializer *init = NULL;
    if (equal(tok, "=")) {
      Token *eq = tok;
      init = init_record(ctx, &tok, tok->next);
      init->eq_tok = eq;
    }

    Node *decl = new_node(ctx, ND_DECL, name);
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
static InitDesig *designators(ChibiccContext *ctx, Token **rest, Token *tok) {
  InitDesig head = {};
  InitDesig *cur = &head;

  while (1) {
    if (equal(tok, "[")) {
      InitDesig *desig = chibicc_arena_calloc(ctx, 1, sizeof(InitDesig));
      desig->tok = tok;
      desig->begin = conditional(ctx, &tok, tok->next);
      desig->after_begin = tok;
      if (equal(tok, "..."))
        desig->end = conditional(ctx, &tok, tok->next);
      desig->rbracket = tok;
      tok = skip(ctx, tok, "]");
      cur = cur->next = desig;
      continue;
    }

    if (equal(tok, ".")) {
      InitDesig *desig = chibicc_arena_calloc(ctx, 1, sizeof(InitDesig));
      desig->tok = tok;
      tok = tok->next;
      if (tok->kind != TK_IDENT)
        error_tok(ctx, tok, "expected a field designator");
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
static Initializer *init_record(ChibiccContext *ctx, Token **rest, Token *tok) {
  if (++ctx->parse_depth > MAX_PARSE_DEPTH)
    error_tok(ctx, tok, "nesting too deep");
  Initializer *node = init_record1(ctx, rest, tok);
  ctx->parse_depth--;
  return node;
}

static Initializer *init_record1(ChibiccContext *ctx, Token **rest, Token *tok) {
  Initializer *rec = chibicc_arena_calloc(ctx, 1, sizeof(Initializer));
  rec->tok = tok;

  if (equal(tok, "{")) {
    rec->kind = INIT_LIST;
    tok = tok->next;

    InitItem head = {};
    InitItem *cur = &head;
    bool first = true;

    while (!consume_end(&tok, tok)) {
      InitItem *item = chibicc_arena_calloc(ctx, 1, sizeof(InitItem));
      if (!first) {
        item->comma_tok = tok;
        tok = skip(ctx, tok, ",");
      }
      first = false;

      item->desigs = designators(ctx, &tok, tok);
      if (item->desigs && equal(tok, "="))
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
  if (tok->kind == TK_STR && (equal(tok->next, ",") ||
                              equal(tok->next, "}") ||
                              equal(tok->next, ";") ||
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
static bool is_typename(ChibiccContext *ctx, Token *tok) {
  HashMap *map = &ctx->typename_map;

  if (map->capacity == 0) {
    static char *kw[] = {
      "void", "_Bool", "char", "short", "int", "long", "struct", "union",
      "typedef", "enum", "static", "extern", "_Alignas", "signed", "unsigned",
      "const", "volatile", "auto", "register", "restrict", "__restrict",
      "__restrict__", "_Noreturn", "float", "double", "typeof", "inline",
      "_Thread_local", "__thread", "_Atomic",
    };

    for (int i = 0; i < sizeof(kw) / sizeof(*kw); i++)
      hashmap_put(ctx, map, kw[i], (void *)1);
  }

  return hashmap_get2(ctx, map, tok->loc, tok->len) || find_typedef(ctx, tok);
}

// asm-stmt = "asm" ("volatile" | "inline")* "(" string-literal ")"
static Node *asm_stmt(ChibiccContext *ctx, Token **rest, Token *tok) {
  Node *node = new_node(ctx, ND_ASM, tok);
  tok = tok->next;

  while (equal(tok, "volatile") || equal(tok, "inline"))
    tok = tok->next;

  tok = skip(ctx, tok, "(");
  if (tok->kind != TK_STR || tok->ty->base->kind != TY_CHAR)
    error_tok(ctx, tok, "expected string literal");
  node->asm_str = tok->str;
  *rest = skip(ctx, tok->next, ")");
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
static Node *stmt(ChibiccContext *ctx, Token **rest, Token *tok) {
  if (equal(tok, "return")) {
    // The implicit conversion to the return type is sema's: the parser
    // does not know the enclosing function.
    Node *node = new_node(ctx, ND_RETURN, tok);
    if (consume(rest, tok->next, ";"))
      return node;

    node->lhs = expr(ctx, &tok, tok->next);
    *rest = skip(ctx, tok, ";");
    return node;
  }

  if (equal(tok, "if")) {
    Node *node = new_node(ctx, ND_IF, tok);
    tok = skip(ctx, tok->next, "(");
    node->cond = expr(ctx, &tok, tok);
    tok = skip(ctx, tok, ")");
    node->then = stmt(ctx, &tok, tok);
    if (equal(tok, "else"))
      node->els = stmt(ctx, &tok, tok->next);
    *rest = tok;
    return node;
  }

  if (equal(tok, "switch")) {
    Node *node = new_node(ctx, ND_SWITCH, tok);
    tok = skip(ctx, tok->next, "(");
    node->cond = expr(ctx, &tok, tok);
    tok = skip(ctx, tok, ")");
    node->then = stmt(ctx, rest, tok);
    return node;
  }

  if (equal(tok, "case")) {
    // The values a case label stands for are constant expressions, so
    // they are recorded unevaluated; sema evaluates them into begin/end
    // when it resolves the node. `:` is where the range check is
    // anchored.
    Node *node = new_node(ctx, ND_CASE, tok);
    node->begin_expr = conditional(ctx, &tok, tok->next);

    if (equal(tok, "...")) {
      // [GNU] Case ranges, e.g. "case 1 ... 5:"
      node->end_expr = conditional(ctx, &tok, tok->next);
    }

    node->colon_tok = tok;
    tok = skip(ctx, tok, ":");
    node->lhs = stmt(ctx, rest, tok);
    return node;
  }

  if (equal(tok, "default")) {
    Node *node = new_node(ctx, ND_CASE, tok);
    node->is_default = true;
    tok = skip(ctx, tok->next, ":");
    node->lhs = stmt(ctx, rest, tok);
    return node;
  }

  if (equal(tok, "for")) {
    Node *node = new_node(ctx, ND_FOR, tok);
    tok = skip(ctx, tok->next, "(");

    enter_scope(ctx);

    if (is_typename(ctx, tok)) {
      Node *specs = NULL;
      Type *basety = declspec(ctx, &tok, tok, NULL, &specs);
      // Enum records a for-init declspec defined share the init's
      // scope, so they ride at the head of the init chain. The init is
      // a chain of declaration records like any other declaration; sema
      // folds it into one statement once the records are lowered,
      // because codegen's `for` has a single-statement init slot.
      node->init = specs_then(specs, declaration(ctx, &tok, tok, basety, NULL));
    } else {
      node->init = expr_stmt(ctx, &tok, tok);
    }

    if (!equal(tok, ";"))
      node->cond = expr(ctx, &tok, tok);
    tok = skip(ctx, tok, ";");

    if (!equal(tok, ")"))
      node->inc = expr(ctx, &tok, tok);
    tok = skip(ctx, tok, ")");

    node->then = stmt(ctx, rest, tok);

    leave_scope(ctx);
    return node;
  }

  if (equal(tok, "while")) {
    Node *node = new_node(ctx, ND_WHILE, tok);
    tok = skip(ctx, tok->next, "(");
    node->cond = expr(ctx, &tok, tok);
    tok = skip(ctx, tok, ")");
    node->then = stmt(ctx, rest, tok);
    return node;
  }

  if (equal(tok, "do")) {
    Node *node = new_node(ctx, ND_DO, tok);
    node->then = stmt(ctx, &tok, tok->next);

    tok = skip(ctx, tok, "while");
    tok = skip(ctx, tok, "(");
    node->cond = expr(ctx, &tok, tok);
    tok = skip(ctx, tok, ")");
    *rest = skip(ctx, tok, ";");
    return node;
  }

  if (equal(tok, "asm"))
    return asm_stmt(ctx, rest, tok);

  if (equal(tok, "goto")) {
    if (equal(tok->next, "*")) {
      // [GNU] `goto *ptr` jumps to the address specified by `ptr`.
      Node *node = new_node(ctx, ND_GOTO_EXPR, tok);
      node->lhs = expr(ctx, &tok, tok->next->next);
      *rest = skip(ctx, tok, ";");
      return node;
    }

    Node *node = new_node(ctx, ND_GOTO, tok);
    node->label = get_ident(ctx, tok->next);
    *rest = skip(ctx, tok->next->next, ";");
    return node;
  }

  if (equal(tok, "break")) {
    *rest = skip(ctx, tok->next, ";");
    return new_node(ctx, ND_BREAK, tok);
  }

  if (equal(tok, "continue")) {
    *rest = skip(ctx, tok->next, ";");
    return new_node(ctx, ND_CONTINUE, tok);
  }

  if (tok->kind == TK_IDENT && equal(tok->next, ":")) {
    Node *node = new_node(ctx, ND_LABEL, tok);
    node->label = chibicc_arena_strndup(ctx, tok->loc, tok->len);
    node->lhs = stmt(ctx, rest, tok->next->next);
    return node;
  }

  if (equal(tok, "{"))
    return compound_stmt(ctx, rest, tok->next);

  return expr_stmt(ctx, rest, tok);
}

// compound-stmt = (typedef | declaration | stmt)* "}"
//
// Declaration records that produce no statement of their own (typedefs,
// enum constants, block-scope extern declarations and [GNU] nested
// function definitions) enter the chain at their source position; sema
// removes them again before codegen sees the chain.
static Node *compound_stmt(ChibiccContext *ctx, Token **rest, Token *tok) {
  Node *node = new_node(ctx, ND_BLOCK, tok);
  node->is_scope_block = true;
  Node head = {};
  Node *cur = &head;

  enter_scope(ctx);

  while (!equal(tok, "}")) {
    if (is_typename(ctx, tok) && !equal(tok->next, ":")) {
      VarAttr attr = {};
      Node *specs = NULL;
      Type *basety = declspec(ctx, &tok, tok, &attr, &specs);
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
static Node *expr_stmt(ChibiccContext *ctx, Token **rest, Token *tok) {
  if (equal(tok, ";")) {
    *rest = tok->next;
    return new_node(ctx, ND_BLOCK, tok);
  }

  Node *node = new_node(ctx, ND_EXPR_STMT, tok);
  node->lhs = expr(ctx, &tok, tok);
  *rest = skip(ctx, tok, ";");
  return node;
}

#if !CHIBICC_PRATT_PARSE_FEATURE
// expr = assign ("," expr)?
static Node *expr(ChibiccContext *ctx, Token **rest, Token *tok) {
  if (++ctx->parse_depth > MAX_PARSE_DEPTH)
    error_tok(ctx, tok, "nesting too deep");
  Node *node = expr1(ctx, rest, tok);
  ctx->parse_depth--;
  return node;
}

static Node *expr1(ChibiccContext *ctx, Token **rest, Token *tok) {
  Node *node = assign(ctx, &tok, tok);

  // Comma is left-associative: `a, b, c` is `(a, b), c`, the shape the C
  // grammar gives. Both parsers build this shape.
  while (equal(tok, ",")) {
    Token *start = tok;
    node = new_binary(ctx, ND_COMMA, node, assign(ctx, &tok, tok->next), start);
  }

  *rest = tok;
  return node;
}

// assign    = conditional (assign-op assign)?
// assign-op = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^="
//           | "<<=" | ">>="
//
// A compound assignment keeps its operator in `op`; sema rewrites it to
// the read-modify-write form (handling the bitfield and atomic cases).
static Node *assign(ChibiccContext *ctx, Token **rest, Token *tok) {
  if (++ctx->parse_depth > MAX_PARSE_DEPTH)
    error_tok(ctx, tok, "nesting too deep");
  Node *node = assign1(ctx, rest, tok);
  ctx->parse_depth--;
  return node;
}

static Node *assign1(ChibiccContext *ctx, Token **rest, Token *tok) {
  Node *node = conditional(ctx, &tok, tok);

  if (equal(tok, "="))
    return new_binary(ctx, ND_ASSIGN, node, assign(ctx, rest, tok->next), tok);

  if (equal(tok, "+=")) {
    Node *expr = new_binary(ctx, ND_ASSIGN, node, assign(ctx, rest, tok->next), tok);
    expr->op = ND_ADD;
    return expr;
  }

  if (equal(tok, "-=")) {
    Node *expr = new_binary(ctx, ND_ASSIGN, node, assign(ctx, rest, tok->next), tok);
    expr->op = ND_SUB;
    return expr;
  }

  if (equal(tok, "*=")) {
    Node *expr = new_binary(ctx, ND_ASSIGN, node, assign(ctx, rest, tok->next), tok);
    expr->op = ND_MUL;
    return expr;
  }

  if (equal(tok, "/=")) {
    Node *expr = new_binary(ctx, ND_ASSIGN, node, assign(ctx, rest, tok->next), tok);
    expr->op = ND_DIV;
    return expr;
  }

  if (equal(tok, "%=")) {
    Node *expr = new_binary(ctx, ND_ASSIGN, node, assign(ctx, rest, tok->next), tok);
    expr->op = ND_MOD;
    return expr;
  }

  if (equal(tok, "&=")) {
    Node *expr = new_binary(ctx, ND_ASSIGN, node, assign(ctx, rest, tok->next), tok);
    expr->op = ND_BITAND;
    return expr;
  }

  if (equal(tok, "|=")) {
    Node *expr = new_binary(ctx, ND_ASSIGN, node, assign(ctx, rest, tok->next), tok);
    expr->op = ND_BITOR;
    return expr;
  }

  if (equal(tok, "^=")) {
    Node *expr = new_binary(ctx, ND_ASSIGN, node, assign(ctx, rest, tok->next), tok);
    expr->op = ND_BITXOR;
    return expr;
  }

  if (equal(tok, "<<=")) {
    Node *expr = new_binary(ctx, ND_ASSIGN, node, assign(ctx, rest, tok->next), tok);
    expr->op = ND_SHL;
    return expr;
  }

  if (equal(tok, ">>=")) {
    Node *expr = new_binary(ctx, ND_ASSIGN, node, assign(ctx, rest, tok->next), tok);
    expr->op = ND_SHR;
    return expr;
  }

  *rest = tok;
  return node;
}

// conditional = logor ("?" expr? ":" conditional)?
Node *conditional(ChibiccContext *ctx, Token **rest, Token *tok) {
  if (++ctx->parse_depth > MAX_PARSE_DEPTH)
    error_tok(ctx, tok, "nesting too deep");
  Node *node = conditional1(ctx, rest, tok);
  ctx->parse_depth--;
  return node;
}

static Node *conditional1(ChibiccContext *ctx, Token **rest, Token *tok) {
  Node *cond = logor(ctx, &tok, tok);

  if (!equal(tok, "?")) {
    *rest = tok;
    return cond;
  }

  if (equal(tok->next, ":")) {
    // [GNU] `a ?: b`. Kept as-is in the tree with is_elvis set;
    // sema lowers it to `tmp = a, tmp ? tmp : b`.
    Node *node = new_node(ctx, ND_COND, tok);
    node->is_elvis = true;
    node->cond = cond;
    node->els = conditional(ctx, rest, tok->next->next);
    return node;
  }

  Node *node = new_node(ctx, ND_COND, tok);
  node->cond = cond;
  node->then = expr(ctx, &tok, tok->next);
  tok = skip(ctx, tok, ":");
  node->els = conditional(ctx, rest, tok);
  return node;
}

// logor = logand ("||" logand)*
static Node *logor(ChibiccContext *ctx, Token **rest, Token *tok) {
  Node *node = logand(ctx, &tok, tok);
  while (equal(tok, "||")) {
    Token *start = tok;
    node = new_binary(ctx, ND_LOGOR, node, logand(ctx, &tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

// logand = bitor ("&&" bitor)*
static Node *logand(ChibiccContext *ctx, Token **rest, Token *tok) {
  Node *node = bitor(ctx, &tok, tok);
  while (equal(tok, "&&")) {
    Token *start = tok;
    node = new_binary(ctx, ND_LOGAND, node, bitor(ctx, &tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

// bitor = bitxor ("|" bitxor)*
static Node *bitor(ChibiccContext *ctx, Token **rest, Token *tok) {
  Node *node = bitxor(ctx, &tok, tok);
  while (equal(tok, "|")) {
    Token *start = tok;
    node = new_binary(ctx, ND_BITOR, node, bitxor(ctx, &tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

// bitxor = bitand ("^" bitand)*
static Node *bitxor(ChibiccContext *ctx, Token **rest, Token *tok) {
  Node *node = bitand(ctx, &tok, tok);
  while (equal(tok, "^")) {
    Token *start = tok;
    node = new_binary(ctx, ND_BITXOR, node, bitand(ctx, &tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

// bitand = equality ("&" equality)*
static Node *bitand(ChibiccContext *ctx, Token **rest, Token *tok) {
  Node *node = equality(ctx, &tok, tok);
  while (equal(tok, "&")) {
    Token *start = tok;
    node = new_binary(ctx, ND_BITAND, node, equality(ctx, &tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

// equality = relational ("==" relational | "!=" relational)*
static Node *equality(ChibiccContext *ctx, Token **rest, Token *tok) {
  Node *node = relational(ctx, &tok, tok);

  for (;;) {
    Token *start = tok;

    if (equal(tok, "==")) {
      node = new_binary(ctx, ND_EQ, node, relational(ctx, &tok, tok->next), start);
      continue;
    }

    if (equal(tok, "!=")) {
      node = new_binary(ctx, ND_NE, node, relational(ctx, &tok, tok->next), start);
      continue;
    }

    *rest = tok;
    return node;
  }
}

// relational = shift ("<" shift | "<=" shift | ">" shift | ">=" shift)*
static Node *relational(ChibiccContext *ctx, Token **rest, Token *tok) {
  Node *node = shift(ctx, &tok, tok);

  for (;;) {
    Token *start = tok;

    if (equal(tok, "<")) {
      node = new_binary(ctx, ND_LT, node, shift(ctx, &tok, tok->next), start);
      continue;
    }

    if (equal(tok, "<=")) {
      node = new_binary(ctx, ND_LE, node, shift(ctx, &tok, tok->next), start);
      continue;
    }

    if (equal(tok, ">")) {
      node = new_binary(ctx, ND_GT, node, shift(ctx, &tok, tok->next), start);
      continue;
    }

    if (equal(tok, ">=")) {
      node = new_binary(ctx, ND_GE, node, shift(ctx, &tok, tok->next), start);
      continue;
    }

    *rest = tok;
    return node;
  }
}

// shift = add ("<<" add | ">>" add)*
static Node *shift(ChibiccContext *ctx, Token **rest, Token *tok) {
  Node *node = add(ctx, &tok, tok);

  for (;;) {
    Token *start = tok;

    if (equal(tok, "<<")) {
      node = new_binary(ctx, ND_SHL, node, add(ctx, &tok, tok->next), start);
      continue;
    }

    if (equal(tok, ">>")) {
      node = new_binary(ctx, ND_SHR, node, add(ctx, &tok, tok->next), start);
      continue;
    }

    *rest = tok;
    return node;
  }
}
// add = mul ("+" mul | "-" mul)*
static Node *add(ChibiccContext *ctx, Token **rest, Token *tok) {
  Node *node = mul(ctx, &tok, tok);

  for (;;) {
    Token *start = tok;

    if (equal(tok, "+")) {
      node = new_binary(ctx, ND_ADD, node, mul(ctx, &tok, tok->next), start);
      continue;
    }

    if (equal(tok, "-")) {
      node = new_binary(ctx, ND_SUB, node, mul(ctx, &tok, tok->next), start);
      continue;
    }

    *rest = tok;
    return node;
  }
}

// mul = cast ("*" cast | "/" cast | "%" cast)*
static Node *mul(ChibiccContext *ctx, Token **rest, Token *tok) {
  Node *node = cast(ctx, &tok, tok);

  for (;;) {
    Token *start = tok;

    if (equal(tok, "*")) {
      node = new_binary(ctx, ND_MUL, node, cast(ctx, &tok, tok->next), start);
      continue;
    }

    if (equal(tok, "/")) {
      node = new_binary(ctx, ND_DIV, node, cast(ctx, &tok, tok->next), start);
      continue;
    }

    if (equal(tok, "%")) {
      node = new_binary(ctx, ND_MOD, node, cast(ctx, &tok, tok->next), start);
      continue;
    }

    *rest = tok;
    return node;
  }
}
#endif // !CHIBICC_PRATT_PARSE_FEATURE

#if !CHIBICC_PRATT_PARSE_FEATURE
// cast = "(" type-name ")" cast | unary
static Node *cast(ChibiccContext *ctx, Token **rest, Token *tok) {
  if (++ctx->parse_depth > MAX_PARSE_DEPTH)
    error_tok(ctx, tok, "nesting too deep");
  Node *node = cast1(ctx, rest, tok);
  ctx->parse_depth--;
  return node;
}

static Node *cast1(ChibiccContext *ctx, Token **rest, Token *tok) {
  if (equal(tok, "(") && is_typename(ctx, tok->next)) {
    Token *start = tok;
    Node *specs = NULL;
    Type *ty = typename(ctx, &tok, tok->next, &specs);
    tok = skip(ctx, tok, ")");

    // compound literal
    if (equal(tok, "{"))
      return unary(ctx, rest, start);

    // Type cast: kept faithful and untyped, with the target type
    // recorded for sema to complete and apply. This is a cast the
    // source wrote, so is_implicit stays false; the casts sema inserts
    // to carry out an implicit conversion are the ones new_cast marks.
    Node *node = new_node(ctx, ND_CAST, start);
    node->lhs = cast(ctx, rest, tok);
    node->ty_op = ty;
    node->spec_decls = specs;
    return node;
  }

  return unary(ctx, rest, tok);
}

// unary = ("+" | "-" | "*" | "&" | "!" | "~") cast
//       | ("++" | "--") unary
//       | "&&" ident
//       | postfix
static Node *unary(ChibiccContext *ctx, Token **rest, Token *tok) {
  if (++ctx->parse_depth > MAX_PARSE_DEPTH)
    error_tok(ctx, tok, "nesting too deep");
  Node *node = unary1(ctx, rest, tok);
  ctx->parse_depth--;
  return node;
}

static Node *unary1(ChibiccContext *ctx, Token **rest, Token *tok) {
  if (equal(tok, "+"))
    return cast(ctx, rest, tok->next);

  if (equal(tok, "-"))
    return new_unary(ctx, ND_NEG, cast(ctx, rest, tok->next), tok);

  if (equal(tok, "&")) {
    // The bitfield check lives in sema: it needs the operand's type.
    return new_unary(ctx, ND_ADDR, cast(ctx, rest, tok->next), tok);
  }

  if (equal(tok, "*")) {
    // [https://www.sigbus.info/n1570#6.5.3.2p4] This is an oddity
    // in the C spec, but dereferencing a function shouldn't do
    // anything. If foo is a function, `*foo`, `**foo` or `*****foo`
    // are all equivalent to just `foo`. The check needs the operand's
    // type, so sema drops the node when it sees one.
    return new_unary(ctx, ND_DEREF, cast(ctx, rest, tok->next), tok);
  }

  if (equal(tok, "!"))
    return new_unary(ctx, ND_NOT, cast(ctx, rest, tok->next), tok);

  if (equal(tok, "~"))
    return new_unary(ctx, ND_BITNOT, cast(ctx, rest, tok->next), tok);

  // Read ++i as a faithful prefix increment; sema lowers it to i+=1
  if (equal(tok, "++")) {
    Node *node = new_node(ctx, ND_INCDEC, tok);
    node->lhs = unary(ctx, rest, tok->next);
    node->addend = 1;
    return node;
  }

  // Read --i as a faithful prefix decrement; sema lowers it to i-=1
  if (equal(tok, "--")) {
    Node *node = new_node(ctx, ND_INCDEC, tok);
    node->lhs = unary(ctx, rest, tok->next);
    node->addend = -1;
    return node;
  }

  // [GNU] labels-as-values
  if (equal(tok, "&&")) {
    Node *node = new_node(ctx, ND_LABEL_VAL, tok);
    node->label = get_ident(ctx, tok->next);
    *rest = tok->next->next;
    return node;
  }

  return postfix(ctx, rest, tok);
}
#endif // !CHIBICC_PRATT_PARSE_FEATURE

// struct-members = (declspec declarator (","  declarator)* ";")*
//
// Member placement (offsets, size, alignment, bitfield widths) and the
// flexible-array-member conversion are sema's; the member list here is
// the syntax shape, with `_Alignas` and bitfield widths recorded
// unevaluated.
static void struct_members(ChibiccContext *ctx, Token **rest, Token *tok, Type *ty, Node **specs) {
  Member head = {};
  Member *cur = &head;
  int idx = 0;

  while (!equal(tok, "}")) {
    VarAttr attr = {};
    Token *decl_start = tok;
    Type *basety = declspec(ctx, &tok, tok, &attr, specs);
    bool first = true;

    // An anonymous member is recognized by the kind of the base type.
    // A `typeof(expr)` base has not settled its kind yet, so the
    // placeholder counts as a candidate too; sema rejects a nameless
    // member that turns out not to be an aggregate.
    if ((basety->kind == TY_STRUCT || basety->kind == TY_UNION ||
         basety->kind == TY_TYPEOF) &&
        consume(&tok, tok, ";")) {
      Member *mem = chibicc_arena_calloc(ctx, 1, sizeof(Member));
      mem->tok = decl_start;
      mem->ty = basety;
      mem->idx = idx++;
      mem->align_ty = attr.align_ty;
      mem->align_expr = attr.align_expr;
      cur = cur->next = mem;
      continue;
    }

    // Regular struct members
    while (!consume(&tok, tok, ";")) {
      if (!first)
        tok = skip(ctx, tok, ",");
      first = false;

      Member *mem = chibicc_arena_calloc(ctx, 1, sizeof(Member));
      mem->tok = tok;
      DeclName decl = {};
      mem->ty = declarator(ctx, &tok, tok, basety, &decl);
      mem->name = decl.name;
      mem->idx = idx++;
      mem->align_ty = attr.align_ty;
      mem->align_expr = attr.align_expr;

      if (consume(&tok, tok, ":")) {
        mem->is_bitfield = true;
        // The width is a constant expression; it is recorded
        // unevaluated and sema evaluates it when the aggregate is
        // laid out.
        mem->width_expr = conditional(ctx, &tok, tok);
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
static Token *skip_attribute_args(ChibiccContext *ctx, Token *tok) {
  if (!equal(tok, "("))
    return tok;

  int depth = 0;
  for (; tok->kind != TK_EOF; tok = tok->next) {
    if (equal(tok, "(")) {
      depth++;
    } else if (equal(tok, ")")) {
      if (--depth == 0)
        return tok->next;
    }
  }
  error_tok(ctx, tok, "unclosed attribute");
  return tok;
}

static bool is_ignored_attribute(Token *tok) {
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
    if (equal(tok, names[i]))
      return true;
  return false;
}

static Token *attribute_list(ChibiccContext *ctx, Token *tok, Type *ty) {
  while (consume(&tok, tok, "__attribute__")) {
    tok = skip(ctx, tok, "(");
    tok = skip(ctx, tok, "(");

    bool first = true;

    while (!consume(&tok, tok, ")")) {
      if (!first)
        tok = skip(ctx, tok, ",");
      first = false;

      if (consume(&tok, tok, "packed")) {
        if (ty)
          ty->is_packed = true;
        continue;
      }

      if (consume(&tok, tok, "aligned")) {
        // The alignment is a constant expression. It is applied only when
        // the attribute is attached to a type definition; declaration
        // positions pass NULL so the shared type records stay read-only.
        if (consume(&tok, tok, "(")) {
          if (ty)
            ty->align_expr = conditional(ctx, &tok, tok);
          else
            (void)conditional(ctx, &tok, tok);
          tok = skip(ctx, tok, ")");
        }
        continue;
      }

      if (equal(tok, "cleanup")) {
        warn_tok(ctx, tok, "cleanup attribute is ignored");
        tok = skip_attribute_args(ctx, tok->next);
        continue;
      }

      if (is_ignored_attribute(tok)) {
        tok = skip_attribute_args(ctx, tok->next);
        continue;
      }

      error_tok(ctx, tok, "unknown attribute");
    }

    tok = skip(ctx, tok, ")");
  }

  return tok;
}

// struct-union-decl = attribute? ident? ("{" struct-members)?
static Type *struct_union_decl(ChibiccContext *ctx, Token **rest, Token *tok, Node **specs) {
  Type *ty = struct_type(ctx);
  tok = attribute_list(ctx, tok, ty);

  // Read a tag.
  Token *tag = NULL;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }

  if (tag && !equal(tok, "{")) {
    *rest = tok;

    Type *ty2 = find_tag(ctx, tag);
    if (ty2)
      return ty2;

    ty->size = -1;
    push_tag_scope(ctx, tag, ty);
    return ty;
  }

  tok = skip(ctx, tok, "{");

  // Construct a struct object. The member list is syntax; placing the
  // members is sema's job, which it does when the type is completed -
  // at the definition for a bare one, or at the first declaration that
  // resolves a type referring to it.
  struct_members(ctx, &tok, tok, ty, specs);
  *rest = attribute_list(ctx, tok, ty);
  ty->layout_pending = true;

  Type *ret = ty;
  if (tag) {
    // If this is a redefinition, overwrite a previous type.
    // Otherwise, register the struct type.
    Type *ty2 = find_current_tag(ctx, tag);
    if (ty2) {
      // A forward declaration (size -1) is completed here. A second
      // definition of the same tag in the same scope is a constraint
      // violation (C11 6.7.2.3p1), not a silent replacement.
      if (ty2->size >= 0)
        error_tok(ctx, tag, "redefinition of tag");
      *ty2 = *ty;
      ret = ty2;
    } else {
      push_tag_scope(ctx, tag, ty);
    }
  }

  return ret;
}

// struct-decl = struct-union-decl
static Type *struct_decl(ChibiccContext *ctx, Token **rest, Token *tok, Node **specs) {
  Type *ty = struct_union_decl(ctx, rest, tok, specs);
  ty->kind = TY_STRUCT;
  return ty;
}

// union-decl = struct-union-decl
static Type *union_decl(ChibiccContext *ctx, Token **rest, Token *tok, Node **specs) {
  Type *ty = struct_union_decl(ctx, rest, tok, specs);
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
static Node *struct_ref(ChibiccContext *ctx, Node *node, Token *tok, Token *arrow) {
  Node *mem = new_unary(ctx, ND_MEMBER, node, tok);
  mem->arrow_tok = arrow;
  return mem;
}

#if !CHIBICC_PRATT_PARSE_FEATURE
// postfix = "(" type-name ")" "{" initializer-list "}"
//         = ident "(" func-args ")" postfix-tail*
//         | primary postfix-tail*
//
// postfix-tail = "[" expr "]"
//              | "(" func-args ")"
//              | "." ident
//              | "->" ident
//              | "++"
//              | "--"
static Node *postfix(ChibiccContext *ctx, Token **rest, Token *tok) {
  if (equal(tok, "(") && is_typename(ctx, tok->next)) {
    // Compound literal. Kept faithful: the type and the initializer
    // record ride on the node, and sema materializes the hidden
    // variable (an anonymous global at file scope, a hidden local in a
    // block) when it resolves the node, then lowers it when typing.
    Token *start = tok;
    Node *specs = NULL;
    Type *ty = typename(ctx, &tok, tok->next, &specs);
    tok = skip(ctx, tok, ")");

    Node *node = new_node(ctx, ND_COMPOUND_LITERAL, start);
    node->ty_op = ty;
    node->spec_decls = specs;
    node->decl_init = init_record(ctx, rest, tok);
    return node;
  }

  Node *node = primary(ctx, &tok, tok);

  for (;;) {
    if (equal(tok, "(")) {
      node = funcall(ctx, &tok, tok->next, node);
      continue;
    }

    if (equal(tok, "[")) {
      Token *start = tok;
      Node *idx = expr(ctx, &tok, tok->next);
      tok = skip(ctx, tok, "]");
      Node *subscript = new_node(ctx, ND_SUBSCRIPT, start);
      subscript->lhs = node;
      subscript->rhs = idx;
      node = subscript;
      continue;
    }

    if (equal(tok, ".")) {
      node = struct_ref(ctx, node, tok->next, NULL);
      tok = tok->next->next;
      continue;
    }

    if (equal(tok, "->")) {
      // x->y is short for (*x).y
      node = struct_ref(ctx, node, tok->next, tok);
      tok = tok->next->next;
      continue;
    }

    if (equal(tok, "++")) {
      Node *incdec = new_node(ctx, ND_INCDEC, tok);
      incdec->lhs = node;
      incdec->is_post = true;
      incdec->addend = 1;
      node = incdec;
      tok = tok->next;
      continue;
    }

    if (equal(tok, "--")) {
      Node *incdec = new_node(ctx, ND_INCDEC, tok);
      incdec->lhs = node;
      incdec->is_post = true;
      incdec->addend = -1;
      node = incdec;
      tok = tok->next;
      continue;
    }

    *rest = tok;
    return node;
  }
}
#endif // !CHIBICC_PRATT_PARSE_FEATURE

// funcall = (assign ("," assign)*)? ")"
//
// The callee check, the argument conversions and the struct-return
// buffer are sema's; the node stays faithful, anchored at the closing
// paren where the argument-count diagnostics point.
static Node *funcall(ChibiccContext *ctx, Token **rest, Token *tok, Node *fn) {
  Node head = {};
  Node *cur = &head;

  while (!equal(tok, ")")) {
    if (cur != &head)
      tok = skip(ctx, tok, ",");

    cur = cur->next = assign(ctx, &tok, tok);
  }

  *rest = skip(ctx, tok, ")");

  Node *node = new_unary(ctx, ND_FUNCALL, fn, tok);
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
static Node *generic_selection(ChibiccContext *ctx, Token **rest, Token *tok) {
  Node *node = new_node(ctx, ND_GENERIC, tok);

  tok = skip(ctx, tok, "(");
  node->cond = assign(ctx, &tok, tok);

  Node head = {};
  Node *cur = &head;

  while (!consume(rest, tok, ")")) {
    tok = skip(ctx, tok, ",");

    Node *assoc = new_node(ctx, ND_GENERIC_ASSOC, tok);

    // A "default:" association is the one that carries no type name.
    if (equal(tok, "default")) {
      tok = skip(ctx, tok->next, ":");
    } else {
      Node *specs = NULL;
      assoc->ty_op = typename(ctx, &tok, tok, &specs);
      assoc->spec_decls = specs;
      tok = skip(ctx, tok, ":");
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
static Node *primary(ChibiccContext *ctx, Token **rest, Token *tok) {
  Token *start = tok;

  if (equal(tok, "(") && equal(tok->next, "{")) {
    // This is a GNU statement expresssion.
    Node *node = new_node(ctx, ND_STMT_EXPR, tok);
    node->body = compound_stmt(ctx, &tok, tok->next->next)->body;
    *rest = skip(ctx, tok, ")");
    return node;
  }

  if (equal(tok, "(")) {
    Node *node = expr(ctx, &tok, tok->next);
    *rest = skip(ctx, tok, ")");
    return node;
  }

  if (equal(tok, "sizeof") && equal(tok->next, "(") && is_typename(ctx, tok->next->next)) {
    Node *specs = NULL;
    Type *ty = typename(ctx, &tok, tok->next->next, &specs);
    *rest = skip(ctx, tok, ")");

    // Keep sizeof faithful; sema folds it to its value.
    Node *node = new_node(ctx, ND_SIZEOF, start);
    node->ty_op = ty;
    node->spec_decls = specs;
    return node;
  }

  if (equal(tok, "sizeof")) {
    // Keep sizeof faithful; sema folds it to its value.
    Node *node = new_node(ctx, ND_SIZEOF, tok);
    node->lhs = unary(ctx, rest, tok->next);
    return node;
  }

  if (equal(tok, "_Alignof") && equal(tok->next, "(") && is_typename(ctx, tok->next->next)) {
    Node *specs = NULL;
    Type *ty = typename(ctx, &tok, tok->next->next, &specs);
    *rest = skip(ctx, tok, ")");

    Node *node = new_node(ctx, ND_ALIGNOF, start);
    node->ty_op = ty;
    node->spec_decls = specs;
    return node;
  }

  if (equal(tok, "_Alignof")) {
    Node *node = new_node(ctx, ND_ALIGNOF, tok);
    node->lhs = unary(ctx, rest, tok->next);
    return node;
  }

  if (equal(tok, "_Generic"))
    return generic_selection(ctx, rest, tok->next);

  if (equal(tok, "__builtin_types_compatible_p")) {
    // Both type operands are recorded; sema folds the node to 0 or 1.
    Node *node = new_node(ctx, ND_TYPES_COMPATIBLE, start);
    Node *specs = NULL;
    tok = skip(ctx, tok->next, "(");
    node->ty_op = typename(ctx, &tok, tok, &specs);
    tok = skip(ctx, tok, ",");
    node->ty_op2 = typename(ctx, &tok, tok, &specs);
    node->spec_decls = specs;
    *rest = skip(ctx, tok, ")");
    return node;
  }

  if (equal(tok, "__builtin_reg_class")) {
    // sema folds the node to the register class of its type operand.
    Node *node = new_node(ctx, ND_REG_CLASS, start);
    Node *specs = NULL;
    tok = skip(ctx, tok->next, "(");
    node->ty_op = typename(ctx, &tok, tok, &specs);
    node->spec_decls = specs;
    *rest = skip(ctx, tok, ")");
    return node;
  }

  if (equal(tok, "__builtin_compare_and_swap")) {
    Node *node = new_node(ctx, ND_CAS, tok);
    tok = skip(ctx, tok->next, "(");
    node->cas_addr = assign(ctx, &tok, tok);
    tok = skip(ctx, tok, ",");
    node->cas_old = assign(ctx, &tok, tok);
    tok = skip(ctx, tok, ",");
    node->cas_new = assign(ctx, &tok, tok);
    *rest = skip(ctx, tok, ")");
    return node;
  }

  if (equal(tok, "__builtin_atomic_exchange")) {
    Node *node = new_node(ctx, ND_EXCH, tok);
    tok = skip(ctx, tok->next, "(");
    node->lhs = assign(ctx, &tok, tok);
    tok = skip(ctx, tok, ",");
    node->rhs = assign(ctx, &tok, tok);
    *rest = skip(ctx, tok, ")");
    return node;
  }

  if (tok->kind == TK_IDENT) {
    // A reference to a variable, a function or an enum constant. Which
    // one it denotes is not a syntax question, so the reference stays
    // faithful and sema's resolve pass binds it, using the scope stack
    // the tree structure gives it.
    Node *node = new_node(ctx, ND_IDENT, tok);
    *rest = tok->next;
    return node;
  }

  if (tok->kind == TK_STR) {
    // Keep the literal faithful; sema lowers it to a reference to an
    // anonymous global when it types the node.
    Node *node = new_node(ctx, ND_STRING, tok);
    *rest = tok->next;
    return node;
  }

  if (tok->kind == TK_NUM) {
    Node *node;
    if (is_flonum(tok->ty)) {
      node = new_node(ctx, ND_NUM, tok);
      node->fval = tok->fval;
    } else {
      node = new_num(ctx, tok->val, tok);
    }

    node->ty = tok->ty;
    *rest = tok->next;
    return node;
  }

  error_tok(ctx, tok, "expected an expression");
}

#if CHIBICC_PRATT_PARSE_FEATURE
//
// The Pratt expression parser (PLAN-pratt-parse).
//
// One loop replaces the per-precedence cascade: infix_bp() turns the
// current token into an operator and a binding power, and expr_bp() keeps
// consuming while the operator binds tighter than min_bp (left
// associative: rbp == lbp; right associative: rbp == lbp - 1). The three
// entry points keep the old names and meanings: expr allows comma, assign
// allows assignment, conditional allows neither. The prefix forms are in
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
  NodeKind kind;
  NodeKind assign_op;
  int lbp;
  int rbp;
} InfixOp;

static const InfixOp infix_ops[] = {
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
static bool infix_bp(Token *tok, NodeKind *kind, NodeKind *assign_op,
                     int *lbp, int *rbp) {
  int n = sizeof(infix_ops) / sizeof(*infix_ops);

  for (int i = 0; i < n; i++) {
    if (!equal(tok, infix_ops[i].op))
      continue;
    *kind = infix_ops[i].kind;
    *assign_op = infix_ops[i].assign_op;
    *lbp = infix_ops[i].lbp;
    *rbp = infix_ops[i].rbp;
    return true;
  }
  return false;
}

static Node *expr_bp(ChibiccContext *ctx, Token **rest, Token *tok, int min_bp);

// The prefix (nud) forms: a cast or compound literal (the `(` type-name
// lookahead), the unary operators, labels-as-values, then the shared
// primary parser for everything atomic. Unary `+` is transparent and
// builds no node, exactly as the cascade did.
static Node *expr_nud(ChibiccContext *ctx, Token **rest, Token *tok) {
  if (equal(tok, "(") && is_typename(ctx, tok->next)) {
    Token *start = tok;
    Node *specs = NULL;
    Type *ty = typename(ctx, &tok, tok->next, &specs);
    tok = skip(ctx, tok, ")");

    // A compound literal. Kept faithful, with the type and the
    // initializer record on the node; sema materializes the object.
    if (equal(tok, "{")) {
      Node *node = new_node(ctx, ND_COMPOUND_LITERAL, start);
      node->ty_op = ty;
      node->spec_decls = specs;
      node->decl_init = init_record(ctx, rest, tok);
      return node;
    }

    // A cast the source wrote: untyped, with the target type recorded
    // for sema. is_implicit stays false (new_cast sets it for the casts
    // sema inserts).
    Node *node = new_node(ctx, ND_CAST, start);
    node->lhs = expr_bp(ctx, rest, tok, BP_CAST);
    node->ty_op = ty;
    node->spec_decls = specs;
    return node;
  }

  if (equal(tok, "+"))
    return expr_bp(ctx, rest, tok->next, BP_CAST);

  if (equal(tok, "-"))
    return new_unary(ctx, ND_NEG, expr_bp(ctx, rest, tok->next, BP_CAST), tok);

  if (equal(tok, "&"))
    return new_unary(ctx, ND_ADDR, expr_bp(ctx, rest, tok->next, BP_CAST), tok);

  if (equal(tok, "*"))
    return new_unary(ctx, ND_DEREF, expr_bp(ctx, rest, tok->next, BP_CAST), tok);

  if (equal(tok, "!"))
    return new_unary(ctx, ND_NOT, expr_bp(ctx, rest, tok->next, BP_CAST), tok);

  if (equal(tok, "~"))
    return new_unary(ctx, ND_BITNOT, expr_bp(ctx, rest, tok->next, BP_CAST), tok);

  if (equal(tok, "++")) {
    Node *node = new_node(ctx, ND_INCDEC, tok);
    node->lhs = expr_bp(ctx, rest, tok->next, BP_CAST);
    node->addend = 1;
    return node;
  }

  if (equal(tok, "--")) {
    Node *node = new_node(ctx, ND_INCDEC, tok);
    node->lhs = expr_bp(ctx, rest, tok->next, BP_CAST);
    node->addend = -1;
    return node;
  }

  // [GNU] labels-as-values. Checked before any infix interpretation, so
  // `&&label` is not a binary `&&`.
  if (equal(tok, "&&")) {
    Node *node = new_node(ctx, ND_LABEL_VAL, tok);
    node->label = get_ident(ctx, tok->next);
    *rest = tok->next->next;
    return node;
  }

  return primary(ctx, rest, tok);
}

// The one expression loop. The depth gate counts here and only here in
// the Pratt build; the entry wrappers below do not add a frame.
static Node *expr_bp(ChibiccContext *ctx, Token **rest, Token *tok, int min_bp) {
  if (++ctx->parse_depth > MAX_PARSE_DEPTH)
    error_tok(ctx, tok, "nesting too deep");

  Node *node = expr_nud(ctx, rest, tok);

  for (;;) {
    Token *op = *rest;
    NodeKind kind;
    NodeKind assign_op;
    int lbp;
    int rbp;

    // The postfix (led) forms bind tighter than every infix operator:
    // call, subscript, member access and postfix increment/decrement.
    if (min_bp < BP_POSTFIX) {
      if (equal(op, "(")) {
        node = funcall(ctx, rest, op->next, node);
        continue;
      }

      if (equal(op, "[")) {
        Token *idx = op->next;
        Node *subscript = new_node(ctx, ND_SUBSCRIPT, op);
        subscript->lhs = node;
        subscript->rhs = expr(ctx, &idx, idx);
        *rest = skip(ctx, idx, "]");
        node = subscript;
        continue;
      }

      if (equal(op, ".")) {
        node = struct_ref(ctx, node, op->next, NULL);
        *rest = op->next->next;
        continue;
      }

      if (equal(op, "->")) {
        node = struct_ref(ctx, node, op->next, op);
        *rest = op->next->next;
        continue;
      }

      if (equal(op, "++")) {
        Node *incdec = new_node(ctx, ND_INCDEC, op);
        incdec->lhs = node;
        incdec->is_post = true;
        incdec->addend = 1;
        node = incdec;
        *rest = op->next;
        continue;
      }

      if (equal(op, "--")) {
        Node *incdec = new_node(ctx, ND_INCDEC, op);
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
      Node *cond = new_node(ctx, ND_COND, op);

      if (equal(op->next, ":")) {
        // [GNU] `a ?: b`: only cond and els, with is_elvis set.
        cond->is_elvis = true;
        cond->cond = node;
        cond->els = expr_bp(ctx, rest, op->next->next, BP_COND - 1);
        node = cond;
        continue;
      }

      Token *mid = NULL;
      cond->cond = node;
      cond->then = expr_bp(ctx, &mid, op->next, 0);
      Token *colon = skip(ctx, mid, ":");
      cond->els = expr_bp(ctx, rest, colon, BP_COND - 1);
      node = cond;
      continue;
    }

    if (kind == ND_ASSIGN) {
      Node *rhs = expr_bp(ctx, rest, op->next, rbp);
      Node *assign = new_binary(ctx, ND_ASSIGN, node, rhs, op);
      assign->op = assign_op;
      node = assign;
      continue;
    }

    node = new_binary(ctx, kind, node, expr_bp(ctx, rest, op->next, rbp), op);
  }

  ctx->parse_depth--;
  return node;
}

// primary's `sizeof expr` and `_Alignof expr` forms parse their operand
// through the old `unary` name; in the Pratt build that is the cast layer.
static Node *unary(ChibiccContext *ctx, Token **rest, Token *tok) {
  return expr_bp(ctx, rest, tok, BP_CAST);
}

static Node *expr(ChibiccContext *ctx, Token **rest, Token *tok) {
  return expr_bp(ctx, rest, tok, 0);
}

static Node *assign(ChibiccContext *ctx, Token **rest, Token *tok) {
  return expr_bp(ctx, rest, tok, BP_ASSIGN - 1);
}

Node *conditional(ChibiccContext *ctx, Token **rest, Token *tok) {
  return expr_bp(ctx, rest, tok, BP_COND - 1);
}
#endif // CHIBICC_PRATT_PARSE_FEATURE

// Parses typedef declarators into ND_TYPEDEF records. The name also
// goes into the typedef-name oracle immediately: the grammar needs it
// to classify the very next identifier.
static Node *parse_typedef(ChibiccContext *ctx, Token **rest, Token *tok, Type *basety) {
  Node head = {};
  Node *cur = &head;
  bool first = true;

  while (!consume(&tok, tok, ";")) {
    if (!first)
      tok = skip(ctx, tok, ",");
    first = false;

    DeclName decl = {};
    Type *ty = declarator(ctx, &tok, tok, basety, &decl);
    if (!decl.name)
      error_tok(ctx, decl.name_pos, "typedef name omitted");

    Node *node = new_node(ctx, ND_TYPEDEF, decl.name);
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
static Node *function_def(ChibiccContext *ctx, Token **rest, Token *tok, Type *basety, VarAttr *attr) {
  DeclName decl = {};
  Type *ty = declarator(ctx, &tok, tok, basety, &decl);
  if (!decl.name)
    error_tok(ctx, decl.name_pos, "function name omitted");

  Node *node = new_node(ctx, ND_FUNCDEF, tok);
  node->ty_op = ty;
  node->name_tok = decl.name;
  node->attr = *attr;
  add_declared_name(ctx, decl.name);

  if (consume(&tok, tok, ";")) {
    *rest = tok;
    return node;
  }

  // The parameters are visible in the whole body, so they shadow
  // typedefs of the same name from here on, one scope above the body's
  // own - where the old begin_function registered them.
  enter_scope(ctx);
  for (Type *p = ty->params; p; p = p->next)
    if (p->name)
      add_declared_name(ctx, p->name);

  tok = skip(ctx, tok, "{");
  node->body = compound_stmt(ctx, &tok, tok);
  leave_scope(ctx);
  *rest = tok;
  return node;
}

// Parses global-variable declarators (at file scope, or `extern` ones
// inside a block) into ND_GVAR_DECL records; sema declares the objects
// and serializes their initializers.
static Node *global_variable(ChibiccContext *ctx, Token **rest, Token *tok, Type *basety, VarAttr *attr) {
  Node head = {};
  Node *cur = &head;
  bool first = true;

  while (!consume(&tok, tok, ";")) {
    if (!first)
      tok = skip(ctx, tok, ",");
    first = false;

    DeclName decl = {};
    Type *ty = declarator(ctx, &tok, tok, basety, &decl);
    if (!decl.name)
      error_tok(ctx, decl.name_pos, "variable name omitted");

    Token *name = decl.name;
    add_declared_name(ctx, name);

    Node *node = new_node(ctx, ND_GVAR_DECL, tok);
    node->ty_op = ty;
    node->name_tok = name;
    node->attr = *attr;
    if (equal(tok, "="))
      node->decl_init = init_record(ctx, &tok, tok->next);
    cur = cur->next = node;
  }

  *rest = tok;
  return head.next;
}

// Lookahead tokens and returns true if a given token is a start
// of a function definition or declaration.
static bool is_function(ChibiccContext *ctx, Token *tok) {
  if (equal(tok, ";"))
    return false;

  Type dummy = {};
  Type *ty = declarator(ctx, &tok, tok, &dummy, NULL);
  return ty->kind == TY_FUNC;
}

// program = (typedef | function-definition | global-variable)*
//
// The top-level declaration-record chain this returns is what sema
// walks, in source order, to declare the objects and analyze the
// function bodies.
Node *parse(ChibiccContext *ctx, Token *tok) {
  parse_init(ctx);
  Node head = {};
  Node *cur = &head;

  while (tok->kind != TK_EOF) {
    VarAttr attr = {};
    Node *specs = NULL;
    Type *basety = declspec(ctx, &tok, tok, &attr, &specs);
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

  return head.next;
}
