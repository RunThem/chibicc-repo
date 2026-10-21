# 11 缺陷, 限制与坑

本章列出使用 `libchibicc` 时必须知道的边界. 这些大多是设计限制, 不是未修复的崩溃缺陷. 已修复的历史缺陷记录在第 12 章.

## 11.1 先看清承诺

- 版本 `0.1.0`.
- **只承诺源码兼容, 不承诺 ABI**.
- 阶段 5 的闸门全绿, 但这不等于"生产级成熟".
- 阶段 6 (L5) 未开工: 没有真实消费者验证 API 手感, 没有 formatter, 没有源码映射.

如果你的项目需要稳定的二进制接口, 成熟的诊断恢复, 或完整的 C11 覆盖, 这个库目前不合适.

本章列的是已知边界, 不是"库没有缺陷"的声明. 发现新缺陷时按 11.15 的方式整理复现.

## 11.2 生命周期与内存

### 11.2.1 reset 后所有指针失效

这不是"可能失效", 是契约. `chibicc_context_reset` 释放整个 arena. token, AST, Obj, 类型, CST, 诊断字符串全部失效.

规避:

- 在 reset 前复制需要保留的数据.
- 不要把库指针存进长期数据结构.
- 不要在多线程间共享一个 context 的指针.

### 11.2.2 没有逐对象释放

- 不能只释放一棵子树.
- 不能只保留 CST 丢掉 AST.
- 一个 TU 的所有对象一起活, 一起死.

规避: 只需要 CST 时用第二个 context, 或先完成 CST 工作再 reset.

### 11.2.3 内存峰值约为最终大小的两倍

arena 的 `realloc` 复制到新块, 旧块不回收. 扩容的数组 (children, token 数组, 字符串) 会留下死容量.

规避:

- 对大文件预留容量.
- 分批处理, 及时 reset.
- 不要指望库做内存优化.

### 11.2.4 一个 context 一个 TU

没有增量 API, 没有多 TU 共享符号表.

规避: 每个文件一个 context, 或每个文件之间 reset. 跨 TU 分析要自己做.

### 11.2.5 内存不足没有优雅处理

arena 分配失败时, 库没有完整的 OOM 恢复策略. 部分路径可能返回 NULL, 部分路径可能崩溃.

规避: 在受控环境中使用, 给进程足够内存.

## 11.3 错误处理

### 11.3.1 第一个错误就停止

- 没有错误恢复.
- 没有部分树.
- 不能一次报告多个错误.

规避:

- 对每个文件独立调用, 收集单条诊断.
- 需要多错误时, 自己在语句或声明边界分段调用.
- 用 CST 做纯语法检查, 它不报错, 但也不告诉你哪里错.

### 11.3.2 返回 NULL 不一定是错误

空翻译单元:

```c
ChibiccNode *ast = chibicc_parse(ctx, tok);   /* NULL */
bool failed = chibicc_context_failed(ctx);    /* false */
```

必须用 `chibicc_context_failed` 区分.

### 11.3.3 failed 是粘性的

一旦置位, 直到 reset 才清除. 后续调用即使成功, `failed` 仍为 true.

规避: 出错后立即 reset, 再复用.

### 11.3.4 警告不置位 failed

警告走回调, 流程继续. 不要用 `failed` 判断"有没有警告". 需要自己统计 `severity == CHIBICC_DIAG_WARNING`.

### 11.3.5 错误后 context 不一致

库不保证部分树可用. 不要在失败后继续遍历.

## 11.4 线程

- 一个 context 不能跨线程并发.
- 不同 context 可以在不同线程并存.
- `active_ctx` 是 `_Thread_local`, 同一线程嵌套调用会保存 / 恢复.
- `chibicc_context_new` / `free` 会修改 active 指针, 跨线程调用要自己同步.

规避:

- 每个线程一个 context.
- 不要共享 context.
- 不要在一个 context 上开线程.

## 11.5 输入与 include

### 11.5.1 没有默认 include 路径

`chibicc_context_new` 不添加 `/usr/include` 或任何系统路径. 库不知道目标平台.

规避:

```c
chibicc_context_add_include_path(ctx, "/usr/local/include");
chibicc_context_add_include_path(ctx, "/usr/include/x86_64-linux-gnu");
chibicc_context_add_include_path(ctx, "/usr/include");
```

顺序就是搜索顺序. 参考 `main.c` 的 `add_default_include_paths`.

### 11.5.2 没有虚拟文件系统

`#include` 只走文件系统. 不能注册内存中的头文件, 不能拦截读取.

规避:

- 把内存头写到临时文件, 再加路径.
- 自己预处理, 只把库当词法 / 语法 / 语义分析器.

### 11.5.3 内存输入不经过扫描缓冲改写

`chibicc_tokenize_file` 在扫描前做五项改写:

1. 跳过 BOM.
2. CRLF 和孤立 CR 归一成 `\n`.
3. 删除反斜杠续行.
4. 把 `\uXXXX` / `\UXXXXXXXX` 改写成 UTF-8.
5. 保证末尾有换行.

`chibicc_tokenize_buffer*` 一项都不做, 按字节扫描. 后果:

- BOM 留在第一个 token 的拼写里, 可能让 `int` 变成标识符 `\xEF\xBB\xBFint`.
- 反斜杠续行会生成一个 `\` 标点 token, 而不是把两行连起来.
- `\u00e9` 会变成 `\` 加 `u00e9`, 不是一个标识符字符.
- 孤立 CR 不增加行号.

规避:

- 真实源码优先用 `chibicc_tokenize_file`.
- 必须用内存输入时, 自己先做这五项改写, 或至少剥离 BOM 和续行.
- 或者把内容写进临时文件再 tokenize.

### 11.5.4 内存中的输入被标记为 `@synth`

`chibicc_tokenize_buffer` 的 `raw == contents`, provenance dump 会把它当成合成缓冲, 位置显示 `@synth`.

规避: 需要真实的文件与行号时用 `chibicc_tokenize_file`.

### 11.5.5 文件大小上限

超过 `INT32_MAX` 字节的文件被拒绝. 没有分块处理.

### 11.5.6 stdin 只读一次

`chibicc_tokenize_file(ctx, "-")` 第一次读 stdin, 内容缓存. 第二次拿到同一份文本, 不是新的输入.

### 11.5.7 源文件含 NUL 会报错

NUL 被视为诊断, 不是静默截断. 二进制文件不能直接 tokenize.

## 11.6 语言覆盖

继承自上游 chibicc 的限制:

- **不支持复数** (`_Complex`, `_Imaginary`).
- **不支持 K&R 风格函数定义**.
- **不支持 `_Static_assert`**. 解析器会报 `expected ','` 一类错误.
- **不支持 digraph 和 trigraph**.
- **不支持 `#warning`**. 报 `invalid preprocessor directive`.
- **不支持带操作数约束的 GCC 内联汇编**. 但 `asm("...")` 语句会被解析并把字符串原样发射到汇编.
- **限定符被丢弃**. `const`, `volatile`, `restrict` 在解析时被消费, 不进入类型记录. 给 `const` 对象赋值不会报错.
- **`_Noreturn`** 被接受但不参与分析.
- **`_Atomic`** 有基本支持, 但不是完整的内存模型.
- **`long double`** 按 x86-64 的 80 位 x87 格式.
- **不保证其他 ABI**. 布局, 对齐, 位域规则按 x86-64 System V.

规避:

- 需要完整 C11 覆盖的工具不要用这个库.
- 需要 const / volatile 信息时, 从 token 或 CST 读, 不要指望类型记录.
- 不要用它编译依赖 `_Static_assert` 或复数的代码.

## 11.7 宏保真

### 11.7.1 没有宏调用节点

语法树上只有展开后的结构. 你无法从 AST 知道"这里原来是一个宏".

规避: 查 provenance.

### 11.7.2 展开流不能还原原文件

`chibicc_preprocess` 之后:

- 宏调用的拼写和实参布局消失.
- 指令行消失.
- 未选中分支消失.

对预处理后的流调用 `chibicc_reconstruct` 得到的是展开流的拼接, 不是原文件.

规避: 需要原文件就用第 1/2 层, 并且不要预处理.

### 11.7.3 零产出展开不可见

`#define E` 后面的 `E` 展开成空, 不产生 token, 因此没有 token 可以挂 provenance. 这类展开在展开流里完全不可见.

### 11.7.4 实参间的 trivia 丢失

`M(a, b)` 里逗号两边的空白和注释不进入展开流. 产物的 trivia 指向宏体或实参内部.

### 11.7.5 合成位置

粘贴, 字符串化, 内建宏产生的 token 位置是合成的. dump 显示 `@synth`, 诊断可能指向宏定义或合成位置.

### 11.7.6 不能做宏内手术

不能改一个展开产物并期望调用点源码变化. 宏展开是只读的派生数据.

### 11.7.7 预定义宏假设 x86-64 Linux

`__linux__`, `__ELF__`, `_LP64`, `__x86_64__` 无条件定义, 即使库在 macOS 上构建. 做跨目标语义分析时, 这些宏会误导被分析的代码.

规避: `chibicc_context_set_predefined_macros(ctx, false)` 后 reset, 自己定义需要的宏.

## 11.8 CST

### 11.8.1 结构覆盖不完整

截至本文档写作时实测: 48 个语料文件中 29 个含 `CST_UNPARSED`, 共 764 个 span, 主要来源:

- GNU 语句表达式 `({ ... })`.
- 头文件 typedef (`va_list`, `pthread_t` 等) 不在文件内, 导致声明回退.

重建不受影响, 但依赖结构的工具会退化.

规避: 遍历时处理 `CST_UNPARSED`, 原样输出该跨度.

### 11.8.2 typedef oracle 是启发式

- 只认文件内的几种 typedef 写法.
- 名字在整个文件内可见, 不按声明位置.
- 局部 typedef 被当作全局.
- include 来的 typedef 未知.

判断错误时树形可能错 (cast 当括号, 声明当表达式), 字节不丢.

### 11.8.3 没有源码范围

内部节点只有 `children`, 没有 `start` / `end`. 算范围要递归到首末叶子.

### 11.8.4 指令在 `UNPARSED` 跨度内不独立成节点

重建不受影响, 结构查询会漏.

### 11.8.5 深度上限 512

更深的嵌套变成扁平 unparsed group. 每个 token 仍在, 嵌套结构丢失.

### 11.8.6 必须在预处理之前建树

预处理会破坏输入链.

## 11.9 AST 与 sema 的信息丢失

### 11.9.1 第 3 层没有类型计算和名字

- 大多数 `Node.ty` 为 NULL. 例外: 数值字面量带词法类型, `ND_TYPEDEF` 带声明类型.
- `ND_IDENT.var` 为 NULL.
- 常量未求值.

### 11.9.2 第 4 层原地改写

- `ND_IDENT` 变成 `ND_VAR` 或 `ND_NUM`.
- `ND_STRING` 变成 `ND_VAR`.
- 隐式 cast 插入.

原始形态在 sema 后不再存在.

规避:

- 在 sema 前 dump 或处理.
- 对同一个文件重新跑一条管线.
- 库没有深拷贝 API.

### 11.9.3 `init_resolved` 是私有字段

`sema.c` 的初始化树指针. 消费者不得读写. 需要初始化数据时:

- 全局: `Obj.init_data`.
- 局部: 自己从 `Node.decl_init` 重新解析, 或写自己的后端逻辑.

### 11.9.4 声明记录留在语句链上

`ND_DECL`, `ND_GVAR_DECL`, `ND_FUNCDEF`, `ND_TYPEDEF`, `ND_ENUM_CONST` 不会被 sema 摘除. 第 5 层才处理.

消费者必须显式处理这些种类, 否则会漏语句或重复处理.

### 11.9.5 `globals` 链包含匿名对象和声明

- 包含 `__func__` / `__FUNCTION__` 字符串全局.
- 包含 `extern` 声明.
- 不包含 `alloca` 内建.
- 顺序是创建顺序的逆序.

规避: 过滤 `is_definition`, 按名字或类型识别匿名对象.

### 11.9.6 没有跨 TU 信息

只有一个 TU 的 `globals`. 没有链接期符号表, 没有跨文件类型合并.

### 11.9.7 没有源码范围

与第 3 层相同, 节点只有代表 token.

### 11.9.8 限定符丢失

`const`, `volatile`, `restrict` 不进入类型. 依赖它们的分析要自己从 token / CST 读.

### 11.9.9 平台相关的布局

`long double`, 位域, 对齐按 x86-64 System V. 在其他目标上分析时, 布局结论可能不对.

## 11.10 诊断

### 11.10.1 宏内错误可能指向宏定义

有些宏展开产生的错误, 位置是宏体, 不是调用点. 例:

```c
#define BAD (1 +)
int x = BAD;
```

报错位置在 `#define` 那一行. 另一些情况 (例如宏体在表达式末尾) 会指向调用行. 行为依赖错误发生在哪个 token 上.

规避: 用 provenance 把 token 映射回调用点. 库没有现成的诊断展开点字段, 这是 L5.3 的缺口.

### 11.10.2 `#line` 只改行号, 不改文件名

诊断的行号会叠加 `#line` 偏移, 文件名仍是物理路径. `tok->filename` 是显示名, 但 `error_tok` 用的是 `tok->file->name`.

### 11.10.3 无位置错误的字段是 0 / NULL

例如打不开文件:

```text
filename = NULL, line = 0, column = 0
```

回调必须容忍.

### 11.10.4 默认处理器会打印

没有注册回调时, 库按驱动器旧格式打印到 stderr. 不想被打印就注册一个空回调.

### 11.10.5 `expansion_verify` 不调用回调

它直接向 stderr 打印一条消息并返回 false. 这是调试通道, 不是普通 API.

### 11.10.6 诊断文案是锁定文本

`test/diagnostic.sh` 逐字节锁定 68 例. 升级版本时文案可能变. 不要依赖文案做程序判断, 依赖返回值和 `failed`.

## 11.11 公共面与 ABI

### 11.11.1 结构体字段直接暴露

- 改字段就是换版本.
- 没有不透明句柄.
- 没有稳定 ABI.
- 不要依赖字段布局做序列化. 需要持久化时, 写自己的稳定格式.

### 11.11.2 `.a` 暴露内部符号

静态库不重命名内部函数. `nm libchibicc.a` 能看到大量非 `chibicc_` 前缀的 `T` 符号.

- 公共头不声明它们.
- 消费者不得调用.
- 可能与宿主符号碰撞.

规避: Linux 下优先用 `.so`, 它只导出 36 个 D7 函数. 或者把库放到独立命名空间 / 单独进程.

### 11.11.3 公共头没有 `extern "C"`, 参数不是 const

C++ 消费者要自己包:

```cpp
extern "C" {
#include <chibicc/chibicc.h>
}
```

此外, `chibicc_tokenize_buffer` / `chibicc_context_add_include_path` / `_define` 等入口的参数类型是 `char *`, 不是 `const char *`. C++ 里传字符串字面量需要显式转换. C 里不要求.

### 11.11.4 版本宏与 Makefile 必须同步

`CHIBICC_VERSION` 和 `VERSION` 不一致会被 `test/install.sh` 抓到. 手改时要同时改.

## 11.12 性能

- 没有优化 pass. 代码生成质量不在库的范围内.
- 词法和语法是简单实现, 没有为速度做特殊优化.
- 哈希表, 链表, 字符串驻留都是"够用"级别.
- 大文件的内存峰值约为最终数据的两倍.
- 每次调用从头构建, 没有增量.

规避: 对性能敏感的场景先做基准, 不要假设它和 Clang 一样快.

## 11.13 L5 未完成的能力

这些不是 bug, 是还没做:

- **没有 CST formatter**. 第 2 层是数据, 排版策略要自己写.
- **没有 AST 到源码的映射 API**. 只有代表 token, 没有 start / end.
- **没有诊断展开点字段**. provenance 访问器可以自己映射.
- **没有 C 转 C 打印器**. 第 3 层是数据, 打印器要自己写.
- **没有 `-cst-stats`**. CST 覆盖率没有随库发布的统计工具.
- **没有真实消费者验证 API 手感**. 当前的测试是合成消费者.

如果你的工具依赖其中任何一项, 需要自己实现, 或等 L5.

## 11.14 边界行为速查

| 情况 | 实际行为 |
|---|---|
| 空翻译单元 | `parse` / `sema` 返回 NULL, `failed` 为 false |
| 打不开文件 | 返回 NULL, 诊断 filename 为 NULL, line/column 为 0 |
| 预处理后 `reconstruct` | 输出展开流, 不是原文件 |
| 内存输入 provenance | 位置显示 `@synth` |
| 文件输入带 BOM | BOM 在第一个 token 的前置 trivia 里 |
| 内存输入带 BOM | BOM 留在第一个 token 的拼写里, 不跳过 |
| 内存输入带 BOM 后接 `int` | `int` 可能被读成标识符的一部分 |
| 内存输入带反斜杠续行 | `\` 成为独立标点, 两行不连接 |
| 内存输入带 `\u00e9` | 读成 `\` 加 `u00e9` |
| 内存输入带孤立 CR | 行号不增加 |
| `#line` | 行号偏移, 文件名不变 |
| 宏体语法错误 | 可能指向 `#define` 行 |
| 给 `const` 赋值 | 不报错 |
| `_Static_assert` | 报语法错误 |
| `#warning` | 报 `invalid preprocessor directive` |
| `asm("nop")` | 解析并把字符串原样发射 |
| `chibicc_preprocess` 之后 | 输入链不再完整 |
| `chibicc_context_failed` | 粘性, 直到 reset |
| `set_predefined_macros` | 下一次 reset 生效 |
| `tokenize_buffer` | 复制输入, 调用者可立即释放 |
| `tokenize_file("-")` | stdin 只读一次, 之后用缓存 |
| `globals` | 逆序, 含匿名对象和 `extern` 声明 |
| `Obj.params` / `locals` | 含隐藏 ABI 对象, 空名字 |
| CST 的 `UNPARSED` | 结构未知, 字节不丢 |
| `.a` 符号 | 内部符号可见 |
| `.so` 符号 | 只导出 D7 |

## 11.15 如何判断一个限制是不是缺陷

遇到不符合预期的行为, 按顺序检查:

1. 是不是第 1 章说的"不做"的事. 例如错误恢复, 跨 TU, 代码生成.
2. 是不是第 2 章的契约边界. 例如 CST 必须在预处理前, reset 后指针失效.
3. 是不是本节的已知限制.
4. 再去看测试: `test/diagnostic.sh`, `test/ast.sh`, `test/cst.sh` 的锁定行为可能已经把当前行为当作预期.
5. 最后才考虑是不是 bug. 如果是, 最小复现应该包含: 输入, 调用序列, 期望, 实际, 以及 `git rev-parse HEAD`.

库的测试很多是逐字节锁定. 修一个"看起来不对"的行为可能同时要改锁定文件, 这不是随意改的, 需要明确这是行为变更还是缺陷修复.
