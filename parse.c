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

static Type *find_typedef(Token *tok);
static Type *find_tag(Token *tok);
static Type *find_current_tag(Token *tok);
static void push_tag_scope(Token *tok, Type *ty);
static void add_typedef(Node *node);
static void add_declared_name(Token *tok);
static void enter_scope(void);
static void leave_scope(void);

static bool is_typename(Token *tok);
static Type *declspec(Token **rest, Token *tok, VarAttr *attr, Node **specs);
static Type *typename(Token **rest, Token *tok, Node **specs);
static Type *enum_specifier(Token **rest, Token *tok, Node **specs);
static Type *typeof_specifier(Token **rest, Token *tok, Node **specs);
static Type *type_suffix(Token **rest, Token *tok, Type *ty);
static Type *declarator(Token **rest, Token *tok, Type *ty);
static Node *declaration(Token **rest, Token *tok, Type *basety, VarAttr *attr);
static Initializer *init_record(Token **rest, Token *tok);
static Node *compound_stmt(Token **rest, Token *tok);
static Node *stmt(Token **rest, Token *tok);
static Node *expr_stmt(Token **rest, Token *tok);
static Node *expr(Token **rest, Token *tok);
static Node *assign(Token **rest, Token *tok);
static Node *logor(Token **rest, Token *tok);
static Node *logand(Token **rest, Token *tok);
static Node *bitor(Token **rest, Token *tok);
static Node *bitxor(Token **rest, Token *tok);
static Node *bitand(Token **rest, Token *tok);
static Node *equality(Token **rest, Token *tok);
static Node *relational(Token **rest, Token *tok);
static Node *shift(Token **rest, Token *tok);
static Node *add(Token **rest, Token *tok);
static Node *mul(Token **rest, Token *tok);
static Node *cast(Token **rest, Token *tok);
static Type *struct_decl(Token **rest, Token *tok, Node **specs);
static Type *union_decl(Token **rest, Token *tok, Node **specs);
static Node *postfix(Token **rest, Token *tok);
static Node *funcall(Token **rest, Token *tok, Node *node);
static Node *unary(Token **rest, Token *tok);
static Node *primary(Token **rest, Token *tok);
static Node *parse_typedef(Token **rest, Token *tok, Type *basety);
static bool is_function(Token *tok);
static Node *function_def(Token **rest, Token *tok, Type *basety, VarAttr *attr);
static Node *global_variable(Token **rest, Token *tok, Type *basety, VarAttr *attr);

Node *new_node(NodeKind kind, Token *tok) {
  Node *node = calloc(1, sizeof(Node));
  node->kind = kind;
  node->tok = tok;
  return node;
}

Node *new_binary(NodeKind kind, Node *lhs, Node *rhs, Token *tok) {
  Node *node = new_node(kind, tok);
  node->lhs = lhs;
  node->rhs = rhs;
  return node;
}

Node *new_unary(NodeKind kind, Node *expr, Token *tok) {
  Node *node = new_node(kind, tok);
  node->lhs = expr;
  return node;
}

Node *new_num(int64_t val, Token *tok) {
  Node *node = new_node(ND_NUM, tok);
  node->val = val;
  return node;
}

char *get_ident(Token *tok) {
  if (tok->kind != TK_IDENT)
    error_tok(tok, "expected an identifier");
  return strndup(tok->loc, tok->len);
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

static ParseScope *scope = &(ParseScope){};

static void enter_scope(void) {
  ParseScope *sc = calloc(1, sizeof(ParseScope));
  sc->next = scope;
  scope = sc;
}

static void leave_scope(void) {
  scope = scope->next;
}

// The parser's typedef-name oracle: the C grammar needs to know whether
// an identifier names a type before it can parse a declaration.
static Type *find_typedef(Token *tok) {
  if (tok->kind != TK_IDENT)
    return NULL;

  for (ParseScope *sc = scope; sc; sc = sc->next) {
    NameEntry *e = hashmap_get2(&sc->names, tok->loc, tok->len);
    if (e)
      return e->type_def;
  }
  return NULL;
}

// Declares a typedef name in the current scope. The name must be
// visible immediately: the oracle has to classify the very next
// identifier. sema completes the type when it reaches the ND_TYPEDEF
// record this declaration also emits.
static void add_typedef(Node *node) {
  NameEntry *e = calloc(1, sizeof(NameEntry));
  e->type_def = node->ty;
  hashmap_put(&scope->names, get_ident(node->tok), e);
}

// Records a name a declarator just declared (a variable, function,
// parameter or enum constant), so that it shadows a typedef of the same
// spelling for the rest of the parse.
static void add_declared_name(Token *tok) {
  hashmap_put(&scope->names, get_ident(tok), calloc(1, sizeof(NameEntry)));
}

// Find a struct/union/enum tag by name, in any enclosing scope.
static Type *find_tag(Token *tok) {
  for (ParseScope *sc = scope; sc; sc = sc->next) {
    Type *ty = hashmap_get2(&sc->tags, tok->loc, tok->len);
    if (ty)
      return ty;
  }
  return NULL;
}

// Find a tag declared in the current scope only; used when a tag is
// (re)defined, so that a previous forward declaration in the same scope
// keeps its identity.
static Type *find_current_tag(Token *tok) {
  return hashmap_get2(&scope->tags, tok->loc, tok->len);
}

static void push_tag_scope(Token *tok, Type *ty) {
  hashmap_put2(&scope->tags, tok->loc, tok->len, ty);
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
static Type *declspec(Token **rest, Token *tok, VarAttr *attr, Node **specs) {
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

  while (is_typename(tok)) {
    // Handle storage class specifiers.
    if (equal(tok, "typedef") || equal(tok, "static") || equal(tok, "extern") ||
        equal(tok, "inline") || equal(tok, "_Thread_local") || equal(tok, "__thread")) {
      if (!attr)
        error_tok(tok, "storage class specifier is not allowed in this context");

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
        error_tok(tok, "typedef may not be used together with static,"
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
        ty = typename(&tok, tok->next, specs);
        tok = skip(tok, ")");
      }
      is_atomic = true;
      continue;
    }

    if (equal(tok, "_Alignas")) {
      if (!attr)
        error_tok(tok, "_Alignas is not allowed in this context");
      tok = skip(tok->next, "(");

      // The `_Alignas(type)` form asks for a property of a type that
      // may not be complete yet, and the `_Alignas(expr)` form is a
      // constant expression; both are recorded for sema to settle.
      // Whichever comes last is the one that counts, as before.
      if (is_typename(tok)) {
        attr->align_ty = typename(&tok, tok, specs);
        attr->align_expr = NULL;
      } else {
        attr->align_ty = NULL;
        attr->align_expr = conditional(&tok, tok);
      }
      tok = skip(tok, ")");
      continue;
    }

    // Handle user-defined types.
    Type *ty2 = find_typedef(tok);
    if (equal(tok, "struct") || equal(tok, "union") || equal(tok, "enum") ||
        equal(tok, "typeof") || ty2) {
      if (counter)
        break;

      if (equal(tok, "struct")) {
        ty = struct_decl(&tok, tok->next, specs);
      } else if (equal(tok, "union")) {
        ty = union_decl(&tok, tok->next, specs);
      } else if (equal(tok, "enum")) {
        ty = enum_specifier(&tok, tok->next, specs);
      } else if (equal(tok, "typeof")) {
        ty = typeof_specifier(&tok, tok->next, specs);
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
      error_tok(tok, "invalid type");
    }

    tok = tok->next;
  }

  if (is_atomic) {
    ty = copy_type(ty);
    ty->is_atomic = true;
  }

  *rest = tok;
  return ty;
}

// func-params = ("void" | param ("," param)* ("," "...")?)? ")"
// param       = declspec declarator
static Type *func_params(Token **rest, Token *tok, Type *ty) {
  if (equal(tok, "void") && equal(tok->next, ")")) {
    *rest = tok->next->next;
    return func_type(ty);
  }

  Type head = {};
  Type *cur = &head;
  bool is_variadic = false;
  Node *specs = NULL;

  while (!equal(tok, ")")) {
    if (cur != &head)
      tok = skip(tok, ",");

    if (equal(tok, "...")) {
      is_variadic = true;
      tok = tok->next;
      skip(tok, ")");
      break;
    }

    Type *ty2 = declspec(&tok, tok, NULL, &specs);
    ty2 = declarator(&tok, tok, ty2);

    Token *name = ty2->name;

    if (ty2->kind == TY_ARRAY) {
      // "array of T" is converted to "pointer to T" only in the parameter
      // context. For example, *argv[] is converted to **argv by this.
      ty2 = pointer_to(ty2->base);
      ty2->name = name;
    } else if (ty2->kind == TY_FUNC) {
      // Likewise, a function is converted to a pointer to a function
      // only in the parameter context.
      ty2 = pointer_to(ty2);
      ty2->name = name;
    }

    cur = cur->next = copy_type(ty2);
  }

  if (cur == &head)
    is_variadic = true;

  ty = func_type(ty);
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
static Type *array_dimensions(Token **rest, Token *tok, Type *ty) {
  while (equal(tok, "static") || equal(tok, "restrict"))
    tok = tok->next;

  if (equal(tok, "]")) {
    ty = type_suffix(rest, tok->next, ty);
    return array_of(ty, -1);
  }

  Node *expr = conditional(&tok, tok);
  tok = skip(tok, "]");
  ty = type_suffix(rest, tok, ty);

  // Whether the dimension denotes a fixed-length array or a VLA depends
  // on whether the length expression is a constant expression - a
  // semantic question. The expression is recorded here and sema decides
  // the type when it resolves the declarator (see resolve_type).
  return array_of_dim(ty, expr);
}

// type-suffix = "(" func-params
//             | "[" array-dimensions
//             | ε
static Type *type_suffix(Token **rest, Token *tok, Type *ty) {
  if (equal(tok, "("))
    return func_params(rest, tok->next, ty);

  if (equal(tok, "["))
    return array_dimensions(rest, tok->next, ty);

  *rest = tok;
  return ty;
}

// pointers = ("*" ("const" | "volatile" | "restrict")*)*
static Type *pointers(Token **rest, Token *tok, Type *ty) {
  while (consume(&tok, tok, "*")) {
    ty = pointer_to(ty);
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
static Type *declarator(Token **rest, Token *tok, Type *ty) {
  ty = pointers(&tok, tok, ty);

  if (equal(tok, "(")) {
    Token *start = tok;
    Type dummy = {};
    declarator(&tok, start->next, &dummy);
    tok = skip(tok, ")");
    ty = type_suffix(rest, tok, ty);
    return declarator(&tok, start->next, ty);
  }

  Token *name = NULL;
  Token *name_pos = tok;

  if (tok->kind == TK_IDENT) {
    name = tok;
    tok = tok->next;
  }

  ty = type_suffix(rest, tok, ty);
  ty->name = name;
  ty->name_pos = name_pos;
  return ty;
}

// abstract-declarator = pointers ("(" abstract-declarator ")")? type-suffix
static Type *abstract_declarator(Token **rest, Token *tok, Type *ty) {
  ty = pointers(&tok, tok, ty);

  if (equal(tok, "(")) {
    Token *start = tok;
    Type dummy = {};
    abstract_declarator(&tok, start->next, &dummy);
    tok = skip(tok, ")");
    ty = type_suffix(rest, tok, ty);
    return abstract_declarator(&tok, start->next, ty);
  }

  return type_suffix(rest, tok, ty);
}

// type-name = declspec abstract-declarator
static Type *typename(Token **rest, Token *tok, Node **specs) {
  Type *ty = declspec(&tok, tok, NULL, specs);
  return abstract_declarator(rest, tok, ty);
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
static Type *enum_specifier(Token **rest, Token *tok, Node **specs) {
  Type *ty = enum_type();

  // Read a struct tag.
  Token *tag = NULL;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }

  if (tag && !equal(tok, "{")) {
    Type *ty = find_tag(tag);
    if (!ty)
      error_tok(tag, "unknown enum type");
    if (ty->kind != TY_ENUM)
      error_tok(tag, "not an enum tag");
    *rest = tok;
    return ty;
  }

  tok = skip(tok, "{");

  // Read an enum-list. Each member becomes a declaration record node
  // carrying the name, the optional explicit value expression and the
  // enum type; sema evaluates the values and registers the names at
  // this position of the enclosing chain.
  int i = 0;
  while (!consume_end(rest, tok)) {
    if (i++ > 0)
      tok = skip(tok, ",");

    Token *name = tok;
    tok = tok->next;
    add_declared_name(name);

    Node *node = new_node(ND_ENUM_CONST, name);
    if (equal(tok, "="))
      node->lhs = conditional(&tok, tok->next);
    node->ty_op = ty;

    add_spec(specs, node);
  }

  if (tag)
    push_tag_scope(tag, ty);
  return ty;
}

// typeof-specifier = "(" (expr | typename) ")"
static Type *typeof_specifier(Token **rest, Token *tok, Node **specs) {
  tok = skip(tok, "(");

  Type *ty;
  if (is_typename(tok)) {
    ty = typename(&tok, tok, specs);
  } else {
    // What type an expression has is not a syntax question, so the
    // operand is recorded in a placeholder the declarator builds on;
    // sema annotates the expression and fills the record in.
    ty = typeof_placeholder();
    ty->typeof_expr = expr(&tok, tok);
  }
  *rest = skip(tok, ")");
  return ty;
}

// declaration = declspec (declarator ("=" initializer)? ("," declarator ("=" initializer)?)*)? ";"
//
// Each declarator emits an ND_DECL record anchored at the name it
// declares: the name rides on the type (Type.name), the storage class
// and alignment in `attr`, and the initializer stays a faithful record.
// The records go straight into the enclosing statement chain - a
// declaration is not a block in the source, so the block that carries
// them is the one the source wrote. sema declares the object when it
// resolves the record and lowers the node when it types it - including
// the VLA-size sibling statement and the block-scope static's data
// image, whose shapes it reproduces exactly.
static Node *declaration(Token **rest, Token *tok, Type *basety, VarAttr *attr) {
  Node head = {};
  Node *cur = &head;
  int i = 0;

  while (!equal(tok, ";")) {
    if (i++ > 0)
      tok = skip(tok, ",");

    Type *ty = declarator(&tok, tok, basety);
    if (!ty->name)
      error_tok(ty->name_pos, "variable name omitted");

    // The name goes on the shared type only until the next declarator
    // overwrites it, and the declared name shadows a typedef of the
    // same name for the rest of the parse - both before the
    // initializer is parsed, as the old declaration site did.
    Token *name = ty->name;
    add_declared_name(name);

    Initializer *init = NULL;
    if (equal(tok, "=")) {
      Token *eq = tok;
      init = init_record(&tok, tok->next);
      init->eq_tok = eq;
    }

    Node *decl = new_node(ND_DECL, name);
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
static InitDesig *designators(Token **rest, Token *tok) {
  InitDesig head = {};
  InitDesig *cur = &head;

  while (1) {
    if (equal(tok, "[")) {
      InitDesig *desig = calloc(1, sizeof(InitDesig));
      desig->tok = tok;
      desig->begin = conditional(&tok, tok->next);
      desig->after_begin = tok;
      if (equal(tok, "..."))
        desig->end = conditional(&tok, tok->next);
      desig->rbracket = tok;
      tok = skip(tok, "]");
      cur = cur->next = desig;
      continue;
    }

    if (equal(tok, ".")) {
      InitDesig *desig = calloc(1, sizeof(InitDesig));
      desig->tok = tok;
      tok = tok->next;
      if (tok->kind != TK_IDENT)
        error_tok(tok, "expected a field designator");
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
static Initializer *init_record(Token **rest, Token *tok) {
  Initializer *rec = calloc(1, sizeof(Initializer));
  rec->tok = tok;

  if (equal(tok, "{")) {
    rec->kind = INIT_LIST;
    tok = tok->next;

    InitItem head = {};
    InitItem *cur = &head;
    bool first = true;

    while (!consume_end(&tok, tok)) {
      InitItem *item = calloc(1, sizeof(InitItem));
      if (!first) {
        item->comma_tok = tok;
        tok = skip(tok, ",");
      }
      first = false;

      item->desigs = designators(&tok, tok);
      if (item->desigs && equal(tok, "="))
        tok = tok->next;
      item->init = init_record(&tok, tok);
      cur = cur->next = item;
    }

    rec->items = head.next;
    *rest = tok;
    return rec;
  }

  if (tok->kind == TK_STR) {
    rec->kind = INIT_STR;
    rec->str_tok = tok;
    *rest = tok->next;
    return rec;
  }

  rec->kind = INIT_EXPR;
  rec->expr = assign(rest, tok);
  return rec;
}

// Returns true if a given token represents a type.
static bool is_typename(Token *tok) {
  static HashMap map;

  if (map.capacity == 0) {
    static char *kw[] = {
      "void", "_Bool", "char", "short", "int", "long", "struct", "union",
      "typedef", "enum", "static", "extern", "_Alignas", "signed", "unsigned",
      "const", "volatile", "auto", "register", "restrict", "__restrict",
      "__restrict__", "_Noreturn", "float", "double", "typeof", "inline",
      "_Thread_local", "__thread", "_Atomic",
    };

    for (int i = 0; i < sizeof(kw) / sizeof(*kw); i++)
      hashmap_put(&map, kw[i], (void *)1);
  }

  return hashmap_get2(&map, tok->loc, tok->len) || find_typedef(tok);
}

// asm-stmt = "asm" ("volatile" | "inline")* "(" string-literal ")"
static Node *asm_stmt(Token **rest, Token *tok) {
  Node *node = new_node(ND_ASM, tok);
  tok = tok->next;

  while (equal(tok, "volatile") || equal(tok, "inline"))
    tok = tok->next;

  tok = skip(tok, "(");
  if (tok->kind != TK_STR || tok->ty->base->kind != TY_CHAR)
    error_tok(tok, "expected string literal");
  node->asm_str = tok->str;
  *rest = skip(tok->next, ")");
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
static Node *stmt(Token **rest, Token *tok) {
  if (equal(tok, "return")) {
    // The implicit conversion to the return type is sema's: the parser
    // does not know the enclosing function.
    Node *node = new_node(ND_RETURN, tok);
    if (consume(rest, tok->next, ";"))
      return node;

    node->lhs = expr(&tok, tok->next);
    *rest = skip(tok, ";");
    return node;
  }

  if (equal(tok, "if")) {
    Node *node = new_node(ND_IF, tok);
    tok = skip(tok->next, "(");
    node->cond = expr(&tok, tok);
    tok = skip(tok, ")");
    node->then = stmt(&tok, tok);
    if (equal(tok, "else"))
      node->els = stmt(&tok, tok->next);
    *rest = tok;
    return node;
  }

  if (equal(tok, "switch")) {
    Node *node = new_node(ND_SWITCH, tok);
    tok = skip(tok->next, "(");
    node->cond = expr(&tok, tok);
    tok = skip(tok, ")");
    node->then = stmt(rest, tok);
    return node;
  }

  if (equal(tok, "case")) {
    // The values a case label stands for are constant expressions, so
    // they are recorded unevaluated; sema evaluates them into begin/end
    // when it resolves the node. `:` is where the range check is
    // anchored.
    Node *node = new_node(ND_CASE, tok);
    node->begin_expr = conditional(&tok, tok->next);

    if (equal(tok, "...")) {
      // [GNU] Case ranges, e.g. "case 1 ... 5:"
      node->end_expr = conditional(&tok, tok->next);
    }

    node->colon_tok = tok;
    tok = skip(tok, ":");
    node->lhs = stmt(rest, tok);
    return node;
  }

  if (equal(tok, "default")) {
    Node *node = new_node(ND_CASE, tok);
    node->is_default = true;
    tok = skip(tok->next, ":");
    node->lhs = stmt(rest, tok);
    return node;
  }

  if (equal(tok, "for")) {
    Node *node = new_node(ND_FOR, tok);
    tok = skip(tok->next, "(");

    enter_scope();

    if (is_typename(tok)) {
      Node *specs = NULL;
      Type *basety = declspec(&tok, tok, NULL, &specs);
      // Enum records a for-init declspec defined share the init's
      // scope, so they ride at the head of the init chain. The init is
      // a chain of declaration records like any other declaration; sema
      // folds it into one statement once the records are lowered,
      // because codegen's `for` has a single-statement init slot.
      node->init = specs_then(specs, declaration(&tok, tok, basety, NULL));
    } else {
      node->init = expr_stmt(&tok, tok);
    }

    if (!equal(tok, ";"))
      node->cond = expr(&tok, tok);
    tok = skip(tok, ";");

    if (!equal(tok, ")"))
      node->inc = expr(&tok, tok);
    tok = skip(tok, ")");

    node->then = stmt(rest, tok);

    leave_scope();
    return node;
  }

  if (equal(tok, "while")) {
    Node *node = new_node(ND_WHILE, tok);
    tok = skip(tok->next, "(");
    node->cond = expr(&tok, tok);
    tok = skip(tok, ")");
    node->then = stmt(rest, tok);
    return node;
  }

  if (equal(tok, "do")) {
    Node *node = new_node(ND_DO, tok);
    node->then = stmt(&tok, tok->next);

    tok = skip(tok, "while");
    tok = skip(tok, "(");
    node->cond = expr(&tok, tok);
    tok = skip(tok, ")");
    *rest = skip(tok, ";");
    return node;
  }

  if (equal(tok, "asm"))
    return asm_stmt(rest, tok);

  if (equal(tok, "goto")) {
    if (equal(tok->next, "*")) {
      // [GNU] `goto *ptr` jumps to the address specified by `ptr`.
      Node *node = new_node(ND_GOTO_EXPR, tok);
      node->lhs = expr(&tok, tok->next->next);
      *rest = skip(tok, ";");
      return node;
    }

    Node *node = new_node(ND_GOTO, tok);
    node->label = get_ident(tok->next);
    *rest = skip(tok->next->next, ";");
    return node;
  }

  if (equal(tok, "break")) {
    *rest = skip(tok->next, ";");
    return new_node(ND_BREAK, tok);
  }

  if (equal(tok, "continue")) {
    *rest = skip(tok->next, ";");
    return new_node(ND_CONTINUE, tok);
  }

  if (tok->kind == TK_IDENT && equal(tok->next, ":")) {
    Node *node = new_node(ND_LABEL, tok);
    node->label = strndup(tok->loc, tok->len);
    node->lhs = stmt(rest, tok->next->next);
    return node;
  }

  if (equal(tok, "{"))
    return compound_stmt(rest, tok->next);

  return expr_stmt(rest, tok);
}

// compound-stmt = (typedef | declaration | stmt)* "}"
//
// Declaration records that produce no statement of their own (typedefs,
// enum constants, block-scope extern declarations and [GNU] nested
// function definitions) enter the chain at their source position; sema
// removes them again before codegen sees the chain.
static Node *compound_stmt(Token **rest, Token *tok) {
  Node *node = new_node(ND_BLOCK, tok);
  node->is_scope_block = true;
  Node head = {};
  Node *cur = &head;

  enter_scope();

  while (!equal(tok, "}")) {
    if (is_typename(tok) && !equal(tok->next, ":")) {
      VarAttr attr = {};
      Node *specs = NULL;
      Type *basety = declspec(&tok, tok, &attr, &specs);
      cur = chain_append(cur, specs);

      if (attr.is_typedef) {
        cur = chain_append(cur, parse_typedef(&tok, tok, basety));
        continue;
      }

      if (is_function(tok)) {
        cur = cur->next = function_def(&tok, tok, basety, &attr);
        continue;
      }

      if (attr.is_extern) {
        cur = chain_append(cur, global_variable(&tok, tok, basety, &attr));
        continue;
      }

      cur = chain_append(cur, declaration(&tok, tok, basety, &attr));
    } else {
      cur = cur->next = stmt(&tok, tok);
    }
  }

  leave_scope();

  node->body = head.next;
  *rest = tok->next;
  return node;
}

// expr-stmt = expr? ";"
static Node *expr_stmt(Token **rest, Token *tok) {
  if (equal(tok, ";")) {
    *rest = tok->next;
    return new_node(ND_BLOCK, tok);
  }

  Node *node = new_node(ND_EXPR_STMT, tok);
  node->lhs = expr(&tok, tok);
  *rest = skip(tok, ";");
  return node;
}

// expr = assign ("," expr)?
static Node *expr(Token **rest, Token *tok) {
  Node *node = assign(&tok, tok);

  if (equal(tok, ","))
    return new_binary(ND_COMMA, node, expr(rest, tok->next), tok);

  *rest = tok;
  return node;
}

// assign    = conditional (assign-op assign)?
// assign-op = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^="
//           | "<<=" | ">>="
//
// A compound assignment keeps its operator in `op`; sema rewrites it to
// the read-modify-write form (handling the bitfield and atomic cases).
static Node *assign(Token **rest, Token *tok) {
  Node *node = conditional(&tok, tok);

  if (equal(tok, "="))
    return new_binary(ND_ASSIGN, node, assign(rest, tok->next), tok);

  if (equal(tok, "+=")) {
    Node *expr = new_binary(ND_ASSIGN, node, assign(rest, tok->next), tok);
    expr->op = ND_ADD;
    return expr;
  }

  if (equal(tok, "-=")) {
    Node *expr = new_binary(ND_ASSIGN, node, assign(rest, tok->next), tok);
    expr->op = ND_SUB;
    return expr;
  }

  if (equal(tok, "*=")) {
    Node *expr = new_binary(ND_ASSIGN, node, assign(rest, tok->next), tok);
    expr->op = ND_MUL;
    return expr;
  }

  if (equal(tok, "/=")) {
    Node *expr = new_binary(ND_ASSIGN, node, assign(rest, tok->next), tok);
    expr->op = ND_DIV;
    return expr;
  }

  if (equal(tok, "%=")) {
    Node *expr = new_binary(ND_ASSIGN, node, assign(rest, tok->next), tok);
    expr->op = ND_MOD;
    return expr;
  }

  if (equal(tok, "&=")) {
    Node *expr = new_binary(ND_ASSIGN, node, assign(rest, tok->next), tok);
    expr->op = ND_BITAND;
    return expr;
  }

  if (equal(tok, "|=")) {
    Node *expr = new_binary(ND_ASSIGN, node, assign(rest, tok->next), tok);
    expr->op = ND_BITOR;
    return expr;
  }

  if (equal(tok, "^=")) {
    Node *expr = new_binary(ND_ASSIGN, node, assign(rest, tok->next), tok);
    expr->op = ND_BITXOR;
    return expr;
  }

  if (equal(tok, "<<=")) {
    Node *expr = new_binary(ND_ASSIGN, node, assign(rest, tok->next), tok);
    expr->op = ND_SHL;
    return expr;
  }

  if (equal(tok, ">>=")) {
    Node *expr = new_binary(ND_ASSIGN, node, assign(rest, tok->next), tok);
    expr->op = ND_SHR;
    return expr;
  }

  *rest = tok;
  return node;
}

// conditional = logor ("?" expr? ":" conditional)?
Node *conditional(Token **rest, Token *tok) {
  Node *cond = logor(&tok, tok);

  if (!equal(tok, "?")) {
    *rest = tok;
    return cond;
  }

  if (equal(tok->next, ":")) {
    // [GNU] `a ?: b`. Kept as-is in the tree with is_elvis set;
    // sema lowers it to `tmp = a, tmp ? tmp : b`.
    Node *node = new_node(ND_COND, tok);
    node->is_elvis = true;
    node->cond = cond;
    node->els = conditional(rest, tok->next->next);
    return node;
  }

  Node *node = new_node(ND_COND, tok);
  node->cond = cond;
  node->then = expr(&tok, tok->next);
  tok = skip(tok, ":");
  node->els = conditional(rest, tok);
  return node;
}

// logor = logand ("||" logand)*
static Node *logor(Token **rest, Token *tok) {
  Node *node = logand(&tok, tok);
  while (equal(tok, "||")) {
    Token *start = tok;
    node = new_binary(ND_LOGOR, node, logand(&tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

// logand = bitor ("&&" bitor)*
static Node *logand(Token **rest, Token *tok) {
  Node *node = bitor(&tok, tok);
  while (equal(tok, "&&")) {
    Token *start = tok;
    node = new_binary(ND_LOGAND, node, bitor(&tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

// bitor = bitxor ("|" bitxor)*
static Node *bitor(Token **rest, Token *tok) {
  Node *node = bitxor(&tok, tok);
  while (equal(tok, "|")) {
    Token *start = tok;
    node = new_binary(ND_BITOR, node, bitxor(&tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

// bitxor = bitand ("^" bitand)*
static Node *bitxor(Token **rest, Token *tok) {
  Node *node = bitand(&tok, tok);
  while (equal(tok, "^")) {
    Token *start = tok;
    node = new_binary(ND_BITXOR, node, bitand(&tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

// bitand = equality ("&" equality)*
static Node *bitand(Token **rest, Token *tok) {
  Node *node = equality(&tok, tok);
  while (equal(tok, "&")) {
    Token *start = tok;
    node = new_binary(ND_BITAND, node, equality(&tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

// equality = relational ("==" relational | "!=" relational)*
static Node *equality(Token **rest, Token *tok) {
  Node *node = relational(&tok, tok);

  for (;;) {
    Token *start = tok;

    if (equal(tok, "==")) {
      node = new_binary(ND_EQ, node, relational(&tok, tok->next), start);
      continue;
    }

    if (equal(tok, "!=")) {
      node = new_binary(ND_NE, node, relational(&tok, tok->next), start);
      continue;
    }

    *rest = tok;
    return node;
  }
}

// relational = shift ("<" shift | "<=" shift | ">" shift | ">=" shift)*
static Node *relational(Token **rest, Token *tok) {
  Node *node = shift(&tok, tok);

  for (;;) {
    Token *start = tok;

    if (equal(tok, "<")) {
      node = new_binary(ND_LT, node, shift(&tok, tok->next), start);
      continue;
    }

    if (equal(tok, "<=")) {
      node = new_binary(ND_LE, node, shift(&tok, tok->next), start);
      continue;
    }

    if (equal(tok, ">")) {
      node = new_binary(ND_GT, node, shift(&tok, tok->next), start);
      continue;
    }

    if (equal(tok, ">=")) {
      node = new_binary(ND_GE, node, shift(&tok, tok->next), start);
      continue;
    }

    *rest = tok;
    return node;
  }
}

// shift = add ("<<" add | ">>" add)*
static Node *shift(Token **rest, Token *tok) {
  Node *node = add(&tok, tok);

  for (;;) {
    Token *start = tok;

    if (equal(tok, "<<")) {
      node = new_binary(ND_SHL, node, add(&tok, tok->next), start);
      continue;
    }

    if (equal(tok, ">>")) {
      node = new_binary(ND_SHR, node, add(&tok, tok->next), start);
      continue;
    }

    *rest = tok;
    return node;
  }
}
// add = mul ("+" mul | "-" mul)*
static Node *add(Token **rest, Token *tok) {
  Node *node = mul(&tok, tok);

  for (;;) {
    Token *start = tok;

    if (equal(tok, "+")) {
      node = new_binary(ND_ADD, node, mul(&tok, tok->next), start);
      continue;
    }

    if (equal(tok, "-")) {
      node = new_binary(ND_SUB, node, mul(&tok, tok->next), start);
      continue;
    }

    *rest = tok;
    return node;
  }
}

// mul = cast ("*" cast | "/" cast | "%" cast)*
static Node *mul(Token **rest, Token *tok) {
  Node *node = cast(&tok, tok);

  for (;;) {
    Token *start = tok;

    if (equal(tok, "*")) {
      node = new_binary(ND_MUL, node, cast(&tok, tok->next), start);
      continue;
    }

    if (equal(tok, "/")) {
      node = new_binary(ND_DIV, node, cast(&tok, tok->next), start);
      continue;
    }

    if (equal(tok, "%")) {
      node = new_binary(ND_MOD, node, cast(&tok, tok->next), start);
      continue;
    }

    *rest = tok;
    return node;
  }
}

// cast = "(" type-name ")" cast | unary
static Node *cast(Token **rest, Token *tok) {
  if (equal(tok, "(") && is_typename(tok->next)) {
    Token *start = tok;
    Node *specs = NULL;
    Type *ty = typename(&tok, tok->next, &specs);
    tok = skip(tok, ")");

    // compound literal
    if (equal(tok, "{"))
      return unary(rest, start);

    // Type cast: kept faithful and untyped, with the target type
    // recorded for sema to complete and apply. This is a cast the
    // source wrote, so is_implicit stays false; the casts sema inserts
    // to carry out an implicit conversion are the ones new_cast marks.
    Node *node = new_node(ND_CAST, start);
    node->lhs = cast(rest, tok);
    node->ty_op = ty;
    node->spec_decls = specs;
    return node;
  }

  return unary(rest, tok);
}

// unary = ("+" | "-" | "*" | "&" | "!" | "~") cast
//       | ("++" | "--") unary
//       | "&&" ident
//       | postfix
static Node *unary(Token **rest, Token *tok) {
  if (equal(tok, "+"))
    return cast(rest, tok->next);

  if (equal(tok, "-"))
    return new_unary(ND_NEG, cast(rest, tok->next), tok);

  if (equal(tok, "&")) {
    // The bitfield check lives in sema: it needs the operand's type.
    return new_unary(ND_ADDR, cast(rest, tok->next), tok);
  }

  if (equal(tok, "*")) {
    // [https://www.sigbus.info/n1570#6.5.3.2p4] This is an oddity
    // in the C spec, but dereferencing a function shouldn't do
    // anything. If foo is a function, `*foo`, `**foo` or `*****foo`
    // are all equivalent to just `foo`. The check needs the operand's
    // type, so sema drops the node when it sees one.
    return new_unary(ND_DEREF, cast(rest, tok->next), tok);
  }

  if (equal(tok, "!"))
    return new_unary(ND_NOT, cast(rest, tok->next), tok);

  if (equal(tok, "~"))
    return new_unary(ND_BITNOT, cast(rest, tok->next), tok);

  // Read ++i as a faithful prefix increment; sema lowers it to i+=1
  if (equal(tok, "++")) {
    Node *node = new_node(ND_INCDEC, tok);
    node->lhs = unary(rest, tok->next);
    node->addend = 1;
    return node;
  }

  // Read --i as a faithful prefix decrement; sema lowers it to i-=1
  if (equal(tok, "--")) {
    Node *node = new_node(ND_INCDEC, tok);
    node->lhs = unary(rest, tok->next);
    node->addend = -1;
    return node;
  }

  // [GNU] labels-as-values
  if (equal(tok, "&&")) {
    Node *node = new_node(ND_LABEL_VAL, tok);
    node->label = get_ident(tok->next);
    *rest = tok->next->next;
    return node;
  }

  return postfix(rest, tok);
}

// struct-members = (declspec declarator (","  declarator)* ";")*
//
// Member placement (offsets, size, alignment, bitfield widths) and the
// flexible-array-member conversion are sema's; the member list here is
// the syntax shape, with `_Alignas` and bitfield widths recorded
// unevaluated.
static void struct_members(Token **rest, Token *tok, Type *ty, Node **specs) {
  Member head = {};
  Member *cur = &head;
  int idx = 0;

  while (!equal(tok, "}")) {
    VarAttr attr = {};
    Type *basety = declspec(&tok, tok, &attr, specs);
    bool first = true;

    // An anonymous member is recognized by the kind of the base type.
    // A `typeof(expr)` base has not settled its kind yet, so the
    // placeholder counts as a candidate too; sema rejects a nameless
    // member that turns out not to be an aggregate.
    if ((basety->kind == TY_STRUCT || basety->kind == TY_UNION ||
         basety->kind == TY_TYPEOF) &&
        consume(&tok, tok, ";")) {
      Member *mem = calloc(1, sizeof(Member));
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
        tok = skip(tok, ",");
      first = false;

      Member *mem = calloc(1, sizeof(Member));
      mem->ty = declarator(&tok, tok, basety);
      mem->name = mem->ty->name;
      mem->idx = idx++;
      mem->align_ty = attr.align_ty;
      mem->align_expr = attr.align_expr;

      if (consume(&tok, tok, ":")) {
        mem->is_bitfield = true;
        // The width is a constant expression; it is recorded
        // unevaluated and sema evaluates it when the aggregate is
        // laid out.
        mem->width_expr = conditional(&tok, tok);
      }

      cur = cur->next = mem;
    }
  }

  *rest = tok->next;
  ty->members = head.next;
}

// attribute = ("__attribute__" "(" "(" "packed" ")" ")")*
static Token *attribute_list(Token *tok, Type *ty) {
  while (consume(&tok, tok, "__attribute__")) {
    tok = skip(tok, "(");
    tok = skip(tok, "(");

    bool first = true;

    while (!consume(&tok, tok, ")")) {
      if (!first)
        tok = skip(tok, ",");
      first = false;

      if (consume(&tok, tok, "packed")) {
        ty->is_packed = true;
        continue;
      }

      if (consume(&tok, tok, "aligned")) {
        tok = skip(tok, "(");
        // The alignment is a constant expression, so it is recorded and
        // sema evaluates it when the type is completed.
        ty->align_expr = conditional(&tok, tok);
        tok = skip(tok, ")");
        continue;
      }

      error_tok(tok, "unknown attribute");
    }

    tok = skip(tok, ")");
  }

  return tok;
}

// struct-union-decl = attribute? ident? ("{" struct-members)?
static Type *struct_union_decl(Token **rest, Token *tok, Node **specs) {
  Type *ty = struct_type();
  tok = attribute_list(tok, ty);

  // Read a tag.
  Token *tag = NULL;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }

  if (tag && !equal(tok, "{")) {
    *rest = tok;

    Type *ty2 = find_tag(tag);
    if (ty2)
      return ty2;

    ty->size = -1;
    push_tag_scope(tag, ty);
    return ty;
  }

  tok = skip(tok, "{");

  // Construct a struct object. The member list is syntax; placing the
  // members is sema's job, which it does when the type is completed -
  // at the definition for a bare one, or at the first declaration that
  // resolves a type referring to it.
  struct_members(&tok, tok, ty, specs);
  *rest = attribute_list(tok, ty);
  ty->layout_pending = true;

  Type *ret = ty;
  if (tag) {
    // If this is a redefinition, overwrite a previous type.
    // Otherwise, register the struct type.
    Type *ty2 = find_current_tag(tag);
    if (ty2) {
      *ty2 = *ty;
      ret = ty2;
    } else {
      push_tag_scope(tag, ty);
    }
  }

  return ret;
}

// struct-decl = struct-union-decl
static Type *struct_decl(Token **rest, Token *tok, Node **specs) {
  Type *ty = struct_union_decl(rest, tok, specs);
  ty->kind = TY_STRUCT;
  return ty;
}

// union-decl = struct-union-decl
static Type *union_decl(Token **rest, Token *tok, Node **specs) {
  Type *ty = struct_union_decl(rest, tok, specs);
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
static Node *struct_ref(Node *node, Token *tok, Token *arrow) {
  Node *mem = new_unary(ND_MEMBER, node, tok);
  mem->arrow_tok = arrow;
  return mem;
}

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
static Node *postfix(Token **rest, Token *tok) {
  if (equal(tok, "(") && is_typename(tok->next)) {
    // Compound literal. Kept faithful: the type and the initializer
    // record ride on the node, and sema materializes the hidden
    // variable (an anonymous global at file scope, a hidden local in a
    // block) when it resolves the node, then lowers it when typing.
    Token *start = tok;
    Node *specs = NULL;
    Type *ty = typename(&tok, tok->next, &specs);
    tok = skip(tok, ")");

    Node *node = new_node(ND_COMPOUND_LITERAL, start);
    node->ty_op = ty;
    node->spec_decls = specs;
    node->decl_init = init_record(rest, tok);
    return node;
  }

  Node *node = primary(&tok, tok);

  for (;;) {
    if (equal(tok, "(")) {
      node = funcall(&tok, tok->next, node);
      continue;
    }

    if (equal(tok, "[")) {
      Token *start = tok;
      Node *idx = expr(&tok, tok->next);
      tok = skip(tok, "]");
      Node *subscript = new_node(ND_SUBSCRIPT, start);
      subscript->lhs = node;
      subscript->rhs = idx;
      node = subscript;
      continue;
    }

    if (equal(tok, ".")) {
      node = struct_ref(node, tok->next, NULL);
      tok = tok->next->next;
      continue;
    }

    if (equal(tok, "->")) {
      // x->y is short for (*x).y
      node = struct_ref(node, tok->next, tok);
      tok = tok->next->next;
      continue;
    }

    if (equal(tok, "++")) {
      Node *incdec = new_node(ND_INCDEC, tok);
      incdec->lhs = node;
      incdec->is_post = true;
      incdec->addend = 1;
      node = incdec;
      tok = tok->next;
      continue;
    }

    if (equal(tok, "--")) {
      Node *incdec = new_node(ND_INCDEC, tok);
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

// funcall = (assign ("," assign)*)? ")"
//
// The callee check, the argument conversions and the struct-return
// buffer are sema's; the node stays faithful, anchored at the closing
// paren where the argument-count diagnostics point.
static Node *funcall(Token **rest, Token *tok, Node *fn) {
  Node head = {};
  Node *cur = &head;

  while (!equal(tok, ")")) {
    if (cur != &head)
      tok = skip(tok, ",");

    cur = cur->next = assign(&tok, tok);
  }

  *rest = skip(tok, ")");

  Node *node = new_unary(ND_FUNCALL, fn, tok);
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
static Node *generic_selection(Token **rest, Token *tok) {
  Node *node = new_node(ND_GENERIC, tok);

  tok = skip(tok, "(");
  node->cond = assign(&tok, tok);

  Node head = {};
  Node *cur = &head;

  while (!consume(rest, tok, ")")) {
    tok = skip(tok, ",");

    Node *assoc = new_node(ND_GENERIC_ASSOC, tok);

    // A "default:" association is the one that carries no type name.
    if (equal(tok, "default")) {
      tok = skip(tok->next, ":");
    } else {
      Node *specs = NULL;
      assoc->ty_op = typename(&tok, tok, &specs);
      assoc->spec_decls = specs;
      tok = skip(tok, ":");
    }
    assoc->lhs = assign(&tok, tok);

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
static Node *primary(Token **rest, Token *tok) {
  Token *start = tok;

  if (equal(tok, "(") && equal(tok->next, "{")) {
    // This is a GNU statement expresssion.
    Node *node = new_node(ND_STMT_EXPR, tok);
    node->body = compound_stmt(&tok, tok->next->next)->body;
    *rest = skip(tok, ")");
    return node;
  }

  if (equal(tok, "(")) {
    Node *node = expr(&tok, tok->next);
    *rest = skip(tok, ")");
    return node;
  }

  if (equal(tok, "sizeof") && equal(tok->next, "(") && is_typename(tok->next->next)) {
    Node *specs = NULL;
    Type *ty = typename(&tok, tok->next->next, &specs);
    *rest = skip(tok, ")");

    // Keep sizeof faithful; sema folds it to its value.
    Node *node = new_node(ND_SIZEOF, start);
    node->ty_op = ty;
    node->spec_decls = specs;
    return node;
  }

  if (equal(tok, "sizeof")) {
    // Keep sizeof faithful; sema folds it to its value.
    Node *node = new_node(ND_SIZEOF, tok);
    node->lhs = unary(rest, tok->next);
    return node;
  }

  if (equal(tok, "_Alignof") && equal(tok->next, "(") && is_typename(tok->next->next)) {
    Node *specs = NULL;
    Type *ty = typename(&tok, tok->next->next, &specs);
    *rest = skip(tok, ")");

    Node *node = new_node(ND_ALIGNOF, start);
    node->ty_op = ty;
    node->spec_decls = specs;
    return node;
  }

  if (equal(tok, "_Alignof")) {
    Node *node = new_node(ND_ALIGNOF, tok);
    node->lhs = unary(rest, tok->next);
    return node;
  }

  if (equal(tok, "_Generic"))
    return generic_selection(rest, tok->next);

  if (equal(tok, "__builtin_types_compatible_p")) {
    // Both type operands are recorded; sema folds the node to 0 or 1.
    Node *node = new_node(ND_TYPES_COMPATIBLE, start);
    Node *specs = NULL;
    tok = skip(tok->next, "(");
    node->ty_op = typename(&tok, tok, &specs);
    tok = skip(tok, ",");
    node->ty_op2 = typename(&tok, tok, &specs);
    node->spec_decls = specs;
    *rest = skip(tok, ")");
    return node;
  }

  if (equal(tok, "__builtin_reg_class")) {
    // sema folds the node to the register class of its type operand.
    Node *node = new_node(ND_REG_CLASS, start);
    Node *specs = NULL;
    tok = skip(tok->next, "(");
    node->ty_op = typename(&tok, tok, &specs);
    node->spec_decls = specs;
    *rest = skip(tok, ")");
    return node;
  }

  if (equal(tok, "__builtin_compare_and_swap")) {
    Node *node = new_node(ND_CAS, tok);
    tok = skip(tok->next, "(");
    node->cas_addr = assign(&tok, tok);
    tok = skip(tok, ",");
    node->cas_old = assign(&tok, tok);
    tok = skip(tok, ",");
    node->cas_new = assign(&tok, tok);
    *rest = skip(tok, ")");
    return node;
  }

  if (equal(tok, "__builtin_atomic_exchange")) {
    Node *node = new_node(ND_EXCH, tok);
    tok = skip(tok->next, "(");
    node->lhs = assign(&tok, tok);
    tok = skip(tok, ",");
    node->rhs = assign(&tok, tok);
    *rest = skip(tok, ")");
    return node;
  }

  if (tok->kind == TK_IDENT) {
    // A reference to a variable, a function or an enum constant. Which
    // one it denotes is not a syntax question, so the reference stays
    // faithful and sema's resolve pass binds it, using the scope stack
    // the tree structure gives it.
    Node *node = new_node(ND_IDENT, tok);
    *rest = tok->next;
    return node;
  }

  if (tok->kind == TK_STR) {
    // Keep the literal faithful; sema lowers it to a reference to an
    // anonymous global when it types the node.
    Node *node = new_node(ND_STRING, tok);
    *rest = tok->next;
    return node;
  }

  if (tok->kind == TK_NUM) {
    Node *node;
    if (is_flonum(tok->ty)) {
      node = new_node(ND_NUM, tok);
      node->fval = tok->fval;
    } else {
      node = new_num(tok->val, tok);
    }

    node->ty = tok->ty;
    *rest = tok->next;
    return node;
  }

  error_tok(tok, "expected an expression");
}

// Parses typedef declarators into ND_TYPEDEF records. The name also
// goes into the typedef-name oracle immediately: the grammar needs it
// to classify the very next identifier.
static Node *parse_typedef(Token **rest, Token *tok, Type *basety) {
  Node head = {};
  Node *cur = &head;
  bool first = true;

  while (!consume(&tok, tok, ";")) {
    if (!first)
      tok = skip(tok, ",");
    first = false;

    Type *ty = declarator(&tok, tok, basety);
    if (!ty->name)
      error_tok(ty->name_pos, "typedef name omitted");

    Node *node = new_node(ND_TYPEDEF, ty->name);
    node->ty = ty;
    add_typedef(node);
    cur = cur->next = node;
  }

  *rest = tok;
  return head.next;
}

// Parses a function declarator, and its body if one follows, into an
// ND_FUNCDEF record. sema declares the function (checking it against a
// previous declaration, anchored at `tok`) and analyzes the body.
static Node *function_def(Token **rest, Token *tok, Type *basety, VarAttr *attr) {
  Type *ty = declarator(&tok, tok, basety);
  if (!ty->name)
    error_tok(ty->name_pos, "function name omitted");

  Node *node = new_node(ND_FUNCDEF, tok);
  node->ty_op = ty;
  node->name_tok = ty->name;
  node->attr = *attr;
  add_declared_name(ty->name);

  if (consume(&tok, tok, ";")) {
    *rest = tok;
    return node;
  }

  // The parameters are visible in the whole body, so they shadow
  // typedefs of the same name from here on, one scope above the body's
  // own - where the old begin_function registered them.
  enter_scope();
  for (Type *p = ty->params; p; p = p->next)
    if (p->name)
      add_declared_name(p->name);

  tok = skip(tok, "{");
  node->body = compound_stmt(&tok, tok);
  leave_scope();
  *rest = tok;
  return node;
}

// Parses global-variable declarators (at file scope, or `extern` ones
// inside a block) into ND_GVAR_DECL records; sema declares the objects
// and serializes their initializers.
static Node *global_variable(Token **rest, Token *tok, Type *basety, VarAttr *attr) {
  Node head = {};
  Node *cur = &head;
  bool first = true;

  while (!consume(&tok, tok, ";")) {
    if (!first)
      tok = skip(tok, ",");
    first = false;

    Type *ty = declarator(&tok, tok, basety);
    if (!ty->name)
      error_tok(ty->name_pos, "variable name omitted");

    Token *name = ty->name;
    add_declared_name(name);

    Node *node = new_node(ND_GVAR_DECL, tok);
    node->ty_op = ty;
    node->name_tok = name;
    node->attr = *attr;
    if (equal(tok, "="))
      node->decl_init = init_record(&tok, tok->next);
    cur = cur->next = node;
  }

  *rest = tok;
  return head.next;
}

// Lookahead tokens and returns true if a given token is a start
// of a function definition or declaration.
static bool is_function(Token *tok) {
  if (equal(tok, ";"))
    return false;

  Type dummy = {};
  Type *ty = declarator(&tok, tok, &dummy);
  return ty->kind == TY_FUNC;
}

// program = (typedef | function-definition | global-variable)*
//
// The top-level declaration-record chain this returns is what sema
// walks, in source order, to declare the objects and analyze the
// function bodies.
Node *parse(Token *tok) {
  Node head = {};
  Node *cur = &head;

  while (tok->kind != TK_EOF) {
    VarAttr attr = {};
    Node *specs = NULL;
    Type *basety = declspec(&tok, tok, &attr, &specs);
    cur = chain_append(cur, specs);

    // Typedef
    if (attr.is_typedef) {
      cur = chain_append(cur, parse_typedef(&tok, tok, basety));
      continue;
    }

    // Function
    if (is_function(tok)) {
      cur = cur->next = function_def(&tok, tok, basety, &attr);
      continue;
    }

    // Global variable
    cur = chain_append(cur, global_variable(&tok, tok, basety, &attr));
  }

  return head.next;
}
