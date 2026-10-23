# 05 第 2 层: lossless CST

## 5.1 作用与位置

第 2 层是具体语法树 (Concrete Syntax Tree). 它建立在**预处理之前**的 token 流上, 目标是:

- 每个 token 和标点都是显式叶子.
- 预处理指令是节点, 不是被消费掉的文本.
- `#if` 的每个分支都保留, 不求值.
- 叶子按序逐字节重建输入文件.
- 解析是全函数, 不报诊断, 不退出.

第 2 层服务的是要保留源码字节的工具: 格式化, 注释迁移, 局部重构, 源码索引, 预处理感知的编辑.

它与第 3 层不是"同一个东西的两种粒度". 第 3 层建立在展开后的流上, 没有注释, 没有指令, 没有未选中分支. 两者不能互相替代.

## 5.2 设计思路

### 5.2.1 为什么必须在预处理之前

预处理是不可逆的:

- 宏展开丢掉调用点拼写, 实参布局和 `#` / `##` 的合成过程.
- 条件编译丢掉未选中分支.
- `#include` 丢掉指令行, 文件名和文件边界.
- 指令行本身被消费, 不进入 output.

如果 CST 建在展开流上, 无论树形多细, 都无法重建原始文件. 因此 CST 的输入被固定在 `tokenize_file()` 的直接输出上.

这条边界也带来一个操作约束: `libcp_preprocess` 会破坏输入链, 所以 CST 必须在它之前建, 或者对同一个文件重新 tokenize. 公共头 `preprocess.h` 明确写了这一点.

### 5.2.2 全函数, 不报错

CST 解析器不诊断. 遇到文法覆盖不了的跨度, 它建一个 `CST_UNPARSED` 节点, 把已经切好的叶子按序放进去. 重建不依赖树的形状, 只依赖叶子序列, 所以"解析失败"不影响 lossless 契约.

这个设计让 CST 可以处理任意输入: 宏密集的头文件, 语法不完整的片段, 甚至不是合法 C 的文本. 工具不会因为一个不认识的构造就整体失败.

### 5.2.3 结构覆盖与字节保真分开评估

字节保真是硬契约: 每个语料文件都必须 round-trip. 结构覆盖是软目标: 树形越细, 工具越好用, 但库不承诺"每个 token 都在正确的语法节点里".

这两个指标必须分开看. `CST_UNPARSED` 不影响重建, 但会让依赖结构的工具退化. 第 11 章给出当前的覆盖统计.

### 5.2.4 文件内 typedef 预扫描

C 文法需要知道一个标识符是不是 typedef 名, 才能决定 `(T)x` 是 cast 还是函数调用. CST 不做完整语义分析, 但它需要一个 oracle.

实现是一个保守的预扫描 `cst_collect_typedefs`: 扫全文件, 识别几种常见 typedef 形式, 把名字收进集合. 识别范围:

```c
typedef int T;
typedef int *T;
typedef int T[3];
typedef int A, B;
typedef struct S T;
typedef int (*T)(int);
```

不识别参数名和 struct tag. 来自 include 的 typedef 不在文件内, 因此未知.

这是一个启发式, 不是 C 作用域:

- 名字在整个文件内可见, 即使 typedef 写在后面.
- 局部 typedef 也会被当成全局可见.
- 同名变量和 typedef 的遮蔽不处理.

预扫描只影响树形, 不影响重建. 判断错误时, CST 会回退到 `CST_UNPARSED` 或把 cast 当括号表达式, 但字节不丢.

## 5.3 数据模型

### 5.3.1 节点

```c
typedef struct libcp_cst_node LibcpCstNode;

struct libcp_cst_node {
  LibcpCstKind kind;
  LibcpToken *tok;          /* 叶子节点: CST_TOKEN / CST_EOF */
  LibcpCstNode **children;  /* 内部节点 */
  int child_count;
};
```

`children` 的容量不在公共结构里. 它在数组前有一个私有 header, 由 `cst.c` 管理. 消费者只能读 `children[0 .. child_count)`, 不能扩容或释放.

叶子节点的 `tok` 直接指向第 1 层的 token. **CST 不复制 token**. 因此 `libcp_cst_verify` 可以用指针相等比较叶子序列和 token 流.

### 5.3.2 55 种节点

按用途分组:

| 组 | 节点 |
|---|---|
| 结构与叶子 | `CST_TRANSLATION_UNIT`, `CST_TOKEN`, `CST_EOF`, `CST_GROUP_PAREN`, `CST_GROUP_BRACKET`, `CST_GROUP_BRACE`, `CST_BLOCK`, `CST_NULL_STMT`, `CST_UNPARSED` |
| 表达式 | `CST_EXPR_STMT`, `CST_BINARY`, `CST_UNARY`, `CST_POSTFIX`, `CST_CAST`, `CST_COND`, `CST_ASSIGN`, `CST_COMMA`, `CST_PAREN`, `CST_PRIMARY`, `CST_ARG_LIST`, `CST_INIT_LIST`, `CST_COMPOUND_LITERAL` |
| 声明 | `CST_DECL`, `CST_DECL_SPECS`, `CST_DECLARATOR`, `CST_INIT_DECLARATOR`, `CST_FUNC_DEF`, `CST_PARAM_LIST`, `CST_PARAM`, `CST_TYPE_NAME`, `CST_INITIALIZER`, `CST_DESIGNATOR` |
| 语句 | `CST_IF`, `CST_SWITCH`, `CST_WHILE`, `CST_DO`, `CST_FOR`, `CST_CASE`, `CST_DEFAULT`, `CST_LABEL`, `CST_GOTO`, `CST_BREAK`, `CST_CONTINUE`, `CST_RETURN`, `CST_ASM` |
| 预处理 | `CST_PP_DIRECTIVE`, `CST_PP_DEFINE`, `CST_PP_UNDEF`, `CST_PP_INCLUDE`, `CST_PP_LINE`, `CST_PP_PRAGMA`, `CST_PP_ERROR`, `CST_PP_NULL`, `CST_PP_IF`, `CST_PP_BRANCH` |

几点说明:

- `CST_GROUP_PAREN` / `CST_GROUP_BRACKET` / `CST_GROUP_BRACE` 是按括号分组的最小单元, 不声明它是什么语法.
- `CST_BLOCK` 是语句块, 与 `CST_GROUP_BRACE` 区分.
- `CST_UNPARSED` 的子节点是已经切好的 token 和嵌套 group, 不复制 token.
- `CST_PP_IF` 下面每个分支是一个 `CST_PP_BRANCH`. 分支体内的声明和语句用正常文法建树.
- 每个关键字, 运算符, 括号, 分号都是 `CST_TOKEN` 叶子, 没有"隐含标点".

### 5.3.3 dump 格式

`libcp_cst_dump` 输出缩进两格的树:

```text
TU
  DECL
    DECL_SPECS
      TOKEN int
    INIT_DECLARATOR
      DECLARATOR
        TOKEN x
      TOKEN =
      BINARY
        PRIMARY
          TOKEN 1
        TOKEN +
        BINARY
          PRIMARY
            TOKEN 2
          TOKEN *
          PRIMARY
            TOKEN 3
    TOKEN ;
  EOF
```

叶子行是节点种类加 token 拼写. 内部节点只打印种类.

## 5.4 实现流程

### 5.4.1 入口

```c
CstNode *cst_parse(Token *tok) {
  CstContext ctx = {};
  cst_collect_typedefs(&ctx, tok);

  CstParser p = {tok, true, &ctx};
  CstNode *root = new_cst_node(CST_TRANSLATION_UNIT);
  cst_parse_items(root, &p);

  if (p.tok && p.tok->kind == TK_EOF)
    cst_add_leaf(root, p.tok);
  return root;
}
```

步骤:

1. 预扫描 typedef 名.
2. 建 `TU` 根.
3. 循环解析顶层项: 声明, 函数定义, 语句, 预处理指令, 或分号.
4. 补上 EOF 叶子.

### 5.4.2 括号分组

`cst_parse_group` 处理 `()`, `[]`, `{}`:

- 遇到开括号, 建对应 group 节点, 递归到匹配的闭括号.
- 缺闭括号, 或遇到不属于本层的闭括号, 该 group 变成 `CST_UNPARSED`, 已经收进的叶子留在里面.
- 深度由 `MAX_CST_DEPTH` (512) 限制. 超过限制的跨度变成一个扁平的 unparsed group, 每个 token 仍然是叶子.

### 5.4.3 声明与语句

- 顶层和块内都用 `cst_is_decl_start` 判断是不是声明. 判断依据是类型关键字, `struct` / `union` / `enum` / `typeof` / `_Atomic`, 或已知的 typedef 名.
- 声明拆成 `CST_DECL_SPECS`, 一个或多个 `CST_INIT_DECLARATOR`, 分号. 函数定义是 `CST_FUNC_DEF`.
- 语句覆盖 C 的语句形式: `if`, `switch`, `while`, `do`, `for`, `case`, `default`, 标签, `goto`, `break`, `continue`, `return`, `asm`, 块, 空语句, 表达式语句.
- 表达式按 C 优先级链解析, 包括逗号. 每个运算符和括号是显式叶子.
- `(T)x` 与 `(x)` 的区分: 如果 `(` 后面的第一个 token 看起来像类型名, 按 cast 或复合字面量解析; 否则按括号表达式. 判断可能错, 错了就是树形错, 字节不错.

### 5.4.4 预处理指令

- 行首的 `#` 开始一条指令. 指令的范围到下一个逻辑行首.
- `#define` 的对象宏和函数宏分界规则与 `preprocess.c` 一致.
- `#if` / `#elif` / `#else` / `#endif` 组成 `CST_PP_IF`, 每个分支一个 `CST_PP_BRANCH`. 条件不求值, 分支全部保留, `#endif` 作为 `PP_IF` 的子节点.
- 未知指令或无法完整识别的指令变成 `CST_PP_DIRECTIVE`, 其 token 仍是叶子.
- 指令出现在已经退化为 `CST_UNPARSED` 的跨度里时, 它不会额外变成 `CST_PP_*` 节点. 它仍然是 token 叶子. 这是结构覆盖的边界, 不影响重建.

### 5.4.5 回退

`cst_fail` 用于标记"当前产生式识别失败". 调用者把已经收集的子树交给 `CST_UNPARSED`, 然后从失败点继续按最保守的方式收 token, 直到分号或匹配的闭括号.

回退的粒度是跨度, 不是整棵树. 一个不认识的构造不会让整个文件变成 `UNPARSED`.

## 5.5 阶段产物

产物是一棵 `LibcpCstNode` 树:

- 根是 `CST_TRANSLATION_UNIT`.
- 叶子按序覆盖输入 token 流, 包括 EOF.
- 每个叶子直接指向原 token, 不复制.
- 树的形状是文法分组, 不是语义结构. 没有类型, 没有名字绑定, 没有常量值.
- 预处理指令和全部分支都在树里.

配套能力:

- `libcp_cst_reconstruct`: 按叶子输出 trivia 加拼写.
- `libcp_cst_dump`: 树形打印.
- `libcp_cst_verify`: 校验叶子序列与 token 流逐指针相等.

## 5.6 产物可以做什么

- **格式化**: 遍历树, 决定缩进, 空格, 换行, 同时保留注释. 遇到 `UNPARSED` 就原样输出该跨度.
- **注释迁移与保留**: 注释在 token 的 trivia 里. 移动节点时, 前置 trivia 跟着节点走, 可以保留注释.
- **局部重构**: 按语法节点定位源码片段, 例如"所有 if 语句的 then 分支". 需要自己从叶子算 span.
- **预处理感知的索引**: 能看到未选中分支和指令, 不只是编译后的代码.
- **宏定义浏览**: `CST_PP_DEFINE` 直接给出宏名和宏体 token.
- **源码级 diff**: 两侧都建 CST, 按叶子序列比较, 能区分注释和空白变化.
- **教学**: 观察 C 文法怎么切分源码.

不能做:

- 不能做类型检查或名字解析.
- 不能知道 `#if` 哪个分支生效.
- 不能展开宏.
- 不能保证每个跨度都有细粒度结构.
- 不能把树直接转成第 3 层. 两者输入不同.

## 5.7 API 用法

### 5.7.1 建树

```c
LibcpToken *raw = libcp_tokenize_file(ctx, "input.c");
LibcpCstNode *root = libcp_cst_parse(ctx, raw);
if (!root) {
  /* 解析器是全函数, 理论上只在内存不足或上下文错误时返回 NULL */
}
```

注意 `raw` 还没有预处理. 建完 CST 后再预处理 `raw` 会把链改坏, 所以要么先完成所有 CST 工作, 要么重新 tokenize.

### 5.7.2 重建并比较

```c
FILE *out = fopen("rebuilt.c", "wb");
libcp_cst_reconstruct(ctx, root, out);
fclose(out);
```

验证:

```sh
cmp input.c rebuilt.c
```

### 5.7.3 校验叶子

```c
if (!libcp_cst_verify(ctx, root, raw))
  fprintf(stderr, "CST leaves do not match the token stream\n");
```

校验内容:

- 叶子数量与 token 数量一致.
- 每个叶子指向同一个 token 对象, 不是复制.
- 顺序一致.

### 5.7.4 遍历树

```c
static void walk(LibcpCstNode *n, int depth) {
  for (int i = 0; i < depth; i++)
    fputs("  ", stdout);
  printf("kind=%d", (int)n->kind);
  if (n->tok)
    printf(" token=%.*s", n->tok->raw_len, n->tok->raw_loc);
  putchar('\n');

  for (int i = 0; i < n->child_count; i++)
    walk(n->children[i], depth + 1);
}
```

### 5.7.5 只读通道

```sh
./chibicc -dump-cst input.c
./chibicc -reconstruct-cst input.c > rebuilt.c
./chibicc -verify-cst input.c
```

`-reconstruct-cst` 和 `-reconstruct` 在合法输入上应该输出相同的字节. 前者走树, 后者走 token 链.

### 5.7.6 处理 `UNPARSED`

```c
if (node->kind == CST_UNPARSED) {
  /* 结构未知: 直接输出该跨度的叶子 */
  for (int i = 0; i < node->child_count; i++)
    emit_raw(node->children[i]);
}
```

这是 formatter 的必备分支. 不处理它, 格式化器会在不认识的结构上丢字节.

## 5.8 边界与已知限制

- **必须在预处理之前建树**. 预处理会改写输入链. 同一份文件 tokenize 两次也可以, 但要接受两份内存.
- **不做预处理**. 指令不求值, 宏不展开, include 不跟随. `#if 0` 的分支也在树里.
- **typedef oracle 是启发式**. 只认文件内的几种 typedef 写法. include 来的 typedef 未知, 局部 typedef 被当作全局可见. 判断错误时树形退化, 字节不丢.
- **`CST_UNPARSED` 的覆盖面**. 截至本文档写作时实测: 48 个语料文件中 29 个含 `UNPARSED`, 共 764 个 span, 主要来源是 GNU 语句表达式 `({ ... })` 和头文件 typedef 导致的声明回退. 依赖结构的工具必须处理 `UNPARSED`.
- **没有源码范围字段**. 内部节点只有 children, 没有 `start` / `end`. 要算一个节点的字节范围, 得递归到首叶子和末叶子. 这是 L5.1 formatter 需要补的能力, 目前由消费者自己做.
- **指令在 `UNPARSED` 跨度内不独立成 PP 节点**. 重建不受影响, 结构查询会漏.
- **深度上限 512**. 更深的嵌套变成扁平 unparsed group. 每个 token 仍在, 但嵌套结构丢失.
- **不解释语言方言**. `asm`, `typeof`, 语句表达式等 GNU 扩展有节点或回退, 但不是完整支持.
- **没有名字解析**. 同名变量, 作用域, 标签作用域都不处理. 这是第 3/4 层的事.
- **重建只对直接 tokenize 输出有效**. 对合成 token 或预处理后的流, 重建不是原文件.
