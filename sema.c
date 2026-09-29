// This file hosts the semantic analysis passes that operate on the
// faithful syntax tree built by parse.c.
//
// sema runs in two traversals per function body (and record by record
// at file scope):
//
//  - the resolve pass (resolve_node and friends) derives the scope
//    stack from the tree structure, declares the objects the parser's
//    declaration records name, binds every ND_IDENT, evaluates and
//    registers enum constants, settles the type records the parser
//    left to be completed (array dimensions, typeof operands,
//    alignments, bitfield widths, case values) and lays aggregates
//    out;
//
//  - the annotation + lowering pass (add_type and type_chain) types
//    every node and performs the lowerings codegen expects (pointer
//    scaling, compound assignment, increment, function calls, string
//    literals, initializer flattening, control-flow labels via
//    analyze).
//
// It also contains the constant expression evaluator (eval and
// friends), which both passes use, and which the preprocessor uses
// for `#if` through const_expr.
//
// A handful of its type-level tools are exported for consumers that
// build nodes of their own - add_type, new_arith, usual_arith_conv,
// get_common_type, new_cast and the node constructors (see chibicc.h).
// A consumer obtains its conversions from here rather than restating
// them; what it contributes is the shape, not the semantics.

#include "chibicc.h"

static int64_t eval(Node *node);
static int64_t eval2(Node *node, char ***label);
static int64_t eval_rval(Node *node, char ***label);
static double eval_double(Node *node);
static bool is_const_expr(Node *node);
static void layout_struct(Type *ty);
static void layout_union(Type *ty);
static void type_chain(Node **head);
static void analyze(Node *body);
static void analyze_function(Obj *fn);
static void resolve_node(Node *node);
static void resolve_chain(Node *node);
static void resolve_function(Node *node);
static void resolve_enum_records(Node *recs);
static void resolve_type_exprs(Type *ty);
static void resolve_type(Type *ty);
static void serialize_gvar(Node *node);
static int attr_align(VarAttr *attr);
static Obj *new_gvar(char *name, Type *ty);
static Obj *new_lvar(char *name, Type *ty);
static char *new_unique_name(void);
static Member *get_struct_member(Type *ty, Token *tok);

// All local variable instances created for the function being analyzed
// are accumulated to this list.
static Obj *locals;

// Likewise, global variables are accumulated to this list.
static Obj *globals;

// The function sema is currently working on, needed when a reference to
// a "static inline" function is recorded and when a return statement is
// converted to the return type. Like the parser's old current_fn, it
// persists after a [GNU] nested function definition is analyzed.
static Obj *sema_fn;

// The declaration of `alloca`, which the VLA lowering calls. It is a
// global object so that the call node can refer to it, but never a
// definition, so codegen emits nothing for it.
static Obj *builtin_alloca;

// Constructors for the node shapes only sema produces: a bound name
// (the parser leaves names unbound, as ND_IDENT), a VLA designator, and
// a numeric literal whose type is settled at birth rather than read off
// a token. Exported (see chibicc.h): a consumer that builds a node needs
// it to arrive typed, and these are the shapes it cannot build itself.
Node *new_var_node(Obj *var, Token *tok) {
  Node *node = new_node(ND_VAR, tok);
  node->var = var;
  return node;
}

Node *new_vla_ptr(Obj *var, Token *tok) {
  Node *node = new_node(ND_VLA_PTR, tok);
  node->var = var;
  return node;
}

Node *new_long(int64_t val, Token *tok) {
  Node *node = new_node(ND_NUM, tok);
  node->val = val;
  node->ty = ty_long;
  return node;
}

Node *new_ulong(long val, Token *tok) {
  Node *node = new_node(ND_NUM, tok);
  node->val = val;
  node->ty = ty_ulong;
  return node;
}

// The one place an implicit cast is built: every conversion sema
// inserts - the usual arithmetic conversions, an assignment, a return
// or an argument converted to its target type, the cast a lowered
// increment restores the operand's type with - comes through here, so
// marking the node is a single assignment. A cast the source wrote
// arrives as an ND_CAST node from the parser instead, which leaves the
// flag false, and add_type resolves rather than builds it.
Node *new_cast(Node *expr, Type *ty) {
  add_type(expr);

  Node *node = calloc(1, sizeof(Node));
  node->kind = ND_CAST;
  node->tok = expr->tok;
  node->lhs = expr;
  node->ty = copy_type(ty);
  node->is_implicit = true;
  return node;
}

// An object or an enum constant the resolve pass has declared.
typedef struct {
  Obj *var;
  Type *enum_ty;
  int enum_val;
} VarScope;

// Represents a block scope.
typedef struct Scope Scope;
struct Scope {
  Scope *next;
  HashMap vars;
};

// The resolve pass derives this scope stack from the tree structure, so
// it lives entirely within sema; the parser keeps a separate stack of
// its own for the typedef/tag classification the C grammar needs.
static Scope *scope = &(Scope){};

static void enter_scope(void) {
  Scope *sc = calloc(1, sizeof(Scope));
  sc->next = scope;
  scope = sc;
}

static void leave_scope(void) {
  scope = scope->next;
}

// Find a variable by name.
static VarScope *find_var(Token *tok) {
  for (Scope *sc = scope; sc; sc = sc->next) {
    VarScope *sc2 = hashmap_get2(&sc->vars, tok->loc, tok->len);
    if (sc2)
      return sc2;
  }
  return NULL;
}

static VarScope *push_scope(char *name) {
  VarScope *sc = calloc(1, sizeof(VarScope));
  hashmap_put(&scope->vars, name, sc);
  return sc;
}

// A block-scope declaration may not declare a void object. `tok` is the
// position of the declaration, anchored exactly as the parser used to.
static void check_declared_void(Token *tok, Type *ty) {
  if (ty->kind == TY_VOID)
    error_tok(tok, "variable declared void");
}

// Looks up a function in the file scope.
static Obj *find_func(char *name) {
  Scope *sc = scope;
  while (sc->next)
    sc = sc->next;

  VarScope *sc2 = hashmap_get(&sc->vars, name);
  if (sc2 && sc2->var && sc2->var->is_function)
    return sc2->var;
  return NULL;
}

// Declares a function at file scope, or checks a redeclaration against
// the object declared before. `tok` is the token following the
// declarator and `is_definition` says whether a body follows.
static Obj *declare_function(char *name, Type *ty, VarAttr *attr, Token *tok,
                             bool is_definition) {
  Obj *fn = find_func(name);

  if (fn) {
    // Redeclaration. The first check is unreachable in the current
    // tree: find_func only ever returns an is_function object, so
    // fn->is_function is always true here (the diagnostic lock
    // leaves it uncovered for exactly that reason).
    if (!fn->is_function)
      error_tok(tok, "redeclared as a different kind of symbol");
    if (fn->is_definition && is_definition)
      error_tok(tok, "redefinition of %s", name);
    if (!fn->is_static && attr->is_static)
      error_tok(tok, "static declaration follows a non-static declaration");
    fn->is_definition = fn->is_definition || is_definition;
  } else {
    fn = new_gvar(name, ty);
    fn->is_function = true;
    fn->is_definition = is_definition;
    fn->is_static = attr->is_static || (attr->is_inline && !attr->is_extern);
    fn->is_inline = attr->is_inline;
  }

  fn->is_root = !(fn->is_static && fn->is_inline);
  return fn;
}

// The state of the control-flow descent (analyze): the loop a
// break/continue binds to, the switch a case label belongs to, and the
// gotos and labels of the function descended so far.
static char *brk_label;
static char *cont_label;
static Node *current_switch;
static Node *gotos;
static Node *labels;

// Sets the unique label of every goto in a function to that of the
// matching label. Gotos may refer to a label that appears later, so
// this runs once the whole body has been descended.
static void resolve_labels(void) {
  for (Node *x = gotos; x; x = x->goto_next) {
    for (Node *y = labels; y; y = y->goto_next) {
      if (!strcmp(x->label, y->label)) {
        x->unique_label = y->unique_label;
        break;
      }
    }

    if (x->unique_label == NULL)
      error_tok(x->tok->next, "use of undeclared label");
  }
}

static void analyze_node(Node *node);

static void analyze_chain(Node *node) {
  for (Node *n = node; n; n = n->next)
    analyze_node(n);
}

// Descends every subtree a node owns, statements and expressions alike:
// a break can sit in a statement expression, which is an expression
// node holding a statement chain.
static void analyze_children(Node *node) {
  analyze_node(node->lhs);
  analyze_node(node->rhs);
  analyze_node(node->cond);
  analyze_node(node->then);
  analyze_node(node->els);
  analyze_node(node->init);
  analyze_node(node->inc);
  analyze_chain(node->body);
  analyze_chain(node->args);
  analyze_node(node->cas_addr);
  analyze_node(node->cas_old);
  analyze_node(node->cas_new);
}

// The descent is pre-order: a loop or a switch owns its labels before
// its body is visited, which is what binds a break in that body to the
// innermost enclosing one.
static void analyze_node(Node *node) {
  if (!node)
    return;

  switch (node->kind) {
  case ND_WHILE:
  case ND_DO:
  case ND_FOR: {
    // A loop sema built itself (the compare-and-swap retry loop of an
    // atomic compound assignment) arrives with its labels allocated.
    if (!node->brk_label)
      node->brk_label = new_unique_name();
    if (!node->cont_label)
      node->cont_label = new_unique_name();

    char *brk = brk_label;
    char *cont = cont_label;
    brk_label = node->brk_label;
    cont_label = node->cont_label;
    analyze_children(node);
    brk_label = brk;
    cont_label = cont;

    // Lower the faithful `while` to the ND_FOR shape codegen
    // understands: no init/inc, so cont_label jumps to the loop top.
    if (node->kind == ND_WHILE)
      node->kind = ND_FOR;
    return;
  }
  case ND_SWITCH: {
    if (!node->brk_label)
      node->brk_label = new_unique_name();

    // A switch is a break target but not a continue target, so the
    // continue label of an enclosing loop stays in force in the body.
    Node *sw = current_switch;
    char *brk = brk_label;
    current_switch = node;
    brk_label = node->brk_label;
    analyze_children(node);
    current_switch = sw;
    brk_label = brk;
    return;
  }
  case ND_CASE:
    if (!current_switch)
      error_tok(node->tok, node->is_default ? "stray default" : "stray case");

    node->label = new_unique_name();
    analyze_node(node->lhs);

    // Linked after the body: a case label may sit inside the statement
    // of an earlier one (`case 0: while (..) { .. case 1: .. }`), and
    // the chain is prepended, so this order is what puts the outer case
    // ahead of the buried one in codegen's comparison chain.
    if (node->is_default) {
      current_switch->default_case = node;
    } else {
      node->case_next = current_switch->case_next;
      current_switch->case_next = node;
    }
    return;
  case ND_BREAK:
    if (!brk_label)
      error_tok(node->tok, "stray break");
    node->unique_label = brk_label;
    node->kind = ND_GOTO;
    return;
  case ND_CONTINUE:
    if (!cont_label)
      error_tok(node->tok, "stray continue");
    node->unique_label = cont_label;
    node->kind = ND_GOTO;
    return;
  case ND_GOTO:
    node->goto_next = gotos;
    gotos = node;
    return;
  case ND_LABEL:
    node->unique_label = new_unique_name();
    node->goto_next = labels;
    labels = node;
    analyze_node(node->lhs);
    return;
  default:
    analyze_children(node);
    return;
  }
}

static void analyze(Node *body) {
  analyze_chain(body);
  resolve_labels();
  gotos = labels = NULL;
}

static void mark_live(Obj *var) {
  if (!var->is_function || var->is_live)
    return;
  var->is_live = true;

  for (int i = 0; i < var->refs.len; i++) {
    Obj *fn = find_func(var->refs.data[i]);
    if (fn)
      mark_live(fn);
  }
}

// Remove redundant tentative definitions.
static void scan_globals(void) {
  Obj head;
  Obj *cur = &head;

  for (Obj *var = globals; var; var = var->next) {
    if (!var->is_tentative) {
      cur = cur->next = var;
      continue;
    }

    // Find another definition of the same identifier.
    Obj *var2 = globals;
    for (; var2; var2 = var2->next)
      if (var != var2 && var2->is_definition && !strcmp(var->name, var2->name))
        break;

    // If there's another definition, the tentative definition
    // is redundant
    if (!var2)
      cur = cur->next = var;
  }

  cur->next = NULL;
  globals = head.next;
}

// Finishes the translated unit: marks the functions reachable from a
// root live and drops the redundant tentative definitions.
static void finalize_globals(void) {
  for (Obj *var = globals; var; var = var->next)
    if (var->is_root)
      mark_live(var);

  scan_globals();
}

// Evaluates and registers a run of enum-constant declaration records.
// The parser records each member with its name, its optional explicit
// value expression and the enum type; a member without an explicit
// value continues the running value of the list, exactly as the old
// on-the-spot evaluation did. Each record is consumed (ty_op cleared)
// so that a later walk skips it.
static void resolve_enum_records(Node *recs) {
  // The walk stops at the first record that is not an enum constant:
  // on the top-level and block chains the run is followed by whatever
  // declaration record comes next, which carries its own type payload.
  while (recs && recs->kind == ND_ENUM_CONST) {
    Type *ety = recs->ty_op;
    if (!ety) {
      recs = recs->next;
      continue;
    }

    int val = 0;
    while (recs && recs->kind == ND_ENUM_CONST && recs->ty_op == ety) {
      if (recs->lhs) {
        resolve_node(recs->lhs);
        val = (int)eval(recs->lhs);
      }
      recs->val = val++;
      recs->ty_op = NULL;

      VarScope *sc = push_scope(get_ident(recs->tok));
      sc->enum_ty = ety;
      sc->enum_val = recs->val;
      recs = recs->next;
    }
  }
}

// Completes the type records the parser's declarator layer could not
// decide. It walks the type a declarator built, so that an inner
// dimension or operand is settled before the outer one wrapped in it -
// the order the parser's own recursion gave. A record is cleared as it is
// consumed, so calling this again costs nothing.
static void resolve_type(Type *ty) {
  if (!ty)
    return;

  switch (ty->kind) {
  case TY_PTR:
  case TY_VLA:
    resolve_type(ty->base);
    break;
  case TY_ARRAY:
    resolve_type(ty->base);
    // An array the parser built on a not-yet-laid-out aggregate was
    // sized with placeholder values; recompute now that the element
    // type is complete. (A pending dimension recomputes below.)
    if (!ty->dim_len && ty->array_len >= 0) {
      ty->size = (ty->base->size < 0) ? -1 : ty->base->size * ty->array_len;
      ty->align = ty->base->align;
    }
    break;
  case TY_STRUCT:
  case TY_UNION:
    // The parser records the member list; placing the members is
    // sema's job, done on first sight of the completed type. An
    // aggregate's own members are placed before an outer one consumes
    // their sizes, because the member types are resolved first.
    if (ty->layout_pending) {
      ty->layout_pending = false;
      if (ty->kind == TY_STRUCT)
        layout_struct(ty);
      else
        layout_union(ty);
    }
    break;
  case TY_FUNC:
    // Enum constants a parameter's declspec defined ride on the
    // function type; they become visible when the type is completed.
    if (ty->spec_decls) {
      resolve_enum_records(ty->spec_decls);
      ty->spec_decls = NULL;
    }
    resolve_type(ty->return_ty);
    for (Type *param = ty->params; param; param = param->next)
      resolve_type(param);
    break;
  default:
    break;
  }

  if (ty->align_expr) {
    ty->align = (int) eval(ty->align_expr);
    ty->align_expr = NULL;
  }

  if (ty->typeof_expr) {
    Node *expr = ty->typeof_expr;
    ty->typeof_expr = NULL;

    // The operand's type fills this record in rather than replacing it:
    // the parser has already built the rest of the declarator's types on
    // top of it, and the name it read off the declarator belongs here.
    add_type(expr);
    resolve_type(expr->ty);

    Token *name = ty->name;
    Token *name_pos = ty->name_pos;
    *ty = *expr->ty;
    ty->name = name;
    ty->name_pos = name_pos;
  }

  if (ty->dim_len) {
    Node *dim = ty->dim_len;
    ty->dim_len = NULL;

    // A dimension that is not a constant expression, like the base of a
    // VLA, gives a variable-length array.
    if (ty->base->kind == TY_VLA || !is_const_expr(dim)) {
      ty->kind = TY_VLA;
      ty->vla_len = dim;
      ty->size = 8;
      ty->align = 8;
    } else {
      ty->array_len = (int) eval(dim);
      ty->size = ty->base->size * ty->array_len;
      ty->align = ty->base->align;
    }
  }
}

// Walks a type's pending records and resolves the expression nodes
// hanging off them (array dimensions, typeof operands, alignments,
// bitfield widths) without evaluating anything: the names inside get
// bound and the declaration records inside (a statement expression can
// appear in a constant context) get declared, so that the evaluations
// resolve_type performs afterwards find a complete picture.
static void resolve_type_exprs(Type *ty) {
  if (!ty)
    return;

  switch (ty->kind) {
  case TY_PTR:
  case TY_VLA:
    resolve_type_exprs(ty->base);
    break;
  case TY_ARRAY:
    resolve_type_exprs(ty->base);
    resolve_node(ty->dim_len);
    break;
  case TY_FUNC:
    resolve_type_exprs(ty->return_ty);
    for (Type *param = ty->params; param; param = param->next)
      resolve_type_exprs(param);
    break;
  case TY_STRUCT:
  case TY_UNION:
    // Layout resolves the member types itself; walking them here would
    // recurse forever on a self-referential member. The pending flag
    // bounds the recursion exactly as it does for the layout.
    if (ty->layout_pending) {
      ty->layout_pending = false;
      for (Member *mem = ty->members; mem; mem = mem->next) {
        resolve_type_exprs(mem->ty);
        resolve_node(mem->width_expr);
        resolve_node(mem->align_expr);
        resolve_type_exprs(mem->align_ty);
      }
      ty->layout_pending = true;
    }
    break;
  default:
    break;
  }

  resolve_node(ty->align_expr);
  resolve_node(ty->typeof_expr);
}

// The alignment a declaration's `_Alignas` asks for. The parser records
// both forms: the `_Alignas(type)` one as a type whose alignment is
// read once the type is complete, and the `_Alignas(expr)` one as a
// constant expression to evaluate. The value is cached, because one
// declaration declares several objects and each of them asks.
static int attr_align(VarAttr *attr) {
  if (!attr)
    return 0;
  if (attr->align_expr) {
    attr->align = (int) eval(attr->align_expr);
    attr->align_expr = NULL;
    return attr->align;
  }
  if (attr->align_ty) {
    resolve_type(attr->align_ty);
    attr->align = attr->align_ty->align;
    attr->align_ty = NULL;
    return attr->align;
  }
  return attr->align;
}

static int align_down(int n, int align) {
  return align_to(n - align + 1, align);
}

// Completes the member records before they are placed: a member type
// may still carry parser stashes (a dimension, a typeof operand), and
// the `_Alignas` forms are settled here. A trailing incomplete-array
// member becomes the zero-sized flexible array member.
static void finalize_members(Type *ty) {
  for (Member *mem = ty->members; mem; mem = mem->next) {
    resolve_type_exprs(mem->ty);
    resolve_type(mem->ty);

    if (mem->align_ty) {
      resolve_type(mem->align_ty);
      mem->align = mem->align_ty->align;
      mem->align_ty = NULL;
    }
    if (mem->align_expr) {
      mem->align = (int) eval(mem->align_expr);
      mem->align_expr = NULL;
    }
    if (!mem->align)
      mem->align = mem->ty->align;
  }

  // If the last element is an array of incomplete type, it's
  // called a "flexible array member". It should behave as if
  // if were a zero-sized array.
  Member *last = NULL;
  for (Member *mem = ty->members; mem; mem = mem->next)
    last = mem;
  if (last && last->ty->kind == TY_ARRAY && last->ty->array_len < 0) {
    last->ty = array_of(last->ty->base, 0);
    ty->is_flexible = true;
  }
}

// Evaluate the recorded bitfield width expressions of a member list.
// The parser records the width unevaluated; layout is where the value
// is first needed.
static void eval_bitfield_widths(Type *ty) {
  for (Member *mem = ty->members; mem; mem = mem->next) {
    if (mem->width_expr) {
      mem->bit_width = eval(mem->width_expr);
      mem->width_expr = NULL;
    }
  }
}

// Assigns an offset to every member of a struct and computes its size
// and alignment. The parser builds the member list (the syntax shape)
// and marks it complete; resolve_type calls this on first sight, and
// nested aggregates are laid out inner-first by the resolve order.
static void layout_struct(Type *ty) {
  // An `aligned` attribute on the type is a constant expression the
  // parser recorded; it has to be settled before the offsets are.
  resolve_type(ty);

  // Defensive: an incomplete type has no members to place.
  if (ty->size < 0)
    return;

  finalize_members(ty);
  eval_bitfield_widths(ty);

  int bits = 0;

  for (Member *mem = ty->members; mem; mem = mem->next) {
    if (mem->is_bitfield && mem->bit_width == 0) {
      // Zero-width anonymous bitfield has a special meaning.
      // It affects only alignment.
      bits = align_to(bits, mem->ty->size * 8);
    } else if (mem->is_bitfield) {
      int sz = mem->ty->size;
      if (bits / (sz * 8) != (bits + mem->bit_width - 1) / (sz * 8))
        bits = align_to(bits, sz * 8);

      mem->offset = align_down(bits / 8, sz);
      mem->bit_offset = bits % (sz * 8);
      bits += mem->bit_width;
    } else {
      if (!ty->is_packed)
        bits = align_to(bits, mem->align * 8);
      mem->offset = bits / 8;
      bits += mem->ty->size * 8;
    }

    if (!ty->is_packed && ty->align < mem->align)
      ty->align = mem->align;
  }

  ty->size = align_to(bits, ty->align * 8) / 8;
}

// Unions need no member offsets (they are all zero), only the union
// of the member sizes and the largest member alignment.
static void layout_union(Type *ty) {
  resolve_type(ty);

  if (ty->size < 0)
    return;

  finalize_members(ty);
  eval_bitfield_widths(ty);

  for (Member *mem = ty->members; mem; mem = mem->next) {
    if (ty->align < mem->align)
      ty->align = mem->align;
    if (ty->size < mem->ty->size)
      ty->size = mem->ty->size;
  }
  ty->size = align_to(ty->size, ty->align);
}

// Binds an unresolved name. A variable or function reference becomes
// ND_VAR; an enum constant becomes ND_NUM.
static void bind_ident(Node *node) {
  Token *tok = node->tok;
  VarScope *sc = find_var(tok);
  Obj *var = sc ? sc->var : NULL;

  if (var) {
    // For "static inline" functions, record the reference so that the
    // liveness analysis can tell which ones are actually needed.
    if (var->is_function) {
      if (sema_fn)
        strarray_push(&sema_fn->refs, var->name);
      else
        var->is_root = true;
    }

    node->kind = ND_VAR;
    node->var = var;
    node->ty = var->ty;
    return;
  }

  if (sc && sc->enum_ty) {
    node->kind = ND_NUM;
    node->val = sc->enum_val;
    node->ty = ty_int;
    return;
  }

  if (equal(tok->next, "("))
    error_tok(tok, "implicit declaration of a function");
  error_tok(tok, "undefined variable");
}

static Obj *new_var(char *name, Type *ty) {
  Obj *var = calloc(1, sizeof(Obj));
  var->name = name;
  var->ty = ty;
  resolve_type(ty);
  var->align = ty->align;
  push_scope(name)->var = var;
  return var;
}

static Obj *new_lvar(char *name, Type *ty) {
  Obj *var = new_var(name, ty);
  var->is_local = true;
  var->next = locals;
  locals = var;
  return var;
}

static Obj *new_gvar(char *name, Type *ty) {
  Obj *var = new_var(name, ty);
  var->next = globals;
  var->is_static = true;
  var->is_definition = true;
  globals = var;
  return var;
}

// The anonymous names of hidden objects - string literal globals,
// static locals and the control-flow labels sema allocates - come
// from this one counter.
// The one name factory for the objects the compiler names on its
// own: string-literal globals, block-scope statics, compound
// literals and control-flow labels all draw from this counter, so
// their numbers interleave in creation order - and .data block
// order is creation order, which is why these allocations stay on
// the sema side even when labels move. The format is load-bearing:
// the snapshot normalizer renumbers labels whose suffix is exactly
// ".<digits>", so a prefix change shows up as a diff even when
// only the numbering moved. A2.1 moves label allocation to codegen
// against this same namespace (one counter), not a parallel one.
static char *new_unique_name(void) {
  static int id = 0;
  return format(".L..%d", id++);
}

static Obj *new_anon_gvar(Type *ty) {
  return new_gvar(new_unique_name(), ty);
}

static Obj *new_string_literal(char *p, Type *ty) {
  Obj *var = new_anon_gvar(ty);
  var->init_data = p;
  return var;
}

static void create_param_lvars(Type *param) {
  if (param) {
    create_param_lvars(param->next);
    if (!param->name)
      error_tok(param->name_pos, "parameter name omitted");
    new_lvar(get_ident(param->name), param);
  }
}

// Sets up the variables a function definition owns: the parameters
// (including the hidden buffer for a large struct/union return value),
// the __va_area__ and __alloca_size__ helpers, and the __func__ /
// __FUNCTION__ strings. The resolve pass calls this once it reaches the
// function body, so that everything is created in the same order as
// before.
static void begin_function(Obj *fn, Type *ty) {
  locals = NULL;
  create_param_lvars(ty->params);

  // A buffer for a struct/union return value is passed
  // as the hidden first parameter.
  Type *rty = ty->return_ty;
  if ((rty->kind == TY_STRUCT || rty->kind == TY_UNION) && rty->size > 16)
    new_lvar("", pointer_to(rty));

  fn->params = locals;

  if (ty->is_variadic)
    fn->va_area = new_lvar("__va_area__", array_of(ty_char, 136));
  fn->alloca_bottom = new_lvar("__alloca_size__", pointer_to(ty_char));

  // [https://www.sigbus.info/n1570#6.4.2.2p1] "__func__" is
  // automatically defined as a local variable containing
  // the current function name.
  push_scope("__func__")->var =
    new_string_literal(fn->name, array_of(ty_char, strlen(fn->name) + 1));

  // [GNU] __FUNCTION__ is yet another name of __func__.
  push_scope("__FUNCTION__")->var =
    new_string_literal(fn->name, array_of(ty_char, strlen(fn->name) + 1));
}

static void declare_builtin_functions(void) {
  Type *ty = func_type(pointer_to(ty_void));
  ty->params = copy_type(ty_int);
  builtin_alloca = new_gvar("alloca", ty);
  builtin_alloca->is_definition = false;
}

// Build the `alloca(<size>)` call node for a VLA declaration. The call
// arrives fully typed, so add_type skips it and codegen recognizes the
// builtin by name.
Node *new_alloca(Node *sz) {
  Node *node = new_unary(ND_FUNCALL, new_var_node(builtin_alloca, sz->tok), sz->tok);
  node->func_ty = builtin_alloca->ty;
  node->ty = builtin_alloca->ty->return_ty;
  node->args = sz;
  add_type(sz);
  return node;
}

// Builds a binary arithmetic node out of operands a lowering has
// already settled on: the usual arithmetic conversions are applied and
// the node comes back fully typed, so add_type has nothing left to do
// with it. This is what makes new_add/new_sub total - every `+`/`-` in
// the tree, whether the parser wrote it or sema rebuilt it from a
// compound assignment or a subscript, is scaled and converted exactly
// once, and no node needs a flag saying which of the two it had.
//
// Exported: this is how a consumer obtains a typed binary node, and with
// it sema's conversion decisions, without re-implementing either.
Node *new_arith(NodeKind kind, Node *lhs, Node *rhs, Token *tok) {
  add_type(lhs);
  add_type(rhs);
  usual_arith_conv(&lhs, &rhs);

  Node *node = new_binary(kind, lhs, rhs, tok);
  node->ty = lhs->ty;
  return node;
}

// The scaled operand of an additive operator on a pointer: `p + n`
// advances by sizeof(*p) * n rather than by n, and when the pointee is
// a VLA, by its runtime size rather than a compile-time one.
static Node *scale_rhs(Type *ptr_ty, Node *rhs, Token *tok) {
  if (ptr_ty->base->kind == TY_VLA)
    return new_binary(ND_MUL, rhs, new_var_node(ptr_ty->base->vla_size, tok), tok);
  return new_binary(ND_MUL, rhs, new_long(ptr_ty->base->size, tok), tok);
}

// In C, `+` operator is overloaded to perform the pointer arithmetic.
// If p is a pointer, p+n adds not n but sizeof(*p)*n to the value of p,
// so that p+n points to the location n elements (not bytes) ahead of p.
// In other words, we need to scale an integer value before adding to a
// pointer value. This function takes care of the scaling.
static Node *new_add(Node *lhs, Node *rhs, Token *tok) {
  add_type(lhs);
  add_type(rhs);

  // num + num
  if (is_numeric(lhs->ty) && is_numeric(rhs->ty))
    return new_arith(ND_ADD, lhs, rhs, tok);

  if (lhs->ty->base && rhs->ty->base)
    error_tok(tok, "invalid operands");

  // Canonicalize `num + ptr` to `ptr + num`.
  if (!lhs->ty->base && rhs->ty->base) {
    Node *tmp = lhs;
    lhs = rhs;
    rhs = tmp;
  }

  // ptr + num; a pointer to a VLA scales by its runtime size
  return new_arith(ND_ADD, lhs, scale_rhs(lhs->ty, rhs, tok), tok);
}

// Like `+`, `-` is overloaded for the pointer type.
static Node *new_sub(Node *lhs, Node *rhs, Token *tok) {
  add_type(lhs);
  add_type(rhs);

  // num - num
  if (is_numeric(lhs->ty) && is_numeric(rhs->ty))
    return new_arith(ND_SUB, lhs, rhs, tok);

  // pointer to a VLA - num
  if (lhs->ty->base && lhs->ty->base->kind == TY_VLA)
    return new_arith(ND_SUB, lhs, scale_rhs(lhs->ty, rhs, tok), tok);

  // ptr - num
  if (lhs->ty->base && is_integer(rhs->ty))
    return new_arith(ND_SUB, lhs, scale_rhs(lhs->ty, rhs, tok), tok);

  // ptr - ptr, which returns how many elements are between the two. The
  // difference itself is an element count, so it is typed long and left
  // alone; only the division that follows it is converted.
  if (lhs->ty->base && rhs->ty->base) {
    Node *node = new_binary(ND_SUB, lhs, rhs, tok);
    node->ty = ty_long;
    return new_arith(ND_DIV, node, new_num(lhs->ty->base->size, tok), tok);
  }

  error_tok(tok, "invalid operands");
}

// Builds the `lhs op rhs` combination a compound assignment or an
// atomic retry loop needs, given an operand whose pointer scaling has
// already been applied. An additive operator goes through new_arith,
// which is the shape `+`/`-` produce; the others are handed to add_type,
// which converts the multiplicative and bitwise ones and leaves the
// shifts alone.
static Node *combine(NodeKind op, Node *lhs, Node *rhs, Token *tok) {
  if (op == ND_ADD || op == ND_SUB)
    return new_arith(op, lhs, rhs, tok);

  Node *node = new_binary(op, lhs, rhs, tok);
  add_type(node);
  return node;
}

// Build the `lhs op rhs` operand expression for a compound assignment
// driven by node->op.
static Node *compound_op(Node *node, Node *lhs, Token *tok) {
  Node *rhs = node->rhs;

  // `+=`/`-=` are routed through new_add/new_sub so that the operand
  // checks run and the pointer arithmetic is scaled exactly as for
  // plain `+`/`-`; the operand they settle on is the one the rebuilt
  // expression combines with `lhs`.
  if (node->op == ND_ADD || node->op == ND_SUB) {
    Node *scaled = node->op == ND_ADD ? new_add(node->lhs, node->rhs, tok)
                                      : new_sub(node->lhs, node->rhs, tok);
    rhs = scaled->rhs;
  }

  return combine(node->op, lhs, rhs, tok);
}

// Convert op= operators to expressions containing an assignment.
//
// `node` is an ND_ASSIGN whose `op` holds the compound assignment
// operator. In general, `A op= C` is converted to
// ``tmp = &A, *tmp = *tmp op C`.
// However, if a given expression is of form `A.x op= C`, the input is
// converted to `tmp = &A, (*tmp).x = (*tmp).x op C` to handle assignments
// to bitfields.
static Node *to_assign(Node *node) {
  add_type(node->lhs);
  add_type(node->rhs);
  Token *tok = node->tok;

  // Convert `A.x op= C` to `tmp = &A, (*tmp).x = (*tmp).x op C`.
  if (node->lhs->kind == ND_MEMBER) {
    Obj *var = new_lvar("", pointer_to(node->lhs->lhs->ty));

    Node *expr1 = new_binary(ND_ASSIGN, new_var_node(var, tok),
                             new_unary(ND_ADDR, node->lhs->lhs, tok), tok);

    Node *expr2 = new_unary(ND_MEMBER,
                            new_unary(ND_DEREF, new_var_node(var, tok), tok),
                            tok);
    expr2->member = node->lhs->member;

    Node *expr3 = new_unary(ND_MEMBER,
                            new_unary(ND_DEREF, new_var_node(var, tok), tok),
                            tok);
    expr3->member = node->lhs->member;

    Node *expr4 = new_binary(ND_ASSIGN, expr2,
                             compound_op(node, expr3, tok),
                             tok);

    return new_binary(ND_COMMA, expr1, expr4, tok);
  }

  // If A is an atomic type, Convert `A op= B` to
  //
  // ({
  //   T1 *addr = &A; T2 val = (B); T1 old = *addr; T1 new;
  //   do {
  //    new = old op val;
  //   } while (!atomic_compare_exchange_strong(addr, &old, new));
  //   new;
  // })
  if (node->lhs->ty->is_atomic) {
    // The `val` temporary holds the operand as `op` would have it, so
    // for `+=`/`-=` the scaling is computed eagerly here - through
    // new_add/new_sub, which is also what runs the operand checks.
    Node *rhs = node->rhs;
    if (node->op == ND_ADD || node->op == ND_SUB) {
      Node *operand = node->op == ND_ADD ? new_add(node->lhs, node->rhs, tok)
                                         : new_sub(node->lhs, node->rhs, tok);
      rhs = operand->rhs;
    }
    add_type(rhs);

    Node head = {};
    Node *cur = &head;

    Obj *addr = new_lvar("", pointer_to(node->lhs->ty));
    Obj *val = new_lvar("", rhs->ty);
    Obj *old = new_lvar("", node->lhs->ty);
    Obj *new = new_lvar("", node->lhs->ty);

    cur = cur->next =
      new_unary(ND_EXPR_STMT,
                new_binary(ND_ASSIGN, new_var_node(addr, tok),
                           new_unary(ND_ADDR, node->lhs, tok), tok),
                tok);

    cur = cur->next =
      new_unary(ND_EXPR_STMT,
                new_binary(ND_ASSIGN, new_var_node(val, tok), rhs, tok),
                tok);

    cur = cur->next =
      new_unary(ND_EXPR_STMT,
                new_binary(ND_ASSIGN, new_var_node(old, tok),
                           new_unary(ND_DEREF, new_var_node(addr, tok), tok), tok),
                tok);

    Node *loop = new_node(ND_DO, tok);
    // Pre-allocated so the descent does not have to: analyze's
    // ND_FOR/ND_SWITCH guards skip nodes that arrive with labels.
    // A2.1 moves label allocation to codegen and must delete these
    // two writes with it - two counters would mint the same .L..N
    // twice, and as rejects the duplicate label.
    loop->brk_label = new_unique_name();
    loop->cont_label = new_unique_name();

    // `val` already holds the operand as `op` would have it - scaled
    // for an additive operator on a pointer - so the retry loop
    // combines it with the old value without scaling again.
    Node *body = new_binary(ND_ASSIGN,
                            new_var_node(new, tok),
                            combine(node->op, new_var_node(old, tok),
                                    new_var_node(val, tok), tok),
                            tok);

    loop->then = new_node(ND_BLOCK, tok);
    loop->then->body = new_unary(ND_EXPR_STMT, body, tok);

    Node *cas = new_node(ND_CAS, tok);
    cas->cas_addr = new_var_node(addr, tok);
    cas->cas_old = new_unary(ND_ADDR, new_var_node(old, tok), tok);
    cas->cas_new = new_var_node(new, tok);
    loop->cond = new_unary(ND_NOT, cas, tok);

    cur = cur->next = loop;
    cur = cur->next = new_unary(ND_EXPR_STMT, new_var_node(new, tok), tok);

    Node *stmt_expr = new_node(ND_STMT_EXPR, tok);
    stmt_expr->body = head.next;
    return stmt_expr;
  }

  // Convert `A op= B` to ``tmp = &A, *tmp = *tmp op B`.
  Obj *var = new_lvar("", pointer_to(node->lhs->ty));

  Node *expr1 = new_binary(ND_ASSIGN, new_var_node(var, tok),
                           new_unary(ND_ADDR, node->lhs, tok), tok);

  Node *expr2 =
    new_binary(ND_ASSIGN,
               new_unary(ND_DEREF, new_var_node(var, tok), tok),
               compound_op(node, new_unary(ND_DEREF, new_var_node(var, tok), tok), tok),
               tok);

  return new_binary(ND_COMMA, expr1, expr2, tok);
}

// Convert A++ to `(typeof A)((A += 1) - 1)`. Moved from parse.c.
static Node *new_inc_dec(Node *node, Token *tok, int addend) {
  add_type(node);
  Node *expr = new_binary(ND_ASSIGN, node, new_num(addend, tok), tok);
  expr->op = ND_ADD;
  Node *sub = new_add(to_assign(expr), new_num(-addend, tok), tok);
  return new_cast(sub, node->ty);
}

// Turns a faithful call node into the shape codegen expects: the callee
// must be a function or a pointer to one, each argument is converted to
// its parameter type (an argument past the parameter list is promoted
// instead, since it belongs to a variadic tail), and a struct or union
// return value gets the buffer the caller owns. `tok` is the call's
// closing paren, where the argument-count diagnostics are anchored.
static void lower_funcall(Node *node, Token *tok) {
  Node *fn = node->lhs;
  Type *ty = fn->ty;

  if (ty->kind != TY_FUNC &&
      (ty->kind != TY_PTR || ty->base->kind != TY_FUNC))
    error_tok(fn->tok, "not a function");

  if (ty->kind != TY_FUNC)
    ty = ty->base;

  node->func_ty = ty;

  Type *param_ty = ty->params;
  Node head = {};
  Node *cur = &head;

  for (Node *arg = node->args; arg;) {
    Node *next = arg->next;
    add_type(arg);

    if (!param_ty && !ty->is_variadic)
      error_tok(tok, "too many arguments");

    if (param_ty) {
      if (param_ty->kind != TY_STRUCT && param_ty->kind != TY_UNION)
        arg = new_cast(arg, param_ty);
      param_ty = param_ty->next;
    } else if (arg->ty->kind == TY_FLOAT) {
      // If parameter type is omitted (e.g. in "..."), float
      // arguments are promoted to double.
      arg = new_cast(arg, ty_double);
    }

    arg->next = NULL;
    cur = cur->next = arg;
    arg = next;
  }

  if (param_ty)
    error_tok(tok, "too few arguments");

  node->args = head.next;
  node->ty = ty->return_ty;

  // If a function returns a struct, it is caller's responsibility
  // to allocate a space for the return value.
  if (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION)
    node->ret_buffer = new_lvar("", node->ty);
}

// Generate code for computing a VLA size: one assignment per VLA the
// type holds, sequenced innermost first, or NULL when it holds none - so
// a declaration of a fixed-size object produces no code at all.
static Node *compute_vla_size(Type *ty, Token *tok) {
  Node *node = ty->base ? compute_vla_size(ty->base, tok) : NULL;

  if (ty->kind != TY_VLA)
    return node;

  Node *base_sz;
  if (ty->base->kind == TY_VLA)
    base_sz = new_var_node(ty->base->vla_size, tok);
  else
    base_sz = new_num(ty->base->size, tok);

  ty->vla_size = new_lvar("", ty_ulong);
  Node *expr = new_binary(ND_ASSIGN, new_var_node(ty->vla_size, tok),
                          new_binary(ND_MUL, ty->vla_len, base_sz, tok),
                          tok);
  return node ? new_binary(ND_COMMA, node, expr, tok) : expr;
}

// `sizeof` of a VLA type: a reference to its runtime size variable,
// computed first if this type has not had its size computed yet
// (e.g. `sizeof(int[n])` with a fresh type).
static Node *vla_size_expr(Type *ty, Token *tok) {
  if (ty->vla_size)
    return new_var_node(ty->vla_size, tok);
  Node *lhs = compute_vla_size(ty, tok);
  return new_binary(ND_COMMA, lhs, new_var_node(ty->vla_size, tok), tok);
}

// The resolved initializer tree: what a faithful initializer record
// (Initializer in chibicc.h) becomes once designators are evaluated,
// member names are bound, brace elision is applied and flexible
// arrays are sized. Built and consumed only within this file:
// create_lvar_init walks it to build assignments and write_gvar_data
// serializes it into .data bytes. Since initializers can be nested
// (e.g. `int x[2][2] = {{1, 2}, {3, 4}}`), it is a tree.
typedef struct ResolvedInit ResolvedInit;
struct ResolvedInit {
  Type *ty;
  bool is_flexible;

  // If it's not an aggregate type and has an initializer,
  // `expr` has an initialization expression.
  Node *expr;

  // If it's an initializer for an aggregate type (e.g. array or struct),
  // `children` has initializers for its children.
  ResolvedInit **children;

  // Only one member can be initialized for a union.
  // `mem` is used to clarify which member is initialized.
  Member *mem;
};

// Designator chain describing the position of an element within a
// local variable initializer (e.g. `x[1].y[2]`).
typedef struct InitDesg InitDesg;
struct InitDesg {
  InitDesg *next;
  int idx;
  Member *member;
  Obj *var;
};

static ResolvedInit *new_resolved_init(Type *ty, bool is_flexible) {
  ResolvedInit *init = calloc(1, sizeof(ResolvedInit));
  init->ty = ty;

  if (ty->kind == TY_ARRAY) {
    if (is_flexible && ty->size < 0) {
      init->is_flexible = true;
      return init;
    }

    init->children = calloc(ty->array_len, sizeof(ResolvedInit *));
    for (int i = 0; i < ty->array_len; i++)
      init->children[i] = new_resolved_init(ty->base, false);
    return init;
  }

  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
    // Count the number of struct members.
    int len = 0;
    for (Member *mem = ty->members; mem; mem = mem->next)
      len++;

    init->children = calloc(len, sizeof(ResolvedInit *));

    for (Member *mem = ty->members; mem; mem = mem->next) {
      if (is_flexible && ty->is_flexible && !mem->next) {
        ResolvedInit *child = calloc(1, sizeof(ResolvedInit));
        child->ty = mem->ty;
        child->is_flexible = true;
        init->children[mem->idx] = child;
      } else {
        init->children[mem->idx] = new_resolved_init(mem->ty, false);
      }
    }
    return init;
  }

  return init;
}

static Type *copy_struct_type(Type *ty) {
  ty = copy_type(ty);

  Member head = {};
  Member *cur = &head;
  for (Member *mem = ty->members; mem; mem = mem->next) {
    Member *m = calloc(1, sizeof(Member));
    *m = *mem;
    cur = cur->next = m;
  }

  ty->members = head.next;
  return ty;
}

// Evaluate a recorded designator index expression.
static int eval_desig_index(Node *expr) {
  return eval(expr);
}

// Evaluate an array designator and run its bounds checks against the
// array being designated. Anchors mirror the old parse-time checks:
// the begin bound reports at the token following the begin expression,
// the end bound and the empty range at `]`.
static void eval_array_desig(InitDesig *d, Type *ty, int *begin, int *end) {
  *begin = eval_desig_index(d->begin);
  if (*begin >= ty->array_len)
    error_tok(d->after_begin, "array designator index exceeds array bounds");

  if (d->end) {
    *end = eval_desig_index(d->end);
    if (*end >= ty->array_len)
      error_tok(d->rbracket, "array designator index exceeds array bounds");
    if (*end < *begin)
      error_tok(d->rbracket, "array designator range [%d, %d] is empty", *begin, *end);
  } else {
    *end = *begin;
  }
}

// Find the member a `.name` designator refers to. If the name lives
// inside an anonymous struct member, that (nameless) member is
// returned and the caller re-applies the designator to its subtree.
static Member *find_init_member(Type *ty, Token *name) {
  for (Member *mem = ty->members; mem; mem = mem->next) {
    // Anonymous struct member
    if (mem->ty->kind == TY_STRUCT && !mem->name) {
      if (get_struct_member(mem->ty, name))
        return mem;
      continue;
    }

    // A nameless member (e.g. an anonymous zero-width bitfield) takes
    // part in layout but can never be designated.
    if (!mem->name)
      continue;

    // Regular struct member
    if (mem->name->len == name->len && !strncmp(mem->name->loc, name->loc, name->len))
      return mem;
  }

  error_tok(name, "struct has no such member");
}

// Turn a recorded string literal into an expression node referencing
// its anonymous global, which is what a string initializer for a
// non-array target used to parse into.
static Node *materialize_str(Initializer *rec) {
  Node *node = new_node(ND_STRING, rec->str_tok);
  add_type(node);
  return node;
}

// Expand a string literal into the elements of a character array,
// one character (of the element width) per element.
static void resolve_string(Initializer *rec, ResolvedInit *init) {
  Token *tok = rec->str_tok;
  int len = MIN(init->ty->array_len, tok->ty->array_len);

  switch (init->ty->base->size) {
  case 1: {
    char *str = tok->str;
    for (int i = 0; i < len; i++)
      init->children[i]->expr = new_num(str[i], tok);
    break;
  }
  case 2: {
    uint16_t *str = (uint16_t *)tok->str;
    for (int i = 0; i < len; i++)
      init->children[i]->expr = new_num(str[i], tok);
    break;
  }
  case 4: {
    uint32_t *str = (uint32_t *)tok->str;
    for (int i = 0; i < len; i++)
      init->children[i]->expr = new_num(str[i], tok);
    break;
  }
  default:
    unreachable();
  }
}

static void resolve_value(Initializer *rec, ResolvedInit *init, InitItem **cursor);
static void resolve_desig(ResolvedInit *init, InitDesig *d, Initializer *rec, InitItem **cursor);
static void resolve_array1(Initializer *rec, ResolvedInit *init);
static void resolve_array_cont(ResolvedInit *init, int i, InitItem **cursor);
static void resolve_struct1(Initializer *rec, ResolvedInit *init);
static void resolve_struct_cont(ResolvedInit *init, Member *mem, InitItem **cursor);
static void resolve_struct_first(ResolvedInit *init, Member *mem, Initializer *rec, InitItem **cursor);
static void resolve_union(Initializer *rec, ResolvedInit *init, InitItem **cursor);
static int count_flex_list(InitItem *items, Type *ty);
static int count_flex(Initializer *rec, InitItem **cursor, Type *ty);

// Consume one braced-list item: apply its designator chain (if any)
// and resolve its value. *cursor advances past the item and any
// siblings the value consumes through brace elision.
static void resolve_item(InitItem **cursor, ResolvedInit *init) {
  InitItem *it = *cursor;
  *cursor = it->next;
  resolve_desig(init, it->desigs, it->init, cursor);
}

// Apply a designator chain to a resolved node, then its value. This
// is the record-driven form of the old designation(): `[n]` moves
// within an array, `.name` within a struct or union, and after a
// struct member the following siblings continue with the next members.
static void resolve_desig(ResolvedInit *init, InitDesig *d, Initializer *rec, InitItem **cursor) {
  if (!d) {
    resolve_value(rec, init, cursor);
    return;
  }

  if (!d->name) {
    if (init->ty->kind != TY_ARRAY)
      error_tok(d->tok, "array index in non-array initializer");

    int begin, end;
    eval_array_desig(d, init->ty, &begin, &end);

    for (int j = begin; j <= end; j++)
      resolve_desig(init->children[j], d->next, rec, cursor);
    resolve_array_cont(init, begin + 1, cursor);
    return;
  }

  if (init->ty->kind == TY_STRUCT) {
    Member *mem = find_init_member(init->ty, d->name);
    if (!mem->name) {
      // Anonymous struct member: re-apply the designator to its subtree.
      resolve_desig(init->children[mem->idx], d, rec, cursor);
      return;
    }
    resolve_desig(init->children[mem->idx], d->next, rec, cursor);
    init->expr = NULL;
    resolve_struct_cont(init, mem->next, cursor);
    return;
  }

  if (init->ty->kind == TY_UNION) {
    Member *mem = find_init_member(init->ty, d->name);
    init->mem = mem;
    if (!mem->name) {
      resolve_desig(init->children[mem->idx], d, rec, cursor);
      return;
    }
    resolve_desig(init->children[mem->idx], d->next, rec, cursor);
    return;
  }

  error_tok(d->tok, "field name not in struct or union initializer");
}

// Fill array elements from sibling items (brace-elision continuation
// or post-designator continuation), stopping at a designated item or
// when the items run out.
static void resolve_array_cont(ResolvedInit *init, int i, InitItem **cursor) {
  for (; i < init->ty->array_len && *cursor && !(*cursor)->desigs; i++)
    resolve_item(cursor, init->children[i]);
}

// Resolve a braced array initializer, designated items included.
// Elements past the end of the array are silently dropped, as the
// parser used to skip them.
static void resolve_array1(Initializer *rec, ResolvedInit *init) {
  int i = 0;
  InitItem *cursor = rec->items;

  while (cursor) {
    InitItem *it = cursor;

    if (it->desigs && !it->desigs->name) {
      int begin, end;
      eval_array_desig(it->desigs, init->ty, &begin, &end);
      cursor = it->next;
      for (int j = begin; j <= end; j++)
        resolve_desig(init->children[j], it->desigs->next, it->init, &cursor);
      i = end + 1;
      continue;
    }

    if (i < init->ty->array_len) {
      resolve_item(&cursor, init->children[i]);
    } else {
      cursor = it->next;
    }
    i++;
  }
}

// Fill struct members from sibling items, stopping at a designated
// item or when the items run out.
static void resolve_struct_cont(ResolvedInit *init, Member *mem, InitItem **cursor) {
  for (; mem && *cursor && !(*cursor)->desigs; mem = mem->next)
    resolve_item(cursor, init->children[mem->idx]);
}

// Brace-elision entry for structs: the expression a struct was
// "initialized" with becomes the first member's initializer and the
// siblings continue with the following members.
static void resolve_struct_first(ResolvedInit *init, Member *mem, Initializer *rec, InitItem **cursor) {
  if (mem) {
    resolve_value(rec, init->children[mem->idx], cursor);
    mem = mem->next;
  }
  resolve_struct_cont(init, mem, cursor);
}

// Resolve a braced struct initializer.
static void resolve_struct1(Initializer *rec, ResolvedInit *init) {
  Member *mem = init->ty->members;
  InitItem *cursor = rec->items;

  while (cursor) {
    InitItem *it = cursor;

    if (it->desigs && it->desigs->name) {
      Member *m = find_init_member(init->ty, it->desigs->name);
      cursor = it->next;
      if (m->name)
        resolve_desig(init->children[m->idx], it->desigs->next, it->init, &cursor);
      else
        resolve_desig(init->children[m->idx], it->desigs, it->init, &cursor);
      mem = m->next;
      continue;
    }

    if (mem) {
      resolve_item(&cursor, init->children[mem->idx]);
      mem = mem->next;
    } else {
      cursor = it->next;
    }
  }
}

// Resolve a union initializer. Unlike structs, union initializers
// take only one initializer, and that initializes the first union
// member by default. You can initialize another member using a
// designated initializer.
static void resolve_union(Initializer *rec, ResolvedInit *init, InitItem **cursor) {
  if (rec->kind == INIT_LIST) {
    InitItem *it = rec->items;

    if (it && it->desigs && it->desigs->name) {
      Member *mem = find_init_member(init->ty, it->desigs->name);
      init->mem = mem;
      InitItem *next = it->next;
      if (mem->name)
        resolve_desig(init->children[mem->idx], it->desigs->next, it->init, &next);
      else
        resolve_desig(init->children[mem->idx], it->desigs, it->init, &next);
      if (next)
        error_tok(next->comma_tok, "expected '}'");
      return;
    }

    init->mem = init->ty->members;
    if (!it) {
      // The old parser dispatched on the closing brace here; only an
      // array first member came out of `{}` without an error.
      if (init->ty->members->ty->kind != TY_ARRAY)
        error_tok(rec->tok->next, "expected an expression");
      return;
    }

    InitItem *next = it->next;
    resolve_desig(init->children[0], it->desigs, it->init, &next);
    if (next)
      error_tok(next->desigs ? next->desigs->tok : next->init->tok, "expected '}'");
    return;
  }

  init->mem = init->ty->members;
  resolve_value(rec, init->children[0], cursor);
}

// Count the elements a flexible-array initializer will have so the
// array type can be completed before the tree is built. Items are
// resolved against a throwaway tree, exactly like the old parser's
// counting pass; designator bounds are not checked here (the build
// pass checks them against the completed length).
static int count_flex_list(InitItem *items, Type *ty) {
  ResolvedInit *dummy = new_resolved_init(ty->base, true);
  int i = 0, max = 0;
  InitItem *cursor = items;

  while (cursor) {
    InitItem *it = cursor;
    if (it->desigs && !it->desigs->name) {
      i = eval_desig_index(it->desigs->begin);
      if (it->desigs->end)
        i = eval_desig_index(it->desigs->end);
      cursor = it->next;
      resolve_desig(dummy, it->desigs->next, it->init, &cursor);
    } else {
      resolve_item(&cursor, dummy);
    }
    i++;
    max = MAX(max, i);
  }
  return max;
}

// The brace-elision form of count_flex_list: the first value is the
// record itself and the siblings follow it.
static int count_flex(Initializer *rec, InitItem **cursor, Type *ty) {
  ResolvedInit *dummy = new_resolved_init(ty->base, true);
  int i = 0, max = 0;

  resolve_value(rec, dummy, cursor);
  i++;
  max = MAX(max, i);

  while (*cursor) {
    InitItem *it = *cursor;
    if (it->desigs && !it->desigs->name) {
      i = eval_desig_index(it->desigs->begin);
      if (it->desigs->end)
        i = eval_desig_index(it->desigs->end);
      *cursor = it->next;
      resolve_desig(dummy, it->desigs->next, it->init, cursor);
    } else {
      resolve_item(cursor, dummy);
    }
    i++;
    max = MAX(max, i);
  }
  return max;
}

// Resolve one initializer record against its target slot.
static void resolve_value(Initializer *rec, ResolvedInit *init, InitItem **cursor) {
  Type *ty = init->ty;

  if (ty->kind == TY_ARRAY && rec->kind == INIT_STR) {
    if (init->is_flexible)
      *init = *new_resolved_init(array_of(ty->base, rec->str_tok->ty->array_len), false);
    resolve_string(rec, init);
    return;
  }

  if (ty->kind == TY_ARRAY) {
    if (init->is_flexible) {
      // The counting pass walks the siblings without consuming them;
      // the build pass below resolves them for real.
      InitItem *save = *cursor;
      int len = (rec->kind == INIT_LIST) ? count_flex_list(rec->items, ty)
                                         : count_flex(rec, cursor, ty);
      *cursor = save;
      *init = *new_resolved_init(array_of(ty->base, len), false);
    }

    if (rec->kind == INIT_LIST) {
      resolve_array1(rec, init);
    } else {
      resolve_value(rec, init->children[0], cursor);
      resolve_array_cont(init, 1, cursor);
    }
    return;
  }

  if (ty->kind == TY_STRUCT) {
    if (rec->kind == INIT_LIST) {
      resolve_struct1(rec, init);
      return;
    }

    // A struct can be initialized with another struct. E.g.
    // `struct T x = y;` where y is a variable of type `struct T`.
    // Handle that case first.
    Node *expr = (rec->kind == INIT_STR) ? materialize_str(rec) : rec->expr;
    add_type(expr);
    if (expr->ty->kind == TY_STRUCT) {
      init->expr = expr;
      return;
    }

    resolve_struct_first(init, ty->members, rec, cursor);
    return;
  }

  if (ty->kind == TY_UNION) {
    resolve_union(rec, init, cursor);
    return;
  }

  if (rec->kind == INIT_LIST) {
    // An initializer for a scalar variable can be surrounded by
    // braces. E.g. `int x = {3};`. Handle that case: exactly one
    // element, no designators.
    InitItem *it = rec->items;
    if (!it)
      error_tok(rec->tok->next, "expected an expression");
    if (it->desigs)
      error_tok(it->desigs->tok, "expected an expression");
    InitItem *next = it->next;
    resolve_value(it->init, init, &next);
    if (next)
      error_tok(next->comma_tok, "expected '}'");
    return;
  }

  init->expr = (rec->kind == INIT_STR) ? materialize_str(rec) : rec->expr;
}

// Resolve a faithful initializer record against a declared type:
// evaluate designators, bind member names, apply brace elision, size
// flexible arrays and complete flexible struct/union members. *new_ty
// receives the completed type (it may differ from ty for flexible
// arrays and flexible members).
static ResolvedInit *resolve_initializer(Initializer *rec, Type *ty, Type **new_ty) {
  InitItem *cursor = NULL;
  ResolvedInit *init = new_resolved_init(ty, true);
  resolve_value(rec, init, &cursor);

  if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->is_flexible) {
    ty = copy_struct_type(ty);

    Member *mem = ty->members;
    while (mem->next)
      mem = mem->next;
    mem->ty = init->children[mem->idx]->ty;
    ty->size += mem->ty->size;

    *new_ty = ty;
    return init;
  }

  *new_ty = init->ty;
  return init;
}

static Node *init_desg_expr(InitDesg *desg, Token *tok) {
  if (desg->var)
    return new_var_node(desg->var, tok);

  if (desg->member) {
    Node *node = new_unary(ND_MEMBER, init_desg_expr(desg->next, tok), tok);
    node->member = desg->member;
    return node;
  }

  Node *lhs = init_desg_expr(desg->next, tok);
  Node *node = new_node(ND_SUBSCRIPT, tok);
  node->lhs = lhs;
  node->rhs = new_num(desg->idx, tok);
  return node;
}

static Node *create_lvar_init(ResolvedInit *init, Type *ty, InitDesg *desg, Token *tok) {
  if (ty->kind == TY_ARRAY) {
    Node *node = new_node(ND_NULL_EXPR, tok);
    for (int i = 0; i < ty->array_len; i++) {
      InitDesg desg2 = {desg, i};
      Node *rhs = create_lvar_init(init->children[i], ty->base, &desg2, tok);
      node = new_binary(ND_COMMA, node, rhs, tok);
    }
    return node;
  }

  if (ty->kind == TY_STRUCT && !init->expr) {
    Node *node = new_node(ND_NULL_EXPR, tok);

    for (Member *mem = ty->members; mem; mem = mem->next) {
      InitDesg desg2 = {desg, 0, mem};
      Node *rhs = create_lvar_init(init->children[mem->idx], mem->ty, &desg2, tok);
      node = new_binary(ND_COMMA, node, rhs, tok);
    }
    return node;
  }

  if (ty->kind == TY_UNION) {
    Member *mem = init->mem ? init->mem : ty->members;
    InitDesg desg2 = {desg, 0, mem};
    return create_lvar_init(init->children[mem->idx], mem->ty, &desg2, tok);
  }

  if (!init->expr)
    return new_node(ND_NULL_EXPR, tok);

  Node *lhs = init_desg_expr(desg, tok);
  return new_binary(ND_ASSIGN, lhs, init->expr, tok);
}

// Build the MEMZERO + assignment comma chain that initializes a local
// variable from its resolved initializer tree. `tok` anchors the
// synthesized nodes; the record's first token is where the parser used
// to anchor them.
static Node *lvar_init_comma(Obj *var, ResolvedInit *init, Token *tok) {
  InitDesg desg = {NULL, 0, NULL, var};

  // If a partial initializer list is given, the standard requires
  // that unspecified elements are set to 0. Here, we simply
  // zero-initialize the entire memory region of a variable before
  // initializing it with user-supplied values.
  Node *lhs = new_node(ND_MEMZERO, tok);
  lhs->var = var;

  Node *rhs = create_lvar_init(init, var->ty, &desg, tok);
  return new_binary(ND_COMMA, lhs, rhs, tok);
}

// A variable definition with an initializer is a shorthand notation
// for a variable definition followed by assignments. This function
// generates assignment expressions for an initializer. For example,
// `int x[2][2] = {{6, 7}, {8, 9}}` is converted to the following
// expressions:
//
//   x[0][0] = 6;
//   x[0][1] = 7;
//   x[1][0] = 8;
//   x[1][1] = 9;
//
// The parser hands the faithful initializer record to the declaration
// node; sema resolves it and this lowering rebuilds the chain from the
// result.

static uint64_t read_buf(char *buf, int sz) {
  if (sz == 1)
    return *buf;
  if (sz == 2)
    return *(uint16_t *)buf;
  if (sz == 4)
    return *(uint32_t *)buf;
  if (sz == 8)
    return *(uint64_t *)buf;
  unreachable();
}

static void write_buf(char *buf, uint64_t val, int sz) {
  if (sz == 1)
    *buf = val;
  else if (sz == 2)
    *(uint16_t *)buf = val;
  else if (sz == 4)
    *(uint32_t *)buf = val;
  else if (sz == 8)
    *(uint64_t *)buf = val;
  else
    unreachable();
}

static Relocation *
write_gvar_data(Relocation *cur, ResolvedInit *init, Type *ty, char *buf, int offset) {
  if (ty->kind == TY_ARRAY) {
    int sz = ty->base->size;
    for (int i = 0; i < ty->array_len; i++)
      cur = write_gvar_data(cur, init->children[i], ty->base, buf, offset + sz * i);
    return cur;
  }

  if (ty->kind == TY_STRUCT) {
    for (Member *mem = ty->members; mem; mem = mem->next) {
      if (mem->is_bitfield) {
        Node *expr = init->children[mem->idx]->expr;
        if (!expr)
          break;

        char *loc = buf + offset + mem->offset;
        uint64_t oldval = read_buf(loc, mem->ty->size);
        uint64_t newval = eval(expr);
        uint64_t mask = (1L << mem->bit_width) - 1;
        uint64_t combined = oldval | ((newval & mask) << mem->bit_offset);
        write_buf(loc, combined, mem->ty->size);
      } else {
        cur = write_gvar_data(cur, init->children[mem->idx], mem->ty, buf,
                              offset + mem->offset);
      }
    }
    return cur;
  }

  if (ty->kind == TY_UNION) {
    if (!init->mem)
      return cur;
    return write_gvar_data(cur, init->children[init->mem->idx],
                           init->mem->ty, buf, offset);
  }

  if (!init->expr)
    return cur;

  if (ty->kind == TY_FLOAT) {
    *(float *)(buf + offset) = eval_double(init->expr);
    return cur;
  }

  if (ty->kind == TY_DOUBLE) {
    *(double *)(buf + offset) = eval_double(init->expr);
    return cur;
  }

  char **label = NULL;
  uint64_t val = eval2(init->expr, &label);

  if (!label) {
    write_buf(buf + offset, val, ty->size);
    return cur;
  }

  Relocation *rel = calloc(1, sizeof(Relocation));
  rel->offset = offset;
  rel->label = label;
  rel->addend = val;
  cur->next = rel;
  return cur->next;
}

// Serialize a resolved initializer tree into the .data image of a
// global variable.
static void gvar_init_data(Obj *var, ResolvedInit *init) {
  Relocation head = {};
  char *buf = calloc(1, var->ty->size);
  write_gvar_data(&head, init, var->ty, buf, 0);
  var->init_data = buf;
  var->rel = head.next;
}

// Resolve a global variable's faithful initializer record and
// serialize it into the variable's .data image. Initializers for
// global variables are evaluated at compile-time; it is a compile
// error if an initializer list contains a non-constant expression.
static void serialize_gvar(Node *node) {
  if (!node->decl_init)
    return;

  gvar_init_data(node->var, node->init_resolved);
  node->decl_init = NULL;
  node->init_resolved = NULL;
}

Type *get_common_type(Type *ty1, Type *ty2) {
  if (ty1->base)
    return pointer_to(ty1->base);

  if (ty1->kind == TY_FUNC)
    return pointer_to(ty1);
  if (ty2->kind == TY_FUNC)
    return pointer_to(ty2);

  if (ty1->kind == TY_LDOUBLE || ty2->kind == TY_LDOUBLE)
    return ty_ldouble;
  if (ty1->kind == TY_DOUBLE || ty2->kind == TY_DOUBLE)
    return ty_double;
  if (ty1->kind == TY_FLOAT || ty2->kind == TY_FLOAT)
    return ty_float;

  if (ty1->size < 4)
    ty1 = ty_int;
  if (ty2->size < 4)
    ty2 = ty_int;

  if (ty1->size != ty2->size)
    return (ty1->size < ty2->size) ? ty2 : ty1;

  if (ty2->is_unsigned)
    return ty2;
  return ty1;
}

// For many binary operators, we implicitly promote operands so that
// both operands have the same type. Any integral type smaller than
// int is always promoted to int. If the type of one operand is larger
// than the other's (e.g. "long" vs. "int"), the smaller operand will
// be promoted to match with the other.
//
// This operation is called the "usual arithmetic conversion".
void usual_arith_conv(Node **lhs, Node **rhs) {
  Type *ty = get_common_type((*lhs)->ty, (*rhs)->ty);
  *lhs = new_cast(*lhs, ty);
  *rhs = new_cast(*rhs, ty);
}

// Find a struct member by name, descending into anonymous members.
static Member *get_struct_member(Type *ty, Token *tok) {
  for (Member *mem = ty->members; mem; mem = mem->next) {
    // Anonymous struct member
    if ((mem->ty->kind == TY_STRUCT || mem->ty->kind == TY_UNION) &&
        !mem->name) {
      if (get_struct_member(mem->ty, tok))
        return mem;
      continue;
    }

    // A nameless member (e.g. an anonymous zero-width bitfield) takes
    // part in layout but can never be named.
    if (!mem->name)
      continue;

    // Regular struct member
    if (mem->name->len == tok->len &&
        !strncmp(mem->name->loc, tok->loc, tok->len))
      return mem;
  }
  return NULL;
}

// Binds a member access the parser left unbound: the checks on the
// operand, the lookup of the name, and the dereference implied by `->`.
// An anonymous member does not name a field of the type the access is
// written against, so each one on the path becomes an ND_MEMBER node of
// its own between the operand and the named member.
//
// `node` is rewritten in place because its parent points at it.
static void resolve_member(Node *node) {
  Node *operand = node->lhs;
  Type *ty = operand->ty;
  Token *arrow = node->arrow_tok;

  // `x->y` is `(*x).y`; these are the checks the dereference would
  // make, anchored at the arrow.
  if (arrow) {
    if (ty->kind != TY_PTR || !ty->base)
      error_tok(arrow, "invalid pointer dereference");
    if (ty->base->kind == TY_VOID)
      error_tok(arrow, "dereferencing a void pointer");
    if (ty->base->kind != TY_STRUCT && ty->base->kind != TY_UNION)
      error_tok(arrow, "not a struct nor a union");
    ty = ty->base;
  } else if (ty->kind != TY_STRUCT && ty->kind != TY_UNION) {
    error_tok(operand->tok, "not a struct nor a union");
  }

  Node *cur = operand;
  for (;;) {
    Member *mem = get_struct_member(ty, node->tok);
    if (!mem)
      error_tok(node->tok, "no such member");

    Node *link = new_unary(ND_MEMBER, cur, node->tok);
    link->member = mem;
    if (arrow) {
      // The dereference belongs to the innermost link.
      link->lhs = new_unary(ND_DEREF, cur, node->tok);
      add_type(link->lhs);
      arrow = NULL;
    }
    link->ty = mem->ty;
    cur = link;

    if (mem->name)
      break;
    ty = mem->ty;
  }

  node->lhs = cur->lhs;
  node->member = cur->member;
  node->arrow_tok = NULL;
}

// Picks the association of an ND_GENERIC that the controlling expression
// selects, and makes the node that association's result expression. The
// comparison is written against the type the controlling expression decays
// to; a "default:" association (the one that carries no type name) is a
// fallback that a later match overrides, and among matching types the
// last one wins, exactly as the parser's loop used to decide.
//
// The result expression is deliberately left untyped: the caller types
// the node afresh. The other associations are typed for their checks
// only - they are expressions in the source, and a real compiler reports
// their errors too.
static void select_generic(Node *node) {
  add_type(node->cond);

  Type *ty = node->cond->ty;
  if (ty->kind == TY_FUNC)
    ty = pointer_to(ty);
  else if (ty->kind == TY_ARRAY)
    ty = pointer_to(ty->base);

  Node *sel = NULL;
  for (Node *assoc = node->args; assoc; assoc = assoc->next) {
    if (!assoc->ty_op) {
      if (!sel)
        sel = assoc->lhs;
    } else {
      resolve_type(assoc->ty_op);
      if (is_compatible(ty, assoc->ty_op))
        sel = assoc->lhs;
    }
  }

  if (!sel)
    error_tok(node->tok, "controlling expression type not compatible with"
              " any generic association type");

  for (Node *assoc = node->args; assoc; assoc = assoc->next)
    if (assoc->lhs != sel)
      add_type(assoc->lhs);

  // The conclusion of the selection, recorded on the node that asked the
  // question. Nothing reads it yet - the rewrite below still turns this
  // node *into* the selected expression, and that copy is what the
  // evaluator and codegen consume today. PLAN A9.1 drops the rewrite and
  // switches both consumers to this field, which is why the conclusion is
  // written now, while the two ways of carrying it still agree.
  //
  // `next` is the parent's link; the result's own link is not its.
  Node *nxt = node->next;
  *node = *sel;
  node->next = nxt;
  node->generic_sel = sel;
}

// Whether the declaration record being lowered produces no statement at
// all, so that the annotation pass removes it from the chain: a
// block-scope static (its data image goes to the global section) and a
// fixed-size object with no initializer.
static bool decl_remove;

// Walks a statement chain, typing each node. Declaration records that
// produce no code (typedefs, enum constants, extern declarations and
// [GNU] nested function definitions) are consumed here and removed
// from the chain, so that codegen sees exactly the shape it saw when
// the parser emitted the lowerings in place.
static void type_chain(Node **head) {
  for (Node **pp = head; *pp;) {
    Node *n = *pp;

    switch (n->kind) {
    case ND_TYPEDEF:
    case ND_ENUM_CONST:
      *pp = n->next;
      continue;
    case ND_GVAR_DECL:
      serialize_gvar(n);
      *pp = n->next;
      continue;
    case ND_FUNCDEF:
      // A [GNU] nested function definition: its body is analyzed at
      // this position of the enclosing body, as the parser used to,
      // and the record then leaves the chain.
      if (n->body)
        analyze_function(n->var);
      *pp = n->next;
      continue;
    default:
      break;
    }

    bool save_remove = decl_remove;
    decl_remove = false;
    add_type(n);
    bool remove = decl_remove;
    decl_remove = save_remove;

    if (remove) {
      *pp = n->next;
      continue;
    }
    pp = &(*pp)->next;
  }
}

// The annotation pass: types a node and every subtree it owns, inserts
// the implicit conversions the standard requires, runs the checks that
// need a typed tree, materializes the objects the language says exist,
// and records the compile-time conclusions on the nodes.
//
// Exported, and re-entrant on purpose: a consumer that builds a node
// calls it to have the node typed by sema's rules rather than by a copy
// of them, and sema itself calls it that way from the lowerings. The
// `node->ty` test makes it idempotent, so a subtree is annotated exactly
// once - which is also why a consumer must not run its own shaping over a
// subtree twice: this guard stops a second *annotation*, not a second
// *lowering* (PLAN contract 3(d)).
void add_type(Node *node) {
  if (!node || node->ty)
    return;

  // A generic selection is resolved before anything else, because the
  // node becomes the association it selects: the result has to be typed
  // as a fresh node, so that a subtree which registers itself by
  // identity while being typed - a `&&label` joining the gotos list -
  // registers the node that stays in the tree.
  if (node->kind == ND_GENERIC) {
    select_generic(node);
    add_type(node);
    return;
  }

  // A statement expression's value is its last statement, and only an
  // expression statement as written has one: a declaration record
  // lowers to an expression statement in place, so the shape is
  // captured here, before the body is lowered.
  Node *stmt_expr_value = NULL;
  if (node->kind == ND_STMT_EXPR) {
    stmt_expr_value = node->body;
    while (stmt_expr_value && stmt_expr_value->next)
      stmt_expr_value = stmt_expr_value->next;
    if (stmt_expr_value && stmt_expr_value->kind != ND_EXPR_STMT)
      stmt_expr_value = NULL;
  }

  add_type(node->lhs);
  add_type(node->rhs);
  add_type(node->cond);
  add_type(node->then);
  add_type(node->els);
  type_chain(&node->init);
  // codegen's `for` has a single-statement init slot, so an init
  // declaration that lowered to more than one statement is wrapped in a
  // block here. The parser leaves the records as a chain.
  if (node->init && node->init->next) {
    Node *blk = new_node(ND_BLOCK, node->init->tok);
    blk->body = node->init;
    node->init = blk;
  }
  add_type(node->inc);
  type_chain(&node->body);

  for (Node *n = node->args; n; n = n->next)
    add_type(n);

  switch (node->kind) {
  case ND_NUM:
    node->ty = ty_int;
    return;
  case ND_ADD:
  case ND_SUB: {
    // A raw `+`/`-` from the parser: new_add/new_sub apply the pointer
    // scaling, the `num + ptr` canonicalization and the usual arithmetic
    // conversions, and come back fully typed - a plain ADD/SUB, a
    // pre-typed SUB for `ptr - num`, or a DIV for `ptr - ptr`. The
    // result replaces this node.
    Node *result = node->kind == ND_ADD ? new_add(node->lhs, node->rhs, node->tok)
                                        : new_sub(node->lhs, node->rhs, node->tok);
    node->kind = result->kind;
    node->lhs = result->lhs;
    node->rhs = result->rhs;
    node->ty = result->ty;
    return;
  }
  case ND_MUL:
  case ND_DIV:
  case ND_MOD:
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
    usual_arith_conv(&node->lhs, &node->rhs);
    node->ty = node->lhs->ty;
    return;
  case ND_NEG: {
    Type *ty = get_common_type(ty_int, node->lhs->ty);
    node->lhs = new_cast(node->lhs, ty);
    node->ty = ty;
    return;
  }
  case ND_ASSIGN:
    if (node->op) {
      // A compound assignment as the parser recorded it: rewrite to
      // the read-modify-write form (handling the member and atomic
      // cases), then type the result afresh. The rewrite takes the
      // whole node, because the atomic form becomes a statement
      // expression, which carries its body outside lhs/rhs.
      Node *result = to_assign(node);
      Node *nxt = node->next;
      *node = *result;
      node->next = nxt;
      add_type(node);
      return;
    }
    if (node->lhs->ty->kind == TY_ARRAY)
      error_tok(node->lhs->tok, "not an lvalue");
    if (node->lhs->ty->kind != TY_STRUCT)
      node->rhs = new_cast(node->rhs, node->lhs->ty);
    node->ty = node->lhs->ty;
    return;
  case ND_INCDEC: {
    // Lower `++i`/`i--` to the compound-assignment form, rewriting
    // the node in place.
    Node *operand = node->lhs;
    Token *tok = node->tok;

    if (node->is_post) {
      // The result is a cast sema inserted - the one restoring the
      // operand's type around `(A += 1) - 1` - so its marking has to
      // travel with the shape it contributes.
      Node *result = new_inc_dec(operand, tok, node->addend);
      node->kind = result->kind;
      node->lhs = result->lhs;
      node->ty = result->ty;
      node->is_implicit = result->is_implicit;
      return;
    }

    Node *expr = new_binary(ND_ASSIGN, operand, new_num(1, tok), tok);
    expr->op = node->addend < 0 ? ND_SUB : ND_ADD;
    Node *result = to_assign(expr);
    Node *nxt = node->next;
    *node = *result;
    node->next = nxt;
    add_type(node);
    return;
  }
  case ND_GT:
  case ND_GE:
    // Downgrade the faithful `>` / `>=` back to `<` / `<=` with
    // swapped operands, which is the only comparison form codegen
    // understands.
    {
      Node *lhs = node->lhs;
      node->lhs = node->rhs;
      node->rhs = lhs;
      node->kind = node->kind == ND_GT ? ND_LT : ND_LE;
    }
    // fallthrough
  case ND_LT:
  case ND_LE:
    usual_arith_conv(&node->lhs, &node->rhs);
    node->ty = ty_int;
    return;
  case ND_EQ:
  case ND_NE:
    usual_arith_conv(&node->lhs, &node->rhs);
    node->ty = ty_int;
    return;
  case ND_FUNCALL:
    // Lower the faithful call node: the callee check, the argument
    // conversions and the return buffer.
    lower_funcall(node, node->tok);
    return;
  case ND_NOT:
  case ND_LOGOR:
  case ND_LOGAND:
    node->ty = ty_int;
    return;
  case ND_BITNOT:
  case ND_SHL:
  case ND_SHR:
    node->ty = node->lhs->ty;
    return;
  case ND_STRING: {
    // Lower a string literal to a reference to its anonymous global,
    // which is created here, where the literal is typed.
    Obj *var = new_string_literal(node->tok->str, node->tok->ty);
    node->kind = ND_VAR;
    node->var = var;
    node->ty = var->ty;
    return;
  }
  case ND_IDENT:
    // The resolve pass binds every name while the scope it was
    // written in is reconstructed; this is the safety net for nodes
    // the evaluator reaches directly.
    bind_ident(node);
    return;
  case ND_VAR:
  case ND_VLA_PTR:
    node->ty = node->var->ty;
    return;
  case ND_SIZEOF:
  case ND_ALIGNOF: {
    // Fold sizeof/_Alignof to their values. A fixed-length type folds
    // to a number and a VLA folds to a reference of its runtime size
    // variable; a `sizeof expr` operand is typed but never evaluated.
    Type *ty = node->ty_op;
    if (!ty) {
      add_type(node->lhs);
      ty = node->lhs->ty;
    } else {
      resolve_type(ty);
    }

    Node *folded;
    if (node->kind == ND_SIZEOF && ty->kind == TY_VLA)
      folded = vla_size_expr(ty, node->tok);
    else
      folded = new_ulong(node->kind == ND_SIZEOF ? ty->size : ty->align, node->tok);

    // The folded node replaces this one in the tree, so it has to
    // arrive fully typed: the annotation descent visits this position
    // exactly once and never comes back to the fresh nodes.
    add_type(folded);

    node->kind = folded->kind;
    node->lhs = folded->lhs;
    node->rhs = folded->rhs;
    node->var = folded->var;
    node->val = folded->val;
    node->ty = folded->ty;
    node->ty_op = NULL;
    return;
  }
  case ND_TYPES_COMPATIBLE:
    // Fold `__builtin_types_compatible_p(T1, T2)` to 0 or 1.
    resolve_type(node->ty_op);
    resolve_type(node->ty_op2);
    node->val = is_compatible(node->ty_op, node->ty_op2);
    node->ty_op = NULL;
    node->ty_op2 = NULL;
    node->kind = ND_NUM;
    add_type(node);
    return;
  case ND_REG_CLASS: {
    // Fold `__builtin_reg_class(T)` to its register class: integer or
    // pointer, floating-point, or anything else.
    resolve_type(node->ty_op);
    Type *ty = node->ty_op;
    int64_t val = 2;
    if (is_integer(ty) || ty->kind == TY_PTR)
      val = 0;
    else if (is_flonum(ty))
      val = 1;

    node->ty_op = NULL;
    node->val = val;
    node->kind = ND_NUM;
    add_type(node);
    return;
  }
  case ND_COND:
    if (node->is_elvis) {
      // Lower the GNU `a ?: b` to `tmp = a, tmp ? tmp : b`.
      // The node itself is rewritten to the comma expression.
      Obj *var = new_lvar("", node->cond->ty);
      Node *lhs = new_binary(ND_ASSIGN, new_var_node(var, node->tok), node->cond, node->tok);
      Node *rhs = new_node(ND_COND, node->tok);
      rhs->cond = new_var_node(var, node->tok);
      rhs->then = new_var_node(var, node->tok);
      rhs->els = node->els;
      node->kind = ND_COMMA;
      node->lhs = lhs;
      node->rhs = rhs;
      add_type(node);
      return;
    }
    if (node->then->ty->kind == TY_VOID || node->els->ty->kind == TY_VOID) {
      node->ty = ty_void;
    } else {
      usual_arith_conv(&node->then, &node->els);
      node->ty = node->then->ty;
    }
    return;
  case ND_COMMA:
    node->ty = node->rhs->ty;
    return;
  case ND_MEMBER:
    // Bind the member name the parser left unresolved. Nodes that sema
    // builds itself (compound assignment, initializer designators)
    // carry their member already.
    if (!node->member)
      resolve_member(node);
    node->ty = node->member->ty;
    return;
  case ND_ADDR: {
    if (node->lhs->kind == ND_MEMBER && node->lhs->member->is_bitfield)
      error_tok(node->tok, "cannot take address of bitfield");

    Type *ty = node->lhs->ty;
    if (ty->kind == TY_ARRAY)
      node->ty = pointer_to(ty->base);
    else
      node->ty = pointer_to(ty);
    return;
  }
  case ND_SUBSCRIPT: {
    // Downgrade `x[y]` to `*(x+y)` (with pointer scaling), the only
    // subscript form codegen understands. The node is rewritten in
    // place; new_add returns the scaled sum already typed.
    Node *add = new_add(node->lhs, node->rhs, node->tok);
    node->kind = ND_DEREF;
    node->lhs = add;
    add_type(node);
    return;
  }
  case ND_DEREF:
    if (node->lhs->ty->kind == TY_FUNC) {
      // [https://www.sigbus.info/n1570#6.5.3.2p4] Dereferencing a
      // function shouldn't do anything: `*foo` is just `foo`. The
      // parser cannot check this without typing the operand, so the
      // node survives until here and becomes its operand.
      Node *nxt = node->next;
      *node = *node->lhs;
      node->next = nxt;
      return;
    }
    if (!node->lhs->ty->base)
      error_tok(node->tok, "invalid pointer dereference");
    if (node->lhs->ty->base->kind == TY_VOID)
      error_tok(node->tok, "dereferencing a void pointer");

    node->ty = node->lhs->ty->base;
    return;
  case ND_STMT_EXPR:
    if (stmt_expr_value) {
      node->ty = stmt_expr_value->lhs->ty;
      return;
    }
    error_tok(node->tok, "statement expression returning void is not supported");
    return;
  case ND_LABEL_VAL:
    // [GNU] `&&lbl` is matched with the labels of the function by
    // analyze. It is collected here rather than there because such an
    // expression can live outside the statement tree - a block-scope
    // static initializer holds one, and only the evaluator reaches it.
    // The type annotation makes this run exactly once per node.
    node->goto_next = gotos;
    gotos = node;
    node->ty = pointer_to(ty_void);
    return;
  case ND_CAS:
    add_type(node->cas_addr);
    add_type(node->cas_old);
    add_type(node->cas_new);
    node->ty = ty_bool;

    if (node->cas_addr->ty->kind != TY_PTR)
      error_tok(node->cas_addr->tok, "pointer expected");
    if (node->cas_old->ty->kind != TY_PTR)
      error_tok(node->cas_old->tok, "pointer expected");
    return;
  case ND_EXCH:
    if (node->lhs->ty->kind != TY_PTR)
      error_tok(node->cas_addr->tok, "pointer expected");
    node->ty = node->lhs->ty->base;
    return;
  case ND_CAST:
    // An explicit cast from the parser: the target type is the record
    // in ty_op and the operand has been typed by the recursion above.
    // Casts sema builds itself arrive fully typed and never get here.
    resolve_type(node->ty_op);
    node->ty = node->ty_op;
    return;
  case ND_RETURN:
    // The implicit conversion to the return type. Struct and union
    // returns are passed through as the parser left them.
    if (node->lhs) {
      Type *ty = sema_fn->ty->return_ty;
      if (ty->kind != TY_STRUCT && ty->kind != TY_UNION)
        node->lhs = new_cast(node->lhs, ty);
    }
    return;
  case ND_DECL: {
    // Lower a declaration record to at most one statement. A block-scope
    // static was declared as an anonymous global by the resolve pass;
    // its initializer is serialized here and the record leaves the
    // chain, producing no statement. Otherwise the VLA sizes the type
    // holds are computed first and sequenced before the rest -
    // `x = alloca(<size>)` for a VLA, the MEMZERO + assignment comma
    // chain for an initialized object - and a fixed-size object with no
    // initializer produces no statement at all.
    Obj *var = node->var;
    Token *tok = node->tok;

    if (node->attr.is_static) {
      if (node->decl_init) {
        gvar_init_data(var, node->init_resolved);
        node->decl_init = NULL;
        node->init_resolved = NULL;
      }
      decl_remove = true;
      return;
    }

    // Every declared type is walked, not just a VLA one, because a
    // pointer to a VLA (`int (*p)[n]`) needs the size computed here too:
    // later arithmetic on it reads the size variable instead of
    // computing it again.
    Node *vla_size = compute_vla_size(var->ty, tok);
    Node *lowered = NULL;

    if (var->ty->kind == TY_VLA) {
      // A variable-length object may not be initialized. The parser left
      // the two standing side by side because whether a declaration is a
      // VLA one depends on the dimension being a constant expression;
      // the `=` is where that used to be reported.
      if (node->decl_init)
        error_tok(node->decl_init->eq_tok,
                  "variable-sized object may not be initialized");

      lowered = new_binary(ND_ASSIGN, new_vla_ptr(var, tok),
                           new_alloca(new_var_node(var->ty->vla_size, tok)),
                           tok);
    } else {
      // A declared object must have a complete, non-void type.
      check_declared_void(tok, var->ty);
      if (var->ty->size < 0)
        error_tok(node->name_tok, "variable has incomplete type");

      // The resolve pass resolved the faithful initializer record and
      // completed the declared type with it; what is left here is the
      // lowering to the assignment chain.
      if (node->init_resolved)
        lowered = lvar_init_comma(var, node->init_resolved, node->decl_init->tok);
      node->decl_init = NULL;
    }

    if (!vla_size && !lowered) {
      decl_remove = true;
      return;
    }

    node->kind = ND_EXPR_STMT;
    node->lhs = (vla_size && lowered)
                  ? new_binary(ND_COMMA, vla_size, lowered, tok)
                  : (lowered ? lowered : vla_size);
    add_type(node);
    return;
  }
  case ND_COMPOUND_LITERAL: {
    // Materialize the compound literal. The resolve pass created the
    // hidden variable it owns - a hidden local in block scope - and
    // resolved its initializer record against the variable's type.
    // The node lowers to `initializer-comma, var`. At file scope the
    // variable is an anonymous global whose data is serialized here,
    // and the node lowers to a reference of it.
    Obj *var = node->var;
    Token *tok = node->tok;

    ResolvedInit *init = node->init_resolved;

    if (var->is_local) {
      node->kind = ND_COMMA;
      node->lhs = lvar_init_comma(var, init, node->decl_init->tok);
      node->rhs = new_var_node(var, tok);
    } else {
      gvar_init_data(var, init);
      node->kind = ND_VAR;
    }
    node->decl_init = NULL;
    node->init_resolved = NULL;
    add_type(node);
    return;
  }
  }
}

static int64_t eval(Node *node) {
  return eval2(node, NULL);
}

// Evaluate a given node as a constant expression.
//
// A constant expression is either just a number or ptr+n where ptr
// is a pointer to a global variable and n is a postiive/negative
// number. The latter form is accepted only as an initialization
// expression for a global variable.
//
// The add_type at the entry is what lets the switch below see
// lowered shapes only - GT already swapped to LT, SUBSCRIPT already
// DEREF, STRING already a VAR, sizeof already a number, arr+2
// already ADD(VAR, MUL(2, 4)). The preprocessor reaches this through
// const_expr, so `#if` rides the same path. When the lowerings leave
// the annotation pass (A1.1 makes the evaluator read the faithful
// shapes itself), this entry call goes away with them.
static int64_t eval2(Node *node, char ***label) {
  add_type(node);

  if (is_flonum(node->ty))
    return eval_double(node);

  switch (node->kind) {
  case ND_ADD:
    return eval2(node->lhs, label) + eval(node->rhs);
  case ND_SUB:
    return eval2(node->lhs, label) - eval(node->rhs);
  case ND_MUL:
    return eval(node->lhs) * eval(node->rhs);
  case ND_DIV:
    if (node->ty->is_unsigned)
      return (uint64_t)eval(node->lhs) / eval(node->rhs);
    return eval(node->lhs) / eval(node->rhs);
  case ND_NEG:
    return -eval(node->lhs);
  case ND_MOD:
    if (node->ty->is_unsigned)
      return (uint64_t)eval(node->lhs) % eval(node->rhs);
    return eval(node->lhs) % eval(node->rhs);
  case ND_BITAND:
    return eval(node->lhs) & eval(node->rhs);
  case ND_BITOR:
    return eval(node->lhs) | eval(node->rhs);
  case ND_BITXOR:
    return eval(node->lhs) ^ eval(node->rhs);
  case ND_SHL:
    return eval(node->lhs) << eval(node->rhs);
  case ND_SHR:
    if (node->ty->is_unsigned && node->ty->size == 8)
      return (uint64_t)eval(node->lhs) >> eval(node->rhs);
    return eval(node->lhs) >> eval(node->rhs);
  case ND_EQ:
    return eval(node->lhs) == eval(node->rhs);
  case ND_NE:
    return eval(node->lhs) != eval(node->rhs);
  case ND_LT:
    if (node->lhs->ty->is_unsigned)
      return (uint64_t)eval(node->lhs) < eval(node->rhs);
    return eval(node->lhs) < eval(node->rhs);
  case ND_LE:
    if (node->lhs->ty->is_unsigned)
      return (uint64_t)eval(node->lhs) <= eval(node->rhs);
    return eval(node->lhs) <= eval(node->rhs);
  case ND_COND:
    return eval(node->cond) ? eval2(node->then, label) : eval2(node->els, label);
  case ND_COMMA:
    return eval2(node->rhs, label);
  case ND_NOT:
    return !eval(node->lhs);
  case ND_BITNOT:
    return ~eval(node->lhs);
  case ND_LOGAND:
    return eval(node->lhs) && eval(node->rhs);
  case ND_LOGOR:
    return eval(node->lhs) || eval(node->rhs);
  case ND_CAST: {
    int64_t val = eval2(node->lhs, label);
    if (is_integer(node->ty)) {
      switch (node->ty->size) {
      case 1: return node->ty->is_unsigned ? (uint8_t)val : (int8_t)val;
      case 2: return node->ty->is_unsigned ? (uint16_t)val : (int16_t)val;
      case 4: return node->ty->is_unsigned ? (uint32_t)val : (int32_t)val;
      }
    }
    return val;
  }
  case ND_ADDR:
    return eval_rval(node->lhs, label);
  case ND_LABEL_VAL:
    *label = &node->unique_label;
    return 0;
  case ND_MEMBER:
    if (!label)
      error_tok(node->tok, "not a compile-time constant");
    if (node->ty->kind != TY_ARRAY)
      error_tok(node->tok, "invalid initializer");
    return eval_rval(node->lhs, label) + node->member->offset;
  case ND_VAR:
    if (!label)
      error_tok(node->tok, "not a compile-time constant");
    if (node->var->ty->kind != TY_ARRAY && node->var->ty->kind != TY_FUNC)
      error_tok(node->tok, "invalid initializer");
    *label = &node->var->name;
    return 0;
  case ND_NUM:
    return node->val;
  }

  error_tok(node->tok, "not a compile-time constant");
}

static int64_t eval_rval(Node *node, char ***label) {
  switch (node->kind) {
  case ND_VAR:
    if (node->var->is_local)
      error_tok(node->tok, "not a compile-time constant");
    *label = &node->var->name;
    return 0;
  case ND_DEREF:
    return eval2(node->lhs, label);
  case ND_MEMBER:
    return eval_rval(node->lhs, label) + node->member->offset;
  }

  error_tok(node->tok, "invalid initializer");
}

// Annotates first, like eval2, so it walks lowered shapes; see there.
// Its one real caller is resolve_type, classifying an array dimension
// as fixed or variable. The entry call goes away with the lowerings
// (A1.1), at which point the sizeof-of-a-VLA operand has to answer
// false by its own kind, not because the fold hid it.
static bool is_const_expr(Node *node) {
  add_type(node);

  switch (node->kind) {
  case ND_ADD:
  case ND_SUB:
  case ND_MUL:
  case ND_DIV:
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
  case ND_SHL:
  case ND_SHR:
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
  case ND_LOGAND:
  case ND_LOGOR:
    return is_const_expr(node->lhs) && is_const_expr(node->rhs);
  case ND_COND:
    if (!is_const_expr(node->cond))
      return false;
    return is_const_expr(eval(node->cond) ? node->then : node->els);
  case ND_COMMA:
    return is_const_expr(node->rhs);
  case ND_NEG:
  case ND_NOT:
  case ND_BITNOT:
  case ND_CAST:
    return is_const_expr(node->lhs);
  case ND_NUM:
    return true;
  }

  return false;
}

int64_t const_expr(Token **rest, Token *tok) {
  Node *node = conditional(rest, tok);
  return eval(node);
}

// Annotates first, like eval2 (see there): the shapes this switch
// sees are the lowered ones, and the entry call goes away with the
// lowerings (A1.1).
static double eval_double(Node *node) {
  add_type(node);

  if (is_integer(node->ty)) {
    if (node->ty->is_unsigned)
      return (unsigned long)eval(node);
    return eval(node);
  }

  switch (node->kind) {
  case ND_ADD:
    return eval_double(node->lhs) + eval_double(node->rhs);
  case ND_SUB:
    return eval_double(node->lhs) - eval_double(node->rhs);
  case ND_MUL:
    return eval_double(node->lhs) * eval_double(node->rhs);
  case ND_DIV:
    return eval_double(node->lhs) / eval_double(node->rhs);
  case ND_NEG:
    return -eval_double(node->lhs);
  case ND_COND:
    return eval_double(node->cond) ? eval_double(node->then) : eval_double(node->els);
  case ND_COMMA:
    return eval_double(node->rhs);
  case ND_CAST:
    if (is_flonum(node->lhs->ty))
      return eval_double(node->lhs);
    return eval(node->lhs);
  case ND_NUM:
    return node->fval;
  }

  error_tok(node->tok, "not a compile-time constant");
}

//
// The resolve pass
//
// The scope stack is derived from the tree structure: a compound-stmt
// block pushes and pops a scope, a statement expression does the same
// for its body, and a `for` statement's init declarations share one
// scope with its condition, increment and body - exactly the scopes
// the parser used to drive while parsing.

// True while the resolve pass is inside a function body. A compound
// literal owns a hidden local there and an anonymous global at file
// scope, and this is what says which: the storage class follows from
// where the resolve pass is, not from the depth of a scope stack.
static bool resolving_body;

// Walks a faithful initializer record and resolves the expressions it
// carries (values and designator bounds); resolution and lowering of
// the record happen later, against the declared type.
static void resolve_init_record(Initializer *rec) {
  if (!rec)
    return;

  if (rec->kind == INIT_EXPR)
    resolve_node(rec->expr);

  for (InitItem *it = rec->items; it; it = it->next) {
    for (InitDesig *d = it->desigs; d; d = d->next) {
      resolve_node(d->begin);
      resolve_node(d->end);
    }
    resolve_init_record(it->init);
  }
}

static void resolve_children(Node *node) {
  resolve_node(node->lhs);
  resolve_node(node->rhs);
  resolve_node(node->cond);
  resolve_node(node->then);
  resolve_node(node->els);
  resolve_chain(node->init);
  resolve_node(node->inc);
  resolve_chain(node->body);
  resolve_chain(node->args);
  resolve_node(node->cas_addr);
  resolve_node(node->cas_old);
  resolve_node(node->cas_new);
}

static void resolve_node(Node *node) {
  if (!node)
    return;

  // Enum constants an expression-context type defined become visible
  // at this node's position.
  if (node->spec_decls) {
    resolve_enum_records(node->spec_decls);
    node->spec_decls = NULL;
  }

  switch (node->kind) {
  case ND_TYPEDEF:
    resolve_type_exprs(node->ty);
    resolve_type(node->ty);
    return;
  case ND_ENUM_CONST:
    if (node->ty_op)
      resolve_enum_records(node);
    return;
  case ND_BLOCK:
    if (node->is_scope_block)
      enter_scope();
    resolve_chain(node->body);
    if (node->is_scope_block)
      leave_scope();
    return;
  case ND_STMT_EXPR:
    enter_scope();
    resolve_chain(node->body);
    leave_scope();
    return;
  case ND_FOR:
    // The init declarations share their scope with the rest of the
    // statement, and the records the init chain may start with are
    // registered inside it.
    enter_scope();
    resolve_chain(node->init);
    resolve_node(node->cond);
    resolve_node(node->inc);
    resolve_node(node->then);
    leave_scope();
    return;
  case ND_DECL: {
    resolve_type_exprs(node->ty_op);
    resolve_type(node->ty_op);

    if (node->attr.is_static) {
      // A block-scope static variable lives in the global data section
      // under an anonymous name, but its name is registered like any
      // other local. The void check is anchored where the old
      // declaration site reported it: at the `=` if there is an
      // initializer, at the declaration position otherwise.
      Token *anchor = node->decl_init ? node->decl_init->eq_tok : node->tok;
      check_declared_void(anchor, node->ty_op);
      node->var = new_anon_gvar(node->ty_op);
      push_scope(get_ident(node->name_tok))->var = node->var;
    } else {
      node->var = new_lvar(get_ident(node->name_tok), node->ty_op);
      int align = attr_align(&node->attr);
      if (align)
        node->var->align = align;
    }

    // The declared name is visible to its own initializer, as it was
    // when the parser created the variable before parsing the `=`.
    resolve_init_record(node->decl_init);

    // The initializer is resolved here, at the declaration's position:
    // resolution completes the declared type (an array sized by its
    // initializer, a flexible member), and later references in the
    // resolve order have to see the completed type.
    if (node->decl_init) {
      Type *new_ty;
      node->init_resolved =
        resolve_initializer(node->decl_init, node->var->ty, &new_ty);
      node->var->ty = new_ty;
    }
    return;
  }
  case ND_GVAR_DECL: {
    resolve_type_exprs(node->ty_op);
    resolve_type(node->ty_op);

    Obj *var = new_gvar(get_ident(node->name_tok), node->ty_op);
    var->is_definition = !node->attr.is_extern;
    var->is_static = node->attr.is_static;
    var->is_tls = node->attr.is_tls;
    int align = attr_align(&node->attr);
    if (align)
      var->align = align;
    if (!node->decl_init && !node->attr.is_extern && !node->attr.is_tls)
      var->is_tentative = true;
    node->var = var;

    resolve_init_record(node->decl_init);
    if (node->decl_init) {
      Type *new_ty;
      node->init_resolved = resolve_initializer(node->decl_init, var->ty, &new_ty);
      var->ty = new_ty;
    }
    return;
  }
  case ND_FUNCDEF:
    resolve_function(node);
    return;
  case ND_COMPOUND_LITERAL:
    // The hidden variable is created here, where the resolve context
    // says whether the literal lives in an anonymous global or on the
    // stack, and before the initializer record is walked.
    resolve_type_exprs(node->ty_op);
    resolve_type(node->ty_op);
    node->var = resolving_body ? new_lvar("", node->ty_op)
                               : new_anon_gvar(node->ty_op);
    resolve_init_record(node->decl_init);
    if (node->decl_init) {
      Type *new_ty;
      node->init_resolved =
        resolve_initializer(node->decl_init, node->var->ty, &new_ty);
      node->var->ty = new_ty;
    }
    return;
  case ND_STRING: {
    // The literal becomes a reference to its anonymous global here, at
    // its source position, so the globals keep the declaration order
    // the single-pass parser produced.
    Obj *var = new_string_literal(node->tok->str, node->tok->ty);
    node->kind = ND_VAR;
    node->var = var;
    node->ty = var->ty;
    return;
  }
  case ND_CASE:
    // The value(s) a case label stands for: the parser recorded the
    // operands unevaluated, because whether an expression is a
    // constant is not a syntax question. They are truncated to `int`,
    // as they were when the parser wrote `begin`/`end` on its way past
    // them. `:` is where the range check is anchored.
    resolve_node(node->begin_expr);
    resolve_node(node->end_expr);
    if (node->begin_expr) {
      int begin = (int) eval(node->begin_expr);
      int end = begin;

      if (node->end_expr) {
        end = (int) eval(node->end_expr);
        if (end < begin)
          error_tok(node->colon_tok, "empty case range specified");
      }

      node->begin = begin;
      node->end = end;
      node->begin_expr = NULL;
      node->end_expr = NULL;
    }
    resolve_node(node->lhs);
    return;
  case ND_IDENT:
    // Bind the name (variable, function or enum constant), rewriting
    // the node into the shape codegen understands.
    bind_ident(node);
    return;
  case ND_CAST:
    resolve_type_exprs(node->ty_op);
    resolve_type(node->ty_op);
    break;
  case ND_SIZEOF:
  case ND_ALIGNOF:
  case ND_REG_CLASS:
  case ND_GENERIC_ASSOC:
    if (node->ty_op) {
      resolve_type_exprs(node->ty_op);
      resolve_type(node->ty_op);
    }
    break;
  case ND_TYPES_COMPATIBLE:
    resolve_type_exprs(node->ty_op);
    resolve_type(node->ty_op);
    resolve_type_exprs(node->ty_op2);
    resolve_type(node->ty_op2);
    break;
  default:
    break;
  }

  resolve_children(node);
}

static void resolve_chain(Node *node) {
  for (; node; node = node->next)
    resolve_node(node);
}

// Declares the function a record names (checking it against a previous
// declaration), then resolves its body: the parameters and helpers
// first, then the statements, with the scope stack the block structure
// gives. The variable list is captured before the annotation pass adds
// its temporaries; analyze_function splices those in afterwards.
static void resolve_function(Node *node) {
  Type *ty = node->ty_op;
  resolve_type_exprs(ty);
  resolve_type(ty);

  Obj *fn = declare_function(get_ident(node->name_tok), ty, &node->attr,
                             node->tok, node->body != NULL);
  node->var = fn;

  if (!node->body)
    return;

  fn->body = node->body;
  sema_fn = fn;

  // A [GNU] nested function definition reaches this from inside the
  // enclosing body, so the flag is saved rather than cleared.
  bool save_body = resolving_body;
  resolving_body = true;

  enter_scope();
  begin_function(fn, ty);
  resolve_node(node->body);
  fn->locals = locals;
  leave_scope();

  resolving_body = save_body;
}

// Runs the annotation + lowering pass and the control-flow descent over
// a resolved function body. The temporaries the lowering creates are
// collected on a fresh variable list and spliced in front of the ones
// the resolve pass captured, so that the function's frame is complete
// and no other function's list is polluted.
static void splice_locals(Obj *fn) {
  Obj *pass2 = locals;
  if (pass2) {
    Obj *tail = pass2;
    while (tail->next)
      tail = tail->next;
    tail->next = fn->locals;
    fn->locals = pass2;
  }
}

static void analyze_function(Obj *fn) {
  Obj *save = locals;
  locals = NULL;
  add_type(fn->body);
  analyze(fn->body);
  splice_locals(fn);
  locals = save;
}

// Runs semantic analysis over the parser's top-level declaration-record
// chain, in source order, and returns the list of global objects for
// codegen. A function record is resolved first, so that later records
// see its name, and its body is annotated and descended right after -
// the order the parser used to interleave declaration and analysis.
Obj *sema(Node *toplevel) {
  declare_builtin_functions();
  // `alloca` is only a declaration for the VLA lowering to call; the
  // list codegen walks starts after it.
  globals = NULL;

  for (Node *n = toplevel; n; n = n->next) {
    switch (n->kind) {
    case ND_TYPEDEF:
    case ND_ENUM_CONST:
      resolve_node(n);
      break;
    case ND_GVAR_DECL:
      resolve_node(n);
      serialize_gvar(n);
      break;
    case ND_FUNCDEF:
      resolve_function(n);
      if (n->body)
        analyze_function(n->var);
      break;
    default:
      unreachable();
    }
  }

  // Mark the reachable functions live and drop the redundant tentative
  // definitions.
  finalize_globals();
  return globals;
}
