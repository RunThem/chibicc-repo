// 第 1 层词法分析: 把扫描缓冲区切分成 token 流.
// 原始文本 (raw) 与扫描文本 (contents) 双缓冲并行, 由 map / lines 两张
// 表互相映射; 每个 token 携带 trivia 与 raw 坐标, 因此该流可以逐字节
// 重建输入文件. 本文件同时提供诊断入口与 token 比较等共用设施.
#include "libcp.h"

// The input file, the input file list and the current position flags
// live in LibcpContext (PLAN L2.2). The lexer reads them through the
// active context like every other phase.
//
// 输入文件, 输入文件列表与当前位置标志都存放在 LibcpContext 中
// (PLAN L2.2). 与其它阶段一样, 词法分析器通过当前活动的 context 读取它们.

// The raw offset of a position in the scan buffer (PLAN T2.1): the two
// texts differ where a continuation, a '\r' or a universal character
// name was rewritten. They coincide for synthetic buffers.
//
// 扫描缓冲区中某个位置的 raw 偏移 (PLAN T2.1): 在续行, '\r' 或
// universal character name 被改写的地方, 两份文本并不相同.
// 对合成缓冲区来说两者完全一致.
static int raw_off(LibcpFile* file, char* p) {
  return file->map[p - file->contents];
}

// The raw offset just after a token's spelling: the bytes skipped in
// front of the next token start there.
//
// token 拼写结束之后的 raw 偏移: 下一个 token 前面被跳过的字节
// 从这里开始.
static int raw_end(LibcpToken* tok) {
  return tok->raw_loc - tok->file->raw + tok->raw_len;
}

// 把一个十六进制数字字符转成数值; 调用方保证 c 是十六进制数字.
static int from_hex(char c) {
  if ('0' <= c && c <= '9')
    return c - '0';
  if ('a' <= c && c <= 'f')
    return c - 'a' + 10;
  return c - 'A' + 10;
}

// 解析 \u / \U 之后的 len 个十六进制数字; 遇到非十六进制数字时返回 0.
static uint32_t read_universal_char(char* p, int len) {
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
//
// 产生扫描偏移 `off` 的那些 raw 字节之后的 raw 偏移: 换行与
// universal character name 对应多个 raw 字节, 其余情况只对应一个.
static int raw_end_of(LibcpFile* file, int off) {
  int lo  = file->map[off];
  char* p = file->raw + lo;

  if (libcp_is_line_break(p))
    return lo + libcp_line_break_len(p);

  if (p[0] == '\\' && (p[1] == 'u' || p[1] == 'U')) {
    int digs = p[1] == 'u' ? 4 : 8;
    if (read_universal_char(p + 2, digs))
      return lo + digs + 2;
  }
  return lo + 1;
}

// Formats a diagnostic message into the arena (or malloc before a
// context exists).
//
// 把诊断消息格式化进 arena (context 尚不存在时退回 malloc).
static char* vformat(LibcpContext* ctx, char* fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  char* buf = libcp_arena_alloc(ctx, n + 1, 1);
  vsnprintf(buf, n + 1, fmt, ap);
  return buf;
}

// Unwinds to the innermost public-entry frame; without one (the
// driver's pre-cc1 paths), exits as before.
//
// 回退到最内层的公共入口栈帧; 没有这样的栈帧时 (驱动器在 cc1 之前的
// 路径), 与以前一样直接退出.
static noreturn void raise_error(LibcpContext* ctx) {
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
//
// 按以上格式报告错误消息.
//
// 注册过回调时改走结构化形式; 未注册时的默认路径与 L2.6 之前编译器
// 的输出逐字节一致.
static void verror_at(LibcpContext* ctx,
                      char* filename,
                      char* input,
                      int line_no,
                      char* loc,
                      LibcpToken* tok,
                      LibcpDiagnosticSeverity severity,
                      char* fmt,
                      va_list ap) {

  // Find a line containing `loc`.
  // 找到包含 `loc` 的那一行.
  char* line = loc;
  while (input < line && line[-1] != '\n')
    line--;

  if (ctx && ctx->diag_fn) {
    char* msg         = vformat(ctx, fmt, ap);
    LibcpDiagnostic d = {filename,
                         line_no,
                         libcp_display_width(ctx, line, loc - line) + 1,
                         severity,
                         msg,
                         tok,
                         tok ? tok->prov : NULL};
    ctx->diag_fn(ctx->diag_user, &d);
    return;
  }

  char* end = loc;
  while (*end && *end != '\n')
    end++;

  // Print out the line.
  // 打印出这一行源码.
  int indent = fprintf(stderr, "%s:%d: ", filename, line_no);
  fprintf(stderr, "%.*s\n", (int)(end - line), line);

  // Show the error message.
  // 显示错误消息.
  int pos = libcp_display_width(ctx, line, loc - line) + indent;

  fprintf(stderr, "%*s", pos, "");  // print pos spaces.
                                    // 打印 pos 个空格.
  fprintf(stderr, "^ ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
}

// Reports an error; unwinds to the current public entry, or exits when
// the driver is outside one.
//
// 报告错误; 回退到当前公共入口, 若驱动器不在任何公共入口内则直接退出.
void libcp_error(LibcpContext* ctx, char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);

  if (ctx && ctx->diag_fn) {
    char* msg         = vformat(ctx, fmt, ap);
    LibcpDiagnostic d = {NULL, 0, 0, LIBCP_DIAG_ERROR, msg, NULL, NULL};
    ctx->diag_fn(ctx->diag_user, &d);
  } else {
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
  }

  va_end(ap);
  raise_error(ctx);
}

void libcp_error_at(LibcpContext* ctx, char* loc, char* fmt, ...) {
  int line_no = ctx->current_file->lines[raw_off(ctx->current_file, loc)];

  va_list ap;
  va_start(ap, fmt);
  verror_at(ctx,
            ctx->current_file->name,
            ctx->current_file->contents,
            line_no,
            loc,
            NULL,
            LIBCP_DIAG_ERROR,
            fmt,
            ap);
  va_end(ap);
  raise_error(ctx);
}

void libcp_error_tok(LibcpContext* ctx, LibcpToken* tok, char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  verror_at(ctx,
            tok->file->name,
            tok->file->contents,
            tok->line_no,
            tok->loc,
            tok,
            LIBCP_DIAG_ERROR,
            fmt,
            ap);
  va_end(ap);
  raise_error(ctx);
}

void libcp_warn_tok(LibcpContext* ctx, LibcpToken* tok, char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  verror_at(ctx,
            tok->file->name,
            tok->file->contents,
            tok->line_no,
            tok->loc,
            tok,
            LIBCP_DIAG_WARNING,
            fmt,
            ap);
  va_end(ap);
}

// The dump name of a token kind. Lives here, not in the driver, so the
// provenance channel can use it without main.c.
//
// token 种类的 dump 名称. 放在这里而不是驱动器里, 这样 provenance 通道
// 无需 main.c 就能使用它.
char* libcp_token_kind_name(LibcpContext* ctx, LibcpTokenKind kind) {
  switch (kind) {
    case TK_IDENT: return "ident";
    case TK_PUNCT: return "punct";
    case TK_KEYWORD: return "keyword";
    case TK_STR: return "str";
    case TK_NUM: return "num";
    case TK_PP_NUM: return "pp-num";
    case TK_EOF: return "eof";
  }
  unreachable();
}

// Consumes the current token if it matches `op`.
// 若当前 token 与 `op` 匹配则消耗它.
bool libcp_equal(LibcpToken* tok, char* op) {
  // The length check comes first: a token longer than `op` must not make
  // memcmp read past the end of `op` (ASan found this in L3.4c).
  //
  // 先比较长度: 比 `op` 更长的 token 不得让 memcmp 读过 `op` 的末尾
  // (L3.4c 中由 ASan 发现).
  return (int)strlen(op) == tok->len && memcmp(tok->loc, op, tok->len) == 0;
}

// Ensure that the current token is `op`.
// 确保当前 token 是 `op`, 否则报错.
LibcpToken* libcp_skip(LibcpContext* ctx, LibcpToken* tok, char* op) {
  if (!libcp_equal(tok, op))
    libcp_error_tok(ctx, tok, "expected '%s'", op);
  return tok->next;
}

bool libcp_consume(LibcpToken** rest, LibcpToken* tok, char* str) {
  if (libcp_equal(tok, str)) {
    *rest = tok->next;
    return true;
  }
  *rest = tok;
  return false;
}

// Create a new token. `raw_trivia` is the raw offset where the bytes
// skipped in front of this token start (see LibcpToken.trivia).
//
// 新建一个 token. `raw_trivia` 是本 token 前面被跳过的字节起始处的
// raw 偏移 (见 LibcpToken.trivia).
static LibcpToken*
    new_token(LibcpContext* ctx, LibcpTokenKind kind, char* start, char* end, int raw_trivia) {
  LibcpToken* tok = libcp_arena_calloc(ctx, 1, sizeof(LibcpToken));
  tok->kind       = kind;
  tok->loc        = start;
  tok->len        = end - start;

  int raw_start   = raw_off(ctx->current_file, start);
  tok->trivia     = ctx->current_file->raw + raw_trivia;
  tok->trivia_len = raw_start - raw_trivia;
  tok->raw_loc    = ctx->current_file->raw + raw_start;
  tok->raw_len =
      start == end ?
          0 :
          raw_end_of(ctx->current_file, end - ctx->current_file->contents - 1) - raw_start;

  tok->file      = ctx->current_file;
  tok->filename  = ctx->current_file->display_name;
  tok->at_bol    = ctx->at_bol;
  tok->has_space = ctx->has_space;

  ctx->at_bol = ctx->has_space = false;
  return tok;
}

static bool startswith(char* p, char* q) {
  return strncmp(p, q, strlen(q)) == 0;
}

// Read an identifier and returns the length of it.
// If p does not point to a valid identifier, 0 is returned.
//
// 读一个标识符并返回它的长度. 若 p 不指向合法标识符, 返回 0.
static int read_ident(LibcpContext* ctx, char* start) {
  char* p    = start;
  uint32_t c = libcp_decode_utf8(ctx, &p, p);
  if (!libcp_is_ident1(c))
    return 0;

  for (;;) {
    char* q;
    c = libcp_decode_utf8(ctx, &q, p);
    if (!libcp_is_ident2(c))
      return p - start;
    p = q;
  }
}

// Read a punctuator token from p and returns its length.
// 从 p 读一个标点 token 并返回它的长度.
static int read_punct(char* p) {
  static char* kw[] = {
      "<<=", ">>=", "...", "==", "!=", "<=", ">=", "->", "+=", "-=", "*=", "/=",
      "++",  "--",  "%=",  "&=", "|=", "^=", "&&", "||", "<<", ">>", "##",
  };

  for (int i = 0; i < sizeof(kw) / sizeof(*kw); i++)
    if (startswith(p, kw[i]))
      return strlen(kw[i]);

  return ispunct((unsigned char)*p) ? 1 : 0;
}

// 判断 token 是否为关键字; 关键字表在首次调用时惰性建入 context 的 hashmap.
static bool is_keyword(LibcpContext* ctx, LibcpToken* tok) {
  LibcpHashMap* map = &ctx->keyword_map;

  if (map->capacity == 0) {
    static char* kw[] = {
        "return",    "if",         "else",
        "for",       "while",      "int",
        "sizeof",    "char",       "struct",
        "union",     "short",      "long",
        "void",      "typedef",    "_Bool",
        "enum",      "static",     "goto",
        "break",     "continue",   "switch",
        "case",      "default",    "extern",
        "_Alignof",  "_Alignas",   "do",
        "signed",    "unsigned",   "const",
        "volatile",  "auto",       "register",
        "restrict",  "__restrict", "__restrict__",
        "_Noreturn", "float",      "double",
        "typeof",    "asm",        "_Thread_local",
        "__thread",  "_Atomic",    "__attribute__",
    };

    for (int i = 0; i < sizeof(kw) / sizeof(*kw); i++)
      libcp_hashmap_put(ctx, map, kw[i], (void*)1);
  }

  return libcp_hashmap_get2(ctx, map, tok->loc, tok->len);
}

// 解析并返回 p 处的一个转义序列, 同时把 *new_pos 推进到序列之后.
static int read_escaped_char(LibcpContext* ctx, char** new_pos, char* p) {
  if ('0' <= *p && *p <= '7') {
    // Read an octal number.
    // 读一个八进制数.
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
    // 读一个十六进制数.
    p++;
    if (!isxdigit((unsigned char)*p))
      libcp_error_at(ctx, p, "invalid hex escape sequence");

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
  //
  // 这里的转义序列用它们自身来定义. 例如 '\n' 就是用 '\n' 实现的.
  // 这个同义反复的定义能成立, 是因为编译我们编译器的那个编译器知道
  // '\n' 到底是什么. 换句话说, 我们从编译我们编译器的编译器那里
  // "继承" 了 '\n' 的 ASCII 码, 因此不必在这里重新教实际代码.
  //
  // 这一事实不仅影响编译器的正确性, 也深刻影响生成代码的安全性.
  // 更多信息见 Ken Thompson 的 "Reflections on Trusting Trust" 一文.
  switch (*p) {
    case 'a': return '\a';
    case 'b': return '\b';
    case 't': return '\t';
    case 'n': return '\n';
    case 'v': return '\v';
    case 'f': return '\f';
    case 'r': return '\r';
    // [GNU] \e for the ASCII escape character is a GNU C extension.
    // [GNU] \e 表示 ASCII 转义字符, 这是 GNU C 的扩展.
    case 'e': return 27;
    default: return *p;
  }
}

// Find a closing double-quote.
// 找到收尾的双引号.
static char* string_literal_end(LibcpContext* ctx, char* p) {
  char* start = p;
  for (; *p != '"'; p++) {
    if (*p == '\n' || *p == '\0')
      libcp_error_at(ctx, start, "unclosed string literal");
    if (*p == '\\')
      p++;
  }
  return p;
}

static LibcpToken*
    read_string_literal(LibcpContext* ctx, char* start, char* quote, int raw_trivia) {
  char* end = string_literal_end(ctx, quote + 1);
  char* buf = libcp_arena_calloc(ctx, 1, end - quote);
  int len   = 0;

  for (char* p = quote + 1; p < end;) {
    if (*p == '\\')
      buf[len++] = read_escaped_char(ctx, &p, p + 1);
    else
      buf[len++] = *p++;
  }

  LibcpToken* tok = new_token(ctx, TK_STR, start, end + 1, raw_trivia);
  tok->ty         = libcp_array_of(ctx, libcp_ty_char, len + 1);
  tok->str        = buf;
  return tok;
}

// Read a UTF-8-encoded string literal and transcode it in UTF-16.
//
// UTF-16 is yet another variable-width encoding for Unicode. Code
// points smaller than U+10000 are encoded in 2 bytes. Code points
// equal to or larger than that are encoded in 4 bytes. Each 2 bytes
// in the 4 byte sequence is called "surrogate", and a 4 byte sequence
// is called a "surrogate pair".
//
// 读一个 UTF-8 编码的字符串字面量, 并把它转码为 UTF-16.
//
// UTF-16 是 Unicode 的另一种变长编码. 小于 U+10000 的码点用 2 字节
// 编码. 大于或等于该值的码点用 4 字节编码. 4 字节序列中的每个 2 字节
// 称为 "surrogate", 整个 4 字节序列称为 "surrogate pair".
static LibcpToken*
    read_utf16_string_literal(LibcpContext* ctx, char* start, char* quote, int raw_trivia) {
  char* end     = string_literal_end(ctx, quote + 1);
  uint16_t* buf = libcp_arena_calloc(ctx, 2, end - start);
  int len       = 0;

  for (char* p = quote + 1; p < end;) {
    if (*p == '\\') {
      buf[len++] = read_escaped_char(ctx, &p, p + 1);
      continue;
    }

    uint32_t c = libcp_decode_utf8(ctx, &p, p);
    if (c < 0x10000) {
      // Encode a code point in 2 bytes.
      // 把码点编码为 2 字节.
      buf[len++] = c;
    } else {
      // Encode a code point in 4 bytes.
      // 把码点编码为 4 字节.
      c -= 0x10000;
      buf[len++] = 0xd800 + ((c >> 10) & 0x3ff);
      buf[len++] = 0xdc00 + (c & 0x3ff);
    }
  }

  LibcpToken* tok = new_token(ctx, TK_STR, start, end + 1, raw_trivia);
  tok->ty         = libcp_array_of(ctx, libcp_ty_ushort, len + 1);
  tok->str        = (char*)buf;
  return tok;
}

// Read a UTF-8-encoded string literal and transcode it in UTF-32.
//
// UTF-32 is a fixed-width encoding for Unicode. Each code point is
// encoded in 4 bytes.
//
// 读一个 UTF-8 编码的字符串字面量, 并把它转码为 UTF-32.
//
// UTF-32 是 Unicode 的定长编码. 每个码点占 4 字节.
static LibcpToken* read_utf32_string_literal(LibcpContext* ctx,
                                             char* start,
                                             char* quote,
                                             LibcpType* ty,
                                             int raw_trivia) {
  char* end     = string_literal_end(ctx, quote + 1);
  uint32_t* buf = libcp_arena_calloc(ctx, 4, end - quote);
  int len       = 0;

  for (char* p = quote + 1; p < end;) {
    if (*p == '\\')
      buf[len++] = read_escaped_char(ctx, &p, p + 1);
    else
      buf[len++] = libcp_decode_utf8(ctx, &p, p);
  }

  LibcpToken* tok = new_token(ctx, TK_STR, start, end + 1, raw_trivia);
  tok->ty         = libcp_array_of(ctx, ty, len + 1);
  tok->str        = (char*)buf;
  return tok;
}

// 读字符字面量: 取一个字符 (或转义序列) 的值, 生成 TK_NUM token.
static LibcpToken*
    read_char_literal(LibcpContext* ctx, char* start, char* quote, LibcpType* ty, int raw_trivia) {
  char* p = quote + 1;
  if (*p == '\0')
    libcp_error_at(ctx, start, "unclosed char literal");

  int c;
  if (*p == '\\')
    c = read_escaped_char(ctx, &p, p + 1);
  else
    c = libcp_decode_utf8(ctx, &p, p);

  char* end = strchr(p, '\'');
  if (!end)
    libcp_error_at(ctx, p, "unclosed char literal");

  LibcpToken* tok = new_token(ctx, TK_NUM, start, end + 1, raw_trivia);
  tok->val        = c;
  tok->ty         = ty;
  return tok;
}

// 把 pp-number 按整数常量解析: 识别底数, 处理溢出与 U/L/LL 后缀, 再按值
// 推断类型; 不是合法整数时返回 false, 交给浮点分支.
static bool convert_pp_int(LibcpContext* ctx, LibcpToken* tok) {
  char* p = tok->loc;

  // Read a binary, octal, decimal or hexadecimal number.
  // 读一个二进制, 八进制, 十进制或十六进制数.
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
  //
  // strtoul 在溢出时会饱和到 ULONG_MAX 并设置 ERANGE; 不能让无法表示的
  // 整数字面量悄悄变成 ULONG_MAX.
  errno       = 0;
  int64_t val = strtoul(p, &p, base);
  if (errno == ERANGE)
    libcp_error_tok(ctx, tok, "integer literal is too large");

  // Read U, L or LL suffixes.
  // 读 U, L 或 LL 后缀.
  bool l = false;
  bool u = false;

  if (startswith(p, "LLU") || startswith(p, "LLu") || startswith(p, "llU") ||
      startswith(p, "llu") || startswith(p, "ULL") || startswith(p, "Ull") ||
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
  // 推断类型.
  LibcpType* ty;
  if (base == 10) {
    if (l && u)
      ty = libcp_ty_ulong;
    else if (l)
      ty = libcp_ty_long;
    else if (u)
      ty = (val >> 32) ? libcp_ty_ulong : libcp_ty_uint;
    else
      ty = (val >> 31) ? libcp_ty_long : libcp_ty_int;
  } else {
    if (l && u)
      ty = libcp_ty_ulong;
    else if (l)
      ty = (val >> 63) ? libcp_ty_ulong : libcp_ty_long;
    else if (u)
      ty = (val >> 32) ? libcp_ty_ulong : libcp_ty_uint;
    else if (val >> 63)
      ty = libcp_ty_ulong;
    else if (val >> 32)
      ty = libcp_ty_long;
    else if (val >> 31)
      ty = libcp_ty_uint;
    else
      ty = libcp_ty_int;
  }

  tok->kind = TK_NUM;
  tok->val  = val;
  tok->ty   = ty;
  return true;
}

// The definition of the numeric literal at the preprocessing stage
// is more relaxed than the definition of that at the later stages.
// In order to handle that, a numeric literal is tokenized as a
// "pp-number" token first and then converted to a regular number
// token after preprocessing.
//
// This function converts a pp-number token to a regular number token.
//
// 预处理阶段对数值字面量的定义比后续阶段宽松. 为了处理这一点,
// 数值字面量先被 tokenize 成 "pp-number" token, 预处理之后再转换成
// 普通的数值 token.
//
// 本函数把一个 pp-number token 转换成普通数值 token.
static void convert_pp_number(LibcpContext* ctx, LibcpToken* tok) {
  // Try to parse as an integer constant.
  // 先尝试按整数常量解析.
  if (convert_pp_int(ctx, tok))
    return;

  // If it's not an integer, it must be a floating point constant.
  // 若不是整数, 那就必须是浮点常量.
  char* end;
  long double val = strtold(tok->loc, &end);

  LibcpType* ty;
  if (*end == 'f' || *end == 'F') {
    ty = libcp_ty_float;
    end++;
  } else if (*end == 'l' || *end == 'L') {
    ty = libcp_ty_ldouble;
    end++;
  } else {
    ty = libcp_ty_double;
  }

  if (tok->loc + tok->len != end)
    libcp_error_tok(ctx, tok, "invalid numeric constant");

  tok->kind = TK_NUM;
  tok->fval = val;
  tok->ty   = ty;
}

// 预处理结束后调用: 标记关键字, 并把 pp-number 转换成普通数值 token.
void libcp_convert_pp_tokens(LibcpContext* ctx, LibcpToken* tok) {
  for (LibcpToken* t = tok; t->kind != TK_EOF; t = t->next) {
    if (is_keyword(ctx, t))
      t->kind = TK_KEYWORD;
    else if (t->kind == TK_PP_NUM)
      convert_pp_number(ctx, t);
  }
}

// Initialize line info for all tokens.
// 为所有 token 填上行号信息.
static void add_line_numbers(LibcpToken* tok) {
  for (; tok; tok = tok->next)
    tok->line_no = tok->file->lines[raw_off(tok->file, tok->loc)];
}

LibcpToken* libcp_tokenize_string_literal(LibcpContext* ctx, LibcpToken* tok, LibcpType* basety) {
  // The re-scan needs the raw coordinates of the token it replaces, so
  // scan against the file that token belongs to (the running
  // `current_file` is whichever file was tokenized last). The `file` and
  // `filename` fields are restored afterwards: they used to be the stale
  // ones, and that pre-existing behavior is kept for now.
  //
  // 重新扫描需要被替换 token 的 raw 坐标, 因此要对它所属的文件扫描
  // (当前的 `current_file` 是最后被 tokenize 的那个文件). 事后再恢复
  // `file` 与 `filename` 字段: 它们以前是陈旧的值, 这一既有行为暂时保留.
  LibcpFile* save   = ctx->current_file;
  ctx->current_file = tok->file;

  LibcpToken* t;
  if (basety->size == 2)
    t = read_utf16_string_literal(ctx, tok->loc, tok->loc, tok->trivia - tok->file->raw);
  else
    t = read_utf32_string_literal(ctx, tok->loc, tok->loc, basety, tok->trivia - tok->file->raw);

  ctx->current_file = save;
  t->file           = save;
  t->filename       = save->display_name;
  t->next           = tok->next;
  return t;
}

// Tokenize a given string and returns new tokens.
// 对给定的文本做词法分析, 返回新的 token 流.
LibcpToken* libcp_tokenize(LibcpContext* ctx, LibcpFile* file) {
  ctx->current_file = file;

  char* p = file->contents;

  // The raw offset where the bytes skipped in front of the next token
  // start. The four skip loops below only advance `p`; the bytes they
  // pass over are recorded as the trivia slice of the next token.
  //
  // 下一个 token 前面被跳过的字节起始处的 raw 偏移. 下面四个跳过循环
  // 只推进 `p`; 它们越过的字节会记录为下一个 token 的 trivia 片段.
  int raw_trivia = 0;

  LibcpToken head = {};
  LibcpToken* cur = &head;

  ctx->at_bol    = true;
  ctx->has_space = false;

  while (*p) {
    // Skip line comments.
    // 跳过行注释.
    if (startswith(p, "//")) {
      p += 2;
      while (*p != '\n')
        p++;
      ctx->has_space = true;
      continue;
    }

    // Skip block comments.
    // 跳过块注释.
    if (startswith(p, "/*")) {
      char* q = strstr(p + 2, "*/");
      if (!q)
        libcp_error_at(ctx, p, "unclosed block comment");
      p              = q + 2;
      ctx->has_space = true;
      continue;
    }

    // Skip newline.
    // 跳过换行.
    if (*p == '\n') {
      p++;
      ctx->at_bol    = true;
      ctx->has_space = false;
      continue;
    }

    // Skip whitespace characters.
    // 跳过空白字符.
    if (isspace((unsigned char)*p)) {
      p++;
      ctx->has_space = true;
      continue;
    }

    // Numeric literal
    // 数值字面量
    if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
      char* q = p++;
      for (;;) {
        if (p[0] && p[1] && strchr("eEpP", p[0]) && strchr("+-", p[1]))
          p += 2;
        else if (isalnum((unsigned char)*p) || *p == '.')
          p++;
        else
          break;
      }
      cur = cur->next = new_token(ctx, TK_PP_NUM, q, p, raw_trivia);
      raw_trivia      = raw_end(cur);
      continue;
    }

    // String literal
    // 字符串字面量
    if (*p == '"') {
      cur = cur->next = read_string_literal(ctx, p, p, raw_trivia);
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // UTF-8 string literal
    // UTF-8 字符串字面量
    if (startswith(p, "u8\"")) {
      cur = cur->next = read_string_literal(ctx, p, p + 2, raw_trivia);
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // UTF-16 string literal
    // UTF-16 字符串字面量
    if (startswith(p, "u\"")) {
      cur = cur->next = read_utf16_string_literal(ctx, p, p + 1, raw_trivia);
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // Wide string literal
    // 宽字符串字面量
    if (startswith(p, "L\"")) {
      cur = cur->next = read_utf32_string_literal(ctx, p, p + 1, libcp_ty_int, raw_trivia);
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // UTF-32 string literal
    // UTF-32 字符串字面量
    if (startswith(p, "U\"")) {
      cur = cur->next = read_utf32_string_literal(ctx, p, p + 1, libcp_ty_uint, raw_trivia);
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // Character literal
    // 字符字面量
    if (*p == '\'') {
      cur = cur->next = read_char_literal(ctx, p, p, libcp_ty_int, raw_trivia);
      cur->val        = (char)cur->val;
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // UTF-16 character literal
    // UTF-16 字符字面量
    if (startswith(p, "u'")) {
      cur = cur->next = read_char_literal(ctx, p, p + 1, libcp_ty_ushort, raw_trivia);
      cur->val &= 0xffff;
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // Wide character literal
    // 宽字符字面量
    if (startswith(p, "L'")) {
      cur = cur->next = read_char_literal(ctx, p, p + 1, libcp_ty_int, raw_trivia);
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // UTF-32 character literal
    // UTF-32 字符字面量
    if (startswith(p, "U'")) {
      cur = cur->next = read_char_literal(ctx, p, p + 1, libcp_ty_uint, raw_trivia);
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // Identifier or keyword
    // 标识符或关键字
    int ident_len = read_ident(ctx, p);
    if (ident_len) {
      cur = cur->next = new_token(ctx, TK_IDENT, p, p + ident_len, raw_trivia);
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    // Punctuators
    // 标点
    int punct_len = read_punct(p);
    if (punct_len) {
      cur = cur->next = new_token(ctx, TK_PUNCT, p, p + punct_len, raw_trivia);
      p += cur->len;
      raw_trivia = raw_end(cur);
      continue;
    }

    libcp_error_at(ctx, p, "invalid token");
  }

  // The trailing trivia of the file belongs to the EOF token.
  // 文件末尾的 trivia 归属于 EOF token.
  cur = cur->next = new_token(ctx, TK_EOF, p, p, raw_trivia);
  add_line_numbers(head.next);
  return head.next;
}

// Returns the contents of a given file, and its length in *len. The
// text is returned exactly as it is on disk: the rewrites that used to
// happen here (a synthesized final newline, the BOM skip) moved into
// make_scan_buffer, which can remember them in raw coordinates.
//
// 返回给定文件的内容, 并通过 *len 返回它的长度. 文本按磁盘上的原样返回:
// 过去在这里做的改写 (合成末尾换行, 跳过 BOM) 已移入 make_scan_buffer,
// 由它用 raw 坐标记住这些改写.
static char* read_file(LibcpContext* ctx, char* path, size_t* len) {
  FILE* fp;

  if (strcmp(path, "-") == 0) {
    // By convention, read from stdin if a given filename is "-". stdin
    // cannot be rewound and its EOF flag is sticky, so the first read is
    // cached in the context: a second tokenize of the same stream then
    // sees the same text instead of a silently empty one (C12).
    //
    // 按惯例, 文件名为 "-" 时从 stdin 读取. stdin 无法回退, 且其 EOF
    // 标志是粘滞的, 因此第一次读取的结果缓存在 context 里: 再次 tokenize
    // 同一流时会看到相同的文本, 而不是静默地读到空内容 (C12).
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

  char* buf;
  size_t buflen;
  FILE* out = open_memstream(&buf, &buflen);

  // Read the entire file.
  // 读取整个文件.
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
  //
  // 词法分析器用 int 保存偏移; 更大的文件会被静默截断, 因此直接拒绝 (C12).
  if (buflen > INT32_MAX) {
    fclose(out);
    free(buf);
    libcp_error(ctx, "file too large: %s", path);
  }

  // fputc below grows the memstream's length by the terminator; keep the
  // text length itself for the callers and the stdin cache.
  //
  // 下面的 fputc 会让 memstream 的长度把终止符也算进去; 这里先保留纯文本
  // 长度, 供调用方与 stdin 缓存使用.
  size_t data_len = buflen;
  *len            = data_len;
  fputc('\0', out);
  fclose(out);

  // The memstream buffer belongs to the C library's malloc, not to
  // LIBCP_MALLOC, so it is released with free(); the text is moved
  // into the arena so a context reset releases it with everything else.
  //
  // memstream 的缓冲区属于 C 库的 malloc, 而不是 LIBCP_MALLOC, 因此用
  // free() 释放; 文本随后搬进 arena, 这样 context reset 时会连同其它
  // per-TU 内存一起释放.
  if (ctx) {
    char* copy = libcp_arena_alloc(ctx, data_len + 1, 1);
    memcpy(copy, buf, data_len + 1);
    free(buf);
    if (strcmp(path, "-") == 0) {
      ctx->stdin_text = copy;
      ctx->stdin_len  = data_len;
    }
    return copy;
  }
  return buf;
}

LibcpFile** libcp_get_input_files(LibcpContext* ctx) {
  return ctx->input_files;
}

LibcpFile* libcp_new_file_len(LibcpContext* ctx, char* name, int file_no, char* contents, int len) {
  LibcpFile* file    = libcp_arena_calloc(ctx, 1, sizeof(LibcpFile));
  file->name         = name;
  file->display_name = name;
  file->file_no      = file_no;
  file->contents     = contents;

  // A synthetic buffer is scanned as it is: it is its own raw text, the
  // map is the identity and the line numbers are the plain newline
  // count. (libcp_tokenize_file_impl replaces these tables with the ones
  // make_scan_buffer builds.)
  //
  // 合成缓冲区按原样扫描: 它自身就是 raw 文本, map 是恒等映射, 行号就是
  // 简单的换行计数. (libcp_tokenize_file_impl 会用 make_scan_buffer 建出的
  // 表替换这些表.)
  file->raw   = contents;
  file->map   = libcp_arena_calloc(ctx, len + 2, sizeof(int));
  file->lines = libcp_arena_calloc(ctx, len + 2, sizeof(int));

  int line = 1;
  for (int i = 0; i <= len; i++) {
    file->map[i]   = i;
    file->lines[i] = line;
    if (i < len && contents[i] == '\n')
      line++;
  }
  return file;
}

LibcpFile* libcp_new_file(LibcpContext* ctx, char* name, int file_no, char* contents) {
  return libcp_new_file_len(ctx, name, file_no, contents, strlen(contents));
}

// True if p starts a line break: '\n', '\r\n' or '\r'.
// 若 p 处是一个换行则为 true: '\n', '\r\n' 或 '\r'.
bool libcp_is_line_break(char* p) {
  return *p == '\n' || *p == '\r';
}

// The length of the line break at p.
// p 处换行的长度.
int libcp_line_break_len(char* p) {
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
//
// 由 raw 文本为真实文件构建词法分析器扫描的缓冲区 (PLAN T2.1). 扫描文本
// 与过去三个前置 pass 的产物一致 - 换行规范化为 '\n', 换行前的反斜杠连同
// 换行一起删除, universal character name 改写为 UTF-8, 跳过开头的 BOM,
// 并保证末尾有换行 - 这样每个消费者看到的文本与以前相同. `map` 记录每个
// 扫描偏移来自哪个 raw 偏移, `lines` 记录每个 raw 偏移的行号: 两者合起来
// 让 token 流仍可抵达 raw 坐标. 行号是这些 pass 唯一的可观察结果, 并被
// 精确复现 - 续行内部的换行记在下一个真实换行处 (被删除的 pass 把那些
// 换行下移), 合成的末尾换行则记在最后.
static char*
    make_scan_buffer(LibcpContext* ctx, char* raw, int len, int** map_out, int** lines_out) {
  int* map   = libcp_arena_calloc(ctx, len + 2, sizeof(int));
  int* lines = libcp_arena_calloc(ctx, len + 2, sizeof(int));
  char* buf  = libcp_arena_calloc(ctx, 1, len + 2);

  // The text the pre-passes used to see: the raw text plus the final
  // newline for a text that does not end in one.
  //
  // 前置 pass 过去看到的文本: raw 文本, 若不以换行结尾则补一个末尾换行.
  char* text = libcp_arena_calloc(ctx, 1, len + 2);
  memcpy(text, raw, len);
  if (len == 0 || raw[len - 1] != '\n')
    text[len] = '\n';
  // The raw length, plus the synthesized final newline. Never strlen:
  // the raw text may contain a NUL, which libcp_tokenize_file_impl diagnoses.
  //
  // raw 长度, 再加上合成的末尾换行. 绝不能用 strlen: raw 文本可能含有 NUL,
  // 这种情况由 libcp_tokenize_file_impl 诊断.
  int n = len;
  if (len == 0 || raw[len - 1] != '\n')
    n++;

  int i       = 0;  // position in `text`
                    // `text` 中的位置
  int j       = 0;  // position in `buf`
                    // `buf` 中的位置
  int mark    = 0;  // first raw offset without a line number
                    // 第一个还没有行号的 raw 偏移
  int line    = 1;  // the line number at `i`
                    // `i` 处的行号
  int pending = 0;  // continuation breaks not credited yet
                    // 尚未计入的续行换行数

  // A BOM at the very start is skipped and becomes part of the leading
  // trivia of the first token.
  //
  // 开头的 BOM 会被跳过, 并成为第一个 token 的前导 trivia 的一部分.
  if (!strncmp(text, "\xef\xbb\xbf", 3))
    i = 3;
  map[0] = i;

  while (i < n) {
    // Every raw offset the walk passes gets the line number of its
    // position; the offsets inside a continuation belong to the line
    // they are on.
    //
    // 遍历经过的每个 raw 偏移都取它所在位置的行号; 续行内部的偏移属于
    // 它们所在的那一行.
    for (; mark <= i && mark <= len; mark++)
      lines[mark] = line;

    // A backslash before a line break joins the lines: both bytes are
    // dropped, and the break is not a break for the lexer.
    //
    // 换行前的反斜杠让两行连接起来: 两个字节都被丢弃, 该换行对词法
    // 分析器来说不算换行.
    if (text[i] == '\\' && i + 1 < n && libcp_is_line_break(text + i + 1)) {
      pending++;
      i += 1 + libcp_line_break_len(text + i + 1);
      continue;
    }

    // A line break becomes a single '\n'.
    // 换行被规范化为单个 '\n'.
    if (libcp_is_line_break(text + i)) {
      map[j]   = i;
      buf[j++] = '\n';
      i += libcp_line_break_len(text + i);
      line += 1 + pending;
      pending = 0;
      continue;
    }

    // A universal character name becomes UTF-8; `raw` keeps the escape.
    // universal character name 被改写为 UTF-8; `raw` 中保留原来的转义写法.
    if (text[i] == '\\' && (text[i + 1] == 'u' || text[i + 1] == 'U')) {
      int digs   = text[i + 1] == 'u' ? 4 : 8;
      uint32_t c = read_universal_char(text + i + 2, digs);
      if (c) {
        int k = libcp_encode_utf8(buf + j, c);
        for (int e = 0; e < k; e++)
          map[j + e] = i;
        j += k;
        i += digs + 2;
        continue;
      }
    }

    // Any other backslash is copied together with the byte after it, so
    // that the "u" of an escaped "\\u" is not taken for an escape.
    //
    // 其它反斜杠与它后面的一个字节一起原样复制, 以免把 "\\u" 中转义过的
    // "u" 当成转义序列的开头.
    if (text[i] == '\\' && i + 1 < len) {
      map[j]   = i;
      buf[j++] = text[i++];
    }

    map[j]   = i;
    buf[j++] = text[i++];
  }

  // The lexer needs the buffer to end in a newline: a line comment runs
  // to the next one. The old passes kept the synthesized newline when a
  // trailing continuation swallowed it, by materializing the pending
  // breaks at the end.
  //
  // 词法分析器要求缓冲区以换行结尾: 行注释要一直跑到下一个换行. 过去的
  // pass 在末尾续行吞掉合成换行时会保留它, 办法就是在最后补出尚未计入的
  // 换行.
  if (j == 0 || buf[j - 1] != '\n') {
    map[j]   = len;
    buf[j++] = '\n';
  }

  // Credit the continuation breaks that never saw a real break after
  // them, and give the file end (where the EOF token sits) the final
  // number.
  //
  // 计入那些后面从未遇到真实换行的续行, 并给文件末尾 (EOF token 所在处)
  // 赋上最终的行号.
  line += pending;
  for (; mark <= len; mark++)
    lines[mark] = line;
  lines[len] = line;

  map[j]     = len;
  *map_out   = map;
  *lines_out = lines;
  return buf;
}

// 读取路径对应的文件, 建扫描缓冲区并做 tokenize; 文件打不开时返回 NULL.
LibcpToken* libcp_tokenize_file_impl(LibcpContext* ctx, char* path) {
  size_t raw_len;
  char* raw = read_file(ctx, path, &raw_len);
  if (!raw)
    return NULL;

  int *map, *lines;
  char* contents = make_scan_buffer(ctx, raw, (int)raw_len, &map, &lines);

  // Save the filename for assembler .file directive.
  // 为汇编器的 .file 指令保存文件名.
  int file_no     = ctx->next_file_no++;
  LibcpFile* file = libcp_new_file(ctx, path, file_no + 1, contents);

  // The scan buffer replaces the identity tables new_file built.
  // 扫描缓冲区替换掉 new_file 建出的恒等映射表.
  file->raw   = raw;
  file->map   = map;
  file->lines = lines;

  // A NUL byte would silently end the token stream. The scan buffer now
  // contains it (make_scan_buffer no longer stops at strlen), so report
  // the first one before tokenizing.
  //
  // NUL 字节会让 token 流静默结束. 扫描缓冲区现在会包含它 (make_scan_buffer
  // 不再在 strlen 处停下), 因此在 tokenize 之前先报告第一个 NUL.
  if (memchr(raw, 0, raw_len)) {
    ctx->current_file = file;
    libcp_error_at(ctx, memchr(contents, 0, raw_len + 2), "NUL byte in source file");
  }

  // Save the filename for assembler .file directive.
  // 为汇编器的 .file 指令保存文件名.
  if (ctx->input_files_cap < file_no + 2) {
    size_t old       = sizeof(LibcpFile*) * ctx->input_files_cap;
    int cap          = file_no + 2;
    ctx->input_files = libcp_arena_realloc(ctx, ctx->input_files, old, sizeof(LibcpFile*) * cap);
    ctx->input_files_cap = cap;
  }
  ctx->input_files[file_no]     = file;
  ctx->input_files[file_no + 1] = NULL;
  ctx->input_files_len          = file_no + 1;
  file_no++;

  return libcp_tokenize(ctx, file);
}
