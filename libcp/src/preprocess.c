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
// "hideset". LibcpHideset is initially empty, and every time we expand a
// macro, the macro name is added to the resulting tokens' hidesets.
//
// The above macro expansion algorithm is explained in this document
// written by Dave Prossor, which is used as a basis for the
// standard's wording:
// https://github.com/rui314/chibicc/wiki/cpp.algo.pdf
//
// 本文件实现 C 预处理器 (第 1 层: 预处理): 输入一个 token 列表, 输出
// 一个新的 token 列表. 内容包括宏定义与宏展开 (hideset 与 provenance
// 记录), #include 与 include guard, 条件编译, 行标记, 内建宏与预定义宏.
//
// 预处理语言的设计保证即使存在递归宏也一定会终止. 通俗地说, 每个 token
// 上的宏只应用一次: 若宏 token T 出现在 T 直接或间接展开的结果里, T 就
// 不再继续展开. 例如 T 定义为 U, U 又定义为 T, 则 T 展开为 U 再展开为
// T, 宏展开到此为止.
//
// 为实现上述行为, 每个 token 携带一个宏名集合, 记录它经由哪些宏展开而
// 来, 这个集合称为 "hideset". LibcpHideset 初始为空, 每展开一个宏就把
// 该宏名加入结果 token 的 hideset.
//
// 上述宏展开算法在 Dave Prossor 编写的文档中有解释 (链接见上), 标准的
// 措辞即以它为基础.

#include "libcp.h"

typedef struct libcp_macro_param LibcpMacroParam;
struct libcp_macro_param {
  LibcpMacroParam* next;
  char* name;
};

typedef struct libcp_macro_arg LibcpMacroArg;
struct libcp_macro_arg {
  LibcpMacroArg* next;
  char* name;
  bool is_va_args;
  int index;  // 0-based position in the actual argument list
              // 在实际实参列表中的位置, 下标从 0 开始
  LibcpToken* tok;
};

typedef LibcpToken* LibcpMacroHandlerFn(LibcpContext* ctx, LibcpToken*);

typedef struct libcp_macro LibcpMacro;
struct libcp_macro {
  char* name;
  bool is_objlike;  // Object-like or function-like
                    // 对象式宏还是函数式宏, 前者不带形参
  LibcpMacroParam* params;
  char* va_args_name;
  LibcpToken* body;
  LibcpToken* def;  // the name token of the #define; NULL for -D and builtins
                    // #define 处的名字 token; 由 -D 和内建宏创建的宏为 NULL
  LibcpMacroHandlerFn* handler;
};

// Provenance records (plans/PLAN.md): one LibcpMacroExpansion per expansion, built
// once and shared read-only by every token that expansion produced; one
// LibcpMacroProv per token and layer, prepended so a chain reads innermost
// first. Both are arena objects that live as long as the tokens. The
// types are private; libcp.h exposes accessors over them.
//
// provenance 记录 (plans/PLAN.md): 每次展开生成一个 LibcpMacroExpansion,
// 只建一次, 由该次展开产出的所有 token 只读共享; 每个 token 的每一层
// provenance 是一个 LibcpMacroProv, 前插构造, 因此链从头读起是最内层优先.
// 两者都是 arena 对象, 生命周期与 token 相同. 类型是私有的; libcp.h 只
// 对外暴露访问器.
struct libcp_macro_expansion {
  LibcpToken* name;     // macro name token at the call site
                        // 调用点处宏名字的 token
  LibcpToken* rparen;   // ')' of a function-like call; NULL otherwise
                        // 函数式调用结尾的 ')'; 其他情况为 NULL
  LibcpMacro* macro;    // definition in force at expansion time
                        // 展开时生效的那条宏定义
  LibcpMacroArg* args;  // actual arguments; NULL for object-like and builtins
                        // 实际实参; 对象式宏与内建宏为 NULL
  int arg_count;
  bool is_builtin;  // produced by a handler (__LINE__ and friends)
                    // 由处理器产生 (__LINE__ 一类动态宏)
};

struct libcp_macro_prov {
  LibcpMacroExpansion* exp;
  int arg_index;         // -1: macro body; >= 0: index of the argument
                         // -1 表示宏体; >= 0 表示实参下标
  LibcpMacroProv* next;  // the context the invocation itself appeared in
                         // 本次调用自身所处的上下文链
};

// `#if` can be nested, so we use a stack to manage nested `#if`s.
//
// `#if` 可以嵌套, 因此用一个栈管理嵌套的条件编译块.
struct libcp_cond_incl {
  LibcpCondIncl* next;
  enum {
    IN_THEN,
    IN_ELIF,
    IN_ELSE
  } ctx;
  LibcpToken* tok;
  bool included;
};

struct libcp_hideset {
  LibcpHideset* next;
  char* name;
};

// The macro table, conditional stack and pragma-once set live in
// LibcpContext (PLAN L2.3). The preprocessor reads them through the
// active context like every other phase.
//
// 宏表, 条件编译栈和 pragma once 集合都放在 LibcpContext 里 (PLAN L2.3).
// 预处理器与其他阶段一样, 通过当前 context 读取它们.
//
// 下面三个宏只是给 context 字段起短名, 使用前提是当前函数有 ctx 形参.
#define macros      (ctx->macros)
#define cond_incl   (ctx->cond_incl)
#define pragma_once (ctx->pragma_once)

static LibcpToken* preprocess2(LibcpContext* ctx, LibcpToken* tok);
static LibcpMacro* find_macro(LibcpContext* ctx, LibcpToken* tok);

static bool is_hash(LibcpToken* tok) {
  return tok->at_bol && libcp_equal(tok, "#");
}

// Some preprocessor directives such as #include allow extraneous
// tokens before newline. This function skips such tokens.
//
// 某些指令 (如 #include) 允许行尾有多余 token; 本函数跳过它们, 并给出
// "extra token" 警告.
static LibcpToken* skip_line(LibcpContext* ctx, LibcpToken* tok) {
  if (tok->at_bol)
    return tok;
  libcp_warn_tok(ctx, tok, "extra token");
  while (tok->at_bol)
    tok = tok->next;
  return tok;
}

// 复制 token 结构体本身, 并断开 next, 得到可单独挂到新链上的副本.
static LibcpToken* copy_token(LibcpContext* ctx, LibcpToken* tok) {
  LibcpToken* t = libcp_arena_calloc(ctx, 1, sizeof(LibcpToken));
  *t            = *tok;
  t->next       = NULL;
  return t;
}

static LibcpToken* new_eof(LibcpContext* ctx, LibcpToken* tok) {
  LibcpToken* t = copy_token(ctx, tok);
  t->kind       = TK_EOF;
  t->len        = 0;
  t->prov       = NULL;
  return t;
}

// Prepend one provenance layer. The tail is the chain the invocation's
// own text already carried, so chains are built by prepending only and
// shared tails are never mutated (plans/PLAN.md, "挂接规则").
//
// 前插一层 provenance. 尾部是本次调用自身文本已经携带的链, 所以链只靠
// 前插构造, 共享的尾部从不会被改写 (plans/PLAN.md, "挂接规则").
static LibcpMacroProv*
    new_prov(LibcpContext* ctx, LibcpMacroExpansion* exp, int arg_index, LibcpMacroProv* next) {
  LibcpMacroProv* p = libcp_arena_calloc(ctx, 1, sizeof(LibcpMacroProv));
  p->exp            = exp;
  p->arg_index      = arg_index;
  p->next           = next;
  return p;
}

// Build the record of one expansion. It is immutable once returned; every
// token the expansion produces shares it.
//
// 构建一次展开的记录. 返回后即为不可变, 该次展开产出的每个 token 都
// 共享它.
static LibcpMacroExpansion* new_expansion(LibcpContext* ctx,
                                          LibcpToken* name,
                                          LibcpToken* rparen,
                                          LibcpMacro* macro,
                                          LibcpMacroArg* args,
                                          int arg_count,
                                          bool is_builtin) {
  LibcpMacroExpansion* e = libcp_arena_calloc(ctx, 1, sizeof(LibcpMacroExpansion));
  e->name                = name;
  e->rparen              = rparen;
  e->macro               = macro;
  e->args                = args;
  e->arg_count           = arg_count;
  e->is_builtin          = is_builtin;
  return e;
}

static int count_macro_args(LibcpMacroArg* args) {
  int n = 0;
  for (; args; args = args->next)
    n++;
  return n;
}

static LibcpHideset* new_hideset(LibcpContext* ctx, char* name) {
  LibcpHideset* hs = libcp_arena_calloc(ctx, 1, sizeof(LibcpHideset));
  hs->name         = name;
  return hs;
}

// 把 hs1 的节点复制后接到 hs2 之前; hs2 被共享, 不做复制.
static LibcpHideset* hideset_union(LibcpContext* ctx, LibcpHideset* hs1, LibcpHideset* hs2) {
  LibcpHideset head = {};
  LibcpHideset* cur = &head;

  for (; hs1; hs1 = hs1->next)
    cur = cur->next = new_hideset(ctx, hs1->name);
  cur->next = hs2;
  return head.next;
}

static bool hideset_contains(LibcpHideset* hs, char* s, int len) {
  for (; hs; hs = hs->next)
    if (strlen(hs->name) == len && !strncmp(hs->name, s, len))
      return true;
  return false;
}

// 只保留同时出现在两个集合里的宏名; 函数式宏调用用它合并宏名 token 与
// 右括号 token 的 hideset.
static LibcpHideset* hideset_intersection(LibcpContext* ctx, LibcpHideset* hs1, LibcpHideset* hs2) {
  LibcpHideset head = {};
  LibcpHideset* cur = &head;

  for (; hs1; hs1 = hs1->next)
    if (hideset_contains(hs2, hs1->name, strlen(hs1->name)))
      cur = cur->next = new_hideset(ctx, hs1->name);
  return head.next;
}

// 复制整条 token 链, 把 hs 并入每个 token 已有的 hideset.
static LibcpToken* add_hideset(LibcpContext* ctx, LibcpToken* tok, LibcpHideset* hs) {
  LibcpToken head = {};
  LibcpToken* cur = &head;

  for (; tok; tok = tok->next) {
    LibcpToken* t = copy_token(ctx, tok);
    t->hideset    = hideset_union(ctx, t->hideset, hs);
    cur = cur->next = t;
  }
  return head.next;
}

// Append tok2 to the end of tok1.
//
// 把 tok2 接到 tok1 末尾并返回新链的头; tok1 逐个复制, 原链不被改写.
static LibcpToken* append(LibcpContext* ctx, LibcpToken* tok1, LibcpToken* tok2) {
  if (tok1->kind == TK_EOF)
    return tok2;

  LibcpToken head = {};
  LibcpToken* cur = &head;

  for (; tok1->kind != TK_EOF; tok1 = tok1->next)
    cur = cur->next = copy_token(ctx, tok1);
  cur->next = tok2;
  return head.next;
}

// 递归跳过嵌套的 #if / #ifdef / #ifndef 块, 返回匹配的 #endif 之后.
static LibcpToken* skip_cond_incl2(LibcpToken* tok) {
  while (tok->kind != TK_EOF) {
    if (is_hash(tok) && (libcp_equal(tok->next, "if") || libcp_equal(tok->next, "ifdef") ||
                         libcp_equal(tok->next, "ifndef"))) {
      tok = skip_cond_incl2(tok->next->next);
      continue;
    }
    if (is_hash(tok) && libcp_equal(tok->next, "endif"))
      return tok->next->next;
    tok = tok->next;
  }
  return tok;
}

// Skip until next `#else`, `#elif` or `#endif`.
// Nested `#if` and `#endif` are skipped.
//
// 跳到下一个 `#else`, `#elif` 或 `#endif`; 中间的嵌套 `#if` 块整体跳过.
static LibcpToken* skip_cond_incl(LibcpToken* tok) {
  while (tok->kind != TK_EOF) {
    if (is_hash(tok) && (libcp_equal(tok->next, "if") || libcp_equal(tok->next, "ifdef") ||
                         libcp_equal(tok->next, "ifndef"))) {
      tok = skip_cond_incl2(tok->next->next);
      continue;
    }

    if (is_hash(tok) && (libcp_equal(tok->next, "elif") || libcp_equal(tok->next, "else") ||
                         libcp_equal(tok->next, "endif")))
      break;
    tok = tok->next;
  }
  return tok;
}

// Double-quote a given string and returns it.
//
// 给字符串加双引号 (转义其中的反斜杠与双引号), 返回 arena 中的新串.
static char* quote_string(LibcpContext* ctx, char* str) {
  int bufsize = 3;
  for (int i = 0; str[i]; i++) {
    if (str[i] == '\\' || str[i] == '"')
      bufsize++;
    bufsize++;
  }

  char* buf = libcp_arena_calloc(ctx, 1, bufsize);
  char* p   = buf;
  *p++      = '"';
  for (int i = 0; str[i]; i++) {
    if (str[i] == '\\' || str[i] == '"')
      *p++ = '\\';
    *p++ = str[i];
  }
  *p++ = '"';
  *p++ = '\0';
  return buf;
}

// libcp_tokenize() leaves current_file pointing at the nested file. Restore
// the caller's file so libcp_error_at's implicit current_file still matches
// the loc it is given.
//
// libcp_tokenize() 结束后 current_file 会指向被切词的文件; 这里恢复调用者
// 的文件, 使 libcp_error_at 的隐式 current_file 仍与传入的 loc 匹配.
static LibcpToken* tokenize_nested(LibcpContext* ctx, LibcpFile* file) {
  LibcpFile* save   = ctx->current_file;
  LibcpToken* tok   = libcp_tokenize(ctx, file);
  ctx->current_file = save;
  return tok;
}

// 同上, 但按路径打开文件并切词; #include 打开的嵌套文件走这里.
static LibcpToken* tokenize_file_nested(LibcpContext* ctx, char* path) {
  LibcpFile* save   = ctx->current_file;
  LibcpToken* tok   = libcp_tokenize_file_impl(ctx, path);
  ctx->current_file = save;
  return tok;
}

// 把字符串内容做成一个字符串字面量 token, 位置信息取自模板 token.
static LibcpToken* new_str_token(LibcpContext* ctx, char* str, LibcpToken* tmpl) {
  char* buf = quote_string(ctx, str);
  return tokenize_nested(ctx, libcp_new_file(ctx, tmpl->file->name, tmpl->file->file_no, buf));
}

// Copy all tokens until the next newline, terminate them with
// an EOF token and then returns them. This function is used to
// create a new list of tokens for `#if` arguments.
//
// 复制到下一个换行为止的所有 token, 末尾补一个 EOF 后返回; 用于给 `#if`
// 的参数构造一份独立的 token 列表.
static LibcpToken* copy_line(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  LibcpToken head = {};
  LibcpToken* cur = &head;

  for (; !tok->at_bol; tok = tok->next)
    cur = cur->next = copy_token(ctx, tok);

  cur->next = new_eof(ctx, tok);
  *rest     = tok;
  return head.next;
}

// 把整数格式化成文本再切词, 得到一个带位置信息的数值 token.
static LibcpToken* new_num_token(LibcpContext* ctx, int val, LibcpToken* tmpl) {
  char* buf = libcp_format(ctx, "%d\n", val);
  return tokenize_nested(ctx, libcp_new_file(ctx, tmpl->file->name, tmpl->file->file_no, buf));
}

#ifdef LIBCP_TEST_BUILTINS
// A B8 regression hook: a builtin that produces two tokens. Only
// test/library.sh's ASan round builds it.
//
// B8 回归钩子: 一个产出两个 token 的内建宏; 只有 test/library.sh 的
// ASan 轮构建它 (需定义 LIBCP_TEST_BUILTINS).
static LibcpToken* multi_macro(LibcpContext* ctx, LibcpToken* tmpl) {
  LibcpToken* t1 = new_num_token(ctx, 1, tmpl);
  LibcpToken* t2 = new_num_token(ctx, 2, tmpl);
  t1->next       = t2;
  return t1;
}
#endif

// 读取 #if 的整行 token, 并把 defined 运算符就地替换成 1 / 0.
static LibcpToken* read_const_expr(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  tok = copy_line(ctx, rest, tok);

  LibcpToken head = {};
  LibcpToken* cur = &head;

  while (tok->kind != TK_EOF) {
    // "defined(foo)" or "defined foo" becomes "1" if macro "foo"
    // is defined. Otherwise "0".
    //
    // "defined(foo)" 或 "defined foo" 在宏 "foo" 已定义时变成 "1",
    // 否则变成 "0".
    if (libcp_equal(tok, "defined")) {
      LibcpToken* start = tok;
      bool has_paren    = libcp_consume(&tok, tok->next, "(");

      if (tok->kind != TK_IDENT)
        libcp_error_tok(ctx, start, "macro name must be an identifier");
      LibcpMacro* m = find_macro(ctx, tok);
      tok           = tok->next;

      if (has_paren)
        tok = libcp_skip(ctx, tok, ")");

      cur = cur->next = new_num_token(ctx, m ? 1 : 0, start);
      continue;
    }

    cur = cur->next = tok;
    tok             = tok->next;
  }

  cur->next = tok;
  return head.next;
}

// Read and evaluate a constant expression.
//
// 读取并求值一个常量表达式 (#if / #elif 的条件), 返回其整数值.
static long eval_const_expr(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  LibcpToken* start = tok;
  LibcpToken* expr  = read_const_expr(ctx, rest, tok->next);
  expr              = preprocess2(ctx, expr);

  if (expr->kind == TK_EOF)
    libcp_error_tok(ctx, start, "no expression");

  // [https://www.sigbus.info/n1570#6.10.1p4] The standard requires
  // we replace remaining non-macro identifiers with "0" before
  // evaluating a constant expression. For example, `#if foo` is
  // equivalent to `#if 0` if foo is not defined.
  //
  // [https://www.sigbus.info/n1570#6.10.1p4] 标准要求在求值常量表达式
  // 之前, 把剩余的非宏标识符替换为 "0". 例如 foo 未定义时, `#if foo`
  // 等价于 `#if 0`.
  for (LibcpToken* t = expr; t->kind != TK_EOF; t = t->next) {
    if (t->kind == TK_IDENT) {
      LibcpToken* next = t->next;
      *t               = *new_num_token(ctx, 0, t);
      t->next          = next;
    }
  }

  // Convert pp-numbers to regular numbers
  // 把预处理数字转换为普通数值 token.
  libcp_convert_pp_tokens(ctx, expr);

  LibcpToken* rest2;
  long val = libcp_const_expr(ctx, &rest2, expr);
  if (rest2->kind != TK_EOF)
    libcp_error_tok(ctx, rest2, "extra token");
  return val;
}

// 压入一个条件编译栈帧, 初始状态为 IN_THEN.
static LibcpCondIncl* push_cond_incl(LibcpContext* ctx, LibcpToken* tok, bool included) {
  LibcpCondIncl* ci = libcp_arena_calloc(ctx, 1, sizeof(LibcpCondIncl));
  ci->next          = cond_incl;
  ci->ctx           = IN_THEN;
  ci->tok           = tok;
  ci->included      = included;
  cond_incl         = ci;
  return ci;
}

// 只有标识符才可能是宏; 以 token 的拼写 (loc, len) 查表, 不要求 NUL 结尾.
static LibcpMacro* find_macro(LibcpContext* ctx, LibcpToken* tok) {
  if (tok->kind != TK_IDENT)
    return NULL;
  return libcp_hashmap_get2(ctx, &macros, tok->loc, tok->len);
}

// 新建宏记录并登记进宏表; 同名旧宏被覆盖.
static LibcpMacro* add_macro(LibcpContext* ctx, char* name, bool is_objlike, LibcpToken* body) {
  LibcpMacro* m = libcp_arena_calloc(ctx, 1, sizeof(LibcpMacro));
  m->name       = name;
  m->is_objlike = is_objlike;
  m->body       = body;
  libcp_hashmap_put(ctx, &macros, name, m);
  return m;
}

// 读函数式宏的形参表; 处理 `...` 与 GNU 具名可变参数 (name...).
static LibcpMacroParam*
    read_macro_params(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, char** va_args_name) {
  LibcpMacroParam head = {};
  LibcpMacroParam* cur = &head;

  while (!libcp_equal(tok, ")")) {
    if (cur != &head)
      tok = libcp_skip(ctx, tok, ",");

    if (libcp_equal(tok, "...")) {
      *va_args_name = "__VA_ARGS__";
      *rest         = libcp_skip(ctx, tok->next, ")");
      return head.next;
    }

    if (tok->kind != TK_IDENT)
      libcp_error_tok(ctx, tok, "expected an identifier");

    if (libcp_equal(tok->next, "...")) {
      *va_args_name = libcp_arena_strndup(ctx, tok->loc, tok->len);
      *rest         = libcp_skip(ctx, tok->next->next, ")");
      return head.next;
    }

    LibcpMacroParam* m = libcp_arena_calloc(ctx, 1, sizeof(LibcpMacroParam));
    m->name            = libcp_arena_strndup(ctx, tok->loc, tok->len);
    cur = cur->next = m;
    tok             = tok->next;
  }

  *rest = tok->next;
  return head.next;
}

// 处理 #define: 名字后紧跟 `(` (中间没有空格) 才是函数式宏.
static void read_macro_definition(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  if (tok->kind != TK_IDENT)
    libcp_error_tok(ctx, tok, "macro name must be an identifier");
  LibcpToken* name_tok = tok;
  char* name           = libcp_arena_strndup(ctx, tok->loc, tok->len);
  tok                  = tok->next;

  if (!tok->has_space && libcp_equal(tok, "(")) {
    // Function-like macro
    // 函数式宏: 读形参表, 宏体是名字之后到行尾的 token.
    char* va_args_name      = NULL;
    LibcpMacroParam* params = read_macro_params(ctx, &tok, tok->next, &va_args_name);

    LibcpMacro* m   = add_macro(ctx, name, false, copy_line(ctx, rest, tok));
    m->params       = params;
    m->va_args_name = va_args_name;
    m->def          = name_tok;
  } else {
    // Object-like macro
    // 对象式宏: 没有形参, 整个定义体原样保存.
    LibcpMacro* m = add_macro(ctx, name, true, copy_line(ctx, rest, tok));
    m->def        = name_tok;
  }
}

// 读一个实参: 按括号深度配对逗号; read_rest 为真时一直读到右括号.
static LibcpMacroArg*
    read_macro_arg_one(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, bool read_rest) {
  LibcpToken head = {};
  LibcpToken* cur = &head;
  int level       = 0;

  for (;;) {
    if (level == 0 && libcp_equal(tok, ")"))
      break;
    if (level == 0 && !read_rest && libcp_equal(tok, ","))
      break;

    if (tok->kind == TK_EOF)
      libcp_error_tok(ctx, tok, "premature end of input");

    if (libcp_equal(tok, "("))
      level++;
    else if (libcp_equal(tok, ")"))
      level--;

    cur = cur->next = copy_token(ctx, tok);
    tok             = tok->next;
  }

  cur->next = new_eof(ctx, tok);

  LibcpMacroArg* arg = libcp_arena_calloc(ctx, 1, sizeof(LibcpMacroArg));
  arg->tok           = head.next;
  *rest              = tok;
  return arg;
}

// 依次读入各形参对应的实参, 再处理可变参数部分 (可以一个 token 都没有).
static LibcpMacroArg* read_macro_args(LibcpContext* ctx,
                                      LibcpToken** rest,
                                      LibcpToken* tok,
                                      LibcpMacroParam* params,
                                      char* va_args_name) {
  LibcpToken* start = tok;
  tok               = tok->next->next;

  LibcpMacroArg head = {};
  LibcpMacroArg* cur = &head;

  LibcpMacroParam* pp = params;
  int index           = 0;
  for (; pp; pp = pp->next) {
    if (cur != &head)
      tok = libcp_skip(ctx, tok, ",");
    cur = cur->next = read_macro_arg_one(ctx, &tok, tok, false);
    cur->name       = pp->name;
    cur->index      = index++;
  }

  if (va_args_name) {
    LibcpMacroArg* arg;
    if (libcp_equal(tok, ")")) {
      arg      = libcp_arena_calloc(ctx, 1, sizeof(LibcpMacroArg));
      arg->tok = new_eof(ctx, tok);
    } else {
      if (pp != params)
        tok = libcp_skip(ctx, tok, ",");
      arg = read_macro_arg_one(ctx, &tok, tok, true);
    }
    arg->name = va_args_name;
    ;
    arg->is_va_args = true;
    arg->index      = index;
    cur = cur->next = arg;
  } else if (pp) {
    libcp_error_tok(ctx, start, "too many arguments");
  }

  libcp_skip(ctx, tok, ")");
  *rest = tok;
  return head.next;
}

// 按拼写查找实参; 未命中说明该标识符不是这个宏的形参.
static LibcpMacroArg* find_arg(LibcpMacroArg* args, LibcpToken* tok) {
  for (LibcpMacroArg* ap = args; ap; ap = ap->next)
    if (tok->len == strlen(ap->name) && !strncmp(tok->loc, ap->name, tok->len))
      return ap;
  return NULL;
}

// Concatenates all tokens in `tok` and returns a new string.
//
// 把 `tok` 到 `end` 之间的 token 拼成一个字符串返回.
static char* join_tokens(LibcpContext* ctx, LibcpToken* tok, LibcpToken* end) {
  // Compute the length of the resulting token.
  // 先算长度: 各 token 文本之和, 加上 token 之间需要的空格, 再留出结尾 NUL.
  int len = 1;
  for (LibcpToken* t = tok; t != end && t->kind != TK_EOF; t = t->next) {
    if (t != tok && t->has_space)
      len++;
    len += t->len;
  }

  char* buf = libcp_arena_calloc(ctx, 1, len);

  // Copy token texts.
  // 再按同样规则拼接文本; 原 token 之间有空格时补一个空格.
  int pos = 0;
  for (LibcpToken* t = tok; t != end && t->kind != TK_EOF; t = t->next) {
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
//
// 把 `arg` 里的 token 拼接并做成一个字符串字面量 token, 供字符串化
// 运算符 (#) 使用.
static LibcpToken* stringize(LibcpContext* ctx, LibcpToken* hash, LibcpToken* arg) {
  // Create a new string token. We need to set some value to its
  // source location for error reporting function, so we use a macro
  // name token as a template.
  //
  // 新建一个字符串 token. 诊断需要 token 有源码位置, 因此用宏名 token
  // 作为位置模板.
  char* s = join_tokens(ctx, arg, NULL);
  return new_str_token(ctx, s, hash);
}

// Concatenate two tokens to create a new token.
//
// 把两个 token 的拼写拼接起来, 再重新切词, 得到 `##` 的结果 token.
static LibcpToken* paste(LibcpContext* ctx, LibcpToken* lhs, LibcpToken* rhs) {
  // Paste the two tokens.
  // 先直接拼接两个 token 的拼写文本.
  char* buf = libcp_format(ctx, "%.*s%.*s", lhs->len, lhs->loc, rhs->len, rhs->loc);

  // Tokenize the resulting string.
  // 再对结果串切词; 若得到不止一个 token, 说明拼接结果不是合法 token.
  LibcpToken* tok =
      tokenize_nested(ctx, libcp_new_file(ctx, lhs->file->name, lhs->file->file_no, buf));
  if (tok->next->kind != TK_EOF)
    libcp_error_tok(ctx, lhs, "pasting forms '%s', an invalid token", buf);
  return tok;
}

// 判断 __VA_ARGS__ 是否非空, 供 __VA_OPT__ 决定是否产出内容.
static bool has_varargs(LibcpMacroArg* args) {
  for (LibcpMacroArg* ap = args; ap; ap = ap->next)
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
//
// 用给定的实参替换函数式宏的形参. 产出的每个 token 都带上展开记录 `e`:
// 宏体 token 与 `#` / `##` 合成的 token 记为宏体层, 实参 token 记下它来自
// 第几个实参. 紧邻 `#` 或 `##` 的实参不做宏展开, 因此它们的 provenance
// 就是调用点自身的链; 普通实参进来时已经展开过, 所以本函数前插的那一层
// 位于该 token 展开产出的链之前.
static LibcpToken*
    subst(LibcpContext* ctx, LibcpToken* tok, LibcpMacroArg* args, LibcpMacroExpansion* e) {
  LibcpToken head           = {};
  LibcpToken* cur           = &head;
  LibcpMacroProv* body_base = e->name->prov;

  while (tok->kind != TK_EOF) {
    // "#" followed by a parameter is replaced with stringized actuals.
    // "#" 后面跟形参时, 替换为字符串化后的实际参数.
    if (libcp_equal(tok, "#")) {
      LibcpMacroArg* arg = find_arg(args, tok->next);
      if (!arg)
        libcp_error_tok(ctx, tok->next, "'#' is not followed by a macro parameter");
      cur = cur->next = stringize(ctx, tok, arg->tok);
      cur->prov       = new_prov(ctx, e, -1, body_base);
      tok             = tok->next->next;
      continue;
    }

    // [GNU] If __VA_ARG__ is empty, `,##__VA_ARGS__` is expanded
    // to the empty token list. Otherwise, its expaned to `,` and
    // __VA_ARGS__.
    //
    // [GNU] 若 __VA_ARGS__ 为空, `,##__VA_ARGS__` 展开为空 token 列表;
    // 否则展开为 `,` 与 __VA_ARGS__.
    if (libcp_equal(tok, ",") && libcp_equal(tok->next, "##")) {
      LibcpMacroArg* arg = find_arg(args, tok->next->next);
      if (arg && arg->is_va_args) {
        if (arg->tok->kind == TK_EOF) {
          tok = tok->next->next->next;
        } else {
          cur = cur->next = copy_token(ctx, tok);
          cur->prov       = new_prov(ctx, e, -1, body_base);
          tok             = tok->next->next;
        }
        continue;
      }
    }

    if (libcp_equal(tok, "##")) {
      if (cur == &head)
        libcp_error_tok(ctx, tok, "'##' cannot appear at start of macro expansion");

      if (tok->next->kind == TK_EOF)
        libcp_error_tok(ctx, tok, "'##' cannot appear at end of macro expansion");

      LibcpMacroArg* arg = find_arg(args, tok->next);
      if (arg) {
        if (arg->tok->kind != TK_EOF) {
          *cur      = *paste(ctx, cur, arg->tok);
          cur->prov = new_prov(ctx, e, -1, body_base);
          for (LibcpToken* t = arg->tok->next; t->kind != TK_EOF; t = t->next) {
            cur = cur->next = copy_token(ctx, t);
            cur->prov       = new_prov(ctx, e, arg->index, t->prov);
          }
        }
        tok = tok->next->next;
        continue;
      }

      *cur      = *paste(ctx, cur, tok->next);
      cur->prov = new_prov(ctx, e, -1, body_base);
      tok       = tok->next->next;
      continue;
    }

    LibcpMacroArg* arg = find_arg(args, tok);

    if (arg && libcp_equal(tok->next, "##")) {
      LibcpToken* rhs = tok->next->next;

      if (arg->tok->kind == TK_EOF) {
        LibcpMacroArg* arg2 = find_arg(args, rhs);
        if (arg2) {
          for (LibcpToken* t = arg2->tok; t->kind != TK_EOF; t = t->next) {
            cur = cur->next = copy_token(ctx, t);
            cur->prov       = new_prov(ctx, e, arg2->index, t->prov);
          }
        } else {
          cur = cur->next = copy_token(ctx, rhs);
          cur->prov       = new_prov(ctx, e, -1, body_base);
        }
        tok = rhs->next;
        continue;
      }

      for (LibcpToken* t = arg->tok; t->kind != TK_EOF; t = t->next) {
        cur = cur->next = copy_token(ctx, t);
        cur->prov       = new_prov(ctx, e, arg->index, t->prov);
      }
      tok = tok->next;
      continue;
    }

    // If __VA_ARG__ is empty, __VA_OPT__(x) is expanded to the
    // empty token list. Otherwise, __VA_OPT__(x) is expanded to x.
    //
    // 若可变参数为空, __VA_OPT__(x) 展开为空 token 列表; 否则展开为 x.
    if (libcp_equal(tok, "__VA_OPT__") && libcp_equal(tok->next, "(")) {
      LibcpMacroArg* arg = read_macro_arg_one(ctx, &tok, tok->next->next, true);
      if (has_varargs(args))
        for (LibcpToken* t = arg->tok; t->kind != TK_EOF; t = t->next) {
          cur = cur->next = t;
          cur->prov       = new_prov(ctx, e, -1, body_base);
        }
      tok = libcp_skip(ctx, tok, ")");
      continue;
    }

    // Handle a macro token. Macro arguments are completely macro-expanded
    // before they are substituted into a macro body.
    //
    // 处理宏形参: 实参在替换进宏体之前先完整展开一次.
    if (arg) {
      LibcpToken* t = preprocess2(ctx, arg->tok);
      t->at_bol     = tok->at_bol;
      t->has_space  = tok->has_space;
      for (; t->kind != TK_EOF; t = t->next) {
        cur = cur->next = copy_token(ctx, t);
        cur->prov       = new_prov(ctx, e, arg->index, t->prov);
      }
      tok = tok->next;
      continue;
    }

    // Handle a non-macro token.
    // 非宏 token: 原样复制, provenance 记为宏体层.
    cur = cur->next = copy_token(ctx, tok);
    cur->prov       = new_prov(ctx, e, -1, body_base);
    tok             = tok->next;
    continue;
  }

  cur->next = tok;
  return head.next;
}

// If tok is a macro, expand it and return true.
// Otherwise, do nothing and return false.
//
// tok 是宏就展开它并返回 true; 否则什么也不做, 返回 false.
static bool expand_macro(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  if (hideset_contains(tok->hideset, tok->loc, tok->len))
    return false;

  LibcpMacro* m = find_macro(ctx, tok);
  if (!m)
    return false;

  // Built-in dynamic macro application such as __LINE__
  // 内建动态宏的应用, 例如 __LINE__: 由处理器现场生成 token.
  if (m->handler) {
    LibcpMacroExpansion* e = new_expansion(ctx, tok, NULL, m, NULL, 0, true);
    LibcpToken* body       = m->handler(ctx, tok);
    for (LibcpToken* t = body; t->kind != TK_EOF; t = t->next)
      t->prov = new_prov(ctx, e, -1, tok->prov);

    // A handler may produce more than one token; splice after the last
    // one, not after the head, so none is dropped. An empty body expands
    // to nothing (B8).
    //
    // 处理器可能产出多个 token; 要接在最后一个之后而不是头部, 这样才
    // 一个都不丢. 空的宏体展开为什么都没有 (B8).
    if (body->kind == TK_EOF) {
      *rest = tok->next;
      return true;
    }

    LibcpToken* tail = body;
    while (tail->next->kind != TK_EOF)
      tail = tail->next;
    tail->next = tok->next;
    *rest      = body;
    return true;
  }

  // Object-like macro application
  // 对象式宏的应用: 宏体展开后接到原 token 之后.
  if (m->is_objlike) {
    LibcpMacroExpansion* e = new_expansion(ctx, tok, NULL, m, NULL, 0, false);
    LibcpHideset* hs       = hideset_union(ctx, tok->hideset, new_hideset(ctx, m->name));
    LibcpToken* body       = add_hideset(ctx, m->body, hs);
    for (LibcpToken* t = body; t->kind != TK_EOF; t = t->next) {
      t->origin = tok;
      t->prov   = new_prov(ctx, e, -1, tok->prov);
    }
    *rest              = append(ctx, body, tok->next);
    (*rest)->at_bol    = tok->at_bol;
    (*rest)->has_space = tok->has_space;
    return true;
  }

  // If a funclike macro token is not followed by an argument list,
  // treat it as a normal identifier.
  //
  // 函数式宏的名字后面若没有实参列表, 就把它当作普通标识符.
  if (!libcp_equal(tok->next, "("))
    return false;

  // Function-like macro application
  // 函数式宏的应用: 先读实参, 再把实参代入宏体.
  LibcpToken* macro_token = tok;
  LibcpMacroArg* args     = read_macro_args(ctx, &tok, tok, m->params, m->va_args_name);
  LibcpToken* rparen      = tok;
  LibcpMacroExpansion* e =
      new_expansion(ctx, macro_token, rparen, m, args, count_macro_args(args), false);

  // Tokens that consist a func-like macro invocation may have different
  // hidesets, and if that's the case, it's not clear what the hideset
  // for the new tokens should be. We take the interesection of the
  // macro token and the closing parenthesis and use it as a new hideset
  // as explained in the Dave Prossor's algorithm.
  //
  // 组成一次函数式宏调用的 token 可能带有不同的 hideset, 此时新 token 该
  // 用哪个 hideset 并不明确. 按 Dave Prossor 算法, 取宏名 token 与右括号
  // token 的 hideset 交集作为新的 hideset.
  LibcpHideset* hs = hideset_intersection(ctx, macro_token->hideset, rparen->hideset);
  hs               = hideset_union(ctx, hs, new_hideset(ctx, m->name));

  LibcpToken* body = subst(ctx, m->body, args, e);
  body             = add_hideset(ctx, body, hs);
  for (LibcpToken* t = body; t->kind != TK_EOF; t = t->next)
    t->origin = macro_token;
  *rest              = append(ctx, body, tok->next);
  (*rest)->at_bol    = macro_token->at_bol;
  (*rest)->has_space = macro_token->has_space;
  return true;
}

// 在 include 路径中查找文件: 绝对路径直接返回, 结果按文件名缓存在
// context 的 search_cache 里.
char* libcp_search_include_paths(LibcpContext* ctx, char* filename) {
  if (filename[0] == '/')
    return filename;

  char* cached = libcp_hashmap_get(ctx, &ctx->search_cache, filename);
  if (cached)
    return cached;

  // Search a file from the include paths.
  // 按顺序在各个 include 路径下尝试拼出完整路径.
  for (int i = 0; i < ctx->include_paths_len; i++) {
    char* path = libcp_format(ctx, "%s/%s", ctx->include_paths[i], filename);
    if (!libcp_file_exists(path))
      continue;
    libcp_hashmap_put(ctx, &ctx->search_cache, filename, path);
    // 记下命中位置的下一个下标, 供 #include_next 从后面继续搜索.
    ctx->include_next_idx = i + 1;
    return path;
  }
  return NULL;
}

// 从上次命中的下一个目录开始继续搜索 (GNU 的 #include_next).
static char* search_include_next(LibcpContext* ctx, char* filename) {
  for (; ctx->include_next_idx < ctx->include_paths_len; ctx->include_next_idx++) {
    char* path = libcp_format(ctx, "%s/%s", ctx->include_paths[ctx->include_next_idx], filename);
    if (libcp_file_exists(path))
      return path;
  }
  return NULL;
}

// Read an #include argument.
//
// 读取 #include 的参数, 返回文件名; *is_dquote 表示是否用了双引号形式.
static char*
    read_include_filename(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, bool* is_dquote) {
  // Pattern 1: #include "foo.h"
  // 形式 1: 双引号文件名, 先在当前文件所在目录找, 再走 include 路径.
  if (tok->kind == TK_STR) {
    // A double-quoted filename for #include is a special kind of
    // token, and we don't want to interpret any escape sequences in it.
    // For example, "\f" in "C:\foo" is not a formfeed character but
    // just two non-control characters, backslash and f.
    // So we don't want to use token->str.
    //
    // 双引号里的 #include 文件名是一种特殊 token, 不解释其中的转义序列.
    // 例如 "C:\foo" 里的 "\f" 不是换页符, 而就是反斜杠和 f 两个普通字符,
    // 因此不能使用 token->str.
    *is_dquote = true;
    *rest      = skip_line(ctx, tok->next);
    return libcp_arena_strndup(ctx, tok->loc + 1, tok->len - 2);
  }

  // Pattern 2: #include <foo.h>
  // 形式 2: 尖括号文件名, 只走 include 路径.
  if (libcp_equal(tok, "<")) {
    // Reconstruct a filename from a sequence of tokens between
    // "<" and ">".
    // 把 "<" 与 ">" 之间的 token 序列重新拼成文件名.
    LibcpToken* start = tok;

    // Find closing ">".
    // 找到配对的 ">"; 中途遇到行首或文件尾即为语法错误.
    for (; !libcp_equal(tok, ">"); tok = tok->next)
      if (tok->at_bol || tok->kind == TK_EOF)
        libcp_error_tok(ctx, tok, "expected '>'");

    *is_dquote = false;
    *rest      = skip_line(ctx, tok->next);
    return join_tokens(ctx, start->next, tok);
  }

  // Pattern 3: #include FOO
  // In this case FOO must be macro-expanded to either
  // a single string token or a sequence of "<" ... ">".
  //
  // 形式 3: #include FOO. 这里 FOO 必须先宏展开, 结果是单个字符串 token
  // 或一段 "<" ... ">" 序列, 然后递归按前两种形式处理.
  if (tok->kind == TK_IDENT) {
    LibcpToken* tok2 = preprocess2(ctx, copy_line(ctx, rest, tok));
    return read_include_filename(ctx, &tok2, tok2, is_dquote);
  }

  libcp_error_tok(ctx, tok, "expected a filename");
}

// Detect the following "include guard" pattern.
//
//   #ifndef FOO_H
//   #define FOO_H
//   ...
//   #endif
//
// 识别上面这种 include guard 写法: 文件以 #ifndef FOO_H / #define FOO_H
// 开头, 并以匹配的 #endif 结尾. 命中时返回宏名, 供 include_file 跳过
// 重复打开.
static char* detect_include_guard(LibcpContext* ctx, LibcpToken* tok) {
  // Detect the first two lines.
  // 先检查头两行: #ifndef 与紧随其后的同名 #define.
  if (!is_hash(tok) || !libcp_equal(tok->next, "ifndef"))
    return NULL;
  tok = tok->next->next;

  if (tok->kind != TK_IDENT)
    return NULL;

  char* macro = libcp_arena_strndup(ctx, tok->loc, tok->len);
  tok         = tok->next;

  if (!is_hash(tok) || !libcp_equal(tok->next, "define") || !libcp_equal(tok->next->next, macro))
    return NULL;

  // Read until the end of the file.
  // 一直读到文件尾, 要求最后一条指令是 #endif; 中间的条件块整体跳过.
  while (tok->kind != TK_EOF) {
    if (!is_hash(tok)) {
      tok = tok->next;
      continue;
    }

    if (libcp_equal(tok->next, "endif") && tok->next->next->kind == TK_EOF)
      return macro;

    if (libcp_equal(tok, "if") || libcp_equal(tok, "ifdef") || libcp_equal(tok, "ifndef"))
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
//
// 通过不同拼写的路径自我包含的文件, guard 与 pragma once 两张表都拦不住;
// 一个文件还可能拉进数量不受限的头文件. 因此给两者都设上限; 真实的翻译
// 单元远低于这两个值.
//
// MAX_INCLUDE_DEPTH 是包含深度上限, MAX_INCLUDE_COUNT 是一次预处理中
// 打开文件的总数上限.
#define MAX_INCLUDE_DEPTH 200
#define MAX_INCLUDE_COUNT 10000

static LibcpToken* include_file(LibcpContext* ctx,
                                LibcpToken* tok,
                                char* path,
                                LibcpToken* filename_tok,
                                LibcpFile* parent) {
  // Check for "#pragma once"
  // 先看 #pragma once: 这个路径已经包含过就直接跳过.
  if (libcp_hashmap_get(ctx, &pragma_once, path))
    return tok;

  // If we read the same file before, and if the file was guarded
  // by the usual #ifndef ... #endif pattern, we may be able to
  // skip the file without opening it.
  //
  // 若这个文件以前读过, 且是常见的 #ifndef ... #endif 保护写法, 那么
  // 连打开文件都可以省掉, 直接看 guard 宏是否已定义.
  char* guard_name = libcp_hashmap_get(ctx, &ctx->include_guards, path);
  if (guard_name && libcp_hashmap_get(ctx, &macros, guard_name))
    return tok;

  if (parent->include_depth >= MAX_INCLUDE_DEPTH)
    libcp_error_tok(ctx, filename_tok, "include depth exceeded");
  if (++ctx->include_count > MAX_INCLUDE_COUNT)
    libcp_error_tok(ctx, filename_tok, "too many includes");

  LibcpToken* tok2 = tokenize_file_nested(ctx, path);
  if (!tok2)
    libcp_error_tok(ctx, filename_tok, "%s: cannot open file: %s", path, strerror(errno));
  tok2->file->include_depth = parent->include_depth + 1;

  guard_name = detect_include_guard(ctx, tok2);
  if (guard_name)
    libcp_hashmap_put(ctx, &ctx->include_guards, path, guard_name);

  return append(ctx, tok2, tok);
}

// Read #line arguments
//
// 读取 #line 的参数: 行号, 以及可选的文件名; 行号差值记在文件的
// line_delta 上, 文件名覆盖显示名.
static void read_line_marker(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  LibcpToken* start = tok;
  tok               = libcp_preprocess(ctx, copy_line(ctx, rest, tok));

  if (tok->kind != TK_NUM || tok->ty->kind != TY_INT)
    libcp_error_tok(ctx, tok, "invalid line marker");
  start->file->line_delta = tok->val - start->line_no;

  tok = tok->next;
  if (tok->kind == TK_EOF)
    return;

  if (tok->kind != TK_STR)
    libcp_error_tok(ctx, tok, "filename expected");
  start->file->display_name = tok->str;
}

// Visit all tokens in `tok` while evaluating preprocessing
// macros and directives.
//
// 遍历 `tok` 中的所有 token, 边遍历边求值预处理宏与指令, 返回新的 token 链.
static LibcpToken* preprocess2(LibcpContext* ctx, LibcpToken* tok) {
  LibcpToken head = {};
  LibcpToken* cur = &head;

  while (tok->kind != TK_EOF) {
    // If it is a macro, expand it.
    // 是宏就展开; 展开结果继续在同一循环里处理.
    if (expand_macro(ctx, &tok, tok))
      continue;

    // Pass through if it is not a "#".
    // 不是 "#" 就原样放行, 同时记下行号偏移与显示文件名, 供诊断与
    // __LINE__ / __FILE__ 使用.
    if (!is_hash(tok)) {
      tok->line_delta = tok->file->line_delta;
      tok->filename   = tok->file->display_name;
      cur = cur->next = tok;
      tok             = tok->next;
      continue;
    }

    LibcpToken* start = tok;
    tok               = tok->next;

    if (libcp_equal(tok, "include")) {
      bool is_dquote;
      char* filename = read_include_filename(ctx, &tok, tok->next, &is_dquote);

      if (filename[0] != '/' && is_dquote) {
        char* path = libcp_format(ctx,
                                  "%s/%s",
                                  dirname(libcp_arena_strdup(ctx, start->file->name)),
                                  filename);
        if (libcp_file_exists(path)) {
          tok = include_file(ctx, tok, path, start->next->next, start->file);
          continue;
        }
      }

      char* path = libcp_search_include_paths(ctx, filename);
      tok        = include_file(ctx, tok, path ? path : filename, start->next->next, start->file);
      continue;
    }

    if (libcp_equal(tok, "include_next")) {
      bool ignore;
      char* filename = read_include_filename(ctx, &tok, tok->next, &ignore);
      char* path     = search_include_next(ctx, filename);
      tok = include_file(ctx, tok, path ? path : filename, start->next->next, start->file);
      continue;
    }

    if (libcp_equal(tok, "define")) {
      read_macro_definition(ctx, &tok, tok->next);
      continue;
    }

    if (libcp_equal(tok, "undef")) {
      tok = tok->next;
      if (tok->kind != TK_IDENT)
        libcp_error_tok(ctx, tok, "macro name must be an identifier");
      libcp_undef_macro(ctx, libcp_arena_strndup(ctx, tok->loc, tok->len));
      tok = skip_line(ctx, tok->next);
      continue;
    }

    if (libcp_equal(tok, "if")) {
      long val = eval_const_expr(ctx, &tok, tok);
      push_cond_incl(ctx, start, val);
      if (!val)
        tok = skip_cond_incl(tok);
      continue;
    }

    if (libcp_equal(tok, "ifdef")) {
      bool defined = find_macro(ctx, tok->next);
      push_cond_incl(ctx, tok, defined);
      tok = skip_line(ctx, tok->next->next);
      if (!defined)
        tok = skip_cond_incl(tok);
      continue;
    }

    if (libcp_equal(tok, "ifndef")) {
      bool defined = find_macro(ctx, tok->next);
      push_cond_incl(ctx, tok, !defined);
      tok = skip_line(ctx, tok->next->next);
      if (defined)
        tok = skip_cond_incl(tok);
      continue;
    }

    if (libcp_equal(tok, "elif")) {
      if (!cond_incl || cond_incl->ctx == IN_ELSE)
        libcp_error_tok(ctx, start, "stray #elif");
      cond_incl->ctx = IN_ELIF;

      if (!cond_incl->included && eval_const_expr(ctx, &tok, tok))
        cond_incl->included = true;
      else
        tok = skip_cond_incl(tok);
      continue;
    }

    if (libcp_equal(tok, "else")) {
      if (!cond_incl || cond_incl->ctx == IN_ELSE)
        libcp_error_tok(ctx, start, "stray #else");
      cond_incl->ctx = IN_ELSE;
      tok            = skip_line(ctx, tok->next);

      if (cond_incl->included)
        tok = skip_cond_incl(tok);
      continue;
    }

    if (libcp_equal(tok, "endif")) {
      if (!cond_incl)
        libcp_error_tok(ctx, start, "stray #endif");
      cond_incl = cond_incl->next;
      tok       = skip_line(ctx, tok->next);
      continue;
    }

    if (libcp_equal(tok, "line")) {
      read_line_marker(ctx, &tok, tok->next);
      continue;
    }

    if (tok->kind == TK_PP_NUM) {
      read_line_marker(ctx, &tok, tok);
      continue;
    }

    if (libcp_equal(tok, "pragma") && libcp_equal(tok->next, "once")) {
      libcp_hashmap_put(ctx, &pragma_once, tok->file->name, (void*)1);
      tok = skip_line(ctx, tok->next->next);
      continue;
    }

    if (libcp_equal(tok, "pragma")) {
      do {
        tok = tok->next;
      } while (!tok->at_bol);
      continue;
    }

    if (libcp_equal(tok, "error"))
      libcp_error_tok(ctx, tok, "error");

    // `#`-only line is legal. It's called a null directive.
    // 一行只有 `#` 是合法的, 这称为空指令 (null directive).
    if (tok->at_bol)
      continue;

    libcp_error_tok(ctx, tok, "invalid preprocessor directive");
  }

  cur->next = tok;
  return head.next;
}

// 把一段文本 (来自 -D 或内建宏) 定义成对象式宏; token 来自 <built-in>
// 这个合成文件.
void libcp_define_macro(LibcpContext* ctx, char* name, char* buf) {
  LibcpToken* tok = tokenize_nested(ctx, libcp_new_file(ctx, "<built-in>", 1, buf));
  add_macro(ctx, name, true, tok);
}

// 从宏表中删除一条定义; 名字本来就不存在时静默忽略.
void libcp_undef_macro(LibcpContext* ctx, char* name) {
  libcp_hashmap_delete(ctx, &macros, name);
}

// 登记一个由处理器现场求值的内建宏 (__LINE__, __FILE__ 等).
static LibcpMacro* add_builtin(LibcpContext* ctx, char* name, LibcpMacroHandlerFn* fn) {
  LibcpMacro* m = add_macro(ctx, name, true, NULL);
  m->handler    = fn;
  return m;
}

// __FILE__: 沿 origin 找到最外层 token, 取其文件的显示名.
static LibcpToken* file_macro(LibcpContext* ctx, LibcpToken* tmpl) {
  while (tmpl->origin)
    tmpl = tmpl->origin;
  return new_str_token(ctx, tmpl->file->display_name, tmpl);
}

// __LINE__: 当前行号加上 #line 带来的偏移.
static LibcpToken* line_macro(LibcpContext* ctx, LibcpToken* tmpl) {
  while (tmpl->origin)
    tmpl = tmpl->origin;
  int i = tmpl->line_no + tmpl->file->line_delta;
  return new_num_token(ctx, i, tmpl);
}

// __COUNTER__ is expanded to serial values starting from 0.
//
// __COUNTER__ 展开为从 0 开始的递增序号; 计数器保存在 context 里.
static LibcpToken* counter_macro(LibcpContext* ctx, LibcpToken* tmpl) {
  return new_num_token(ctx, ctx->counter++, tmpl);
}

// __TIMESTAMP__ is expanded to a string describing the last
// modification time of the current file. E.g.
// "Fri Jul 24 01:32:50 2020"
//
// __TIMESTAMP__ 展开为一个字符串, 描述当前文件的最后修改时间; 取不到
// 文件信息时用固定的问号串占位.
static LibcpToken* timestamp_macro(LibcpContext* ctx, LibcpToken* tmpl) {
  struct stat st;
  if (stat(tmpl->file->name, &st) != 0)
    return new_str_token(ctx, "??? ??? ?? ??:??:?? ????", tmpl);

  char buf[30];
  ctime_r(&st.st_mtime, buf);
  buf[24] = '\0';
  return new_str_token(ctx, buf, tmpl);
}

// __BASE_FILE__: 顶层源文件名, 由驱动器设置; 未设置时为空串.
static LibcpToken* base_file_macro(LibcpContext* ctx, LibcpToken* tmpl) {
  char* file = ctx->base_file;
  return new_str_token(ctx, file ? file : "", tmpl);
}

// __DATE__ is expanded to the current date, e.g. "May 17 2020".
//
// 把日期格式化成 __DATE__ 的字符串形式, 例如 "May 17 2020".
static char* format_date(LibcpContext* ctx, struct tm* tm) {
  static char mon[][4] = {
      "Jan",
      "Feb",
      "Mar",
      "Apr",
      "May",
      "Jun",
      "Jul",
      "Aug",
      "Sep",
      "Oct",
      "Nov",
      "Dec",
  };

  return libcp_format(ctx, "\"%s %2d %d\"", mon[tm->tm_mon], tm->tm_mday, tm->tm_year + 1900);
}

// __TIME__ is expanded to the current time, e.g. "13:34:03".
//
// 把时间格式化成 __TIME__ 的字符串形式, 例如 "13:34:03".
static char* format_time(LibcpContext* ctx, struct tm* tm) {
  return libcp_format(ctx, "\"%02d:%02d:%02d\"", tm->tm_hour, tm->tm_min, tm->tm_sec);
}

// __SIZEOF_* 宏要报告的字节数来自哪种内建类型.
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
} LibcpSizeKind;

// 预定义宏的四种来源: 固定文本, 内建类型大小, 动态处理器, 运行期日期时间.
typedef enum {
  PREDEF_MACRO,
  PREDEF_SIZEOF,
  PREDEF_BUILTIN,
  PREDEF_DYNAMIC,
} LibcpPredefKind;

// 预定义宏表的一项: 名字, 替换文本, 大小类, 来源与处理器.
typedef struct {
  char* name;
  char* value;
  LibcpSizeKind size;
  LibcpPredefKind kind;
  LibcpMacroHandlerFn* handler;
} LibcpPredefinedDef;

// 按大小类查对应内建类型的字节数; SZ_NONE 返回 0.
static int predefined_size(LibcpSizeKind size) {
  switch (size) {
    case SZ_DOUBLE: return libcp_ty_double->size;
    case SZ_FLOAT: return libcp_ty_float->size;
    case SZ_INT: return libcp_ty_int->size;
    case SZ_LDOUBLE: return libcp_ty_ldouble->size;
    case SZ_LONG_LONG: return libcp_ty_long->size;
    case SZ_LONG: return libcp_ty_long->size;
    case SZ_POINTER: return libcp_ty_ulong->size;
    case SZ_PTRDIFF: return libcp_ty_long->size;
    case SZ_SHORT: return libcp_ty_short->size;
    case SZ_SIZE: return libcp_ty_ulong->size;
    case SZ_NONE: break;
  }
  return 0;
}

// The predefined set. `value` is the replacement text; the __SIZEOF_*
// entries derive it from the built-in types, and the builtin/dynamic
// entries have no replacement text of their own.
//
// 预定义集合. `value` 是替换文本; __SIZEOF_* 项由内建类型推导出替换文本,
// builtin / dynamic 项没有自己的替换文本.
static const LibcpPredefinedDef predefined_defs[] = {
    {"_LP64",                    "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__C99_MACRO_WITH_VA_ARGS", "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__ELF__",                  "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__LP64__",                 "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__SIZEOF_DOUBLE__",        NULL,            SZ_DOUBLE,    PREDEF_SIZEOF,  NULL           },
    {"__SIZEOF_FLOAT__",         NULL,            SZ_FLOAT,     PREDEF_SIZEOF,  NULL           },
    {"__SIZEOF_INT__",           NULL,            SZ_INT,       PREDEF_SIZEOF,  NULL           },
    {"__SIZEOF_LONG_DOUBLE__",   NULL,            SZ_LDOUBLE,   PREDEF_SIZEOF,  NULL           },
    {"__SIZEOF_LONG_LONG__",     NULL,            SZ_LONG_LONG, PREDEF_SIZEOF,  NULL           },
    {"__SIZEOF_LONG__",          NULL,            SZ_LONG,      PREDEF_SIZEOF,  NULL           },
    {"__SIZEOF_POINTER__",       NULL,            SZ_POINTER,   PREDEF_SIZEOF,  NULL           },
    {"__SIZEOF_PTRDIFF_T__",     NULL,            SZ_PTRDIFF,   PREDEF_SIZEOF,  NULL           },
    {"__SIZEOF_SHORT__",         NULL,            SZ_SHORT,     PREDEF_SIZEOF,  NULL           },
    {"__SIZEOF_SIZE_T__",        NULL,            SZ_SIZE,      PREDEF_SIZEOF,  NULL           },
    {"__SIZE_TYPE__",            "unsigned long", SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__STDC_HOSTED__",          "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__STDC_NO_COMPLEX__",      "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__STDC_UTF_16__",          "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__STDC_UTF_32__",          "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__STDC_VERSION__",         "201112L",       SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__STDC__",                 "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__USER_LABEL_PREFIX__",    "",              SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__alignof__",              "_Alignof",      SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__amd64",                  "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__amd64__",                "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__chibicc__",              "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__const__",                "const",         SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__gnu_linux__",            "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__inline__",               "inline",        SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__linux",                  "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__linux__",                "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__signed__",               "signed",        SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__typeof__",               "typeof",        SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__unix",                   "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__unix__",                 "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__volatile__",             "volatile",      SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__x86_64",                 "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__x86_64__",               "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"linux",                    "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"unix",                     "1",             SZ_NONE,      PREDEF_MACRO,   NULL           },
    {"__FILE__",                 NULL,            SZ_NONE,      PREDEF_BUILTIN, file_macro     },
    {"__LINE__",                 NULL,            SZ_NONE,      PREDEF_BUILTIN, line_macro     },
    {"__COUNTER__",              NULL,            SZ_NONE,      PREDEF_BUILTIN, counter_macro  },
    {"__TIMESTAMP__",            NULL,            SZ_NONE,      PREDEF_BUILTIN, timestamp_macro},
    {"__BASE_FILE__",            NULL,            SZ_NONE,      PREDEF_BUILTIN, base_file_macro},
    {"__DATE__",                 NULL,            SZ_NONE,      PREDEF_DYNAMIC, NULL           },
    {"__TIME__",                 NULL,            SZ_NONE,      PREDEF_DYNAMIC, NULL           },
};

// 把预定义表安装进宏表; 同时把表本身挂在 context 上供只读查询.
void libcp_init_macros(LibcpContext* ctx) {
  if (!ctx || !ctx->predefined_macros)
    return;

  int n               = sizeof(predefined_defs) / sizeof(*predefined_defs);
  ctx->predefined_len = n;
  ctx->predefined     = libcp_arena_calloc(ctx, n, sizeof(LibcpPredefinedMacro));

  for (int i = 0; i < n; i++) {
    const LibcpPredefinedDef* d = &predefined_defs[i];
    LibcpPredefinedMacro* e     = &ctx->predefined[i];
    e->name                     = d->name;

    if (d->kind == PREDEF_SIZEOF) {
      char* value = libcp_format(ctx, "%d", predefined_size(d->size));
      e->value    = value;
      libcp_define_macro(ctx, d->name, value);
    } else if (d->kind == PREDEF_MACRO) {
      e->value = d->value;
      libcp_define_macro(ctx, d->name, d->value);
    } else if (d->kind == PREDEF_BUILTIN) {
      add_builtin(ctx, d->name, d->handler);
    }
  }
#ifdef LIBCP_TEST_BUILTINS
  add_builtin(ctx, "__LIBCP_MULTI__", multi_macro);
#endif

  // localtime returns a process-global buffer; contexts may be created
  // in different threads (context.h), so use the reentrant form.
  //
  // localtime 返回进程级共享缓冲区; context 可能在不同线程里创建
  // (见 context.h), 因此使用可重入版本. __DATE__ / __TIME__ 是
  // PREDEF_DYNAMIC 项, 在这里按当前时间定义.
  time_t now = time(NULL);
  struct tm tm_buf;
  localtime_r(&now, &tm_buf);
  libcp_define_macro(ctx, "__DATE__", format_date(ctx, &tm_buf));
  libcp_define_macro(ctx, "__TIME__", format_time(ctx, &tm_buf));
}

// 预定义宏表的只读访问器: count 返回条目数, at 返回第 index 项.
int libcp_context_predefined_count(LibcpContext* ctx) {
  return ctx ? ctx->predefined_len : 0;
}

LibcpPredefinedMacro libcp_context_predefined_at(LibcpContext* ctx, int index) {
  LibcpPredefinedMacro none = {NULL, NULL};
  if (!ctx || index < 0 || index >= ctx->predefined_len)
    return none;
  return ctx->predefined[index];
}

typedef enum {
  STR_NONE,
  STR_UTF8,
  STR_UTF16,
  STR_UTF32,
  STR_WIDE,
} LibcpStringKind;

// 按拼写前缀判断字符串字面量的种类: 普通 / u8 / u / U / L.
static LibcpStringKind getStringKind(LibcpContext* ctx, LibcpToken* tok) {
  // `loc` is the first byte of the spelling, not a NUL-terminated
  // prefix: `u8"..."` continues with the quote, so a strcmp against
  // "u8" never matches. Check the three bytes that form the prefix.
  //
  // `loc` 指向拼写的第一个字节, 不是以 NUL 结尾的前缀: `u8"..."` 后面紧跟
  // 引号, 所以用 strcmp 比较 "u8" 永远不匹配. 这里直接检查构成前缀的
  // 三个字节.
  if (tok->len >= 3 && tok->loc[0] == 'u' && tok->loc[1] == '8' && tok->loc[2] == '"')
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
//
// 按 C 标准把相邻的字符串字面量拼接成一个.
static void join_adjacent_string_literals(LibcpContext* ctx, LibcpToken* tok) {
  // First pass: If regular string literals are adjacent to wide
  // string literals, regular string literals are converted to a wide
  // type before concatenation. In this pass, we do the conversion.
  //
  // 第一遍: 普通字符串字面量若与宽字符串字面量相邻, 要在拼接前先转成
  // 宽类型; 这一遍负责该转换.
  // 相邻字面量种类不一致且都不是普通串时报错.
  for (LibcpToken* tok1 = tok; tok1->kind != TK_EOF;) {
    if (tok1->kind != TK_STR || tok1->next->kind != TK_STR) {
      tok1 = tok1->next;
      continue;
    }

    LibcpStringKind kind = getStringKind(ctx, tok1);
    LibcpType* basety    = tok1->ty->base;

    for (LibcpToken* t = tok1->next; t->kind == TK_STR; t = t->next) {
      LibcpStringKind k = getStringKind(ctx, t);
      if (kind == STR_NONE) {
        kind   = k;
        basety = t->ty->base;
      } else if (k != STR_NONE && kind != k) {
        libcp_error_tok(ctx, t, "unsupported non-standard concatenation of string literals");
      }
    }

    if (basety->size > 1)
      for (LibcpToken* t = tok1; t->kind == TK_STR; t = t->next)
        if (t->ty->base->size == 1) {
          // The re-scan replaces the whole token (and does not set
          // line_no); the provenance belongs to the source token.
          //
          // 重新切词会整体覆盖 token (且不会设置 line_no); provenance
          // 属于源码中的那个 token, 因此先取出再写回.
          LibcpMacroProv* prov = t->prov;
          *t                   = *libcp_tokenize_string_literal(ctx, t, basety);
          t->prov              = prov;
        }

    while (tok1->kind == TK_STR)
      tok1 = tok1->next;
  }

  // Second pass: concatenate adjacent string literals.
  // 第二遍: 把相邻的字面量真正拼成一个 token; 长度按各段宽度累计,
  // 并跳过每段结尾的 NUL.
  for (LibcpToken* tok1 = tok; tok1->kind != TK_EOF;) {
    if (tok1->kind != TK_STR || tok1->next->kind != TK_STR) {
      tok1 = tok1->next;
      continue;
    }

    LibcpToken* tok2 = tok1->next;
    while (tok2->kind == TK_STR)
      tok2 = tok2->next;

    int len = tok1->ty->array_len;
    for (LibcpToken* t = tok1->next; t != tok2; t = t->next)
      len = len + t->ty->array_len - 1;

    char* buf = libcp_arena_calloc(ctx, tok1->ty->base->size, len);

    int i = 0;
    for (LibcpToken* t = tok1; t != tok2; t = t->next) {
      memcpy(buf + i, t->str, t->ty->size);
      i = i + t->ty->size - t->ty->base->size;
    }

    *tok1      = *copy_token(ctx, tok1);
    tok1->ty   = libcp_array_of(ctx, tok1->ty->base, len);
    tok1->str  = buf;
    tok1->next = tok2;
    tok1       = tok2;
  }
}

// Entry point function of the preprocessor.
//
// 预处理器的入口: 展开宏与指令, 检查条件编译是否闭合, 转换 pp-number,
// 拼接相邻字符串字面量, 最后把 #line 的行号偏移并回每个 token.
LibcpToken* libcp_preprocess(LibcpContext* ctx, LibcpToken* tok) {
  ENTER_RET(ctx, NULL);
  tok = preprocess2(ctx, tok);
  if (cond_incl)
    libcp_error_tok(ctx, cond_incl->tok, "unterminated conditional directive");
  libcp_convert_pp_tokens(ctx, tok);
  join_adjacent_string_literals(ctx, tok);

  for (LibcpToken* t = tok; t; t = t->next)
    t->line_no += t->line_delta;
  LEAVE(ctx);
  return tok;
}

// Provenance accessors. Their bodies are trivial on purpose: the chain
// layout is preprocess.c's business, and consumers (provenance.c
// today, the library API later) see only these functions.
//
// provenance 访问器. 函数体刻意保持简单: 链的布局是 preprocess.c 的内部
// 事务, 消费者 (今天的 provenance.c, 以后的库 API) 只看到这些函数.

LibcpMacroProv* libcp_token_provenance(LibcpToken* tok) {
  return tok->prov;
}

LibcpMacroProv* libcp_macro_prov_next(LibcpMacroProv* p) {
  return p->next;
}

LibcpMacroExpansion* libcp_macro_prov_expansion(LibcpMacroProv* p) {
  return p->exp;
}

int libcp_macro_prov_arg_index(LibcpMacroProv* p) {
  return p->arg_index;
}

LibcpToken* libcp_macro_expansion_name(LibcpMacroExpansion* e) {
  return e->name;
}

LibcpToken* libcp_macro_expansion_end(LibcpMacroExpansion* e) {
  return e->rparen ? e->rparen : e->name;
}

char* libcp_macro_expansion_macro(LibcpMacroExpansion* e) {
  return e->macro->name;
}

LibcpToken* libcp_macro_expansion_def(LibcpMacroExpansion* e) {
  return e->macro->def;
}

LibcpToken* libcp_macro_expansion_arg(LibcpMacroExpansion* e, int i) {
  if (i < 0 || i >= e->arg_count)
    return NULL;

  LibcpMacroArg* arg = e->args;
  for (; i > 0 && arg; i--)
    arg = arg->next;
  return arg ? arg->tok : NULL;
}

int libcp_macro_expansion_arg_count(LibcpMacroExpansion* e) {
  return e->arg_count;
}

bool libcp_macro_expansion_is_builtin(LibcpMacroExpansion* e) {
  return e->is_builtin;
}
