# 06 第 3 层: 忠实语法 AST

## 6.1 作用与位置

第 3 层把预处理后的 token 流变成语法树. 它的关键词是**忠实**:

- 保留源码的每一种写法区别.
- 不做名字绑定.
- 不做类型计算.
- 不做常量求值.
- 不做任何为后端服务的整形.

输入: `chibicc_preprocess` 的输出.
输出: `ChibiccNode` 树, 顶层是声明记录链.
消费者: C 转 C 工具, 语法级 linter, 重构工具, 以及第 4 层.

这一层是"语法"而不是"语义"的分界线. 一个只关心代码结构, 不关心类型的工具, 到这一层就够了.

## 6.2 设计思路

### 6.2.1 忠实于写法

编译器通常会在解析时顺手做很多简化:

- `a[i]` 改成 `*(a + i)`.
- `x += 1` 改成 `x = x + 1`.
- `a > b` 改成 `b < a`.
- `while` 改成 `for`.
- `sizeof(T)` 直接折成数字.
- 初始化器拍平成赋值链.
- 字符串字面量变成匿名全局的引用.

这些简化对代码生成有利, 对源到源工具是灾难: 工具再也分不清用户写的是哪种形式.

本库把这些简化全部推迟:

| 源码写法 | 第 3 层的节点 | 不变成 |
|---|---|---|
| `a[i]` | `ND_SUBSCRIPT` | `ND_DEREF(ND_ADD(a, i))` |
| `a > b` | `ND_GT` | `ND_LT(b, a)` |
| `x += 1` | `ND_ASSIGN` 且 `op == ND_ADD` | 读改写 |
| `x++` | `ND_INCDEC`, `is_post = true` | 复合赋值 |
| `while (c) ...` | `ND_WHILE` | `ND_FOR` |
| `sizeof(T)` | `ND_SIZEOF` 且 `ty_op = T` | `ND_NUM` |
| `int a[2] = {1, 2}` | `ND_DECL` + `ChibiccInitializer` | 赋值链 |
| `"abc"` | `ND_STRING` | `ND_VAR` 指向匿名全局 |
| `p->x` | `ND_MEMBER` 且 `arrow_tok` 非 NULL | `ND_DEREF` + `ND_MEMBER` |
| `a ?: b` | `ND_COND` 且 `is_elvis = true` | 完整 `?:` |

隐式 cast 也有标记: 第 3 层不插入隐式 cast; 第 4 层插入时设 `is_implicit = true`. 源码写出的 cast 是 `is_implicit = false`.

### 6.2.2 声明是记录, 不是语句

`int x = 1;` 在语法树里是一个 `ND_DECL` 记录节点:

- `ty_op`: 声明类型, 可能还有待补全的数组维度.
- `name_tok`: 声明的标识符 token.
- `attr`: 存储类与对齐属性.
- `decl_init`: 忠实的初始化器记录, 或 NULL.

它不立即变成"建变量 + 初始化"的语句. 变量什么时候建, 初始化怎么展开, 是第 4 层和第 5 层的事.

文件作用域的声明是 `ND_GVAR_DECL`, 函数定义是 `ND_FUNCDEF`, typedef 是 `ND_TYPEDEF`, 枚举常量是 `ND_ENUM_CONST`. 这五类记录统称声明记录.

### 6.2.3 解析器只保留文法必需的语义反馈

C 文法不是上下文无关的. 最典型的例子:

```c
(T)x
```

如果 `T` 是 typedef 名, 这是 cast; 如果是变量, 这是函数调用. 解析器必须知道答案.

因此第 3 层保留一个最小的语义 oracle:

- `typedef` 名字的作用域栈.
- `struct` / `union` / `enum` tag 的查找.

除此之外不做任何名字解析. 变量引用是 `ND_IDENT`, `var` 为 NULL; 函数调用不检查声明; 成员访问不绑定成员.

这个 oracle 由解析器自己维护, 与第 4 层的作用域是两套独立结构. 第 4 层从树结构重建作用域, 不依赖解析器的栈.

### 6.2.4 待补全的类型记录

解析器会构造 `ChibiccType`, 但有些部分它不完成:

- 数组维度: `array_len` 可能是 0, 维度表达式放在 `dim_len`, 等第 4 层求值.
- `typeof(expr)`: 操作数放在 `typeof_expr`, 等第 4 层标注.
- `_Alignas(<type>)`: 对齐类型放在 `align_expr` / `align_ty`, 等第 4 层计算.
- 位域宽度: 表达式放在 `Member.width_expr`, 等第 4 层求值.
- 聚合类型的成员布局: `layout_pending` 标记, 等第 4 层调用 `layout_struct` / `layout_union`.

这些字段就是"语法到语义"的交接点. 消费者如果直接读类型, 必须知道哪些字段可能还没填.

### 6.2.5 宏调用不建节点

第 3 层的输入是展开后的 token 流, 因此树上没有宏调用. 一个宏展开成 12 个 token, 树上就是这 12 个 token 构成的语法结构.

宏的原始写法由第 2 层保留, 宏到产物的对应由 provenance 提供. 这个分工在第 1 章已经说明.

## 6.3 数据模型

### 6.3.1 69 种节点

`ChibiccNodeKind` 共 69 种. 按用途分组:

| 组 | 节点 |
|---|---|
| 字面量与名字 | `ND_NUM`, `ND_STRING`, `ND_IDENT`, `ND_VAR`, `ND_VLA_PTR` |
| 一元 | `ND_NEG`, `ND_ADDR`, `ND_DEREF`, `ND_NOT`, `ND_BITNOT` |
| 二元算术, 位运算, 比较 | `ND_ADD`, `ND_SUB`, `ND_MUL`, `ND_DIV`, `ND_MOD`, `ND_BITAND`, `ND_BITOR`, `ND_BITXOR`, `ND_SHL`, `ND_SHR`, `ND_EQ`, `ND_NE`, `ND_LT`, `ND_LE`, `ND_GT`, `ND_GE` |
| 赋值与自增 | `ND_ASSIGN`, `ND_INCDEC` |
| 访问 | `ND_SUBSCRIPT`, `ND_MEMBER` |
| 条件与逗号 | `ND_COND`, `ND_COMMA` |
| 逻辑 | `ND_LOGAND`, `ND_LOGOR` |
| cast 与类型操作 | `ND_CAST`, `ND_SIZEOF`, `ND_ALIGNOF`, `ND_TYPES_COMPATIBLE`, `ND_REG_CLASS`, `ND_GENERIC`, `ND_GENERIC_ASSOC` |
| 调用与表达式语句 | `ND_FUNCALL`, `ND_STMT_EXPR`, `ND_NULL_EXPR`, `ND_EXPR_STMT` |
| 语句 | `ND_RETURN`, `ND_IF`, `ND_FOR`, `ND_WHILE`, `ND_DO`, `ND_SWITCH`, `ND_CASE`, `ND_BLOCK`, `ND_GOTO`, `ND_BREAK`, `ND_CONTINUE`, `ND_GOTO_EXPR`, `ND_LABEL`, `ND_LABEL_VAL`, `ND_ASM` |
| 原子 | `ND_CAS`, `ND_EXCH` |
| 声明记录 | `ND_DECL`, `ND_GVAR_DECL`, `ND_FUNCDEF`, `ND_TYPEDEF`, `ND_ENUM_CONST`, `ND_COMPOUND_LITERAL` |
| 内部 | `ND_MEMZERO` |

第 3 层的解析器实际产出 66 种. 不产出的三种:

- `ND_VAR`: 第 4 层绑定名字时产生.
- `ND_VLA_PTR`: 第 4 层处理 VLA 时产生.
- `ND_MEMZERO`: 第 5 层展开初始化器时产生.

### 6.3.2 ChibiccNode

公共 `ChibiccNode` 是一个大结构体, 所有节点共用字段. 不同种类只使用其中一部分. 主要字段:

```c
struct chibicc_node {
  ChibiccNodeKind kind;
  ChibiccNode *next;
  ChibiccType *ty;       /* 第 3 层大多为 NULL, 例外见正文 */
  ChibiccToken *tok;     /* 代表 token */

  ChibiccNode *lhs, *rhs;

  ChibiccNodeKind op;    /* ND_ASSIGN 的复合运算符, 0 表示 = */
  bool is_post;          /* ND_INCDEC: 后缀 */
  int addend;            /* ND_INCDEC: +1 或 -1 */

  ChibiccNode *cond, *then, *els, *init, *inc;
  ChibiccNode *body;
  bool is_scope_block;   /* ND_BLOCK 是否引入作用域 */

  ChibiccMember *member;
  ChibiccToken *arrow_tok; /* ND_MEMBER 的 ->, 普通 . 为 NULL */

  ChibiccType *func_ty;
  ChibiccNode *args;
  char *label;           /* goto / label 的名字 */
  bool is_elvis;

  ChibiccType *ty_op;    /* sizeof / cast / 声明记录的目标类型 */
  ChibiccType *ty_op2;
  ChibiccNode *generic_sel;
  bool is_implicit;      /* ND_CAST 是否是 sema 插入 */

  ChibiccVarAttr attr;
  ChibiccToken *name_tok;
  ChibiccNode *spec_decls;
  ChibiccInitializer *decl_init;
  void *init_resolved;   /* sema 私有, 消费者不要碰 */

  long begin, end;
  bool is_default;
  ChibiccNode *begin_expr, *end_expr;
  ChibiccToken *colon_tok;

  char *asm_str;
  ChibiccNode *cas_addr, *cas_old, *cas_new;
  ChibiccObj *atomic_addr;
  ChibiccNode *atomic_expr;

  ChibiccObj *var;
  int64_t val;
  long double fval;
};
```

规则:

- `ty` 在第 3 层大多为 NULL. 不要假设有类型. 例外: `ND_NUM` 带词法类型, `ND_TYPEDEF` 带声明类型.
- `var` 在第 3 层为 NULL. `ND_IDENT` 只有 `tok` 的拼写.
- `init_resolved` 是 sema 的私有载荷, 消费者不得读写.
- `tok` 是代表 token, 不是完整源码范围. 一个节点可能由多个 token 组成, 但只记录一个锚点.

### 6.3.3 类型

第 3 层会建 `ChibiccType`:

- 基本类型是共享单例.
- 指针, 数组, 函数类型由 `type_suffix` 构造.
- 结构体/联合/枚举的成员列表在解析时收集, 布局留给第 4 层.
- 数组维度, typeof 操作数, 对齐, 位域宽度可能未完成.

`ChibiccType.kind` 共 17 种, 包括 `TY_VLA` 和 `TY_TYPEOF`.

### 6.3.4 初始化器记录

```c
enum { INIT_LIST, INIT_EXPR, INIT_STR };

struct chibicc_initializer {
  int kind;
  ChibiccToken *tok;
  ChibiccInitItem *items;   /* INIT_LIST */
  ChibiccNode *expr;        /* INIT_EXPR */
  ChibiccToken *str_tok;    /* INIT_STR */
  ChibiccToken *eq_tok;     /* 顶层记录的 = */
};
```

`INIT_LIST` 的每个元素是一个 `ChibiccInitItem`, 带逗号 token, 指示符链和值. 指示符 `ChibiccInitDesig` 记录 `[expr]`, `[begin ... end]`, `.name` 三种形式, 索引表达式**未求值**.

这是"源码写法"的记录, 不是初始化树. 第 4 层把它解析成 `ChibiccInitTree`.

## 6.4 实现流程

### 6.4.1 递归下降

`parse.c` 是一个递归下降解析器. 每个函数对应一条产生式, 通过 `rest` 出参返回剩余 token 链:

```c
static Node *stmt(Token **rest, Token *tok);
static Node *declaration(Token **rest, Token *tok, Type *basety, VarAttr *attr);
static Node *assign(Token **rest, Token *tok);
static Node *expr(Token **rest, Token *tok);
```

声明层:

- `declspec`: 解析类型说明符和存储类, 填 `ChibiccVarAttr`.
- `declarator`: 解析声明符, 用 `DeclName` 载体携带标识符, 不写进共享类型记录.
- `type_suffix`: 解析数组和函数后缀.
- `struct_decl` / `union_decl` / `enum_specifier`: 解析聚合类型和枚举.
- `initializer` / `init_record`: 解析初始化器记录.

语句层:

- `compound_stmt`: `{ ... }`, 建 `ND_BLOCK`, `is_scope_block = true`.
- `stmt`: `if`, `for`, `while`, `do`, `switch`, `case`, `default`, 标签, `goto`, `break`, `continue`, `return`, `asm`, 空语句, 表达式语句.

表达式层有两条路径, 见下一节.

### 6.4.2 表达式解析的两种构建

默认构建使用 C 的优先级级联, 每个优先级一个函数:

```text
assign -> conditional -> logor -> logand -> bitor -> bitxor -> bitand
       -> equality -> relational -> shift -> add -> mul -> cast
       -> unary -> postfix -> primary
```

`CHIBICC_PRATT_PARSE_FEATURE=1` 时, 表达式层换成一个 `expr_bp` 绑定力循环, 覆盖前缀, cast, 后缀, 二元, 赋值, `?:` 和逗号. 旧函数保留在 `#if !CHIBICC_PRATT_PARSE_FEATURE` 下.

两种构建的公共 API 相同, 树的形状也相同. `test/pratt-ab.sh` 在 41 个语料文件和 tinycc 真实构建上比较 `-dump-ast` 与 `-S`, 要求逐字节一致. 消费者链接哪个库, 就得到哪个解析器; 默认 `libchibicc.a` 是旧路径.

### 6.4.3 声明记录

`declaration` 按上下文产出不同记录:

- 块内普通变量: `ND_DECL`.
- 文件作用域变量, 或块内 `extern`: `ND_GVAR_DECL`.
- 函数定义: `ND_FUNCDEF`, `body` 非 NULL.
- 函数声明: `ND_FUNCDEF`, `body` 为 NULL.
- `typedef`: `ND_TYPEDEF`.
- 枚举常量: `ND_ENUM_CONST`.
- 复合字面量: `ND_COMPOUND_LITERAL`, 出现在表达式位置.

这些记录直接挂在语句链或顶层链上. 第 3 层不把它们转成语句. 第 4 层消费它们, 第 5 层决定哪些记录变成发射语句, 哪些被摘除.

### 6.4.4 声明符与名字

一个历史问题是: 声明符把名字写进共享的类型记录, 会污染基本类型单例和 typedef 类型. 修复后的做法是:

- 声明符把名字放在 `DeclName` 载体里.
- 声明记录节点保存 `name_tok`.
- 类型记录上的 `name` / `name_pos` 只在函数参数的 per-parameter 副本上填写, 供 sema 读.
- 共享类型记录永远不写名字.

这条规则对消费者很重要: 不要指望 `Node.ty->name` 是声明的名字. 用 `Node.name_tok`, 或第 4 层 `Obj.name`.

### 6.4.5 错误与深度

- 语法错误经诊断回调报告, 公共入口返回 NULL.
- 错误不恢复. 第一个错误就 longjmp 回入口.
- 递归深度由 `MAX_PARSE_DEPTH` (512) 限制. 超深嵌套报诊断, 不栈溢出.
- 空翻译单元返回 NULL, `failed` 为 false.

第 3 层只保留文法可判的诊断. 类型错误, 未定义名字, 参数个数不符等都在第 4 层. 整个库的诊断套件当前锁定 68 例, 由 `test/diagnostic.sh` 逐字节校验, 解析器的文法诊断只是其中一部分.

## 6.5 阶段产物

产物是一棵 `ChibiccNode` 树:

- 顶层是声明记录链, 以 `NULL` 结尾.
- 函数体是 `ND_BLOCK`, 内含语句链.
- 表达式节点不做类型计算, 名字没有绑定, 常量没有求值.
- 声明记录保留, 初始化器是记录而不是树.
- 每个节点有代表 token, 但没有完整源码范围.

调试通道:

```sh
./chibicc -dump-ast input.c
```

输出示例:

```text
ND_FUNCDEF @input.c:1:14 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @input.c:1:16 "int" is_scope_block=1
    ND_DECL @input.c:1:20 "x" ty_op=int name="x" attr=none
      INIT kind=EXPR @input.c:1:24 eq=@input.c:1:22
        ND_ADD @input.c:1:26 "+"
          ND_IDENT @input.c:1:24 "a"
          ND_NUM @input.c:1:28 "1" val=1
    ND_RETURN @input.c:1:31 "return"
      ND_IDENT @input.c:1:38 "x"
```

注意:

- `ND_ADD` 没有 `ty`.
- `ND_IDENT` 只有拼写, 没有 `var`.
- `ND_NUM` 的 `val` 是词法阶段就有的字面值, 不是常量求值的结果.
- `INIT` 是初始化器记录, 不是赋值链.

`-dump-ast` 在 sema 之前早退, 所以它展示的正是第 3 层的形态.

## 6.6 产物可以做什么

- **C 转 C**: 遍历树, 按源码写法重新生成代码. 下标还是下标, `while` 还是 `while`, 复合赋值还是复合赋值.
- **语法级 lint**: 检查 `if` 缺少大括号, `switch` 缺少 `default`, 赋值出现在条件里, 等.
- **重构**: 重命名需要自己处理作用域; 第 3 层没有绑定, 所以更适合做不依赖绑定的结构变换.
- **代码度量**: 统计函数, 语句, 表达式, 嵌套深度.
- **教学**: 观察语法树与源码的对应.
- **作为第 4 层的输入**: 语义分析只在这棵树上做标注.

不能做:

- 不能回答"这个表达式是什么类型".
- 不能回答"这个标识符指向哪个声明".
- 不能求 `sizeof` 的值.
- 不能直接生成可执行代码, 因为没有类型和布局.
- 不能保留注释, 注释在 trivia 里, 不在 AST 里.

## 6.7 API 用法

### 6.7.1 解析

```c
ChibiccToken *tok = chibicc_tokenize_file(ctx, "input.c");
tok = chibicc_preprocess(ctx, tok);
ChibiccNode *ast = chibicc_parse(ctx, tok);

if (!ast && chibicc_context_failed(ctx)) {
  /* 语法错误 */
}
if (!ast && !chibicc_context_failed(ctx)) {
  /* 空翻译单元 */
}
```

### 6.7.2 遍历顶层

```c
for (ChibiccNode *n = ast; n; n = n->next) {
  switch (n->kind) {
  case ND_FUNCDEF:
    printf("function %.*s, body=%s\n",
           n->name_tok->len, n->name_tok->loc,
           n->body ? "yes" : "no");
    break;
  case ND_GVAR_DECL:
    printf("global %.*s\n", n->name_tok->len, n->name_tok->loc);
    break;
  case ND_TYPEDEF:
    printf("typedef\n");
    break;
  default:
    break;
  }
}
```

### 6.7.3 表达式

```c
static void dump_expr(ChibiccNode *n) {
  if (!n)
    return;
  printf("%d", (int)n->kind);
  if (n->tok)
    printf(" (%.*s)", n->tok->len, n->tok->loc);
  putchar('\n');
  dump_expr(n->lhs);
  dump_expr(n->rhs);
}
```

### 6.7.4 初始化器记录

```c
if (n->kind == ND_DECL && n->decl_init) {
  ChibiccInitializer *init = n->decl_init;
  if (init->kind == INIT_LIST) {
    for (ChibiccInitItem *it = init->items; it; it = it->next) {
      /* it->desigs, it->init */
    }
  }
}
```

设计符里的索引表达式没有求值. 要值就自己调用第 4 层, 或用 `chibicc_sema` 的结论.

### 6.7.5 配置参数

`chibicc_parse` 没有额外参数. 解析深度和作用域状态都在 context 里, 每次调用前由 `parse_init` 重建.

## 6.8 边界与已知限制

- **没有类型计算**. 大多数 `Node.ty` 在第 3 层为 NULL. 例外: `ND_NUM` 带词法类型, `ND_TYPEDEF` 带声明类型. 显式 cast 和声明记录的目标类型在 `ty_op`.
- **没有名字绑定**. `ND_IDENT.var` 为 NULL. 第 4 层才绑定.
- **没有常量求值**. `sizeof` / `_Alignof` / `_Generic` 只有形式, 没有结论.
- **声明记录不是语句**. 消费者必须处理 `ND_DECL` / `ND_GVAR_DECL` / `ND_FUNCDEF` / `ND_TYPEDEF` / `ND_ENUM_CONST`. 第 4 层保留它们, 第 5 层摘除.
- **初始化器是记录, 不是树**. 指示符未求值, 大括号省略未应用, 灵活数组成员未定尺寸.
- **没有源码范围**. 每个节点只有代表 token. 计算节点的 start / end 要自己遍历. 这是 L5.3 的缺口.
- **没有错误恢复**. 第一个语法错误就返回. 想要"报多个错误"需要自己分段调用.
- **只接受预处理后的流**. 传原始 token 流会在 `#` 或宏名上报错.
- **宏调用不可见**. 树上只有展开后的结构. 来源查 provenance.
- **语法检查范围有限**. 只有文法可判的 15 类诊断. 类型错误在第 4 层.
- **深度上限 512**. 超深嵌套报诊断, 不返回部分树.
- **两套表达式解析器**. 默认构建是级联, `CHIBICC_PRATT_PARSE_FEATURE=1` 是绑定力循环. 两者的 AST 由 A/B 测试锁定为相同, 但消费者必须知道自己链接的是哪一个构建.
- **GNU 扩展支持不完整**. `typeof`, 语句表达式, `&&label`, 范围 case, `asm` 有节点, 但不是每个细节都与 GCC 一致.
