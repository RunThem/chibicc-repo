// Temporary read-only dump of the parser's faithful syntax tree, taken
// between parse() and sema(): what the dump shows is exactly the tree
// parse.c built, before any name is bound, any type is annotated and any
// shape is lowered. It backs the `--dump-ast` / `-dump-ast` option and
// exists only to inspect that layer; see DUMP-AST-PLAN.md. This file is
// meant to be deleted as a whole, together with its declaration in
// chibicc.h, its call site in main.c and test/ast.sh, so it depends on
// nothing but the shared structures and writes nothing.

#include "chibicc.h"

// A type is printed as its spine only: record members are not expanded
// (a self-referential struct would recurse forever) and nesting deeper
// than this prints "..." instead.
#define MAX_TYPE_DEPTH 8

static void dump_node(Node *node, FILE *out, int depth);
static void dump_chain(Node *node, FILE *out, int depth);
static void dump_init(Initializer *init, FILE *out, int depth);
static void dump_init_item(InitItem *item, FILE *out, int depth);
static void dump_desig(InitDesig *desig, FILE *out, int depth);
static void type_string(Type *ty, char *buf, int cap);
static const char *kind_name(NodeKind kind);
static void print_escaped(FILE *out, char *p, int len);

//
// Output helpers
//

static void indent(FILE *out, int depth) {
  for (int i = 0; i < depth; i++)
    fputs("  ", out);
}

// Token spellings are printed as C-literal text. The escapes are the
// ones cst.c uses, plus the double quote, which the surrounding
// `key="..."` field needs escaped. The space is escaped as \x20 so that
// no dump line ends in whitespace.
static void print_escaped(FILE *out, char *p, int len) {
  for (int i = 0; i < len; i++) {
    unsigned char c = p[i];

    switch (c) {
    case '\n': fputs("\\n", out); break;
    case '\t': fputs("\\t", out); break;
    case '\r': fputs("\\r", out); break;
    case '\\': fputs("\\\\", out); break;
    case '"': fputs("\\\"", out); break;
    default:
      if (c < 0x20 || c >= 0x7f || c == ' ')
        fprintf(out, "\\x%02x", c);
      else
        fputc(c, out);
    }
  }
}

static void print_tok(FILE *out, Token *tok) {
  if (!tok)
    return;
  fputs(" tok=\"", out);
  print_escaped(out, tok->loc, tok->len);
  fputc('"', out);
}

static void print_field_escaped(FILE *out, char *key, char *p, int len) {
  fprintf(out, " %s=\"", key);
  print_escaped(out, p, len);
  fputc('"', out);
}

static void print_str(FILE *out, char *key, char *s) {
  print_field_escaped(out, key, s, strlen(s));
}

static void print_type_field(FILE *out, char *key, Type *ty) {
  char buf[512];
  type_string(ty, buf, sizeof(buf));
  fprintf(out, " %s=\"%s\"", key, buf);
}

// A single child is printed as a label line one level deeper than its
// parent, its content one deeper still. A NULL child prints nothing.
static void dump_child(Node *node, FILE *out, int depth, char *label) {
  if (!node)
    return;
  indent(out, depth + 1);
  fprintf(out, "%s\n", label);
  dump_node(node, out, depth + 2);
}

// The items of a chain are siblings under one label.
static void dump_child_chain(Node *node, FILE *out, int depth, char *label) {
  if (!node)
    return;
  indent(out, depth + 1);
  fprintf(out, "%s\n", label);
  dump_chain(node, out, depth + 2);
}

//
// Types
//

// A bounded append-only string: the dump allocates nothing, so a type
// is assembled in a fixed buffer and truncated rather than grown.
typedef struct {
  char *buf;
  int len;
  int cap;
} TypeBuf;

static void tb_puts(TypeBuf *tb, char *s) {
  int n = strlen(s);
  if (n > tb->cap - 1 - tb->len)
    n = tb->cap - 1 - tb->len;
  if (n <= 0)
    return;
  memcpy(tb->buf + tb->len, s, n);
  tb->len += n;
  tb->buf[tb->len] = '\0';
}

static void tb_printf(TypeBuf *tb, char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tb->buf + tb->len, tb->cap - tb->len, fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if (n > tb->cap - 1 - tb->len)
    tb->len = tb->cap - 1;
  else
    tb->len += n;
}

static void type_append(TypeBuf *tb, Type *ty, int depth);

// The `... <name>` suffix of a parameter; names are identifiers, so no
// escaping is needed.
static void param_name(TypeBuf *tb, Token *tok) {
  tb_puts(tb, " ");
  int n = tok->len;
  if (n > tb->cap - 1 - tb->len)
    n = tb->cap - 1 - tb->len;
  if (n <= 0)
    return;
  memcpy(tb->buf + tb->len, tok->loc, n);
  tb->len += n;
  tb->buf[tb->len] = '\0';
}

static void type_append(TypeBuf *tb, Type *ty, int depth) {
  if (!ty) {
    tb_puts(tb, "?");
    return;
  }

  if (depth > MAX_TYPE_DEPTH) {
    tb_puts(tb, "...");
    return;
  }

  if (ty->is_atomic)
    tb_puts(tb, "_Atomic ");

  switch (ty->kind) {
  case TY_VOID:
    tb_puts(tb, "void");
    return;
  case TY_BOOL:
    tb_puts(tb, "_Bool");
    return;
  case TY_CHAR:
    tb_puts(tb, ty->is_unsigned ? "unsigned char" : "char");
    return;
  case TY_SHORT:
    tb_puts(tb, ty->is_unsigned ? "unsigned short" : "short");
    return;
  case TY_INT:
    tb_puts(tb, ty->is_unsigned ? "unsigned int" : "int");
    return;
  case TY_LONG:
    tb_puts(tb, ty->is_unsigned ? "unsigned long" : "long");
    return;
  case TY_FLOAT:
    tb_puts(tb, "float");
    return;
  case TY_DOUBLE:
    tb_puts(tb, "double");
    return;
  case TY_LDOUBLE:
    tb_puts(tb, "long double");
    return;
  case TY_ENUM:
    tb_puts(tb, "enum");
    return;
  case TY_STRUCT:
    tb_puts(tb, "struct");
    return;
  case TY_UNION:
    tb_puts(tb, "union");
    return;
  case TY_PTR:
    type_append(tb, ty->base, depth + 1);
    tb_puts(tb, " *");
    return;
  case TY_ARRAY:
    type_append(tb, ty->base, depth + 1);
    tb_puts(tb, "[");
    // A written dimension is recorded unevaluated in dim_len; that is
    // the honest answer at this layer, and array_len (-1) would not be.
    if (ty->dim_len) {
      if (ty->dim_len->kind == ND_NUM)
        tb_printf(tb, "%lld", (long long)ty->dim_len->val);
      else
        tb_puts(tb, "?");
    } else if (ty->array_len >= 0) {
      tb_printf(tb, "%d", ty->array_len);
    }
    tb_puts(tb, "]");
    return;
  case TY_VLA:
    type_append(tb, ty->base, depth + 1);
    tb_puts(tb, "[*]");
    return;
  case TY_TYPEOF:
    tb_puts(tb, "typeof(?)");
    return;
  case TY_FUNC: {
    type_append(tb, ty->return_ty, depth + 1);
    tb_puts(tb, " (");
    if (!ty->params) {
      // The parser cannot tell `()` from `(...)`; both have no
      // parameter types, and only is_variadic distinguishes them.
      if (!ty->is_variadic)
        tb_puts(tb, "void");
    } else {
      bool first = true;
      for (Type *p = ty->params; p; p = p->next) {
        if (!first)
          tb_puts(tb, ", ");
        first = false;
        type_append(tb, p, depth + 1);
        if (p->name)
          param_name(tb, p->name);
      }
      if (ty->is_variadic) {
        if (!first)
          tb_puts(tb, ", ");
        tb_puts(tb, "...");
      }
    }
    tb_puts(tb, ")");
    return;
  }
  }
}

static void type_string(Type *ty, char *buf, int cap) {
  TypeBuf tb = {buf, 0, cap};
  buf[0] = '\0';
  type_append(&tb, ty, 0);
}

//
// Initializers
//

static void dump_desig(InitDesig *desig, FILE *out, int depth) {
  indent(out, depth);

  if (desig->name) {
    fputs("INIT_DESIG kind=\"member\"", out);
    print_field_escaped(out, "name", desig->name->loc, desig->name->len);
    fputc('\n', out);
    return;
  }

  fprintf(out, "INIT_DESIG kind=\"%s\"\n", desig->end ? "range" : "index");
  dump_child(desig->begin, out, depth, "begin:");
  dump_child(desig->end, out, depth, "end:");
}

static void dump_init_item(InitItem *item, FILE *out, int depth) {
  if (item->desigs) {
    indent(out, depth);
    fputs("desigs:\n", out);
    for (InitDesig *d = item->desigs; d; d = d->next)
      dump_desig(d, out, depth + 1);
  }

  if (item->init) {
    indent(out, depth);
    fputs("value:\n", out);
    dump_init(item->init, out, depth + 1);
  }
}

static void dump_init(Initializer *init, FILE *out, int depth) {
  indent(out, depth);

  if (init->kind == INIT_STR) {
    fputs("INIT_STR", out);
    print_tok(out, init->str_tok);
    fputc('\n', out);
    return;
  }

  if (init->kind == INIT_EXPR) {
    fputs("INIT_EXPR\n", out);
    dump_child(init->expr, out, depth, "expr:");
    return;
  }

  fputs("INIT_LIST\n", out);
  if (init->items) {
    indent(out, depth + 1);
    fputs("item:\n", out);
    for (InitItem *item = init->items; item; item = item->next)
      dump_init_item(item, out, depth + 2);
  }
}

//
// Nodes
//

static void dump_attr(Node *node, FILE *out) {
  if (node->attr.is_typedef)
    fputs(" typedef=1", out);
  if (node->attr.is_static)
    fputs(" static=1", out);
  if (node->attr.is_extern)
    fputs(" extern=1", out);
  if (node->attr.is_inline)
    fputs(" inline=1", out);
  if (node->attr.is_tls)
    fputs(" tls=1", out);
  if (node->attr.align)
    fprintf(out, " align=%d", node->attr.align);
  if (node->attr.align_ty)
    print_type_field(out, "align_ty", node->attr.align_ty);
}

// The declared identifier: the record's own name slot, which sema
// reads, falling back to the node's representative token.
static Token *decl_name(Node *node) {
  return node->name_tok ? node->name_tok : node->tok;
}

static void dump_node(Node *node, FILE *out, int depth) {
  indent(out, depth);

  const char *name = kind_name(node->kind);
  if (name)
    fputs(name, out);
  else
    fprintf(out, "ND_%d", (int)node->kind);

  switch (node->kind) {
  case ND_ADD:
  case ND_SUB:
  case ND_MUL:
  case ND_DIV:
  case ND_MOD:
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
  case ND_SHL:
  case ND_SHR:
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
  case ND_GT:
  case ND_GE:
  case ND_LOGAND:
  case ND_LOGOR:
  case ND_COMMA:
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->lhs, out, depth, "lhs:");
    dump_child(node->rhs, out, depth, "rhs:");
    return;

  case ND_ASSIGN:
    if (node->op) {
      const char *op = kind_name(node->op);
      fprintf(out, " op=%s", op ? op : "?");
    }
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->lhs, out, depth, "lhs:");
    dump_child(node->rhs, out, depth, "rhs:");
    return;

  case ND_SUBSCRIPT:
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->lhs, out, depth, "base:");
    dump_child(node->rhs, out, depth, "index:");
    return;

  case ND_NEG:
  case ND_ADDR:
  case ND_DEREF:
  case ND_NOT:
  case ND_BITNOT:
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->lhs, out, depth, "expr:");
    return;

  case ND_INCDEC:
    if (node->is_post)
      fputs(" post=1", out);
    fprintf(out, " addend=%d", node->addend);
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->lhs, out, depth, "expr:");
    return;

  case ND_CAST:
    print_type_field(out, "ty_op", node->ty_op);
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->lhs, out, depth, "expr:");
    return;

  case ND_MEMBER:
    if (node->tok)
      print_field_escaped(out, "name", node->tok->loc, node->tok->len);
    if (node->arrow_tok)
      fputs(" arrow=1", out);
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->lhs, out, depth, "base:");
    return;

  case ND_FUNCALL:
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->lhs, out, depth, "callee:");
    dump_child_chain(node->args, out, depth, "args:");
    return;

  case ND_COND:
    if (node->is_elvis)
      fputs(" elvis=1", out);
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->cond, out, depth, "cond:");
    dump_child(node->then, out, depth, "then:");
    dump_child(node->els, out, depth, "else:");
    return;

  case ND_SIZEOF:
  case ND_ALIGNOF:
    if (node->ty_op)
      print_type_field(out, "ty_op", node->ty_op);
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->lhs, out, depth, "expr:");
    return;

  case ND_GENERIC:
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->cond, out, depth, "controlling:");
    dump_child_chain(node->args, out, depth, "assocs:");
    return;

  case ND_GENERIC_ASSOC:
    if (node->ty_op)
      print_type_field(out, "ty_op", node->ty_op);
    else
      fputs(" default=1", out);
    fputc('\n', out);
    dump_child(node->lhs, out, depth, "result:");
    return;

  case ND_TYPES_COMPATIBLE:
    print_type_field(out, "ty_op", node->ty_op);
    print_type_field(out, "ty_op2", node->ty_op2);
    fputc('\n', out);
    return;

  case ND_REG_CLASS:
    print_type_field(out, "ty_op", node->ty_op);
    fputc('\n', out);
    return;

  case ND_COMPOUND_LITERAL:
    print_type_field(out, "ty_op", node->ty_op);
    fputc('\n', out);
    if (node->decl_init) {
      indent(out, depth + 1);
      fputs("init:\n", out);
      dump_init(node->decl_init, out, depth + 2);
    }
    dump_child_chain(node->spec_decls, out, depth, "spec_decls:");
    return;

  case ND_IDENT:
    if (node->tok)
      print_field_escaped(out, "name", node->tok->loc, node->tok->len);
    print_tok(out, node->tok);
    fputc('\n', out);
    return;

  case ND_NUM:
    if (node->ty && is_flonum(node->ty))
      fprintf(out, " fval=%Lg", node->fval);
    else
      fprintf(out, " val=%lld", (long long)node->val);
    print_type_field(out, "ty", node->ty);
    print_tok(out, node->tok);
    fputc('\n', out);
    return;

  case ND_STRING:
    print_tok(out, node->tok);
    fputc('\n', out);
    return;

  case ND_RETURN:
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->lhs, out, depth, "expr:");
    return;

  case ND_IF:
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->cond, out, depth, "cond:");
    dump_child(node->then, out, depth, "then:");
    dump_child(node->els, out, depth, "else:");
    return;

  case ND_FOR:
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child_chain(node->init, out, depth, "init:");
    dump_child(node->cond, out, depth, "cond:");
    dump_child(node->inc, out, depth, "inc:");
    dump_child(node->then, out, depth, "body:");
    return;

  case ND_WHILE:
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->cond, out, depth, "cond:");
    dump_child(node->then, out, depth, "body:");
    return;

  case ND_DO:
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->then, out, depth, "body:");
    dump_child(node->cond, out, depth, "cond:");
    return;

  case ND_SWITCH:
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->cond, out, depth, "cond:");
    dump_child(node->then, out, depth, "body:");
    return;

  case ND_CASE:
    if (node->is_default)
      fputs(" default=1", out);
    print_tok(out, node->tok);
    fputc('\n', out);
    // The operands are recorded unevaluated (begin_expr / end_expr);
    // the evaluated begin / end are what sema fills in later.
    dump_child(node->begin_expr, out, depth, "begin:");
    dump_child(node->end_expr, out, depth, "end:");
    dump_child(node->lhs, out, depth, "body:");
    return;

  case ND_BLOCK:
    if (node->is_scope_block)
      fputs(" scope=1", out);
    fputc('\n', out);
    dump_child_chain(node->body, out, depth, "body:");
    return;

  case ND_GOTO:
    print_str(out, "label", node->label);
    print_tok(out, node->tok);
    fputc('\n', out);
    return;

  case ND_GOTO_EXPR:
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->lhs, out, depth, "expr:");
    return;

  case ND_BREAK:
  case ND_CONTINUE:
    print_tok(out, node->tok);
    fputc('\n', out);
    return;

  case ND_LABEL:
    print_str(out, "label", node->label);
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->lhs, out, depth, "body:");
    return;

  case ND_LABEL_VAL:
    print_str(out, "label", node->label);
    print_tok(out, node->tok);
    fputc('\n', out);
    return;

  case ND_ASM:
    print_str(out, "asm", node->asm_str ? node->asm_str : "");
    print_tok(out, node->tok);
    fputc('\n', out);
    return;

  case ND_EXPR_STMT:
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->lhs, out, depth, "expr:");
    return;

  case ND_STMT_EXPR:
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child_chain(node->body, out, depth, "body:");
    return;

  case ND_DECL:
  case ND_GVAR_DECL:
  case ND_FUNCDEF: {
    Token *ident = decl_name(node);
    if (ident)
      print_field_escaped(out, "name", ident->loc, ident->len);
    print_type_field(out, "ty_op", node->ty_op);
    dump_attr(node, out);
    // The record's own token is the declarator start (`{` or `;` for a
    // function), not the name; `name` above is the useful spelling.
    fputc('\n', out);
    if (node->attr.align_expr) {
      indent(out, depth + 1);
      fputs("align_expr:\n", out);
      dump_node(node->attr.align_expr, out, depth + 2);
    }
    if (node->decl_init) {
      indent(out, depth + 1);
      fputs("init:\n", out);
      dump_init(node->decl_init, out, depth + 2);
    }
    dump_child(node->body, out, depth, "body:");
    dump_child_chain(node->spec_decls, out, depth, "spec_decls:");
    return;
  }

  case ND_TYPEDEF:
    if (node->tok)
      print_field_escaped(out, "name", node->tok->loc, node->tok->len);
    print_type_field(out, "ty", node->ty);
    print_tok(out, node->tok);
    fputc('\n', out);
    return;

  case ND_ENUM_CONST:
    if (node->tok)
      print_field_escaped(out, "name", node->tok->loc, node->tok->len);
    print_type_field(out, "ty_op", node->ty_op);
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->lhs, out, depth, "value:");
    return;

  case ND_VAR:
  case ND_VLA_PTR:
    if (node->var)
      print_str(out, "name", node->var->name);
    fputc('\n', out);
    return;

  case ND_NULL_EXPR:
  case ND_MEMZERO:
    fputc('\n', out);
    return;

  case ND_CAS:
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->cas_addr, out, depth, "addr:");
    dump_child(node->cas_old, out, depth, "old:");
    dump_child(node->cas_new, out, depth, "new:");
    return;

  case ND_EXCH:
    print_tok(out, node->tok);
    fputc('\n', out);
    dump_child(node->lhs, out, depth, "lhs:");
    dump_child(node->rhs, out, depth, "rhs:");
    return;
  }
}

static void dump_chain(Node *node, FILE *out, int depth) {
  for (Node *n = node; n; n = n->next)
    dump_node(n, out, depth);
}

//
// Kind names
//

// Every NodeKind has a name; the caller falls back to ND_<n> so that a
// kind added later shows up (and test/ast.sh fails on it) instead of
// crashing. The four kinds sema produces (ND_NULL_EXPR, ND_VAR,
// ND_VLA_PTR, ND_MEMZERO) are named too, although a dump taken before
// sema never meets them.
static const char *kind_name(NodeKind kind) {
  switch (kind) {
  case ND_NULL_EXPR: return "ND_NULL_EXPR";
  case ND_ADD: return "ND_ADD";
  case ND_SUB: return "ND_SUB";
  case ND_MUL: return "ND_MUL";
  case ND_DIV: return "ND_DIV";
  case ND_NEG: return "ND_NEG";
  case ND_MOD: return "ND_MOD";
  case ND_BITAND: return "ND_BITAND";
  case ND_BITOR: return "ND_BITOR";
  case ND_BITXOR: return "ND_BITXOR";
  case ND_SHL: return "ND_SHL";
  case ND_SHR: return "ND_SHR";
  case ND_EQ: return "ND_EQ";
  case ND_NE: return "ND_NE";
  case ND_LT: return "ND_LT";
  case ND_LE: return "ND_LE";
  case ND_GT: return "ND_GT";
  case ND_GE: return "ND_GE";
  case ND_ASSIGN: return "ND_ASSIGN";
  case ND_INCDEC: return "ND_INCDEC";
  case ND_SUBSCRIPT: return "ND_SUBSCRIPT";
  case ND_COND: return "ND_COND";
  case ND_COMMA: return "ND_COMMA";
  case ND_MEMBER: return "ND_MEMBER";
  case ND_ADDR: return "ND_ADDR";
  case ND_DEREF: return "ND_DEREF";
  case ND_NOT: return "ND_NOT";
  case ND_BITNOT: return "ND_BITNOT";
  case ND_LOGAND: return "ND_LOGAND";
  case ND_LOGOR: return "ND_LOGOR";
  case ND_RETURN: return "ND_RETURN";
  case ND_IF: return "ND_IF";
  case ND_FOR: return "ND_FOR";
  case ND_WHILE: return "ND_WHILE";
  case ND_DO: return "ND_DO";
  case ND_SWITCH: return "ND_SWITCH";
  case ND_CASE: return "ND_CASE";
  case ND_BLOCK: return "ND_BLOCK";
  case ND_GOTO: return "ND_GOTO";
  case ND_BREAK: return "ND_BREAK";
  case ND_CONTINUE: return "ND_CONTINUE";
  case ND_GOTO_EXPR: return "ND_GOTO_EXPR";
  case ND_LABEL: return "ND_LABEL";
  case ND_LABEL_VAL: return "ND_LABEL_VAL";
  case ND_FUNCALL: return "ND_FUNCALL";
  case ND_EXPR_STMT: return "ND_EXPR_STMT";
  case ND_STMT_EXPR: return "ND_STMT_EXPR";
  case ND_IDENT: return "ND_IDENT";
  case ND_VAR: return "ND_VAR";
  case ND_VLA_PTR: return "ND_VLA_PTR";
  case ND_NUM: return "ND_NUM";
  case ND_STRING: return "ND_STRING";
  case ND_SIZEOF: return "ND_SIZEOF";
  case ND_ALIGNOF: return "ND_ALIGNOF";
  case ND_GENERIC: return "ND_GENERIC";
  case ND_GENERIC_ASSOC: return "ND_GENERIC_ASSOC";
  case ND_TYPES_COMPATIBLE: return "ND_TYPES_COMPATIBLE";
  case ND_REG_CLASS: return "ND_REG_CLASS";
  case ND_CAST: return "ND_CAST";
  case ND_MEMZERO: return "ND_MEMZERO";
  case ND_DECL: return "ND_DECL";
  case ND_GVAR_DECL: return "ND_GVAR_DECL";
  case ND_FUNCDEF: return "ND_FUNCDEF";
  case ND_TYPEDEF: return "ND_TYPEDEF";
  case ND_ENUM_CONST: return "ND_ENUM_CONST";
  case ND_COMPOUND_LITERAL: return "ND_COMPOUND_LITERAL";
  case ND_ASM: return "ND_ASM";
  case ND_CAS: return "ND_CAS";
  case ND_EXCH: return "ND_EXCH";
  }
  return NULL;
}

//
// Entry point
//

void dump_ast(Node *toplevel, FILE *out) {
  dump_chain(toplevel, out, 0);
}
