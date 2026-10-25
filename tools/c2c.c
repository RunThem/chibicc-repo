// c2c - the C-to-C printer (plans/PLAN-c2c.md): the library's second
// real consumer, over layer 3. It tokenizes, preprocesses and parses a
// translation unit and prints the faithful syntax tree back as one
// fully expanded C source file.
//
// Output policy (PLAN-c2c 1.4, D3-D8):
// - The input is the preprocessed stream, so macros and #includes are
//   gone; the output is the single expanded translation unit.
// - Comments are dropped and every byte of whitespace is the printer's
//   own: 2-space indent, one declaration per line, one statement per
//   line. No braces are added around substatements - braces would grow
//   an ND_BLOCK and change the tree.
// - Spellings are preserved: identifiers and literals print from the
//   tokens' own text, so 0x10, 'a', 1.5f, L"x" and macro-produced
//   spellings survive verbatim.
// - struct/union/enum tags print from the type records' tag_tok
//   (PLAN-c2c D2). Enum definitions regroup from their ND_ENUM_CONST
//   records; struct and union definitions print inline at their first
//   use and by tag afterwards; a record that stayed incomplete through
//   parse gets a `struct T;` at the top of the file.
// - typedef names are recovered through a side table (the first
//   typedef of a non-basic type record wins); basic-type singletons
//   are never mapped, so `typedef int T` does not rename every int.
//   A typedef record's own declaration prints structurally, so the
//   definition itself is never spelled with the name it introduces.
// - Consecutive declaration records over the same type record and the
//   same attributes join into one multi-declarator statement; this is
//   what keeps an anonymous `enum { A } x, y;` a single definition.
// - const/volatile qualifiers, asm's volatile/inline qualifiers and
//   type-irrelevant GNU attributes are not in the IR and do not come
//   back (known gaps); packed and aligned print in canonical position.
//
// Usage: c2c [-I dir]... [-D name[=val]]... [-U name]... file.c

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libcp/libcp.h>

// ---------------------------------------------------------------------------
// Emitter: raw bytes with indent management. Spacing decisions are the
// callers' - every helper here is byte-faithful.

typedef struct {
  FILE *out;
  int indent;
  int at_bol;
  int last; // last byte emitted, for the maximal-munch guard
} E;

// Two same punctuation bytes glued together would lex as one longer
// token (`--`, `++`, `&&`, `||`, `<<`, `>>`); e_op keeps them apart.
static bool munch_pair(int a, int b) {
  if (a != b)
    return false;
  return a == '-' || a == '+' || a == '&' || a == '|' || a == '<' || a == '>';
}

static void e_putc(E *e, int c) {
  if (e->at_bol) {
    for (int i = 0; i < e->indent; i++)
      fputs("  ", e->out);
    e->at_bol = 0;
  }
  fputc(c, e->out);
  e->last = c;
}

static void e_write(E *e, const char *s, int len) {
  for (int i = 0; i < len; i++)
    e_putc(e, s[i]);
}

static void e_str(E *e, const char *s) { e_write(e, s, strlen(s)); }

// Emits one token, guarding against gluing with the previous byte.
static void e_op(E *e, const char *s) {
  if (!e->at_bol && munch_pair(e->last, s[0]))
    fputc(' ', e->out);
  e_str(e, s);
}

static void e_fmt(E *e, const char *fmt, ...) {
  char buf[64];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  e_str(e, buf);
}

static void e_nl(E *e) {
  fputc('\n', e->out);
  e->at_bol = 1;
}

// A token's own spelling, byte for byte.
static void e_tok(E *e, const LibcpToken *tok) {
  if (tok && tok->len)
    e_write(e, tok->loc, tok->len);
}

// ---------------------------------------------------------------------------
// Per-type side table (PLAN-c2c D5): a pointer-keyed hash with the
// facts the printer accumulates about a type record - its first
// typedef name, the ND_ENUM_CONST records of its definition, whether
// its definition or forward declaration was already printed, and the
// pre-pass cycle guard.

typedef struct EnumRec {
  struct EnumRec *next;
  LibcpNode *node;
} EnumRec;

typedef struct TyInfo {
  struct TyInfo *next;
  LibcpType *ty;
  const char *tname;    // first typedef name, NULL if none
  EnumRec *enums;       // definition records in source order
  EnumRec *enums_tail;
  bool emitted;         // definition already printed
  bool forwarded;       // forward declaration already printed
  bool collected;       // pre-pass aggregate cycle guard
} TyInfo;

#define TY_BUCKETS 509

typedef struct {
  E e;
  TyInfo *buckets[TY_BUCKETS];
  LibcpType *pending[64]; // incomplete aggregates, file-top forward decls
  int pending_len;
  int errors;
} Printer;

static TyInfo *ty_info(Printer *p, LibcpType *ty) {
  uintptr_t h = (uintptr_t)ty >> 3;
  TyInfo *info = p->buckets[h % TY_BUCKETS];
  for (; info; info = info->next)
    if (info->ty == ty)
      return info;
  info = calloc(1, sizeof(TyInfo));
  info->ty = ty;
  h %= TY_BUCKETS;
  info->next = p->buckets[h];
  p->buckets[h] = info;
  return info;
}

// A basic-type record is a shared singleton (PLAN-c2c D5): a typedef
// over it must not rename every use of the base type.
static bool is_basic_type(LibcpType *ty) {
  switch (ty->kind) {
  case TY_VOID:
  case TY_BOOL:
  case TY_CHAR:
  case TY_SHORT:
  case TY_INT:
  case TY_LONG:
  case TY_FLOAT:
  case TY_DOUBLE:
  case TY_LDOUBLE:
    return true;
  default:
    return false;
  }
}

// The declared identifier of a declaration record: ND_TYPEDEF anchors
// its name token directly (the parser never sets name_tok there); the
// other records carry it in name_tok.
static const LibcpToken *record_name_tok(LibcpNode *node) {
  return node->kind == ND_TYPEDEF ? node->tok : node->name_tok;
}

// ---------------------------------------------------------------------------
// Pre-pass: walk the whole tree once and register typedef names and
// enum-definition records, so a type spec can print its definition
// wherever the record first appears.

static void collect_node(Printer *p, LibcpNode *node);
static void collect_type(Printer *p, LibcpType *ty);
static void collect_init(Printer *p, LibcpInitializer *init);

static void collect_exprs(Printer *p, LibcpNode *node) {
  for (; node; node = node->next)
    collect_node(p, node);
}

static void collect_node(Printer *p, LibcpNode *node) {
  if (!node)
    return;

  switch (node->kind) {
  case ND_TYPEDEF: {
    LibcpType *ty = node->ty;
    if (!is_basic_type(ty)) {
      TyInfo *info = ty_info(p, ty);
      if (!info->tname)
        info->tname = strndup(record_name_tok(node)->loc,
                              record_name_tok(node)->len);
    }
    break;
  }
  case ND_ENUM_CONST: {
    TyInfo *info = ty_info(p, node->ty_op);
    EnumRec *rec = calloc(1, sizeof(EnumRec));
    rec->node = node;
    if (info->enums_tail)
      info->enums_tail->next = rec;
    else
      info->enums = rec;
    info->enums_tail = rec;
    break;
  }
  default:
    break;
  }

  collect_type(p, node->ty);
  collect_type(p, node->ty_op);
  collect_type(p, node->ty_op2);
  collect_type(p, node->func_ty);
  collect_type(p, node->attr.align_ty);

  collect_exprs(p, node->lhs);
  collect_exprs(p, node->rhs);
  collect_exprs(p, node->cond);
  collect_exprs(p, node->then);
  collect_exprs(p, node->els);
  collect_exprs(p, node->init);
  collect_exprs(p, node->inc);
  collect_exprs(p, node->body);
  collect_exprs(p, node->args);
  collect_exprs(p, node->generic_sel);
  collect_exprs(p, node->spec_decls);
  collect_exprs(p, node->begin_expr);
  collect_exprs(p, node->end_expr);
  collect_exprs(p, node->cas_addr);
  collect_exprs(p, node->cas_old);
  collect_exprs(p, node->cas_new);
  collect_exprs(p, node->atomic_expr);
  collect_exprs(p, node->attr.align_expr);
  collect_init(p, node->decl_init);
}

static void collect_init(Printer *p, LibcpInitializer *init) {
  if (!init)
    return;
  collect_exprs(p, init->expr);
  for (LibcpInitItem *item = init->items; item; item = item->next) {
    for (LibcpInitDesig *d = item->desigs; d; d = d->next) {
      collect_exprs(p, d->begin);
      collect_exprs(p, d->end);
    }
    collect_init(p, item->init);
  }
}

static void collect_type(Printer *p, LibcpType *ty) {
  if (!ty)
    return;

  switch (ty->kind) {
  case TY_PTR:
  case TY_ARRAY:
  case TY_VLA:
    collect_type(p, ty->base);
    collect_exprs(p, ty->dim_len);
    collect_exprs(p, ty->vla_len);
    collect_exprs(p, ty->typeof_expr);
    collect_exprs(p, ty->align_expr);
    return;
  case TY_FUNC:
    collect_type(p, ty->return_ty);
    for (LibcpType *param = ty->params; param; param = param->next) {
      collect_type(p, param);
      collect_exprs(p, param->spec_decls);
    }
    collect_exprs(p, ty->spec_decls);
    return;
  case TY_TYPEOF:
    collect_exprs(p, ty->typeof_expr);
    return;
  case TY_STRUCT:
  case TY_UNION:
  case TY_ENUM:
    break;
  default:
    return;
  }

  // Aggregates: walking a record's members twice would double-register
  // its enum records, so each record is walked once.
  TyInfo *info = ty_info(p, ty);
  if (info->collected)
    return;
  info->collected = true;

  if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) &&
      !ty->layout_pending && ty->tag_tok && p->pending_len < 64)
    p->pending[p->pending_len++] = ty;

  for (LibcpMember *mem = ty->members; mem; mem = mem->next) {
    collect_type(p, mem->ty);
    collect_type(p, mem->align_ty);
    collect_exprs(p, mem->align_expr);
    collect_exprs(p, mem->width_expr);
  }
}

// A string literal, from the token's decoded contents: adjacent
// literals were joined during preprocessing, so the token's spelling
// covers only the first piece. The encoding prefix is recovered from
// the element type (L"" widens to int, u"" to short); plain char
// needs none.
//
// Escaping is chosen so the text can never re-lex differently: char
// strings use three-digit octal escapes for non-printables (octal
// stops at three digits by itself), and wide strings escape per
// element, padding \x to the element's full width and also escaping a
// hex-digit element that directly follows an escape (a hex escape
// absorbs any following hex digits).
static void e_str_literal(E *e, const LibcpToken *tok) {
  if (!tok)
    return;
  LibcpType *base = tok->ty ? tok->ty->base : NULL;
  int esz = base ? base->size : 1;
  if (esz == 4)
    e_str(e, "L");
  else if (esz == 2)
    e_str(e, "u");
  e_str(e, "\"");
  int elems = tok->ty ? tok->ty->array_len - 1 : 0;
  bool escaped = false;
  for (int i = 0; i < elems; i++) {
    uint32_t v = 0;
    if (esz == 1)
      v = (unsigned char)tok->str[i];
    else if (esz == 2)
      memcpy(&v, tok->str + 2 * i, 2);
    else
      memcpy(&v, tok->str + 4 * i, 4);
    bool hexdigit = (v >= '0' && v <= '9') || (v >= 'a' && v <= 'f') ||
                    (v >= 'A' && v <= 'F');
    if (esz == 1 && v >= 0x20 && v < 0x7f && v != '"' && v != '\\') {
      e_putc(e, v);
      escaped = false;
      continue;
    }
    if (esz > 1 && v >= 0x20 && v < 0x7f && v != '"' && v != '\\' &&
        (!hexdigit || !escaped)) {
      e_putc(e, v);
      escaped = false;
      continue;
    }
    switch (v) {
    case '"':
      e_str(e, "\\\"");
      break;
    case '\\':
      e_str(e, "\\\\");
      break;
    case '\n':
      e_str(e, "\\n");
      break;
    case '\t':
      e_str(e, "\\t");
      break;
    case '\r':
      e_str(e, "\\r");
      break;
    case '\a':
      e_str(e, "\\a");
      break;
    case '\b':
      e_str(e, "\\b");
      break;
    case '\f':
      e_str(e, "\\f");
      break;
    case '\v':
      e_str(e, "\\v");
      break;
    default:
      if (esz == 1)
        e_fmt(e, "\\%03o", v);
      else if (esz == 2)
        e_fmt(e, "\\x%04x", v);
      else
        e_fmt(e, "\\x%08x", v);
      escaped = true;
      continue;
    }
    escaped = true;
  }
  e_str(e, "\"");
}

// ---------------------------------------------------------------------------
// Expression printing with the C precedence ladder (PLAN-c2c D8). A
// node prints its children at a context level; a child whose own
// level falls below the context is parenthesized. The one place the
// ladder is not enough is a postfix base: under a call, a subscript
// or a member access, anything that is not itself a postfix form or
// an atom parenthesizes.

static int expr_level(LibcpNode *node) {
  switch (node->kind) {
  case ND_FUNCALL:
  case ND_SUBSCRIPT:
  case ND_MEMBER:
    return 16;
  case ND_INCDEC:
    return node->is_post ? 16 : 15;
  case ND_NEG:
  case ND_NOT:
  case ND_BITNOT:
  case ND_ADDR:
  case ND_DEREF:
  case ND_CAST:
  case ND_SIZEOF:
  case ND_ALIGNOF:
  case ND_LABEL_VAL:
    return 15;
  case ND_MUL:
  case ND_DIV:
  case ND_MOD:
    return 13;
  case ND_ADD:
  case ND_SUB:
    return 12;
  case ND_SHL:
  case ND_SHR:
    return 11;
  case ND_LT:
  case ND_LE:
  case ND_GT:
  case ND_GE:
    return 10;
  case ND_EQ:
  case ND_NE:
    return 9;
  case ND_BITAND:
    return 8;
  case ND_BITXOR:
    return 7;
  case ND_BITOR:
    return 6;
  case ND_LOGAND:
    return 5;
  case ND_LOGOR:
    return 4;
  case ND_COND:
    return 3;
  case ND_ASSIGN:
    return 2;
  case ND_COMMA:
    return 1;
  default:
    return 17; // atoms
  }
}

static bool is_atom_or_postfix(LibcpNode *node) {
  switch (node->kind) {
  case ND_IDENT:
  case ND_NUM:
  case ND_STRING:
  case ND_VAR:
  case ND_COMPOUND_LITERAL:
  case ND_STMT_EXPR:
  case ND_LABEL_VAL:
  case ND_FUNCALL:
  case ND_SUBSCRIPT:
  case ND_MEMBER:
    return true;
  case ND_INCDEC:
    return node->is_post;
  default:
    return false;
  }
}

static const char *binary_op_str(LibcpNodeKind kind) {
  switch (kind) {
  case ND_ADD:
    return "+";
  case ND_SUB:
    return "-";
  case ND_MUL:
    return "*";
  case ND_DIV:
    return "/";
  case ND_MOD:
    return "%";
  case ND_BITAND:
    return "&";
  case ND_BITOR:
    return "|";
  case ND_BITXOR:
    return "^";
  case ND_SHL:
    return "<<";
  case ND_SHR:
    return ">>";
  case ND_EQ:
    return "==";
  case ND_NE:
    return "!=";
  case ND_LT:
    return "<";
  case ND_LE:
    return "<=";
  case ND_GT:
    return ">";
  case ND_GE:
    return ">=";
  case ND_LOGAND:
    return "&&";
  case ND_LOGOR:
    return "||";
  default:
    return "?";
  }
}

static const char *compound_assign_str(LibcpNodeKind kind) {
  switch (kind) {
  case ND_ADD:
    return "+=";
  case ND_SUB:
    return "-=";
  case ND_MUL:
    return "*=";
  case ND_DIV:
    return "/=";
  case ND_MOD:
    return "%=";
  case ND_BITAND:
    return "&=";
  case ND_BITOR:
    return "|=";
  case ND_BITXOR:
    return "^=";
  case ND_SHL:
    return "<<=";
  case ND_SHR:
    return ">>=";
  default:
    return "?=";
  }
}

static void print_expr_ctx(Printer *p, E *e, LibcpNode *node, int ctx);
static void print_init(Printer *p, LibcpInitializer *init);

static void print_expr_e(Printer *p, E *e, LibcpNode *node) {
  print_expr_ctx(p, e, node, 1);
}
// A type name in expression position: a cast, sizeof(type), a _Generic
// association or a type-predicate builtin argument.
static LibcpType *base_type(LibcpType *ty);
static void print_declspec_e(Printer *p, E *e, LibcpType *ty, bool defining);
static void print_full_type(Printer *p, E *e, LibcpType *ty,
                            const LibcpToken *name, bool defining);
static void print_typename_e(Printer *p, E *e, LibcpType *ty) {
  print_full_type(p, e, ty, NULL, false);
}

static void print_postfix_base(Printer *p, E *e, LibcpNode *node) {
  if (is_atom_or_postfix(node)) {
    // 16, not 17: a postfix chain (a call, subscript or member series)
    // continues without parentheses.
    print_expr_ctx(p, e, node, 16);
    return;
  }
  e_str(e, "(");
  print_expr_ctx(p, e, node, 1);
  e_str(e, ")");
}

static void print_stmt(Printer *p, LibcpNode *node);

static void print_expr_ctx(Printer *p, E *e, LibcpNode *node, int ctx) {
  if (!node)
    return;

  if (expr_level(node) < ctx) {
    e_str(e, "(");
    print_expr_ctx(p, e, node, 1);
    e_str(e, ")");
    return;
  }

  int lv = expr_level(node);

  switch (node->kind) {
  case ND_IDENT:
  case ND_NUM:
    e_tok(e, node->tok);
    return;
  case ND_STRING:
    e_str_literal(e, node->tok);
    return;
  case ND_VAR: // sema-bound; defensive at layer 3
    e_str(e, node->var ? node->var->name : "");
    return;
  case ND_NULL_EXPR: // sema-built; defensive at layer 3
    e_str(e, "((void)0)");
    return;

  // Binary operators, left-associative: the left side may repeat the
  // operator's own level, the right side may not.
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
    print_expr_ctx(p, e, node->lhs, lv);
    e_str(e, " ");
    e_op(e, binary_op_str(node->kind));
    e_str(e, " ");
    print_expr_ctx(p, e, node->rhs, lv + 1);
    return;

  case ND_ASSIGN:
    print_expr_ctx(p, e, node->lhs, 15);
    e_str(e, " ");
    e_op(e, node->op ? compound_assign_str(node->op) : "=");
    e_str(e, " ");
    print_expr_ctx(p, e, node->rhs, 2);
    return;

  case ND_COMMA:
    print_expr_ctx(p, e, node->lhs, 1);
    e_str(e, ", ");
    print_expr_ctx(p, e, node->rhs, 2);
    return;

  case ND_COND:
    // The middle is a full expression; the tail recurses to the right.
    print_expr_ctx(p, e, node->cond, 4);
    if (node->is_elvis) {
      e_str(e, " ?: ");
      print_expr_ctx(p, e, node->els, 3);
      return;
    }
    e_str(e, " ? ");
    print_expr_ctx(p, e, node->then, 1);
    e_str(e, " : ");
    print_expr_ctx(p, e, node->els, 3);
    return;

  case ND_INCDEC:
    if (node->is_post) {
      print_expr_ctx(p, e, node->lhs, 15);
      e_op(e, node->addend < 0 ? "--" : "++");
    } else {
      e_op(e, node->addend < 0 ? "--" : "++");
      print_expr_ctx(p, e, node->lhs, 15);
    }
    return;

  case ND_NEG:
  case ND_NOT:
  case ND_BITNOT:
  case ND_ADDR:
  case ND_DEREF: {
    const char *op = node->kind == ND_NEG     ? "-"
                     : node->kind == ND_NOT   ? "!"
                     : node->kind == ND_BITNOT ? "~"
                     : node->kind == ND_ADDR  ? "&"
                                              : "*";
    e_op(e, op);
    print_expr_ctx(p, e, node->lhs, 15);
    return;
  }

  case ND_CAST:
    e_str(e, "(");
    print_typename_e(p, e, node->ty_op);
    e_str(e, ")");
    print_expr_ctx(p, e, node->lhs, 15);
    return;

  case ND_SIZEOF:
    if (node->ty_op) {
      e_str(e, "sizeof(");
      print_typename_e(p, e, node->ty_op);
      e_str(e, ")");
      return;
    }
    e_str(e, "sizeof ");
    print_expr_ctx(p, e, node->lhs, 15);
    return;

  case ND_ALIGNOF:
    if (node->ty_op) {
      e_str(e, "_Alignof(");
      print_typename_e(p, e, node->ty_op);
      e_str(e, ")");
      return;
    }
    e_str(e, "_Alignof ");
    print_expr_ctx(p, e, node->lhs, 15);
    return;

  case ND_MEMBER:
    print_postfix_base(p, e, node->lhs);
    if (node->arrow_tok)
      e_str(e, "->");
    else
      e_str(e, ".");
    // The member name rides the token while unbound (layer 3) and the
    // member record once sema has bound it.
    if (node->member && node->member->name)
      e_tok(e, node->member->name);
    else
      e_tok(e, node->tok);
    return;

  case ND_SUBSCRIPT:
    print_postfix_base(p, e, node->lhs);
    e_str(e, "[");
    print_expr_ctx(p, e, node->rhs, 1);
    e_str(e, "]");
    return;

  case ND_FUNCALL:
    print_postfix_base(p, e, node->lhs);
    e_str(e, "(");
    bool first = true;
    for (LibcpNode *arg = node->args; arg; arg = arg->next) {
      if (!first)
        e_str(e, ", ");
      first = false;
      print_expr_ctx(p, e, arg, 2);
    }
    e_str(e, ")");
    return;

  case ND_LABEL_VAL:
    e_op(e, "&&");
    e_str(e, node->label);
    return;

  case ND_STMT_EXPR:
    e_str(e, "({ ");
    for (LibcpNode *stmt = node->body; stmt; stmt = stmt->next)
      print_stmt(p, stmt);
    e_str(e, " })");
    return;

  case ND_COMPOUND_LITERAL: {
    e_str(e, "(");
    print_typename_e(p, e, node->ty_op);
    e_str(e, ")");
    // The grammar of a compound literal is `( type-name ) { ... }`;
    // a scalar initializer the parser kept as an expression still
    // prints braced.
    if (node->decl_init && node->decl_init->kind != INIT_LIST) {
      e_str(e, "{ ");
      print_init(p, node->decl_init);
      e_str(e, " }");
      return;
    }
    print_init(p, node->decl_init);
    return;
  }

  case ND_GENERIC:
    e_str(e, "_Generic(");
    print_expr_ctx(p, e, node->cond, 2);
    for (LibcpNode *assoc = node->args; assoc; assoc = assoc->next) {
      e_str(e, ", ");
      if (assoc->ty_op) {
        print_typename_e(p, e, assoc->ty_op);
      } else {
        e_str(e, "default");
      }
      e_str(e, ": ");
      print_expr_ctx(p, e, assoc->lhs, 2);
    }
    e_str(e, ")");
    return;

  case ND_TYPES_COMPATIBLE:
    e_str(e, "__builtin_types_compatible_p(");
    print_typename_e(p, e, node->ty_op);
    e_str(e, ", ");
    print_typename_e(p, e, node->ty_op2);
    e_str(e, ")");
    return;

  case ND_REG_CLASS:
    e_str(e, "__builtin_reg_class(");
    print_typename_e(p, e, node->ty_op);
    e_str(e, ")");
    return;

  case ND_CAS:
    e_str(e, "__builtin_compare_and_swap(");
    print_expr_ctx(p, e, node->cas_addr, 2);
    e_str(e, ", ");
    print_expr_ctx(p, e, node->cas_old, 2);
    e_str(e, ", ");
    print_expr_ctx(p, e, node->cas_new, 2);
    e_str(e, ")");
    return;

  case ND_EXCH:
    e_str(e, "__builtin_atomic_exchange(");
    print_expr_ctx(p, e, node->lhs, 2);
    e_str(e, ", ");
    print_expr_ctx(p, e, node->rhs, 2);
    e_str(e, ")");
    return;

  default:
    p->errors++;
    fprintf(stderr, "c2c: unprintable expression kind %d\n", (int)node->kind);
    exit(2);
  }
}

// ---------------------------------------------------------------------------
// Type printing (PLAN-c2c D2, D6-D8).

static void print_declspec_e(Printer *p, E *e, LibcpType *ty, bool defining);
static void print_declarator(Printer *p, E *e, LibcpType *ty,
                             const LibcpToken *name, LibcpType *stop);

// The base of a declarator's derivation chain: the declspec type the
// derived layers (pointer, array, function) wrap.
static LibcpType *base_type(LibcpType *ty) {
  for (;;) {
    switch (ty->kind) {
    case TY_PTR:
    case TY_ARRAY:
    case TY_VLA:
      ty = ty->base;
      continue;
    case TY_FUNC:
      ty = ty->return_ty;
      continue;
    default:
      return ty;
    }
  }
}

static void print_enum_spec_e(Printer *p, E *e, LibcpType *ty) {
  TyInfo *info = ty_info(p, ty);
  e_str(e, "enum");
  if (ty->tag_tok) {
    e_str(e, " ");
    e_tok(e, ty->tag_tok);
  }
  if (info->emitted)
    return;
  info->emitted = true;

  // The definition's records regroup here (PLAN-c2c D6); an enum spec
  // without records (`enum E { }`) keeps its empty braces.
  e_str(e, " {");
  bool first = true;
  for (EnumRec *rec = info->enums; rec; rec = rec->next) {
    LibcpNode *node = rec->node;
    if (!first)
      e_str(e, ",");
    e_nl(e);
    e->indent++;
    e_tok(e, node->tok);
    if (node->lhs) {
      e_str(e, " = ");
      print_expr_ctx(p, e, node->lhs, 3); // constant-expression
    }
    e->indent--;
    first = false;
  }
  if (first) {
    e_str(e, " }");
    return;
  }
  e_nl(e);
  e_str(e, "}");
}

static void print_member_e(Printer *p, E *e, LibcpMember *mem) {
  if (mem->align_expr) {
    e_str(e, "_Alignas(");
    print_expr_ctx(p, e, mem->align_expr, 3); // constant-expression
    e_str(e, ") ");
  }
  if (mem->align_ty) {
    e_str(e, "_Alignas(");
    print_full_type(p, e, mem->align_ty, NULL, false);
    e_str(e, ") ");
  }
  print_full_type(p, e, mem->ty, mem->name, false);
  if (mem->width_expr) {
    e_str(e, " : ");
    print_expr_ctx(p, e, mem->width_expr, 3); // constant-expression
  }
  e_str(e, ";");
}

static void print_struct_spec_e(Printer *p, E *e, LibcpType *ty) {
  TyInfo *info = ty_info(p, ty);
  e_str(e, ty->kind == TY_UNION ? "union" : "struct");
  if (ty->tag_tok) {
    e_str(e, " ");
    e_tok(e, ty->tag_tok);
  }
  if (info->emitted)
    return;

  if (!ty->layout_pending)
    // Incomplete through the whole parse (PLAN-c2c D7): a forward
    // declaration was arranged at the top of the file; the spec itself
    // stays tag-only.
    return;

  info->emitted = true;
  e_str(e, " {");
  for (LibcpMember *mem = ty->members; mem; mem = mem->next) {
    e_nl(e);
    e->indent++;
    print_member_e(p, e, mem);
    e->indent--;
  }
  e_nl(e);
  e_str(e, "}");
  if (ty->is_packed)
    e_str(e, " __attribute__((packed))");
  if (ty->align_expr) {
    e_str(e, " __attribute__((aligned(");
    print_expr_ctx(p, e, ty->align_expr, 3); // constant-expression
    e_str(e, ")))");
  }
}

static void print_declspec_e(Printer *p, E *e, LibcpType *ty, bool defining) {
  TyInfo *info = ty_info(p, ty);
  if (!defining && info->tname) {
    e_str(e, info->tname);
    return;
  }

  if (ty->is_atomic)
    e_str(e, "_Atomic ");

  switch (ty->kind) {
  case TY_VOID:
    e_str(e, "void");
    return;
  case TY_BOOL:
    e_str(e, "_Bool");
    return;
  case TY_CHAR:
    e_str(e, ty->is_unsigned ? "unsigned char" : "char");
    return;
  case TY_SHORT:
    e_str(e, ty->is_unsigned ? "unsigned short" : "short");
    return;
  case TY_INT:
    e_str(e, ty->is_unsigned ? "unsigned int" : "int");
    return;
  case TY_LONG:
    e_str(e, ty->is_unsigned ? "unsigned long" : "long");
    return;
  case TY_FLOAT:
    e_str(e, "float");
    return;
  case TY_DOUBLE:
    e_str(e, "double");
    return;
  case TY_LDOUBLE:
    e_str(e, "long double");
    return;
  case TY_ENUM:
    print_enum_spec_e(p, e, ty);
    return;
  case TY_STRUCT:
  case TY_UNION:
    print_struct_spec_e(p, e, ty);
    return;
  case TY_TYPEOF:
    e_str(e, "typeof(");
    print_expr_e(p, e, ty->typeof_expr);
    e_str(e, ")");
    return;
  default:
    p->errors++;
    fprintf(stderr, "c2c: type kind %d in declspec position\n", (int)ty->kind);
    exit(2);
  }
}

// ---------------------------------------------------------------------------
// Declarator printing, by name accumulation (PLAN-c2c D8): the type
// record's derivation chain is walked outside-in while the declarator
// text is built around the identifier. A pointer layer prepends "*";
// a function or array layer appends its suffix, wrapping what it has
// so far in parentheses when a "*" is exposed - exactly the C rule
// that a postfix operator binds tighter than a pointer declarator.

typedef struct {
  char *s;
  size_t len;
  size_t cap;
} SB;

static void sb_push(SB *sb, const char *s, size_t n) {
  if (sb->len + n + 1 > sb->cap) {
    sb->cap = sb->cap ? sb->cap * 2 : 32;
    while (sb->cap < sb->len + n + 1)
      sb->cap *= 2;
    sb->s = realloc(sb->s, sb->cap);
  }
  memcpy(sb->s + sb->len, s, n);
  sb->len += n;
  sb->s[sb->len] = '\0';
}

static void sb_str(SB *sb, const char *s) { sb_push(sb, s, strlen(s)); }

// Prepends text, for the "*" of a pointer layer.
static void sb_prepend_str(SB *sb, const char *s) {
  size_t n = strlen(s);
  if (sb->len + n + 1 > sb->cap) {
    sb->cap = sb->cap ? sb->cap * 2 : 32;
    while (sb->cap < sb->len + n + 1)
      sb->cap *= 2;
    sb->s = realloc(sb->s, sb->cap);
  }
  memmove(sb->s + n, sb->s, sb->len + 1);
  memcpy(sb->s, s, n);
  sb->len += n;
}

// Runs `fn` with a memory stream emitter and returns the text it
// wrote; the declarator builder needs suffixes as strings.
typedef void (*EmitFn)(Printer *, E *, LibcpType *);

static char *capture(Printer *p, EmitFn fn, LibcpType *ty) {
  char *buf = NULL;
  size_t size = 0;
  FILE *f = open_memstream(&buf, &size);
  if (!f) {
    fprintf(stderr, "c2c: open_memstream failed\n");
    exit(1);
  }
  E e = {f, 0, 1};
  fn(p, &e, ty);
  fclose(f);
  return buf;
}

static void print_declarator(Printer *p, E *e, LibcpType *ty,
                             const LibcpToken *name, LibcpType *stop);

// Where the declspec of a full type starts: the outermost segment of
// the derivation chain a typedef names (PLAN-c2c D5), or the base
// type when none does. `typedef int (*fp)(int); fp f;` prints `fp f`
// because the pointer-to-function segment is fp.
static LibcpType *declspec_start(Printer *p, LibcpType *ty) {
  for (LibcpType *t = ty;;) {
    if (ty_info(p, t)->tname)
      return t;
    switch (t->kind) {
    case TY_PTR:
    case TY_ARRAY:
    case TY_VLA:
      t = t->base;
      continue;
    case TY_FUNC:
      t = t->return_ty;
      continue;
    default:
      return t;
    }
  }
}

// A full type in declaration position: the declspec, then the
// declarator derived above it. `defining` suppresses the typedef
// shortcut, so the record a typedef introduces never spells itself.
static void print_full_type(Printer *p, E *e, LibcpType *ty,
                            const LibcpToken *name, bool defining) {
  LibcpType *start =
      defining ? base_type(ty) : declspec_start(p, ty);
  print_declspec_e(p, e, start, defining);
  if (name || ty != start)
    e_str(e, " ");
  print_declarator(p, e, ty, name, start);
}

// The dimension suffix of one array layer.
static void emit_dim(Printer *p, E *e, LibcpType *ty) {
  LibcpNode *dim = ty->dim_len ? ty->dim_len : ty->vla_len;
  if (dim) {
    e_str(e, "[");
    print_expr_ctx(p, e, dim, 2); // assignment-expression
    e_str(e, "]");
  } else if (ty->array_len > 0) {
    e_fmt(e, "[%d]", ty->array_len);
  } else {
    e_str(e, "[]");
  }
}

static void emit_params(Printer *p, E *e, LibcpType *fn) {
  e_str(e, "(");
  if (!fn->params) {
    // An unprototyped `()` is variadic in chibicc's record; `(void)`
    // is the zero-parameter prototype.
    e_str(e, fn->is_variadic ? "" : "void");
  } else {
    bool first = true;
    for (LibcpType *param = fn->params; param; param = param->next) {
      if (!first)
        e_str(e, ", ");
      first = false;
      print_full_type(p, e, param, param->name, false);
    }
    if (fn->is_variadic)
      e_str(e, ", ...");
  }
  e_str(e, ")");
}

// Builds the declarator text around the name by walking the type
// record's derivation chain outside-in, stopping where the declspec
// takes over (the base type, or a typedef-named segment). A pointer
// layer prepends "*"; a function or array layer appends its suffix,
// wrapping what has accumulated so far in parentheses when a "*" is
// exposed - exactly the C rule that a postfix suffix binds tighter
// than a pointer.
static void decl_chain(Printer *p, LibcpType *ty, SB *sb, LibcpType *stop) {
  if (ty == stop)
    return;
  switch (ty->kind) {
  case TY_PTR:
    sb_prepend_str(sb, "*");
    decl_chain(p, ty->base, sb, stop);
    return;
  case TY_ARRAY:
  case TY_VLA: {
    if (sb->len && sb->s[0] == '*') {
      sb_prepend_str(sb, "(");
      sb_str(sb, ")");
    }
    char *dim = capture(p, emit_dim, ty);
    sb_str(sb, dim);
    free(dim);
    decl_chain(p, ty->base, sb, stop);
    return;
  }
  case TY_FUNC: {
    if (sb->len && sb->s[0] == '*') {
      sb_prepend_str(sb, "(");
      sb_str(sb, ")");
    }
    char *params = capture(p, emit_params, ty);
    sb_str(sb, params);
    free(params);
    decl_chain(p, ty->return_ty, sb, stop);
    return;
  }
  default:
    return;
  }
}

static void print_declarator(Printer *p, E *e, LibcpType *ty,
                             const LibcpToken *name, LibcpType *stop) {
  SB sb = {0};
  if (name && name->len)
    sb_push(&sb, name->loc, name->len);
  decl_chain(p, ty, &sb, stop);
  if (sb.len)
    e_str(e, sb.s);
  free(sb.s);
}

// Storage-class and _Alignas spellings of a declaration record's
// attributes, in a fixed canonical order. ND_TYPEDEF records carry no
// attributes of their own (the parser registers the name instead), so
// the keyword comes from the record's kind.
static void print_attr_prefix(Printer *p, LibcpNode *node) {
  E *e = &p->e;
  LibcpVarAttr *attr = &node->attr;
  if (node->kind == ND_TYPEDEF || attr->is_typedef)
    e_str(e, "typedef ");
  if (attr->is_static)
    e_str(e, "static ");
  if (attr->is_extern)
    e_str(e, "extern ");
  if (attr->is_inline)
    e_str(e, "inline ");
  if (attr->is_tls)
    e_str(e, "_Thread_local ");
  if (attr->align_expr) {
    e_str(e, "_Alignas(");
    print_expr_ctx(p, e, attr->align_expr, 3); // constant-expression
    e_str(e, ") ");
  }
  if (attr->align_ty) {
    e_str(e, "_Alignas(");
    print_full_type(p, e, attr->align_ty, NULL, false);
    e_str(e, ") ");
  }
  if (attr->align) {
    e_fmt(e, "_Alignas(%d) ", attr->align);
  }
}

// The declared type of a declaration record: ND_TYPEDEF parks it on
// `ty`, the others on `ty_op`.
static LibcpType *record_type(LibcpNode *node) {
  return node->kind == ND_TYPEDEF ? node->ty : node->ty_op;
}

static bool same_attr(LibcpVarAttr *a, LibcpVarAttr *b) {
  return a->is_typedef == b->is_typedef && a->is_static == b->is_static &&
         a->is_extern == b->is_extern && a->is_inline == b->is_inline &&
         a->is_tls == b->is_tls && a->align == b->align &&
         a->align_expr == b->align_expr && a->align_ty == b->align_ty;
}

// ---------------------------------------------------------------------------
// Initializer printing: the faithful record as written, designators
// and brace nesting included (PLAN-c2c D3).

static void print_init_list(Printer *p, LibcpInitializer *init) {
  e_str(&p->e, "{");
  bool first = true;
  for (LibcpInitItem *item = init->items; item; item = item->next) {
    if (!first)
      e_str(&p->e, ",");
    e_str(&p->e, " ");
    first = false;
    for (LibcpInitDesig *d = item->desigs; d; d = d->next) {
      if (d->name) {
        e_str(&p->e, ".");
        e_tok(&p->e, d->name);
      } else {
        e_str(&p->e, "[");
        print_expr_ctx(p, &p->e, d->begin, 3); // constant-expression
        if (d->end) {
          e_str(&p->e, " ... ");
          print_expr_ctx(p, &p->e, d->end, 3);
        }
        e_str(&p->e, "]");
      }
      e_str(&p->e, " = ");
    }
    print_init(p, item->init);
  }
  if (!first)
    e_str(&p->e, " ");
  e_str(&p->e, "}");
}

static void print_init(Printer *p, LibcpInitializer *init) {
  if (!init)
    return;
  switch (init->kind) {
  case INIT_EXPR:
    // An initializer is an assignment-expression: a top-level comma
    // would otherwise split into another declarator on re-parse.
    print_expr_ctx(p, &p->e, init->expr, 2);
    return;
  case INIT_STR:
    e_str_literal(&p->e, init->str_tok);
    return;
  case INIT_LIST:
    print_init_list(p, init);
    return;
  default:
    p->errors++;
    fprintf(stderr, "c2c: unknown initializer kind %d\n", init->kind);
    exit(2);
  }
}

// One declarator of a declaration group: name, dims, initializer,
// derived above the group's shared declspec start.
static void print_one_declarator(Printer *p, LibcpNode *node,
                                 LibcpType *start) {
  print_declarator(p, &p->e, record_type(node), record_name_tok(node), start);
  if (node->decl_init) {
    e_str(&p->e, " = ");
    print_init(p, node->decl_init);
  }
}

// True when `node` continues the declaration group of `first`: same
// kind, the same declspec start (the base record, or the typedef-named
// segment both derive above) and the same attributes (PLAN-c2c D3).
// Grouping by the declspec start is what keeps every declarator of one
// anonymous enum or struct definition inside the single statement that
// spells it.
static bool continues_group(Printer *p, LibcpNode *first, LibcpNode *node) {
  return node->kind == first->kind &&
         declspec_start(p, record_type(node)) ==
             declspec_start(p, record_type(first)) &&
         same_attr(&node->attr, &first->attr);
}

static void print_stmt(Printer *p, LibcpNode *node);

// Prints a declaration record (and its declarator group) and returns
// the last chain node it consumed, so the caller steps past the group.
static LibcpNode *print_record(Printer *p, LibcpNode *node) {
  bool defining = node->kind == ND_TYPEDEF;
  print_attr_prefix(p, node);
  LibcpType *start = defining ? base_type(record_type(node))
                              : declspec_start(p, record_type(node));
  print_declspec_e(p, &p->e, start, defining);
  e_str(&p->e, " ");

  LibcpNode *last = node;
  print_one_declarator(p, node, start);

  // Consecutive records over the same declspec start and attributes
  // join into one multi-declarator statement (PLAN-c2c D3); a function
  // definition never joins.
  if (node->kind != ND_FUNCDEF) {
    while (last->next && continues_group(p, node, last->next)) {
      last = last->next;
      e_str(&p->e, ", ");
      print_one_declarator(p, last, start);
    }
  }

  if (node->kind == ND_FUNCDEF && node->body) {
    e_str(&p->e, " ");
    print_stmt(p, node->body);
    return last;
  }
  e_str(&p->e, ";");
  e_nl(&p->e);
  return last;
}

static void print_stmt(Printer *p, LibcpNode *node) {
  (void)node;
  p->errors++;
  fprintf(stderr, "c2c: statements are CC4\n");
  exit(2);
}

// A run of consecutive ND_ENUM_CONST records over one type record:
// either absorbed by the declaration record that follows (the enum
// then prints inline in its declspec) or printed standalone. Returns
// the last record of the run.
static LibcpNode *print_enum_group(Printer *p, LibcpNode *node) {
  LibcpType *ty = node->ty_op;
  LibcpNode *last = node;
  while (last->next && last->next->kind == ND_ENUM_CONST &&
         last->next->ty_op == ty)
    last = last->next;

  LibcpNode *next = last->next;
  if (next && (next->kind == ND_DECL || next->kind == ND_GVAR_DECL ||
               next->kind == ND_FUNCDEF || next->kind == ND_TYPEDEF) &&
      base_type(record_type(next)) == ty)
    return last; // absorbed by `next`'s declspec

  print_enum_spec_e(p, &p->e, ty);
  e_str(&p->e, ";");
  e_nl(&p->e);
  return last;
}

static void add_define(LibcpContext *ctx, char *str) {
  char *eq = strchr(str, '=');
  if (!eq) {
    libcp_context_define(ctx, str, (char *)"1");
    return;
  }
  char *name = strndup(str, eq - str);
  libcp_context_define(ctx, name, eq + 1);
}

static void on_diag(void *user, const LibcpDiagnostic *d) {
  Printer *p = user;
  p->errors++;
  fprintf(stderr, "c2c: %s:%d: %s\n", d->filename ? d->filename : "<c2c>",
          d->line, d->message);
}

int main(int argc, char **argv) {
  const char *path = NULL;

  Printer printer = {{stdout, 0, 1}, {0}, {0}, 0, 0};
  Printer *p = &printer;

  LibcpContext *ctx = libcp_context_new();
  if (!ctx) {
    fprintf(stderr, "c2c: out of memory\n");
    return 1;
  }
  libcp_context_set_diagnostic(ctx, on_diag, p);

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-I") && i + 1 < argc) {
      libcp_context_add_include_path(ctx, argv[++i]);
      continue;
    }
    if (!strncmp(argv[i], "-I", 2)) {
      libcp_context_add_include_path(ctx, argv[i] + 2);
      continue;
    }
    if (!strcmp(argv[i], "-D") && i + 1 < argc) {
      add_define(ctx, argv[++i]);
      continue;
    }
    if (!strncmp(argv[i], "-D", 2)) {
      add_define(ctx, argv[i] + 2);
      continue;
    }
    if (!strcmp(argv[i], "-U") && i + 1 < argc) {
      libcp_context_undef(ctx, argv[++i]);
      continue;
    }
    if (!strncmp(argv[i], "-U", 2)) {
      libcp_context_undef(ctx, argv[i] + 2);
      continue;
    }
    if (argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "c2c: unknown option: %s\n", argv[i]);
      return 1;
    }
    path = argv[i];
  }
  if (!path) {
    fprintf(stderr,
            "usage: c2c [-I dir]... [-D name[=val]]... [-U name]... file.c\n");
    return 1;
  }

  LibcpToken *tok = libcp_tokenize_file(ctx, (char *)path);
  if (!tok || libcp_context_failed(ctx))
    return 1;
  tok = libcp_preprocess(ctx, tok);
  if (!tok || libcp_context_failed(ctx))
    return 1;
  LibcpNode *prog = libcp_parse(ctx, tok);
  if (!prog || libcp_context_failed(ctx))
    return 1;

  collect_exprs(p, prog);

  // Forward declarations for aggregates that stayed incomplete through
  // the whole parse come first (PLAN-c2c D7): a later same-tag
  // definition is then exactly the forward-plus-definition pattern.
  for (int i = 0; i < p->pending_len; i++) {
    LibcpType *ty = p->pending[i];
    TyInfo *info = ty_info(p, ty);
    if (info->forwarded)
      continue;
    info->forwarded = true;
    e_str(&p->e, ty->kind == TY_UNION ? "union " : "struct ");
    e_tok(&p->e, ty->tag_tok);
    e_str(&p->e, ";");
    e_nl(&p->e);
  }

  for (LibcpNode *node = prog; node;) {
    if (node->kind == ND_ENUM_CONST) {
      node = print_enum_group(p, node)->next;
      continue;
    }
    node = print_record(p, node)->next;
  }

  libcp_context_free(ctx);

  if (fflush(stdout) != 0 || p->errors) {
    fprintf(stderr, "c2c: failed to print %s\n", path);
    return 1;
  }
  return 0;
}
