# RESULT.md - CST 执行记录

本文件记录 `PLAN.md`(CST 线, 路线图阶段 2)各步骤的实际执行结果, 供审核与后续会话接续参考.
前四条线已归档: 语法语义拆分见 `PLAN-split.md` / `RESULT-split.md`, 忠实层收尾见
`PLAN-faithful.md` / `RESULT-faithful.md`, codegen 直连见 `PLAN-codegen-direct.md` /
`RESULT-codegen-direct.md`, trivia 保留见 `PLAN-trivia.md` / `RESULT-trivia.md` - 后者记录的
终态(443b4c2)是本线的量化对比基线.

本线的目标与边界见 `PLAN.md`: 在预处理前的原始文本上建 lossless CST, 叶子按序与 token 流
一一对应, 重建逐字节等于输入文件; CST 只走 `-dump-cst` / `-reconstruct-cst` / `-verify-cst`
三个只读通道, 不接编译路径.

## 闸门口径(本线生效)

- 每步(均为行为不变步): `make docker-test`(含自举与诊断 55 例 + 形状 33 段) +
  `make docker-snapshot-diff` 为空(raw 逐字节) + `test/cst.sh` 该步新增的断言.
- 本线不接编译路径, 预期快照全程为空, 不重置基线; 若出现 diff, 先查根因, 不视为计划内变化.
- 本地 A/B 规程(出现无法归因的差异时): `git worktree add /tmp/cbase 443b4c2`, 两份二进制
  对同一输入逐字节对照 `-E` / `-S` / dump 输出.
- 一步一提交; 每步汇报四项(改了什么 / 为什么 / 闸门结果 / 偏差).

## 基线记录(C0.1, 复验于 fe3c56b)

- 起始状态: HEAD = fe3c56b(线间归档提交; 代码与其父 443b4c2 相同, 只动文档), tokenize.c 924 行,
  chibicc.h 864 行, main.c 959 行, preprocess.c 1208 行, parse.c 2148 行, sema.c 2992 行,
  type.c 157 行, codegen.c 2677 行.
- docker-test(`make docker-test`): EXIT=0. 41 个 test/*.c 特性测试在 stage1/stage2 两遍全过
  (日志 82 行 `.exe`), driver.sh / diagnostic.sh(55 例逐字节) / shape.sh(33 段) /
  trivia.sh(7 -E + 23 dump + 48 round-trip + 12 边界 + 4 锚点)在 ./chibicc 与
  ./stage2/chibicc 上各跑一遍, 自举完成. 日志 `.cache/C0.1-docker-test.log`.
- snapshot-diff(`make docker-snapshot-diff`): `snapshot diff: empty`(raw 逐字节, 41 文件,
  0 条 diff). 基线 .cache/snapshot 为 B2 终态重置的快照, 本线预期全程不重置.
  日志 `.cache/C0.1-snapdiff.log`.
- tinycc(`make docker-test-thirdparty THIRDPARTY=tinycc`): EXIT=0, 0 条失败记账
  (Rosetta shim 照旧跳过 106_pthread/112_backtrace/113_btdll). 日志
  `.cache/C0.1-tinycc.log`.
- A/B worktree: `git worktree add /tmp/cbase 443b4c2` 已建(旧的可裁剪记录先 prune);
  两份二进制可逐字节对照 `-E` / `-S` / dump 输出.
- 语料清单核对: `test/*.c` 41 个 + `include/*.h` 7 个 = 48, 与 trivia.sh 的全量循环一致;
  12 个边界样本按 trivia.sh 的 exp_rec 清单复用到 cst.sh (CRLF / 孤立 CR / 无末行换行 /
  BOM / 续行 / 续行落在标识符 / 字符串 / universal chars / 续行 + universal chars /
  CRLF + 续行 + 无末行换行 / 深层混合 / 空文件).
- 既有闸门计数: 诊断 55 例, 形状 33 段, trivia 94 项断言.

## CST 设计账本(开工前分析, 实施时逐项核对)

### 输入事实(token 流已经提供什么)

| # | 事实 | 位置 | CST 的用法 |
|---|---|---|---|
| 1 | 每个 token 的前置被跳过字节 `trivia` / `trivia_len` | `chibicc.h:106-107` | 叶子的文本前半; 重建原样输出 |
| 2 | 每个 token 的原拼写 `raw_loc` / `raw_len`(raw 坐标) | `chibicc.h:108-109` | 叶子的文本后半; 重建原样输出 |
| 3 | EOF 携带文件尾部 trivia, `raw_len = 0` | `tokenize.c:703-705` | 根节点末尾放一个 `CST_EOF` 叶子, 重建统一 |
| 4 | `at_bol` 标记行首(块注释内换行与续行不置位) | `tokenize.c:541-707` | 指令起点与指令行边界 |
| 5 | `has_space` 标记前置空白 | `tokenize.c:541-707` | 函数宏与对象宏的分界(名字后紧跟 `(`) |
| 6 | `line_no` 由 `File.lines` 按 raw 偏移给出 | `tokenize.c:512-515` | dump 与调试; 不影响重建 |
| 7 | `equal()` 只按拼写比较, 与 kind 无关 | `tokenize.c:125-127` | 预处理前关键字是 `TK_IDENT`, 全用拼写判断 |
| 8 | 预处理前的 token 未跑 `convert_pp_tokens` | `tokenize.c:502-509`, `preprocess.c:1198-1207` | CST 不调用预处理器, 保持与 tokenize 同一流 |

结论: CST **不需要给 `Token` / `File` 增加任何字段**, 也不需要在 `tokenize.c` 里留钩子;
`cst.c` 只消费既有字段.

### 歧义清单(预处理前不可判定或不可解析的构造)

| # | 构造 | 为什么歧义 | 默认策略 | 锁定步 |
|---|---|---|---|---|
| 1 | `T *x;` vs `a * b;` | `T` 是否 typedef 名在预处理前不可知 | `T` 是类型关键字或文件内已见 typedef 名 -> 声明; 否则表达式 | C2.2 |
| 2 | `(T)x` | cast 与括号表达式同形 | `T` 已知为类型 -> `CST_CAST`; 否则 `CST_PAREN`; 后续无法衔接 -> `CST_UNPARSED` | C2.1 |
| 3 | `sizeof(T)` / `_Alignof(T)` | 类型名与表达式同形 | 同 2 的判据; 无法判定时保留括号内为表达式 | C2.1 |
| 4 | `for (T x = ...; ...)` | 初始化子句可为声明或表达式 | 同 1 的判据 | C2.2 |
| 5 | `T(x);` | 宏调用与声明同形 | `T` 未知 -> 调用表达式; `T` 已知为类型 -> 声明 | C2.2 |
| 6 | `size_t x;`(来自 include 的类型) | 类型名不在本文件可见 | 表达式解析失败 -> 整条语句 `CST_UNPARSED`, 无损保持 | C2.2 |
| 7 | `#if` 条件 | 条件求值属于预处理器, 不属于 CST | 不求值, 条件 token 原样保留 | C3.1 |
| 8 | `#define` 替换列表 | 可含任意 token, 可跨续行 | 只保留 token 序列, 不解析成表达式 | C3.1 |
| 9 | 孤立 `#elif` / `#else` / `#endif` | 畸形输入, 预处理器会报错退出 | CST 不报错: 保留为指令节点或 `CST_UNPARSED`, 仍可重建 | C3.1 |
| 10 | 未闭合的 `(` / `{` / `#if` | 畸形输入 | 保留已消费叶子, 剩余跨度以 `CST_UNPARSED` 收尾, 仍可重建 | C1.2 / C3.1 |

原则: 歧义决定的是**树形**, 不是**无损性**; 任何策略下叶子序列都必须与 token 流一致.

### 节点种类草案(名称在实施时可微调, 范畴不变)

- 根 / 叶子 / 回退: `CST_TRANSLATION_UNIT`, `CST_TOKEN`, `CST_EOF`, `CST_UNPARSED`.
- 骨架(C1): `CST_GROUP_PAREN`, `CST_GROUP_BRACKET`, `CST_GROUP_BRACE`, `CST_STMT`,
  `CST_BLOCK`, `CST_NULL_STMT`.
- 声明(C2.2): `CST_DECL`, `CST_DECL_SPECS`, `CST_DECLARATOR`, `CST_INIT_DECLARATOR`,
  `CST_INITIALIZER`, `CST_FUNC_DEF`, `CST_PARAM_LIST`, `CST_TYPE_NAME`, `CST_ATTRIBUTE`.
- 语句(C2.2): `CST_IF`, `CST_SWITCH`, `CST_WHILE`, `CST_DO`, `CST_FOR`, `CST_CASE`,
  `CST_DEFAULT`, `CST_LABEL`, `CST_GOTO`, `CST_BREAK`, `CST_CONTINUE`, `CST_RETURN`,
  `CST_ASM`, `CST_EXPR_STMT`.
- 表达式(C2.1): `CST_BINARY`, `CST_UNARY`, `CST_POSTFIX`, `CST_CALL`, `CST_SUBSCRIPT`,
  `CST_MEMBER`, `CST_CAST`, `CST_COND`, `CST_ASSIGN`, `CST_COMMA`, `CST_PAREN`,
  `CST_PRIMARY`, `CST_ARG_LIST`, `CST_INIT_LIST`, `CST_COMPOUND_LITERAL`.
- 指令(C3.1): `CST_PP_DIRECTIVE`, `CST_PP_DEFINE`, `CST_PP_UNDEF`, `CST_PP_INCLUDE`,
  `CST_PP_LINE`, `CST_PP_PRAGMA`, `CST_PP_ERROR`, `CST_PP_NULL`, `CST_PP_IF`,
  `CST_PP_BRANCH`.
- 每个节点持 `CstKind`, 子节点数组(`children` / `child_count` / `child_cap`); 叶子额外持
  `Token *tok`; 非叶子的 `tok` 为 `NULL`. 不放 `Node` / `Type`, 不放类型与绑定.

树形示例(以 `int x = 1 + 2;` 为例, 叶子文本来自 `Token`):

```
TU
  DECL
    DECL_SPECS
      TOKEN "int"
    INIT_DECLARATOR
      DECLARATOR
        TOKEN "x"
      TOKEN "="
      BINARY
        TOKEN "1"
        TOKEN "+"
        TOKEN "2"
    TOKEN ";"
  EOF
```

### 指令分组规则(C3.1)

1. 指令起点 = `at_bol && "#"`(`preprocess.c:76-78` 的 `is_hash` 语义); 不以 `at_bol` 开头的
   `#` 是普通标点, 不进指令节点.
2. 指令行 = 从 `#` 到下一个 `at_bol` token 之前. 扫描缓冲已删除续行, 续行后的 token 不置
   `at_bol`; 块注释内的换行也不置 `at_bol` - 两者都与 tokenizer 的现状一致, 不要另行按
   物理行切.
3. `#define`: 名字后紧跟 `(` 且 `!has_space` 才是函数宏(`preprocess.c:368-386`); 参数表到
   匹配的 `)`, 支持 `...` 与变参名; 替换列表 = 行内剩余 token, 不解析.
4. `#include`: `TK_STR` 形, `<` ... `>` 形, 或其余 token 序列; 路径 token 原样保留,
   不解释转义, 不查文件.
5. `#if` / `#ifdef` / `#ifndef`: 建 `CST_PP_IF`, 后续 `#elif` / `#else` 切成
   `CST_PP_BRANCH`, `#endif` 收尾; 分支体照常建声明 / 语句 / 指令子树; 条件 token 原样,
   不求值. 嵌套用递归.
6. `#pragma`(含 `once`), `#line`(含 GNU 行标记 `# 123 "file"`), `#error`, `#undef`, 空指令
   `#`: 各自独立节点或指令行节点; 行内多余 token 全部保留.
7. 孤立或错配的条件指令不报错: 保留为指令节点或 `CST_UNPARSED`; 无损优先.

### 不变量(每一步都测)

- **I1 叶子序列 == token 流**: 对 `tokenize_file` 的输出, 树的中序叶子指针序列逐项相等
  (含 EOF).
- **I2 重建 == 原文件**: 按叶子序写 `trivia` + 拼写, 48 个语料文件与 12 个边界样本逐字节
  相等, 0 例外.
- **I3 每个 token 恰好一次**: 不允许重复挂载或漏挂; 失败回溯产生的死节点不挂进树.
- **I4 解析不改变文本**: 任何结构决策只影响分组, 不影响叶子的顺序与文本.
- **I5 无静态状态**: `cst.c` 不新增文件域 static; 解析 context 在栈上; 内存走 arena.
- **I6 无新 exit 点**: `cst.c` 不调用 `error` / `error_at` / `error_tok`; 校验失败由
  `-verify-cst` 报出, 驱动返回非零.

## 关键结论(开工前分析, 供审核者速览)

- 无损性已经在 trivia 线完成; 本线只加**结构**, 不改文本. 因此最大的风险不是字节, 而是
  "为了结构而丢 token"; I1 / I3 与全量重建就是为此设的闸门.
- 预处理前的 C 文法天然不可完全消歧(typedef 名, include 来的类型, 宏). 本线不试图用语义
  补洞: 文件内 typedef 集合 + 类型关键字拼写 + 回溯是上限, 超出即 `CST_UNPARSED`. 这既
  符合第 2 层"不查名字, 不算类型"的边界, 也保证 formatter 拿到的是可逐字节打印的树.
- CST 不接编译路径, 所以 `make docker-snapshot-diff` 必须全程为空; 任何 `.s` 差异都说明
  改动泄漏进了既有消费者, 按回归处理.
- `cst.c` 是唯一的新源文件; `main.c` 只加三个只读旗标; `chibicc.h` 只加 CST 类型与声明.
  这条边界让阶段 5 的库化可以直接把 CST 暴露为公共能力, 而不需要再拆逻辑.

## C1.1 叶子树与三个只读通道(已提交)

- 改了什么:
  - `chibicc.h`: 在 tokenize.c 与 preprocess.c 之间新增 cst.c 段 - `CstKind`
    (`CST_TRANSLATION_UNIT` / `CST_TOKEN` / `CST_EOF`) 与 `CstNode`(kind / tok /
    可增长 children 数组), 四个函数声明. `Token` / `File` 一个字段未动.
  - `cst.c`(136 行): `cst_parse` 建 `TU -> TOKEN* + EOF` 的扁平叶子脊(EOF 单独成
    kind, 因为它收文件尾部 trivia 且没有拼写); `cst_reconstruct` 按叶子序写
    trivia + 拼写; `cst_dump` 两空格缩进打印种类与转义后的原始拼写; `cst_verify`
    按叶子序与 token 流逐指针比较, 并检查叶子不带子节点. 全函数: 不调用
    `error*`, 无文件域 static, arena 分配, 只返回结果.
  - `main.c`: 新增 `-dump-cst` / `-reconstruct-cst` / `-verify-cst` 三个旗标, 共用
    `-dump-tokens` 的早退分支(只 tokenize, 不预处理)与驱动透传; `cc1` 由 void 改为
    int, `main` 的 opt_cc1 分支改为 `return cc1()`, 使校验失败以非零退出(驱动侧由
    `run_subprocess` 传播). 其余分支行为不变.
  - `Makefile`: `test/cst.sh` 挂进 `make test` 与 `make test-stage2`.
  - `test/cst.sh`(新): 48 文件 `-reconstruct-cst` 全量 round-trip + 48 文件
    `-verify-cst` + 12 个边界样本(每个既查重建也查校验)+ 5 个叶子 dump 样本 +
    EOF 收尾 trivia 的重建, 共 126 项断言.
- 为什么: 第 2 层的无损合同先以最小树形落地 - 叶子就是 token 流本身, 结构后加
  (判据 6); 三个只读通道给后续每一步提供 round-trip / 不变量 / 树形三种证据.
- dump 口径(冻结): 每行 `<两空格缩进><KIND>`; 叶子再跟一个空格与转义后的 raw 拼写
  (与 `-dump-tokens` 同一规则: `\n` `\t` `\r` `\\`, 其余不可见字节与空格写成
  `\xHH`); EOF 无文本; 不加引号 - 空格已转义, 行内不存在歧义. `-verify-cst` 成功
  时不输出, 失败时由驱动返回 1.
- 闸门: `make docker-test` EXIT=0(日志 `.cache/C1.1-docker-test.log`; cst lock
  在 ./chibicc 与 ./stage2/chibicc 各一遍, 诊断 55 / 形状 33 / trivia 94 不变);
  `make docker-snapshot-diff`: `snapshot diff: empty`(日志
  `.cache/C1.1-snapdiff.log`). 本地负向探针: 临时让 `cst_verify` 恒假, 驱动与
  `-cc1` 两条路径都返回 1, 已还原.
- 偏差: `cc1` 的返回类型改动超出"main.c 只增旗标"的字面边界, 但判据 5 要求校验失败
  时驱动返回非零且不调用 `exit`, 这是满足该条的最小改动. dump 用无引号形式, 与
  PLAN 树形示例的引号写法不同 - 示例只示意分组, 不是格式合同.

## C1.2 定界符与语句骨架(已提交)

- 改了什么:
  - `chibicc.h`: `CstKind` 增 `CST_GROUP_PAREN` / `CST_GROUP_BRACKET` /
    `CST_GROUP_BRACE` / `CST_BLOCK` / `CST_STMT` / `CST_NULL_STMT` / `CST_UNPARSED`.
  - `cst.c`: 语句层切分(顶层与块内): 每项是 `;` 结尾的 `STMT`, 孤立 `;` 的
    `NULL_STMT`, 或 `{` 开头的 `BLOCK`; 语句内 `(` `[` `{` 递归成原始组, 组内不
    切语句(`for (;;)` 的分号是叶子); 缺闭括号的组 / 块与语句层无开括号的孤立
    闭括号成为 `UNPARSED`, 已消费 token 全保留. 仍是全函数, 无诊断.
  - `test/cst.sh`: 第二段 9 个骨架 dump(语句切分 / 嵌套括号 / for 头 / 块 / 嵌套
    块 / 语句内花括号 / 未闭合括号 / 未闭合块 / 孤立闭括号), 每个 dump 样本同时
    跑 `-verify-cst` 与重建; C1.1 的 5 个 dump 期望随 `STMT` 包装更新. 全脚本
    158 项断言, 汇总行 14 个 dump 样本.
- 为什么: 先固定"定界符不错配, 语句边界可切"的骨架, 让 C2 的表达式与声明文法
  有一个可靠的容器; 组内不切语句是刻意的 - 骨架阶段无法区分 `for` 头与普通括号.
- 口径与限制: 语句层只覆盖顶层与块内. 函数体的 `{` 出现在语句中段, C1.2 按原始
  `GROUP_BRACE` 处理(没有声明文法时无法与初始化器花括号区分); C2.2 的 `FUNC_DEF`
  再把函数体落成 `BLOCK`. 这是判据 6 的 best-effort 结构, 不影响无损.
- 闸门: `make docker-test` EXIT=0(日志 `.cache/C1.2-docker-test.log`; cst lock
  158 项在 ./chibicc 与 ./stage2/chibicc 各一遍); `make docker-snapshot-diff`:
  `snapshot diff: empty`(日志 `.cache/C1.2-snapdiff.log`).
- 偏差: 无. 计划里的 `CST_GROUP` 三种括号与 `CST_BLOCK` / `CST_STMT` /
  `CST_NULL_STMT` / `CST_UNPARSED` 全部落地; dump 名称用去掉 `CST_` 前缀的全名.

## C2.1 表达式结构(已提交)

- 改了什么:
  - `chibicc.h`: `CstKind` 增 `CST_EXPR_STMT` / `CST_BINARY` / `CST_UNARY` /
    `CST_POSTFIX` / `CST_CAST` / `CST_COND` / `CST_ASSIGN` / `CST_COMMA` /
    `CST_PAREN` / `CST_PRIMARY` / `CST_ARG_LIST` / `CST_INIT_LIST` /
    `CST_COMPOUND_LITERAL`; 移除 `CST_STMT` - C1.2 的裸语句回退并入 `CST_UNPARSED`
    (与计划 C2.1 的措辞一致, 避免两个含义重叠的回退种类).
  - `cst.c`(1016 行): 表达式递归下降 - 10 级左结合二元链, 右结合赋值与条件
    (含 GNU elvis), 逗号; 前缀一元 / 后缀链(调用, 下标, `.` `->`, `++` `--`);
    `sizeof` / `_Alignof` / `typeof` 的类型名与表达式两种写法; cast 与复合字面量;
    `PRIMARY`(标识符 / 数字 / 字符 / 相邻字符串); `ARG_LIST`; `INIT_LIST`(嵌套
    花括号, 不带指派 - 指派属 C2.2). 每个运算符, 括号与逗号都是显式叶子.
  - 歧义判据: `(` 后首 token 是类型关键字(`void` / 整数浮点 / 限定符 /
    `struct` `union` `enum` / `typeof` / `_Atomic` 等 GNU 拼写)或**文件内已见
    typedef 名**时按 cast / 类型名; 否则按括号表达式. typedef 集合由一遍保守扫描
    收集(简单声明, 指针, 数组, 多声明符, 函数指针, 结构体 typedef; 成员名与参数名
    不收). include 来的名字不猜.
  - 语句接入: 整条语句能被 "表达式 `;`" 覆盖时成为 `CST_EXPR_STMT`; 失败则整段
    挂 `CST_UNPARSED`, 子节点是 C1.2 的原始骨架(到 `;` 的 token 与定界符组).
  - `test/cst.sh`: 第三段 15 个 dump(整条优先级链, 赋值链, 显式括号, 调用 / 下标 /
    成员 / 后缀自增, 条件与 elvis, 逗号, 一元链, sizeof 类型与表达式, typedef cast,
    结构体 typedef, 未知名字, 复合字面量, 字符串拼接), 每个同时查 verify 与重建.
    全脚本 203 项断言, 29 个 dump 样本.
- 为什么: 表达式是 formatter / 源到源工具消费面最大的一层; 树形只分组, 不改写
  源码写法(`a[i]` 与 `*(a+i)` 各自照原样), 也不做类型与常量结论.
- 口径与限制: 声明在 C2.2 之前一律是 `UNPARSED`(表达式文法解释不了). `T(x);` 在
  有 typedef T 时当前按调用表达式; C2.2 的声明优先规则会把它改判为声明. 函数体的
  `{` 仍是 `GROUP_BRACE`(见 C1.2 的限制).
- 开发中修复: 关键字 / 类型 / 赋值运算符表初版漏了 NULL 终止符, ASan 报
  stack-buffer-overflow(未提交); 补上后全量 ASan 扫描(48 文件 + cst.sh)无
  AddressSanitizer 报告. 扫描时关掉 memcmp 拦截: `equal()` 按 token 长度从短
  字符串读是既有 tokenize.c 行为, 红线内不改.
- 闸门: `make docker-test` EXIT=0(日志 `.cache/C2.1-docker-test.log`; cst lock
  203 项在 ./chibicc 与 ./stage2/chibicc 各一遍); `make docker-snapshot-diff`:
  `snapshot diff: empty`(日志 `.cache/C2.1-snapdiff.log`).
- 偏差: `CST_EXPR_STMT` 在 C2.1 就落地(计划列在 C2.2 的语句清单), 因为表达式
  解析已经产生成功的语句形态, 让裸回退与成功形态分开是 C2.1 的自然结果. 其余节点
  按计划 C2.1 的清单落地; 指派初始化器按计划留到 C2.2.

## C2.2 声明与语句结构(已提交)

- 改了什么:
  - `chibicc.h`: `CstKind` 增 `CST_DECL` / `CST_DECL_SPECS` / `CST_DECLARATOR` /
    `CST_INIT_DECLARATOR` / `CST_FUNC_DEF` / `CST_PARAM_LIST` / `CST_PARAM` /
    `CST_TYPE_NAME` / `CST_INITIALIZER` / `CST_DESIGNATOR` 与 12 个语句种类
    (`CST_IF` / `CST_SWITCH` / `CST_WHILE` / `CST_DO` / `CST_FOR` / `CST_CASE` /
    `CST_DEFAULT` / `CST_LABEL` / `CST_GOTO` / `CST_BREAK` / `CST_CONTINUE` /
    `CST_RETURN` / `CST_ASM`).
  - `cst.c`(2037 行): 声明层 - decl-specs(存储类 / 限定符 / 内建类型 / struct /
    union / enum / typeof / _Atomic / _Alignas / 属性), declarator(指针与限定符,
    括号内层声明符, 名字, 参数表, 数组维度, 属性), 参数(CST_PARAM), type-name
    (cast / sizeof / 复合字面量), 声明列表与函数定义; 初始化器带指派(`[expr]`,
    `[a ... b]`, `.name`)与嵌套花括号, 每个元素是 CST_INITIALIZER. 语句层 - 上述
    12 种语句 + 表达式语句 + 块; 语句关键字, 括号, 分号都是显式叶子.
  - 歧义判据: 首个 token 是声明起始(存储类 / 限定符 / 内建类型 / struct / union /
    enum / typeof / _Atomic / _Alignas / 文件内 typedef 名)时才尝试声明, 否则按
    表达式; 两个方向都失败才回退 `UNPARSED`. `T *x;` 在 typedef 已知时是声明, 未知
    时是乘法; `T(x);` 已知时是声明, 未知时是调用. `size_t x;`(include 来的未知
    类型)不猜, 整条 `UNPARSED`.
  - `test/cst.sh`: 第四段 19 个 dump(标量 / 初始化 / 多声明符 / 函数指针 / 数组 /
    结构体 / 指派与嵌套初始化 / 字段指派 / 函数定义 / if / while-do / for / switch /
    标签与 goto / asm / 四个歧义样本)与相应重建; C1.1 / C1.2 / C2.1 中受结构升级
    影响的期望同步更新(裸声明 -> DECL, cast 的类型部分 -> TYPE_NAME, 初始化列表
    -> INITIALIZER). 全脚本 260 项断言, 48 个 dump 样本.
- 为什么: 声明与语句是 C 前端消费者(formatter / C→C)的主要结构面; 这一层把
  文法可判的部分全部定位, 把歧义留给 `UNPARSED`, 不引入名字绑定与类型结论.
- 口径与限制: struct / union / enum 的成员体、typeof / _Atomic / _Alignas 的
  参数、asm 操作数保持原始组(成员声明的细分留给后续消费者或线); K&R 标识符参数表
  回退原始组; include 来的类型不猜; 函数体里的 typedef 名遮蔽参数名属于语义问题,
  CST 不做绑定(判据 6 的 best-effort).
- 开发中校验: 未提交前用 ASan 构建跑遍 48 个语料的重建 / 校验 / dump 与全部
  cst.sh, 无 AddressSanitizer 报告(`intercept_memcmp=0`, 原因同 C2.1).
- 闸门: `make docker-test` EXIT=0(日志 `.cache/C2.2-docker-test.log`; cst lock
  260 项在 ./chibicc 与 ./stage2/chibicc 各一遍); `make docker-snapshot-diff`:
  `snapshot diff: empty`(日志 `.cache/C2.2-snapdiff.log`).
- 偏差: `CST_PARAM` 是计划清单之外新增的种类 - 参数需要把 specs 与 declarator
  成组, 否则 PARAM_LIST 只有一串扁平孩子; `CST_STMT` 已在 C2.1 并入 `UNPARSED`.
  计划里的 `CST_TYPE_NAME` 在 C2.2 接管了 C2.1 的 cast / sizeof 类型组, 属于计划
  内的结构升级, C2.1 的 dump 期望随之更新.

## C3.1 指令节点与条件分组(已提交)

- 改了什么:
  - `chibicc.h`: `CstKind` 增 10 个指令种类(`CST_PP_DIRECTIVE` / `CST_PP_DEFINE` /
    `CST_PP_UNDEF` / `CST_PP_INCLUDE` / `CST_PP_LINE` / `CST_PP_PRAGMA` /
    `CST_PP_ERROR` / `CST_PP_NULL` / `CST_PP_IF` / `CST_PP_BRANCH`).
  - `cst.c`(2254 行): 指令行 = `at_bol && "#"` 到下一个 `at_bol` token 之前, 行内 token
    全是叶子. `#define` 的对象 / 函数宏分界与 `preprocess.c` 一致(名字后 `(` 且无空白):
    函数宏参数表是一个扁平 `GROUP_PAREN`(未闭合则整组 `UNPARSED`, 停在行尾), 替换列表
    平铺; 变参 `...` 留在参数表里. `#undef` / `#include`(引号形与尖括号形, 路径 token
    原样) / `#line`(含 GNU 行标记 `# 123 "file"`) / `#pragma` / `#error` / 空指令 /
    未知指令各成节点. `#if` / `#ifdef` / `#ifndef` 成 `CST_PP_IF`, 每个 `#if` /
    `#elif` / `#else` 是一个 `CST_PP_BRANCH`, 收尾的 `#endif` 是 `PP_IF` 的最后一个
    孩子; 分支体按所在上下文(顶层 / 块内)用同一套 item 文法建树, 嵌套条件递归. 孤立的
    条件指令是普通指令节点; 未闭合的条件保留已消费部分.
  - 语句层把 item 解析收进 `cst_parse_item`(指令分派 + 语句 / 声明 + 原始回退);
    `cst_parse_fallback_statement` 把行首 `#` 当作跨度边界, 畸形语句不会吞掉后面的指令.
  - `test/cst.sh`: 第五段 13 个 dump(include / define 对象与函数宏 / 变参 /
    undef+line / GNU 行标记 / pragma+error+空指令+未知 / 三分支 / 嵌套 / 块内 / 孤立 /
    未闭合参数表 / 未闭合条件 / 续行 / 注释)与 3 个边界样本(BOM / CRLF / 缺分号后的
    指令), 全脚本 305 项断言, 61 个 dump 样本.
- 为什么: 预处理指令是阶段 2 的显式节点; 指令与条件分支是 formatter 与源到源工具必须
  保留的源码结构, 而条件求值与宏展开属于预处理器, 不在 CST.
- 口径与限制: 指令只在语句层(顶层与块内)的 item 位置识别; 原始组(结构体成员体, 初始化
  列表, 函数参数等)内部的指令仍按 token 叶子保留(全语料无此形态, 无损不受影响).
  条件分组按 `#elif` / `#else` / `#endif` 的语法层级配对, 不求值; 块内未闭合的条件停在
  包围的 `}` 前, 其后的 `#else` / `#endif` 成为孤立指令节点.
- 开发中校验: ASan 构建跑遍 48 个语料的 reconstruct / verify / dump 与全部 cst.sh,
  无 AddressSanitizer 报告(`intercept_memcmp=0`, 原因同 C2.1).
- 闸门: `make docker-test` EXIT=0(日志 `.cache/C3.1-docker-test.log`; cst lock 305 项在
  ./chibicc 与 ./stage2/chibicc 各一遍); `make docker-snapshot-diff`:
  `snapshot diff: empty`(日志 `.cache/C3.1-snapdiff.log`); `make docker-test-thirdparty
  THIRDPARTY=tinycc` EXIT=0(日志 `.cache/C3.1-tinycc.log`).
- 偏差: 函数宏参数表复用 `CST_GROUP_PAREN` 而未新增参数表种类 - 让对象 / 函数宏的分界
  在 dump 里可见, 同时不扩散种类; 参数表内层括号不再嵌套分组(合法宏参数不嵌套),
  未闭合时整组变 `UNPARSED`.

## C3.2 收口(已提交)

- 改了什么: `AGENTS.md` 更新(目标架构第 2 条标为已实现; 路线图第 2 条与现行线段改为
  完成; `make test` 与前端的可跑命令加入 `test/cst.sh` 与三个 CST 旗标; 代码地图新增
  `cst.c` 条目并更新 `chibicc.h` / `main.c` 说明; 测试约定加入 CST 的断言数与 dump
  覆盖); `PLAN.md` 勾选完成; `RESULT.md` 的终态验收逐项回填. 本步无代码改动.
- 覆盖审计: 55 个 `CstKind` 全部出现在 `test/cst.sh` 的期望 dump 中(61 个 dump 样本).
- 闸门: 沿用 C3.1 终态代码的闸门结果(`make docker-test` EXIT=0 含自举,
  `make docker-snapshot-diff` 为 `snapshot diff: empty`, tinycc EXIT=0); C3.2 只改
  文档, 不改变二进制.
- 偏差: 无.

## 提交一览

| 提交 | 步骤 | 内容 |
|---|---|---|
| fe3c56b | 线间归档 | trivia 线归档为 `PLAN-trivia.md` / `RESULT-trivia.md`; 本线与设计账本建立, `PLAN.md` 计划书就位; AGENTS.md 现行线段与路线图同步 |
| 5a9057a | C0.1 | 三道闸门复验并留档(EXIT=0 / diff 空 / tinycc EXIT=0); A/B worktree `/tmp/cbase` 建立在 443b4c2; 语料清单核对 41 + 7 + 12; 基线回填 |
| c5fa672 | C1.1 | `cst.c` 叶子树 + `-dump-cst` / `-reconstruct-cst` / `-verify-cst` 三通道; `cc1` 返回码; `test/cst.sh` 126 项断言并挂进两个测试目标; docker-test EXIT=0, snapshot diff 空 |
| 40e9c05 | C1.2 | 定界符组 + 语句切分 + BLOCK / NULL_STMT / UNPARSED; cst.sh 第二段 9 个骨架 dump, 全脚本 158 项断言; docker-test EXIT=0, snapshot diff 空 |
| 87858ef | C2.1 | 表达式递归下降(10 级二元链 / 一元 / 后缀 / cast / 条件 / 赋值 / 逗号 / 初始化列表 / 复合字面量)+ 文件内 typedef 预扫描; cst.sh 第三段 15 个 dump, 全脚本 203 项断言; docker-test EXIT=0, snapshot diff 空 |
| 67864c3 | C2.2 | 声明层(decl-specs / declarator / 参数 / type-name / 函数定义 / 指派初始化器)+ 语句层(if / switch / while / do / for / case / default / label / goto / break / continue / return / asm); cst.sh 第四段 19 个 dump, 全脚本 260 项断言; docker-test EXIT=0, snapshot diff 空 |
| 05d4099 | C3.1 | 指令行节点(#define 对象 / 函数宏 / 变参, #undef, #include, #line 与 GNU 行标记, #pragma, #error, 空指令与未知指令)+ 条件分组(PP_IF / PP_BRANCH, 分支全保留, 嵌套, 块内, 孤立与未闭合); cst.sh 第五段 13 个 dump 与 3 个边界样本, 全脚本 305 项断言; docker-test EXIT=0(含自举), snapshot diff 空, tinycc EXIT=0 |
| (本提交) | C3.2 | 收口: AGENTS.md / PLAN.md / RESULT.md 更新; 55 个 CstKind 的 dump 覆盖审计; 终态验收回填; 无代码改动 |

## 终态验收(本线完成时逐项实测)

- **通道**: `tokenize_file` 的直接输出可建 CST; 48 个语料文件的叶子序列与 token 流逐指针
  相等(每个 token 恰好一次, `-verify-cst` 全绿); 重建在 48 个语料文件与 15 个边界样本
  (C1.1 的 12 个 + C3.1 的 BOM / CRLF / 缺分号后接指令 3 个)上逐字节成立, 例外清零.
- **结构**: 55 个 `CstKind` 全部出现在 `test/cst.sh` 的期望 dump 里(61 个 dump 样本,
  含声明 / 语句 / 表达式 / 指令 / 条件分支 / 宏参数表 / `CST_UNPARSED`), 由脚本逐种类
  核对; 48 个语料的 dump 与 verify 全部正常结束; 回退跨度与歧义清单一致 - 唯一的已知
  结构限制是原始组(结构体成员体, 初始化列表, 函数参数)内部的指令仍按 token 叶子保留.
- **行为**: `make docker-test`(含自举)EXIT=0; snapshot diff 空; 诊断锁 55 例逐字节,
  形状锁 33 段, trivia 锁 7 个 -E 样本 / 23 个 dump / 48 个 round-trip / 12 个边界 /
  4 个锚点, 全部不变; `make docker-test-thirdparty THIRDPARTY=tinycc` EXIT=0.
- **对比锚点**(相对本线起点 `fe3c56b`): `cst.c` 2254 行(新文件); `test/cst.sh` 398 行 /
  305 项断言(61 个 dump 样本, 15 个边界样本); `main.c` +41/-12, `chibicc.h` +82,
  `Makefile` +2; `tokenize.c` / `preprocess.c` / `parse.c` / `sema.c` / `type.c` /
  `codegen.c` 零改动(红线守住).
