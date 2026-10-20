// This file implements the C preprocessor.
//
// The preprocessor takes a list of tokens as an input and returns a
// new list of tokens as an output.
//
// The preprocessing language is designed in such a way that that's
// guaranteed to stop even if there is a recursive macro.
// Informally speaking, a macro is applied only once for each token.
// That is, if a macro token T appears in a result of direct or
// indirect macro expansion of T, T won't be expanded any further.
// For example, if T is defined as U, and U is defined as T, then
// token T is expanded to U and then to T and the macro expansion
// stops at that point.
//
// To achieve the above behavior, we attach for each token a set of
// macro names from which the token is expanded. The set is called
// "hideset". Hideset is initially empty, and every time we expand a
// macro, the macro name is added to the resulting tokens' hidesets.
//
// The above macro expansion algorithm is explained in this document
// written by Dave Prossor, which is used as a basis for the
// standard's wording:
// https://github.com/rui314/chibicc/wiki/cpp.algo.pdf

#include "chibicc.h"

typedef struct MacroParam MacroParam;
struct MacroParam {
  MacroParam *next;
  char *name;
};

typedef struct MacroArg MacroArg;
struct MacroArg {
  MacroArg *next;
  char *name;
  bool is_va_args;
  int index;      // 0-based position in the actual argument list
  Token *tok;
};

typedef Token *macro_handler_fn(Token *);

typedef struct Macro Macro;
struct Macro {
  char *name;
  bool is_objlike; // Object-like or function-like
  MacroParam *params;
  char *va_args_name;
  Token *body;
  Token *def;      // the name token of the #define; NULL for -D and builtins
  macro_handler_fn *handler;
};

// Provenance records (PLAN.md): one MacroExpansion per expansion, built
// once and shared read-only by every token that expansion produced; one
// MacroProv per token and layer, prepended so a chain reads innermost
// first. Both are arena objects that live as long as the tokens. The
// types are private; chibicc.h exposes accessors over them.
struct chibicc_macro_expansion {
  Token *name;      // macro name token at the call site
  Token *rparen;    // ')' of a function-like call; NULL otherwise
  Macro *macro;     // definition in force at expansion time
  MacroArg *args;   // actual arguments; NULL for object-like and builtins
  int arg_count;
  bool is_builtin;  // produced by a handler (__LINE__ and friends)
};

struct chibicc_macro_prov {
  MacroExpansion *exp;
  int arg_index;    // -1: macro body; >= 0: index of the argument
  MacroProv *next;  // the context the invocation itself appeared in
};

// `#if` can be nested, so we use a stack to manage nested `#if`s.
typedef struct CondIncl CondIncl;
struct CondIncl {
  CondIncl *next;
  enum { IN_THEN, IN_ELIF, IN_ELSE } ctx;
  Token *tok;
  bool included;
};

struct chibicc_hideset {
  Hideset *next;
  char *name;
};

// The macro table, conditional stack and pragma-once set live in
// ChibiccContext (PLAN L2.3). The preprocessor reads them through the
// active context like every other phase.
#define macros (chibicc_ctx()->macros)
#define cond_incl (chibicc_ctx()->cond_incl)
#define pragma_once (chibicc_ctx()->pragma_once)

static Token *preprocess2(Token *tok);
static Macro *find_macro(Token *tok);

static bool is_hash(Token *tok) {
  return tok->at_bol && equal(tok, "#");
}

// Some preprocessor directives such as #include allow extraneous
// tokens before newline. This function skips such tokens.
static Token *skip_line(Token *tok) {
  if (tok->at_bol)
    return tok;
  warn_tok(tok, "extra token");
  while (tok->at_bol)
    tok = tok->next;
  return tok;
}

static Token *copy_token(Token *tok) {
  Token *t = chibicc_arena_calloc(1, sizeof(Token));
  *t = *tok;
  t->next = NULL;
  return t;
}

static Token *new_eof(Token *tok) {
  Token *t = copy_token(tok);
  t->kind = TK_EOF;
  t->len = 0;
  t->prov = NULL;
  return t;
}

// Prepend one provenance layer. The tail is the chain the invocation's
// own text already carried, so chains are built by prepending only and
// shared tails are never mutated (PLAN.md, "挂接规则").
static MacroProv *new_prov(MacroExpansion *exp, int arg_index, MacroProv *next) {
  MacroProv *p = chibicc_arena_calloc(1, sizeof(MacroProv));
  p->exp = exp;
  p->arg_index = arg_index;
  p->next = next;
  return p;
}

// Build the record of one expansion. It is immutable once returned; every
// token the expansion produces shares it.
static MacroExpansion *
new_expansion(Token *name, Token *rparen, Macro *macro, MacroArg *args,
              int arg_count, bool is_builtin) {
  MacroExpansion *e = chibicc_arena_calloc(1, sizeof(MacroExpansion));
  e->name = name;
  e->rparen = rparen;
  e->macro = macro;
  e->args = args;
  e->arg_count = arg_count;
  e->is_builtin = is_builtin;
  return e;
}

static int count_macro_args(MacroArg *args) {
  int n = 0;
  for (; args; args = args->next)
    n++;
  return n;
}

static Hideset *new_hideset(char *name) {
  Hideset *hs = chibicc_arena_calloc(1, sizeof(Hideset));
  hs->name = name;
  return hs;
}

static Hideset *hideset_union(Hideset *hs1, Hideset *hs2) {
  Hideset head = {};
  Hideset *cur = &head;

  for (; hs1; hs1 = hs1->next)
    cur = cur->next = new_hideset(hs1->name);
  cur->next = hs2;
  return head.next;
}

static bool hideset_contains(Hideset *hs, char *s, int len) {
  for (; hs; hs = hs->next)
    if (strlen(hs->name) == len && !strncmp(hs->name, s, len))
      return true;
  return false;
}

static Hideset *hideset_intersection(Hideset *hs1, Hideset *hs2) {
  Hideset head = {};
  Hideset *cur = &head;

  for (; hs1; hs1 = hs1->next)
    if (hideset_contains(hs2, hs1->name, strlen(hs1->name)))
      cur = cur->next = new_hideset(hs1->name);
  return head.next;
}

static Token *add_hideset(Token *tok, Hideset *hs) {
  Token head = {};
  Token *cur = &head;

  for (; tok; tok = tok->next) {
    Token *t = copy_token(tok);
    t->hideset = hideset_union(t->hideset, hs);
    cur = cur->next = t;
  }
  return head.next;
}

// Append tok2 to the end of tok1.
static Token *append(Token *tok1, Token *tok2) {
  if (tok1->kind == TK_EOF)
    return tok2;

  Token head = {};
  Token *cur = &head;

  for (; tok1->kind != TK_EOF; tok1 = tok1->next)
    cur = cur->next = copy_token(tok1);
  cur->next = tok2;
  return head.next;
}

static Token *skip_cond_incl2(Token *tok) {
  while (tok->kind != TK_EOF) {
    if (is_hash(tok) &&
        (equal(tok->next, "if") || equal(tok->next, "ifdef") ||
         equal(tok->next, "ifndef"))) {
      tok = skip_cond_incl2(tok->next->next);
      continue;
    }
    if (is_hash(tok) && equal(tok->next, "endif"))
      return tok->next->next;
    tok = tok->next;
  }
  return tok;
}

// Skip until next `#else`, `#elif` or `#endif`.
// Nested `#if` and `#endif` are skipped.
static Token *skip_cond_incl(Token *tok) {
  while (tok->kind != TK_EOF) {
    if (is_hash(tok) &&
        (equal(tok->next, "if") || equal(tok->next, "ifdef") ||
         equal(tok->next, "ifndef"))) {
      tok = skip_cond_incl2(tok->next->next);
      continue;
    }

    if (is_hash(tok) &&
        (equal(tok->next, "elif") || equal(tok->next, "else") ||
         equal(tok->next, "endif")))
      break;
    tok = tok->next;
  }
  return tok;
}

// Double-quote a given string and returns it.
static char *quote_string(char *str) {
  int bufsize = 3;
  for (int i = 0; str[i]; i++) {
    if (str[i] == '\\' || str[i] == '"')
      bufsize++;
    bufsize++;
  }

  char *buf = chibicc_arena_calloc(1, bufsize);
  char *p = buf;
  *p++ = '"';
  for (int i = 0; str[i]; i++) {
    if (str[i] == '\\' || str[i] == '"')
      *p++ = '\\';
    *p++ = str[i];
  }
  *p++ = '"';
  *p++ = '\0';
  return buf;
}

// tokenize() leaves current_file pointing at the nested file. Restore
// the caller's file so error_at's implicit current_file still matches
// the loc it is given.
static Token *tokenize_nested(File *file) {
  File *save = chibicc_ctx()->current_file;
  Token *tok = tokenize(file);
  chibicc_ctx()->current_file = save;
  return tok;
}

static Token *tokenize_file_nested(char *path) {
  File *save = chibicc_ctx()->current_file;
  Token *tok = tokenize_file(path);
  chibicc_ctx()->current_file = save;
  return tok;
}

static Token *new_str_token(char *str, Token *tmpl) {
  char *buf = quote_string(str);
  return tokenize_nested(new_file(tmpl->file->name, tmpl->file->file_no, buf));
}

// Copy all tokens until the next newline, terminate them with
// an EOF token and then returns them. This function is used to
// create a new list of tokens for `#if` arguments.
static Token *copy_line(Token **rest, Token *tok) {
  Token head = {};
  Token *cur = &head;

  for (; !tok->at_bol; tok = tok->next)
    cur = cur->next = copy_token(tok);

  cur->next = new_eof(tok);
  *rest = tok;
  return head.next;
}

static Token *new_num_token(int val, Token *tmpl) {
  char *buf = format("%d\n", val);
  return tokenize_nested(new_file(tmpl->file->name, tmpl->file->file_no, buf));
}

#ifdef CHIBICC_TEST_BUILTINS
// A B8 regression hook: a builtin that produces two tokens. Only
// test/library.sh's ASan round builds it.
static Token *multi_macro(Token *tmpl) {
  Token *t1 = new_num_token(1, tmpl);
  Token *t2 = new_num_token(2, tmpl);
  t1->next = t2;
  return t1;
}
#endif

static Token *read_const_expr(Token **rest, Token *tok) {
  tok = copy_line(rest, tok);

  Token head = {};
  Token *cur = &head;

  while (tok->kind != TK_EOF) {
    // "defined(foo)" or "defined foo" becomes "1" if macro "foo"
    // is defined. Otherwise "0".
    if (equal(tok, "defined")) {
      Token *start = tok;
      bool has_paren = consume(&tok, tok->next, "(");

      if (tok->kind != TK_IDENT)
        error_tok(start, "macro name must be an identifier");
      Macro *m = find_macro(tok);
      tok = tok->next;

      if (has_paren)
        tok = skip(tok, ")");

      cur = cur->next = new_num_token(m ? 1 : 0, start);
      continue;
    }

    cur = cur->next = tok;
    tok = tok->next;
  }

  cur->next = tok;
  return head.next;
}

// Read and evaluate a constant expression.
static long eval_const_expr(Token **rest, Token *tok) {
  Token *start = tok;
  Token *expr = read_const_expr(rest, tok->next);
  expr = preprocess2(expr);

  if (expr->kind == TK_EOF)
    error_tok(start, "no expression");

  // [https://www.sigbus.info/n1570#6.10.1p4] The standard requires
  // we replace remaining non-macro identifiers with "0" before
  // evaluating a constant expression. For example, `#if foo` is
  // equivalent to `#if 0` if foo is not defined.
  for (Token *t = expr; t->kind != TK_EOF; t = t->next) {
    if (t->kind == TK_IDENT) {
      Token *next = t->next;
      *t = *new_num_token(0, t);
      t->next = next;
    }
  }

  // Convert pp-numbers to regular numbers
  convert_pp_tokens(expr);

  Token *rest2;
  long val = const_expr(&rest2, expr);
  if (rest2->kind != TK_EOF)
    error_tok(rest2, "extra token");
  return val;
}

static CondIncl *push_cond_incl(Token *tok, bool included) {
  CondIncl *ci = chibicc_arena_calloc(1, sizeof(CondIncl));
  ci->next = cond_incl;
  ci->ctx = IN_THEN;
  ci->tok = tok;
  ci->included = included;
  cond_incl = ci;
  return ci;
}

static Macro *find_macro(Token *tok) {
  if (tok->kind != TK_IDENT)
    return NULL;
  return hashmap_get2(&macros, tok->loc, tok->len);
}

static Macro *add_macro(char *name, bool is_objlike, Token *body) {
  Macro *m = chibicc_arena_calloc(1, sizeof(Macro));
  m->name = name;
  m->is_objlike = is_objlike;
  m->body = body;
  hashmap_put(&macros, name, m);
  return m;
}

static MacroParam *read_macro_params(Token **rest, Token *tok, char **va_args_name) {
  MacroParam head = {};
  MacroParam *cur = &head;

  while (!equal(tok, ")")) {
    if (cur != &head)
      tok = skip(tok, ",");

    if (equal(tok, "...")) {
      *va_args_name = "__VA_ARGS__";
      *rest = skip(tok->next, ")");
      return head.next;
    }

    if (tok->kind != TK_IDENT)
      error_tok(tok, "expected an identifier");

    if (equal(tok->next, "...")) {
      *va_args_name = chibicc_arena_strndup(tok->loc, tok->len);
      *rest = skip(tok->next->next, ")");
      return head.next;
    }

    MacroParam *m = chibicc_arena_calloc(1, sizeof(MacroParam));
    m->name = chibicc_arena_strndup(tok->loc, tok->len);
    cur = cur->next = m;
    tok = tok->next;
  }

  *rest = tok->next;
  return head.next;
}

static void read_macro_definition(Token **rest, Token *tok) {
  if (tok->kind != TK_IDENT)
    error_tok(tok, "macro name must be an identifier");
  Token *name_tok = tok;
  char *name = chibicc_arena_strndup(tok->loc, tok->len);
  tok = tok->next;

  if (!tok->has_space && equal(tok, "(")) {
    // Function-like macro
    char *va_args_name = NULL;
    MacroParam *params = read_macro_params(&tok, tok->next, &va_args_name);

    Macro *m = add_macro(name, false, copy_line(rest, tok));
    m->params = params;
    m->va_args_name = va_args_name;
    m->def = name_tok;
  } else {
    // Object-like macro
    Macro *m = add_macro(name, true, copy_line(rest, tok));
    m->def = name_tok;
  }
}

static MacroArg *read_macro_arg_one(Token **rest, Token *tok, bool read_rest) {
  Token head = {};
  Token *cur = &head;
  int level = 0;

  for (;;) {
    if (level == 0 && equal(tok, ")"))
      break;
    if (level == 0 && !read_rest && equal(tok, ","))
      break;

    if (tok->kind == TK_EOF)
      error_tok(tok, "premature end of input");

    if (equal(tok, "("))
      level++;
    else if (equal(tok, ")"))
      level--;

    cur = cur->next = copy_token(tok);
    tok = tok->next;
  }

  cur->next = new_eof(tok);

  MacroArg *arg = chibicc_arena_calloc(1, sizeof(MacroArg));
  arg->tok = head.next;
  *rest = tok;
  return arg;
}

static MacroArg *
read_macro_args(Token **rest, Token *tok, MacroParam *params, char *va_args_name) {
  Token *start = tok;
  tok = tok->next->next;

  MacroArg head = {};
  MacroArg *cur = &head;

  MacroParam *pp = params;
  int index = 0;
  for (; pp; pp = pp->next) {
    if (cur != &head)
      tok = skip(tok, ",");
    cur = cur->next = read_macro_arg_one(&tok, tok, false);
    cur->name = pp->name;
    cur->index = index++;
  }

  if (va_args_name) {
    MacroArg *arg;
    if (equal(tok, ")")) {
      arg = chibicc_arena_calloc(1, sizeof(MacroArg));
      arg->tok = new_eof(tok);
    } else {
      if (pp != params)
        tok = skip(tok, ",");
      arg = read_macro_arg_one(&tok, tok, true);
    }
    arg->name = va_args_name;;
    arg->is_va_args = true;
    arg->index = index;
    cur = cur->next = arg;
  } else if (pp) {
    error_tok(start, "too many arguments");
  }

  skip(tok, ")");
  *rest = tok;
  return head.next;
}

static MacroArg *find_arg(MacroArg *args, Token *tok) {
  for (MacroArg *ap = args; ap; ap = ap->next)
    if (tok->len == strlen(ap->name) && !strncmp(tok->loc, ap->name, tok->len))
      return ap;
  return NULL;
}

// Concatenates all tokens in `tok` and returns a new string.
static char *join_tokens(Token *tok, Token *end) {
  // Compute the length of the resulting token.
  int len = 1;
  for (Token *t = tok; t != end && t->kind != TK_EOF; t = t->next) {
    if (t != tok && t->has_space)
      len++;
    len += t->len;
  }

  char *buf = chibicc_arena_calloc(1, len);

  // Copy token texts.
  int pos = 0;
  for (Token *t = tok; t != end && t->kind != TK_EOF; t = t->next) {
    if (t != tok && t->has_space)
      buf[pos++] = ' ';
    strncpy(buf + pos, t->loc, t->len);
    pos += t->len;
  }
  buf[pos] = '\0';
  return buf;
}

// Concatenates all tokens in `arg` and returns a new string token.
// This function is used for the stringizing operator (#).
static Token *stringize(Token *hash, Token *arg) {
  // Create a new string token. We need to set some value to its
  // source location for error reporting function, so we use a macro
  // name token as a template.
  char *s = join_tokens(arg, NULL);
  return new_str_token(s, hash);
}

// Concatenate two tokens to create a new token.
static Token *paste(Token *lhs, Token *rhs) {
  // Paste the two tokens.
  char *buf = format("%.*s%.*s", lhs->len, lhs->loc, rhs->len, rhs->loc);

  // Tokenize the resulting string.
  Token *tok = tokenize_nested(new_file(lhs->file->name, lhs->file->file_no, buf));
  if (tok->next->kind != TK_EOF)
    error_tok(lhs, "pasting forms '%s', an invalid token", buf);
  return tok;
}

static bool has_varargs(MacroArg *args) {
  for (MacroArg *ap = args; ap; ap = ap->next)
    if (!strcmp(ap->name, "__VA_ARGS__"))
      return ap->tok->kind != TK_EOF;
  return false;
}

// Replace func-like macro parameters with given arguments. Every token
// this produces is tagged with the expansion record `e`: body tokens and
// the synthetic tokens of `#` / `##` as the body layer, argument tokens
// with the index of the argument they came from. Arguments adjacent to
// `#` or `##` are not macro-expanded, which is why their provenance is the
// call site's own chain; ordinary arguments arrive pre-expanded, so the
// layer this function prepends sits in front of the chain of the token
// the expansion produced.
static Token *subst(Token *tok, MacroArg *args, MacroExpansion *e) {
  Token head = {};
  Token *cur = &head;
  MacroProv *body_base = e->name->prov;

  while (tok->kind != TK_EOF) {
    // "#" followed by a parameter is replaced with stringized actuals.
    if (equal(tok, "#")) {
      MacroArg *arg = find_arg(args, tok->next);
      if (!arg)
        error_tok(tok->next, "'#' is not followed by a macro parameter");
      cur = cur->next = stringize(tok, arg->tok);
      cur->prov = new_prov(e, -1, body_base);
      tok = tok->next->next;
      continue;
    }

    // [GNU] If __VA_ARG__ is empty, `,##__VA_ARGS__` is expanded
    // to the empty token list. Otherwise, its expaned to `,` and
    // __VA_ARGS__.
    if (equal(tok, ",") && equal(tok->next, "##")) {
      MacroArg *arg = find_arg(args, tok->next->next);
      if (arg && arg->is_va_args) {
        if (arg->tok->kind == TK_EOF) {
          tok = tok->next->next->next;
        } else {
          cur = cur->next = copy_token(tok);
          cur->prov = new_prov(e, -1, body_base);
          tok = tok->next->next;
        }
        continue;
      }
    }

    if (equal(tok, "##")) {
      if (cur == &head)
        error_tok(tok, "'##' cannot appear at start of macro expansion");

      if (tok->next->kind == TK_EOF)
        error_tok(tok, "'##' cannot appear at end of macro expansion");

      MacroArg *arg = find_arg(args, tok->next);
      if (arg) {
        if (arg->tok->kind != TK_EOF) {
          *cur = *paste(cur, arg->tok);
          cur->prov = new_prov(e, -1, body_base);
          for (Token *t = arg->tok->next; t->kind != TK_EOF; t = t->next) {
            cur = cur->next = copy_token(t);
            cur->prov = new_prov(e, arg->index, t->prov);
          }
        }
        tok = tok->next->next;
        continue;
      }

      *cur = *paste(cur, tok->next);
      cur->prov = new_prov(e, -1, body_base);
      tok = tok->next->next;
      continue;
    }

    MacroArg *arg = find_arg(args, tok);

    if (arg && equal(tok->next, "##")) {
      Token *rhs = tok->next->next;

      if (arg->tok->kind == TK_EOF) {
        MacroArg *arg2 = find_arg(args, rhs);
        if (arg2) {
          for (Token *t = arg2->tok; t->kind != TK_EOF; t = t->next) {
            cur = cur->next = copy_token(t);
            cur->prov = new_prov(e, arg2->index, t->prov);
          }
        } else {
          cur = cur->next = copy_token(rhs);
          cur->prov = new_prov(e, -1, body_base);
        }
        tok = rhs->next;
        continue;
      }

      for (Token *t = arg->tok; t->kind != TK_EOF; t = t->next) {
        cur = cur->next = copy_token(t);
        cur->prov = new_prov(e, arg->index, t->prov);
      }
      tok = tok->next;
      continue;
    }

    // If __VA_ARG__ is empty, __VA_OPT__(x) is expanded to the
    // empty token list. Otherwise, __VA_OPT__(x) is expanded to x.
    if (equal(tok, "__VA_OPT__") && equal(tok->next, "(")) {
      MacroArg *arg = read_macro_arg_one(&tok, tok->next->next, true);
      if (has_varargs(args))
        for (Token *t = arg->tok; t->kind != TK_EOF; t = t->next) {
          cur = cur->next = t;
          cur->prov = new_prov(e, -1, body_base);
        }
      tok = skip(tok, ")");
      continue;
    }

    // Handle a macro token. Macro arguments are completely macro-expanded
    // before they are substituted into a macro body.
    if (arg) {
      Token *t = preprocess2(arg->tok);
      t->at_bol = tok->at_bol;
      t->has_space = tok->has_space;
      for (; t->kind != TK_EOF; t = t->next) {
        cur = cur->next = copy_token(t);
        cur->prov = new_prov(e, arg->index, t->prov);
      }
      tok = tok->next;
      continue;
    }

    // Handle a non-macro token.
    cur = cur->next = copy_token(tok);
    cur->prov = new_prov(e, -1, body_base);
    tok = tok->next;
    continue;
  }

  cur->next = tok;
  return head.next;
}

// If tok is a macro, expand it and return true.
// Otherwise, do nothing and return false.
static bool expand_macro(Token **rest, Token *tok) {
  if (hideset_contains(tok->hideset, tok->loc, tok->len))
    return false;

  Macro *m = find_macro(tok);
  if (!m)
    return false;

  // Built-in dynamic macro application such as __LINE__
  if (m->handler) {
    MacroExpansion *e = new_expansion(tok, NULL, m, NULL, 0, true);
    Token *body = m->handler(tok);
    for (Token *t = body; t->kind != TK_EOF; t = t->next)
      t->prov = new_prov(e, -1, tok->prov);

    // A handler may produce more than one token; splice after the last
    // one, not after the head, so none is dropped. An empty body expands
    // to nothing (B8).
    if (body->kind == TK_EOF) {
      *rest = tok->next;
      return true;
    }

    Token *tail = body;
    while (tail->next->kind != TK_EOF)
      tail = tail->next;
    tail->next = tok->next;
    *rest = body;
    return true;
  }

  // Object-like macro application
  if (m->is_objlike) {
    MacroExpansion *e = new_expansion(tok, NULL, m, NULL, 0, false);
    Hideset *hs = hideset_union(tok->hideset, new_hideset(m->name));
    Token *body = add_hideset(m->body, hs);
    for (Token *t = body; t->kind != TK_EOF; t = t->next) {
      t->origin = tok;
      t->prov = new_prov(e, -1, tok->prov);
    }
    *rest = append(body, tok->next);
    (*rest)->at_bol = tok->at_bol;
    (*rest)->has_space = tok->has_space;
    return true;
  }

  // If a funclike macro token is not followed by an argument list,
  // treat it as a normal identifier.
  if (!equal(tok->next, "("))
    return false;

  // Function-like macro application
  Token *macro_token = tok;
  MacroArg *args = read_macro_args(&tok, tok, m->params, m->va_args_name);
  Token *rparen = tok;
  MacroExpansion *e = new_expansion(macro_token, rparen, m, args,
                                    count_macro_args(args), false);

  // Tokens that consist a func-like macro invocation may have different
  // hidesets, and if that's the case, it's not clear what the hideset
  // for the new tokens should be. We take the interesection of the
  // macro token and the closing parenthesis and use it as a new hideset
  // as explained in the Dave Prossor's algorithm.
  Hideset *hs = hideset_intersection(macro_token->hideset, rparen->hideset);
  hs = hideset_union(hs, new_hideset(m->name));

  Token *body = subst(m->body, args, e);
  body = add_hideset(body, hs);
  for (Token *t = body; t->kind != TK_EOF; t = t->next)
    t->origin = macro_token;
  *rest = append(body, tok->next);
  (*rest)->at_bol = macro_token->at_bol;
  (*rest)->has_space = macro_token->has_space;
  return true;
}

char *search_include_paths(char *filename) {
  if (filename[0] == '/')
    return filename;

  ChibiccContext *ctx = chibicc_ctx();
  char *cached = hashmap_get(&ctx->search_cache, filename);
  if (cached)
    return cached;

  // Search a file from the include paths.
  for (int i = 0; i < ctx->include_paths_len; i++) {
    char *path = format("%s/%s", ctx->include_paths[i], filename);
    if (!file_exists(path))
      continue;
    hashmap_put(&ctx->search_cache, filename, path);
    ctx->include_next_idx = i + 1;
    return path;
  }
  return NULL;
}

static char *search_include_next(char *filename) {
  ChibiccContext *ctx = chibicc_ctx();
  for (; ctx->include_next_idx < ctx->include_paths_len;
       ctx->include_next_idx++) {
    char *path = format("%s/%s", ctx->include_paths[ctx->include_next_idx],
                        filename);
    if (file_exists(path))
      return path;
  }
  return NULL;
}

// Read an #include argument.
static char *read_include_filename(Token **rest, Token *tok, bool *is_dquote) {
  // Pattern 1: #include "foo.h"
  if (tok->kind == TK_STR) {
    // A double-quoted filename for #include is a special kind of
    // token, and we don't want to interpret any escape sequences in it.
    // For example, "\f" in "C:\foo" is not a formfeed character but
    // just two non-control characters, backslash and f.
    // So we don't want to use token->str.
    *is_dquote = true;
    *rest = skip_line(tok->next);
    return chibicc_arena_strndup(tok->loc + 1, tok->len - 2);
  }

  // Pattern 2: #include <foo.h>
  if (equal(tok, "<")) {
    // Reconstruct a filename from a sequence of tokens between
    // "<" and ">".
    Token *start = tok;

    // Find closing ">".
    for (; !equal(tok, ">"); tok = tok->next)
      if (tok->at_bol || tok->kind == TK_EOF)
        error_tok(tok, "expected '>'");

    *is_dquote = false;
    *rest = skip_line(tok->next);
    return join_tokens(start->next, tok);
  }

  // Pattern 3: #include FOO
  // In this case FOO must be macro-expanded to either
  // a single string token or a sequence of "<" ... ">".
  if (tok->kind == TK_IDENT) {
    Token *tok2 = preprocess2(copy_line(rest, tok));
    return read_include_filename(&tok2, tok2, is_dquote);
  }

  error_tok(tok, "expected a filename");
}

// Detect the following "include guard" pattern.
//
//   #ifndef FOO_H
//   #define FOO_H
//   ...
//   #endif
static char *detect_include_guard(Token *tok) {
  // Detect the first two lines.
  if (!is_hash(tok) || !equal(tok->next, "ifndef"))
    return NULL;
  tok = tok->next->next;

  if (tok->kind != TK_IDENT)
    return NULL;

  char *macro = chibicc_arena_strndup(tok->loc, tok->len);
  tok = tok->next;

  if (!is_hash(tok) || !equal(tok->next, "define") || !equal(tok->next->next, macro))
    return NULL;

  // Read until the end of the file.
  while (tok->kind != TK_EOF) {
    if (!is_hash(tok)) {
      tok = tok->next;
      continue;
    }

    if (equal(tok->next, "endif") && tok->next->next->kind == TK_EOF)
      return macro;

    if (equal(tok, "if") || equal(tok, "ifdef") || equal(tok, "ifndef"))
      tok = skip_cond_incl(tok->next);
    else
      tok = tok->next;
  }
  return NULL;
}

// A file that includes itself through differently spelled paths cannot
// be stopped by the guard/pragma-once maps, and one file can pull in an
// unbounded number of headers. Bound both; a real translation unit stays
// well below either limit.
#define MAX_INCLUDE_DEPTH 200
#define MAX_INCLUDE_COUNT 10000

static Token *include_file(Token *tok, char *path, Token *filename_tok,
                           File *parent) {
  // Check for "#pragma once"
  if (hashmap_get(&pragma_once, path))
    return tok;

  // If we read the same file before, and if the file was guarded
  // by the usual #ifndef ... #endif pattern, we may be able to
  // skip the file without opening it.
  ChibiccContext *ctx = chibicc_ctx();
  char *guard_name = hashmap_get(&ctx->include_guards, path);
  if (guard_name && hashmap_get(&macros, guard_name))
    return tok;

  if (parent->include_depth >= MAX_INCLUDE_DEPTH)
    error_tok(filename_tok, "include depth exceeded");
  if (++ctx->include_count > MAX_INCLUDE_COUNT)
    error_tok(filename_tok, "too many includes");

  Token *tok2 = tokenize_file_nested(path);
  if (!tok2)
    error_tok(filename_tok, "%s: cannot open file: %s", path, strerror(errno));
  tok2->file->include_depth = parent->include_depth + 1;

  guard_name = detect_include_guard(tok2);
  if (guard_name)
    hashmap_put(&ctx->include_guards, path, guard_name);

  return append(tok2, tok);
}

// Read #line arguments
static void read_line_marker(Token **rest, Token *tok) {
  Token *start = tok;
  tok = preprocess(copy_line(rest, tok));

  if (tok->kind != TK_NUM || tok->ty->kind != TY_INT)
    error_tok(tok, "invalid line marker");
  start->file->line_delta = tok->val - start->line_no;

  tok = tok->next;
  if (tok->kind == TK_EOF)
    return;

  if (tok->kind != TK_STR)
    error_tok(tok, "filename expected");
  start->file->display_name = tok->str;
}

// Visit all tokens in `tok` while evaluating preprocessing
// macros and directives.
static Token *preprocess2(Token *tok) {
  Token head = {};
  Token *cur = &head;

  while (tok->kind != TK_EOF) {
    // If it is a macro, expand it.
    if (expand_macro(&tok, tok))
      continue;

    // Pass through if it is not a "#".
    if (!is_hash(tok)) {
      tok->line_delta = tok->file->line_delta;
      tok->filename = tok->file->display_name;
      cur = cur->next = tok;
      tok = tok->next;
      continue;
    }

    Token *start = tok;
    tok = tok->next;

    if (equal(tok, "include")) {
      bool is_dquote;
      char *filename = read_include_filename(&tok, tok->next, &is_dquote);

      if (filename[0] != '/' && is_dquote) {
        char *path = format("%s/%s", dirname(chibicc_arena_strdup(start->file->name)), filename);
        if (file_exists(path)) {
          tok = include_file(tok, path, start->next->next, start->file);
          continue;
        }
      }

      char *path = search_include_paths(filename);
      tok = include_file(tok, path ? path : filename, start->next->next,
                         start->file);
      continue;
    }

    if (equal(tok, "include_next")) {
      bool ignore;
      char *filename = read_include_filename(&tok, tok->next, &ignore);
      char *path = search_include_next(filename);
      tok = include_file(tok, path ? path : filename, start->next->next,
                         start->file);
      continue;
    }

    if (equal(tok, "define")) {
      read_macro_definition(&tok, tok->next);
      continue;
    }

    if (equal(tok, "undef")) {
      tok = tok->next;
      if (tok->kind != TK_IDENT)
        error_tok(tok, "macro name must be an identifier");
      undef_macro(chibicc_arena_strndup(tok->loc, tok->len));
      tok = skip_line(tok->next);
      continue;
    }

    if (equal(tok, "if")) {
      long val = eval_const_expr(&tok, tok);
      push_cond_incl(start, val);
      if (!val)
        tok = skip_cond_incl(tok);
      continue;
    }

    if (equal(tok, "ifdef")) {
      bool defined = find_macro(tok->next);
      push_cond_incl(tok, defined);
      tok = skip_line(tok->next->next);
      if (!defined)
        tok = skip_cond_incl(tok);
      continue;
    }

    if (equal(tok, "ifndef")) {
      bool defined = find_macro(tok->next);
      push_cond_incl(tok, !defined);
      tok = skip_line(tok->next->next);
      if (defined)
        tok = skip_cond_incl(tok);
      continue;
    }

    if (equal(tok, "elif")) {
      if (!cond_incl || cond_incl->ctx == IN_ELSE)
        error_tok(start, "stray #elif");
      cond_incl->ctx = IN_ELIF;

      if (!cond_incl->included && eval_const_expr(&tok, tok))
        cond_incl->included = true;
      else
        tok = skip_cond_incl(tok);
      continue;
    }

    if (equal(tok, "else")) {
      if (!cond_incl || cond_incl->ctx == IN_ELSE)
        error_tok(start, "stray #else");
      cond_incl->ctx = IN_ELSE;
      tok = skip_line(tok->next);

      if (cond_incl->included)
        tok = skip_cond_incl(tok);
      continue;
    }

    if (equal(tok, "endif")) {
      if (!cond_incl)
        error_tok(start, "stray #endif");
      cond_incl = cond_incl->next;
      tok = skip_line(tok->next);
      continue;
    }

    if (equal(tok, "line")) {
      read_line_marker(&tok, tok->next);
      continue;
    }

    if (tok->kind == TK_PP_NUM) {
      read_line_marker(&tok, tok);
      continue;
    }

    if (equal(tok, "pragma") && equal(tok->next, "once")) {
      hashmap_put(&pragma_once, tok->file->name, (void *)1);
      tok = skip_line(tok->next->next);
      continue;
    }

    if (equal(tok, "pragma")) {
      do {
        tok = tok->next;
      } while (!tok->at_bol);
      continue;
    }

    if (equal(tok, "error"))
      error_tok(tok, "error");

    // `#`-only line is legal. It's called a null directive.
    if (tok->at_bol)
      continue;

    error_tok(tok, "invalid preprocessor directive");
  }

  cur->next = tok;
  return head.next;
}

void define_macro(char *name, char *buf) {
  Token *tok = tokenize_nested(new_file("<built-in>", 1, buf));
  add_macro(name, true, tok);
}

void undef_macro(char *name) {
  hashmap_delete(&macros, name);
}

static Macro *add_builtin(char *name, macro_handler_fn *fn) {
  Macro *m = add_macro(name, true, NULL);
  m->handler = fn;
  return m;
}

static Token *file_macro(Token *tmpl) {
  while (tmpl->origin)
    tmpl = tmpl->origin;
  return new_str_token(tmpl->file->display_name, tmpl);
}

static Token *line_macro(Token *tmpl) {
  while (tmpl->origin)
    tmpl = tmpl->origin;
  int i = tmpl->line_no + tmpl->file->line_delta;
  return new_num_token(i, tmpl);
}

// __COUNTER__ is expanded to serial values starting from 0.
static Token *counter_macro(Token *tmpl) {
  return new_num_token(chibicc_ctx()->counter++, tmpl);
}

// __TIMESTAMP__ is expanded to a string describing the last
// modification time of the current file. E.g.
// "Fri Jul 24 01:32:50 2020"
static Token *timestamp_macro(Token *tmpl) {
  struct stat st;
  if (stat(tmpl->file->name, &st) != 0)
    return new_str_token("??? ??? ?? ??:??:?? ????", tmpl);

  char buf[30];
  ctime_r(&st.st_mtime, buf);
  buf[24] = '\0';
  return new_str_token(buf, tmpl);
}

static Token *base_file_macro(Token *tmpl) {
  char *file = chibicc_ctx()->base_file;
  return new_str_token(file ? file : "", tmpl);
}

// __DATE__ is expanded to the current date, e.g. "May 17 2020".
static char *format_date(struct tm *tm) {
  static char mon[][4] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
  };

  return format("\"%s %2d %d\"", mon[tm->tm_mon], tm->tm_mday, tm->tm_year + 1900);
}

// __TIME__ is expanded to the current time, e.g. "13:34:03".
static char *format_time(struct tm *tm) {
  return format("\"%02d:%02d:%02d\"", tm->tm_hour, tm->tm_min, tm->tm_sec);
}

typedef enum {
  SZ_NONE,
  SZ_DOUBLE,
  SZ_FLOAT,
  SZ_INT,
  SZ_LDOUBLE,
  SZ_LONG_LONG,
  SZ_LONG,
  SZ_POINTER,
  SZ_PTRDIFF,
  SZ_SHORT,
  SZ_SIZE,
} SizeKind;

typedef enum {
  PREDEF_MACRO,
  PREDEF_SIZEOF,
  PREDEF_BUILTIN,
  PREDEF_DYNAMIC,
} PredefKind;

typedef struct {
  char *name;
  char *value;
  SizeKind size;
  PredefKind kind;
  macro_handler_fn *handler;
} PredefinedDef;

static int predefined_size(SizeKind size) {
  switch (size) {
  case SZ_DOUBLE: return ty_double->size;
  case SZ_FLOAT: return ty_float->size;
  case SZ_INT: return ty_int->size;
  case SZ_LDOUBLE: return ty_ldouble->size;
  case SZ_LONG_LONG: return ty_long->size;
  case SZ_LONG: return ty_long->size;
  case SZ_POINTER: return ty_ulong->size;
  case SZ_PTRDIFF: return ty_long->size;
  case SZ_SHORT: return ty_short->size;
  case SZ_SIZE: return ty_ulong->size;
  case SZ_NONE: break;
  }
  return 0;
}

// The predefined set. `value` is the replacement text; the __SIZEOF_*
// entries derive it from the built-in types, and the builtin/dynamic
// entries have no replacement text of their own.
static PredefinedDef predefined_defs[] = {
  {"_LP64", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"__C99_MACRO_WITH_VA_ARGS", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"__ELF__", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"__LP64__", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"__SIZEOF_DOUBLE__", NULL, SZ_DOUBLE, PREDEF_SIZEOF, NULL},
  {"__SIZEOF_FLOAT__", NULL, SZ_FLOAT, PREDEF_SIZEOF, NULL},
  {"__SIZEOF_INT__", NULL, SZ_INT, PREDEF_SIZEOF, NULL},
  {"__SIZEOF_LONG_DOUBLE__", NULL, SZ_LDOUBLE, PREDEF_SIZEOF, NULL},
  {"__SIZEOF_LONG_LONG__", NULL, SZ_LONG_LONG, PREDEF_SIZEOF, NULL},
  {"__SIZEOF_LONG__", NULL, SZ_LONG, PREDEF_SIZEOF, NULL},
  {"__SIZEOF_POINTER__", NULL, SZ_POINTER, PREDEF_SIZEOF, NULL},
  {"__SIZEOF_PTRDIFF_T__", NULL, SZ_PTRDIFF, PREDEF_SIZEOF, NULL},
  {"__SIZEOF_SHORT__", NULL, SZ_SHORT, PREDEF_SIZEOF, NULL},
  {"__SIZEOF_SIZE_T__", NULL, SZ_SIZE, PREDEF_SIZEOF, NULL},
  {"__SIZE_TYPE__", "unsigned long", SZ_NONE, PREDEF_MACRO, NULL},
  {"__STDC_HOSTED__", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"__STDC_NO_COMPLEX__", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"__STDC_UTF_16__", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"__STDC_UTF_32__", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"__STDC_VERSION__", "201112L", SZ_NONE, PREDEF_MACRO, NULL},
  {"__STDC__", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"__USER_LABEL_PREFIX__", "", SZ_NONE, PREDEF_MACRO, NULL},
  {"__alignof__", "_Alignof", SZ_NONE, PREDEF_MACRO, NULL},
  {"__amd64", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"__amd64__", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"__chibicc__", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"__const__", "const", SZ_NONE, PREDEF_MACRO, NULL},
  {"__gnu_linux__", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"__inline__", "inline", SZ_NONE, PREDEF_MACRO, NULL},
  {"__linux", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"__linux__", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"__signed__", "signed", SZ_NONE, PREDEF_MACRO, NULL},
  {"__typeof__", "typeof", SZ_NONE, PREDEF_MACRO, NULL},
  {"__unix", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"__unix__", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"__volatile__", "volatile", SZ_NONE, PREDEF_MACRO, NULL},
  {"__x86_64", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"__x86_64__", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"linux", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"unix", "1", SZ_NONE, PREDEF_MACRO, NULL},
  {"__FILE__", NULL, SZ_NONE, PREDEF_BUILTIN, file_macro},
  {"__LINE__", NULL, SZ_NONE, PREDEF_BUILTIN, line_macro},
  {"__COUNTER__", NULL, SZ_NONE, PREDEF_BUILTIN, counter_macro},
  {"__TIMESTAMP__", NULL, SZ_NONE, PREDEF_BUILTIN, timestamp_macro},
  {"__BASE_FILE__", NULL, SZ_NONE, PREDEF_BUILTIN, base_file_macro},
  {"__DATE__", NULL, SZ_NONE, PREDEF_DYNAMIC, NULL},
  {"__TIME__", NULL, SZ_NONE, PREDEF_DYNAMIC, NULL},
};

void init_macros(void) {
  ChibiccContext *ctx = chibicc_ctx();
  if (!ctx || !ctx->predefined_macros)
    return;

  int n = sizeof(predefined_defs) / sizeof(*predefined_defs);
  ctx->predefined_len = n;
  ctx->predefined = chibicc_arena_calloc(n, sizeof(ChibiccPredefinedMacro));

  for (int i = 0; i < n; i++) {
    PredefinedDef *d = &predefined_defs[i];
    ChibiccPredefinedMacro *e = &ctx->predefined[i];
    e->name = d->name;

    if (d->kind == PREDEF_SIZEOF) {
      char *value = format("%d", predefined_size(d->size));
      e->value = value;
      define_macro(d->name, value);
    } else if (d->kind == PREDEF_MACRO) {
      e->value = d->value;
      define_macro(d->name, d->value);
    } else if (d->kind == PREDEF_BUILTIN) {
      add_builtin(d->name, d->handler);
    }
  }
#ifdef CHIBICC_TEST_BUILTINS
  add_builtin("__CHIBICC_MULTI__", multi_macro);
#endif

  // localtime returns a process-global buffer; contexts may be created
  // in different threads (context.h), so use the reentrant form.
  time_t now = time(NULL);
  struct tm tm_buf;
  localtime_r(&now, &tm_buf);
  define_macro("__DATE__", format_date(&tm_buf));
  define_macro("__TIME__", format_time(&tm_buf));
}

int chibicc_context_predefined_count(ChibiccContext *ctx) {
  return ctx ? ctx->predefined_len : 0;
}

ChibiccPredefinedMacro chibicc_context_predefined_at(ChibiccContext *ctx,
                                                     int index) {
  ChibiccPredefinedMacro none = {NULL, NULL};
  if (!ctx || index < 0 || index >= ctx->predefined_len)
    return none;
  return ctx->predefined[index];
}

typedef enum {
  STR_NONE, STR_UTF8, STR_UTF16, STR_UTF32, STR_WIDE,
} StringKind;

static StringKind getStringKind(Token *tok) {
  // `loc` is the first byte of the spelling, not a NUL-terminated
  // prefix: `u8"..."` continues with the quote, so a strcmp against
  // "u8" never matches. Check the three bytes that form the prefix.
  if (tok->len >= 3 && tok->loc[0] == 'u' && tok->loc[1] == '8' &&
      tok->loc[2] == '"')
    return STR_UTF8;

  switch (tok->loc[0]) {
  case '"': return STR_NONE;
  case 'u': return STR_UTF16;
  case 'U': return STR_UTF32;
  case 'L': return STR_WIDE;
  }
  unreachable();
}

// Concatenate adjacent string literals into a single string literal
// as per the C spec.
static void join_adjacent_string_literals(Token *tok) {
  // First pass: If regular string literals are adjacent to wide
  // string literals, regular string literals are converted to a wide
  // type before concatenation. In this pass, we do the conversion.
  for (Token *tok1 = tok; tok1->kind != TK_EOF;) {
    if (tok1->kind != TK_STR || tok1->next->kind != TK_STR) {
      tok1 = tok1->next;
      continue;
    }

    StringKind kind = getStringKind(tok1);
    Type *basety = tok1->ty->base;

    for (Token *t = tok1->next; t->kind == TK_STR; t = t->next) {
      StringKind k = getStringKind(t);
      if (kind == STR_NONE) {
        kind = k;
        basety = t->ty->base;
      } else if (k != STR_NONE && kind != k) {
        error_tok(t, "unsupported non-standard concatenation of string literals");
      }
    }

    if (basety->size > 1)
      for (Token *t = tok1; t->kind == TK_STR; t = t->next)
        if (t->ty->base->size == 1) {
          // The re-scan replaces the whole token (and does not set
          // line_no); the provenance belongs to the source token.
          MacroProv *prov = t->prov;
          *t = *tokenize_string_literal(t, basety);
          t->prov = prov;
        }

    while (tok1->kind == TK_STR)
      tok1 = tok1->next;
  }

  // Second pass: concatenate adjacent string literals.
  for (Token *tok1 = tok; tok1->kind != TK_EOF;) {
    if (tok1->kind != TK_STR || tok1->next->kind != TK_STR) {
      tok1 = tok1->next;
      continue;
    }

    Token *tok2 = tok1->next;
    while (tok2->kind == TK_STR)
      tok2 = tok2->next;

    int len = tok1->ty->array_len;
    for (Token *t = tok1->next; t != tok2; t = t->next)
      len = len + t->ty->array_len - 1;

    char *buf = chibicc_arena_calloc(tok1->ty->base->size, len);

    int i = 0;
    for (Token *t = tok1; t != tok2; t = t->next) {
      memcpy(buf + i, t->str, t->ty->size);
      i = i + t->ty->size - t->ty->base->size;
    }

    *tok1 = *copy_token(tok1);
    tok1->ty = array_of(tok1->ty->base, len);
    tok1->str = buf;
    tok1->next = tok2;
    tok1 = tok2;
  }
}

// Entry point function of the preprocessor.
Token *preprocess(Token *tok) {
  tok = preprocess2(tok);
  if (cond_incl)
    error_tok(cond_incl->tok, "unterminated conditional directive");
  convert_pp_tokens(tok);
  join_adjacent_string_literals(tok);

  for (Token *t = tok; t; t = t->next)
    t->line_no += t->line_delta;
  return tok;
}

// Provenance accessors. Their bodies are trivial on purpose: the chain
// layout is preprocess.c's business, and consumers (provenance.c
// today, the library API later) see only these functions.

MacroProv *token_provenance(Token *tok) {
  return tok->prov;
}

MacroProv *macro_prov_next(MacroProv *p) {
  return p->next;
}

MacroExpansion *macro_prov_expansion(MacroProv *p) {
  return p->exp;
}

int macro_prov_arg_index(MacroProv *p) {
  return p->arg_index;
}

Token *macro_expansion_name(MacroExpansion *e) {
  return e->name;
}

Token *macro_expansion_end(MacroExpansion *e) {
  return e->rparen ? e->rparen : e->name;
}

char *macro_expansion_macro(MacroExpansion *e) {
  return e->macro->name;
}

Token *macro_expansion_def(MacroExpansion *e) {
  return e->macro->def;
}

Token *macro_expansion_arg(MacroExpansion *e, int i) {
  if (i < 0 || i >= e->arg_count)
    return NULL;

  MacroArg *arg = e->args;
  for (; i > 0 && arg; i--)
    arg = arg->next;
  return arg ? arg->tok : NULL;
}

int macro_expansion_arg_count(MacroExpansion *e) {
  return e->arg_count;
}

bool macro_expansion_is_builtin(MacroExpansion *e) {
  return e->is_builtin;
}
