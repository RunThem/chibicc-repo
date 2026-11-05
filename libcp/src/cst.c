// cst.c - 第 2 层: 无损 CST (lossless concrete syntax tree). 它建在预处理之前的
// 原始文本上, 预处理指令与条件分支都是节点, trivia 挂靠, 叶子可逐字节还原源码.
// 主要内容: 分隔符分组, 表达式优先级链, 声明与声明符, 语句, 预处理指令行,
// 以及重建 / dump / 校验三个只读通道.
#include "libcp.h"

// 建在原始 token 流上的无损具体语法树 (plans/PLAN.md 阶段 C1/C2/C3). 输入的
// 每个 token 依次成为一个叶子; 叶子的 trivia 与拼写可以逐字节还原输入文件.
//
// 这棵树负责分隔符分组, 切分语句并构建声明. 声明是 CST_DECL (specs + 各个
// init-declarator + ';') 或 CST_FUNC_DEF (specs + declarator + 函数体块);
// 参数是 CST_PARAM_LIST 内的 CST_PARAM; 类型名 (cast, sizeof, 复合字面量)
// 是 CST_TYPE_NAME. 语句覆盖 C 的各种语句形式, 每个关键字, 括号, 方括号与
// ';' 都是显式的. 表达式语句是 CST_EXPR_STMT.
//
// 文法无法覆盖的内容变成 CST_UNPARSED, 持有原始骨架子节点 (直到 ';' 的
// token, 其中嵌套的分隔符已分组). 缺少闭合 token 的分组或块, 以及在本层
// 找不到开括号的闭括号, 也都是 CST_UNPARSED; 它们已经持有的 token 仍留在树里.
//
// 预处理指令是独立节点 (CST_PP_*). 指令行从逻辑行首的 '#' 开始, 到下一个行首
// token 之前结束; 其中的 token 保持为叶子. 条件分组保留每个分支: CST_PP_IF
// 为 #if / #elif / #else 各持有一个 CST_PP_BRANCH, 再加收尾的 #endif; 分支体
// 用与外围相同的 item 文法构建. 条件从不求值, 宏从不展开.
//
// 表达式文法是 C 的优先级链, 一直到并包括逗号运算符. 每个运算符, 括号与
// 方括号都是显式叶子; 节点只做分组. 若 '(' 的第一个 token 能开始一个类型名
// (类型关键字, struct/union/enum/typeof/_Atomic 之一, 或本文件中先前见过的
// typedef 名), 它就是 cast 或复合字面量, 否则是带括号的表达式. 这里不做
// 名字查找, 不做类型计算, 也不做常量折叠: 文件局部的 typedef 集合加上拼写
// 就是全部的判定依据; 无法判定的解析退回骨架, 而不是猜测.
//
// 无损契约与树形无关: 解析器是全函数, 从不报诊断, 从不退出
// (plans/RESULT.md I1-I6).

// 限制 CST 解析器的递归深度. 超过深度时解析失败并退回 raw 骨架; 分组解析
// 则把余下部分摊成扁平叶子. 每个 token 都保留, 因此重建仍然无损.
#define MAX_CST_DEPTH 512

static LibcpCstNode* new_cst_node(LibcpContext* ctx, LibcpCstKind kind) {
  LibcpCstNode* node = libcp_arena_calloc(ctx, 1, sizeof(LibcpCstNode));
  node->kind         = kind;
  return node;
}

static void cst_add_child(LibcpContext* ctx, LibcpCstNode* node, LibcpCstNode* child) {
  // 容量存放在 `children` 之前的 size_t 头里; 公共节点只暴露 child_count
  // 与有效条目.
  size_t cap = node->children ? ((size_t*)node->children)[-1] : 0;
  if ((size_t)node->child_count == cap) {
    cap                     = cap ? cap * 2 : 8;
    size_t bytes            = sizeof(size_t) + cap * sizeof(LibcpCstNode*);
    char* p                 = libcp_arena_alloc(ctx, bytes, 8);
    *(size_t*)p             = cap;
    LibcpCstNode** children = (LibcpCstNode**)(p + sizeof(size_t));
    if (node->children)
      memcpy(children, node->children, (size_t)node->child_count * sizeof(LibcpCstNode*));
    node->children = children;
  }
  node->children[node->child_count++] = child;
}

// 在 parent 末尾追加一个叶子; EOF 标记用 CST_EOF, 其余 token 用 CST_TOKEN.
static LibcpCstNode* cst_add_leaf(LibcpContext* ctx, LibcpCstNode* parent, LibcpToken* tok) {
  LibcpCstNode* leaf = new_cst_node(ctx, tok->kind == TK_EOF ? CST_EOF : CST_TOKEN);
  leaf->tok          = tok;
  cst_add_child(ctx, parent, leaf);
  return leaf;
}

static bool cst_is_open(LibcpToken* tok) {
  return libcp_equal(tok, "(") || libcp_equal(tok, "[") || libcp_equal(tok, "{");
}

static bool cst_is_close(LibcpToken* tok) {
  return libcp_equal(tok, ")") || libcp_equal(tok, "]") || libcp_equal(tok, "}");
}

// 解析器的单文件状态: typedef 名 oracle 与实时递归深度 (F5b). 深度超限时
// 当前区间变成原始的未解析子节点; 不丢弃任何 token.
typedef struct {
  LibcpStringArray typedefs;
  int depth;
} LibcpCstContext;

// 从 `tok` 开始 ( '(' '[' '{' 之一 ) 按 raw 模式消费一个带定界符的分组:
// 内部不切分成语句或表达式, 只把嵌套的分隔符分组. 缺少闭合符或深度超限时,
// 节点变成 CST_UNPARSED, 但仍保留开括号与已消费的全部内容.
static LibcpCstNode*
    cst_parse_group(LibcpContext* ctx, LibcpCstContext* cst, LibcpToken** rest, LibcpToken* tok) {
  char* closer      = libcp_equal(tok, "(") ? ")" : libcp_equal(tok, "[") ? "]" : "}";
  LibcpCstKind kind = libcp_equal(tok, "(") ? CST_GROUP_PAREN :
                      libcp_equal(tok, "[") ? CST_GROUP_BRACKET :
                                              CST_GROUP_BRACE;

  LibcpCstNode* group = new_cst_node(ctx, kind);
  cst_add_leaf(ctx, group, tok);

  LibcpToken* p = tok->next;
  if (++cst->depth > MAX_CST_DEPTH) {
    // 深度回退: 把分组剩余部分保持为扁平叶子. 该分组是一个未解析区间;
    // token 顺序 (因而重建结果) 不变.
    group->kind = CST_UNPARSED;
    while (p && p->kind != TK_EOF && !libcp_equal(p, closer)) {
      cst_add_leaf(ctx, group, p);
      p = p->next;
    }
    if (p && p->kind != TK_EOF && libcp_equal(p, closer)) {
      cst_add_leaf(ctx, group, p);
      p = p->next;
    }
    cst->depth--;
    *rest = p;
    return group;
  }

  while (p && p->kind != TK_EOF && !libcp_equal(p, closer)) {
    if (cst_is_open(p)) {
      cst_add_child(ctx, group, cst_parse_group(ctx, cst, &p, p));
      continue;
    }
    cst_add_leaf(ctx, group, p);
    p = p->next;
  }

  if (p && p->kind != TK_EOF && libcp_equal(p, closer)) {
    cst_add_leaf(ctx, group, p);
    p = p->next;
  } else {
    group->kind = CST_UNPARSED;
  }
  cst->depth--;
  *rest = p;
  return group;
}

//
// 表达式解析 (C2.1)
//
// 在预处理之前的 token 流上做回溯递归下降. `ok` 在第一次失败时变为 false,
// 之后的调用者不再查看 `tok`; 语句层随即用原始骨架重建该区间, 因此一次失败
// 的尝试可能在 arena 里留下不可达的节点, 却不会在树里重复同一个 token.
//

// 表达式解析器的游标: 当前 token, 仍然成功的标志, 以及共享的单文件状态.
typedef struct {
  LibcpToken* tok;
  bool ok;
  LibcpCstContext* cst;
} LibcpCstParser;

static void cst_fail(LibcpCstParser* p) {
  p->ok = false;
}

static bool cst_at(LibcpCstParser* p, char* op) {
  return p->tok && p->tok->kind != TK_EOF && libcp_equal(p->tok, op);
}

static bool cst_in_list(LibcpToken* tok, char** list) {
  for (int i = 0; list[i]; i++)
    if (libcp_equal(tok, list[i]))
      return true;
  return false;
}

// 不能作为表达式操作数开头的词: C11 关键字, 类运算符关键字, 以及常见的
// GNU 拼写. 在原始流里关键字仍然是 TK_IDENT, 所以这实际上是一次拼写测试.
static bool cst_is_keyword(LibcpToken* tok) {
  char* kw[] = {
      "auto",
      "break",
      "case",
      "char",
      "const",
      "continue",
      "default",
      "do",
      "double",
      "else",
      "enum",
      "extern",
      "float",
      "for",
      "goto",
      "if",
      "inline",
      "int",
      "long",
      "register",
      "restrict",
      "return",
      "short",
      "signed",
      "sizeof",
      "static",
      "struct",
      "switch",
      "typedef",
      "union",
      "unsigned",
      "void",
      "volatile",
      "while",
      "_Alignas",
      "_Alignof",
      "_Atomic",
      "_Bool",
      "_Complex",
      "_Generic",
      "_Imaginary",
      "_Noreturn",
      "_Static_assert",
      "_Thread_local",
      "typeof",
      "__typeof__",
      "__typeof",
      "alignof",
      "__alignof__",
      "__alignof",
      "asm",
      "__asm__",
      "__attribute__",
      "__extension__",
      NULL,
  };
  return cst_in_list(tok, kw);
}

// tok 是否是已知的 typedef 名: 来自消费者种子或本文件内收集的集合.
static bool cst_is_typedef_name(LibcpCstContext* cst, LibcpToken* tok) {
  if (!tok || tok->kind != TK_IDENT)
    return false;
  for (int i = 0; i < cst->typedefs.len; i++)
    if (libcp_equal(tok, cst->typedefs.data[i]))
      return true;
  return false;
}

// tok 能否在 cast 或 sizeof 中开始一个类型名: 类型说明符 / 限定符关键字,
// 或文件局部的 typedef 名.
static bool cst_is_type_start(LibcpToken* tok, LibcpCstContext* cst) {
  char* kw[] = {
      "void",         "_Bool",      "char",         "short",    "int",        "long",
      "float",        "double",     "signed",       "unsigned", "_Complex",   "_Imaginary",
      "const",        "volatile",   "restrict",     "struct",   "union",      "enum",
      "typeof",       "__typeof__", "__typeof",     "_Atomic",  "__signed__", "__const__",
      "__volatile__", "__restrict", "__restrict__", NULL,
  };
  return cst_in_list(tok, kw) || cst_is_typedef_name(cst, tok);
}

// tok 能否开始一个声明. 包含存储类与只能出现在声明中的关键字; 不包含
// 语句关键字.
static bool cst_is_decl_start(LibcpToken* tok, LibcpCstContext* cst) {
  char* kw[] = {
      "typedef",       "static",        "extern",    "auto",         "register",
      "inline",        "_Thread_local", "__thread",  "_Noreturn",    "_Alignas",
      "const",         "volatile",      "restrict",  "__restrict",   "__restrict__",
      "__attribute__", "__extension__", "struct",    "union",        "enum",
      "typeof",        "__typeof__",    "__typeof",  "_Atomic",      "void",
      "_Bool",         "char",          "short",     "int",          "long",
      "float",         "double",        "signed",    "unsigned",     "_Complex",
      "_Imaginary",    "__signed__",    "__const__", "__volatile__", NULL,
  };
  return cst_in_list(tok, kw) || cst_is_typedef_name(cst, tok);
}

static void cst_add_typedef(LibcpContext* ctx, LibcpCstContext* cst, LibcpToken* tok) {
  libcp_strarray_push(ctx, &cst->typedefs, libcp_arena_strndup(ctx, tok->loc, tok->len));
}

// 对文件局部 typedef 名的保守预扫描: 收集 `typedef int T;`, `typedef int *T;`,
// `typedef int T[3];`, `typedef int A, B;`, `typedef struct S T;` 与
// `typedef int (*T)(int);` 引入的名字; 不收集参数名与 struct 标签. 这就是
// 计划允许的 cast / 声明 oracle - 由 include 提供的名字保持未知, 不做猜测.
static void cst_collect_typedefs(LibcpContext* ctx, LibcpCstContext* cst, LibcpToken* tok) {
  LibcpToken* p = tok;

  while (p && p->kind != TK_EOF) {
    if (p->kind != TK_IDENT || !libcp_equal(p, "typedef")) {
      p = p->next;
      continue;
    }

    int depth        = 0;
    LibcpToken* prev = p;
    LibcpToken* q    = p->next;

    for (; q && q->kind != TK_EOF; prev = q, q = q->next) {
      if (libcp_equal(q, "(") || libcp_equal(q, "[") || libcp_equal(q, "{")) {
        depth++;
        continue;
      }
      if (libcp_equal(q, ")") || libcp_equal(q, "]") || libcp_equal(q, "}")) {
        depth--;
        continue;
      }
      if (depth == 0 && libcp_equal(q, ";"))
        break;
      if (q->kind != TK_IDENT)
        continue;

      // 顶层形式: 名字后紧跟 ';' / ',' / '[' 的是被声明的类型名.
      if (depth == 0 &&
          (libcp_equal(q->next, ";") || libcp_equal(q->next, ",") || libcp_equal(q->next, "[")))
        cst_add_typedef(ctx, cst, q);
      // 函数指针形式 "(*T)": 名字在深度 1, 前一个 token 是 '*' 且后面是 ')'.
      else if (depth == 1 && libcp_equal(prev, "*") && libcp_equal(q->next, ")"))
        cst_add_typedef(ctx, cst, q);
    }

    p = q ? q : p->next;
  }
}

static LibcpCstNode* cst_parse_comma(LibcpContext* ctx, LibcpCstParser* p);
static LibcpCstNode* cst_parse_assign(LibcpContext* ctx, LibcpCstParser* p);
static LibcpCstNode* cst_parse_conditional(LibcpContext* ctx, LibcpCstParser* p);
static LibcpCstNode* cst_parse_unary(LibcpContext* ctx, LibcpCstParser* p);
static LibcpCstNode* cst_parse_postfix(LibcpContext* ctx, LibcpCstParser* p);
static LibcpCstNode*
    cst_parse_postfix_tail(LibcpContext* ctx, LibcpCstParser* p, LibcpCstNode* node);
static LibcpCstNode* cst_parse_primary(LibcpContext* ctx, LibcpCstParser* p);
static LibcpCstNode* cst_parse_arg_list(LibcpContext* ctx, LibcpCstParser* p);
static LibcpCstNode* cst_parse_init_list(LibcpContext* ctx, LibcpCstParser* p);
static LibcpCstNode* cst_parse_type_name(LibcpContext* ctx, LibcpCstParser* p);
static LibcpCstNode* cst_parse_type_name_core(LibcpContext* ctx, LibcpCstParser* p);
static LibcpCstNode*
    cst_parse_declarator(LibcpContext* ctx, LibcpCstParser* p, bool allow_abstract);
static LibcpCstNode* cst_parse_decl(LibcpContext* ctx, LibcpCstParser* p);
static void cst_parse_compound(LibcpContext* ctx, LibcpCstParser* p, LibcpCstNode* block);
// 每个带深度保护的表达式 / 声明符入口背后真正的实现函数.
static LibcpCstNode* cst_parse_comma1(LibcpContext* ctx, LibcpCstParser* p);
static LibcpCstNode* cst_parse_assign1(LibcpContext* ctx, LibcpCstParser* p);
static LibcpCstNode* cst_parse_conditional1(LibcpContext* ctx, LibcpCstParser* p);
static LibcpCstNode* cst_parse_unary1(LibcpContext* ctx, LibcpCstParser* p);
static LibcpCstNode* cst_parse_init_list1(LibcpContext* ctx, LibcpCstParser* p);
static LibcpCstNode*
    cst_parse_declarator1(LibcpContext* ctx, LibcpCstParser* p, bool allow_abstract);

// 为 cast 或 sizeof 中的类型名, 按 raw 模式消费一个配平的分组. 分组未闭合
// 时整个表达式失败: 语句退回骨架, 该区间变成 UNPARSED.
static LibcpCstNode* cst_parse_raw_group(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpToken* rest    = NULL;
  LibcpCstNode* group = cst_parse_group(ctx, p->cst, &rest, p->tok);

  if (group->kind == CST_UNPARSED) {
    cst_fail(p);
    return NULL;
  }
  p->tok = rest;
  return group;
}

// primary = ident | number | char | string+ | "(" expr ")"
// 主表达式: 标识符 / 数字 / 字符 / 连续字符串字面量 / 括号表达式.
static LibcpCstNode* cst_parse_primary(LibcpContext* ctx, LibcpCstParser* p) {
  if (!p->ok)
    return NULL;

  LibcpToken* tok = p->tok;
  if (!tok || tok->kind == TK_EOF) {
    cst_fail(p);
    return NULL;
  }

  if (libcp_equal(tok, "(")) {
    // GNU 语句表达式: "(" block ")". 块解析器从花括号开始运行, 因此内部
    // 条目保持完整的语句文法 (PLAN-l5 W1: 该区间以前会退回 CST_UNPARSED).
    if (libcp_equal(tok->next, "{")) {
      LibcpCstNode* node = new_cst_node(ctx, CST_STMT_EXPR);
      cst_add_leaf(ctx, node, tok);
      p->tok = tok->next;

      LibcpCstNode* block = new_cst_node(ctx, CST_BLOCK);
      cst_parse_compound(ctx, p, block);
      if (!p->ok)
        return NULL;
      cst_add_child(ctx, node, block);

      if (!cst_at(p, ")")) {
        cst_fail(p);
        return NULL;
      }
      cst_add_leaf(ctx, node, p->tok);
      p->tok = p->tok->next;
      return node;
    }

    LibcpCstNode* node = new_cst_node(ctx, CST_PAREN);
    cst_add_leaf(ctx, node, tok);

    p->tok              = tok->next;
    LibcpCstNode* inner = cst_parse_comma(ctx, p);
    if (!p->ok || !cst_at(p, ")")) {
      cst_fail(p);
      return NULL;
    }

    cst_add_child(ctx, node, inner);
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;
    return node;
  }

  // _Generic 在 C11 里是主表达式, 但在这里是关键字; 它形如调用, 因此变成
  // 一个 PRIMARY 叶子, 由后缀尾部的参数列表接手 (各个分支按关联参数解析,
  // 见 cst_parse_arg).
  if (libcp_equal(tok, "_Generic") && libcp_equal(tok->next, "(")) {
    LibcpCstNode* node = new_cst_node(ctx, CST_PRIMARY);
    cst_add_leaf(ctx, node, tok);
    p->tok = tok->next;
    return node;
  }

  if (tok->kind == TK_PUNCT || cst_is_keyword(tok)) {
    cst_fail(p);
    return NULL;
  }

  // 标识符 (关键字在这里仍是标识符), 数字, 字符与字符串字面量. 相邻的字符串
  // 字面量合为一个 primary; 每个仍保持为独立的显式叶子.
  LibcpCstNode* node = new_cst_node(ctx, CST_PRIMARY);
  cst_add_leaf(ctx, node, tok);
  p->tok = tok->next;

  while (p->tok && p->tok->kind == TK_STR) {
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;
  }
  return node;
}

// arg = assign
//     | type-name                              (内建类型实参)
//     | (type-name | "default") ":" assign     (_Generic 关联项)
//
// 这些类型形式不是赋值表达式, 但 __builtin_types_compatible_p(int, int),
// __builtin_offsetof(struct T, m) 与 _Generic(100.0, double: 1, default: 0)
// 这类调用会把它们当作实参. 先尝试赋值表达式; 失败时只回退当前这一个实参,
// 失败尝试留下的节点不可达, 这是允许的 (PLAN-l5 W1).
static LibcpCstNode* cst_parse_arg(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpToken* start = p->tok;

  LibcpCstNode* arg = cst_parse_assign(ctx, p);
  if (p->ok)
    return arg;

  p->ok  = true;
  p->tok = start;

  LibcpCstNode* ty;
  if (libcp_equal(p->tok, "default")) {
    // _Generic 的 default 分支在这里是关键字, 不是类型名.
    ty = new_cst_node(ctx, CST_PRIMARY);
    cst_add_leaf(ctx, ty, p->tok);
    p->tok = p->tok->next;
  } else {
    ty = cst_parse_type_name_core(ctx, p);
    if (!p->ok)
      return NULL;
  }

  if (!cst_at(p, ":"))
    return ty;

  LibcpCstNode* assoc = new_cst_node(ctx, CST_GENERIC_ASSOC);
  cst_add_child(ctx, assoc, ty);
  cst_add_leaf(ctx, assoc, p->tok);  // ':'
  p->tok = p->tok->next;
  cst_add_child(ctx, assoc, cst_parse_assign(ctx, p));
  return p->ok ? assoc : NULL;
}

// arg-list = "(" (arg ("," arg)*)? ")"
// 实参列表: 可以为空, 实参之间用逗号分隔.
static LibcpCstNode* cst_parse_arg_list(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* args = new_cst_node(ctx, CST_ARG_LIST);
  cst_add_leaf(ctx, args, p->tok);  // 左括号
  p->tok = p->tok->next;

  if (!cst_at(p, ")")) {
    for (;;) {
      LibcpCstNode* arg = cst_parse_arg(ctx, p);
      if (!p->ok)
        return NULL;
      cst_add_child(ctx, args, arg);

      if (!cst_at(p, ","))
        break;
      cst_add_leaf(ctx, args, p->tok);
      p->tok = p->tok->next;
    }
  }

  if (!cst_at(p, ")")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, args, p->tok);
  p->tok = p->tok->next;
  return args;
}

// designator = "[" conditional ("..." conditional)? "]" | "." ident
// 指示符: 下标形式 (支持 GNU 的 "..." 范围) 或 "." 加成员名.
static LibcpCstNode* cst_parse_designator(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* desig = new_cst_node(ctx, CST_DESIGNATOR);

  if (cst_at(p, "[")) {
    cst_add_leaf(ctx, desig, p->tok);  // 左方括号
    p->tok = p->tok->next;

    LibcpCstNode* begin = cst_parse_conditional(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, desig, begin);

    if (cst_at(p, "...")) {
      cst_add_leaf(ctx, desig, p->tok);
      p->tok            = p->tok->next;
      LibcpCstNode* end = cst_parse_conditional(ctx, p);
      if (!p->ok)
        return NULL;
      cst_add_child(ctx, desig, end);
    }

    if (!cst_at(p, "]")) {
      cst_fail(p);
      return NULL;
    }
    cst_add_leaf(ctx, desig, p->tok);
    p->tok = p->tok->next;
    return desig;
  }

  if (cst_at(p, ".")) {
    LibcpToken* name = p->tok->next;
    if (!name || name->kind != TK_IDENT) {
      cst_fail(p);
      return NULL;
    }
    cst_add_leaf(ctx, desig, p->tok);
    cst_add_leaf(ctx, desig, name);
    p->tok = name->next;
    return desig;
  }

  cst_fail(p);
  return NULL;
}

// initializer = brace-list | string+ | assign
// 初始化器: 花括号列表 / 连续字符串字面量 / 赋值表达式.
static LibcpCstNode* cst_parse_initializer(LibcpContext* ctx, LibcpCstParser* p) {
  if (cst_at(p, "{"))
    return cst_parse_init_list(ctx, p);

  if (p->tok && p->tok->kind == TK_STR) {
    LibcpCstNode* node = new_cst_node(ctx, CST_PRIMARY);
    while (p->tok && p->tok->kind == TK_STR) {
      cst_add_leaf(ctx, node, p->tok);
      p->tok = p->tok->next;
    }
    return node;
  }

  return cst_parse_assign(ctx, p);
}

// init-list = "{" (init-item ("," init-item)* ","?)? "}"
// init-item = designator* "="? initializer
//
// 保留书写形态: 指示符是显式节点, 带花括号的子列表是嵌套的 CST_INIT_LIST,
// 这里不对目标类型做任何决定 (花括号省略属于后续消费者).
static LibcpCstNode* cst_parse_init_list(LibcpContext* ctx, LibcpCstParser* p) {
  if (++p->cst->depth > MAX_CST_DEPTH) {
    p->cst->depth--;
    cst_fail(p);
    return NULL;
  }
  LibcpCstNode* node = cst_parse_init_list1(ctx, p);
  p->cst->depth--;
  return node;
}

static LibcpCstNode* cst_parse_init_list1(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* list = new_cst_node(ctx, CST_INIT_LIST);
  cst_add_leaf(ctx, list, p->tok);  // 左花括号
  p->tok = p->tok->next;

  if (cst_at(p, "}")) {
    cst_add_leaf(ctx, list, p->tok);
    p->tok = p->tok->next;
    return list;
  }

  for (;;) {
    LibcpCstNode* item = new_cst_node(ctx, CST_INITIALIZER);

    while (cst_at(p, "[") || cst_at(p, ".")) {
      LibcpCstNode* desig = cst_parse_designator(ctx, p);
      if (!p->ok)
        return NULL;
      cst_add_child(ctx, item, desig);
    }

    if (cst_at(p, "=")) {
      cst_add_leaf(ctx, item, p->tok);
      p->tok = p->tok->next;
    }

    LibcpCstNode* value = cst_parse_initializer(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, item, value);
    cst_add_child(ctx, list, item);

    if (!cst_at(p, ","))
      break;
    cst_add_leaf(ctx, list, p->tok);
    p->tok = p->tok->next;
    if (cst_at(p, "}"))
      break;
  }

  if (!cst_at(p, "}")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, list, p->tok);
  p->tok = p->tok->next;
  return list;
}

// postfix-tail = ("(" arg-list | "[" expr "]" | "." ident | "->" ident
//                | "++" | "--")*
// 后缀尾部: 调用 / 下标 / 成员访问 (. 与 ->) / 后置 ++ 与 --, 可连续出现.
static LibcpCstNode*
    cst_parse_postfix_tail(LibcpContext* ctx, LibcpCstParser* p, LibcpCstNode* node) {
  for (;;) {
    if (!p->ok)
      return NULL;

    if (cst_at(p, "(")) {
      LibcpCstNode* args = cst_parse_arg_list(ctx, p);
      if (!p->ok)
        return NULL;
      LibcpCstNode* call = new_cst_node(ctx, CST_POSTFIX);
      cst_add_child(ctx, call, node);
      cst_add_child(ctx, call, args);
      node = call;
      continue;
    }

    if (cst_at(p, "[")) {
      LibcpToken* open  = p->tok;
      p->tok            = open->next;
      LibcpCstNode* idx = cst_parse_comma(ctx, p);
      if (!p->ok || !cst_at(p, "]")) {
        cst_fail(p);
        return NULL;
      }

      LibcpCstNode* sub = new_cst_node(ctx, CST_POSTFIX);
      cst_add_child(ctx, sub, node);
      cst_add_leaf(ctx, sub, open);
      cst_add_child(ctx, sub, idx);
      cst_add_leaf(ctx, sub, p->tok);
      p->tok = p->tok->next;
      node   = sub;
      continue;
    }

    if (cst_at(p, ".") || cst_at(p, "->")) {
      LibcpToken* op   = p->tok;
      LibcpToken* name = op->next;
      if (!name || name->kind != TK_IDENT) {
        cst_fail(p);
        return NULL;
      }

      LibcpCstNode* member = new_cst_node(ctx, CST_POSTFIX);
      cst_add_child(ctx, member, node);
      cst_add_leaf(ctx, member, op);
      cst_add_leaf(ctx, member, name);
      p->tok = name->next;
      node   = member;
      continue;
    }

    if (cst_at(p, "++") || cst_at(p, "--")) {
      LibcpCstNode* inc = new_cst_node(ctx, CST_POSTFIX);
      cst_add_child(ctx, inc, node);
      cst_add_leaf(ctx, inc, p->tok);
      p->tok = p->tok->next;
      node   = inc;
      continue;
    }

    return node;
  }
}

// postfix = primary postfix-tail
// 后缀表达式: 主表达式后接零个或多个后缀尾部.
static LibcpCstNode* cst_parse_postfix(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = cst_parse_primary(ctx, p);
  if (!p->ok)
    return NULL;
  return cst_parse_postfix_tail(ctx, p, node);
}

// unary = ("+" | "-" | "!" | "~" | "*" | "&" | "&&" | "++" | "--") unary
//       | ("sizeof" | "_Alignof" | "typeof") (type-name | unary)
//       | "(" type-name ")" unary                    // cast
//       | "(" type-name ")" "{" init-list "}"        // 复合字面量
//       | postfix
//
// 一元表达式: 前缀运算符 / sizeof, _Alignof, typeof / cast / 复合字面量 /
// 后缀表达式.
static LibcpCstNode* cst_parse_unary(LibcpContext* ctx, LibcpCstParser* p) {
  if (++p->cst->depth > MAX_CST_DEPTH) {
    p->cst->depth--;
    cst_fail(p);
    return NULL;
  }
  LibcpCstNode* node = cst_parse_unary1(ctx, p);
  p->cst->depth--;
  return node;
}

static LibcpCstNode* cst_parse_unary1(LibcpContext* ctx, LibcpCstParser* p) {
  if (!p->ok)
    return NULL;

  if (cst_at(p, "sizeof") || cst_at(p, "_Alignof") || cst_at(p, "alignof") ||
      cst_at(p, "__alignof__") || cst_at(p, "__alignof") || cst_at(p, "typeof") ||
      cst_at(p, "__typeof__") || cst_at(p, "__typeof")) {
    LibcpToken* op = p->tok;
    p->tok         = op->next;

    LibcpCstNode* operand =
        (cst_at(p, "(") && p->tok->next && cst_is_type_start(p->tok->next, p->cst)) ?
            cst_parse_type_name(ctx, p) :
            cst_parse_unary(ctx, p);
    if (!p->ok)
      return NULL;

    LibcpCstNode* node = new_cst_node(ctx, CST_UNARY);
    cst_add_leaf(ctx, node, op);
    cst_add_child(ctx, node, operand);
    return node;
  }

  if (cst_at(p, "+") || cst_at(p, "-") || cst_at(p, "!") || cst_at(p, "~") || cst_at(p, "*") ||
      cst_at(p, "&") || cst_at(p, "&&") || cst_at(p, "++") || cst_at(p, "--")) {
    LibcpToken* op = p->tok;
    p->tok         = op->next;

    LibcpCstNode* operand = cst_parse_unary(ctx, p);
    if (!p->ok)
      return NULL;

    LibcpCstNode* node = new_cst_node(ctx, CST_UNARY);
    cst_add_leaf(ctx, node, op);
    cst_add_child(ctx, node, operand);
    return node;
  }

  if (cst_at(p, "(") && p->tok->next && cst_is_type_start(p->tok->next, p->cst)) {
    LibcpCstNode* type = cst_parse_type_name(ctx, p);
    if (!p->ok)
      return NULL;

    if (cst_at(p, "{")) {
      LibcpCstNode* init = cst_parse_init_list(ctx, p);
      if (!p->ok)
        return NULL;
      LibcpCstNode* lit = new_cst_node(ctx, CST_COMPOUND_LITERAL);
      cst_add_child(ctx, lit, type);
      cst_add_child(ctx, lit, init);
      return cst_parse_postfix_tail(ctx, p, lit);
    }

    LibcpCstNode* operand = cst_parse_unary(ctx, p);
    if (!p->ok)
      return NULL;

    LibcpCstNode* cast = new_cst_node(ctx, CST_CAST);
    cst_add_child(ctx, cast, type);
    cst_add_child(ctx, cast, operand);
    return cast;
  }

  return cst_parse_postfix(ctx, p);
}

// 二元层级 <name> = <operand-level> (op <operand-level>)*, 左结合. 每一层都
// 把运算符保持为显式叶子.

static LibcpCstNode* cst_parse_mul(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = cst_parse_unary(ctx, p);
  while (p->ok && (cst_at(p, "*") || cst_at(p, "/") || cst_at(p, "%"))) {
    LibcpCstNode* bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    LibcpCstNode* rhs = cst_parse_unary(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

static LibcpCstNode* cst_parse_add(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = cst_parse_mul(ctx, p);
  while (p->ok && (cst_at(p, "+") || cst_at(p, "-"))) {
    LibcpCstNode* bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    LibcpCstNode* rhs = cst_parse_mul(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

static LibcpCstNode* cst_parse_shift(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = cst_parse_add(ctx, p);
  while (p->ok && (cst_at(p, "<<") || cst_at(p, ">>"))) {
    LibcpCstNode* bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    LibcpCstNode* rhs = cst_parse_add(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

static LibcpCstNode* cst_parse_relational(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = cst_parse_shift(ctx, p);
  while (p->ok && (cst_at(p, "<") || cst_at(p, "<=") || cst_at(p, ">") || cst_at(p, ">="))) {
    LibcpCstNode* bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    LibcpCstNode* rhs = cst_parse_shift(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

static LibcpCstNode* cst_parse_equality(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = cst_parse_relational(ctx, p);
  while (p->ok && (cst_at(p, "==") || cst_at(p, "!="))) {
    LibcpCstNode* bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    LibcpCstNode* rhs = cst_parse_relational(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

static LibcpCstNode* cst_parse_bitand(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = cst_parse_equality(ctx, p);
  while (p->ok && cst_at(p, "&")) {
    LibcpCstNode* bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    LibcpCstNode* rhs = cst_parse_equality(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

static LibcpCstNode* cst_parse_bitxor(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = cst_parse_bitand(ctx, p);
  while (p->ok && cst_at(p, "^")) {
    LibcpCstNode* bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    LibcpCstNode* rhs = cst_parse_bitand(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

static LibcpCstNode* cst_parse_bitor(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = cst_parse_bitxor(ctx, p);
  while (p->ok && cst_at(p, "|")) {
    LibcpCstNode* bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    LibcpCstNode* rhs = cst_parse_bitxor(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

static LibcpCstNode* cst_parse_logand(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = cst_parse_bitor(ctx, p);
  while (p->ok && cst_at(p, "&&")) {
    LibcpCstNode* bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    LibcpCstNode* rhs = cst_parse_bitor(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

static LibcpCstNode* cst_parse_logor(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = cst_parse_logand(ctx, p);
  while (p->ok && cst_at(p, "||")) {
    LibcpCstNode* bin = new_cst_node(ctx, CST_BINARY);
    cst_add_child(ctx, bin, node);
    cst_add_leaf(ctx, bin, p->tok);
    p->tok = p->tok->next;

    LibcpCstNode* rhs = cst_parse_logand(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, bin, rhs);
    node = bin;
  }
  return node;
}

// conditional = logor ("?" expr ":" conditional | "?" ":" conditional)?
// 条件表达式: 逻辑或表达式, 后面可接 ? : 三元形式.
static LibcpCstNode* cst_parse_conditional(LibcpContext* ctx, LibcpCstParser* p) {
  if (++p->cst->depth > MAX_CST_DEPTH) {
    p->cst->depth--;
    cst_fail(p);
    return NULL;
  }
  LibcpCstNode* node = cst_parse_conditional1(ctx, p);
  p->cst->depth--;
  return node;
}

static LibcpCstNode* cst_parse_conditional1(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* cond = cst_parse_logor(ctx, p);
  if (!p->ok || !cst_at(p, "?"))
    return cond;

  LibcpCstNode* node = new_cst_node(ctx, CST_COND);
  cst_add_child(ctx, node, cond);
  cst_add_leaf(ctx, node, p->tok);  // 问号
  p->tok = p->tok->next;

  // [GNU] `a ?: b` 没有中间操作数.
  if (cst_at(p, ":")) {
    cst_add_leaf(ctx, node, p->tok);
    p->tok            = p->tok->next;
    LibcpCstNode* els = cst_parse_conditional(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, node, els);
    return node;
  }

  LibcpCstNode* then = cst_parse_comma(ctx, p);
  if (!p->ok || !cst_at(p, ":")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_child(ctx, node, then);
  cst_add_leaf(ctx, node, p->tok);  // 冒号 (三元表达式的分隔符)
  p->tok = p->tok->next;

  LibcpCstNode* els = cst_parse_conditional(ctx, p);
  if (!p->ok)
    return NULL;
  cst_add_child(ctx, node, els);
  return node;
}

// 当前 token 是否是赋值运算符之一 (= += -= *= /= %= &= |= ^= <<= >>=).
static bool cst_at_assign_op(LibcpCstParser* p) {
  char* ops[] = {
      "=",
      "+=",
      "-=",
      "*=",
      "/=",
      "%=",
      "&=",
      "|=",
      "^=",
      "<<=",
      ">>=",
      NULL,
  };
  return p->tok && p->tok->kind != TK_EOF && cst_in_list(p->tok, ops);
}

// assign = conditional (assign-op assign)?
// 赋值表达式: 条件表达式, 后面可接一个赋值运算符和右结合的赋值表达式.
static LibcpCstNode* cst_parse_assign(LibcpContext* ctx, LibcpCstParser* p) {
  if (++p->cst->depth > MAX_CST_DEPTH) {
    p->cst->depth--;
    cst_fail(p);
    return NULL;
  }
  LibcpCstNode* node = cst_parse_assign1(ctx, p);
  p->cst->depth--;
  return node;
}

static LibcpCstNode* cst_parse_assign1(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* lhs = cst_parse_conditional(ctx, p);
  if (!p->ok || !cst_at_assign_op(p))
    return lhs;

  LibcpCstNode* node = new_cst_node(ctx, CST_ASSIGN);
  cst_add_child(ctx, node, lhs);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  LibcpCstNode* rhs = cst_parse_assign(ctx, p);
  if (!p->ok)
    return NULL;
  cst_add_child(ctx, node, rhs);
  return node;
}

// comma = assign ("," assign)*
// 逗号表达式: 赋值表达式, 后接零个或多个 "," assign, 左结合.
static LibcpCstNode* cst_parse_comma(LibcpContext* ctx, LibcpCstParser* p) {
  if (++p->cst->depth > MAX_CST_DEPTH) {
    p->cst->depth--;
    cst_fail(p);
    return NULL;
  }
  LibcpCstNode* node = cst_parse_comma1(ctx, p);
  p->cst->depth--;
  return node;
}

static LibcpCstNode* cst_parse_comma1(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = cst_parse_assign(ctx, p);
  while (p->ok && cst_at(p, ",")) {
    LibcpCstNode* comma = new_cst_node(ctx, CST_COMMA);
    cst_add_child(ctx, comma, node);
    cst_add_leaf(ctx, comma, p->tok);
    p->tok = p->tok->next;

    LibcpCstNode* rhs = cst_parse_assign(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, comma, rhs);
    node = comma;
  }
  return node;
}

//
// 声明层 (C2.2)
//

static void cst_parse_compound(LibcpContext* ctx, LibcpCstParser* p, LibcpCstNode* block);
static LibcpCstNode* cst_parse_param_list(LibcpContext* ctx, LibcpCstParser* p);

// 从 `start` 起按 raw 模式消费一个配平的分组; 进入前先把解析器重置到
// `start`. 用于结构化尝试不适合, 但 token 仍必须留在树里的地方 (K&R
// 参数列表, 数组维度, 属性).
static LibcpCstNode*
    cst_parse_raw_group_at(LibcpContext* ctx, LibcpCstParser* p, LibcpToken* start) {
  p->tok = start;
  p->ok  = true;

  LibcpToken* rest    = NULL;
  LibcpCstNode* group = cst_parse_group(ctx, p->cst, &rest, start);
  if (group->kind == CST_UNPARSED) {
    cst_fail(p);
    return NULL;
  }
  p->tok = rest;
  return group;
}

// decl-specs = (storage-class | qualifier | type-specifier | ...)+
//
// 只看形态: `struct` / `union` / `enum` 的主体, typeof 操作数,
// _Atomic / _Alignas 实参与属性都保持为 raw 分组. 用户定义的类型名只有在
// 尚未见过其他说明符时才算说明符, 因此 `T T;` 把第二个 T 留给声明符.
static LibcpCstNode* cst_parse_decl_specs(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* specs = new_cst_node(ctx, CST_DECL_SPECS);
  bool has_type       = false;

  for (;;) {
    LibcpToken* tok = p->tok;
    if (!p->ok || !tok || tok->kind == TK_EOF)
      break;

    char* plain[] = {
        "typedef",
        "static",
        "extern",
        "auto",
        "register",
        "inline",
        "_Thread_local",
        "__thread",
        "_Noreturn",
        "const",
        "volatile",
        "restrict",
        "__restrict",
        "__restrict__",
        NULL,
    };
    if (cst_in_list(tok, plain)) {
      cst_add_leaf(ctx, specs, tok);
      p->tok = tok->next;
      continue;
    }

    char* builtin[] = {
        "void",
        "_Bool",
        "char",
        "short",
        "int",
        "long",
        "float",
        "double",
        "signed",
        "unsigned",
        "_Complex",
        "_Imaginary",
        "__signed__",
        "__const__",
        "__volatile__",
        NULL,
    };
    if (cst_in_list(tok, builtin)) {
      cst_add_leaf(ctx, specs, tok);
      p->tok   = tok->next;
      has_type = true;
      continue;
    }

    if (libcp_equal(tok, "struct") || libcp_equal(tok, "union")) {
      if (has_type)
        break;
      cst_add_leaf(ctx, specs, tok);
      p->tok = tok->next;
      if (p->tok && p->tok->kind == TK_IDENT && !cst_is_keyword(p->tok)) {
        cst_add_leaf(ctx, specs, p->tok);
        p->tok = p->tok->next;
      }
      if (cst_at(p, "{")) {
        LibcpToken* rest   = NULL;
        LibcpCstNode* body = cst_parse_group(ctx, p->cst, &rest, p->tok);
        if (body->kind == CST_UNPARSED) {
          cst_fail(p);
          return NULL;
        }
        p->tok = rest;
        cst_add_child(ctx, specs, body);
      }
      has_type = true;
      continue;
    }

    if (libcp_equal(tok, "enum")) {
      if (has_type)
        break;
      cst_add_leaf(ctx, specs, tok);
      p->tok = tok->next;
      if (p->tok && p->tok->kind == TK_IDENT && !cst_is_keyword(p->tok)) {
        cst_add_leaf(ctx, specs, p->tok);
        p->tok = p->tok->next;
      }
      if (cst_at(p, "{")) {
        LibcpToken* rest   = NULL;
        LibcpCstNode* body = cst_parse_group(ctx, p->cst, &rest, p->tok);
        if (body->kind == CST_UNPARSED) {
          cst_fail(p);
          return NULL;
        }
        p->tok = rest;
        cst_add_child(ctx, specs, body);
      }
      has_type = true;
      continue;
    }

    if (libcp_equal(tok, "typeof") || libcp_equal(tok, "__typeof__") ||
        libcp_equal(tok, "__typeof")) {
      if (has_type)
        break;
      cst_add_leaf(ctx, specs, tok);
      p->tok = tok->next;
      if (cst_at(p, "(")) {
        LibcpCstNode* operand = cst_parse_raw_group(ctx, p);
        if (!p->ok)
          return NULL;
        cst_add_child(ctx, specs, operand);
      }
      has_type = true;
      continue;
    }

    if (libcp_equal(tok, "_Atomic")) {
      cst_add_leaf(ctx, specs, tok);
      p->tok = tok->next;
      if (cst_at(p, "(")) {
        LibcpCstNode* operand = cst_parse_raw_group(ctx, p);
        if (!p->ok)
          return NULL;
        cst_add_child(ctx, specs, operand);
        has_type = true;
      }
      continue;
    }

    if (libcp_equal(tok, "_Alignas")) {
      cst_add_leaf(ctx, specs, tok);
      p->tok = tok->next;
      if (cst_at(p, "(")) {
        LibcpCstNode* operand = cst_parse_raw_group(ctx, p);
        if (!p->ok)
          return NULL;
        cst_add_child(ctx, specs, operand);
      }
      continue;
    }

    if (libcp_equal(tok, "__attribute__") || libcp_equal(tok, "__extension__")) {
      bool attr = libcp_equal(tok, "__attribute__");
      cst_add_leaf(ctx, specs, tok);
      p->tok = tok->next;
      if (attr && cst_at(p, "(")) {
        LibcpCstNode* group = cst_parse_raw_group(ctx, p);
        if (!p->ok)
          return NULL;
        cst_add_child(ctx, specs, group);
      }
      continue;
    }

    if (cst_is_typedef_name(p->cst, tok)) {
      if (has_type)
        break;
      cst_add_leaf(ctx, specs, tok);
      p->tok   = tok->next;
      has_type = true;
      continue;
    }

    break;
  }

  if (specs->child_count == 0) {
    cst_fail(p);
    return NULL;
  }
  return specs;
}

// 消费声明符之后的 `__attribute__((...))` 对, 把它们保留在声明符节点上.
static bool cst_parse_attributes(LibcpContext* ctx, LibcpCstParser* p, LibcpCstNode* node) {
  while (cst_at(p, "__attribute__")) {
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;
    if (cst_at(p, "(")) {
      LibcpCstNode* group = cst_parse_raw_group(ctx, p);
      if (!p->ok)
        return false;
      cst_add_child(ctx, node, group);
    }
  }
  return true;
}

// 数组维度: "[" ("static" | "restrict" | "const" | "volatile")*
// ("*" | expr)? "]". 表达式能解析时按表达式解析; 否则方括号保留其原始 token.
static LibcpCstNode* cst_parse_dimension(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpToken* start     = p->tok;
  LibcpCstNode* bracket = new_cst_node(ctx, CST_GROUP_BRACKET);
  cst_add_leaf(ctx, bracket, p->tok);  // 左方括号
  p->tok = p->tok->next;

  while (cst_at(p, "static") || cst_at(p, "restrict") || cst_at(p, "const") ||
         cst_at(p, "volatile")) {
    cst_add_leaf(ctx, bracket, p->tok);
    p->tok = p->tok->next;
  }

  if (cst_at(p, "]")) {
    cst_add_leaf(ctx, bracket, p->tok);
    p->tok = p->tok->next;
    return bracket;
  }

  if (cst_at(p, "*")) {
    cst_add_leaf(ctx, bracket, p->tok);
    p->tok = p->tok->next;
  } else {
    LibcpCstNode* expr = cst_parse_assign(ctx, p);
    if (!p->ok)
      return cst_parse_raw_group_at(ctx, p, start);
    cst_add_child(ctx, bracket, expr);
  }

  if (!cst_at(p, "]"))
    return cst_parse_raw_group_at(ctx, p, start);
  cst_add_leaf(ctx, bracket, p->tok);
  p->tok = p->tok->next;
  return bracket;
}

// declarator = "*"* ("(" declarator ")" | ident)? ("(" params ")" | "[" dim "]")*
//
// 保留书写时的分组: 指针的 '*', GROUP_PAREN 中的内层声明符, 名字, 以及按
// 源码顺序排列的后缀. 不构建类型, 也不绑定名字.
static LibcpCstNode*
    cst_parse_declarator(LibcpContext* ctx, LibcpCstParser* p, bool allow_abstract) {
  if (++p->cst->depth > MAX_CST_DEPTH) {
    p->cst->depth--;
    cst_fail(p);
    return NULL;
  }
  LibcpCstNode* node = cst_parse_declarator1(ctx, p, allow_abstract);
  p->cst->depth--;
  return node;
}

static LibcpCstNode*
    cst_parse_declarator1(LibcpContext* ctx, LibcpCstParser* p, bool allow_abstract) {
  LibcpCstNode* decl = new_cst_node(ctx, CST_DECLARATOR);

  while (cst_at(p, "*")) {
    cst_add_leaf(ctx, decl, p->tok);
    p->tok = p->tok->next;
    while (cst_at(p, "const") || cst_at(p, "volatile") || cst_at(p, "restrict") ||
           cst_at(p, "__restrict") || cst_at(p, "__restrict__")) {
      cst_add_leaf(ctx, decl, p->tok);
      p->tok = p->tok->next;
    }
  }

  if (cst_at(p, "(")) {
    LibcpToken* open = p->tok;
    p->tok           = open->next;

    LibcpCstNode* inner = cst_parse_declarator(ctx, p, allow_abstract);
    if (!p->ok || !cst_at(p, ")")) {
      cst_fail(p);
      return NULL;
    }

    LibcpCstNode* group = new_cst_node(ctx, CST_GROUP_PAREN);
    cst_add_leaf(ctx, group, open);
    cst_add_child(ctx, group, inner);
    cst_add_leaf(ctx, group, p->tok);
    p->tok = p->tok->next;
    cst_add_child(ctx, decl, group);
  } else if (p->tok && p->tok->kind == TK_IDENT && !cst_is_keyword(p->tok)) {
    cst_add_leaf(ctx, decl, p->tok);
    p->tok = p->tok->next;
  } else if (!allow_abstract) {
    cst_fail(p);
    return NULL;
  }

  for (;;) {
    if (cst_at(p, "(")) {
      LibcpCstNode* params = cst_parse_param_list(ctx, p);
      if (!p->ok)
        return NULL;
      cst_add_child(ctx, decl, params);
      continue;
    }
    if (cst_at(p, "[")) {
      LibcpCstNode* dim = cst_parse_dimension(ctx, p);
      if (!p->ok)
        return NULL;
      cst_add_child(ctx, decl, dim);
      continue;
    }
    break;
  }

  if (!cst_parse_attributes(ctx, p, decl))
    return NULL;
  return decl;
}

// parameter = decl-specs declarator?
// 参数: 声明说明符加可选的声明符.
static LibcpCstNode* cst_parse_param(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* specs = cst_parse_decl_specs(ctx, p);
  if (!p->ok)
    return NULL;

  LibcpCstNode* decl = cst_parse_declarator(ctx, p, true);
  if (!p->ok)
    return NULL;

  LibcpCstNode* param = new_cst_node(ctx, CST_PARAM);
  cst_add_child(ctx, param, specs);
  if (decl->child_count)
    cst_add_child(ctx, param, decl);
  return param;
}

// param-list = "(" (param ("," param)* | "..." | raw)? ")"
// 参数列表: 带括号的参数序列, 支持省略号或无法结构化时的 raw 形式.
// 声明文法解释不了的参数列表把原始 token 留在 GROUP_PAREN 里
// (常见情形是 K&R 的标识符列表).
static LibcpCstNode* cst_parse_param_list(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpToken* start  = p->tok;
  LibcpCstNode* list = new_cst_node(ctx, CST_PARAM_LIST);
  cst_add_leaf(ctx, list, p->tok);  // 左括号
  p->tok = p->tok->next;

  if (cst_at(p, ")")) {
    cst_add_leaf(ctx, list, p->tok);
    p->tok = p->tok->next;
    return list;
  }

  if (cst_at(p, "...")) {
    cst_add_leaf(ctx, list, p->tok);
    p->tok = p->tok->next;
  } else {
    for (;;) {
      LibcpCstNode* param = cst_parse_param(ctx, p);
      if (!param)
        return cst_parse_raw_group_at(ctx, p, start);
      cst_add_child(ctx, list, param);

      if (!cst_at(p, ","))
        break;
      cst_add_leaf(ctx, list, p->tok);
      p->tok = p->tok->next;
    }
  }

  if (!cst_at(p, ")"))
    return cst_parse_raw_group_at(ctx, p, start);
  cst_add_leaf(ctx, list, p->tok);
  p->tok = p->tok->next;
  return list;
}

// type-name = "(" decl-specs abstract-declarator? ")"
// 类型名: 括号内的声明说明符加可选的抽象声明符.
static LibcpCstNode* cst_parse_type_name(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpToken* open   = p->tok;
  LibcpCstNode* node = new_cst_node(ctx, CST_TYPE_NAME);
  cst_add_leaf(ctx, node, open);  // 左括号
  p->tok = open->next;

  LibcpCstNode* specs = cst_parse_decl_specs(ctx, p);
  if (!p->ok)
    return NULL;
  cst_add_child(ctx, node, specs);

  LibcpCstNode* decl = cst_parse_declarator(ctx, p, true);
  if (!p->ok)
    return NULL;
  if (decl->child_count)
    cst_add_child(ctx, node, decl);

  if (!cst_at(p, ")")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;
  return node;
}

// 类型名去掉括号的部分: 声明说明符加可选的抽象声明符. 子节点与
// cst_parse_type_name 相同, 只是没有外层括号; 用于裸类型名出现的地方
// (调用实参, PLAN-l5 W1).
static LibcpCstNode* cst_parse_type_name_core(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = new_cst_node(ctx, CST_TYPE_NAME);

  LibcpCstNode* specs = cst_parse_decl_specs(ctx, p);
  if (!p->ok)
    return NULL;
  cst_add_child(ctx, node, specs);

  LibcpCstNode* decl = cst_parse_declarator(ctx, p, true);
  if (!p->ok)
    return NULL;
  if (decl->child_count)
    cst_add_child(ctx, node, decl);

  return node;
}

// declaration = decl-specs init-declarator ("," init-declarator)* ";"
//             | decl-specs ";"
// function-definition = decl-specs declarator compound-stmt
//
// 声明: 声明说明符加一个或多个 init-declarator, 以 ";" 结尾; 或只有声明
// 说明符加 ";". 函数定义: 声明说明符 + 声明符 + 复合语句.
static LibcpCstNode* cst_parse_decl(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* specs = cst_parse_decl_specs(ctx, p);
  if (!p->ok)
    return NULL;

  // 没有声明符的标签或说明符: `struct S {...};`, `int;`.
  if (cst_at(p, ";")) {
    LibcpCstNode* decl = new_cst_node(ctx, CST_DECL);
    cst_add_child(ctx, decl, specs);
    cst_add_leaf(ctx, decl, p->tok);
    p->tok = p->tok->next;
    return decl;
  }

  LibcpCstNode* declr = cst_parse_declarator(ctx, p, false);
  if (!p->ok)
    return NULL;

  // `declarator {` 是函数定义. 也允许 GNU 嵌套函数; 这里不检查作用域.
  if (cst_at(p, "{")) {
    LibcpCstNode* body = new_cst_node(ctx, CST_BLOCK);
    cst_parse_compound(ctx, p, body);
    if (!p->ok)
      return NULL;

    LibcpCstNode* fn = new_cst_node(ctx, CST_FUNC_DEF);
    cst_add_child(ctx, fn, specs);
    cst_add_child(ctx, fn, declr);
    cst_add_child(ctx, fn, body);
    return fn;
  }

  LibcpCstNode* decl = new_cst_node(ctx, CST_DECL);
  cst_add_child(ctx, decl, specs);

  for (;;) {
    if (cst_at(p, "=")) {
      LibcpCstNode* init_decl = new_cst_node(ctx, CST_INIT_DECLARATOR);
      cst_add_child(ctx, init_decl, declr);
      cst_add_leaf(ctx, init_decl, p->tok);  // 等号
      p->tok = p->tok->next;

      LibcpCstNode* init = cst_parse_initializer(ctx, p);
      if (!p->ok)
        return NULL;
      cst_add_child(ctx, init_decl, init);
      cst_add_child(ctx, decl, init_decl);
    } else {
      cst_add_child(ctx, decl, declr);
    }

    if (!cst_at(p, ","))
      break;
    cst_add_leaf(ctx, decl, p->tok);
    p->tok = p->tok->next;

    declr = cst_parse_declarator(ctx, p, false);
    if (!p->ok)
      return NULL;
  }

  if (!cst_at(p, ";")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, decl, p->tok);
  p->tok = p->tok->next;
  return decl;
}

//
// 语句层
//

static bool cst_is_directive(LibcpToken* tok);
static bool cst_is_pp_terminator(LibcpToken* tok);
static LibcpCstNode* cst_parse_directive_line(LibcpContext* ctx, LibcpCstParser* p);
static LibcpCstNode* cst_parse_pp_if(LibcpContext* ctx, LibcpCstParser* p);
static LibcpCstNode* cst_parse_item(LibcpContext* ctx, LibcpCstParser* p);
static LibcpCstNode* cst_parse_item1(LibcpContext* ctx, LibcpCstParser* p);

// 按回退形态解析一条语句: 直到并包括本层 ';' 的 token, 每个分隔符一个 raw
// 分组, 遇到属于外层的闭括号就停止. 被闭括号 (或 EOF) 截断的区间保留已有
// token; 骨架不猜测它本该在哪里结束. 节点是 CST_UNPARSED: 表达式文法没有
// 解释该区间 (在 C2.2 之前, 声明也是这样的区间), 子节点正是原始片段,
// 而不是猜测.
static LibcpCstNode* cst_parse_fallback_statement(LibcpContext* ctx,
                                                  LibcpCstContext* cst,
                                                  LibcpToken** rest,
                                                  LibcpToken* tok) {
  LibcpCstNode* stmt = new_cst_node(ctx, CST_UNPARSED);

  LibcpToken* p = tok;
  while (p && p->kind != TK_EOF) {
    // 行首的指令开启下一个 item; 畸形区间不能吞掉它. 区间自身的第一个
    // token 从不是指令 (那些由 item 解析器处理).
    if (p != tok && cst_is_directive(p))
      break;
    if (libcp_equal(p, ";")) {
      cst_add_leaf(ctx, stmt, p);
      p = p->next;
      break;
    }
    if (cst_is_close(p))
      break;
    if (cst_is_open(p)) {
      cst_add_child(ctx, stmt, cst_parse_group(ctx, cst, &p, p));
      continue;
    }
    cst_add_leaf(ctx, stmt, p);
    p = p->next;
  }
  *rest = p;
  return stmt;
}

// 整段是"表达式 + ';'"的语句变成 CST_EXPR_STMT. 任何失败时调用者改用原始
// 骨架; 失败尝试留下的节点不可达, 这是允许的.
static LibcpCstNode*
    cst_try_expr_stmt(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpCstContext* cst) {
  LibcpCstParser p = {tok, true, cst};

  LibcpCstNode* expr = cst_parse_comma(ctx, &p);
  if (!p.ok || !libcp_equal(p.tok, ";"))
    return NULL;

  LibcpCstNode* stmt = new_cst_node(ctx, CST_EXPR_STMT);
  cst_add_child(ctx, stmt, expr);
  cst_add_leaf(ctx, stmt, p.tok);
  *rest = p.tok->next;
  return stmt;
}

static LibcpCstNode* cst_parse_statement(LibcpContext* ctx, LibcpCstParser* p);
static LibcpCstNode* cst_parse_statement1(LibcpContext* ctx, LibcpCstParser* p);

// statement = "return" expr? ";"
//           | "if" "(" expr ")" statement ("else" statement)?
//           | "switch" "(" expr ")" statement
//           | "while" "(" expr ")" statement
//           | "do" statement "while" "(" expr ")" ";"
//           | "for" "(" (decl | expr)? ";" expr? ";" expr? ")" statement
//           | "case" conditional ("..." conditional)? ":" statement
//           | "default" ":" statement
//           | "goto" (ident | "*" expr) ";"
//           | "break" ";" | "continue" ";"
//           | "asm" qualifiers* "(" raw ")" ";"
//           | ident ":" statement
//           | compound-stmt | declaration | expr-stmt
//
// 语句: return / if / switch / while / do-while / for / case / default /
// goto / break / continue / asm / 标签语句 / 复合语句 / 声明 / 表达式语句.
// 关键字, 括号与 ';' 都保持为显式叶子.
static LibcpCstNode*
    cst_parse_simple_stmt(LibcpContext* ctx, LibcpCstParser* p, LibcpCstKind kind) {
  LibcpCstNode* node = new_cst_node(ctx, kind);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, ";")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;
  return node;
}

static LibcpCstNode* cst_parse_return(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = new_cst_node(ctx, CST_RETURN);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, ";")) {
    LibcpCstNode* expr = cst_parse_comma(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, node, expr);
  }

  if (!cst_at(p, ";")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;
  return node;
}

static LibcpCstNode* cst_parse_if(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = new_cst_node(ctx, CST_IF);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, "(")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  LibcpCstNode* cond = cst_parse_comma(ctx, p);
  if (!p->ok || !cst_at(p, ")")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_child(ctx, node, cond);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  LibcpCstNode* then = cst_parse_statement(ctx, p);
  if (!then)
    return NULL;
  cst_add_child(ctx, node, then);

  if (cst_at(p, "else")) {
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;

    LibcpCstNode* els = cst_parse_statement(ctx, p);
    if (!els)
      return NULL;
    cst_add_child(ctx, node, els);
  }
  return node;
}

static LibcpCstNode* cst_parse_switch(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = new_cst_node(ctx, CST_SWITCH);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, "(")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  LibcpCstNode* cond = cst_parse_comma(ctx, p);
  if (!p->ok || !cst_at(p, ")")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_child(ctx, node, cond);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  LibcpCstNode* body = cst_parse_statement(ctx, p);
  if (!body)
    return NULL;
  cst_add_child(ctx, node, body);
  return node;
}

static LibcpCstNode* cst_parse_while(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = new_cst_node(ctx, CST_WHILE);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, "(")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  LibcpCstNode* cond = cst_parse_comma(ctx, p);
  if (!p->ok || !cst_at(p, ")")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_child(ctx, node, cond);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  LibcpCstNode* body = cst_parse_statement(ctx, p);
  if (!body)
    return NULL;
  cst_add_child(ctx, node, body);
  return node;
}

static LibcpCstNode* cst_parse_do(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = new_cst_node(ctx, CST_DO);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  LibcpCstNode* body = cst_parse_statement(ctx, p);
  if (!body)
    return NULL;
  cst_add_child(ctx, node, body);

  if (!cst_at(p, "while")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, "(")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  LibcpCstNode* cond = cst_parse_comma(ctx, p);
  if (!p->ok || !cst_at(p, ")")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_child(ctx, node, cond);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, ";")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;
  return node;
}

static LibcpCstNode* cst_parse_for(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = new_cst_node(ctx, CST_FOR);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, "(")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  // 初始化部分: 声明自带结尾的 ';', 表达式则不带.
  if (cst_at(p, ";")) {
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;
  } else if (cst_is_decl_start(p->tok, p->cst)) {
    LibcpCstNode* decl = cst_parse_decl(ctx, p);
    if (!decl)
      return NULL;
    cst_add_child(ctx, node, decl);
  } else {
    LibcpCstNode* init = cst_parse_comma(ctx, p);
    if (!p->ok || !cst_at(p, ";")) {
      cst_fail(p);
      return NULL;
    }
    cst_add_child(ctx, node, init);
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;
  }

  if (!cst_at(p, ";")) {
    LibcpCstNode* cond = cst_parse_comma(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, node, cond);
  }
  if (!cst_at(p, ";")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, ")")) {
    LibcpCstNode* inc = cst_parse_comma(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, node, inc);
  }
  if (!cst_at(p, ")")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  LibcpCstNode* body = cst_parse_statement(ctx, p);
  if (!body)
    return NULL;
  cst_add_child(ctx, node, body);
  return node;
}

static LibcpCstNode* cst_parse_case(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = new_cst_node(ctx, CST_CASE);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  LibcpCstNode* begin = cst_parse_conditional(ctx, p);
  if (!p->ok)
    return NULL;
  cst_add_child(ctx, node, begin);

  if (cst_at(p, "...")) {
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;

    LibcpCstNode* end = cst_parse_conditional(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, node, end);
  }

  if (!cst_at(p, ":")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  LibcpCstNode* body = cst_parse_statement(ctx, p);
  if (!body)
    return NULL;
  cst_add_child(ctx, node, body);
  return node;
}

static LibcpCstNode* cst_parse_default(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = new_cst_node(ctx, CST_DEFAULT);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (!cst_at(p, ":")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  LibcpCstNode* body = cst_parse_statement(ctx, p);
  if (!body)
    return NULL;
  cst_add_child(ctx, node, body);
  return node;
}

static LibcpCstNode* cst_parse_label(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = new_cst_node(ctx, CST_LABEL);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  cst_add_leaf(ctx, node, p->tok);  // 冒号
  p->tok = p->tok->next;

  LibcpCstNode* body = cst_parse_statement(ctx, p);
  if (!body)
    return NULL;
  cst_add_child(ctx, node, body);
  return node;
}

static LibcpCstNode* cst_parse_goto(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = new_cst_node(ctx, CST_GOTO);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  if (cst_at(p, "*")) {
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;

    LibcpCstNode* expr = cst_parse_comma(ctx, p);
    if (!p->ok)
      return NULL;
    cst_add_child(ctx, node, expr);
  } else {
    if (!p->tok || p->tok->kind != TK_IDENT) {
      cst_fail(p);
      return NULL;
    }
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;
  }

  if (!cst_at(p, ";")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;
  return node;
}

static LibcpCstNode* cst_parse_asm(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node = new_cst_node(ctx, CST_ASM);
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;

  while (cst_at(p, "volatile") || cst_at(p, "inline")) {
    cst_add_leaf(ctx, node, p->tok);
    p->tok = p->tok->next;
  }

  if (!cst_at(p, "(")) {
    cst_fail(p);
    return NULL;
  }
  LibcpCstNode* operand = cst_parse_raw_group(ctx, p);
  if (!p->ok)
    return NULL;
  cst_add_child(ctx, node, operand);

  if (!cst_at(p, ";")) {
    cst_fail(p);
    return NULL;
  }
  cst_add_leaf(ctx, node, p->tok);
  p->tok = p->tok->next;
  return node;
}

static LibcpCstNode* cst_parse_statement(LibcpContext* ctx, LibcpCstParser* p) {
  if (++p->cst->depth > MAX_CST_DEPTH) {
    p->cst->depth--;
    cst_fail(p);
    return NULL;
  }
  LibcpCstNode* node = cst_parse_statement1(ctx, p);
  p->cst->depth--;
  return node;
}

static LibcpCstNode* cst_parse_statement1(LibcpContext* ctx, LibcpCstParser* p) {
  if (!p->ok || !p->tok || p->tok->kind == TK_EOF)
    return NULL;

  LibcpToken* tok = p->tok;

  if (libcp_equal(tok, "return"))
    return cst_parse_return(ctx, p);
  if (libcp_equal(tok, "if"))
    return cst_parse_if(ctx, p);
  if (libcp_equal(tok, "switch"))
    return cst_parse_switch(ctx, p);
  if (libcp_equal(tok, "while"))
    return cst_parse_while(ctx, p);
  if (libcp_equal(tok, "do"))
    return cst_parse_do(ctx, p);
  if (libcp_equal(tok, "for"))
    return cst_parse_for(ctx, p);
  if (libcp_equal(tok, "case"))
    return cst_parse_case(ctx, p);
  if (libcp_equal(tok, "default"))
    return cst_parse_default(ctx, p);
  if (libcp_equal(tok, "goto"))
    return cst_parse_goto(ctx, p);
  if (libcp_equal(tok, "break"))
    return cst_parse_simple_stmt(ctx, p, CST_BREAK);
  if (libcp_equal(tok, "continue"))
    return cst_parse_simple_stmt(ctx, p, CST_CONTINUE);
  if (libcp_equal(tok, "asm") || libcp_equal(tok, "__asm__"))
    return cst_parse_asm(ctx, p);

  if (tok->kind == TK_IDENT && libcp_equal(tok->next, ":"))
    return cst_parse_label(ctx, p);

  if (libcp_equal(tok, "{")) {
    LibcpCstNode* block = new_cst_node(ctx, CST_BLOCK);
    cst_parse_compound(ctx, p, block);
    return p->ok ? block : NULL;
  }

  if (libcp_equal(tok, ";")) {
    LibcpCstNode* null = new_cst_node(ctx, CST_NULL_STMT);
    cst_add_leaf(ctx, null, tok);
    p->tok = tok->next;
    return null;
  }

  // 只有当第一个 token 能开始声明时才尝试声明; 因此未知类型 (`size_t x;`)
  // 会在表达式路径上失败并变成 UNPARSED, 而不是被猜测.
  if (cst_is_decl_start(tok, p->cst)) {
    LibcpCstNode* decl = cst_parse_decl(ctx, p);
    if (decl)
      return decl;
    cst_fail(p);
    return NULL;
  }

  LibcpToken* next   = NULL;
  LibcpCstNode* stmt = cst_try_expr_stmt(ctx, &next, tok, p->cst);
  if (stmt) {
    p->tok = next;
    return stmt;
  }

  cst_fail(p);
  return NULL;
}

//
// 预处理指令层 (C3.1)
//
// 指令行从 at_bol 的 '#' 开始 - 与 preprocess.c 的 is_hash 是同一判定 - 并在
// 下一个 at_bol token 之前结束. 扫描缓冲区已经去掉了续行, 块注释内的换行
// 不会置 at_bol, 所以多行指令在这里同样算一行. 指令的 token 保持为叶子;
// 树只按行分组它们, 对条件指令再按分支分组. 条件不求值, 宏不展开,
// 也不查找任何文件.
//

static bool cst_is_directive(LibcpToken* tok) {
  return tok && tok->kind != TK_EOF && tok->at_bol && libcp_equal(tok, "#");
}

// '#' 之后的指令词. 返回 NULL 表示空指令 (逻辑行上只有一个 `#`).
static LibcpToken* cst_directive_name(LibcpToken* tok) {
  LibcpToken* name = tok->next;
  if (!name || name->kind == TK_EOF || name->at_bol)
    return NULL;
  return name;
}

// #elif / #else / #endif 结束一个分支体; 它们属于外层的 CST_PP_IF,
// 而不属于分支体的 item.
static bool cst_is_pp_terminator(LibcpToken* tok) {
  if (!cst_is_directive(tok))
    return false;
  LibcpToken* name = cst_directive_name(tok);
  return name &&
         (libcp_equal(name, "elif") || libcp_equal(name, "else") || libcp_equal(name, "endif"));
}

static bool cst_is_pp_if(LibcpToken* tok) {
  if (!cst_is_directive(tok))
    return false;
  LibcpToken* name = cst_directive_name(tok);
  return name &&
         (libcp_equal(name, "if") || libcp_equal(name, "ifdef") || libcp_equal(name, "ifndef"));
}

// 本指令行之后的第一个 token: 下一个 at_bol token, 或者 EOF. 在
// libcp_tokenize_file_impl() 的输出上从不为 NULL.
static LibcpToken* cst_directive_end(LibcpToken* tok) {
  LibcpToken* p = tok->next;
  while (p && p->kind != TK_EOF && !p->at_bol)
    p = p->next;
  return p;
}

// 按拼写决定指令的 kind. '#' 之后的数字是 GNU 行标记 (`# 123 "file"`);
// 无法识别的词保持 CST_PP_DIRECTIVE, 空行则是 CST_PP_NULL. 在良构的条件
// 指令中, #if / #elif / #else / #endif 不会走到这个函数.
static LibcpCstKind cst_pp_kind(LibcpToken* tok) {
  LibcpToken* name = cst_directive_name(tok);
  if (!name)
    return CST_PP_NULL;
  if (name->kind == TK_PP_NUM)
    return CST_PP_LINE;
  if (libcp_equal(name, "define"))
    return CST_PP_DEFINE;
  if (libcp_equal(name, "undef"))
    return CST_PP_UNDEF;
  if (libcp_equal(name, "include"))
    return CST_PP_INCLUDE;
  if (libcp_equal(name, "line"))
    return CST_PP_LINE;
  if (libcp_equal(name, "pragma"))
    return CST_PP_PRAGMA;
  if (libcp_equal(name, "error"))
    return CST_PP_ERROR;
  return CST_PP_DIRECTIVE;
}

// 函数式宏的参数列表: 从 '(' 到配对的 ')', 不越过行尾. 在合法输入里参数
// 不会嵌套, 因此分组保持扁平; 缺少 ')' 时在行尾停止, 并把该分组变成
// UNPARSED.
static LibcpCstNode*
    cst_parse_pp_params(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpToken* end) {
  LibcpCstNode* group = new_cst_node(ctx, CST_GROUP_PAREN);
  cst_add_leaf(ctx, group, tok);

  int depth     = 1;
  LibcpToken* p = tok->next;
  while (p != end) {
    if (libcp_equal(p, "(")) {
      depth++;
    } else if (libcp_equal(p, ")")) {
      depth--;
      cst_add_leaf(ctx, group, p);
      p = p->next;
      if (depth == 0) {
        *rest = p;
        return group;
      }
      continue;
    }
    cst_add_leaf(ctx, group, p);
    p = p->next;
  }

  group->kind = CST_UNPARSED;
  *rest       = p;
  return group;
}

// 恰好消费一条指令行. 它的所有 token 都成为指令节点的叶子. 对 #define,
// 只有当 '(' 前没有空格时, 宏名之后才跟一个 GROUP_PAREN - 这正是
// preprocess.c 中对象式宏与函数式宏的分界 - 因此 dump 记录了这一区别.
static LibcpCstNode* cst_parse_directive_line(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpToken* hash   = p->tok;
  LibcpToken* end    = cst_directive_end(hash);
  LibcpCstNode* node = new_cst_node(ctx, cst_pp_kind(hash));
  cst_add_leaf(ctx, node, hash);

  LibcpToken* tok = hash->next;
  if (node->kind == CST_PP_DEFINE && tok != end && tok->kind == TK_IDENT) {
    cst_add_leaf(ctx, node, tok);  // 指令名 "define"
    tok = tok->next;

    if (tok != end && tok->kind == TK_IDENT) {
      cst_add_leaf(ctx, node, tok);  // 宏名
      tok = tok->next;

      if (tok != end && libcp_equal(tok, "(") && !tok->has_space) {
        LibcpToken* rest = NULL;
        cst_add_child(ctx, node, cst_parse_pp_params(ctx, &rest, tok, end));
        tok = rest;
      }
    }
  }

  while (tok != end) {
    cst_add_leaf(ctx, node, tok);
    tok = tok->next;
  }

  p->tok = end;
  return node;
}

// #if / #ifdef / #ifndef 开启一个条件分组: 每个 #if / #elif / #else 一个
// CST_PP_BRANCH, 收尾的 #endif 作为分组的最后一个子节点. 分支体用与外围
// 相同的 item 文法, 嵌套通过 cst_parse_item 递归, 每个分支都保留其 token.
// 未终止的分组在 EOF 或外层闭括号处结束; 孤立的 #elif / #else / #endif
// 当作普通指令行.
static LibcpCstNode* cst_parse_pp_if(LibcpContext* ctx, LibcpCstParser* p) {
  LibcpCstNode* node   = new_cst_node(ctx, CST_PP_IF);
  LibcpCstNode* branch = new_cst_node(ctx, CST_PP_BRANCH);
  cst_add_child(ctx, branch, cst_parse_directive_line(ctx, p));
  cst_add_child(ctx, node, branch);

  for (;;) {
    while (p->ok && p->tok && p->tok->kind != TK_EOF && !cst_is_pp_terminator(p->tok)) {
      // 闭括号属于外层块; 把它留在那里, 让分组以未终止状态结束, 而不是吞掉它.
      if (cst_is_close(p->tok))
        return node;
      cst_add_child(ctx, branch, cst_parse_item(ctx, p));
    }

    if (!p->ok || !p->tok || p->tok->kind == TK_EOF || !cst_is_directive(p->tok))
      return node;

    LibcpToken* name = cst_directive_name(p->tok);
    if (libcp_equal(name, "endif")) {
      cst_add_child(ctx, node, cst_parse_directive_line(ctx, p));
      return node;
    }

    branch = new_cst_node(ctx, CST_PP_BRANCH);
    cst_add_child(ctx, branch, cst_parse_directive_line(ctx, p));
    cst_add_child(ctx, node, branch);
  }
}

// 语句层的一个 item: 一条指令行 (条件指令递归), 或者带 raw 回退的语句 /
// 声明. 只要有 token 可用, 每条路径都至少消费一个 token.
static LibcpCstNode* cst_parse_item(LibcpContext* ctx, LibcpCstParser* p) {
  if (++p->cst->depth > MAX_CST_DEPTH) {
    p->cst->depth--;
    LibcpToken* start  = p->tok;
    p->ok              = true;
    LibcpToken* next   = NULL;
    LibcpCstNode* item = cst_parse_fallback_statement(ctx, p->cst, &next, start);
    p->tok             = next;
    return item;
  }
  LibcpCstNode* item = cst_parse_item1(ctx, p);
  p->cst->depth--;
  return item;
}

static LibcpCstNode* cst_parse_item1(LibcpContext* ctx, LibcpCstParser* p) {
  if (cst_is_directive(p->tok)) {
    if (cst_is_pp_if(p->tok))
      return cst_parse_pp_if(ctx, p);
    return cst_parse_directive_line(ctx, p);
  }

  LibcpToken* start  = p->tok;
  LibcpCstNode* item = cst_parse_statement(ctx, p);
  if (item)
    return item;

  p->tok           = start;
  p->ok            = true;
  LibcpToken* next = NULL;
  item             = cst_parse_fallback_statement(ctx, p->cst, &next, start);
  p->tok           = next;
  return item;
}

// compound-stmt = "{" (declaration | statement)* "}"
//
// 失败的 item 会从它的第一个 token 起用 raw 回退重建, 因此一个畸形构造不会
// 拖累块里其余内容. 缺少 '}' 时块本身变成 UNPARSED; 解析在 EOF 处结束.
static void cst_parse_compound(LibcpContext* ctx, LibcpCstParser* p, LibcpCstNode* block) {
  cst_add_leaf(ctx, block, p->tok);  // 左花括号
  p->tok = p->tok->next;

  while (p->ok && p->tok && p->tok->kind != TK_EOF && !libcp_equal(p->tok, "}")) {
    if (cst_is_close(p->tok)) {
      LibcpCstNode* unparsed = new_cst_node(ctx, CST_UNPARSED);
      cst_add_leaf(ctx, unparsed, p->tok);
      cst_add_child(ctx, block, unparsed);
      p->tok = p->tok->next;
      continue;
    }

    cst_add_child(ctx, block, cst_parse_item(ctx, p));
  }

  if (p->ok && p->tok && p->tok->kind != TK_EOF && libcp_equal(p->tok, "}")) {
    cst_add_leaf(ctx, block, p->tok);
    p->tok = p->tok->next;
  } else {
    block->kind = CST_UNPARSED;
  }
}

// 把顶层切分成 item. EOF token 留给 cst_parse 处理, 这样无论 item 怎么解析,
// 它都保持为树的最后一个叶子.
static void cst_parse_items(LibcpContext* ctx, LibcpCstNode* parent, LibcpCstParser* p) {
  while (p->ok && p->tok && p->tok->kind != TK_EOF) {
    if (cst_is_close(p->tok)) {
      LibcpCstNode* unparsed = new_cst_node(ctx, CST_UNPARSED);
      cst_add_leaf(ctx, unparsed, p->tok);
      cst_add_child(ctx, parent, unparsed);
      p->tok = p->tok->next;
      continue;
    }

    cst_add_child(ctx, parent, cst_parse_item(ctx, p));
  }
}

// 为 libcp_tokenize_file_impl() 的输出构建 CST. EOF 标记有独立的 kind,
// 因为它携带文件末尾的 trivia 且没有拼写.
LibcpCstNode* libcp_cst_parse(LibcpContext* ctx, LibcpToken* tok) {
  ENTER_RET(ctx, NULL);
  LibcpCstContext cst = {};

  // 先放入种子名字: 它们与文件内扫描结果一起参与每一个声明判定
  // (PLAN-l5 W2).
  for (int i = 0; i < ctx->cst_seeds_len; i++)
    libcp_strarray_push(ctx, &cst.typedefs, libcp_arena_strdup(ctx, ctx->cst_seeds[i]));
  cst_collect_typedefs(ctx, &cst, tok);

  LibcpCstParser p   = {tok, true, &cst};
  LibcpCstNode* root = new_cst_node(ctx, CST_TRANSLATION_UNIT);
  cst_parse_items(ctx, root, &p);

  if (p.tok && p.tok->kind == TK_EOF)
    cst_add_leaf(ctx, root, p.tok);
  LEAVE(ctx);
  return root;
}

// 叶子按先 trivia 后拼写的顺序写出原始字节; 内部节点递归下降.
static void cst_reconstruct_node(LibcpCstNode* node, FILE* out) {
  if (node->tok) {
    fwrite(node->tok->trivia, 1, node->tok->trivia_len, out);
    fwrite(node->tok->raw_loc, 1, node->tok->raw_len, out);
    return;
  }

  for (int i = 0; i < node->child_count; i++)
    cst_reconstruct_node(node->children[i], out);
}

// 按顺序写出树中各叶子的原始字节 (每个 token 先 trivia 后拼写). 对
// libcp_tokenize_file_impl() 的直接输出, 这等于输入文件的原始文本.
void libcp_cst_reconstruct(LibcpContext* ctx, LibcpCstNode* root, FILE* out) {
  ENTER_VOID(ctx);
  cst_reconstruct_node(root, out);
  LEAVE(ctx);
}

// CST kind 到 dump 文本名的映射; 必须覆盖所有 kind, 否则落到 unreachable().
static char* cst_kind_name(LibcpContext* ctx, LibcpCstKind kind) {
  switch (kind) {
    case CST_TRANSLATION_UNIT: return "TU";
    case CST_TOKEN: return "TOKEN";
    case CST_EOF: return "EOF";
    case CST_GROUP_PAREN: return "GROUP_PAREN";
    case CST_GROUP_BRACKET: return "GROUP_BRACKET";
    case CST_GROUP_BRACE: return "GROUP_BRACE";
    case CST_BLOCK: return "BLOCK";
    case CST_STMT_EXPR: return "STMT_EXPR";
    case CST_GENERIC_ASSOC: return "GENERIC_ASSOC";
    case CST_NULL_STMT: return "NULL_STMT";
    case CST_UNPARSED: return "UNPARSED";
    case CST_EXPR_STMT: return "EXPR_STMT";
    case CST_BINARY: return "BINARY";
    case CST_UNARY: return "UNARY";
    case CST_POSTFIX: return "POSTFIX";
    case CST_CAST: return "CAST";
    case CST_COND: return "COND";
    case CST_ASSIGN: return "ASSIGN";
    case CST_COMMA: return "COMMA";
    case CST_PAREN: return "PAREN";
    case CST_PRIMARY: return "PRIMARY";
    case CST_ARG_LIST: return "ARG_LIST";
    case CST_INIT_LIST: return "INIT_LIST";
    case CST_COMPOUND_LITERAL: return "COMPOUND_LITERAL";
    case CST_DECL: return "DECL";
    case CST_DECL_SPECS: return "DECL_SPECS";
    case CST_DECLARATOR: return "DECLARATOR";
    case CST_INIT_DECLARATOR: return "INIT_DECLARATOR";
    case CST_FUNC_DEF: return "FUNC_DEF";
    case CST_PARAM_LIST: return "PARAM_LIST";
    case CST_PARAM: return "PARAM";
    case CST_TYPE_NAME: return "TYPE_NAME";
    case CST_INITIALIZER: return "INITIALIZER";
    case CST_DESIGNATOR: return "DESIGNATOR";
    case CST_IF: return "IF";
    case CST_SWITCH: return "SWITCH";
    case CST_WHILE: return "WHILE";
    case CST_DO: return "DO";
    case CST_FOR: return "FOR";
    case CST_CASE: return "CASE";
    case CST_DEFAULT: return "DEFAULT";
    case CST_LABEL: return "LABEL";
    case CST_GOTO: return "GOTO";
    case CST_BREAK: return "BREAK";
    case CST_CONTINUE: return "CONTINUE";
    case CST_RETURN: return "RETURN";
    case CST_ASM: return "ASM";
    case CST_PP_DIRECTIVE: return "PP_DIRECTIVE";
    case CST_PP_DEFINE: return "PP_DEFINE";
    case CST_PP_UNDEF: return "PP_UNDEF";
    case CST_PP_INCLUDE: return "PP_INCLUDE";
    case CST_PP_LINE: return "PP_LINE";
    case CST_PP_PRAGMA: return "PP_PRAGMA";
    case CST_PP_ERROR: return "PP_ERROR";
    case CST_PP_NULL: return "PP_NULL";
    case CST_PP_IF: return "PP_IF";
    case CST_PP_BRANCH: return "PP_BRANCH";
  }
  unreachable();
}

// 与 token dump (main.c) 相同的转义: \n, \t, \r 与 \\ 用 C 风格, 其余不可见
// 字符用 \xHH, 包括空格, 这样 dump 行不会以空白结尾. 拼写里从不包含裸换行
// (续行属于拼写, 会转义成 \\ + \n), 因此一个叶子就是一行.
static void cst_print_escaped(FILE* out, char* p, int len) {
  for (int i = 0; i < len; i++) {
    unsigned char c = p[i];

    switch (c) {
      case '\n': fprintf(out, "\\n"); break;
      case '\t': fprintf(out, "\\t"); break;
      case '\r': fprintf(out, "\\r"); break;
      case '\\': fprintf(out, "\\\\"); break;
      default:
        if (c < 0x20 || c >= 0x7f || c == ' ')
          fprintf(out, "\\x%02x", c);
        else
          fputc(c, out);
    }
  }
}

static void cst_dump_node(LibcpContext* ctx, LibcpCstNode* node, FILE* out, int depth) {
  for (int i = 0; i < depth; i++)
    fputs("  ", out);

  fputs(cst_kind_name(ctx, node->kind), out);

  if (node->tok && node->tok->raw_len) {
    fputc(' ', out);
    cst_print_escaped(out, node->tok->raw_loc, node->tok->raw_len);
  }
  fputc('\n', out);

  for (int i = 0; i < node->child_count; i++)
    cst_dump_node(ctx, node->children[i], out, depth + 1);
}

// 打印树形, 每层两个空格. 叶子文本是 token 的原始拼写 (不含 trivia); dump
// 锁定树形, 往返重建锁定字节.
void libcp_cst_dump(LibcpContext* ctx, LibcpCstNode* root, FILE* out) {
  ENTER_VOID(ctx);
  cst_dump_node(ctx, root, out, 0);
  LEAVE(ctx);
}

// 递归校验: 叶子必须按 token 链顺序逐一对上, 内部节点不得持有 token.
static bool cst_verify_node(LibcpCstNode* node, LibcpToken** cur) {
  if (node->tok) {
    if (node->child_count || *cur != node->tok)
      return false;
    *cur = (*cur)->next;
    return true;
  }

  for (int i = 0; i < node->child_count; i++)
    if (!cst_verify_node(node->children[i], cur))
      return false;
  return true;
}

// I1 / I3: 按顺序排列的叶子恰好是 `tok` 的各个 token, 每个只出现一次
// (指针相等), 并且每个叶子持有 token 而每个内部节点不持有. 返回结果,
// 由调用者报告.
bool libcp_cst_verify(LibcpContext* ctx, LibcpCstNode* root, LibcpToken* tok) {
  ENTER_RET(ctx, false);
  LibcpToken* cur = tok;
  bool r          = cst_verify_node(root, &cur) && !cur;
  LEAVE(ctx);
  return r;
}
