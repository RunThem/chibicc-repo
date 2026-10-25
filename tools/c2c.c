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

// open_memstream and strndup are POSIX; say so before any include, so
// the tool also builds under glibc's strict -std=c11.
#define _POSIX_C_SOURCE 200809L

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
  bool tname_emitted;   // the typedef statement spelling it has printed
  EnumRec *enums;       // definition records in source order
  EnumRec *enums_tail;
  bool emitted;         // definition already printed
  bool forwarded;       // forward declaration already printed
  bool collected;       // pre-pass aggregate cycle guard
  // The chain position of an enum's ND_ENUM_CONST run - where the
  // definition must spell itself so the re-parsed tree carries the
  // constants at the same chain positions as the original.
  int consts_idx;
  // The position of the first top-level chain node whose walk reaches
  // this record, counted in chain nodes from the start of the
  // translation unit. The definition re-establishes as a file-scope
  // statement before the print reaches that position - or right after
  // it, if a declaration group swallowed the anchor node (still ahead
  // of every later use).
  int anchor_idx;
} TyInfo;

#define TY_BUCKETS 509

typedef struct {
  E e;
  TyInfo *buckets[TY_BUCKETS];
  LibcpType *pending[64]; // incomplete aggregates, file-top forward decls
  int pending_len;
  // Aggregate definitions to re-establish as file-scope statements
  // ahead of their first user (a standalone `struct T { ... };` leaves
  // no chain node, so without this the definition would print inline
  // wherever the record is first reached - possibly inside a
  // parameter list or a sizeof, the wrong scope on re-parse).
  LibcpType *flushable[128];
  int flushable_len;
  int item_idx;       // pre-pass: 1-based position of the node being walked
  int cur_idx;        // print: chain nodes consumed so far
  int body_depth;     // >0 while the pre-pass walks inside a function body
  int atomic_depth;   // >0 while spelling the inside of _Atomic(...)
  LibcpType *flush_ty; // the record the flush loop is spelling right now
  bool flushing;      // inside the flush loop's own spec print
  // The aggregate whose definition is being printed right now. Its
  // typedef name is not yet visible where it is being defined (a
  // self-referential member spells `struct T`, never the typedef), so
  // declspec_start skips the typedef shortcut for these records.
  LibcpType *defining[16];
  int defining_len;
  int errors;
} Printer;

// The record a type's facts live on: libcp_copy_type shallow-copies a
// record for function parameters and qualifier application (the copy
// carries `origin`), and the copy would otherwise be a distinct side-
// table key from the record the specifier text parsed - a definition
// could then print twice (once for the original, once for the copy).
static LibcpType *origin_root(LibcpType *ty) {
  while (ty->origin)
    ty = ty->origin;
  return ty;
}

static TyInfo *ty_info(Printer *p, LibcpType *ty) {
  ty = origin_root(ty);
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
    if (info->consts_idx == 0)
      info->consts_idx = p->item_idx;
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

  if (node->kind == ND_FUNCDEF && node->body)
    p->body_depth++;
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
  if (node->kind == ND_FUNCDEF && node->body)
    p->body_depth--;
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

  // A tagged, complete record re-establishes itself as a file-scope
  // statement before its first user (see the Printer comment) - unless
  // another definition of the same tag exists, in which case hoisting
  // both would collide on re-parse and each prints inline instead.
  if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->tag_tok &&
      info->anchor_idx == 0 && p->body_depth == 0 &&
      p->flushable_len < 128) {
    bool shadowed = false;
    for (int i = 0; i < p->flushable_len; i++)
      if (p->flushable[i]->tag_tok->len == ty->tag_tok->len &&
          !strncmp(p->flushable[i]->tag_tok->loc, ty->tag_tok->loc,
                   ty->tag_tok->len))
        shadowed = true;
    if (!shadowed) {
      info->anchor_idx = p->item_idx;
      p->flushable[p->flushable_len++] = ty;
    }
  }

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
  // The prefix is recovered from the element type: L widens to signed
  // int, U to unsigned int - same size, different signedness.
  if (esz == 4)
    e_str(e, base && base->is_unsigned ? "U" : "L");
  else if (esz == 2)
    e_str(e, base && base->is_unsigned ? "u" : "L");
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
    if (esz == 1 && v >= 0x20 && v != '"' && v != '\\') {
      // Bytes >= 0x80 are UTF-8 continuation data - emitted raw, the
      // same bytes the original spelling contributed.
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
static void print_chain(Printer *p, LibcpNode *node);

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
      // A postfix on a unary operand parenthesizes: (*p)-- must not
      // re-lex as *p-- (the postfix binds the p).
      print_expr_ctx(p, e, node->lhs, 16);
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
    // A cast or compound-literal operand has to parenthesize: glued,
    // `sizeof (char)1` would read as the sizeof(type) form.
    if (node->lhs->kind == ND_CAST || node->lhs->kind == ND_COMPOUND_LITERAL) {
      e_str(e, "(");
      print_expr_ctx(p, e, node->lhs, 1);
      e_str(e, ")");
    } else {
      print_expr_ctx(p, e, node->lhs, 15);
    }
    return;

  case ND_ALIGNOF:
    if (node->ty_op) {
      e_str(e, "_Alignof(");
      print_typename_e(p, e, node->ty_op);
      e_str(e, ")");
      return;
    }
    e_str(e, "_Alignof ");
    if (node->lhs->kind == ND_CAST || node->lhs->kind == ND_COMPOUND_LITERAL) {
      e_str(e, "(");
      print_expr_ctx(p, e, node->lhs, 1);
      e_str(e, ")");
    } else {
      print_expr_ctx(p, e, node->lhs, 15);
    }
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
    e_str(e, "({");
    if (node->body) {
      e_nl(e);
      e->indent++;
      print_chain(p, node->body);
      e->indent--;
    } else {
      e_str(e, " ");
    }
    e_str(e, "})");
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

  info->emitted = true;

  // The definition's records regroup here (PLAN-c2c D6); an enum spec
  // without records (`enum E { }`) spells its empty braces.
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

// Whether the definition of `ty` can spell its members with what has
// been emitted so far: every typedef name a (transitive) member spells
// must have had its statement printed already, and a nested aggregate
// member that would spell inline must be flushable itself. Derivation
// layers (pointers, arrays, functions) are walked through to the base
// the declspec actually spells.
static bool type_feasible(Printer *p, LibcpType *t, int depth);

static bool flush_feasible(Printer *p, LibcpType *ty, int depth) {
  if (depth > 16)
    return true;
  for (LibcpMember *mem = ty->members; mem; mem = mem->next) {
    if (!type_feasible(p, mem->ty, depth))
      return false;
    if (mem->align_ty && !type_feasible(p, mem->align_ty, depth))
      return false;
  }
  return true;
}

static bool type_feasible(Printer *p, LibcpType *t, int depth) {
  for (; t;) {
    // Self-referential members (a linked node's next) make the naive
    // walk exponential; a record already on the current path is
    // feasible iff the rest of its subtree is, and its subtree is
    // being spelled by the outer frame - treat it as feasible.
    static LibcpType *seen[256];
    static int seen_len;
    if (depth == 0)
      seen_len = 0;
    for (int i = 0; i < seen_len; i++)
      if (seen[i] == t)
        return true;
    if (seen_len < 256)
      seen[seen_len++] = t;
    TyInfo *info = ty_info(p, t);
    if (info->tname && !info->tname_emitted)
      return false;
    if ((t->kind == TY_STRUCT || t->kind == TY_UNION) && t->members &&
        !info->emitted) {
      // A pending tagged definition spells tag-only here and
      // re-establishes at its own anchor; an anonymous one spells
      // inline right here. Either way its own members must be
      // spellable now.
      if (!flush_feasible(p, t, depth + 1))
        return false;
    }
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
      t = NULL;
    }
  }
  return true;
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

  // A definition with a pending flush prints tag-only everywhere
  // outside the flush itself: the definition re-establishes at its
  // anchor, and spelling it here would hoist it into whatever context
  // reached it first - a parameter list, a sizeof, a forward typedef.
  if (info->anchor_idx && (!p->flushing || ty != p->flush_ty))
    return;

  if (!ty->layout_pending)
    // Incomplete through the whole parse (PLAN-c2c D7): a forward
    // declaration was arranged at the top of the file; the spec itself
    // stays tag-only.
    return;

  info->emitted = true;
  e_str(e, " {");
  if (p->defining_len < 16)
    p->defining[p->defining_len++] = ty;
  for (LibcpMember *mem = ty->members; mem; mem = mem->next) {
    e_nl(e);
    e->indent++;
    print_member_e(p, e, mem);
    e->indent--;
  }
  p->defining_len--;
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
  if (!defining && info->tname && info->tname_emitted) {
    e_str(e, info->tname);
    return;
  }

  // The plain `_Atomic ` prefix is for base-kind records only; a
  // derived record spells in the parenthesized form below.
  if (ty->is_atomic &&
      !(p->atomic_depth == 0 &&
        (ty->kind == TY_PTR || ty->kind == TY_ARRAY || ty->kind == TY_VLA ||
         ty->kind == TY_FUNC)))
    e_str(e, "_Atomic ");

  // An _Atomic on a derived record spells in parenthesized form:
  // `_Atomic(int *) p`, never `int * _Atomic p` (chibicc reads only
  // the declspec form back).
  if (ty->is_atomic && p->atomic_depth == 0 &&
      (ty->kind == TY_PTR || ty->kind == TY_ARRAY || ty->kind == TY_VLA ||
       ty->kind == TY_FUNC)) {
    p->atomic_depth++;
    e_str(e, "_Atomic(");
    print_full_type(p, e, ty, NULL, false);
    e_str(e, ")");
    p->atomic_depth--;
    return;
  }

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
  if (sb->len)
    memmove(sb->s + n, sb->s, sb->len);
  memcpy(sb->s, s, n);
  sb->len += n;
  sb->s[sb->len] = '\0';
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
static bool in_definition(Printer *p, LibcpType *ty);

// Where the declspec of a full type starts: the outermost segment of
// the derivation chain a typedef names (PLAN-c2c D5), or the base
// type when none does. `typedef int (*fp)(int); fp f;` prints `fp f`
// because the pointer-to-function segment is fp. `avoid` suppresses
// the shortcut at one exact record - the record a typedef is itself
// naming, whose declaration must spell the type it introduces.
static LibcpType *declspec_start(Printer *p, LibcpType *ty) {
  for (LibcpType *t = ty;;) {
    TyInfo *info = ty_info(p, t);
    if (info->tname && info->tname_emitted && !in_definition(p, t))
      return t;
    // An _Atomic qualifier lives on one record of the chain and prints
    // in the declspec, so the walk stops there (`_Atomic(int *) z`).
    if (t->is_atomic && p->atomic_depth == 0)
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
// declarator derived above it.
static void print_full_type(Printer *p, E *e, LibcpType *ty,
                            const LibcpToken *name, bool defining) {
  LibcpType *start = defining ? base_type(ty) : declspec_start(p, ty);
  // A record whose definition is being printed spells itself by tag,
  // never by the typedef name it is about to introduce.
  print_declspec_e(p, e, start, defining || in_definition(p, start));
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
    bool has_desig = false;
    for (LibcpInitDesig *d = item->desigs; d; d = d->next) {
      has_desig = true;
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
    }
    if (has_desig)
      e_str(&p->e, " = ");
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
  // The typedef name becomes usable for later declarations only now
  // that the statement spelling it has printed - including the second
  // and later declarators of a grouped typedef statement.
  if (node->kind == ND_TYPEDEF)
    ty_info(p, record_type(node))->tname_emitted = true;
  if (node->decl_init) {
    e_str(&p->e, " = ");
    print_init(p, node->decl_init);
  }
}

// The declspec start a record's declaration will actually print with:
// a typedef suppresses the typedef-name shortcut on the record it is
// itself naming (see print_decl_head).
static bool in_definition(Printer *p, LibcpType *ty) {
  for (int i = 0; i < p->defining_len; i++)
    if (p->defining[i] == ty)
      return true;
  return false;
}

static LibcpType *print_start(Printer *p, LibcpNode *node) {
  return declspec_start(p, record_type(node));
}

// True when `node` continues the declaration group of `first`: same
// kind, same printed declspec start and same attributes (PLAN-c2c
// D3). Grouping by the declspec start is what keeps every declarator
// of one anonymous enum or struct definition inside the single
// statement that spells it - and only groups that share one declspec
// text round-trip: two typedefs never join, because each suppresses
// the name shortcut at its own record (c35).
static bool continues_group(Printer *p, LibcpNode *first, LibcpNode *node) {
  // Typedef records never join: each of a group's declarators parses
  // its own derivation of the shared declspec, so which records a
  // multi-name statement holds differs between parse passes and the
  // printed form would not stabilize.
  if (node->kind == ND_TYPEDEF)
    return false;
  bool r = node->kind == first->kind &&
           print_start(p, node) == print_start(p, first) &&
           same_attr(&node->attr, &first->attr);
  if (getenv("C2C_DBG6") && first->name_tok && node->name_tok &&
      (strncmp(first->name_tok->loc, "__locale_t", 10) == 0 ||
       strncmp(first->name_tok->loc, "locale_t", 8) == 0))
    fprintf(stderr, "DBG group %.*s + %.*s -> %d\n",
            first->name_tok->len, first->name_tok->loc,
            node->name_tok->len, node->name_tok->loc, r);
  return r;
}

static void print_stmt(Printer *p, LibcpNode *node);

// The declaration's head - storage class, declspec, and the
// declarator group - without any terminator; returns the last chain
// node of the group. A for-loop's init declaration reuses this.
static LibcpNode *print_decl_head(Printer *p, LibcpNode *node) {
  print_attr_prefix(p, node);
  // A typedef suppresses the typedef-name shortcut only on the record
  // it is itself naming: `typedef struct { int x; } P;` must spell the
  // struct, while `typedef __va_elem va_list[1];` keeps __va_elem.
  // The declspec prints structurally exactly when the suppressed
  // record is where the declspec itself starts.
  LibcpType *start = print_start(p, node);
  bool defining =
      (node->kind == ND_TYPEDEF && ty_info(p, start)->tname == NULL) ||
      in_definition(p, start);
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
  return last;
}

// Prints a declaration record (and its declarator group) and returns
// the last chain node it consumed, so the caller steps past the group.
static LibcpNode *print_record(Printer *p, LibcpNode *node) {
  LibcpNode *last = print_decl_head(p, node);

  if (node->kind == ND_FUNCDEF && node->body) {
    e_str(&p->e, " ");
    print_stmt(p, node->body);
    e_nl(&p->e);
    return last;
  }
  e_str(&p->e, ";");
  e_nl(&p->e);
  return last;
}

static void print_stmt(Printer *p, LibcpNode *node);
static void print_chain(Printer *p, LibcpNode *node);
static LibcpNode *print_top_item(Printer *p, LibcpNode *node);
static LibcpNode *print_decl_head(Printer *p, LibcpNode *node);
static LibcpNode *print_record(Printer *p, LibcpNode *node);
static LibcpNode *print_enum_group(Printer *p, LibcpNode *node);

// An escaped C string from raw contents: asm statements carry their
// text decoded, so it is re-escaped here with the char-string rules.
static void e_escaped_c_str(E *e, const char *s) {
  e_str(e, "\"");
  for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
    switch (*c) {
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
    default:
      if (*c < 0x20 || *c >= 0x7f)
        e_fmt(e, "\\%03o", *c);
      else
        e_putc(e, *c);
    }
  }
  e_str(e, "\"");
}

// A block: `{\n ...\n}`; an empty compound prints `{ }` on one line.
// The scopeless block is the empty statement and prints `;` - braces
// around it would change the tree.
static void print_block(Printer *p, LibcpNode *node) {
  if (!node->is_scope_block) {
    if (!node->body) {
      e_str(&p->e, ";");
      return;
    }
    // Defensive: only sema's folded chains reach here, never a parse
    // tree; print them as a plain block so nothing is lost.
  }
  if (!node->body) {
    e_str(&p->e, "{ }");
    return;
  }
  e_str(&p->e, "{");
  e_nl(&p->e);
  p->e.indent++;
  print_chain(p, node->body);
  p->e.indent--;
  e_str(&p->e, "}");
}

// A statement nested under a head (if, else, while, for, do, switch,
// case, label): a block shares the head's line, any other statement
// moves to the next line one indent deeper. No braces are added - they
// would grow an ND_BLOCK and change the tree (PLAN-c2c D3).
static void print_substmt(Printer *p, LibcpNode *node) {
  if (node && node->kind == ND_BLOCK && node->is_scope_block) {
    e_str(&p->e, " ");
    print_block(p, node);
    return;
  }
  e_nl(&p->e);
  p->e.indent++;
  print_stmt(p, node);
  p->e.indent--;
}

static void print_stmt(Printer *p, LibcpNode *node) {
  if (!node)
    return;

  switch (node->kind) {
  case ND_BLOCK:
    print_block(p, node);
    return;

  case ND_EXPR_STMT:
    print_expr_ctx(p, &p->e, node->lhs, 1);
    e_str(&p->e, ";");
    return;

  case ND_RETURN:
    e_str(&p->e, "return");
    if (node->lhs) {
      e_str(&p->e, " ");
      print_expr_ctx(p, &p->e, node->lhs, 1);
    }
    e_str(&p->e, ";");
    return;

  case ND_IF:
    e_str(&p->e, "if (");
    print_expr_ctx(p, &p->e, node->cond, 1);
    e_str(&p->e, ")");
    if (node->els && node->then && node->then->kind == ND_IF &&
        !node->then->els) {
      // A parsed tree never holds this shape (the else would have
      // bound to the inner if); braced so an else can never rebind.
      e_str(&p->e, " { ");
      print_stmt(p, node->then);
      e_str(&p->e, " }");
    } else {
      print_substmt(p, node->then);
    }
    if (node->els) {
      if (p->e.last == '}') {
        // A braced then-part keeps its else on the same line.
        e_str(&p->e, " else");
      } else {
        e_nl(&p->e);
        e_str(&p->e, "else");
      }
      if (node->els->kind == ND_IF) {
        e_str(&p->e, " ");
        print_stmt(p, node->els);
      } else {
        print_substmt(p, node->els);
      }
    }
    return;

  case ND_FOR: {
    e_str(&p->e, "for (");
    // The init is a chain: enum-definition records may precede the
    // declaration record (`for (enum { E = 2 } i = 0; ...)`), and an
    // expression statement carries its expression in lhs. No
    // terminators here - the `;`s are the for-head's own.
    for (LibcpNode *n = node->init; n;) {
      if (n->kind == ND_ENUM_CONST) {
        n = print_enum_group(p, n)->next;
        continue;
      }
      if (n->kind == ND_DECL) {
        n = print_decl_head(p, n)->next;
        continue;
      }
      print_expr_ctx(p, &p->e, n->lhs, 1);
      n = n->next;
    }
    e_str(&p->e, ";");
    if (node->cond) {
      e_str(&p->e, " ");
      print_expr_ctx(p, &p->e, node->cond, 1);
    }
    e_str(&p->e, ";");
    if (node->inc) {
      e_str(&p->e, " ");
      print_expr_ctx(p, &p->e, node->inc, 1);
    }
    e_str(&p->e, ")");
    print_substmt(p, node->then);
    return;
  }

  case ND_WHILE:
    e_str(&p->e, "while (");
    print_expr_ctx(p, &p->e, node->cond, 1);
    e_str(&p->e, ")");
    print_substmt(p, node->then);
    return;

  case ND_DO:
    e_str(&p->e, "do");
    print_substmt(p, node->then);
    if (p->e.last == '}')
      e_str(&p->e, " while (");
    else {
      e_nl(&p->e);
      e_str(&p->e, "while (");
    }
    print_expr_ctx(p, &p->e, node->cond, 1);
    e_str(&p->e, ");");
    return;

  case ND_SWITCH:
    e_str(&p->e, "switch (");
    print_expr_ctx(p, &p->e, node->cond, 1);
    e_str(&p->e, ")");
    print_substmt(p, node->then);
    return;

  case ND_CASE:
    if (node->is_default) {
      e_str(&p->e, "default:");
    } else {
      e_str(&p->e, "case ");
      print_expr_ctx(p, &p->e, node->begin_expr, 3); // constant-expression
      if (node->end_expr) {
        e_str(&p->e, " ... ");
        print_expr_ctx(p, &p->e, node->end_expr, 3);
      }
      e_str(&p->e, ":");
    }
    print_substmt(p, node->lhs);
    return;

  case ND_LABEL:
    e_str(&p->e, node->label);
    e_str(&p->e, ":");
    print_substmt(p, node->lhs);
    return;

  case ND_GOTO:
    e_str(&p->e, "goto ");
    e_str(&p->e, node->label);
    e_str(&p->e, ";");
    return;

  case ND_GOTO_EXPR:
    e_str(&p->e, "goto *");
    print_expr_ctx(p, &p->e, node->lhs, 15);
    e_str(&p->e, ";");
    return;

  case ND_BREAK:
    e_str(&p->e, "break;");
    return;

  case ND_CONTINUE:
    e_str(&p->e, "continue;");
    return;

  case ND_ASM:
    // No `;` here: chibicc's asm statement does not consume its own
    // terminator - the empty statement that follows it in the chain
    // carries the `;`, and print_chain merges the two into one line.
    e_str(&p->e, "asm(");
    e_escaped_c_str(&p->e, node->asm_str);
    e_str(&p->e, ")");
    return;

  case ND_DECL:
  case ND_GVAR_DECL:
  case ND_TYPEDEF:
  case ND_FUNCDEF:
    // Declaration records reach the chain printer, never a statement
    // slot; print defensively as one anyway.
    print_record(p, node);
    return;

  default:
    p->errors++;
    fprintf(stderr, "c2c: unprintable statement kind %d\n", (int)node->kind);
    exit(2);
  }
}

// The chain printer shared by file scope, block bodies, function
// bodies and statement expressions: declaration records (grouped into
// multi-declarator statements), enum-definition record runs, and
// statements. Statements that follow a case label stay one indent
// deeper until the next label, the layout every C printer uses.
// One item of a statement chain: an enum-definition record run, a
// declaration record (grouped), or a statement. Returns the last
// chain node it consumed.
static LibcpNode *print_top_item(Printer *p, LibcpNode *node) {
  if (node->kind == ND_ENUM_CONST)
    return print_enum_group(p, node);
  if (node->kind == ND_DECL || node->kind == ND_GVAR_DECL ||
      node->kind == ND_TYPEDEF || node->kind == ND_FUNCDEF)
    return print_record(p, node);
  print_stmt(p, node);
  e_nl(&p->e);
  return node;
}

static void print_chain(Printer *p, LibcpNode *node) {
  int case_lift = 0;
  for (; node;) {
    if (node->kind == ND_CASE) {
      while (case_lift) {
        p->e.indent--;
        case_lift--;
      }
      print_stmt(p, node);
      e_nl(&p->e);
      node = node->next;
      p->e.indent++;
      case_lift++;
      continue;
    }
    // An asm statement's `;` is the empty statement that follows it;
    // the two merge into one line (see ND_ASM).
    if (node->kind == ND_ASM && node->next &&
        node->next->kind == ND_BLOCK && !node->next->is_scope_block &&
        !node->next->body) {
      print_stmt(p, node);
      e_str(&p->e, ";");
      e_nl(&p->e);
      node = node->next->next;
      continue;
    }
    LibcpNode *last = print_top_item(p, node);
    for (LibcpNode *n = node;; n = n->next) {
      p->cur_idx++;
      if (n == last)
        break;
    }
    node = last->next;
  }
  while (case_lift) {
    p->e.indent--;
    case_lift--;
  }
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

  // The definition may already have spelled itself inside a flushed
  // host that reached it first (its constants then sit at that host's
  // position, matching the original tree); the run has nothing left
  // to say.
  if (ty_info(p, ty)->emitted)
    return last;
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
  // Warnings are advisory: the printed translation unit is still
  // well-formed (an ignored attribute, say), so only errors fail.
  if (d->severity == LIBCP_DIAG_ERROR)
    p->errors++;
  fprintf(stderr, "c2c: %s:%d: %s\n", d->filename ? d->filename : "<c2c>",
          d->line, d->message);
}

int main(int argc, char **argv) {
  const char *path = NULL;

  Printer printer = {{stdout, 0, 1}, {0}, {0}, 0, {0}, 0, 0};
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
  // An empty translation unit (a header with only directives) parses
  // to a NULL chain - that is success, not failure.
  if (libcp_context_failed(ctx))
    return 1;

  int idx = 0;
  for (LibcpNode *item = prog; item; item = item->next) {
    p->item_idx = ++idx;
    collect_node(p, item);
  }

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
    // Standalone tag definitions the source had at file scope reprint
    // ahead of the top-level item that first reaches them (their
    // anchor node), so the tag lands in file scope, not inside a
    // parameter list or a sizeof.
    p->flushing = true;
    for (int i = 0; i < p->flushable_len; i++) {
      LibcpType *ty = p->flushable[i];
      TyInfo *info = ty_info(p, ty);
      // A record that stayed incomplete through the whole parse has
      // no definition to re-establish; its file-top forward
      // declaration (printed below) covers it.
      if (!ty->members)
        continue;
      if (info->emitted || info->anchor_idx > p->cur_idx + 1)
        continue;
      // A definition may only print once every typedef name its
      // members spell has had its statement printed; otherwise the
      // spelling falls back to structural forms that swallow the
      // definitions the later typedef statements would introduce.
      if (!flush_feasible(p, ty, 0))
        continue;
      p->flush_ty = ty;
      print_struct_spec_e(p, &p->e, ty);
      e_str(&p->e, ";");
      e_nl(&p->e);
    }
    p->flushing = false;
    LibcpNode *last = print_top_item(p, node);
    for (LibcpNode *n = node;; n = n->next) {
      p->cur_idx++;
      if (n == last)
        break;
    }
    node = last->next;
  }

  libcp_context_free(ctx);

  if (fflush(stdout) != 0 || p->errors) {
    fprintf(stderr, "c2c: failed to print %s\n", path);
    return 1;
  }
  return 0;
}
