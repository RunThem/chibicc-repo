// 第 1 层词法分析: 把扫描缓冲区切分成 token 流.
// 原始文本 (raw) 与扫描文本 (contents) 双缓冲并行, 由 map / lines 两张
// 表建立扫描偏移到 raw 偏移, raw 偏移到行号的映射; 每个 token 携带 trivia
// 与 raw 坐标, 因此该流可以逐字节重建输入文件. 本文件同时提供诊断入口与
// token 比较等共用设施.
#include "libcp.h"

// 输入文件, 输入文件列表与当前位置标志都存放在 LibcpContext 中
// (PLAN L2.2). 与其它阶段一样, 词法分析器通过当前活动的 context 读取它们.
// 坐标表的构造与换算由 source.c 独占, 本文件只做扫描与 token 构造.

// 把一个十六进制数字字符转成数值; 调用方保证 c 是十六进制数字.
static int from_hex(char c) {
  if ('0' <= c && c <= '9')
    return c - '0';
  if ('a' <= c && c <= 'f')
    return c - 'a' + 10;
  return c - 'A' + 10;
}

// 把诊断消息格式化进 per-TU 分配.
static char* vformat(LibcpContext* ctx, const char* fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  char* buf = libcp_tu_alloc(ctx, n + 1);
  vsnprintf(buf, n + 1, fmt, ap);
  return buf;
}

// 回退到最内层的公共入口栈帧; 没有这样的栈帧时 (驱动器在 cc1 之前的
// 路径), 与以前一样直接退出.
static noreturn void raise_error(LibcpContext* ctx) {
  if (ctx->jmp_top) {
    ctx->failed = true;
    longjmp(ctx->jmp_top->buf, 1);
  }
  exit(1);
}

// 按以下格式报告错误消息.
//
// foo.c:10: x = y + 1;
//               ^ <error message here>
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
                      const char* fmt,
                      va_list ap) {

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

  // 打印出这一行源码.
  int indent = fprintf(stderr, "%s:%d: ", filename, line_no);
  fprintf(stderr, "%.*s\n", (int)(end - line), line);

  // 显示错误消息.
  int pos = libcp_display_width(ctx, line, loc - line) + indent;

  fprintf(stderr, "%*s", pos, "");  // 打印 pos 个空格.
  fprintf(stderr, "^ ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
}

// 报告的核心 (公共 libcp_diagnostic_report 与内部 error / warn 入口共用):
// 有锚点 token 时按位置报告, 没有时只报告消息. 只交付诊断, 不 longjmp,
// 也不改 context 状态.
void libcp_diagnostic_vreport(LibcpContext* ctx,
                              LibcpToken* tok,
                              LibcpDiagnosticSeverity severity,
                              const char* fmt,
                              va_list ap) {
  if (tok) {
    verror_at(ctx,
              tok->file->name,
              tok->file->contents,
              tok->line_no,
              tok->loc,
              tok,
              severity,
              fmt,
              ap);
    return;
  }

  if (ctx && ctx->diag_fn) {
    char* msg         = vformat(ctx, fmt, ap);
    LibcpDiagnostic d = {NULL, 0, 0, severity, msg, NULL, NULL};
    ctx->diag_fn(ctx->diag_user, &d);
    return;
  }
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
}

// 公共的返回式诊断 (issue #18): 与 error / warn 入口走同一条报告路径,
// 但不中断调用者; 错误把 context 标记为失败, 警告不标记.
void libcp_diagnostic_report(LibcpContext* ctx,
                             LibcpToken* tok,
                             LibcpDiagnosticSeverity severity,
                             const char* fmt,
                             ...) {
  va_list ap;
  va_start(ap, fmt);
  libcp_diagnostic_vreport(ctx, tok, severity, fmt, ap);
  va_end(ap);

  if (severity == LIBCP_DIAG_ERROR && ctx)
    ctx->failed = true;
}

// 报告错误; 回退到当前公共入口, 若驱动器不在任何公共入口内则直接退出.
void libcp_error(LibcpContext* ctx, char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  libcp_diagnostic_vreport(ctx, NULL, LIBCP_DIAG_ERROR, fmt, ap);
  va_end(ap);
  raise_error(ctx);
}

void libcp_error_at(LibcpContext* ctx, char* loc, char* fmt, ...) {
  int line_no =
      libcp_file_physical_line(ctx->current_file, libcp_file_raw_offset(ctx->current_file, loc));

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
  libcp_diagnostic_vreport(ctx, tok, LIBCP_DIAG_ERROR, fmt, ap);
  va_end(ap);
  raise_error(ctx);
}

void libcp_warn_tok(LibcpContext* ctx, LibcpToken* tok, char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  libcp_diagnostic_vreport(ctx, tok, LIBCP_DIAG_WARNING, fmt, ap);
  va_end(ap);
}

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

// 若 token 的拼写等于 `op` 则返回 true.
bool libcp_equal(LibcpToken* tok, char* op) {
  // 先比较长度: 比 `op` 更长的 token 不得让 memcmp 读过 `op` 的末尾
  // (L3.4c 中由 ASan 发现).
  return (int)strlen(op) == tok->len && memcmp(tok->loc, op, tok->len) == 0;
}

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

// 新建一个 token. `raw_trivia` 是本 token 前面被跳过的字节起始处的
// raw 偏移 (见 LibcpToken.trivia).
static LibcpToken*
    new_token(LibcpContext* ctx, LibcpTokenKind kind, char* start, char* end, int raw_trivia) {
  LibcpToken* tok = libcp_tu_calloc(ctx, 1, sizeof(LibcpToken));
  tok->kind       = kind;
  tok->loc        = start;
  tok->len        = end - start;

  int raw_start   = libcp_file_raw_offset(ctx->current_file, start);
  tok->trivia     = ctx->current_file->raw + raw_trivia;
  tok->trivia_len = raw_start - raw_trivia;
  tok->raw_loc    = ctx->current_file->raw + raw_start;
  tok->raw_len    = 0;
  if (start != end)
    tok->raw_len =
        libcp_file_raw_end_of(ctx->current_file, end - ctx->current_file->contents - 1) - raw_start;

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

  // 这里的转义序列用它们自身来定义. 例如 '\n' 就是用 '\n' 实现的.
  // 这个同义反复的定义能成立, 是因为编译我们编译器的那个编译器知道
  // '\n' 到底是什么. 换句话说, 我们从编译我们编译器的编译器那里
  // "继承" 了 '\n' 的 ASCII 码, 因此不必在这里重新教实际代码.
  //
  // 这一事实不仅影响编译器的正确性, 也深刻影响生成代码的安全性.
  // 更多信息见 Ken Thompson 的 "Reflections on Trusting Trust" 一文:
  // https://github.com/rui314/chibicc/wiki/thompson1984.pdf
  switch (*p) {
    case 'a': return '\a';
    case 'b': return '\b';
    case 't': return '\t';
    case 'n': return '\n';
    case 'v': return '\v';
    case 'f': return '\f';
    case 'r': return '\r';
    // [GNU] \e 表示 ASCII 转义字符, 这是 GNU C 的扩展.
    case 'e': return 27;
    default: return *p;
  }
}

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
  char* buf = libcp_tu_calloc(ctx, 1, end - quote);
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

// 把 quote 处的字符串字面量转码为 UTF-16 载荷, 返回数组类型; 载荷与
// 收尾引号后的位置分别通过 *str_out 与 *end_out 返回.
//
// UTF-16 是 Unicode 的另一种变长编码. 小于 U+10000 的码点用 2 字节
// 编码. 大于或等于该值的码点用 4 字节编码. 4 字节序列中的每个 2 字节
// 称为 "surrogate", 整个 4 字节序列称为 "surrogate pair".
static LibcpType*
    scan_utf16_string(LibcpContext* ctx, char* quote, char** end_out, char** str_out) {
  char* end     = string_literal_end(ctx, quote + 1);
  uint16_t* buf = libcp_tu_calloc(ctx, 2, end - quote);
  int len       = 0;

  for (char* p = quote + 1; p < end;) {
    if (*p == '\\') {
      buf[len++] = read_escaped_char(ctx, &p, p + 1);
      continue;
    }

    uint32_t c = libcp_decode_utf8(ctx, &p, p);
    if (c < 0x10000) {
      // 把码点编码为 2 字节.
      buf[len++] = c;
    } else {
      // 把码点编码为 4 字节.
      c -= 0x10000;
      buf[len++] = 0xd800 + ((c >> 10) & 0x3ff);
      buf[len++] = 0xdc00 + (c & 0x3ff);
    }
  }

  *end_out = end;
  *str_out = (char*)buf;
  return libcp_array_of(ctx, libcp_ty_ushort, len + 1);
}

// 读一个 UTF-8 编码的字符串字面量, 并把它转码为 UTF-16.
static LibcpToken*
    read_utf16_string_literal(LibcpContext* ctx, char* start, char* quote, int raw_trivia) {
  char* end;
  char* str;
  LibcpType* ty   = scan_utf16_string(ctx, quote, &end, &str);
  LibcpToken* tok = new_token(ctx, TK_STR, start, end + 1, raw_trivia);
  tok->ty         = ty;
  tok->str        = str;
  return tok;
}

// 把 quote 处的字符串字面量转码为 UTF-32 载荷, 返回数组类型; 载荷与
// 收尾引号后的位置分别通过 *str_out 与 *end_out 返回.
//
// UTF-32 是 Unicode 的定长编码. 每个码点占 4 字节.
static LibcpType* scan_utf32_string(LibcpContext* ctx,
                                    char* quote,
                                    LibcpType* ty,
                                    char** end_out,
                                    char** str_out) {
  char* end     = string_literal_end(ctx, quote + 1);
  uint32_t* buf = libcp_tu_calloc(ctx, 4, end - quote);
  int len       = 0;

  for (char* p = quote + 1; p < end;) {
    if (*p == '\\')
      buf[len++] = read_escaped_char(ctx, &p, p + 1);
    else
      buf[len++] = libcp_decode_utf8(ctx, &p, p);
  }

  *end_out = end;
  *str_out = (char*)buf;
  return libcp_array_of(ctx, ty, len + 1);
}

// 读一个 UTF-8 编码的字符串字面量, 并把它转码为 UTF-32.
static LibcpToken* read_utf32_string_literal(LibcpContext* ctx,
                                             char* start,
                                             char* quote,
                                             LibcpType* ty,
                                             int raw_trivia) {
  char* end;
  char* str;
  LibcpType* arr  = scan_utf32_string(ctx, quote, ty, &end, &str);
  LibcpToken* tok = new_token(ctx, TK_STR, start, end + 1, raw_trivia);
  tok->ty         = arr;
  tok->str        = str;
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

  // strtoul 在溢出时会饱和到 ULONG_MAX 并设置 ERANGE; 不能让无法表示的
  // 整数字面量悄悄变成 ULONG_MAX. 先记住溢出, 等确认整个 token 是整数常量
  // 后再报错: 若后面还有 '.' 或指数, 它其实是整数部分超出范围的浮点常量,
  // 应交给下面的浮点分支.
  errno         = 0;
  int64_t val   = strtoul(p, &p, base);
  bool overflow = errno == ERANGE;

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

  if (overflow)
    libcp_error_tok(ctx, tok, "integer literal is too large");

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

// 预处理阶段对数值字面量的定义比后续阶段宽松. 为了处理这一点,
// 数值字面量先被 tokenize 成 "pp-number" token, 预处理之后再转换成
// 普通的数值 token.
//
// 本函数把一个 pp-number token 转换成普通数值 token.
static void convert_pp_number(LibcpContext* ctx, LibcpToken* tok) {
  // 先尝试按整数常量解析.
  if (convert_pp_int(ctx, tok))
    return;

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

// 为所有 token 填上行号信息.
static void add_line_numbers(LibcpToken* tok) {
  for (; tok; tok = tok->next)
    tok->line_no = libcp_token_physical_line(tok);
}

// 把一个已有字符串字面量就地重新扫描为目标基类型: 只写回 kind, 类型与
// 字符串载荷. file, loc, len, raw 坐标, 行号, trivia 与 provenance 都属于
// 源码中的那个 token, 全部保留.
void libcp_tokenize_string_literal(LibcpContext* ctx, LibcpToken* tok, LibcpType* basety) {
  // `loc` 指向拼写的起点; quote 要跳过 u8 / u / U / L 前缀.
  char* quote = tok->loc;
  if (quote[0] == 'u' && quote[1] == '8')
    quote += 2;
  else if (quote[0] != '"')
    quote += 1;

  char* end;
  char* str;
  if (basety->size == 2)
    tok->ty = scan_utf16_string(ctx, quote, &end, &str);
  else
    tok->ty = scan_utf32_string(ctx, quote, basety, &end, &str);

  tok->kind = TK_STR;
  tok->str  = str;
}

// 对给定的文本做词法分析, 返回新的 token 流.
LibcpToken* libcp_tokenize(LibcpContext* ctx, LibcpFile* file) {
  ctx->current_file = file;

  char* p = file->contents;

  // 下一个 token 前面被跳过的字节起始处的 raw 偏移. 下面四个跳过循环
  // 只推进 `p`; 它们越过的字节会记录为下一个 token 的 trivia 片段.
  int raw_trivia = 0;

  LibcpToken head = {};
  LibcpToken* cur = &head;

  ctx->at_bol    = true;
  ctx->has_space = false;

  while (*p) {
    // 跳过行注释.
    if (startswith(p, "//")) {
      p += 2;
      while (*p != '\n')
        p++;
      ctx->has_space = true;
      continue;
    }

    // 跳过块注释.
    if (startswith(p, "/*")) {
      char* q = strstr(p + 2, "*/");
      if (!q)
        libcp_error_at(ctx, p, "unclosed block comment");
      p              = q + 2;
      ctx->has_space = true;
      continue;
    }

    // 跳过换行.
    if (*p == '\n') {
      p++;
      ctx->at_bol    = true;
      ctx->has_space = false;
      continue;
    }

    // 跳过空白字符.
    if (isspace((unsigned char)*p)) {
      p++;
      ctx->has_space = true;
      continue;
    }

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
      raw_trivia      = libcp_token_raw_end(cur);
      continue;
    }

    // 字符串字面量
    if (*p == '"') {
      cur = cur->next = read_string_literal(ctx, p, p, raw_trivia);
      p += cur->len;
      raw_trivia = libcp_token_raw_end(cur);
      continue;
    }

    // UTF-8 字符串字面量
    if (startswith(p, "u8\"")) {
      cur = cur->next = read_string_literal(ctx, p, p + 2, raw_trivia);
      p += cur->len;
      raw_trivia = libcp_token_raw_end(cur);
      continue;
    }

    // UTF-16 字符串字面量
    if (startswith(p, "u\"")) {
      cur = cur->next = read_utf16_string_literal(ctx, p, p + 1, raw_trivia);
      p += cur->len;
      raw_trivia = libcp_token_raw_end(cur);
      continue;
    }

    // 宽字符串字面量
    if (startswith(p, "L\"")) {
      cur = cur->next = read_utf32_string_literal(ctx, p, p + 1, libcp_ty_int, raw_trivia);
      p += cur->len;
      raw_trivia = libcp_token_raw_end(cur);
      continue;
    }

    // UTF-32 字符串字面量
    if (startswith(p, "U\"")) {
      cur = cur->next = read_utf32_string_literal(ctx, p, p + 1, libcp_ty_uint, raw_trivia);
      p += cur->len;
      raw_trivia = libcp_token_raw_end(cur);
      continue;
    }

    // 字符字面量
    if (*p == '\'') {
      cur = cur->next = read_char_literal(ctx, p, p, libcp_ty_int, raw_trivia);
      cur->val        = (char)cur->val;
      p += cur->len;
      raw_trivia = libcp_token_raw_end(cur);
      continue;
    }

    // UTF-16 字符字面量
    if (startswith(p, "u'")) {
      cur = cur->next = read_char_literal(ctx, p, p + 1, libcp_ty_ushort, raw_trivia);
      cur->val &= 0xffff;
      p += cur->len;
      raw_trivia = libcp_token_raw_end(cur);
      continue;
    }

    // 宽字符字面量
    if (startswith(p, "L'")) {
      cur = cur->next = read_char_literal(ctx, p, p + 1, libcp_ty_int, raw_trivia);
      p += cur->len;
      raw_trivia = libcp_token_raw_end(cur);
      continue;
    }

    // UTF-32 字符字面量
    if (startswith(p, "U'")) {
      cur = cur->next = read_char_literal(ctx, p, p + 1, libcp_ty_uint, raw_trivia);
      p += cur->len;
      raw_trivia = libcp_token_raw_end(cur);
      continue;
    }

    // 标识符或关键字
    int ident_len = read_ident(ctx, p);
    if (ident_len) {
      cur = cur->next = new_token(ctx, TK_IDENT, p, p + ident_len, raw_trivia);
      p += cur->len;
      raw_trivia = libcp_token_raw_end(cur);
      continue;
    }

    // 标点
    int punct_len = read_punct(p);
    if (punct_len) {
      cur = cur->next = new_token(ctx, TK_PUNCT, p, p + punct_len, raw_trivia);
      p += cur->len;
      raw_trivia = libcp_token_raw_end(cur);
      continue;
    }

    libcp_error_at(ctx, p, "invalid token");
  }

  // 文件末尾的 trivia 归属于 EOF token.
  cur = cur->next = new_token(ctx, TK_EOF, p, p, raw_trivia);
  add_line_numbers(head.next);
  return head.next;
}

// 返回给定文件的内容, 并通过 *len 返回它的长度. 文本按磁盘上的原样返回:
// 过去在这里做的改写 (合成末尾换行, 跳过 BOM) 已移入 make_scan_buffer,
// 由它用 raw 坐标记住这些改写.
static char* read_file(LibcpContext* ctx, char* path, size_t* len) {
  FILE* fp;

  if (strcmp(path, "-") == 0) {
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

  // 词法分析器用 int 保存偏移; 更大的文件会被静默截断, 因此直接拒绝 (C12).
  if (buflen > INT32_MAX) {
    fclose(out);
    free(buf);
    libcp_error(ctx, "file too large: %s", path);
  }

  // 下面的 fputc 会让 memstream 的长度把终止符也算进去; 这里先保留纯文本
  // 长度, 供调用方与 stdin 缓存使用.
  size_t data_len = buflen;
  *len            = data_len;
  fputc('\0', out);
  fclose(out);

  // memstream 的缓冲区属于 C 库的 malloc, 而不是 LIBCP_MALLOC, 因此用
  // free() 释放; 文本随后搬进 per-TU 分配, 这样 context reset 时会连同其它
  // per-TU 内存一起释放.
  if (ctx) {
    char* copy = libcp_tu_alloc(ctx, data_len + 1);
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

// 读取路径对应的文件, 建扫描缓冲区并做 tokenize; 文件打不开时返回 NULL.
LibcpToken* libcp_tokenize_file_impl(LibcpContext* ctx, char* path) {
  size_t raw_len;
  char* raw = read_file(ctx, path, &raw_len);
  if (!raw)
    return NULL;

  // LibcpFile 与坐标表的构造由 source.c 独占.
  LibcpFile* file = libcp_new_file_from_raw(ctx, path, raw, (int)raw_len);

  // NUL 字节会让 token 流静默结束. 扫描缓冲区现在会包含它 (make_scan_buffer
  // 不再在 strlen 处停下), 因此在 tokenize 之前先报告第一个 NUL.
  if (memchr(raw, 0, raw_len)) {
    ctx->current_file = file;
    libcp_error_at(ctx, memchr(file->contents, 0, raw_len + 2), "NUL byte in source file");
  }

  // 为汇编器的 .file 指令保存文件名, 并登记进 input file 表.
  libcp_source_register_input_file(ctx, file);
  return libcp_tokenize(ctx, file);
}
