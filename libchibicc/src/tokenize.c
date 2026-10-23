#include "chibicc.h"

// The input file, the input file list and the current position flags
// live in ChibiccContext (PLAN L2.2). The lexer reads them through the
// active context like every other phase.

// The raw offset of a position in the scan buffer (PLAN T2.1): the two
// texts differ where a continuation, a '\r' or a universal character
// name was rewritten. They coincide for synthetic buffers.
static int raw_off(File *file, char *p) {
  return file->map[p - file->contents];
}

// The raw offset just after a token's spelling: the bytes skipped in
// front of the next token start there.
static int raw_end(Token *tok) {
  return tok->raw_loc - tok->file->raw + tok->raw_len;
}

static int from_hex(char c) {
  if ('0' <= c && c <= '9')
    return c - '0';
  if ('a' <= c && c <= 'f')
    return c - 'a' + 10;
  return c - 'A' + 10;
}

static uint32_t read_universal_char(char *p, int len) {
  uint32_t c = 0;
  for (int i = 0; i < len; i++) {
    if (!isxdigit((unsigned char)p[i]))
      return 0;
    c = (c << 4) | from_hex(p[i]);
  }
  return c;
}

// The raw offset just after the bytes that scan offset `off` was produced
// from: a line break and a universal character name stand for more than
// one raw byte, everything else for one.
static int raw_end_of(File *file, int off) {
  int lo = file->map[off];
  char *p = file->raw + lo;

  if (is_line_break(p))
    return lo + line_break_len(p);

  if (p[0] == '\\' && (p[1] == 'u' || p[1] == 'U')) {
    int digs = p[1] == 'u' ? 4 : 8;
    if (read_universal_char(p + 2, digs))
      return lo + digs + 2;
  }
  return lo + 1;
}

// Formats a diagnostic message into the arena (or malloc before a
// context exists).
static char *vformat(ChibiccContext *ctx, char *fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  char *buf = chibicc_arena_alloc(ctx, n + 1, 1);
  vsnprintf(buf, n + 1, fmt, ap);
  return buf;
}

// Unwinds to the innermost public-entry frame; without one (the
// driver's pre-cc1 paths), exits as before.
static noreturn void raise_error(ChibiccContext *ctx) {
  if (ctx->jmp_top) {
    ctx->failed = true;
    longjmp(ctx->jmp_top->buf, 1);
  }
  exit(1);
}

// Reports an error message in the following format.
//
// foo.c:10: x = y + 1;
//               ^ <error message here>
//
// A registered callback gets the structured form instead; the default
// path below is byte-for-byte what the compiler printed before L2.6.
static void verror_at(ChibiccContext *ctx, char *filename, char *input, int line_no, char *loc,
                      ChibiccDiagnosticSeverity severity, char *fmt, va_list ap) {

  // Find a line containing `loc`.
  char *line = loc;
  while (input < line && line[-1] != '\n')
    line--;

  if (ctx && ctx->diag_fn) {
    char *msg = vformat(ctx, fmt, ap);
    ChibiccDiagnostic d = {filename, line_no,
                           display_width(line, loc - line) + 1,
                           severity, msg};
    ctx->diag_fn(ctx->diag_user, &d);
    return;
  }

  char *end = loc;
  while (*end && *end != '\n')
    end++;

  // Print out the line.
  int indent = fprintf(stderr, "%s:%d: ", filename, line_no);
  fprintf(stderr, "%.*s\n", (int)(end - line), line);

  // Show the error message.
  int pos = display_width(line, loc - line) + indent;

  fprintf(stderr, "%*s", pos, ""); // print pos spaces.
  fprintf(stderr, "^ ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
}

// Reports an error; unwinds to the current public entry, or exits when
// the driver is outside one.
void error(ChibiccContext *ctx, char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);

  if (ctx && ctx->diag_fn) {
    char *msg = vformat(ctx, fmt, ap);
    ChibiccDiagnostic d = {NULL, 0, 0, CHIBICC_DIAG_ERROR, msg};
    ctx->diag_fn(ctx->diag_user, &d);
  } else {
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
  }

  va_end(ap);
  raise_error(ctx);
}

void error_at(ChibiccContext *ctx, char *loc, char *fmt, ...) {
  int line_no = ctx->current_file->lines[raw_off(ctx->current_file, loc)];

  va_list ap;
  va_start(ap, fmt);
  verror_at(ctx, ctx->current_file->name, ctx->current_file->contents, line_no,
            loc, CHIBICC_DIAG_ERROR, fmt, ap);
  va_end(ap);
  raise_error(ctx);
}

void error_tok(ChibiccContext *ctx, Token *tok, char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  verror_at(ctx, tok->file->name, tok->file->contents, tok->line_no, tok->loc,
            CHIBICC_DIAG_ERROR, fmt, ap);
  va_end(ap);
  raise_error(ctx);
}

void warn_tok(ChibiccContext *ctx, Token *tok, char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  verror_at(ctx, tok->file->name, tok->file->contents, tok->line_no, tok->loc,
            CHIBICC_DIAG_WARNING, fmt, ap);
  va_end(ap);
}

// The dump name of a token kind. Lives here, not in the driver, so the
// provenance channel can use it without main.c.
char *token_kind_name(TokenKind kind) {
  switch (kind) {
  case TK_IDENT:   return "ident";
  case TK_PUNCT:   return "punct";
  case TK_KEYWORD: return "keyword";
  case TK_STR:     return "str";
  case TK_NUM:     return "num";
  case TK_PP_NUM:  return "pp-num";
  case TK_EOF:     return "eof";
  }
  unreachable();
}

// Consumes the current token if it matches `op`.
bool equal(Token *tok, char *op) {
  // The length check comes first: a token longer than `op` must not make
  // memcmp read past the end of `op` (ASan found this in L3.4c).
  return (int)strlen(op) == tok->len && memcmp(tok->loc, op, tok->len) == 0;
}

// Ensure that the current token is `op`.
Token *skip(Token *tok, char *op) {
  if (!equal(tok, op))
    error_tok(chibicc_ctx(), tok, "expected '%s'", op);
  return tok->next;
}

bool consume(Token **rest, Token *tok, char *str) {
  if (equal(tok, str)) {
    *rest = tok->next;
    return true;
  }
  *rest = tok;
  return false;
}

// Create a new token. `raw_trivia` is the raw offset where the bytes
// skipped in front of this token start (see Token.trivia).
static Token *new_token(TokenKind kind, char *start, char *end, int raw_trivia) {
  Token *tok = chibicc_arena_calloc(chibicc_ctx(), 1, sizeof(Token));
  tok->kind = kind;
  tok->loc = start;
  tok->len = end - start;

  int raw_start = raw_off(chibicc_ctx()->current_file, start);
  tok->trivia = chibicc_ctx()->current_file->raw + raw_trivia;
  tok->trivia_len = raw_start - raw_trivia;
  tok->raw_loc = chibicc_ctx()->current_file->raw + raw_start;
  tok->raw_len = start == end ? 0 :
    raw_end_of(chibicc_ctx()->current_file, end - chibicc_ctx()->current_file->contents - 1) - raw_start;

  tok->file = chibicc_ctx()->current_file;
  tok->filename = chibicc_ctx()->current_file->display_name;
  tok->at_bol = chibicc_ctx()->at_bol;
  tok->has_space = chibicc_ctx()->has_space;

  chibicc_ctx()->at_bol = chibicc_ctx()->has_space = false;
  return tok;
}

static bool startswith(char *p, char *q) {
  return strncmp(p, q, strlen(q)) == 0;
}

// Read an identifier and returns the length of it.
// If p does not point to a valid identifier, 0 is returned.
static int read_ident(char *start) {
  char *p = start;
  uint32_t c = decode_utf8(&p, p);
  if (!is_ident1(c))
    return 0;

  for (;;) {
    char *q;
    c = decode_utf8(&q, p);
    if (!is_ident2(c))
      return p - start;
    p = q;
  }
}

// Read a punctuator token from p and returns its length.
static int read_punct(char *p) {
  static char *kw[] = {
    "<<=", ">>=", "...", "==", "!=", "<=", ">=", "->", "+=",
    "-=", "*=", "/=", "++", "--", "%=", "&=", "|=", "^=", "&&",
    "||", "<<", ">>", "##",
  };

  for (int i = 0; i < sizeof(kw) / sizeof(*kw); i++)
    if (startswith(p, kw[i]))
      return strlen(kw[i]);

  return ispunct((unsigned char)*p) ? 1 : 0;
}

static bool is_keyword(Token *tok) {
  ChibiccContext *ctx = chibicc_ctx();
  HashMap *map = &ctx->keyword_map;

  if (map->capacity == 0) {
    static char *kw[] = {
      "return", "if", "else", "for", "while", "int", "sizeof", "char",
      "struct", "union", "short", "long", "void", "typedef", "_Bool",
      "enum", "static", "goto", "break", "continue", "switch", "case",
      "default", "extern", "_Alignof", "_Alignas", "do", "signed",
      "unsigned", "const", "volatile", "auto", "register", "restrict",
      "__restrict", "__restrict__", "_Noreturn", "float", "double",
      "typeof", "asm", "_Thread_local", "__thread", "_Atomic",
      "__attribute__",
    };

    for (int i = 0; i < sizeof(kw) / sizeof(*kw); i++)
      hashmap_put(chibicc_ctx(), map, kw[i], (void *)1);
  }

  return hashmap_get2(map, tok->loc, tok->len);
}

static int read_escaped_char(char **new_pos, char *p) {
  if ('0' <= *p && *p <= '7') {
    // Read an octal number.
    int c = *p++ - '0';
    if ('0' <= *p && *p <= '7') {
      c = (c << 3) + (*p++ - '0');
      if ('0' <= *p && *p <= '7')
        c = (c << 3) + (*p++ - '0');
    }
    *new_pos = p;
    return c;
  }

  if (*p == 'x') {
    // Read a hexadecimal number.
    p++;
    if (!isxdigit((unsigned char)*p))
      error_at(chibicc_ctx(), p, "invalid hex escape sequence");

    int c = 0;
    for (; isxdigit((unsigned char)*p); p++)
      c = (c << 4) + from_hex(*p);
    *new_pos = p;
    return c;
  }

  *new_pos = p + 1;

  // Escape sequences are defined using themselves here. E.g.
  // '\n' is implemented using '\n'. This tautological definition
  // works because the compiler that compiles our compiler knows
  // what '\n' actually is. In other words, we "inherit" the ASCII
  // code of '\n' from the compiler that compiles our compiler,
  // so we don't have to teach the actual code here.
  //
  // This fact has huge implications not only for the correctness
  // of the compiler but also for the security of the generated code.
  // For more info, read "Reflections on Trusting Trust" by Ken Thompson.
  // https://github.com/rui314/chibicc/wiki/thompson1984.pdf
  switch (*p) {
  case 'a': return '\a';
  case 'b': return '\b';
  case 't': return '\t';
  case 'n': return '\n';
  case 'v': return '\v';
  case 'f': return '\f';
  case 'r': return '\r';
  // [GNU] \e for the ASCII escape character is a GNU C extension.
  case 'e': return 27;
  default: return *p;
  }
}

// Find a closing double-quote.
static char *string_literal_end(char *p) {
  char *start = p;
  for (; *p != '"'; p++) {
    if (*p == '\n' || *p == '\0')
      error_at(chibicc_ctx(), start, "unclosed string literal");
    if (*p == '\\')
      p++;
  }
  return p;
}

static Token *read_string_literal(char *start, char *quote, int raw_trivia) {
  char *end = string_literal_end(quote + 1);
  char *buf = chibicc_arena_calloc(chibicc_ctx(), 1, end - quote);
  int len = 0;

  for (char *p = quote + 1; p < end;) {
    if (*p == '\\')
      buf[len++] = read_escaped_char(&p, p + 1);
    else
      buf[len++] = *p++;
  }

  Token *tok = new_token(TK_STR, start, end + 1, raw_trivia);
  tok->ty = array_of(ty_char, len + 1);
  tok->str = buf;
  return tok;
}

// Read a UTF-8-encoded string literal and transcode it in UTF-16.
//
// UTF-16 is yet another variable-width encoding for Unicode. Code
// points smaller than U+10000 are encoded in 2 bytes. Code points
// equal to or larger than that are encoded in 4 bytes. Each 2 bytes
// in the 4 byte sequence is called "surrogate", and a 4 byte sequence
// is called a "surrogate pair".
static Token *read_utf16_string_literal(char *start, char *quote, int raw_trivia) {
  char *end = string_literal_end(quote + 1);
  uint16_t *buf = chibicc_arena_calloc(chibicc_ctx(), 2, end - start);
  int len = 0;

  for (char *p = quote + 1; p < end;) {
    if (*p == '\\') {
      buf[len++] = read_escaped_char(&p, p + 1);
      continue;
    }

    uint32_t c = decode_utf8(&p, p);
    if (c < 0x10000) {
      // Encode a code point in 2 bytes.
      buf[len++] = c;
    } else {
      // Encode a code point in 4 bytes.
      c -= 0x10000;
      buf[len++] = 0xd800 + ((c >> 10) & 0x3ff);
      buf[len++] = 0xdc00 + (c & 0x3ff);
    }
  }

  Token *tok = new_token(TK_STR, start, end + 1, raw_trivia);
  tok->ty = array_of(ty_ushort, len + 1);
  tok->str = (char *)buf;
  return tok;
}

// Read a UTF-8-encoded string literal and transcode it in UTF-32.
//
// UTF-32 is a fixed-width encoding for Unicode. Each code point is
// encoded in 4 bytes.
static Token *read_utf32_string_literal(char *start, char *quote, Type *ty, int raw_trivia) {
  char *end = string_literal_end(quote + 1);
  uint32_t *buf = chibicc_arena_calloc(chibicc_ctx(), 4, end - quote);
  int len = 0;

  for (char *p = quote + 1; p < end;) {
    if (*p == '\\')
      buf[len++] = read_escaped_char(&p, p + 1);
    else
      buf[len++] = decode_utf8(&p, p);
  }

  Token *tok = new_token(TK_STR, start, end + 1, raw_trivia);
  tok->ty = array_of(ty, len + 1);
  tok->str = (char *)buf;
  return tok;
}

static Token *read_char_literal(char *start, char *quote, Type *ty, int raw_trivia) {
  char *p = quote + 1;
  if (*p == '\0')
    error_at(chibicc_ctx(), start, "unclosed char literal");

  int c;
  if (*p == '\\')
    c = read_escaped_char(&p, p + 1);
  else
    c = decode_utf8(&p, p);

  char *end = strchr(p, '\'');
  if (!end)
    error_at(chibicc_ctx(), p, "unclosed char literal");

  Token *tok = new_token(TK_NUM, start, end + 1, raw_trivia);
  tok->val = c;
  tok->ty = ty;
  return tok;
}

static bool convert_pp_int(Token *tok) {
  char *p = tok->loc;

  // Read a binary, octal, decimal or hexadecimal number.
  int base = 10;
  if (!strncasecmp(p, "0x", 2) && isxdigit((unsigned char)p[2])) {
    p += 2;
    base = 16;
  } else if (!strncasecmp(p, "0b", 2) && (p[2] == '0' || p[2] == '1')) {
    p += 2;
    base = 2;
  } else if (*p == '0') {
    base = 8;
  }

  // strtoul saturates at ULONG_MAX and sets ERANGE on overflow; do not
  // let an unrepresentable literal silently become ULONG_MAX.
  errno = 0;
  int64_t val = strtoul(p, &p, base);
  if (errno == ERANGE)
    error_tok(chibicc_ctx(), tok, "integer literal is too large");

  // Read U, L or LL suffixes.
  bool l = false;
  bool u = false;

  if (startswith(p, "LLU") || startswith(p, "LLu") ||
      startswith(p, "llU") || startswith(p, "llu") ||
      startswith(p, "ULL") || startswith(p, "Ull") ||
      startswith(p, "uLL") || startswith(p, "ull")) {
    p += 3;
    l = u = true;
  } else if (!strncasecmp(p, "lu", 2) || !strncasecmp(p, "ul", 2)) {
    p += 2;
    l = u = true;
  } else if (startswith(p, "LL") || startswith(p, "ll")) {
    p += 2;
    l = true;
  } else if (*p == 'L' || *p == 'l') {
    p++;
    l = true;
  } else if (*p == 'U' || *p == 'u') {
    p++;
    u = true;
  }

  if (p != tok->loc + tok->len)
    return false;

  // Infer a type.
  Type *ty;
  if (base == 10) {
    if (l && u)
      ty = ty_ulong;
    else if (l)
      ty = ty_long;
    else if (u)
      ty = (val >> 32) ? ty_ulong : ty_uint;
    else
      ty = (val >> 31) ? ty_long : ty_int;
  } else {
    if (l && u)
      ty = ty_ulong;
    else if (l)
      ty = (val >> 63) ? ty_ulong : ty_long;
    else if (u)
      ty = (val >> 32) ? ty_ulong : ty_uint;
    else if (val >> 63)
      ty = ty_ulong;
    else if (val >> 32)
      ty = ty_long;
    else if (val >> 31)
      ty = ty_uint;
    else
      ty = ty_int;
  }

  tok->kind = TK_NUM;
  tok->val = val;
  tok->ty = ty;
  return true;
}

// The definition of the numeric literal at the preprocessing stage
// is more relaxed than the definition of that at the later stages.
// In order to handle that, a numeric literal is tokenized as a
// "pp-number" token first and then converted to a regular number
// token after preprocessing.
//
// This function converts a pp-number token to a regular number token.
static void convert_pp_number(Token *tok) {
  // Try to parse as an integer constant.
  if (convert_pp_int(tok))
    return;

  // If it's not an integer, it must be a floating point constant.
  char *end;
  long double val = strtold(tok->loc, &end);

  Type *ty;
  if (*end == 'f' || *end == 'F') {
    ty = ty_float;
    end++;
  } else if (*end == 'l' || *end == 'L') {
    ty = ty_ldouble;
    end++;
  } else {
    ty = ty_double;
  }

  if (tok->loc + tok->len != end)
    error_tok(chibicc_ctx(), tok, "invalid numeric constant");

  tok->kind = TK_NUM;
  tok->fval = val;
  tok->ty = ty;
}

void convert_pp_tokens(Token *tok) {
  for (Token *t = tok; t->kind != TK_EOF; t = t->next) {
    if (is_keyword(t))
      t->kind = TK_KEYWORD;
    else if (t->kind == TK_PP_NUM)
      convert_pp_number(t);
  }
}

// Initialize line info for all tokens.
static void add_line_numbers(Token *tok) {
  for (; tok; tok = tok->next)
    tok->line_no = tok->file->lines[raw_off(tok->file, tok->loc)];
}

Token *tokenize_string_literal(Token *tok, Type *basety) {
  // The re-scan needs the raw coordinates of the token it replaces, so
  // scan against the file that token belongs to (the running
  // `current_file` is whichever file was tokenized last). The `file` and
  // `filename` fields are restored afterwards: they used to be the stale
  // ones, and that pre-existing behavior is kept for now.
  File *save = chibicc_ctx()->current_file;
  chibicc_ctx()->current_file = tok->file;

  Token *t;
  if (basety->size == 2)
    t = read_utf16_string_literal(tok->loc, tok->loc, tok->trivia - tok->file->raw);
  else
    t = read_utf32_string_literal(tok->loc, tok->loc, basety,
                                  tok->trivia - tok->file->raw);

  chibicc_ctx()->current_file = save;
  t->file = save;
  t->filename = save->display_name;
  t->next = tok->next;
  return t;
}

// Tokenize a given string and returns new tokens.
Token *tokenize(File *file) {
  chibicc_ctx()->current_file = file;

  char *p = file->contents;

  // The raw offset where the bytes skipped in front of the next token
  // start. The four skip loops below only advance `p`; the bytes they
  // pass over are recorded as the trivia slice of the next token.
  int raw_trivia = 0;

  Token head = {};
  Token *cur = &head;

  chibicc_ctx()->at_bol = true;
  chibicc_ctx()->has_space = false;

  while (*p) {
    // Skip line comments.
    if (startswith(p, "//")) {
      p += 2;
      while (*p != '\n')
        p++;
      chibicc_ctx()->has_space = true;
      continue;
    }

    // Skip block comments.
    if (startswith(p, "/*")) {
      char *q = strstr(p + 2, "*/");
      if (!q)
        error_at(chibicc_ctx(), p, "unclosed block comment");
      p = q + 2;
      chibicc_ctx()->has_space = true;
      continue;
    }

    // Skip newline.
    if (*p == '\n') {
      p++;
      chibicc_ctx()->at_bol = true;
      chibicc_ctx()->has_space = false;
      continue;
    }

    // Skip whitespace characters.
    if (isspace((unsigned char)*p)) {
      p++;
      chibicc_ctx()->has_space = true;
      continue;
    }

    // Numeric literal
    if (isdigit((unsigned char)*p) ||
        (*p == '.' && isdigit((unsigned char)p[1]))) {
      char *q = p++;
      for (;;) {
        if (p[0] && p[1] && strchr("eEpP", p[0]) && strchr("+-", p[1]))
          p += 2;
        else if (isalnum((unsigned char)*p) || *p == '.')
          p++;
        else
          break;
      }
      cur = cur->next = new_token(TK_PP_NUM, q, p, raw_trivia);
      raw_trivia = raw_end(cur);
      continue;
    }

    // String literal
    if (*p == '"') {
      cur = cur->next = read_string_literal(p, p, raw_trivia);
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // UTF-8 string literal
    if (startswith(p, "u8\"")) {
      cur = cur->next = read_string_literal(p, p + 2, raw_trivia);
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // UTF-16 string literal
    if (startswith(p, "u\"")) {
      cur = cur->next = read_utf16_string_literal(p, p + 1, raw_trivia);
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // Wide string literal
    if (startswith(p, "L\"")) {
      cur = cur->next = read_utf32_string_literal(p, p + 1, ty_int, raw_trivia);
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // UTF-32 string literal
    if (startswith(p, "U\"")) {
      cur = cur->next = read_utf32_string_literal(p, p + 1, ty_uint, raw_trivia);
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // Character literal
    if (*p == '\'') {
      cur = cur->next = read_char_literal(p, p, ty_int, raw_trivia);
      cur->val = (char)cur->val;
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // UTF-16 character literal
    if (startswith(p, "u'")) {
      cur = cur->next = read_char_literal(p, p + 1, ty_ushort, raw_trivia);
      cur->val &= 0xffff;
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // Wide character literal
    if (startswith(p, "L'")) {
      cur = cur->next = read_char_literal(p, p + 1, ty_int, raw_trivia);
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // UTF-32 character literal
    if (startswith(p, "U'")) {
      cur = cur->next = read_char_literal(p, p + 1, ty_uint, raw_trivia);
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // Identifier or keyword
    int ident_len = read_ident(p);
    if (ident_len) {
      cur = cur->next = new_token(TK_IDENT, p, p + ident_len, raw_trivia);
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // Punctuators
    int punct_len = read_punct(p);
    if (punct_len) {
      cur = cur->next = new_token(TK_PUNCT, p, p + punct_len, raw_trivia);
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    error_at(chibicc_ctx(), p, "invalid token");
  }

  // The trailing trivia of the file belongs to the EOF token.
  cur = cur->next = new_token(TK_EOF, p, p, raw_trivia);
  add_line_numbers(head.next);
  return head.next;
}

// Returns the contents of a given file, and its length in *len. The
// text is returned exactly as it is on disk: the rewrites that used to
// happen here (a synthesized final newline, the BOM skip) moved into
// make_scan_buffer, which can remember them in raw coordinates.
static char *read_file(char *path, size_t *len) {
  ChibiccContext *ctx = chibicc_active_ctx;
  FILE *fp;

  if (strcmp(path, "-") == 0) {
    // By convention, read from stdin if a given filename is "-". stdin
    // cannot be rewound and its EOF flag is sticky, so the first read is
    // cached in the context: a second tokenize of the same stream then
    // sees the same text instead of a silently empty one (C12).
    if (ctx && ctx->stdin_text) {
      *len = ctx->stdin_len;
      return ctx->stdin_text;
    }
    fp = stdin;
  } else {
    fp = fopen(path, "r");
    if (!fp)
      return NULL;
  }

  char *buf;
  size_t buflen;
  FILE *out = open_memstream(&buf, &buflen);

  // Read the entire file.
  for (;;) {
    char buf2[4096];
    size_t n = fread(buf2, 1, sizeof(buf2), fp);
    if (n == 0)
      break;
    fwrite(buf2, 1, n, out);
  }

  if (fp != stdin)
    fclose(fp);

  fflush(out);

  // The lexer keeps offsets in int; a larger file would be truncated
  // silently, so refuse it instead (C12).
  if (buflen > INT32_MAX) {
    fclose(out);
    free(buf);
    error(chibicc_ctx(), "file too large: %s", path);
  }

  // fputc below grows the memstream's length by the terminator; keep the
  // text length itself for the callers and the stdin cache.
  size_t data_len = buflen;
  *len = data_len;
  fputc('\0', out);
  fclose(out);

  // The memstream buffer is malloc-owned; move the text into the arena
  // so a context reset releases it with everything else.
  if (ctx) {
    char *copy = chibicc_arena_alloc(chibicc_ctx(), data_len + 1, 1);
    memcpy(copy, buf, data_len + 1);
    free(buf);
    if (strcmp(path, "-") == 0) {
      ctx->stdin_text = copy;
      ctx->stdin_len = data_len;
    }
    return copy;
  }
  return buf;
}

File **get_input_files(void) {
  return chibicc_ctx()->input_files;
}

File *new_file_len(char *name, int file_no, char *contents, int len) {
  File *file = chibicc_arena_calloc(chibicc_ctx(), 1, sizeof(File));
  file->name = name;
  file->display_name = name;
  file->file_no = file_no;
  file->contents = contents;

  // A synthetic buffer is scanned as it is: it is its own raw text, the
  // map is the identity and the line numbers are the plain newline
  // count. (tokenize_file replaces these tables with the ones
  // make_scan_buffer builds.)
  file->raw = contents;
  file->map = chibicc_arena_calloc(chibicc_ctx(), len + 2, sizeof(int));
  file->lines = chibicc_arena_calloc(chibicc_ctx(), len + 2, sizeof(int));

  int line = 1;
  for (int i = 0; i <= len; i++) {
    file->map[i] = i;
    file->lines[i] = line;
    if (i < len && contents[i] == '\n')
      line++;
  }
  return file;
}

File *new_file(char *name, int file_no, char *contents) {
  return new_file_len(name, file_no, contents, strlen(contents));
}

// True if p starts a line break: '\n', '\r\n' or '\r'.
bool is_line_break(char *p) {
  return *p == '\n' || *p == '\r';
}

// The length of the line break at p.
int line_break_len(char *p) {
  return (*p == '\r' && p[1] == '\n') ? 2 : 1;
}

// Build the buffer the lexer scans for a real file, out of the raw text
// (PLAN T2.1). The scan text is the one the three former pre-passes
// produced - line breaks canonicalized to '\n', a backslash before a
// line break removed together with it, universal character names
// rewritten to UTF-8, a leading BOM skipped and a final newline
// guaranteed - so every consumer still sees the text it always saw. The
// `map` records, for every scan offset, the raw offset it came from, and
// `lines` the line number of every raw offset: the two together keep the
// raw coordinates reachable from the token stream. The line numbers are
// the one observable of those passes and are reproduced exactly - a
// break inside a continuation counts at the next real break (the removed
// pass moved those newlines down) and the synthesized final newline
// counts at the end.
static char *make_scan_buffer(char *raw, int len, int **map_out, int **lines_out) {
  int *map = chibicc_arena_calloc(chibicc_ctx(), len + 2, sizeof(int));
  int *lines = chibicc_arena_calloc(chibicc_ctx(), len + 2, sizeof(int));
  char *buf = chibicc_arena_calloc(chibicc_ctx(), 1, len + 2);

  // The text the pre-passes used to see: the raw text plus the final
  // newline for a text that does not end in one.
  char *text = chibicc_arena_calloc(chibicc_ctx(), 1, len + 2);
  memcpy(text, raw, len);
  if (len == 0 || raw[len - 1] != '\n')
    text[len] = '\n';
  // The raw length, plus the synthesized final newline. Never strlen:
  // the raw text may contain a NUL, which tokenize_file diagnoses.
  int n = len;
  if (len == 0 || raw[len - 1] != '\n')
    n++;

  int i = 0;        // position in `text`
  int j = 0;        // position in `buf`
  int mark = 0;     // first raw offset without a line number
  int line = 1;     // the line number at `i`
  int pending = 0;  // continuation breaks not credited yet

  // A BOM at the very start is skipped and becomes part of the leading
  // trivia of the first token.
  if (!strncmp(text, "\xef\xbb\xbf", 3))
    i = 3;
  map[0] = i;

  while (i < n) {
    // Every raw offset the walk passes gets the line number of its
    // position; the offsets inside a continuation belong to the line
    // they are on.
    for (; mark <= i && mark <= len; mark++)
      lines[mark] = line;

    // A backslash before a line break joins the lines: both bytes are
    // dropped, and the break is not a break for the lexer.
    if (text[i] == '\\' && i + 1 < n && is_line_break(text + i + 1)) {
      pending++;
      i += 1 + line_break_len(text + i + 1);
      continue;
    }

    // A line break becomes a single '\n'.
    if (is_line_break(text + i)) {
      map[j] = i;
      buf[j++] = '\n';
      i += line_break_len(text + i);
      line += 1 + pending;
      pending = 0;
      continue;
    }

    // A universal character name becomes UTF-8; `raw` keeps the escape.
    if (text[i] == '\\' && (text[i + 1] == 'u' || text[i + 1] == 'U')) {
      int digs = text[i + 1] == 'u' ? 4 : 8;
      uint32_t c = read_universal_char(text + i + 2, digs);
      if (c) {
        int k = encode_utf8(buf + j, c);
        for (int e = 0; e < k; e++)
          map[j + e] = i;
        j += k;
        i += digs + 2;
        continue;
      }
    }

    // Any other backslash is copied together with the byte after it, so
    // that the "u" of an escaped "\\u" is not taken for an escape.
    if (text[i] == '\\' && i + 1 < len) {
      map[j] = i;
      buf[j++] = text[i++];
    }

    map[j] = i;
    buf[j++] = text[i++];
  }

  // The lexer needs the buffer to end in a newline: a line comment runs
  // to the next one. The old passes kept the synthesized newline when a
  // trailing continuation swallowed it, by materializing the pending
  // breaks at the end.
  if (j == 0 || buf[j - 1] != '\n') {
    map[j] = len;
    buf[j++] = '\n';
  }

  // Credit the continuation breaks that never saw a real break after
  // them, and give the file end (where the EOF token sits) the final
  // number.
  line += pending;
  for (; mark <= len; mark++)
    lines[mark] = line;
  lines[len] = line;

  map[j] = len;
  *map_out = map;
  *lines_out = lines;
  return buf;
}

Token *tokenize_file(char *path) {
  size_t raw_len;
  char *raw = read_file(path, &raw_len);
  if (!raw)
    return NULL;

  int *map, *lines;
  char *contents = make_scan_buffer(raw, (int)raw_len, &map, &lines);

  // Save the filename for assembler .file directive.
  int file_no = chibicc_ctx()->next_file_no++;
  File *file = new_file(path, file_no + 1, contents);

  // The scan buffer replaces the identity tables new_file built.
  file->raw = raw;
  file->map = map;
  file->lines = lines;

  // A NUL byte would silently end the token stream. The scan buffer now
  // contains it (make_scan_buffer no longer stops at strlen), so report
  // the first one before tokenizing.
  if (memchr(raw, 0, raw_len)) {
    chibicc_ctx()->current_file = file;
    error_at(chibicc_ctx(), memchr(contents, 0, raw_len + 2), "NUL byte in source file");
  }

  // Save the filename for assembler .file directive.
  ChibiccContext *ctx = chibicc_ctx();
  if (ctx->input_files_cap < file_no + 2) {
    size_t old = sizeof(File *) * ctx->input_files_cap;
    int cap = file_no + 2;
    ctx->input_files = chibicc_arena_realloc(chibicc_ctx(), ctx->input_files, old,
                                             sizeof(File *) * cap);
    ctx->input_files_cap = cap;
  }
  ctx->input_files[file_no] = file;
  ctx->input_files[file_no + 1] = NULL;
  ctx->input_files_len = file_no + 1;
  file_no++;

  return tokenize(file);
}
