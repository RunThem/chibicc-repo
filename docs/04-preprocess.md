# 04 预处理与宏展开 provenance

## 4.1 作用与位置

预处理是第 1 层和第 3 层之间的一条横切步骤. 它消费 token 链, 执行 C 预处理语言, 输出展开后的 token 链. 它同时是库唯一会**破坏输入链**的阶段, 也是唯一记录宏展开来源的阶段.

输入: `libcp_tokenize_*` 的直接输出.
输出: 一条新的 token 链, 宏已展开, 条件指令已消解, include 已拼接.
附带产物: 每个非源 token 上的 provenance 链.

## 4.2 设计思路

### 4.2.1 token 进, token 出

预处理器不建树, 也不输出字符串. 它的输入和输出都是 `LibcpToken` 链, 指令用 token 拼写识别. 好处是:

- 不需要在预处理阶段引入新的中间表示.
- 输出可以直接喂给解析器.
- 宏展开可以复用 token 的位置信息.

代价是预处理必须原地操作输入链. `libcp_preprocess` 在拼接 include 和展开宏时会改写输入 token 的 `next` 指针. 调用之后, 原来那条链不再完整.

### 4.2.2 宏展开必须终止

C 标准要求宏展开在递归定义下也能终止. 例如:

```c
#define A B
#define B A
A
```

不能无限展开. 库沿用上游的 hideset 算法: 每个 token 携带一组宏名, 表示"这些宏已经应用过". 展开一个宏时, 把宏名加入所有产物的 hideset. 一个 token 不会再被同一个宏展开.

这个算法来自 Dave Prosser 的 cpp 算法, 也是标准措辞的基础. 实现里 `LibcpHideset` 是一条链表, 成员查找是 O(n), 因为 n 通常很小.

### 4.2.3 为什么要 provenance

hideset 解决了"展开到哪里停", 但没有回答"这个 token 从哪来". 对编译器, 来源不重要; 对工具, 来源很重要:

- 诊断要指向用户写的那一行, 而不是宏体.
- 重构要知道某个 token 是不是宏产生的, 不能直接改.
- 代码浏览器要展示宏调用和展开结果的关系.
- 分析器要区分"源码里写了这个标识符"和"宏展开出了这个标识符".

provenance 记录每次宏展开:

- 调用点的宏名 token.
- function-like 宏的右括号 token.
- 展开时刻生效的宏定义.
- `#define` 的宏名 token, 用于定位定义处.
- 实参 token 列表.
- 嵌套展开的上一层链.

记录在展开时建一次, 之后只读. 链节点只做 prepend, 共享的尾部不可变. 这样同一批产物可以共享记录, 不需要深拷贝.

### 4.2.4 不为宏调用建语法节点

一个反复讨论过的设计问题是: 语法层要不要保留宏调用节点. 决定是不保留.

理由:

- 宏调用不是 C 语法的一部分, 它的"参数"和"体"没有统一的语法结构.
- 保留宏调用节点会让语法树变成两套文法混在一起, 下游每一步都要判断"这是宏还是代码".
- 宏的原始写法已经由第 2 层完整保留.
- 宏到产物的对应由 provenance 提供.

放弃的能力是"在宏展开内部做外科式编辑". 这个能力没有被承诺, 需要它的工具应该改宏定义, 而不是改展开产物.

## 4.3 数据模型

### 4.3.1 私有类型

`Macro`, `MacroParam`, `MacroArg` 只在 `libcp/src/preprocess.c` 内部使用, 不出现在公共头. 公共面只有两个不透明类型和访问器:

```c
typedef struct libcp_macro_expansion LibcpMacroExpansion;
/* LibcpMacroProv 在 token.h 中前向声明 */
```

`LibcpMacroExpansion` 的内部布局:

```c
struct libcp_macro_expansion {
  Token *name;      /* 调用点的宏名 token */
  Token *rparen;    /* function-like 的右括号, 否则 NULL */
  Macro *macro;     /* 展开时生效的宏定义 */
  MacroArg *args;   /* 实参, 对象宏和内建宏为 NULL */
  int arg_count;
  bool is_builtin;  /* 由 handler 产生, 例如 __LINE__ */
};
```

`LibcpMacroProv` 是链节点:

```c
struct libcp_macro_prov {
  MacroExpansion *exp;
  int arg_index;    /* -1 表示宏体, >= 0 表示实参序号, 0 起 */
  MacroProv *next;  /* 调用本身所在的上下文 */
};
```

### 4.3.2 链的方向

链的最内层在前. 例如:

```c
#define INNER 1
#define OUTER INNER
int x = OUTER;
```

`1` 这个 token 的链是: 最内层 `INNER` 的展开, 然后是 `OUTER` 的展开. 最外层是源码里的 `OUTER`.

`libcp_macro_prov_next` 往链尾走, 也就是往更外层的调用上下文走.

### 4.3.3 实参列表

`libcp_macro_expansion_arg(e, i)` 返回第 i 个实参的 token 链. 这个链以 `TK_EOF` 结尾, 不是以 NULL 结尾. 空实参只有 EOF 标记.

`libcp_macro_prov_arg_index(p)` 返回这个产物属于宏体还是某个实参:

- `-1`: 宏体.
- `0` 或更大: 实参序号, 从 0 开始.

dump 输出里显示为 `body` 或 `arg1`, `arg2`, 即索引加 1.

## 4.4 实现流程

### 4.4.1 主循环

`preprocess(tok)` 的流程:

1. `preprocess2` 逐 token 扫描.
2. 如果 token 是宏名, `expand_macro` 展开, 插入产物.
3. 如果不是行首的 `#`, 原样输出, 同时填写 `line_delta` 和 `filename`.
4. 如果是 `#`, 按指令名分派.
5. 结束后检查有没有未闭合的条件指令.
6. `convert_pp_tokens`: 把 `TK_IDENT` 中的关键字改成 `TK_KEYWORD`, 把 `TK_PP_NUM` 转成 `TK_NUM`.
7. `join_adjacent_string_literals`: 拼接相邻字符串字面量, 并保留 provenance.
8. 给每个 token 的 `line_no` 加上 `line_delta`, 让 `#line` 生效.

### 4.4.2 指令

支持的指令:

| 指令 | 行为 |
|---|---|
| `#include` | 引号形式先查当前文件所在目录, 再查 include 路径; 尖括号形式只查 include 路径 |
| `#include_next` | 从当前 include 路径的下一个位置继续查 |
| `#define` | 对象宏, 函数宏, 变参宏; 记录宏体和 `#define` 名 token |
| `#undef` | 删除宏 |
| `#if` | 用 `const_expr` 求值, 未选中分支跳过 |
| `#ifdef` / `#ifndef` | 检查宏是否存在 |
| `#elif` / `#else` / `#endif` | 条件栈 |
| `#line` | 设置 `line_delta` 和 `display_name` |
| GNU 行标记 | `# 123 "file"` 形式, 与 `#line` 等价 |
| `#pragma once` | 记录文件只包含一次 |
| 其他 `#pragma` | 忽略整行 |
| `#error` | 报告 `error` 并终止 |
| 空指令 | `#` 单独一行, 合法, 忽略 |

不支持的指令按 `invalid preprocessor directive` 报错. 例如 `#warning` 不被识别.

`#include` 的搜索细节:

- 绝对路径直接使用.
- `"foo.h"` 且不是绝对路径: 先试 `dirname(当前文件)/foo.h`, 存在就用.
- 否则遍历 context 的 include 路径.
- 都找不到时用字面文件名再试一次, 由 `include_file` 报告打不开.

include 的文件会做 include guard 检测和 `#pragma once` 记录. `include_depth` 记录嵌套深度.

### 4.4.3 宏展开

`expand_macro` 处理三类宏:

1. **内建宏 handler**: `__FILE__`, `__LINE__`, `__COUNTER__`, `__TIMESTAMP__`, `__BASE_FILE__`. 产物 token 带 `is_builtin` 记录.
2. **对象宏**: 把宏体复制出来, 套上 hideset, 记录展开.
3. **函数宏**: 读实参, 展开实参, 对宏体做替换, 处理 `#` 字符串化和 `##` 粘贴, 套上 hideset, 记录展开.

每个产物 token 的 provenance 在 `subst` 里挂接:

- 来自宏体的 token: `arg_index = -1`.
- 来自第 i 个实参的 token: `arg_index = i`.
- 产物自身的 `next` 链上下文作为新链的尾部.

`join_adjacent_string_literals` 会重扫字符串, 它保存并恢复 `prov`, 保证拼接后的 token 仍能追到来源.

### 4.4.4 预定义宏

context 默认安装 47 个预定义宏, 分四类:

- **平台与标准**: `_LP64`, `__ELF__`, `__linux__`, `__x86_64__`, `__STDC__`, `__STDC_VERSION__` (`201112L`), `__STDC_HOSTED__`, `__chibicc__` 等.
- **尺寸**: `__SIZEOF_INT__`, `__SIZEOF_LONG__`, `__SIZEOF_POINTER__`, `__SIZEOF_SIZE_T__` 等, 值由类型记录推导.
- **类型与关键字别名**: `__SIZE_TYPE__`, `__alignof__`, `__const__`, `__inline__`, `__typeof__`, `__volatile__` 等.
- **动态与内建**: `__DATE__`, `__TIME__`, `__TIMESTAMP__`, `__FILE__`, `__LINE__`, `__BASE_FILE__`, `__COUNTER__`.

注意 `__chibicc__` 默认定义. 被预处理代码如果检查这个宏, 会走 chibicc 分支.

`libcp_context_set_predefined_macros(ctx, false)` 关闭整个集合, 但**下一次 reset 才生效**. 关闭后, `libcp_context_predefined_count` 返回 0.

### 4.4.5 命令行宏

```c
libcp_context_define(ctx, "NAME", "value");
libcp_context_undef(ctx, "NAME");
```

两个函数都立即生效, 并把操作记录在 context 的配置里. reset 后重新按顺序应用. 因此 `-D` / `-U` 的顺序对最终宏表有影响.

`define` 的第二个参数是替换文本, 不是值. 写 `"1"` 得到 `#define NAME 1`; 写 `"foo(x)"` 会按函数宏定义解析.

## 4.5 阶段产物

预处理的产物有两层:

1. **展开后的 token 链**. 所有宏已展开, 条件指令已消解, include 已拼接, 关键字和数值已分类. 这条链可以直接给 `libcp_parse`.
2. **每个非源 token 的 provenance 链**. 源 token 的 `prov` 为 NULL.

此外还有可观察的调试通道:

- `libcp_expansion_dump(ctx, tok, out)`.
- `libcp_expansion_verify(ctx, tok)`.
- 驱动器 `-dump-expansions` / `-verify-expansions`.

dump 格式:

```text
T <file>:<line> <kind> <spelling> <chain>
```

`<chain>` 为 `-` 表示源 token. 否则是若干层, 最内层在前, 用 `>` 连接. 每层:

```text
<macro>@<call-file>:<call-line>:<body|argN>:<def-file>:<def-line>
```

没有 `#define` token 的宏, 例如内建宏和 `-D` 定义, 定义位置显示为 `?:?`. 合成位置显示为 `@synth`.

真实文件上的例子:

```text
T m.c:2 keyword int -
T m.c:2 ident f -
T m.c:2 punct ( -
T m.c:2 keyword void -
T m.c:2 punct ) -
T m.c:2 punct { -
T m.c:2 keyword return -
T m.c:1 num 1 A@m.c:2:body:m.c:1
T m.c:2 punct ; -
T m.c:2 punct } -
```

最后一行数字 1 是 `A` 的展开产物. 它自己的位置是 `m.c:1`, 即宏定义所在行; 链里记录调用点 `m.c:2`, 层是 `body`, 定义点 `m.c:1`.

## 4.6 产物可以做什么

- **编译路径的输入**: 解析器只接受预处理后的 token 链.
- **宏展开检查**: 判断某个 token 是不是宏产生的, 来自哪个宏, 哪一层, 哪个实参.
- **诊断定位**: 结合 token 的物理位置和 provenance, 把错误映射回调用点. 库目前没有把这个映射封装成 API, 但访问器足够自己实现, 见第 11 章.
- **宏使用统计**: 统计每个宏被展开多少次, 哪些实参产生了哪些代码.
- **源码浏览器**: 展示宏调用与展开产物的对应关系.
- **预处理结果导出**: 消费者可以自己遍历 token 链, 用 `loc` / `len` 输出展开后的文本.

不能做:

- 不能从展开流还原原始文件. 宏体, 未选中分支, 指令行和实参之间的 trivia 都已经不在流里.
- 不能修改宏展开内部的某个产物并期望源文件对应位置改变. 产物是合成 token, 不映射回调用点的字节.
- 不能在同一个 token 链上既做 CST 又做预处理. 预处理会破坏输入链.

## 4.7 API 用法

### 4.7.1 基本调用

```c
LibcpToken *tok = libcp_tokenize_file(ctx, "input.c");
tok = libcp_preprocess(ctx, tok);
if (!tok) {
  /* 诊断已报告, ctx 已失败 */
}
```

输入链在调用后不再可用. 需要原始流时先取走:

```c
LibcpToken *raw = libcp_tokenize_file(ctx, "input.c");
LibcpCstNode *cst = libcp_cst_parse(ctx, raw);   /* 先建 CST */
LibcpToken *expanded = libcp_preprocess(ctx, raw); /* 再预处理 */
```

或者对同一个文件 tokenize 两次. 两次得到的 `LibcpFile` 是不同对象, 文件编号也不同.

### 4.7.2 遍历 provenance

```c
LibcpToken *t = expanded;
for (; t && t->kind != TK_EOF; t = t->next) {
  LibcpMacroProv *p = libcp_token_provenance(t);
  if (!p)
    continue;

  for (; p; p = libcp_macro_prov_next(p)) {
    LibcpMacroExpansion *e = libcp_macro_prov_expansion(p);
    int arg = libcp_macro_prov_arg_index(p);
    printf("token %.*s: macro=%s, arg=%d, builtin=%d\n",
           t->len, t->loc,
           libcp_macro_expansion_macro(e), arg,
           libcp_macro_expansion_is_builtin(e));
  }
}
```

### 4.7.3 定位调用点和定义点

```c
LibcpMacroExpansion *e = libcp_macro_prov_expansion(p);
LibcpToken *call = libcp_macro_expansion_name(e);
LibcpToken *end  = libcp_macro_expansion_end(e);
LibcpToken *def  = libcp_macro_expansion_def(e);

printf("call at %s:%d\n", call->filename, call->line_no);
if (def)
  printf("defined at %s:%d\n", def->filename, def->line_no);
```

`libcp_macro_expansion_end` 对函数宏返回右括号 token, 对对象宏返回宏名 token.

### 4.7.4 取实参

```c
int n = libcp_macro_expansion_arg_count(e);
for (int i = 0; i < n; i++) {
  LibcpToken *arg = libcp_macro_expansion_arg(e, i);
  for (LibcpToken *t = arg; t && t->kind != TK_EOF; t = t->next)
    printf("arg %d token: %.*s\n", i, t->len, t->loc);
}
```

实参链以 `TK_EOF` 结尾.

### 4.7.5 校验

```c
if (!libcp_expansion_verify(ctx, expanded)) {
  /* 链损坏: 深度超限, 节点重复, 实参越界, 名字不一致 */
}
```

校验只用于调试和测试. 正常编译路径不调用它.

### 4.7.6 关闭预定义宏

```c
libcp_context_set_predefined_macros(ctx, false);
libcp_context_reset(ctx);   /* 生效 */
```

用于构造干净的预处理环境, 例如测试一个头文件在最小宏集下的行为. 记录下来的 `-D` / `-U` 仍然生效.

## 4.8 边界与已知限制

- **输入链被破坏**. 这是最重要的限制. `libcp_preprocess` 之后不要再用原来的 token 指针, 除非你确定它们没有被拼接影响. CST 必须在它之前建.
- **展开流不能重建原文件**. 对展开流调用 `libcp_reconstruct` 得到的是展开产物的 raw 坐标拼接. 例如 `int x = A;` 会被重建为 `int x = 42;`, 而 `#define A 42` 那一行消失. 这是预期行为, 不是 bug.
- **没有宏调用节点**. 语法树上看不到"这里调用了一个宏". 需要知道来源就查 provenance.
- **零产出展开不可见**. `#define E` 后面的 `E` 展开成空, 不产生任何 token, 因此没有 token 可以挂 provenance. 这类展开在展开流里完全不可见.
- **`#if` 的分支丢弃**. 未选中分支不进 output. 需要保留全部分支时用第 2 层 CST.
- **实参之间的 trivia 丢失**. 宏调用 `M(a, b)` 中逗号两边的空白和注释不进入展开流. 产物的 trivia 指向宏体或实参内部.
- **合成位置**. 粘贴或字符串化产生的 token 位置是合成缓冲. dump 显示 `@synth`, 诊断位置可能指向宏定义或合成位置, 而不是调用点.
- **内存中的输入也被标成 `@synth`**. `libcp_tokenize_buffer` 的 `raw == contents`, provenance 的 `is_synthetic` 判定会把它当作合成缓冲. 用文件输入才有真实行号.
- **不支持的指令**. `#warning` 等未知指令报 `invalid preprocessor directive`. `#pragma` 除 `once` 外忽略.
- **include 只走文件系统**. 没有虚拟文件系统或内存 include 回调. 内存中的源码可以 `#include` 磁盘上的头, 但不能 include 另一个内存字符串.
- **预定义宏假设 x86-64 Linux**. `__linux__`, `__ELF__`, `_LP64`, `__x86_64__` 无条件定义, 即使库在 macOS 上构建. 做跨目标语义分析时要注意.
- **`__chibicc__` 默认定义**. 被预处理代码可能因此走 chibicc 专用分支.
