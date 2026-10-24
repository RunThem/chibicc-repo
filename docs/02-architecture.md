# 02 架构与分层

## 2.1 总流水线

库的流水线按中间表示划分. 第 1 层之后分成两条互不依赖的路径: 一条走 CST, 服务格式化和重构; 一条走预处理, 语法和语义, 服务编译型消费者. 第 5 层在库外.

```text
                源文件 / 内存缓冲
                       |
                       v
              +------------------+
              | tokenize         |  第 1 层
              | LibcpToken 链    |
              +------------------+
                 |            |
       CST 路径  |            |  编译路径
                 v            v
        +----------------+  +-------------------------+
        | cst_parse      |  | preprocess              |  横切: 宏展开,
        | 第 2 层        |  | 展开后的 token 链        |  条件编译, include
        +----------------+  +-------------------------+
                 |            |            |
                 |            |            +--> provenance 链
                 |            v
                 |     +------------------+
                 |     | parse            |  第 3 层
                 |     | 忠实语法 AST      |
                 |     +------------------+
                 |            |
                 |            v
                 |     +------------------+
                 |     | sema             |  第 4 层
                 |     | 标注树 + Obj 链   |
                 |     +------------------+
                 |            |
                 |            v
                 |     +------------------+
                 |     | codegen          |  第 5 层, 库外
                 |     | x86-64 汇编       |
                 |     +------------------+
                 v
        formatter / 重构 / 源级 lint
```

两条路径的输入都是 `tokenize_file()` 的输出. CST 路径必须在预处理之前取走 token 链, 或者对同一个文件做第二次 tokenize. 原因是 `libcp_preprocess` 会原地改写输入链的 `next` 指针, 把展开产物拼接进来.

## 2.2 五层定义

| 层 | 入口 | 输入 | 输出 | 是否在库内 |
|---|---|---|---|---|
| 第 1 层 | `libcp_tokenize_file` / `_buffer` / `_buffer_len` | 文件或内存字节 | `LibcpToken` 链, 带 trivia 与 raw 坐标 | 是 |
| 预处理 | `libcp_preprocess` | 第 1 层的 token 链 | 展开后的 token 链, 带 provenance | 是 |
| 第 2 层 | `libcp_cst_parse` | 第 1 层的 token 链, 预处理之前 | `LibcpCstNode` 树 | 是 |
| 第 3 层 | `libcp_parse` | 预处理后的 token 链 | `LibcpNode` 树 | 是 |
| 第 4 层 | `libcp_sema` | 第 3 层的树 | 原地标注的同一棵树 + `LibcpObj` 链 | 是 |
| 第 5 层 | `codegen()` | 第 4 层的树和 `Obj` 链 | x86-64 GAS 汇编 | 否 |

每一层的"产物"不是抽象概念, 而是具体的内存结构: token 链, CST 树, AST 树, 标注树和全局对象链. 这些结构体在公共头里定义, 字段直接可读.

## 2.3 两条路径为什么分开

CST 和语法 AST 看起来都在描述语法, 但它们的输入不同, 服务对象也不同.

**CST 路径**的输入是预处理之前的原始 token 流:

- 预处理指令本身是节点, 不是被消费掉的文本.
- `#if` 的每个分支都在树里, 即使条件为假.
- `#define` 的宏体保留原始 token 和布局.
- 每个标点是显式叶子, trivia 挂在 token 叶子上.
- 叶子的 `trivia` 加拼写可以逐字节重建输入文件.

它服务的是"要保留源码字节"的工具: 格式化, 注释迁移, 局部重构, 源码索引.

**编译路径**的输入是预处理之后的 token 流:

- 宏调用已经被展开, `#if` 未选中的分支已经消失.
- 语法树描述的是展开后的程序.
- 宏展开的来源由 provenance 提供, 不由语法树提供.

它服务的是"要理解程序"的工具: 类型检查, 语义分析, 代码生成, C 转 C.

一条重要的设计决定是: **不为宏调用建语法节点**. 语法层消费展开流, 宏的原始写法由第 2 层保留, 宏到产物的对应由 provenance 提供. 放弃的是"在宏展开内部做外科式编辑"的能力. 这个决定及理由记录在 `plans/PLAN-provenance.md` 与 `AGENTS.md` 的开放决策一节.

## 2.4 层间契约

每一层都有明确的不变量. 消费者可以依赖这些不变量, 也可以据此判断某个需求该不该由库承担.

### 2.4.1 第 1 层的契约

- 对 `libcp_tokenize_file()` 和 `libcp_tokenize_buffer_len()` 的直接输出, 每个 token 的 `trivia` 加 `raw_loc` 按序覆盖 raw 文本的每一个字节. 唯一的例外是 `make_scan_buffer` 为保证扫描缓冲以换行结束而合成的那个换行, 它不属于文件, 不出现在 raw 坐标里.
- 文件输入时, 第一个 token 的前置 trivia 包含 BOM, 如果文件有 BOM. `libcp_tokenize_buffer*` 不做过改写, BOM 会留在第一个 token 的拼写里, 调用者应自己剥离.
- 最后一个 token 的尾部 trivia 挂在 `TK_EOF` 上.
- token 的 `line_no` 是显示行号, 已经叠加 `#line` 的偏移; `file->lines` 给出物理行号.
- 词法错误经诊断回调报告, 公共入口返回 NULL.

契约只覆盖 `tokenize` 的直接输出. 宏粘贴, 字符串化, 内建宏产生的 token 是合成 token, 它们的 `trivia` / `raw_loc` 指向派生来源或合成缓冲, 不参与逐字节重建.

### 2.4.2 预处理的契约

- 输入是一条 token 链, 输出是另一条 token 链.
- 输入链会被原地改写: `next` 指针被拼接, 展开产物插入其中. 需要原始流或 CST 的消费者必须先取走.
- 输出的每个非 EOF token 都带 `prov` 链, 源 token 的 `prov` 为 NULL.
- 宏展开遵循 hideset 规则, 保证递归宏一定停止.
- 条件编译的未选中分支被丢弃, 不进入输出.
- `#include` 通过文件系统读取, 搜索路径来自 context 配置.
- 错误经回调报告, 入口返回 NULL.

### 2.4.3 第 2 层的契约

- 输入必须是第 1 层的直接输出, 不能是预处理后的流.
- 解析是全函数: 任何输入都返回一棵树, 不报诊断, 不退出.
- 树的叶子序列与输入 token 序列逐指针相等.
- 叶子的 trivia 与拼写按序重建输入文件, 见 `libcp_cst_reconstruct`.
- 文法覆盖不了的跨度成为 `CST_UNPARSED`, 其子节点仍是已经建好的叶子, 不复制 token.
- 条件指令不求值, 宏不展开, 名字不绑定, 类型不计算.

### 2.4.4 第 3 层的契约

- 输入是预处理后的 token 链.
- 表达式节点不做类型计算. 大多数 `LibcpNode.ty` 在第 3 层为 NULL. 两个例外: 数值字面量 `ND_NUM` 带词法阶段的类型, `ND_TYPEDEF` 记录带它声明的类型. 显式 cast 与声明记录的目标类型在 `ty_op`. 其余类型由第 4 层填.
- 名字不绑定. 标识符是 `ND_IDENT`, `var` 为 NULL.
- 常量不求值. `sizeof`, `_Alignof` 的结论不填.
- 声明以记录节点交给 sema: `ND_DECL`, `ND_GVAR_DECL`, `ND_FUNCDEF`, `ND_TYPEDEF`, `ND_ENUM_CONST`.
- 数组维度, `typeof` 操作数, 对齐表达式, 位域宽度都以"待补全"的字段挂在类型或节点上.
- 解析器只保留文法必需的语义反馈: 标识符是不是 typedef 名, tag 指向哪个类型.
- 语法错误经诊断回调报告, 入口返回 NULL. 空翻译单元也返回 NULL, 但 `libcp_context_failed` 为 false.

### 2.4.5 第 4 层的契约

- 输入是第 3 层的树, 原地标注.
- 输出是全局对象链 `LibcpObj *`, 每个函数和全局变量一项, 包含 `extern` 声明与 `__func__` 一类匿名对象.
- 树上的每个表达式节点都有 `ty`.
- `ND_IDENT` 被绑定成 `ND_VAR` 或 `ND_NUM`. `ND_STRING` 被改写成指向匿名全局的 `ND_VAR`. 这是第 4 层仅有的节点种类改写.
- 声明记录保留在语句链里, 供消费者决定怎么展开. sema 不把记录变成语句.
- 编译期结论记录在节点字段上: `ND_SIZEOF` / `ND_ALIGNOF` 的 `val`, `ND_GENERIC` 的 `generic_sel`, 内建类型谓词的 `val`.
- 隐式 cast 由 sema 插入, 带 `is_implicit = true`.
- 初始化器记录被解析成 `LibcpInitTree`, 指针放在 `init_resolved`. 该字段是 sema 私有载荷, 消费者不得读写.
- 不做指针缩放, 不做复合赋值展开, 不做控制流标签, 不做临时槽. 这些是第 5 层的.

### 2.4.6 第 5 层的契约

第 5 层不是库. 它消费第 4 层的树, 自己做整形和发射. 它的存在证明第 4 层的产物足够支撑一个真实后端, 但不构成公共 API 承诺. 详见第 8 章.

## 2.5 生命周期与内存所有权

### 2.5.1 三态

```c
LibcpContext *ctx = libcp_context_new();   /* 建 */
/* ... 一个 TU 的工作 ... */
libcp_context_reset(ctx);                    /* 复用, 保留配置 */
/* ... 下一个 TU ... */
libcp_context_free(ctx);                     /* 销毁 */
```

- `libcp_context_new` 创建一个空 context. 默认安装预定义宏集. **不安装任何 include 搜索路径**, 系统头路径由消费者自己加.
- `libcp_context_reset` 释放 arena 中的全部 per-TU 对象, 清空文件表, 宏表, 条件栈, 作用域和语义状态, 重新安装预定义宏, 重新应用记录下来的 `-D` / `-U` 操作. 保留 include 路径, 诊断回调和"是否启用预定义宏"的设置.
- `libcp_context_free` 在 reset 的基础上释放配置数组和 context 本身.

### 2.5.2 指针失效规则

**reset 之后, 该 context 返回过的所有指针立即失效**, 包括 token, file, AST 节点, 类型, Obj, CST 节点, 诊断消息字符串, 宏名字字符串.

库不做悬垂指针检测. 消费者要么在 reset 前用完, 要么自己复制需要保留的数据. 诊断回调收到的 `filename` 和 `message` 也只保证到下一次 reset.

### 2.5.3 arena

一个 context 一个块链 arena. 所有 per-TU 对象从 arena 分配:

- token, file, 各种数组
- AST 节点, 类型, 成员, 对象
- CST 节点与 children 数组
- 初始化器记录与初始化树
- 宏展开记录与 provenance 节点
- 诊断消息, 中间字符串

分配策略:

- 块大小 64 KiB, 按需追加. 块本身从 `<libcp/allocator.h>` 的 `LIBCP_*` 宏取得, 默认是 libc, 可在重编库时替换.
- `calloc` 语义, 返回的内存零初始化.
- 对齐至少 8 字节; 零初始化分配用 16 字节, 因为 `Token` 和 `Node` 内含 `long double`.
- `realloc` 复制到新块, 旧块不回收. 因此扩容的峰值约为最终大小的两倍.
- 不提供逐对象 free. 不做 per-phase arena.

这条设计是刻意的: 库的调用者不需要追踪每个对象的生命周期, 只需要在 TU 边界 reset. 代价是内存峰值和"不能只释放一半"的限制.

## 2.6 错误模型

### 2.6.1 诊断回调

```c
typedef struct libcp_diagnostic {
  const char *filename;   /* 可能为 NULL */
  int line;               /* 可能为 0 */
  int column;             /* 可能为 0 */
  LibcpDiagnosticSeverity severity;
  const char *message;
} LibcpDiagnostic;

typedef void (*LibcpDiagnosticFn)(void *user, const LibcpDiagnostic *);
```

注册方式:

```c
libcp_context_set_diagnostic(ctx, on_diagnostic, user);
```

- 注册回调后, 库不向 stderr 打印任何东西.
- 没有注册回调时, 库按驱动器旧格式打印 `file:line: 源码行` 和插入符, 然后返回或退出.
- `error()` 一类没有位置的错误, `filename` 为 NULL, `line` 和 `column` 为 0. 例如文件打不开.

### 2.6.2 返回与展开

每个会报告错误的公共入口都是一个错误边界:

- 错误经回调报告后, 库 longjmp 回当前公共入口.
- 入口返回自己的失败值: 指针函数返回 NULL, `bool` 函数返回 false, `void` 函数直接返回.
- context 的 `failed` 标志置位, 直到 reset.

`libcp_context_failed(ctx)` 是判断"上一次调用是成功还是出错"的可靠方式. 返回 NULL 本身不一定是错误: 空翻译单元的 `libcp_parse` 也返回 NULL, 但 `failed` 为 false.

一旦出错, context 处于不一致状态. 库不保证部分树可用. 复用前必须 reset.

### 2.6.3 警告

警告走同一个回调, `severity` 为 `LIBCP_DIAG_WARNING`. 警告不中断流程, 不设置 `failed`. 目前只有数组初始化器 excess elements 一类警告.

## 2.7 线程模型

契约是三句话:

1. **一个 context 只能在一个线程里使用**, 不能两个线程同时调用同一个 context.
2. **不同 context 可以在不同线程并存**.
3. **同一线程可以嵌套调用公共入口**.

实现方式是显式传参: 每个直接或传递地触碰 arena, 诊断, 格式化, 容器分配或 context 字段的内部函数都以 `LibcpContext *ctx` 作为首参, 由公共入口把自己的 context 传下去. 库层没有线程局部指针, 也没有 `libcp_ctx()` 访问器; `make check-context` (即 `test/context-audit.sh --strict`) 在源码与 `.a` 符号两层锁定这一点.

这意味着:

- 你不能在一个 context 上并发.
- 你可以在线程 A 创建 context, 在线程 B 使用它, 只要不同时用.
- 你可以在同一线程交替使用多个 context: 每次公共入口只认自己收到的那个指针.
- 诊断回调里创建, 配置, 使用并释放另一个 context 是安全的; 外层 `raise_error` 用的是外层 context 自己的 frame, 不依赖任何全局状态.

`libcp_context_new`, `reset` 和 `free` 只操作自己的 context, 不修改任何全局指针.

## 2.8 如何选择层

按需求从低到高选, 不要为了"以后可能用得上"直接上第 4 层. 越高层, 内存和输入约束越多.

```text
要保留注释/空白/宏/未选中分支?
  是 -> 第 1 层 + 第 2 层
  否 -> 要继续

要只做语法模式匹配, 不需要类型?
  是 -> 第 3 层
  否 -> 要继续

要类型, 名字绑定, 常量, 结构体布局?
  是 -> 第 4 层
  否 -> 回到上一层

要自己生成代码?
  是 -> 第 4 层 + 自己的后端
```

几个具体判断:

- **只想拿 token**: 第 1 层. 不要预处理, 否则宏会消失.
- **想重排空白但保留注释**: 第 2 层. 不要用第 3 层, 第 3 层没有注释.
- **想改写某个函数体但保留其他字节**: 第 2 层定位 span, 再手工替换. 第 3 层没有源码范围.
- **想知道一个表达式的类型**: 第 4 层. 但注意第 4 层会改写 `ND_IDENT` / `ND_STRING`, 需要原始形状就先复制或先 dump.
- **想追踪宏**: 预处理 + provenance. 不要指望语法树上有宏调用节点.

## 2.9 参考消费者

仓库里的编译器由两个文件组成, 不是库的一部分:

- `main.c` 是驱动器: 命令行解析, 文件调度, `-E` / `-S` / `-c` 模式, 链接器调用, 以及若干只读调试通道.
- `codegen.c` 是 x86-64 代码生成器: 整形遍加发射器, 直接消费第 4 层的树.

驱动器的 `cc1()` 展示了库的标准用法:

```c
LibcpToken *tok = libcp_tokenize_file(ctx, path);
tok = libcp_preprocess(ctx, tok);
LibcpNode *ast = libcp_parse(ctx, tok);
LibcpObj *prog = libcp_sema(ctx, ast);
codegen(ctx, prog, out);
```

其中 `codegen()` 是驱动器自己的函数(声明在 `driver.h`), 不属于库 - 库的产物到第 4 层为止.

只读通道在 `cc1()` 里早退, 不写目标文件, 不调用汇编器:

| 通道 | 层 | 作用 |
|---|---|---|
| `-dump-tokens` | 第 1 层 | 打印原始 token 与 trivia |
| `-reconstruct` | 第 1 层 | 按 token 流重建原始字节 |
| `-dump-cst` | 第 2 层 | 打印 CST 树 |
| `-reconstruct-cst` | 第 2 层 | 按 CST 叶子重建原始字节 |
| `-verify-cst` | 第 2 层 | 校验叶子与 token 流逐指针相等 |
| `-dump-expansions` | provenance | 打印每个产物 token 的展开链 |
| `-verify-expansions` | provenance | 校验展开链的不变量 |
| `-dump-ast` | 第 3 层 | 打印忠实语法树, 在 sema 之前早退 |

这些通道是理解库行为的最好工具. 本章后面的例子会反复用到.

## 2.10 目录与文件地图

```text
libcp/
  include/libcp/
    libcp.h        伞头, 唯一需要 include 的文件
    context.h        context, 诊断回调, 配置, 预定义宏
    diagnostic.h     LibcpDiagnostic 与 severity
    token.h          LibcpToken, LibcpFile, 第 1 层入口
    preprocess.h     libcp_preprocess
    provenance.h     宏展开访问器与两个只读通道
    cst.h            第 2 层: LibcpCstKind, LibcpCstNode
    ast.h            第 3/4 层: Node, Type, Obj, 初始化器, 类型种类
    sema.h           libcp_sema
  src/
    tokenize.c       词法, trivia, raw 坐标, 诊断格式化
    preprocess.c     指令, 宏展开, include, provenance 记录
    provenance.c     展开链的 dump / verify
    cst.c            lossless CST 的建树, dump, verify
    parse.c          递归下降解析器
    sema.c           名字解析, 类型标注, 常量求值, 布局, 初始化器
    type.c           类型构造器与谓词
    lib.c            facade, context, arena, 诊断边界
    hashmap.c        字符串驻留哈希表
    strings.c        字符串辅助
    unicode.c        UTF 编码
    libcp.h        内部伞头, 不对外
main.c               驱动器, 参考消费者
codegen.c            x86-64 代码生成器, 参考消费者
driver.h             驱动器内部头
test/                测试, 见第 10 章
plans/               历史计划与执行记录
include/             chibicc 自举头文件, 通过 -Iinclude 使用
```

公共头只包含标准头和彼此, 不包含 `libcp/src/libcp.h`, 不声明内部函数, 不定义 `_POSIX_C_SOURCE`, 不包含 `glob.h` 或 `sys/wait.h`. 每个公共头都能单独用 `cc -std=c11 -fsyntax-only -Wall -Wextra` 编译. 这条卫生规则由 `test/public-api.sh` 锁定.
