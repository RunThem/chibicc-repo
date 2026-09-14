// This file hosts semantic analysis passes that operate on the AST
// built by parse.c.
//
// Currently it contains the type annotator (add_type and its helpers)
// and the constant expression evaluator (eval and friends), moved
// verbatim out of parse.c and type.c as the first step of splitting
// semantic analysis out of the parser.

#include "chibicc.h"

static int64_t eval_rval(Node *node, char ***label);
double eval_double(Node *node);

// All local variable instances created during parsing are
// accumulated to this list.
static Obj *locals;

// Likewise, global variables are accumulated to this list.
static Obj *globals;

Obj *get_locals(void) {
  return locals;
}

void set_locals(Obj *vars) {
  locals = vars;
}

Obj *get_globals(void) {
  return globals;
}

void set_globals(Obj *vars) {
  globals = vars;
}

// Typedef declarations, in source order. The parser records them here
// as it parses; sema will rebuild scopes from these records instead of
// the parser's scope table.
static Node *typedefs;
static Node *typedefs_tail;

void add_typedef(Node *node) {
  if (typedefs)
    typedefs_tail = typedefs_tail->next = node;
  else
    typedefs = typedefs_tail = node;
}

Node *get_typedefs(void) {
  return typedefs;
}

static Obj *new_var(char *name, Type *ty) {
  Obj *var = calloc(1, sizeof(Obj));
  var->name = name;
  var->ty = ty;
  var->align = ty->align;
  push_var_scope(name, var);
  return var;
}

Obj *new_lvar(char *name, Type *ty) {
  Obj *var = new_var(name, ty);
  var->is_local = true;
  var->next = locals;
  locals = var;
  return var;
}

Obj *new_gvar(char *name, Type *ty) {
  Obj *var = new_var(name, ty);
  var->next = globals;
  var->is_static = true;
  var->is_definition = true;
  globals = var;
  return var;
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
    return new_binary(ND_ADD, lhs, rhs, tok);

  if (lhs->ty->base && rhs->ty->base)
    error_tok(tok, "invalid operands");

  // Canonicalize `num + ptr` to `ptr + num`.
  if (!lhs->ty->base && rhs->ty->base) {
    Node *tmp = lhs;
    lhs = rhs;
    rhs = tmp;
  }

  // VLA + num
  if (lhs->ty->base->kind == TY_VLA) {
    rhs = new_binary(ND_MUL, rhs, new_var_node(lhs->ty->base->vla_size, tok), tok);
    return new_binary(ND_ADD, lhs, rhs, tok);
  }

  // ptr + num
  rhs = new_binary(ND_MUL, rhs, new_long(lhs->ty->base->size, tok), tok);
  return new_binary(ND_ADD, lhs, rhs, tok);
}

// Like `+`, `-` is overloaded for the pointer type.
static Node *new_sub(Node *lhs, Node *rhs, Token *tok) {
  add_type(lhs);
  add_type(rhs);

  // num - num
  if (is_numeric(lhs->ty) && is_numeric(rhs->ty))
    return new_binary(ND_SUB, lhs, rhs, tok);

  // VLA + num
  if (lhs->ty->base->kind == TY_VLA) {
    rhs = new_binary(ND_MUL, rhs, new_var_node(lhs->ty->base->vla_size, tok), tok);
    add_type(rhs);
    Node *node = new_binary(ND_SUB, lhs, rhs, tok);
    node->ty = lhs->ty;
    return node;
  }

  // ptr - num
  if (lhs->ty->base && is_integer(rhs->ty)) {
    rhs = new_binary(ND_MUL, rhs, new_long(lhs->ty->base->size, tok), tok);
    add_type(rhs);
    Node *node = new_binary(ND_SUB, lhs, rhs, tok);
    node->ty = lhs->ty;
    return node;
  }

  // ptr - ptr, which returns how many elements are between the two.
  if (lhs->ty->base && rhs->ty->base) {
    Node *node = new_binary(ND_SUB, lhs, rhs, tok);
    node->ty = ty_long;
    return new_binary(ND_DIV, node, new_num(lhs->ty->base->size, tok), tok);
  }

  error_tok(tok, "invalid operands");
}

// Build the `lhs op rhs` operand expression for a compound assignment
// driven by node->op. The raw expression is emitted here; add_type
// applies the pointer scaling and the usual arithmetic conversions.
static Node *compound_op(Node *node, Node *lhs, Token *tok) {
  // `+=`/`-=` are routed through new_add/new_sub so that pointer
  // arithmetic is scaled exactly as for plain `+`/`-`; the rebuilt
  // node carries a non-zero `op` marking it as already scaled.
  Node *expr;
  if (node->op == ND_ADD || node->op == ND_SUB) {
    Node *scaled = node->op == ND_ADD ? new_add(node->lhs, node->rhs, tok)
                                      : new_sub(node->lhs, node->rhs, tok);
    add_type(scaled->rhs);
    expr = new_binary(node->op, lhs, scaled->rhs, tok);
  } else {
    expr = new_binary(node->op, lhs, node->rhs, tok);
  }
  expr->op = node->op;
  return expr;
}

// Convert op= operators to expressions containing an assignment.
//
// `node` is an ND_ASSIGN whose `op` holds the compound assignment
// operator. In general, `A op= C` is converted to
// ``tmp = &A, *tmp = *tmp op C`.
// However, if a given expression is of form `A.x op= C`, the input is
// converted to `tmp = &A, (*tmp).x = (*tmp).x op C` to handle assignments
// to bitfields.
Node *to_assign(Node *node) {
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
    // The `val` temporary has the type of the (scaled) operand, so
    // the scaling is computed eagerly here.
    Node *operand;
    if (node->op == ND_ADD || node->op == ND_SUB)
      operand = node->op == ND_ADD ? new_add(node->lhs, node->rhs, tok)
                                   : new_sub(node->lhs, node->rhs, tok);
    else
      operand = new_binary(node->op, node->lhs, node->rhs, tok);
    add_type(operand->rhs);

    Node head = {};
    Node *cur = &head;

    Obj *addr = new_lvar("", pointer_to(node->lhs->ty));
    Obj *val = new_lvar("", operand->rhs->ty);
    Obj *old = new_lvar("", node->lhs->ty);
    Obj *new = new_lvar("", node->lhs->ty);

    cur = cur->next =
      new_unary(ND_EXPR_STMT,
                new_binary(ND_ASSIGN, new_var_node(addr, tok),
                           new_unary(ND_ADDR, node->lhs, tok), tok),
                tok);

    cur = cur->next =
      new_unary(ND_EXPR_STMT,
                new_binary(ND_ASSIGN, new_var_node(val, tok), operand->rhs, tok),
                tok);

    cur = cur->next =
      new_unary(ND_EXPR_STMT,
                new_binary(ND_ASSIGN, new_var_node(old, tok),
                           new_unary(ND_DEREF, new_var_node(addr, tok), tok), tok),
                tok);

    Node *loop = new_node(ND_DO, tok);
    loop->brk_label = new_unique_name();
    loop->cont_label = new_unique_name();

    Node *body = new_binary(ND_ASSIGN,
                            new_var_node(new, tok),
                            new_binary(node->op, new_var_node(old, tok),
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
  sub->op = ND_ADD; // already scaled; keep add_type from scaling again
  return new_cast(sub, node->ty);
}

// Generate code for computing a VLA size. Moved from parse.c; the
// parser still calls it for every declarator it declares.
Node *compute_vla_size(Type *ty, Token *tok) {
  Node *node = new_node(ND_NULL_EXPR, tok);
  if (ty->base)
    node = new_binary(ND_COMMA, node, compute_vla_size(ty->base, tok), tok);

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
  return new_binary(ND_COMMA, node, expr, tok);
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

static Node *create_lvar_init(Initializer *init, Type *ty, InitDesg *desg, Token *tok) {
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
// variable from its parsed initializer tree. `tok` anchors the
// synthesized nodes; the parser anchors them at the first token of
// the initializer source (Initializer::tok of an ND_DECL's tree).
static Node *lvar_init_comma(Obj *var, Initializer *init, Token *tok) {
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
// The parser hands the parsed initializer tree to ND_DECL and this
// lowering rebuilds the chain from it.

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
write_gvar_data(Relocation *cur, Initializer *init, Type *ty, char *buf, int offset) {
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

// Serialize a parsed initializer tree into the .data image of a global
// variable. Split from gvar_initializer so that sema-internal
// lowerings (e.g. compound literals) can reuse it.
static void gvar_init_data(Obj *var, Initializer *init) {
  Relocation head = {};
  char *buf = calloc(1, var->ty->size);
  write_gvar_data(&head, init, var->ty, buf, 0);
  var->init_data = buf;
  var->rel = head.next;
}

// Initializers for global variables are evaluated at compile-time and
// embedded to .data section. This function serializes Initializer
// objects to a flat byte array. It is a compile error if an
// initializer list contains a non-constant expression.
void gvar_initializer(Token **rest, Token *tok, Obj *var) {
  Initializer *init = initializer(rest, tok, var->ty, &var->ty);
  gvar_init_data(var, init);
}

static Type *get_common_type(Type *ty1, Type *ty2) {
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
static void usual_arith_conv(Node **lhs, Node **rhs) {
  Type *ty = get_common_type((*lhs)->ty, (*rhs)->ty);
  *lhs = new_cast(*lhs, ty);
  *rhs = new_cast(*rhs, ty);
}

void add_type(Node *node) {
  if (!node || node->ty)
    return;

  add_type(node->lhs);
  add_type(node->rhs);
  add_type(node->cond);
  add_type(node->then);
  add_type(node->els);
  add_type(node->init);
  add_type(node->inc);

  for (Node *n = node->body; n; n = n->next)
    add_type(n);
  for (Node *n = node->args; n; n = n->next)
    add_type(n);

  switch (node->kind) {
  case ND_NUM:
    node->ty = ty_int;
    return;
  case ND_ADD:
  case ND_SUB: {
    // A non-zero `op` marks a node that a compound assignment or
    // subscript lowering already scaled; it only needs the usual
    // arithmetic conversions, which its fresh untyped predecessor
    // also went through.
    if (node->op) {
      add_type(node->lhs);
      add_type(node->rhs);
      usual_arith_conv(&node->lhs, &node->rhs);
      node->ty = node->lhs->ty;
      return;
    }
    // A raw `+`/`-` from the parser: apply the pointer scaling (and
    // the `num + ptr` canonicalization) here, then type the result.
    // The result is an untyped ADD (numeric or pointer), a pre-typed
    // SUB (`ptr - num`), or a DIV (`ptr - ptr`).
    Node *result = node->kind == ND_ADD ? new_add(node->lhs, node->rhs, node->tok)
                                        : new_sub(node->lhs, node->rhs, node->tok);
    node->kind = result->kind;
    node->lhs = result->lhs;
    node->rhs = result->rhs;
    node->ty = result->ty;
    if (node->ty)
      return;
    add_type(node->lhs);
    add_type(node->rhs);
    usual_arith_conv(&node->lhs, &node->rhs);
    node->ty = node->lhs->ty;
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
      Node *result = new_inc_dec(operand, tok, node->addend);
      node->kind = result->kind;
      node->lhs = result->lhs;
      node->ty = result->ty;
      return;
    }

    Node *expr = new_binary(ND_ASSIGN, operand, new_num(1, tok), tok);
    expr->op = node->addend < 0 ? ND_SUB : ND_ADD;
    Node *result = to_assign(expr);
    node->kind = result->kind;
    node->lhs = result->lhs;
    node->rhs = result->rhs;
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
    node->ty = node->func_ty->return_ty;
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
    // Lower a string literal to a reference to its anonymous global.
    // The global is created here, where the parser used to create it,
    // to keep the allocation order of anonymous names intact.
    Obj *var = new_string_literal(node->tok->str, node->tok->ty);
    node->kind = ND_VAR;
    node->var = var;
    node->ty = var->ty;
    return;
  }
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
    }

    Node *folded;
    if (node->kind == ND_SIZEOF && ty->kind == TY_VLA)
      folded = vla_size_expr(ty, node->tok);
    else
      folded = new_ulong(node->kind == ND_SIZEOF ? ty->size : ty->align, node->tok);

    node->kind = folded->kind;
    node->lhs = folded->lhs;
    node->rhs = folded->rhs;
    node->var = folded->var;
    node->val = folded->val;
    node->ty = folded->ty;
    node->ty_op = NULL;
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
    // Re-insert the dereference implied by `->` (`x->y` is (*x).y).
    if (node->is_arrow) {
      node->lhs = new_unary(ND_DEREF, node->lhs, node->tok);
      node->is_arrow = false;
      add_type(node->lhs);
    }
    node->ty = node->member->ty;
    return;
  case ND_ADDR: {
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
    // place. The ADD is already scaled, so it is marked to keep
    // add_type from scaling it again.
    Node *add = new_add(node->lhs, node->rhs, node->tok);
    add->op = ND_ADD;
    node->kind = ND_DEREF;
    node->lhs = add;
    add_type(node);
    return;
  }
  case ND_DEREF:
    if (!node->lhs->ty->base)
      error_tok(node->tok, "invalid pointer dereference");
    if (node->lhs->ty->base->kind == TY_VOID)
      error_tok(node->tok, "dereferencing a void pointer");

    node->ty = node->lhs->ty->base;
    return;
  case ND_STMT_EXPR:
    if (node->body) {
      Node *stmt = node->body;
      while (stmt->next)
        stmt = stmt->next;
      if (stmt->kind == ND_EXPR_STMT) {
        node->ty = stmt->lhs->ty;
        return;
      }
    }
    error_tok(node->tok, "statement expression returning void is not supported");
    return;
  case ND_LABEL_VAL:
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
  case ND_WHILE:
    // Lower the faithful `while` back to the ND_FOR shape codegen
    // understands: no init/inc, so cont_label jumps to the loop top.
    node->kind = ND_FOR;
    return;
  case ND_BREAK:
  case ND_CONTINUE:
    // Restore the parse-time rewrite to a jump to the enclosing
    // loop's label (or the switch's, for break).
    node->kind = ND_GOTO;
    return;
  case ND_DECL:
    // Lower a declaration. A VLA becomes `x = alloca(<size>)` (the
    // VLA-size statement stays a parse-emitted sibling). With an
    // initializer, the parsed initializer tree becomes the MEMZERO +
    // assignment comma chain the parser used to flatten directly; the
    // node becomes its expression statement. Without one, the node
    // carries the VLA-size computation in lhs and simply becomes that
    // statement.
    if (node->var->ty->kind == TY_VLA) {
      Token *tok = node->tok;
      node->kind = ND_EXPR_STMT;
      node->lhs = new_binary(ND_ASSIGN, new_vla_ptr(node->var, tok),
                             new_alloca(new_var_node(node->var->ty->vla_size, tok)),
                             tok);
    } else if (node->decl_init) {
      node->kind = ND_EXPR_STMT;
      node->lhs = lvar_init_comma(node->var, node->decl_init, node->decl_init->tok);
      node->decl_init = NULL;
    } else {
      node->kind = ND_EXPR_STMT;
    }
    add_type(node);
    return;
  case ND_COMPOUND_LITERAL: {
    // Materialize the compound literal. In block scope it owns the
    // hidden local variable the parser created, and the node lowers to
    // `initializer-comma, var`. At file scope it owns an anonymous
    // global whose data is serialized here, and the node lowers to a
    // reference of it.
    Obj *var = node->var;
    Token *tok = node->tok;

    if (var->is_local) {
      node->kind = ND_COMMA;
      node->lhs = lvar_init_comma(var, node->decl_init, node->decl_init->tok);
      node->rhs = new_var_node(var, tok);
    } else {
      gvar_init_data(var, node->decl_init);
      node->kind = ND_VAR;
    }
    node->decl_init = NULL;
    add_type(node);
    return;
  }
  }
}

int64_t eval(Node *node) {
  return eval2(node, NULL);
}

// Evaluate a given node as a constant expression.
//
// A constant expression is either just a number or ptr+n where ptr
// is a pointer to a global variable and n is a postiive/negative
// number. The latter form is accepted only as an initialization
// expression for a global variable.
int64_t eval2(Node *node, char ***label) {
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

bool is_const_expr(Node *node) {
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

double eval_double(Node *node) {
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
