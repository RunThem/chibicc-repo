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
} E;

static void e_putc(E *e, int c) {
  if (e->at_bol) {
    for (int i = 0; i < e->indent; i++)
      fputs("  ", e->out);
    e->at_bol = 0;
  }
  fputc(c, e->out);
}

static void e_write(E *e, const char *s, int len) {
  for (int i = 0; i < len; i++)
    e_putc(e, s[i]);
}

static void e_str(E *e, const char *s) { e_write(e, s, strlen(s)); }

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

// ---------------------------------------------------------------------------
// Expression printing. CC2 covers what a declaration can reach
// (identifiers, numeric literals); the full precedence engine is
// PLAN-c2c CC3.

static void print_expr_e(Printer *p, E *e, LibcpNode *node) {
  if (!node)
    return;
  switch (node->kind) {
  case ND_IDENT:
  case ND_NUM:
    e_tok(e, node->tok);
    return;
  default:
    p->errors++;
    fprintf(stderr, "c2c: expression kind %d not printable yet\n",
            (int)node->kind);
    exit(2);
  }
}

// ---------------------------------------------------------------------------
// Type printing (PLAN-c2c D2, D6-D8).

static void print_declspec_e(Printer *p, E *e, LibcpType *ty, bool defining);
static void print_declarator(Printer *p, E *e, LibcpType *ty,
                             const LibcpToken *name);

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
      print_expr_e(p, e, node->lhs);
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
    print_expr_e(p, e, mem->align_expr);
    e_str(e, ") ");
  }
  if (mem->align_ty) {
    e_str(e, "_Alignas(");
    print_declspec_e(p, e, base_type(mem->align_ty), false);
    print_declarator(p, e, mem->align_ty, NULL);
    e_str(e, ") ");
  }
  print_declspec_e(p, e, base_type(mem->ty), false);
  if (mem->name || mem->ty->kind != base_type(mem->ty)->kind)
    e_str(e, " ");
  print_declarator(p, e, mem->ty, mem->name);
  if (mem->width_expr) {
    e_str(e, " : ");
    print_expr_e(p, e, mem->width_expr);
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
    print_expr_e(p, e, ty->align_expr);
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
                             const LibcpToken *name);

// The dimension suffix of one array layer.
static void emit_dim(Printer *p, E *e, LibcpType *ty) {
  LibcpNode *dim = ty->dim_len ? ty->dim_len : ty->vla_len;
  if (dim) {
    e_str(e, "[");
    print_expr_e(p, e, dim);
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
      print_declspec_e(p, e, base_type(param), false);
      // A parameter always has a derivation or a name; an abstract
      // bare `int` takes no trailing space.
      if (param->name || param->kind != base_type(param)->kind)
        e_str(e, " ");
      print_declarator(p, e, param, param->name);
    }
    if (fn->is_variadic)
      e_str(e, ", ...");
  }
  e_str(e, ")");
}

// Builds the declarator text around the name by walking the type
// record's derivation chain outside-in. A pointer layer prepends "*";
// a function or array layer appends its suffix, wrapping what has
// accumulated so far in parentheses when a "*" is exposed - exactly
// the C rule that a postfix suffix binds tighter than a pointer.
static void decl_chain(Printer *p, LibcpType *ty, SB *sb) {
  switch (ty->kind) {
  case TY_PTR:
    sb_prepend_str(sb, "*");
    decl_chain(p, ty->base, sb);
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
    decl_chain(p, ty->base, sb);
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
    decl_chain(p, ty->return_ty, sb);
    return;
  }
  default:
    return;
  }
}

static void print_declarator(Printer *p, E *e, LibcpType *ty,
                             const LibcpToken *name) {
  SB sb = {0};
  if (name && name->len)
    sb_push(&sb, name->loc, name->len);
  decl_chain(p, ty, &sb);
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
    print_expr_e(p, e, attr->align_expr);
    e_str(e, ") ");
  }
  if (attr->align_ty) {
    e_str(e, "_Alignas(");
    print_declspec_e(p, e, base_type(attr->align_ty), false);
    print_declarator(p, e, attr->align_ty, NULL);
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
// Initializer and statement printing. Lists and statements are
// PLAN-c2c CC4.

static void print_init(Printer *p, LibcpInitializer *init) {
  if (!init)
    return;
  switch (init->kind) {
  case INIT_EXPR:
    print_expr_e(p, &p->e, init->expr);
    return;
  case INIT_STR:
    e_tok(&p->e, init->str_tok);
    return;
  default:
    p->errors++;
    fprintf(stderr, "c2c: initializer lists are CC4\n");
    exit(2);
  }
}

// One declarator of a declaration group: name, dims, initializer.
static void print_one_declarator(Printer *p, LibcpNode *node) {
  print_declarator(p, &p->e, record_type(node), record_name_tok(node));
  if (node->decl_init) {
    e_str(&p->e, " = ");
    print_init(p, node->decl_init);
  }
}

// True when `node` continues the declaration group of `first`: same
// kind, same declspec record (the base the declarators derive from)
// and same attributes (PLAN-c2c D3). Grouping by the base record is
// what keeps every declarator of one anonymous enum or struct
// definition inside the single statement that spells it.
static bool continues_group(LibcpNode *first, LibcpNode *node) {
  return node->kind == first->kind &&
         base_type(record_type(node)) == base_type(record_type(first)) &&
         same_attr(&node->attr, &first->attr);
}

static void print_stmt(Printer *p, LibcpNode *node);

// Prints a declaration record (and its declarator group) and returns
// the last chain node it consumed, so the caller steps past the group.
static LibcpNode *print_record(Printer *p, LibcpNode *node) {
  print_attr_prefix(p, node);
  print_declspec_e(p, &p->e, base_type(record_type(node)),
                   node->kind == ND_TYPEDEF);
  e_str(&p->e, " ");

  LibcpNode *last = node;
  print_one_declarator(p, node);

  // Consecutive records over the same type record and attributes join
  // into one multi-declarator statement (PLAN-c2c D3); a function
  // definition never joins.
  if (node->kind != ND_FUNCDEF) {
    while (last->next && continues_group(node, last->next)) {
      last = last->next;
      e_str(&p->e, ", ");
      print_one_declarator(p, last);
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
