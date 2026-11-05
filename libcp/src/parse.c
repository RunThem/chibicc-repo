// 第 3 层: 语法 AST. 本文件是递归下降的 C 语法分析器, 从展开流下降,
// 构建忠实于源码写法的 AST (无类型标注, 名字不绑定, 常量表达式不求值),
// 声明以记录形式留给第 4 层 sema 声明与下降. 主要内容: 说明符与声明符
// 的类型形态构建, 声明/语句/表达式 (Pratt 绑定力循环) 的解析, 以及
// 解析器私有的作用域栈 - 它只为回答 typedef 名与 tag 的语法问题.

#include "libcp.h"

static LibcpType* find_typedef(LibcpContext* ctx, LibcpToken* tok);
static LibcpType* find_tag(LibcpContext* ctx, LibcpToken* tok);
static LibcpType* find_current_tag(LibcpContext* ctx, LibcpToken* tok);
static void push_tag_scope(LibcpContext* ctx, LibcpToken* tok, LibcpType* ty);
static void add_typedef(LibcpContext* ctx, LibcpNode* node);
static void add_declared_name(LibcpContext* ctx, LibcpToken* tok);
static void enter_scope(LibcpContext* ctx);
static void leave_scope(LibcpContext* ctx);

// 限制解析器的递归深度: 病态嵌套必须以诊断失败, 而不能是栈溢出.
#define MAX_PARSE_DEPTH 512

static bool is_typename(LibcpContext* ctx, LibcpToken* tok);
static LibcpToken* attribute_list(LibcpContext* ctx, LibcpToken* tok, LibcpType* ty);
static LibcpType* declspec(LibcpContext* ctx,
                           LibcpToken** rest,
                           LibcpToken* tok,
                           LibcpVarAttr* attr,
                           LibcpNode** specs);
static LibcpType* typename(LibcpContext* ctx,
                           LibcpToken** rest,
                           LibcpToken* tok,
                           LibcpNode** specs);
static LibcpType*
    enum_specifier(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpNode** specs);
static LibcpType*
    typeof_specifier(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpNode** specs);
static LibcpType* type_suffix(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpType* ty);

// 内建类型由所有 context 共享 (type.c), tag 与 typedef 类型由每个命名
// 它的声明共享. 因此声明符从不把名字写进类型记录: 名字改由这个每次
// 声明私有的载体携带 (F1).
typedef struct {
  LibcpToken* name;      // 声明符没有名字时为 NULL
  LibcpToken* name_pos;  // 名字本应出现的位置, 供 "name omitted" 诊断使用
} LibcpDeclName;

static LibcpType* declarator(LibcpContext* ctx,
                             LibcpToken** rest,
                             LibcpToken* tok,
                             LibcpType* ty,
                             LibcpDeclName* decl);
static LibcpNode* declaration(LibcpContext* ctx,
                              LibcpToken** rest,
                              LibcpToken* tok,
                              LibcpType* basety,
                              LibcpVarAttr* attr);
static LibcpInitializer* init_record(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok);
static LibcpNode* compound_stmt(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok);
static LibcpNode* stmt(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok);
static LibcpNode* expr_stmt(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok);
static LibcpNode* expr(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok);
static LibcpNode* assign(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok);
static LibcpType*
    struct_decl(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpNode** specs);
static LibcpType*
    union_decl(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpNode** specs);
static LibcpNode* funcall(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpNode* node);
static LibcpNode* unary(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok);
// 每个带深度守卫的入口背后的本体 (包装函数在调用它之前先递增
static LibcpType* declarator1(LibcpContext* ctx,
                              LibcpToken** rest,
                              LibcpToken* tok,
                              LibcpType* ty,
                              LibcpDeclName* decl);
static LibcpType*
    abstract_declarator(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpType* ty);
static LibcpType*
    abstract_declarator1(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpType* ty);
static LibcpInitializer* init_record1(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok);
static LibcpNode* primary(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok);
static LibcpNode*
    parse_typedef(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpType* basety);
static bool is_function(LibcpContext* ctx, LibcpToken* tok);
static LibcpNode* function_def(LibcpContext* ctx,
                               LibcpToken** rest,
                               LibcpToken* tok,
                               LibcpType* basety,
                               LibcpVarAttr* attr);
static LibcpNode* global_variable(LibcpContext* ctx,
                                  LibcpToken** rest,
                                  LibcpToken* tok,
                                  LibcpType* basety,
                                  LibcpVarAttr* attr);

LibcpNode* libcp_new_node(LibcpContext* ctx, LibcpNodeKind kind, LibcpToken* tok) {
  LibcpNode* node = libcp_arena_calloc(ctx, 1, sizeof(LibcpNode));
  node->kind      = kind;
  node->tok       = tok;
  return node;
}

LibcpNode* libcp_new_binary(LibcpContext* ctx,
                            LibcpNodeKind kind,
                            LibcpNode* lhs,
                            LibcpNode* rhs,
                            LibcpToken* tok) {
  LibcpNode* node = libcp_new_node(ctx, kind, tok);
  node->lhs       = lhs;
  node->rhs       = rhs;
  return node;
}

LibcpNode*
    libcp_new_unary(LibcpContext* ctx, LibcpNodeKind kind, LibcpNode* expr, LibcpToken* tok) {
  LibcpNode* node = libcp_new_node(ctx, kind, tok);
  node->lhs       = expr;
  return node;
}

LibcpNode* libcp_new_num(LibcpContext* ctx, int64_t val, LibcpToken* tok) {
  LibcpNode* node = libcp_new_node(ctx, ND_NUM, tok);
  node->val       = val;
  return node;
}

char* libcp_get_ident(LibcpContext* ctx, LibcpToken* tok) {
  if (tok->kind != TK_IDENT)
    libcp_error_tok(ctx, tok, "expected an identifier");
  return libcp_arena_strndup(ctx, tok->loc, tok->len);
}

// 解析器的作用域栈
//
// C 文法不是上下文无关的: `t * x;` 是声明指针还是两个变量相乘, 取决
// 于 `t` 是否是 typedef 名; `struct T` 必须可解析, 才能知道该 tag 之前
// 是否出现过. 因此解析器维护自己的块作用域栈, 且其中不放别的东西 -
// 没有对象, 没有枚举常量, 没有类型标注. sema 在解析名字时从树中派生
// 自己的作用域栈.

// 解析器见过的标识符. typedef 名的 `type_def` 非 NULL; 普通已声明名字
// 得到一条空记录, 这样变量从声明点起就遮蔽同拼写的 typedef.
typedef struct {
  LibcpType* type_def;
} LibcpNameEntry;

struct libcp_parse_scope {
  LibcpParseScope* next;

  // C 有两个块作用域: 一个用于普通名字, 另一个用于 struct/union/enum tag.
  LibcpHashMap names;
  LibcpHashMap tags;
};

// 初始化解析器的块作用域栈, 每个翻译单位调用一次.
void libcp_parse_init(LibcpContext* ctx) {
  ctx->scope = libcp_arena_calloc(ctx, 1, sizeof(LibcpParseScope));
}

// 解析器的作用域栈存放在 LibcpContext 中 (PLAN L2.4).
#define scope (ctx->scope)

// 压入一个新的块作用域; 名字表与 tag 表都是空的 hashmap.
static void enter_scope(LibcpContext* ctx) {
  LibcpParseScope* sc = libcp_arena_calloc(ctx, 1, sizeof(LibcpParseScope));
  sc->next            = scope;
  scope               = sc;
}

// 弹出当前块作用域, 回到外层.
static void leave_scope(LibcpContext* ctx) {
  scope = scope->next;
}

// 解析器的 typedef 名判定器: C 文法在解析声明之前, 必须知道一个标识符
// 是否是类型名.
static LibcpType* find_typedef(LibcpContext* ctx, LibcpToken* tok) {
  if (tok->kind != TK_IDENT)
    return NULL;

  for (LibcpParseScope* sc = scope; sc; sc = sc->next) {
    LibcpNameEntry* e = libcp_hashmap_get2(ctx, &sc->names, tok->loc, tok->len);
    if (e)
      return e->type_def;
  }
  return NULL;
}

// 在当前作用域声明一个 typedef 名. 该名字必须立即可见: 判定器要能对
// 下一个标识符分类. sema 走到这次声明同时发出的 ND_TYPEDEF 记录时,
// 补全类型.
static void add_typedef(LibcpContext* ctx, LibcpNode* node) {
  LibcpNameEntry* e = libcp_arena_calloc(ctx, 1, sizeof(LibcpNameEntry));
  e->type_def       = node->ty;
  libcp_hashmap_put(ctx, &scope->names, libcp_get_ident(ctx, node->tok), e);
}

// 记录声明符刚刚声明的名字 (变量, 函数, 参数或枚举常量), 使其在后续
// 解析中遮蔽同拼写的 typedef.
static void add_declared_name(LibcpContext* ctx, LibcpToken* tok) {
  libcp_hashmap_put(ctx,
                    &scope->names,
                    libcp_get_ident(ctx, tok),
                    libcp_arena_calloc(ctx, 1, sizeof(LibcpNameEntry)));
}

// 在所有外层作用域中按名字查找 struct/union/enum tag.
static LibcpType* find_tag(LibcpContext* ctx, LibcpToken* tok) {
  for (LibcpParseScope* sc = scope; sc; sc = sc->next) {
    LibcpType* ty = libcp_hashmap_get2(ctx, &sc->tags, tok->loc, tok->len);
    if (ty)
      return ty;
  }
  return NULL;
}

// 只在当前作用域查找 tag; 用于 tag 的 (重) 定义, 使同一作用域中先前的
// 前向声明保持同一身份.
static LibcpType* find_current_tag(LibcpContext* ctx, LibcpToken* tok) {
  return libcp_hashmap_get2(ctx, &scope->tags, tok->loc, tok->len);
}

// 在当前作用域登记一个 tag 类型.
static void push_tag_scope(LibcpContext* ctx, LibcpToken* tok, LibcpType* ty) {
  libcp_hashmap_put2(ctx, &scope->tags, tok->loc, tok->len, ty);
}

// 把节点链 `chain` 接到 `cur` 所指向的链尾, 并返回新的链尾.
static LibcpNode* chain_append(LibcpNode* cur, LibcpNode* chain) {
  for (; chain; chain = chain->next)
    cur = cur->next = chain;
  return cur;
}

// 返回在 `specs` 之后接上 `chain` 的链 (若还没有 specs 则直接返回
// `chain`). 用于 declspec 产生的声明记录必须先于声明本身发出的记录.
static LibcpNode* specs_then(LibcpNode* specs, LibcpNode* chain) {
  if (!specs)
    return chain;
  LibcpNode* cur = specs;
  while (cur->next)
    cur = cur->next;
  cur->next = chain;
  return specs;
}

// 向 spec 列表追加一条声明记录 (一个枚举常量).
static void add_spec(LibcpNode** specs, LibcpNode* node) {
  node->next = NULL;
  if (!*specs) {
    *specs = node;
    return;
  }
  LibcpNode* cur = *specs;
  while (cur->next)
    cur = cur->next;
  cur->next = node;
}

// 类型说明符中类型名的顺序无关紧要: 例如 `int long static` 与
// `static long int` 相同; 指定 `long` 或 `short` 时可以省略 `int`, 但
// 像 `char int` 这样的组合非法, 只接受有限的内建类型名组合.
// 本函数在解析过程中为每种类型名计数, 同时维护"到此处为止"这些类型名
// 所表示的当前类型对象; 遇到非类型名 token 时返回当前类型.
// 途中 enum-specifier 定义的枚举常量是没有独立语句的声明记录, 它们被
// 追加到 `*specs`, 由调用者放到其链或节点的这个位置上.
static LibcpType* declspec(LibcpContext* ctx,
                           LibcpToken** rest,
                           LibcpToken* tok,
                           LibcpVarAttr* attr,
                           LibcpNode** specs) {
  // 用一个整数为所有类型名计数: 位 0 和 1 表示目前见到 "void" 的次数,
  // 其余同理. 这样下面就能直接用 switch 判定类型.
  enum {
    VOID     = 1 << 0,
    BOOL     = 1 << 2,
    CHAR     = 1 << 4,
    SHORT    = 1 << 6,
    INT      = 1 << 8,
    LONG     = 1 << 10,
    FLOAT    = 1 << 12,
    DOUBLE   = 1 << 14,
    OTHER    = 1 << 16,
    SIGNED   = 1 << 17,
    UNSIGNED = 1 << 18,
  };

  LibcpType* ty  = libcp_ty_int;
  int counter    = 0;
  bool is_atomic = false;

  while (is_typename(ctx, tok) || libcp_equal(tok, "__attribute__")) {
    // 声明属性与函数属性可出现在任意说明符位置; 没有类型含义的属性
    // 直接忽略.
    if (libcp_equal(tok, "__attribute__")) {
      tok = attribute_list(ctx, tok, NULL);
      continue;
    }

    // 处理存储类说明符.
    if (libcp_equal(tok, "typedef") || libcp_equal(tok, "static") || libcp_equal(tok, "extern") ||
        libcp_equal(tok, "inline") || libcp_equal(tok, "_Thread_local") ||
        libcp_equal(tok, "__thread")) {
      if (!attr)
        libcp_error_tok(ctx, tok, "storage class specifier is not allowed in this context");

      if (libcp_equal(tok, "typedef"))
        attr->is_typedef = true;
      else if (libcp_equal(tok, "static"))
        attr->is_static = true;
      else if (libcp_equal(tok, "extern"))
        attr->is_extern = true;
      else if (libcp_equal(tok, "inline"))
        attr->is_inline = true;
      else
        attr->is_tls = true;

      if (attr->is_typedef &&
          attr->is_static + attr->is_extern + attr->is_inline + attr->is_tls > 1)
        libcp_error_tok(ctx,
                        tok,
                        "typedef may not be used together with static,"
                        " extern, inline, __thread or _Thread_local");
      tok = tok->next;
      continue;
    }

    // 这些关键字识别后直接忽略.
    if (libcp_consume(&tok, tok, "const") || libcp_consume(&tok, tok, "volatile") ||
        libcp_consume(&tok, tok, "auto") || libcp_consume(&tok, tok, "register") ||
        libcp_consume(&tok, tok, "restrict") || libcp_consume(&tok, tok, "__restrict") ||
        libcp_consume(&tok, tok, "__restrict__") || libcp_consume(&tok, tok, "_Noreturn"))
      continue;

    if (libcp_equal(tok, "_Atomic")) {
      tok = tok->next;
      if (libcp_equal(tok, "(")) {
        ty  = typename(ctx, &tok, tok->next, specs);
        tok = libcp_skip(ctx, tok, ")");
      }
      is_atomic = true;
      continue;
    }

    if (libcp_equal(tok, "_Alignas")) {
      if (!attr)
        libcp_error_tok(ctx, tok, "_Alignas is not allowed in this context");
      tok = libcp_skip(ctx, tok->next, "(");

      // `_Alignas(type)` 形式要求某个可能尚未完整的类型的属性,
      // `_Alignas(expr)` 形式则是常量表达式; 两种都记录下来交给 sema
      // 裁定. 与以前一样, 最后出现的那个生效.
      if (is_typename(ctx, tok)) {
        attr->align_ty   = typename(ctx, &tok, tok, specs);
        attr->align_expr = NULL;
      } else {
        attr->align_ty   = NULL;
        attr->align_expr = libcp_conditional(ctx, &tok, tok);
      }
      tok = libcp_skip(ctx, tok, ")");
      continue;
    }

    // 处理用户定义的类型: struct/union/enum/typeof 与 typedef 名.
    LibcpType* ty2 = find_typedef(ctx, tok);
    if (libcp_equal(tok, "struct") || libcp_equal(tok, "union") || libcp_equal(tok, "enum") ||
        libcp_equal(tok, "typeof") || ty2) {
      if (counter)
        break;

      if (libcp_equal(tok, "struct")) {
        ty = struct_decl(ctx, &tok, tok->next, specs);
      } else if (libcp_equal(tok, "union")) {
        ty = union_decl(ctx, &tok, tok->next, specs);
      } else if (libcp_equal(tok, "enum")) {
        ty = enum_specifier(ctx, &tok, tok->next, specs);
      } else if (libcp_equal(tok, "typeof")) {
        ty = typeof_specifier(ctx, &tok, tok->next, specs);
      } else {
        ty  = ty2;
        tok = tok->next;
      }

      counter += OTHER;
      continue;
    }

    // 处理内建类型: 只累加计数, 随后按计数组合选出类型.
    if (libcp_equal(tok, "void"))
      counter += VOID;
    else if (libcp_equal(tok, "_Bool"))
      counter += BOOL;
    else if (libcp_equal(tok, "char"))
      counter += CHAR;
    else if (libcp_equal(tok, "short"))
      counter += SHORT;
    else if (libcp_equal(tok, "int"))
      counter += INT;
    else if (libcp_equal(tok, "long"))
      counter += LONG;
    else if (libcp_equal(tok, "float"))
      counter += FLOAT;
    else if (libcp_equal(tok, "double"))
      counter += DOUBLE;
    else if (libcp_equal(tok, "signed"))
      counter |= SIGNED;
    else if (libcp_equal(tok, "unsigned"))
      counter |= UNSIGNED;
    else
      unreachable();

    // 计数器组合唯一确定内建类型; 未列出的组合都是非法类型.
    switch (counter) {
      case VOID: ty = libcp_ty_void; break;
      case BOOL: ty = libcp_ty_bool; break;
      case CHAR:
      case SIGNED + CHAR: ty = libcp_ty_char; break;
      case UNSIGNED + CHAR: ty = libcp_ty_uchar; break;
      case SHORT:
      case SHORT + INT:
      case SIGNED + SHORT:
      case SIGNED + SHORT + INT: ty = libcp_ty_short; break;
      case UNSIGNED + SHORT:
      case UNSIGNED + SHORT + INT: ty = libcp_ty_ushort; break;
      case INT:
      case SIGNED:
      case SIGNED + INT: ty = libcp_ty_int; break;
      case UNSIGNED:
      case UNSIGNED + INT: ty = libcp_ty_uint; break;
      case LONG:
      case LONG + INT:
      case LONG + LONG:
      case LONG + LONG + INT:
      case SIGNED + LONG:
      case SIGNED + LONG + INT:
      case SIGNED + LONG + LONG:
      case SIGNED + LONG + LONG + INT: ty = libcp_ty_long; break;
      case UNSIGNED + LONG:
      case UNSIGNED + LONG + INT:
      case UNSIGNED + LONG + LONG:
      case UNSIGNED + LONG + LONG + INT: ty = libcp_ty_ulong; break;
      case FLOAT: ty = libcp_ty_float; break;
      case DOUBLE: ty = libcp_ty_double; break;
      case LONG + DOUBLE: ty = libcp_ty_ldouble; break;
      default: libcp_error_tok(ctx, tok, "invalid type");
    }

    tok = tok->next;
  }

  // _Atomic 只标记类型副本, 不改写共享的内建类型记录.
  if (is_atomic) {
    ty            = libcp_copy_type(ctx, ty);
    ty->is_atomic = true;
  }

  *rest = tok;
  return ty;
}

// 函数参数表: 单独的 `void` 表示没有参数; 每个参数由 declspec 加
// 声明符组成, 末尾可以用 "..." 表示可变参数.
static LibcpType*
    func_params(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpType* ty) {
  if (libcp_equal(tok, "void") && libcp_equal(tok->next, ")")) {
    *rest = tok->next->next;
    return libcp_func_type(ctx, ty);
  }

  LibcpType head   = {};
  LibcpType* cur   = &head;
  bool is_variadic = false;
  LibcpNode* specs = NULL;

  while (!libcp_equal(tok, ")")) {
    if (cur != &head)
      tok = libcp_skip(ctx, tok, ",");

    if (libcp_equal(tok, "...")) {
      is_variadic = true;
      tok         = tok->next;
      libcp_skip(ctx, tok, ")");
      break;
    }

    LibcpDeclName decl = {};
    LibcpType* ty2     = declspec(ctx, &tok, tok, NULL, &specs);
    ty2                = declarator(ctx, &tok, tok, ty2, &decl);

    if (ty2->kind == TY_ARRAY) {
      // 只在参数上下文中把 "T 的数组" 转换为 "指向 T 的指针": 例如
      // *argv[] 在这里被转换为 **argv.
      ty2 = libcp_pointer_to(ctx, ty2->base);
    } else if (ty2->kind == TY_FUNC) {
      // 同理, 函数也只在参数上下文中转换为函数指针.
      ty2 = libcp_pointer_to(ctx, ty2);
    }

    // 参数记录被复制进函数类型; 名字存放在这份每参数私有的副本上,
    // 由 sema 读取.
    LibcpType* param = libcp_copy_type(ctx, ty2);
    param->name      = decl.name;
    param->name_pos  = decl.name_pos;
    cur = cur->next = param;
  }

  // 空参数表 `()` 按可变参数处理, 与旧实现一致.
  if (cur == &head)
    is_variadic = true;

  ty              = libcp_func_type(ctx, ty);
  ty->params      = head.next;
  ty->is_variadic = is_variadic;
  // 参数 declspec 定义的枚举常量 (例如 `int f(enum E { A } x)`) 挂在
  // 函数类型上; sema 在补全该类型时登记它们.
  ty->spec_decls  = specs;
  *rest           = tok->next;
  return ty;
}

// 数组维度: 可选的 `static` / `restrict` 限定, 可选的常量表达式长度,
// 然后是 `]` 与后续的类型后缀.
static LibcpType*
    array_dimensions(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpType* ty) {
  while (libcp_equal(tok, "static") || libcp_equal(tok, "restrict"))
    tok = tok->next;

  if (libcp_equal(tok, "]")) {
    ty = type_suffix(ctx, rest, tok->next, ty);
    return libcp_array_of(ctx, ty, -1);
  }

  LibcpNode* expr = libcp_conditional(ctx, &tok, tok);
  tok             = libcp_skip(ctx, tok, "]");
  ty              = type_suffix(ctx, rest, tok, ty);

  // 该维度表示定长数组还是 VLA, 取决于长度表达式是否是常量表达式 -
  // 这是语义问题. 表达式在这里原样记录, 由 sema 在解析声明符时决定
  // 类型 (见 resolve_type).
  return libcp_array_of_dim(ctx, ty, expr);
}

// type-suffix = "(" func-params
//             | "[" array-dimensions
//             | ε
//
// 类型后缀: 函数参数表, 数组维度, 或为空.
static LibcpType*
    type_suffix(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpType* ty) {
  if (libcp_equal(tok, "("))
    return func_params(ctx, rest, tok->next, ty);

  if (libcp_equal(tok, "["))
    return array_dimensions(ctx, rest, tok->next, ty);

  *rest = tok;
  return ty;
}

// 指针层: 每个 `*` 加一层指针, 其后的 const/volatile/restrict 限定
// 只被跳过, 不参与类型.
static LibcpType* pointers(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpType* ty) {
  while (libcp_consume(&tok, tok, "*")) {
    ty = libcp_pointer_to(ctx, ty);
    while (libcp_equal(tok, "const") || libcp_equal(tok, "volatile") ||
           libcp_equal(tok, "restrict") || libcp_equal(tok, "__restrict") ||
           libcp_equal(tok, "__restrict__"))
      tok = tok->next;
  }
  *rest = tok;
  return ty;
}

// 声明符: 构造类型的形态, 并把无法在语法层决定的部分 (数组维度,
// typeof 操作数) 留成记录, 交给 sema 补全.
static LibcpType* declarator(LibcpContext* ctx,
                             LibcpToken** rest,
                             LibcpToken* tok,
                             LibcpType* ty,
                             LibcpDeclName* decl) {
  if (++ctx->parse_depth > MAX_PARSE_DEPTH)
    libcp_error_tok(ctx, tok, "nesting too deep");
  LibcpType* node = declarator1(ctx, rest, tok, ty, decl);
  ctx->parse_depth--;
  return node;
}

static LibcpType* declarator1(LibcpContext* ctx,
                              LibcpToken** rest,
                              LibcpToken* tok,
                              LibcpType* ty,
                              LibcpDeclName* decl) {
  ty = pointers(ctx, &tok, tok, ty);

  if (libcp_equal(tok, "(")) {
    LibcpToken* start = tok;
    LibcpType dummy   = {};
    // 嵌套声明符: 先用占位类型空跑一遍, 定位到匹配的 `)` 并取到后续
    // 类型后缀, 再用真实类型重走内层声明符.
    declarator(ctx, &tok, start->next, &dummy, NULL);
    tok = libcp_skip(ctx, tok, ")");
    ty  = type_suffix(ctx, rest, tok, ty);
    return declarator(ctx, &tok, start->next, ty, decl);
  }

  LibcpToken* name     = NULL;
  LibcpToken* name_pos = tok;

  if (tok->kind == TK_IDENT) {
    name = tok;
    tok  = tok->next;
  }

  ty    = type_suffix(ctx, rest, tok, ty);
  // 具名声明在这里可以接受尾随属性. 对具名声明忽略属性的类型含义,
  // 因此共享的类型记录永远不会被改写.
  tok   = *rest;
  tok   = attribute_list(ctx, tok, NULL);
  *rest = tok;
  if (decl) {
    decl->name     = name;
    decl->name_pos = name_pos;
  }
  return ty;
}

// 抽象声明符: 没有名字的声明符, 用于类型名 (sizeof, cast, 函数参数等).
static LibcpType*
    abstract_declarator(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpType* ty) {
  if (++ctx->parse_depth > MAX_PARSE_DEPTH)
    libcp_error_tok(ctx, tok, "nesting too deep");
  LibcpType* node = abstract_declarator1(ctx, rest, tok, ty);
  ctx->parse_depth--;
  return node;
}

static LibcpType*
    abstract_declarator1(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpType* ty) {
  ty = pointers(ctx, &tok, tok, ty);

  if (libcp_equal(tok, "(")) {
    LibcpToken* start = tok;
    LibcpType dummy   = {};
    abstract_declarator(ctx, &tok, start->next, &dummy);
    tok = libcp_skip(ctx, tok, ")");
    ty  = type_suffix(ctx, rest, tok, ty);
    return abstract_declarator(ctx, &tok, start->next, ty);
  }

  ty    = type_suffix(ctx, rest, tok, ty);
  tok   = *rest;
  tok   = attribute_list(ctx, tok, NULL);
  *rest = tok;
  return ty;
}

// 类型名: 说明符加抽象声明符.
static LibcpType* typename(LibcpContext* ctx,
                           LibcpToken** rest,
                           LibcpToken* tok,
                           LibcpNode** specs) {
  LibcpType* ty = declspec(ctx, &tok, tok, NULL, specs);
  return abstract_declarator(ctx, rest, tok, ty);
}

// 尝试消费 `}` 或 `,}`, 用于 brace 列表与 enum-list 的收尾; 返回是否
// 已经消费.
static bool consume_end(LibcpToken** rest, LibcpToken* tok) {
  if (libcp_equal(tok, "}")) {
    *rest = tok->next;
    return true;
  }

  if (libcp_equal(tok, ",") && libcp_equal(tok->next, "}")) {
    *rest = tok->next->next;
    return true;
  }

  return false;
}

// 枚举说明符: 可选的 tag, 后接枚举列表 (可选); 或者只引用已有的枚举
// tag. 枚举列表项可带显式常量值.
static LibcpType*
    enum_specifier(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpNode** specs) {
  LibcpType* ty = libcp_enum_type(ctx);

  // 读 tag (在枚举说明符里实际是 enum tag).
  LibcpToken* tag = NULL;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }
  if (tag)
    ty->tag_tok = tag;

  if (tag && !libcp_equal(tok, "{")) {
    LibcpType* ty = find_tag(ctx, tag);
    if (!ty)
      libcp_error_tok(ctx, tag, "unknown enum type");
    if (ty->kind != TY_ENUM)
      libcp_error_tok(ctx, tag, "not an enum tag");
    *rest = tok;
    return ty;
  }

  tok = libcp_skip(ctx, tok, "{");

  // 读枚举列表: 每个成员成为一条声明记录节点, 携带名字, 可选的显式
  // 值表达式与枚举类型; sema 对值求值, 并在所在链的这个位置登记名字.
  int i = 0;
  while (!consume_end(rest, tok)) {
    if (i++ > 0)
      tok = libcp_skip(ctx, tok, ",");

    LibcpToken* name = tok;
    tok              = tok->next;
    add_declared_name(ctx, name);

    LibcpNode* node = libcp_new_node(ctx, ND_ENUM_CONST, name);
    if (libcp_equal(tok, "="))
      node->lhs = libcp_conditional(ctx, &tok, tok->next);
    node->ty_op = ty;

    add_spec(specs, node);
  }

  if (tag)
    push_tag_scope(ctx, tag, ty);
  return ty;
}

// typeof 说明符: 圆括号内是一个表达式或一个类型名.
static LibcpType*
    typeof_specifier(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpNode** specs) {
  tok = libcp_skip(ctx, tok, "(");

  LibcpType* ty;
  if (is_typename(ctx, tok)) {
    ty = typename(ctx, &tok, tok, specs);
  } else {
    // 表达式具有什么类型不是语法问题, 因此操作数记录在一个占位记录里,
    // 供声明符继续构建; sema 标注该表达式并填充这条记录.
    ty              = libcp_typeof_placeholder(ctx);
    ty->typeof_expr = expr(ctx, &tok, tok);
  }
  *rest = libcp_skip(ctx, tok, ")");
  return ty;
}

// 声明: 每个声明符发出一个锚定在它所声明名字上的 ND_DECL 记录 - 名字
// 来自声明符的局部记录, 存储类与对齐来自 `attr`, 初始化器保持忠实记录.
// 这些记录直接进入所在语句链: 声明在源码中不是块, 因此承载它们的块
// 就是源码写出的那个块. sema 在解析记录时声明对象, 在标注类型时下降
// 节点, 包括 VLA 长度的兄弟语句与块作用域 static 的数据映像, 其形态
// 被精确复现.
static LibcpNode* declaration(LibcpContext* ctx,
                              LibcpToken** rest,
                              LibcpToken* tok,
                              LibcpType* basety,
                              LibcpVarAttr* attr) {
  LibcpNode head = {};
  LibcpNode* cur = &head;
  int i          = 0;

  while (!libcp_equal(tok, ";")) {
    if (i++ > 0)
      tok = libcp_skip(ctx, tok, ",");

    LibcpDeclName decl_name = {};
    LibcpType* ty           = declarator(ctx, &tok, tok, basety, &decl_name);
    if (!decl_name.name)
      libcp_error_tok(ctx, decl_name.name_pos, "variable name omitted");

    // 被声明的名字在后续解析中遮蔽同名 typedef - 且在初始化器解析之前
    // 就生效, 与旧的声明点一致.
    LibcpToken* name = decl_name.name;
    add_declared_name(ctx, name);

    LibcpInitializer* init = NULL;
    if (libcp_equal(tok, "=")) {
      LibcpToken* eq = tok;
      init           = init_record(ctx, &tok, tok->next);
      init->eq_tok   = eq;
    }

    LibcpNode* decl = libcp_new_node(ctx, ND_DECL, name);
    decl->ty_op     = ty;
    decl->name_tok  = name;
    if (attr)
      decl->attr = *attr;
    decl->decl_init = init;
    cur = cur->next = decl;
  }

  *rest = tok->next;
  return head.next;
}

// 指定初始化器 (designator): `[5]` 把初始化游标移到第 5 个元素, 之后
// 按顺序继续; `[5][8]` 这样的嵌套是允许的; `.fieldname` 把游标移到
// 结构体的成员. 未指明的元素初始化为零.
// 指定符按源码原样记录: 下标表达式不求值, 成员名不绑定; sema 针对
// 目标类型求值并施加它们.
static LibcpInitDesig* designators(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  LibcpInitDesig head = {};
  LibcpInitDesig* cur = &head;

  while (1) {
    if (libcp_equal(tok, "[")) {
      LibcpInitDesig* desig = libcp_arena_calloc(ctx, 1, sizeof(LibcpInitDesig));
      desig->tok            = tok;
      desig->begin          = libcp_conditional(ctx, &tok, tok->next);
      desig->after_begin    = tok;
      if (libcp_equal(tok, "..."))
        desig->end = libcp_conditional(ctx, &tok, tok->next);
      desig->rbracket = tok;
      tok             = libcp_skip(ctx, tok, "]");
      cur = cur->next = desig;
      continue;
    }

    if (libcp_equal(tok, ".")) {
      LibcpInitDesig* desig = libcp_arena_calloc(ctx, 1, sizeof(LibcpInitDesig));
      desig->tok            = tok;
      tok                   = tok->next;
      if (tok->kind != TK_IDENT)
        libcp_error_tok(ctx, tok, "expected a field designator");
      desig->name = tok;
      tok         = tok->next;
      cur = cur->next = desig;
      continue;
    }

    break;
  }

  *rest = tok;
  return head.next;
}

// 初始化器记录: 花括号列表, 字符串字面量或赋值表达式.
// 记录是忠实的: 元素保留源码写法 (带花括号的子列表, 字符串字面量或
// 表达式), 这里不做任何需要目标类型的决定. 花括号省略, 字符串展开,
// 指定符施加以及柔性数组成员定长, 全部在 sema 中完成.
static LibcpInitializer* init_record(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  if (++ctx->parse_depth > MAX_PARSE_DEPTH)
    libcp_error_tok(ctx, tok, "nesting too deep");
  LibcpInitializer* node = init_record1(ctx, rest, tok);
  ctx->parse_depth--;
  return node;
}

static LibcpInitializer* init_record1(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  LibcpInitializer* rec = libcp_arena_calloc(ctx, 1, sizeof(LibcpInitializer));
  rec->tok              = tok;

  if (libcp_equal(tok, "{")) {
    rec->kind = INIT_LIST;
    tok       = tok->next;

    LibcpInitItem head = {};
    LibcpInitItem* cur = &head;
    bool first         = true;

    while (!consume_end(&tok, tok)) {
      LibcpInitItem* item = libcp_arena_calloc(ctx, 1, sizeof(LibcpInitItem));
      if (!first) {
        item->comma_tok = tok;
        tok             = libcp_skip(ctx, tok, ",");
      }
      first = false;

      item->desigs = designators(ctx, &tok, tok);
      if (item->desigs && libcp_equal(tok, "="))
        tok = tok->next;
      item->init = init_record(ctx, &tok, tok);
      cur = cur->next = item;
    }

    rec->items = head.next;
    *rest      = tok;
    return rec;
  }

  // 字符串字面量只有作为整个初始化器时才是字符串初始化器. 若后面还跟
  // 着运算符或后缀, 该字面量只是普通表达式的开头 (`"ab" + 1`,
  // `"8.6" ? 0 : 1`, `"ab"[0]`).
  if (tok->kind == TK_STR && (libcp_equal(tok->next, ",") || libcp_equal(tok->next, "}") ||
                              libcp_equal(tok->next, ";") || tok->next->kind == TK_EOF)) {
    rec->kind    = INIT_STR;
    rec->str_tok = tok;
    *rest        = tok->next;
    return rec;
  }

  rec->kind = INIT_EXPR;
  rec->expr = assign(ctx, rest, tok);
  return rec;
}

// 判断给定 token 是否表示一个类型.
static bool is_typename(LibcpContext* ctx, LibcpToken* tok) {
  LibcpHashMap* map = &ctx->typename_map;

  // 类型关键字表惰性灌入 context 的 typename_map; 这里的 static 数组
  // 只是只读常量, 不承载语义状态.
  if (map->capacity == 0) {
    static char* kw[] = {
        "void",     "_Bool",    "char",       "short",         "int",       "long",
        "struct",   "union",    "typedef",    "enum",          "static",    "extern",
        "_Alignas", "signed",   "unsigned",   "const",         "volatile",  "auto",
        "register", "restrict", "__restrict", "__restrict__",  "_Noreturn", "float",
        "double",   "typeof",   "inline",     "_Thread_local", "__thread",  "_Atomic",
    };

    for (int i = 0; i < sizeof(kw) / sizeof(*kw); i++)
      libcp_hashmap_put(ctx, map, kw[i], (void*)1);
  }

  return libcp_hashmap_get2(ctx, map, tok->loc, tok->len) || find_typedef(ctx, tok);
}

// 内联汇编语句: 只接受一个字符串字面量, 可带 volatile / inline 限定.
static LibcpNode* asm_stmt(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  LibcpNode* node = libcp_new_node(ctx, ND_ASM, tok);
  tok             = tok->next;

  while (libcp_equal(tok, "volatile") || libcp_equal(tok, "inline"))
    tok = tok->next;

  tok = libcp_skip(ctx, tok, "(");
  if (tok->kind != TK_STR || tok->ty->base->kind != TY_CHAR)
    libcp_error_tok(ctx, tok, "expected string literal");
  node->asm_str = tok->str;
  *rest         = libcp_skip(ctx, tok->next, ")");
  return node;
}

// 语句: 上述各形式按 token 分派, 每条语句构造一个对应的 AST 节点;
// 需要类型信息的部分 (如 return 的隐式转换) 留给 sema.
static LibcpNode* stmt(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  if (libcp_equal(tok, "return")) {
    // 到返回类型的隐式转换属于 sema: 解析器不知道外层函数.
    LibcpNode* node = libcp_new_node(ctx, ND_RETURN, tok);
    if (libcp_consume(rest, tok->next, ";"))
      return node;

    node->lhs = expr(ctx, &tok, tok->next);
    *rest     = libcp_skip(ctx, tok, ";");
    return node;
  }

  if (libcp_equal(tok, "if")) {
    LibcpNode* node = libcp_new_node(ctx, ND_IF, tok);
    tok             = libcp_skip(ctx, tok->next, "(");
    node->cond      = expr(ctx, &tok, tok);
    tok             = libcp_skip(ctx, tok, ")");
    node->then      = stmt(ctx, &tok, tok);
    if (libcp_equal(tok, "else"))
      node->els = stmt(ctx, &tok, tok->next);
    *rest = tok;
    return node;
  }

  if (libcp_equal(tok, "switch")) {
    LibcpNode* node = libcp_new_node(ctx, ND_SWITCH, tok);
    tok             = libcp_skip(ctx, tok->next, "(");
    node->cond      = expr(ctx, &tok, tok);
    tok             = libcp_skip(ctx, tok, ")");
    node->then      = stmt(ctx, rest, tok);
    return node;
  }

  if (libcp_equal(tok, "case")) {
    // case 标签代表的值是常量表达式, 因此不求值地记录; sema 在解析节点
    // 时把它们求值到 begin/end. 范围检查的锚点是 `:`.
    LibcpNode* node  = libcp_new_node(ctx, ND_CASE, tok);
    node->begin_expr = libcp_conditional(ctx, &tok, tok->next);

    if (libcp_equal(tok, "...")) {
      // [GNU] case 范围, 例如 "case 1 ... 5:".
      node->end_expr = libcp_conditional(ctx, &tok, tok->next);
    }

    node->colon_tok = tok;
    tok             = libcp_skip(ctx, tok, ":");
    node->lhs       = stmt(ctx, rest, tok);
    return node;
  }

  if (libcp_equal(tok, "default")) {
    LibcpNode* node  = libcp_new_node(ctx, ND_CASE, tok);
    node->is_default = true;
    tok              = libcp_skip(ctx, tok->next, ":");
    node->lhs        = stmt(ctx, rest, tok);
    return node;
  }

  if (libcp_equal(tok, "for")) {
    LibcpNode* node = libcp_new_node(ctx, ND_FOR, tok);
    tok             = libcp_skip(ctx, tok->next, "(");

    enter_scope(ctx);

    if (is_typename(ctx, tok)) {
      LibcpNode* specs  = NULL;
      LibcpType* basety = declspec(ctx, &tok, tok, NULL, &specs);
      // for-init 的 declspec 定义的枚举记录与 init 共享作用域, 因此挂在
      // init 链头部. init 与其他声明一样是声明记录链; 记录下降后 sema
      // 把它折叠成一条语句, 因为 codegen 的 `for` 只有一个单语句的 init
      // 槽位.
      node->init        = specs_then(specs, declaration(ctx, &tok, tok, basety, NULL));
    } else {
      node->init = expr_stmt(ctx, &tok, tok);
    }

    if (!libcp_equal(tok, ";"))
      node->cond = expr(ctx, &tok, tok);
    tok = libcp_skip(ctx, tok, ";");

    if (!libcp_equal(tok, ")"))
      node->inc = expr(ctx, &tok, tok);
    tok = libcp_skip(ctx, tok, ")");

    node->then = stmt(ctx, rest, tok);

    leave_scope(ctx);
    return node;
  }

  if (libcp_equal(tok, "while")) {
    LibcpNode* node = libcp_new_node(ctx, ND_WHILE, tok);
    tok             = libcp_skip(ctx, tok->next, "(");
    node->cond      = expr(ctx, &tok, tok);
    tok             = libcp_skip(ctx, tok, ")");
    node->then      = stmt(ctx, rest, tok);
    return node;
  }

  if (libcp_equal(tok, "do")) {
    LibcpNode* node = libcp_new_node(ctx, ND_DO, tok);
    node->then      = stmt(ctx, &tok, tok->next);

    tok        = libcp_skip(ctx, tok, "while");
    tok        = libcp_skip(ctx, tok, "(");
    node->cond = expr(ctx, &tok, tok);
    tok        = libcp_skip(ctx, tok, ")");
    *rest      = libcp_skip(ctx, tok, ";");
    return node;
  }

  if (libcp_equal(tok, "asm"))
    return asm_stmt(ctx, rest, tok);

  if (libcp_equal(tok, "goto")) {
    if (libcp_equal(tok->next, "*")) {
      // [GNU] `goto *ptr` 跳转到 `ptr` 指定的地址.
      LibcpNode* node = libcp_new_node(ctx, ND_GOTO_EXPR, tok);
      node->lhs       = expr(ctx, &tok, tok->next->next);
      *rest           = libcp_skip(ctx, tok, ";");
      return node;
    }

    LibcpNode* node = libcp_new_node(ctx, ND_GOTO, tok);
    node->label     = libcp_get_ident(ctx, tok->next);
    *rest           = libcp_skip(ctx, tok->next->next, ";");
    return node;
  }

  if (libcp_equal(tok, "break")) {
    *rest = libcp_skip(ctx, tok->next, ";");
    return libcp_new_node(ctx, ND_BREAK, tok);
  }

  if (libcp_equal(tok, "continue")) {
    *rest = libcp_skip(ctx, tok->next, ";");
    return libcp_new_node(ctx, ND_CONTINUE, tok);
  }

  if (tok->kind == TK_IDENT && libcp_equal(tok->next, ":")) {
    LibcpNode* node = libcp_new_node(ctx, ND_LABEL, tok);
    node->label     = libcp_arena_strndup(ctx, tok->loc, tok->len);
    node->lhs       = stmt(ctx, rest, tok->next->next);
    return node;
  }

  if (libcp_equal(tok, "{"))
    return compound_stmt(ctx, rest, tok->next);

  return expr_stmt(ctx, rest, tok);
}

// 复合语句: 不产生独立语句的声明记录 (typedef, 枚举常量, 块作用域的
// extern 声明以及 [GNU] 嵌套函数定义) 在各自的源码位置进入链中; sema
// 在 codegen 看到链之前会把它们移除.
static LibcpNode* compound_stmt(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  LibcpNode* node      = libcp_new_node(ctx, ND_BLOCK, tok);
  node->is_scope_block = true;
  LibcpNode head       = {};
  LibcpNode* cur       = &head;

  enter_scope(ctx);

  while (!libcp_equal(tok, "}")) {
    // `ident:` 是标签语句而不是声明, 因此 typedef 形式的标签不按声明解析.
    if (is_typename(ctx, tok) && !libcp_equal(tok->next, ":")) {
      LibcpVarAttr attr = {};
      LibcpNode* specs  = NULL;
      LibcpType* basety = declspec(ctx, &tok, tok, &attr, &specs);
      cur               = chain_append(cur, specs);

      if (attr.is_typedef) {
        cur = chain_append(cur, parse_typedef(ctx, &tok, tok, basety));
        continue;
      }

      if (is_function(ctx, tok)) {
        cur = cur->next = function_def(ctx, &tok, tok, basety, &attr);
        continue;
      }

      if (attr.is_extern) {
        cur = chain_append(cur, global_variable(ctx, &tok, tok, basety, &attr));
        continue;
      }

      cur = chain_append(cur, declaration(ctx, &tok, tok, basety, &attr));
    } else {
      cur = cur->next = stmt(ctx, &tok, tok);
    }
  }

  leave_scope(ctx);

  node->body = head.next;
  *rest      = tok->next;
  return node;
}

// 表达式语句: 单个 `;` 产生一个空 ND_BLOCK 节点.
static LibcpNode* expr_stmt(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  if (libcp_equal(tok, ";")) {
    *rest = tok->next;
    return libcp_new_node(ctx, ND_BLOCK, tok);
  }

  LibcpNode* node = libcp_new_node(ctx, ND_EXPR_STMT, tok);
  node->lhs       = expr(ctx, &tok, tok);
  *rest           = libcp_skip(ctx, tok, ";");
  return node;
}

// 结构体成员: LibcpMember 的放置 (偏移, 大小, 对齐, 位域宽度) 与柔性
// 数组成员的转换属于 sema; 这里的成员列表只是语法形态, `_Alignas` 与
// 位域宽度都不求值地记录.
static void struct_members(LibcpContext* ctx,
                           LibcpToken** rest,
                           LibcpToken* tok,
                           LibcpType* ty,
                           LibcpNode** specs) {
  LibcpMember head = {};
  LibcpMember* cur = &head;
  int idx          = 0;

  while (!libcp_equal(tok, "}")) {
    LibcpVarAttr attr      = {};
    LibcpToken* decl_start = tok;
    LibcpType* basety      = declspec(ctx, &tok, tok, &attr, specs);
    bool first             = true;

    // 匿名成员按基类型的种类识别. `typeof(expr)` 基类型尚未确定种类,
    // 因此占位记录也算候选; 若最终不是聚合类型, sema 会拒绝无名成员.
    if ((basety->kind == TY_STRUCT || basety->kind == TY_UNION || basety->kind == TY_TYPEOF) &&
        libcp_consume(&tok, tok, ";")) {
      LibcpMember* mem = libcp_arena_calloc(ctx, 1, sizeof(LibcpMember));
      mem->tok         = decl_start;
      mem->ty          = basety;
      mem->idx         = idx++;
      mem->align_ty    = attr.align_ty;
      mem->align_expr  = attr.align_expr;
      cur = cur->next = mem;
      continue;
    }

    // 普通结构体成员: 逗号分隔的每个声明符产生一个成员记录.
    while (!libcp_consume(&tok, tok, ";")) {
      if (!first)
        tok = libcp_skip(ctx, tok, ",");
      first = false;

      LibcpMember* mem   = libcp_arena_calloc(ctx, 1, sizeof(LibcpMember));
      mem->tok           = tok;
      LibcpDeclName decl = {};
      mem->ty            = declarator(ctx, &tok, tok, basety, &decl);
      mem->name          = decl.name;
      mem->idx           = idx++;
      mem->align_ty      = attr.align_ty;
      mem->align_expr    = attr.align_expr;

      if (libcp_consume(&tok, tok, ":")) {
        mem->is_bitfield = true;
        // 位域宽度是常量表达式; 这里不求值地记录, sema 在布局聚合时求值.
        mem->width_expr  = libcp_conditional(ctx, &tok, tok);
      }

      cur = cur->next = mem;
    }
  }

  *rest       = tok->next;
  ty->members = head.next;
}

// 属性: 给定类型记录时 `packed` 与 `aligned` 会被施加; 已知的声明属性
// 与函数属性只识别, 不施加.
static LibcpToken* skip_attribute_args(LibcpContext* ctx, LibcpToken* tok) {
  if (!libcp_equal(tok, "("))
    return tok;

  int depth = 0;
  for (; tok->kind != TK_EOF; tok = tok->next) {
    if (libcp_equal(tok, "(")) {
      depth++;
    } else if (libcp_equal(tok, ")")) {
      if (--depth == 0)
        return tok->next;
    }
  }
  libcp_error_tok(ctx, tok, "unclosed attribute");
  return tok;
}

// 无类型含义的声明/函数属性名: 识别后连同参数一起跳过.
static bool is_ignored_attribute(LibcpToken* tok) {
  static char* names[] = {
      "unused",
      "used",
      "noreturn",
      "format",
      "format_arg",
      "visibility",
      "deprecated",
      "warn_unused_result",
      "nonnull",
      "returns_nonnull",
      "malloc",
      "pure",
      "const",
      "always_inline",
      "noinline",
      "section",
      "weak",
      "alias",
      "constructor",
      "destructor",
      "may_alias",
      "mode",
      "fallthrough",
      "nodiscard",
      "access",
      "alloc_size",
      "hot",
      "cold",
      "gnu_inline",
      "sentinel",
      "nothrow",
      "leaf",
      "returns_twice",
      "artificial",
      "error",
      "warning",
      NULL,
  };
  for (int i = 0; names[i]; i++)
    if (libcp_equal(tok, names[i]))
      return true;
  return false;
}

static LibcpToken* attribute_list(LibcpContext* ctx, LibcpToken* tok, LibcpType* ty) {
  while (libcp_consume(&tok, tok, "__attribute__")) {
    tok = libcp_skip(ctx, tok, "(");
    tok = libcp_skip(ctx, tok, "(");

    bool first = true;

    while (!libcp_consume(&tok, tok, ")")) {
      if (!first)
        tok = libcp_skip(ctx, tok, ",");
      first = false;

      if (libcp_consume(&tok, tok, "packed")) {
        if (ty)
          ty->is_packed = true;
        continue;
      }

      if (libcp_consume(&tok, tok, "aligned")) {
        // 对齐值是常量表达式. 只有属性挂在类型定义上时才施加; 声明位置
        // 传 NULL, 使共享的类型记录保持只读.
        if (libcp_consume(&tok, tok, "(")) {
          if (ty)
            ty->align_expr = libcp_conditional(ctx, &tok, tok);
          else
            (void)libcp_conditional(ctx, &tok, tok);
          tok = libcp_skip(ctx, tok, ")");
        }
        continue;
      }

      // cleanup 属性不在支持范围: 告警后跳过它的参数.
      if (libcp_equal(tok, "cleanup")) {
        libcp_warn_tok(ctx, tok, "cleanup attribute is ignored");
        tok = skip_attribute_args(ctx, tok->next);
        continue;
      }

      if (is_ignored_attribute(tok)) {
        tok = skip_attribute_args(ctx, tok->next);
        continue;
      }

      libcp_error_tok(ctx, tok, "unknown attribute");
    }

    tok = libcp_skip(ctx, tok, ")");
  }

  return tok;
}

// struct/union 说明符: 可带属性与 tag; 带花括号时是定义, 否则是引用
// 或前向声明.
static LibcpType*
    struct_union_decl(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpNode** specs) {
  LibcpType* ty = libcp_struct_type(ctx);
  tok           = attribute_list(ctx, tok, ty);

  // 读 tag.
  LibcpToken* tag = NULL;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }
  if (tag)
    ty->tag_tok = tag;

  if (tag && !libcp_equal(tok, "{")) {
    *rest = tok;

    LibcpType* ty2 = find_tag(ctx, tag);
    if (ty2)
      return ty2;

    ty->size = -1;
    push_tag_scope(ctx, tag, ty);
    return ty;
  }

  tok = libcp_skip(ctx, tok, "{");

  // 构造 struct 对象. 成员列表只是语法; 放置成员是 sema 的工作, 在类型
  // 补全时进行 - 裸定义就在定义处, 否则推迟到第一个解析出引用该类型的
  // 声明.
  struct_members(ctx, &tok, tok, ty, specs);
  *rest              = attribute_list(ctx, tok, ty);
  ty->layout_pending = true;

  LibcpType* ret = ty;
  if (tag) {
    // 若是重定义, 覆盖先前的类型; 否则登记新的 struct 类型.
    LibcpType* ty2 = find_current_tag(ctx, tag);
    if (ty2) {
      // 前向声明 (size 为 -1) 在这里补全. 同一作用域内第二次定义同一
      // tag 是约束违规 (C11 6.7.2.3p1), 不做静默替换.
      if (ty2->size >= 0)
        libcp_error_tok(ctx, tag, "redefinition of tag");
      *ty2 = *ty;
      ret  = ty2;
    } else {
      push_tag_scope(ctx, tag, ty);
    }
  }

  return ret;
}

// struct 说明符: 复用公共形态构建, 只把种类标为 TY_STRUCT.
static LibcpType*
    struct_decl(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpNode** specs) {
  LibcpType* ty = struct_union_decl(ctx, rest, tok, specs);
  ty->kind      = TY_STRUCT;
  return ty;
}

// union 说明符: 与 struct 共用形态, 只把种类标为 TY_UNION.
static LibcpType*
    union_decl(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpNode** specs) {
  LibcpType* ty = struct_union_decl(ctx, rest, tok, specs);
  ty->kind      = TY_UNION;
  return ty;
}

// 创建表示结构体成员访问的节点, 例如 foo.bar (foo 是结构体, bar 是
// 成员名). 匿名 struct 的成员属于外层结构体的成员命名空间, 因此示例中
// 匿名 struct 的成员 "a" 可以用 "x.a" 访问.
// 一个名字指代哪个成员不是语法问题, 因此节点保持未绑定: 它只携带成员名
// token, 对于 `x->y` 还在 `arrow_tok` 中携带 `->` token. sema 查找成员
// (沿途展开匿名成员), 检查操作数, 并重新插入解引用.
static LibcpNode*
    struct_ref(LibcpContext* ctx, LibcpNode* node, LibcpToken* tok, LibcpToken* arrow) {
  LibcpNode* mem = libcp_new_unary(ctx, ND_MEMBER, node, tok);
  mem->arrow_tok = arrow;
  return mem;
}

// 函数调用: 被调者检查, 实参转换与结构体返回缓冲区都属于 sema; 节点
// 保持忠实, 锚定在右括号上, 实参个数诊断就指向这里.
static LibcpNode* funcall(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpNode* fn) {
  LibcpNode head = {};
  LibcpNode* cur = &head;

  while (!libcp_equal(tok, ")")) {
    if (cur != &head)
      tok = libcp_skip(ctx, tok, ",");

    cur = cur->next = assign(ctx, &tok, tok);
  }

  *rest = libcp_skip(ctx, tok, ")");

  LibcpNode* node = libcp_new_unary(ctx, ND_FUNCALL, fn, tok);
  node->args      = head.next;
  return node;
}

// 控制表达式的类型选中哪个关联不是语法问题, 因此选择留给 sema: 节点
// 在 `cond` 中保存控制表达式, 在 `args` 中为每个关联保存一个
// ND_GENERIC_ASSOC.
static LibcpNode* generic_selection(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  LibcpNode* node = libcp_new_node(ctx, ND_GENERIC, tok);

  tok        = libcp_skip(ctx, tok, "(");
  node->cond = assign(ctx, &tok, tok);

  LibcpNode head = {};
  LibcpNode* cur = &head;

  while (!libcp_consume(rest, tok, ")")) {
    tok = libcp_skip(ctx, tok, ",");

    LibcpNode* assoc = libcp_new_node(ctx, ND_GENERIC_ASSOC, tok);

    // "default:" 关联是唯一不携带类型名的关联.
    if (libcp_equal(tok, "default")) {
      tok = libcp_skip(ctx, tok->next, ":");
    } else {
      LibcpNode* specs  = NULL;
      assoc->ty_op      = typename(ctx, &tok, tok, &specs);
      assoc->spec_decls = specs;
      tok               = libcp_skip(ctx, tok, ":");
    }
    assoc->lhs = assign(ctx, &tok, tok);

    cur = cur->next = assoc;
  }

  node->args = head.next;
  return node;
}

// 基本表达式: 语句表达式, 加括号的表达式, sizeof/_Alignof 的两种形式,
// _Generic, 两个内建查询, 以及标识符, 字符串字面量与数值字面量.
static LibcpNode* primary(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  LibcpToken* start = tok;

  if (libcp_equal(tok, "(") && libcp_equal(tok->next, "{")) {
    // 这是 GNU 语句表达式.
    LibcpNode* node = libcp_new_node(ctx, ND_STMT_EXPR, tok);
    node->body      = compound_stmt(ctx, &tok, tok->next->next)->body;
    *rest           = libcp_skip(ctx, tok, ")");
    return node;
  }

  if (libcp_equal(tok, "(")) {
    LibcpNode* node = expr(ctx, &tok, tok->next);
    *rest           = libcp_skip(ctx, tok, ")");
    return node;
  }

  if (libcp_equal(tok, "sizeof") && libcp_equal(tok->next, "(") &&
      is_typename(ctx, tok->next->next)) {
    LibcpNode* specs = NULL;
    LibcpType* ty    = typename(ctx, &tok, tok->next->next, &specs);
    *rest            = libcp_skip(ctx, tok, ")");

    // sizeof 保持忠实, 由 sema 折叠成它的值.
    LibcpNode* node  = libcp_new_node(ctx, ND_SIZEOF, start);
    node->ty_op      = ty;
    node->spec_decls = specs;
    return node;
  }

  if (libcp_equal(tok, "sizeof")) {
    // sizeof 保持忠实, 由 sema 折叠成它的值.
    LibcpNode* node = libcp_new_node(ctx, ND_SIZEOF, tok);
    node->lhs       = unary(ctx, rest, tok->next);
    return node;
  }

  if (libcp_equal(tok, "_Alignof") && libcp_equal(tok->next, "(") &&
      is_typename(ctx, tok->next->next)) {
    LibcpNode* specs = NULL;
    LibcpType* ty    = typename(ctx, &tok, tok->next->next, &specs);
    *rest            = libcp_skip(ctx, tok, ")");

    LibcpNode* node  = libcp_new_node(ctx, ND_ALIGNOF, start);
    node->ty_op      = ty;
    node->spec_decls = specs;
    return node;
  }

  if (libcp_equal(tok, "_Alignof")) {
    LibcpNode* node = libcp_new_node(ctx, ND_ALIGNOF, tok);
    node->lhs       = unary(ctx, rest, tok->next);
    return node;
  }

  if (libcp_equal(tok, "_Generic"))
    return generic_selection(ctx, rest, tok->next);

  if (libcp_equal(tok, "__builtin_types_compatible_p")) {
    // 两个类型操作数都被记录; sema 把该节点折叠成 0 或 1.
    LibcpNode* node  = libcp_new_node(ctx, ND_TYPES_COMPATIBLE, start);
    LibcpNode* specs = NULL;
    tok              = libcp_skip(ctx, tok->next, "(");
    node->ty_op      = typename(ctx, &tok, tok, &specs);
    tok              = libcp_skip(ctx, tok, ",");
    node->ty_op2     = typename(ctx, &tok, tok, &specs);
    node->spec_decls = specs;
    *rest            = libcp_skip(ctx, tok, ")");
    return node;
  }

  if (libcp_equal(tok, "__builtin_reg_class")) {
    // sema 把该节点折叠成其类型操作数的寄存器类别.
    LibcpNode* node  = libcp_new_node(ctx, ND_REG_CLASS, start);
    LibcpNode* specs = NULL;
    tok              = libcp_skip(ctx, tok->next, "(");
    node->ty_op      = typename(ctx, &tok, tok, &specs);
    node->spec_decls = specs;
    *rest            = libcp_skip(ctx, tok, ")");
    return node;
  }

  if (libcp_equal(tok, "__builtin_compare_and_swap")) {
    LibcpNode* node = libcp_new_node(ctx, ND_CAS, tok);
    tok             = libcp_skip(ctx, tok->next, "(");
    node->cas_addr  = assign(ctx, &tok, tok);
    tok             = libcp_skip(ctx, tok, ",");
    node->cas_old   = assign(ctx, &tok, tok);
    tok             = libcp_skip(ctx, tok, ",");
    node->cas_new   = assign(ctx, &tok, tok);
    *rest           = libcp_skip(ctx, tok, ")");
    return node;
  }

  if (libcp_equal(tok, "__builtin_atomic_exchange")) {
    LibcpNode* node = libcp_new_node(ctx, ND_EXCH, tok);
    tok             = libcp_skip(ctx, tok->next, "(");
    node->lhs       = assign(ctx, &tok, tok);
    tok             = libcp_skip(ctx, tok, ",");
    node->rhs       = assign(ctx, &tok, tok);
    *rest           = libcp_skip(ctx, tok, ")");
    return node;
  }

  if (tok->kind == TK_IDENT) {
    // 对变量, 函数或枚举常量的引用. 它指代哪一个不是语法问题, 因此引用
    // 保持忠实, 由 sema 的解析 pass 利用树结构提供的作用域栈绑定.
    LibcpNode* node = libcp_new_node(ctx, ND_IDENT, tok);
    *rest           = tok->next;
    return node;
  }

  if (tok->kind == TK_STR) {
    // 字面量保持忠实; sema 在标注节点类型时把它下降为对一个匿名全局
    // 对象的引用.
    LibcpNode* node = libcp_new_node(ctx, ND_STRING, tok);
    *rest           = tok->next;
    return node;
  }

  // 数值字面量按是否浮点分成两条构造路径; 类型由 token 带下来.
  if (tok->kind == TK_NUM) {
    LibcpNode* node;
    if (libcp_is_flonum(tok->ty)) {
      node       = libcp_new_node(ctx, ND_NUM, tok);
      node->fval = tok->fval;
    } else {
      node = libcp_new_num(ctx, tok->val, tok);
    }

    node->ty = tok->ty;
    *rest    = tok->next;
    return node;
  }

  libcp_error_tok(ctx, tok, "expected an expression");
}

// Pratt 表达式解析器 (PLAN-pratt-parse): 用一个循环取代按优先级分层的
// 级联调用. infix_bp() 把当前 token 转成运算符与绑定力, expr_bp() 在
// 运算符结合得比 min_bp 更紧时持续消费 (左结合 rbp == lbp, 右结合
// rbp == lbp - 1). 三个入口保持旧名字与旧含义: expr 允许逗号, assign
// 允许赋值, libcp_conditional 两者都不允许. 前缀形式在 expr_nud 中,
// 后缀形式是下面循环的 led 部分.
enum {
  BP_COMMA      = 1,
  BP_ASSIGN     = 2,
  BP_COND       = 3,
  BP_LOGOR      = 4,
  BP_LOGAND     = 5,
  BP_BITOR      = 6,
  BP_BITXOR     = 7,
  BP_BITAND     = 8,
  BP_EQUALITY   = 9,
  BP_RELATIONAL = 10,
  BP_SHIFT      = 11,
  BP_ADD        = 12,
  BP_MUL        = 13,
  BP_CAST       = 14,
  BP_POSTFIX    = 15,
};

// 绑定力表: 每个中缀拼写一行. 逗号行左结合 (rbp == lbp), 赋值行右结合
// (rbp == lbp - 1), ?: 在 expr_bp 中有自己的分支. 这里线性扫描就够:
// 拼写不到四十个, 而且不是热点循环.
typedef struct {
  char* op;
  LibcpNodeKind kind;
  LibcpNodeKind assign_op;
  int lbp;
  int rbp;
} LibcpInfixOp;

static const LibcpInfixOp infix_ops[] = {
    {",",   ND_COMMA,  ND_NULL_EXPR, BP_COMMA,      BP_COMMA     },
    {"=",   ND_ASSIGN, ND_NULL_EXPR, BP_ASSIGN,     BP_ASSIGN - 1},
    {"+=",  ND_ASSIGN, ND_ADD,       BP_ASSIGN,     BP_ASSIGN - 1},
    {"-=",  ND_ASSIGN, ND_SUB,       BP_ASSIGN,     BP_ASSIGN - 1},
    {"*=",  ND_ASSIGN, ND_MUL,       BP_ASSIGN,     BP_ASSIGN - 1},
    {"/=",  ND_ASSIGN, ND_DIV,       BP_ASSIGN,     BP_ASSIGN - 1},
    {"%=",  ND_ASSIGN, ND_MOD,       BP_ASSIGN,     BP_ASSIGN - 1},
    {"&=",  ND_ASSIGN, ND_BITAND,    BP_ASSIGN,     BP_ASSIGN - 1},
    {"|=",  ND_ASSIGN, ND_BITOR,     BP_ASSIGN,     BP_ASSIGN - 1},
    {"^=",  ND_ASSIGN, ND_BITXOR,    BP_ASSIGN,     BP_ASSIGN - 1},
    {"<<=", ND_ASSIGN, ND_SHL,       BP_ASSIGN,     BP_ASSIGN - 1},
    {">>=", ND_ASSIGN, ND_SHR,       BP_ASSIGN,     BP_ASSIGN - 1},
    {"?",   ND_COND,   ND_NULL_EXPR, BP_COND,       BP_COND - 1  },
    {"||",  ND_LOGOR,  ND_NULL_EXPR, BP_LOGOR,      BP_LOGOR     },
    {"&&",  ND_LOGAND, ND_NULL_EXPR, BP_LOGAND,     BP_LOGAND    },
    {"|",   ND_BITOR,  ND_NULL_EXPR, BP_BITOR,      BP_BITOR     },
    {"^",   ND_BITXOR, ND_NULL_EXPR, BP_BITXOR,     BP_BITXOR    },
    {"&",   ND_BITAND, ND_NULL_EXPR, BP_BITAND,     BP_BITAND    },
    {"==",  ND_EQ,     ND_NULL_EXPR, BP_EQUALITY,   BP_EQUALITY  },
    {"!=",  ND_NE,     ND_NULL_EXPR, BP_EQUALITY,   BP_EQUALITY  },
    {"<",   ND_LT,     ND_NULL_EXPR, BP_RELATIONAL, BP_RELATIONAL},
    {"<=",  ND_LE,     ND_NULL_EXPR, BP_RELATIONAL, BP_RELATIONAL},
    {">",   ND_GT,     ND_NULL_EXPR, BP_RELATIONAL, BP_RELATIONAL},
    {">=",  ND_GE,     ND_NULL_EXPR, BP_RELATIONAL, BP_RELATIONAL},
    {"<<",  ND_SHL,    ND_NULL_EXPR, BP_SHIFT,      BP_SHIFT     },
    {">>",  ND_SHR,    ND_NULL_EXPR, BP_SHIFT,      BP_SHIFT     },
    {"+",   ND_ADD,    ND_NULL_EXPR, BP_ADD,        BP_ADD       },
    {"-",   ND_SUB,    ND_NULL_EXPR, BP_ADD,        BP_ADD       },
    {"*",   ND_MUL,    ND_NULL_EXPR, BP_MUL,        BP_MUL       },
    {"/",   ND_DIV,    ND_NULL_EXPR, BP_MUL,        BP_MUL       },
    {"%",   ND_MOD,    ND_NULL_EXPR, BP_MUL,        BP_MUL       },
};

// 查 tok 处的中缀运算符 (若有): 返回节点种类, 复合赋值运算符 (普通 "="
// 或非赋值运算符为 ND_NULL_EXPR) 以及左右绑定力.
static bool
    infix_bp(LibcpToken* tok, LibcpNodeKind* kind, LibcpNodeKind* assign_op, int* lbp, int* rbp) {
  int n = sizeof(infix_ops) / sizeof(*infix_ops);

  for (int i = 0; i < n; i++) {
    if (!libcp_equal(tok, infix_ops[i].op))
      continue;
    *kind      = infix_ops[i].kind;
    *assign_op = infix_ops[i].assign_op;
    *lbp       = infix_ops[i].lbp;
    *rbp       = infix_ops[i].rbp;
    return true;
  }
  return false;
}

static LibcpNode* expr_bp(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, int min_bp);

// 前缀 (nud) 形式: cast 或复合字面量 (通过 `(` 加类型名前瞻判定), 一元
// 运算符, labels-as-values, 其余原子表达式交给共享的 primary. 一元 `+`
// 是透明的, 不建节点, 与级联实现一致.
static LibcpNode* expr_nud(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  if (libcp_equal(tok, "(") && is_typename(ctx, tok->next)) {
    LibcpToken* start = tok;
    LibcpNode* specs  = NULL;
    LibcpType* ty     = typename(ctx, &tok, tok->next, &specs);
    tok               = libcp_skip(ctx, tok, ")");

    // 复合字面量: 保持忠实, 类型与初始化器记录挂在节点上; sema 物化该
    // 对象.
    if (libcp_equal(tok, "{")) {
      LibcpNode* node  = libcp_new_node(ctx, ND_COMPOUND_LITERAL, start);
      node->ty_op      = ty;
      node->spec_decls = specs;
      node->decl_init  = init_record(ctx, rest, tok);
      return node;
    }

    // 源码写出的 cast: 无类型, 目标类型记录下来交给 sema. is_implicit
    // 保持 false (sema 插入的 cast 由 libcp_new_cast 置位).
    LibcpNode* node  = libcp_new_node(ctx, ND_CAST, start);
    node->lhs        = expr_bp(ctx, rest, tok, BP_CAST);
    node->ty_op      = ty;
    node->spec_decls = specs;
    return node;
  }

  if (libcp_equal(tok, "+"))
    return expr_bp(ctx, rest, tok->next, BP_CAST);

  if (libcp_equal(tok, "-"))
    return libcp_new_unary(ctx, ND_NEG, expr_bp(ctx, rest, tok->next, BP_CAST), tok);

  if (libcp_equal(tok, "&"))
    return libcp_new_unary(ctx, ND_ADDR, expr_bp(ctx, rest, tok->next, BP_CAST), tok);

  if (libcp_equal(tok, "*"))
    return libcp_new_unary(ctx, ND_DEREF, expr_bp(ctx, rest, tok->next, BP_CAST), tok);

  if (libcp_equal(tok, "!"))
    return libcp_new_unary(ctx, ND_NOT, expr_bp(ctx, rest, tok->next, BP_CAST), tok);

  if (libcp_equal(tok, "~"))
    return libcp_new_unary(ctx, ND_BITNOT, expr_bp(ctx, rest, tok->next, BP_CAST), tok);

  if (libcp_equal(tok, "++")) {
    LibcpNode* node = libcp_new_node(ctx, ND_INCDEC, tok);
    node->lhs       = expr_bp(ctx, rest, tok->next, BP_CAST);
    node->addend    = 1;
    return node;
  }

  if (libcp_equal(tok, "--")) {
    LibcpNode* node = libcp_new_node(ctx, ND_INCDEC, tok);
    node->lhs       = expr_bp(ctx, rest, tok->next, BP_CAST);
    node->addend    = -1;
    return node;
  }

  // [GNU] labels-as-values. 必须在任何中缀解释之前判断, 因此 `&&label`
  // 不会被当作二元 `&&`.
  if (libcp_equal(tok, "&&")) {
    LibcpNode* node = libcp_new_node(ctx, ND_LABEL_VAL, tok);
    node->label     = libcp_get_ident(ctx, tok->next);
    *rest           = tok->next->next;
    return node;
  }

  return primary(ctx, rest, tok);
}

// 唯一的表达式循环. 深度门只在这里计数; 下面的入口包装不再增加一层.
static LibcpNode* expr_bp(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, int min_bp) {
  if (++ctx->parse_depth > MAX_PARSE_DEPTH)
    libcp_error_tok(ctx, tok, "nesting too deep");

  LibcpNode* node = expr_nud(ctx, rest, tok);

  for (;;) {
    LibcpToken* op = *rest;
    LibcpNodeKind kind;
    LibcpNodeKind assign_op;
    int lbp;
    int rbp;

    // 后缀 (led) 形式结合得比所有中缀运算符都紧: 调用, 下标, 成员访问与
    // 后缀自增/自减.
    if (min_bp < BP_POSTFIX) {
      if (libcp_equal(op, "(")) {
        node = funcall(ctx, rest, op->next, node);
        continue;
      }

      if (libcp_equal(op, "[")) {
        LibcpToken* idx      = op->next;
        LibcpNode* subscript = libcp_new_node(ctx, ND_SUBSCRIPT, op);
        subscript->lhs       = node;
        subscript->rhs       = expr(ctx, &idx, idx);
        *rest                = libcp_skip(ctx, idx, "]");
        node                 = subscript;
        continue;
      }

      if (libcp_equal(op, ".")) {
        node  = struct_ref(ctx, node, op->next, NULL);
        *rest = op->next->next;
        continue;
      }

      if (libcp_equal(op, "->")) {
        node  = struct_ref(ctx, node, op->next, op);
        *rest = op->next->next;
        continue;
      }

      if (libcp_equal(op, "++")) {
        LibcpNode* incdec = libcp_new_node(ctx, ND_INCDEC, op);
        incdec->lhs       = node;
        incdec->is_post   = true;
        incdec->addend    = 1;
        node              = incdec;
        *rest             = op->next;
        continue;
      }

      if (libcp_equal(op, "--")) {
        LibcpNode* incdec = libcp_new_node(ctx, ND_INCDEC, op);
        incdec->lhs       = node;
        incdec->is_post   = true;
        incdec->addend    = -1;
        node              = incdec;
        *rest             = op->next;
        continue;
      }
    }

    if (!infix_bp(op, &kind, &assign_op, &lbp, &rbp) || lbp <= min_bp)
      break;

    if (kind == ND_COND) {
      LibcpNode* cond = libcp_new_node(ctx, ND_COND, op);

      if (libcp_equal(op->next, ":")) {
        // [GNU] `a ?: b`: 只有 cond 与 els, 并置上 is_elvis.
        cond->is_elvis = true;
        cond->cond     = node;
        cond->els      = expr_bp(ctx, rest, op->next->next, BP_COND - 1);
        node           = cond;
        continue;
      }

      LibcpToken* mid   = NULL;
      cond->cond        = node;
      cond->then        = expr_bp(ctx, &mid, op->next, 0);
      LibcpToken* colon = libcp_skip(ctx, mid, ":");
      cond->els         = expr_bp(ctx, rest, colon, BP_COND - 1);
      node              = cond;
      continue;
    }

    if (kind == ND_ASSIGN) {
      LibcpNode* rhs    = expr_bp(ctx, rest, op->next, rbp);
      LibcpNode* assign = libcp_new_binary(ctx, ND_ASSIGN, node, rhs, op);
      assign->op        = assign_op;
      node              = assign;
      continue;
    }

    node = libcp_new_binary(ctx, kind, node, expr_bp(ctx, rest, op->next, rbp), op);
  }

  ctx->parse_depth--;
  return node;
}

// primary 的 `sizeof expr` 与 `_Alignof expr` 形式通过 `unary` 这个名字
// 解析操作数; 在这里它就是 cast 层.
static LibcpNode* unary(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  return expr_bp(ctx, rest, tok, BP_CAST);
}

static LibcpNode* expr(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  return expr_bp(ctx, rest, tok, 0);
}

static LibcpNode* assign(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  return expr_bp(ctx, rest, tok, BP_ASSIGN - 1);
}

LibcpNode* libcp_conditional(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  return expr_bp(ctx, rest, tok, BP_COND - 1);
}

// 把 typedef 声明符解析成 ND_TYPEDEF 记录. 名字同时立即进入 typedef 名
// 判定器: 文法需要它来分类下一个标识符.
static LibcpNode*
    parse_typedef(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok, LibcpType* basety) {
  LibcpNode head = {};
  LibcpNode* cur = &head;
  bool first     = true;

  while (!libcp_consume(&tok, tok, ";")) {
    if (!first)
      tok = libcp_skip(ctx, tok, ",");
    first = false;

    LibcpDeclName decl = {};
    LibcpType* ty      = declarator(ctx, &tok, tok, basety, &decl);
    if (!decl.name)
      libcp_error_tok(ctx, decl.name_pos, "typedef name omitted");

    LibcpNode* node = libcp_new_node(ctx, ND_TYPEDEF, decl.name);
    node->ty        = ty;
    add_typedef(ctx, node);
    cur = cur->next = node;
  }

  *rest = tok;
  return head.next;
}

// 把函数声明符及其后的函数体解析成 ND_FUNCDEF 记录. sema 声明该函数
// (与先前的声明比对, 锚点为 `tok`) 并分析函数体.
static LibcpNode* function_def(LibcpContext* ctx,
                               LibcpToken** rest,
                               LibcpToken* tok,
                               LibcpType* basety,
                               LibcpVarAttr* attr) {
  LibcpDeclName decl = {};
  LibcpType* ty      = declarator(ctx, &tok, tok, basety, &decl);
  if (!decl.name)
    libcp_error_tok(ctx, decl.name_pos, "function name omitted");

  LibcpNode* node = libcp_new_node(ctx, ND_FUNCDEF, tok);
  node->ty_op     = ty;
  node->name_tok  = decl.name;
  node->attr      = *attr;
  add_declared_name(ctx, decl.name);

  // 只有声明符而没有函数体: 原型声明, 发完记录直接返回.
  if (libcp_consume(&tok, tok, ";")) {
    *rest = tok;
    return node;
  }

  // 参数在整个函数体内可见, 因此从这时起遮蔽同名 typedef; 作用域在函数体
  // 自身作用域之上, 与旧的 begin_function 登记位置一致.
  enter_scope(ctx);
  for (LibcpType* p = ty->params; p; p = p->next)
    if (p->name)
      add_declared_name(ctx, p->name);

  tok        = libcp_skip(ctx, tok, "{");
  node->body = compound_stmt(ctx, &tok, tok);
  leave_scope(ctx);
  *rest = tok;
  return node;
}

// 把全局变量声明符 (文件作用域, 或块内的 `extern` 声明) 解析成
// ND_GVAR_DECL 记录; sema 声明这些对象并序列化它们的初始化器.
static LibcpNode* global_variable(LibcpContext* ctx,
                                  LibcpToken** rest,
                                  LibcpToken* tok,
                                  LibcpType* basety,
                                  LibcpVarAttr* attr) {
  LibcpNode head = {};
  LibcpNode* cur = &head;
  bool first     = true;

  while (!libcp_consume(&tok, tok, ";")) {
    if (!first)
      tok = libcp_skip(ctx, tok, ",");
    first = false;

    LibcpDeclName decl = {};
    LibcpType* ty      = declarator(ctx, &tok, tok, basety, &decl);
    if (!decl.name)
      libcp_error_tok(ctx, decl.name_pos, "variable name omitted");

    LibcpToken* name = decl.name;
    add_declared_name(ctx, name);

    LibcpNode* node = libcp_new_node(ctx, ND_GVAR_DECL, tok);
    node->ty_op     = ty;
    node->name_tok  = name;
    node->attr      = *attr;
    if (libcp_equal(tok, "="))
      node->decl_init = init_record(ctx, &tok, tok->next);
    cur = cur->next = node;
  }

  *rest = tok;
  return head.next;
}

// 前瞻若干 token, 判断它们是否构成函数定义或函数声明.
static bool is_function(LibcpContext* ctx, LibcpToken* tok) {
  if (libcp_equal(tok, ";"))
    return false;

  LibcpType dummy = {};
  LibcpType* ty   = declarator(ctx, &tok, tok, &dummy, NULL);
  return ty->kind == TY_FUNC;
}

// 顶层: 返回的声明记录链就是 sema 按源码顺序遍历的对象, 用来声明对象并
// 分析函数体.
LibcpNode* libcp_parse(LibcpContext* ctx, LibcpToken* tok) {
  ENTER_RET(ctx, NULL);
  libcp_parse_init(ctx);
  LibcpNode head = {};
  LibcpNode* cur = &head;

  while (tok->kind != TK_EOF) {
    LibcpVarAttr attr = {};
    LibcpNode* specs  = NULL;
    LibcpType* basety = declspec(ctx, &tok, tok, &attr, &specs);
    // declspec 途中产生的枚举记录先入链, 保持在源码中的位置.
    cur               = chain_append(cur, specs);

    // typedef 声明
    if (attr.is_typedef) {
      cur = chain_append(cur, parse_typedef(ctx, &tok, tok, basety));
      continue;
    }

    // 函数定义或声明
    if (is_function(ctx, tok)) {
      cur = cur->next = function_def(ctx, &tok, tok, basety, &attr);
      continue;
    }

    // 全局变量
    cur = chain_append(cur, global_variable(ctx, &tok, tok, basety, &attr));
  }

  LEAVE(ctx);
  return head.next;
}
