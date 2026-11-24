# 06 第 3 层: 忠实语法 AST

## 6.1 作用与位置

第 3 层把预处理后的 token 流变成语法树. 它的关键词是**忠实**:

- 保留源码的每一种写法区别.
- 不做名字绑定.
- 不做类型计算.
- 不做常量求值.
- 不做任何为后端服务的整形.

输入: `libcp_preprocess` 的输出.
输出: `LibcpNode` 树, 顶层是声明记录链.
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
| `x += 1` | `ND_ASSIGN` 且 `assign.op == ND_ADD` | 读改写 |
| `x++` | `ND_INCDEC`, `incdec.is_post = true` | 复合赋值 |
| `while (c) ...` | `ND_WHILE` | `ND_FOR` |
| `sizeof(T)` | `ND_SIZEOF` 且 `ty_op = T` | `ND_NUM` |
| `int a[2] = {1, 2}` | `ND_DECL` + `LibcpInitializer` | 赋值链 |
| `"abc"` | `ND_STRING` | `ND_VAR` 指向匿名全局 |
| `p->x` | `ND_MEMBER` 且 `access.arrow_tok` 非 NULL | `ND_DEREF` + `ND_MEMBER` |
| `a ?: b` | `ND_COND` 且 `ctrl.is_elvis = true` | 完整 `?:` |

隐式 cast 也有标记: 第 3 层不插入隐式 cast; 第 4 层插入时设 `value.is_implicit = true`. 源码写出的 cast 是 `value.is_implicit = false`.

### 6.2.2 声明是记录, 不是语句

`int x = 1;` 在语法树里是一个 `ND_DECL` 记录节点:

- `ty_op`: 声明类型, 可能还有待补全的数组维度.
- `decl.name_tok`: 声明的标识符 token.
- `decl.attr`: 存储类与对齐属性.
- `decl.decl_init`: 忠实的初始化器记录, 或 NULL. 它在 parse 后有效; sema 构建 `decl.init_resolved` 后把它置 NULL.

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

除此之外不做任何名字解析. 变量引用是 `ND_IDENT`, 不带绑定字段; 函数调用不检查声明; 成员访问不绑定成员.

这个 oracle 由解析器自己维护, 与第 4 层的作用域是两套独立结构. 第 4 层从树结构重建作用域, 不依赖解析器的栈.

### 6.2.4 待补全的类型记录

解析器会构造 `LibcpType`, 但有些部分它不完成:

- 数组维度: 未定长度在解析器里记为 `array_len = -1`, 维度表达式放在 `dim_len`, 等第 4 层求值后写回真实长度.
- `typeof(expr)`: 操作数放在 `typeof_expr`, 等第 4 层标注.
- `_Alignas(<type>)`: 对齐类型放在 `align_expr` / `align_ty`, 等第 4 层计算.
- 位域宽度: 表达式放在 `LibcpMember.width_expr`, 等第 4 层求值.
- 聚合类型的成员布局: `layout_pending` 标记, 等第 4 层调用 `layout_struct` / `layout_union`.

这些字段就是"语法到语义"的交接点. 消费者如果直接读类型, 必须知道哪些字段可能还没填.

### 6.2.5 宏调用不建节点

第 3 层的输入是展开后的 token 流, 因此树上没有宏调用. 一个宏展开成 12 个 token, 树上就是这 12 个 token 构成的语法结构.

宏的原始写法由第 2 层保留, 宏到产物的对应由 provenance 提供. 这个分工在第 1 章已经说明.

## 6.3 数据模型

### 6.3.1 69 种节点

`LibcpNodeKind` 共 69 种. 按用途分组:

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

第 3 层的解析器实际产出 65 种. 计数方法: 把 `libcp_node_new` / `libcp_node_unary` / `libcp_node_binary` 直接命名的 46 种, 与 `infix_ops[].kind` 的 21 种合并, 去掉重复的 `ND_ASSIGN` 与 `ND_COND`, 得到 65. 不产出的四种:

- `ND_VAR`: 第 4 层绑定名字时产生.
- `ND_VLA_PTR`: 第 5 层整形遍展开 VLA 声明时产生; `sema.c` 只提供构造器 `libcp_node_vla_ptr`, 不在第 4 层生成该节点.
- `ND_MEMZERO`: 第 5 层展开初始化器时产生.
- `ND_NULL_EXPR`: 第 5 层整形遍生成的空表达式占位; 解析器只在 `infix_ops` 表里把它当作 `assign_op` 的哨兵, 不建该节点.

### 6.3.2 LibcpNode

公共 `LibcpNode` 由公共头和字段族 union 两部分组成. 所有 kind 共享公共头;
每个 kind 只使用一个族. 访问形式是 `node->ctrl.cond`, `node->decl.attr`,
而不是 `node->cond`; `libcp_node_family(kind)` 返回 kind 所属的族.

```c
struct libcp_node {
  LibcpNodeKind kind;
  LibcpNode *next;
  LibcpType *ty;         /* 第 3 层大多为 NULL, 例外见正文 */
  LibcpToken *tok;       /* 代表 token */

  LibcpNode *lhs, *rhs;
  LibcpType *ty_op;      /* sizeof / cast / 声明记录的目标类型 */
  LibcpNode *spec_decls; /* declspec 中产生的枚举常量记录 */

  union {
    LibcpNodeCtrl ctrl;
    LibcpNodeDecl decl;
    LibcpNodeValue value;
    LibcpNodeCase case_;
    LibcpNodeAccess access;
    LibcpNodeJump jump;
    LibcpNodeAssign assign;
    LibcpNodeIncDec incdec;
    LibcpNodeAsm asm_;
    LibcpNodeCas cas;
  };
};
```

族与 kind 的对应:

| 族 | 字段 | kind |
|---|---|---|
| `ctrl` | `cond, then, els, init, inc, args, func_ty, generic_sel, is_elvis` | `ND_COND`, `ND_IF`, `ND_FOR`, `ND_WHILE`, `ND_DO`, `ND_SWITCH`, `ND_FUNCALL`, `ND_GENERIC` |
| `decl` | `body, is_scope_block, attr, name_tok, decl_init, init_resolved, var` | `ND_BLOCK`, `ND_STMT_EXPR`, `ND_FUNCDEF`, `ND_DECL`, `ND_GVAR_DECL`, `ND_COMPOUND_LITERAL`, `ND_VAR`, `ND_VLA_PTR`, `ND_STRING`, `ND_MEMZERO` |
| `value` | `val, fval, ty_op2, is_implicit` | `ND_NUM`, `ND_SIZEOF`, `ND_ALIGNOF`, `ND_TYPES_COMPATIBLE`, `ND_REG_CLASS`, `ND_CAST`, `ND_ENUM_CONST` |
| `case_` | `begin, end, is_default, begin_expr, end_expr, colon_tok` | `ND_CASE` |
| `access` | `member, arrow_tok` | `ND_MEMBER` |
| `jump` | `label` | `ND_GOTO`, `ND_LABEL`, `ND_LABEL_VAL` |
| `assign` | `op` | `ND_ASSIGN` |
| `incdec` | `is_post, addend` | `ND_INCDEC` |
| `asm_` | `asm_str` | `ND_ASM` |
| `cas` | `cas_addr, cas_old, cas_new` | `ND_CAS` |

只使用公共头的 kind: 二元/一元/下标/逗号/逻辑运算, `ND_RETURN`,
`ND_EXPR_STMT`, `ND_GOTO_EXPR`, `ND_NULL_EXPR`, `ND_BREAK`, `ND_CONTINUE`,
`ND_IDENT`, `ND_TYPEDEF`, `ND_GENERIC_ASSOC`, `ND_EXCH`. 这些节点调用
`libcp_node_family` 得到 `LIBCP_NF_NONE`.

规则:

- 通用 walker 用 `libcp_node_child_count` / `libcp_node_child_at` 下降,
  不要自己重写字段 switch; 只有需要族特有的锚点或链策略时才结合
  `libcp_node_family`. 读取非活动族是未定义行为, 不能依赖 calloc 的零值.
- `ty` 在第 3 层大多为 NULL. 不要假设有类型. 例外: `ND_NUM` 带词法类型,
  `ND_TYPEDEF` 带声明类型.
- 声明记录的 `decl.var` 在第 3 层为 NULL; `ND_IDENT` 不带绑定字段, 只有 `tok` 的拼写.
- `decl.init_resolved` 是 sema 输出的只读数据, sema 后有效到 reset. 第 3 层只需要 `decl.decl_init`; 需要忠实记录时在 sema 前读取, 或直接消费 CST.
- `tok` 是代表 token, 不是完整源码范围. 一个节点可能由多个 token 组成,
  但只记录一个锚点.
- `sizeof(LibcpNode)` 为 144 字节, 16 字节对齐; 节点仍是一次固定大小分配,
  `libcp_node_new` 不按 kind 改变分配尺寸.

子节点枚举 (`libcp_node_child_count` / `libcp_node_child_at`) 是只读的
通用下降接口, 不需要 `LibcpContext`:

```c
typedef struct {
  const LibcpNode *node;
  bool is_list;
} LibcpNodeChild;

int libcp_node_child_count(const LibcpNode *node);
LibcpNodeChild libcp_node_child_at(const LibcpNode *node, int index);
```

- `index` 是存在子节点的序号 `0..count-1`, 不是字段槽位; 越界返回
  `{NULL, false}`.
- 顺序固定为 `lhs`, `rhs`, 活动族的节点字段, `spec_decls`: ctrl 的
  `cond / then / els / init / inc / args`, decl 的 `body`, case 的
  `begin_expr / end_expr`, cas 的三个操作数. 不是源码顺序.
- `next` 是兄弟链接, 永远不是子节点. `decl.body`, `ctrl.init`,
  `ctrl.args` 与 `spec_decls` 是列表字段: 只返回头节点并置
  `is_list = true`, 调用者沿 `next` 访问兄弟. 枚举一个节点不会带出它的兄弟.
- 排除 `generic_sel` (某个关联结果的别名), `decl_init`,
  `init_resolved`, `attr.align_expr` 与类型载荷里的节点 (`ty->dim_len`,
  `ty->typeof_expr`, `ty->vla_len`, `member->width_expr` 等).
- 它回答 "下降哪些子节点, 顺序如何"; `libcp_node_family` 回答 "哪个
  union 载荷有效", 并继续服务锚点 token 收集.
- 树被修改后序号不稳定; 库内的 range 与 sema 三个 walker 共用这份枚举,
  各自的链与特例策略保留在各自的代码里.

### 6.3.3 类型

第 3 层会建 `LibcpType`:

- 基本类型是共享单例.
- 指针类型由 `pointers()` 构造; `type_suffix` 只解析函数参数表和数组后缀.
- 结构体/联合/枚举的成员列表在解析时收集, 布局留给第 4 层.
- 数组维度, typeof 操作数, 对齐, 位域宽度可能未完成.

`LibcpType.kind` 共 17 种, 包括 `TY_VLA` 和 `TY_TYPEOF`.

### 6.3.4 初始化器记录

```c
enum { INIT_LIST, INIT_EXPR, INIT_STR };

struct libcp_initializer {
  int kind;
  LibcpToken *tok;
  LibcpInitItem *items;   /* INIT_LIST */
  LibcpNode *expr;        /* INIT_EXPR */
  LibcpToken *str_tok;    /* INIT_STR */
  LibcpToken *eq_tok;     /* 顶层记录的 = */
};
```

`INIT_LIST` 的每个元素是一个 `LibcpInitItem`, 带逗号 token, 指示符链和值. 指示符 `LibcpInitDesig` 记录 `[expr]`, `[begin ... end]`, `.name` 三种形式, 索引表达式**未求值**.

这是"源码写法"的记录, 不是初始化树. 第 4 层把它解析成 `LibcpInitTree`.

## 6.4 实现流程

### 6.4.1 递归下降

`parse.c` 是一个递归下降解析器. 每个函数对应一条产生式, 以 `LibcpContext *ctx` 为首参, 通过 `rest` 出参返回剩余 token 链:

```c
static LibcpNode *stmt(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok);
static LibcpNode *
    declaration(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok, LibcpType *basety,
                LibcpVarAttr *attr);
static LibcpNode *assign(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok);
static LibcpNode *expr(LibcpContext *ctx, LibcpToken **rest, LibcpToken *tok);
```

声明层:

- `declspec`: 解析类型说明符和存储类, 填 `LibcpVarAttr`.
- `declarator`: 解析声明符, 用 `LibcpDeclName` 载体携带标识符, 不写进共享类型记录.
- `type_suffix`: 解析数组和函数后缀.
- `struct_decl` / `union_decl` / `enum_specifier`: 解析聚合类型和枚举.
- `init_record` / `init_record1`: 解析初始化器记录.

语句层:

- `compound_stmt`: `{ ... }`, 建 `ND_BLOCK`, `is_scope_block = true`.
- `stmt`: `if`, `for`, `while`, `do`, `switch`, `case`, `default`, 标签, `goto`, `break`, `continue`, `return`, `asm`, 空语句, 表达式语句.

表达式层是一个绑定力循环, 见下一节.

### 6.4.2 表达式解析: 一个绑定力循环

表达式层由一个 `expr_bp` 绑定力循环构成, 覆盖前缀, cast, 后缀, 二元, 赋值, `?:` 和逗号:

- `infix_bp()` 把当前 token 翻译成操作符与绑定力(只读 `LibcpInfixOp` 表 `infix_ops` + 线性查找).
- 左结合 `rbp == lbp`, 右结合 `rbp == lbp - 1`; 具名 `BP_*` 常量.
- 前缀形式在 `expr_nud`, 后缀形式是主循环的 led 段.
- 三个入口保持名字与语义: `expr` 允许逗号, `assign` 允许赋值, `libcp_conditional` 两者都不允许.

历史上这里曾是与 C 优先级级联(每优先级一个函数)并存的双路径
(`LIBCP_PRATT_PARSE_FEATURE` 开关, 见 `docs/12` §12.11); 2026-10-25 的 Pratt 单一路径线删掉了级联,
Pratt 成为唯一实现, 开关与双构建一并移除. 迁移前的 41 个语料文件与
tinycc 真实构建曾由 `test/pratt-ab.sh` 锁定两路径 `-dump-ast` 与 `-S`
逐字节一致 (该脚本已随单一路径线删除), 迁移后由汇编快照基线继续锁定.

### 6.4.3 声明记录

`declaration` 按上下文产出不同记录:

- 块内普通变量: `ND_DECL`.
- 文件作用域变量, 或块内 `extern`: `ND_GVAR_DECL`.
- 函数定义: `ND_FUNCDEF`, `decl.body` 非 NULL.
- 函数声明: `ND_FUNCDEF`, `decl.body` 为 NULL.
- `typedef`: `ND_TYPEDEF`.
- 枚举常量: `ND_ENUM_CONST`.
- 复合字面量: `ND_COMPOUND_LITERAL`, 出现在表达式位置.

这些记录直接挂在语句链或顶层链上. 第 3 层不把它们转成语句. 第 4 层消费它们, 第 5 层决定哪些记录变成发射语句, 哪些被摘除.

### 6.4.4 声明符与名字

一个历史问题是: 声明符把名字写进共享的类型记录, 会污染基本类型单例和 typedef 类型. 修复后的做法是:

- 声明符把名字放在 `LibcpDeclName` 载体里.
- 声明记录节点保存 `decl.name_tok`.
- 类型记录上的 `name` / `name_pos` 只在函数参数的 per-parameter 副本上填写, 供 sema 读.
- 共享类型记录永远不写名字.

这条规则对消费者很重要: 不要指望 `LibcpNode.ty->name` 是声明的名字. 用 `LibcpNode.decl.name_tok`, 或第 4 层 `LibcpObj.name`.

### 6.4.5 错误与深度

- 语法错误经诊断回调报告, 公共入口返回 NULL.
- 错误不恢复. 第一个错误就 longjmp 回入口.
- 递归深度由 `MAX_PARSE_DEPTH` (512) 限制. 超深嵌套报诊断, 不栈溢出.
- 空翻译单元返回 NULL, `failed` 为 false.

第 3 层只保留文法可判的诊断. 类型错误, 未定义名字, 参数个数不符等都在第 4 层. 整个库的诊断套件当前锁定 68 例, 由 `test/diagnostic.sh` 逐字节校验, 解析器的文法诊断只是其中一部分.

## 6.5 阶段产物

产物是一棵 `LibcpNode` 树:

- 顶层是声明记录链, 以 `NULL` 结尾.
- 函数体是 `ND_BLOCK`, 内含语句链.
- 表达式节点不做类型计算, 名字没有绑定, 常量没有求值.
- 声明记录保留, 初始化器是记录而不是树.
- 每个节点有代表 token; 子树范围可用 `libcp_node_source_range` 取锚点 token 的极值, 但它不跟随 `next` 兄弟链, 也不深入初始化器树.

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
- `ND_IDENT` 只有拼写, 没有绑定字段.
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
LibcpToken *tok = libcp_tokenize_file(ctx, "input.c");
tok = libcp_preprocess(ctx, tok);
LibcpNode *ast = libcp_parse(ctx, tok);

if (!ast && libcp_context_failed(ctx)) {
  /* 语法错误 */
}
if (!ast && !libcp_context_failed(ctx)) {
  /* 空翻译单元 */
}
```

### 6.7.2 遍历顶层

```c
for (LibcpNode *n = ast; n; n = n->next) {
  switch (n->kind) {
  case ND_FUNCDEF:
    printf("function %.*s, body=%s\n",
           n->decl.name_tok->len, n->decl.name_tok->loc,
           n->decl.body ? "yes" : "no");
    break;
  case ND_GVAR_DECL:
    printf("global %.*s\n", n->decl.name_tok->len, n->decl.name_tok->loc);
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
static void dump_expr(LibcpNode *n) {
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
if (n->kind == ND_DECL && n->decl.decl_init) {
  LibcpInitializer *init = n->decl.decl_init;
  if (init->kind == INIT_LIST) {
    for (LibcpInitItem *it = init->items; it; it = it->next) {
      /* it->desigs, it->init */
    }
  }
}
```

设计符里的索引表达式没有求值. 要值就自己调用第 4 层, 或用 `libcp_sema` 的结论.

`sema` 之后 `decl.decl_init` 为 NULL: 这个记录是第 3 层形态, 只在 sema 前可读. 解析后的形态在 `decl.init_resolved`, 见 `docs/07-layer4-sema.md` 7.3.3.

### 6.7.5 配置参数

`libcp_parse` 没有额外参数. `libcp_parse_init` 每次调用前重建解析器的作用域栈; `parse_depth` 也存放在 context 里, 但它只在 `libcp_context_reset` 的 `clear_state` 中清零. 解析出错后要复用 context 必须先 `libcp_context_reset`.

## 6.8 边界与已知限制

- **没有类型计算**. 大多数 `LibcpNode.ty` 在第 3 层为 NULL. 例外: `ND_NUM` 带词法类型, `ND_TYPEDEF` 带声明类型. 显式 cast 和声明记录的目标类型在 `ty_op`.
- **没有名字绑定**. `ND_IDENT` 不携带绑定字段; 第 4 层把变量/函数引用改写成 `ND_VAR`, 绑定在 `decl.var`; 枚举常量改写成 `ND_NUM`, 值在 `value.val`.
- **没有常量求值**. `sizeof` / `_Alignof` / `_Generic` 只有形式, 没有结论.
- **声明记录不是语句**. 消费者必须处理 `ND_DECL` / `ND_GVAR_DECL` / `ND_FUNCDEF` / `ND_TYPEDEF` / `ND_ENUM_CONST`. 第 4 层保留它们, 第 5 层摘除.
- **初始化器是记录, 不是树**. 指示符未求值, 大括号省略未应用, 灵活数组成员未定尺寸.
- **没有完整源码范围**. 每个节点只有代表 token; 需要子树范围时调用 `libcp_node_source_range`, 它按锚点 token 的 (文件, 偏移) 取最小 / 最大值. 坐标与物理行由内部 source 模块 (3.4.2) 统一换算, 不依赖 `line_no - line_delta`. `next` 链是兄弟节点, 不会被跟随; 初始化器树不会被深入.
- **没有错误恢复**. 第一个语法错误就返回. 想要"报多个错误"需要自己分段调用.
- **只接受预处理后的流**. 传原始 token 流会在 `#` 或宏名上报错.
- **宏调用不可见**. 树上只有展开后的结构. 来源查 provenance.
- **语法检查范围有限**. `test/diagnostic.sh` 的 A 组锁定 15 例文法诊断; 当前 `parse.c` 有 21 个 `libcp_error_tok` 调用点, 17 条不同消息. 类型错误在第 4 层.
- **深度上限 512**. 超深嵌套报诊断, 不返回部分树.
- **GNU 扩展支持不完整**. `typeof`, 语句表达式, `&&label`, 范围 case, `asm` 有节点, 但不是每个细节都与 GCC 一致.
