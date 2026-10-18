> 归档说明: 本文件是 CST 线(路线图阶段 2, C0.1-C3.2)的执行计划, 随该线收官归档
> (终态见本线 C3.2 提交); 现行计划见 `PLAN.md`(宏展开 provenance 线). 文中"RESULT.md"指
> 本线的执行记录, 已归档为 `RESULT-cst.md`.

# PLAN.md - CST 执行计划(路线图阶段 2)

目标(用户 2026-10-17 拍板): **在预处理之前的原始文本上建一棵 lossless 具体语法树(CST);
预处理指令是树中的节点, 每个语法 token 与标点都是显式节点, trivia 挂在 token 叶子上; 树的
叶子按序重建的结果逐字节等于输入文件.**

四条线已归档: 语法语义拆分线见 `PLAN-split.md` / `RESULT-split.md`, 忠实层收尾线见
`PLAN-faithful.md` / `RESULT-faithful.md`, codegen 直连线见 `PLAN-codegen-direct.md` /
`RESULT-codegen-direct.md`, trivia 保留线见 `PLAN-trivia.md` / `RESULT-trivia.md`. 本线从
trivia 终态(443b4c2: tokenize.c 924 行 + chibicc.h 864 行 + main.c 959 行 + preprocess.c
1208 行 + parse.c 2148 行 + sema.c 2992 行 + codegen.c 2677 行)起步, 是 AGENTS.md 目标架构
第 2 层, 服务 formatter 与精确重构; 库化(阶段 5)与参考消费者(阶段 6)在其后.

本线结束时: **`cst.c` 能从 `tokenize_file()` 的直接输出建出 CST; 树的叶子按序与 token 流
一一对应(同一批 token, 不增不减不乱序), 叶子按序拼接 trivia 与拼写逐字节等于输入文件;
`-dump-cst` / `-reconstruct-cst` / `-verify-cst` 三个只读通道可跑; 一切既有消费者(预处理,
解析, sema, codegen, 全部测试)的可观察行为逐字节不变.**

新会话恢复方法: 通读 AGENTS.md -> 查看本文件勾选状态 -> `git log --oneline` 确认最后完成的
步骤 -> 从第一个未勾选项继续; 开工前先跑 C0.1 的闸门复验. 行号一律以函数名为准.

## 目标边界与判据

**判据 1 - 新增消费者, 不接编译路径**
- CST 只在 `-dump-cst` / `-reconstruct-cst` / `-verify-cst` 三个只读模式下构建; 编译路径
  (`preprocess` -> `parse` -> `sema` -> `codegen`)不读 CST, 也不感知它的存在.
- `tokenize.c` / `preprocess.c` / `parse.c` / `sema.c` / `type.c` / `codegen.c` 零改动;
  `chibicc.h` 只增 CST 类型与函数声明; `main.c` 只增旗标, 早退分支与三个通道的调用;
  Makefile 只增 `test/cst.sh` 与两个测试目标的调用.
- `tokenize_file` 的输出与 `Token` / `File` 既有字段一个不改. CST 需要的全部文本事实
  (trivia, raw 坐标, `at_bol`, `has_space`, 行号)在 trivia 线已经齐备.

**判据 2 - 无损是结构合同, 不是解析成功**
- 叶子 = token 流的每个 token 恰好一次(含 `TK_EOF`); 叶子按序遍历得到的 token 序列与
  `tokenize_file` 的输出指针一一相等; 重建 = 按叶子序输出 `trivia` 再输出拼写, 对
  `tokenize_file` 的直接输出恒等于 raw 文本.
- 解析失败或歧义不得影响无损: 失败的跨度以 `CST_UNPARSED` 节点覆盖, 其子节点就是已经建好的
  叶子, 不复制 token; 不允许为了解析成功而丢弃, 复制或重排任何 token.
- 所有验收以 `-reconstruct-cst` 的逐字节比较与 `-verify-cst` 的叶子序列检查为准; dump 只
  用于锁定树形, 不作为无损性的唯一证据.

**判据 3 - 适用面: 单文件, 预处理前**
- 只覆盖 `tokenize_file(path)` 的直接输出. `#include` 是节点, 不展开; 宏调用是普通标识符与
  调用语法, 不展开; 条件编译的每个分支都保留在树里.
- 多文件 CST(include 展开后的整棵翻译单元), 宏展开保真, `paste` / `stringize` 的合成 token
  都不在本线; 语法层 AST(第 3 层)的宏调用保真策略是 AGENTS 的开放决策, 与本线正交.

**判据 4 - 树形与表示取最小**
- 每个语法 token 与标点是显式叶子(`CST_TOKEN`, 持 `Token *`); 括号, 分号, 逗号, 运算符都
  是叶子, 不吞并, 不折叠.
- trivia 只挂在 token 上(已有字段), 不复制进节点; 重建与未来 formatter 都从叶子读.
- 节点按语法范畴组织(声明 / 语句 / 表达式 / 指令 / 条件分支 / 回退), 种类草案冻结在
  `RESULT.md` 的设计账本; 不复用第 3 层的 `Node` / 第 4 层的 `Type`, 不携带类型与绑定.
- 子节点用可增长数组(frame + `calloc` / `realloc`), 不用 `next` 链表; 解析状态放在栈上的
  context 结构, 不新增文件域 static; 不逐对象 `free`.
- 树形示例(叶子的文本来自 `Token`, 节点只分组):

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
  ```

**判据 5 - 全函数, 不新增 exit**
- `cst.c` 不调用 `error` / `error_at` / `error_tok`; 解析失败是数据(`CST_UNPARSED`), 不是
  诊断. 孤立 `#endif`, 未闭合括号, 未知指令, 无法消歧的声明都必须仍能建树并重建.
- 不变量失败由 `-verify-cst` 报告, 驱动返回非零状态(`main` 返回, 不调用 `exit`); 库函数
  只返回结果, 不打印, 不退出.
- 这是为库化(阶段 5)预留的形态: 错误回调化之后, CST 的失败语义不需要重新设计.

**判据 6 - 两阶段粒度, 先无损后结构**
- C1 只做无损骨架: 叶子序列, 定界符配对, 语句 / 块切分, 指令行切分; 不做声明与表达式文法.
  此阶段任何输入(包括畸形输入)都能建树, 无损合同先成立.
- C2 在骨架上加表达式与声明 / 语句结构. 文法歧义(typedef 名 vs 表达式)用"文件内 typedef
  集合 + 类型关键字拼写 + 回溯 + 回退"处理; 回退永远不破坏判据 2.
- 结构覆盖是 best-effort, 无损是绝对的: 前者按 C2 / C3 的样本验收, 后者按全语料验收.

## 现状锚点(开工前实测)

- **token 叶子的全部文本事实**(实测, trivia 线终态):
  - `Token` 字段在 `chibicc.h:87-122`: `trivia` / `trivia_len` 是前置被跳过字节,
    `raw_loc` / `raw_len` 是拼写, 四者均为 raw 坐标; `at_bol` / `has_space` / `line_no`
    保持旧语义.
  - EOF 收文件尾部 trivia(`tokenize.c:703-705`), 因此叶子序列含 EOF 即可覆盖文件最后一个
    字节之后的全部字节; 重建不需要额外的根级尾部字段.
  - `equal()` 只按拼写比较(`tokenize.c:125-127`), 与 kind 无关. 预处理前的流里关键字仍是
    `TK_IDENT`, 数字仍是 `TK_PP_NUM`; `convert_pp_tokens`(`tokenize.c:502-509`)只在
    `preprocess`(`preprocess.c:1198-1207`)里跑. **CST 必须按拼写比较, 且不得调用
    `convert_pp_tokens` / `preprocess`.**
- **指令语法的事实**(CST 只做语法分组, 不求值, 不展开):
  - 指令起点 = `at_bol && equal(tok, "#")`(`preprocess.c:76-78` 的 `is_hash`). 指令行 =
    从 `#` 到下一个 `at_bol` token 之前; 续行在扫描缓冲里已被删除, 续行后的 token 不置
    `at_bol`, 因此多行指令天然是一行.
  - 函数宏的判据 = 名字后紧跟 `(` 且无空白(`!tok->has_space`, `preprocess.c:368-386`).
    对象宏与函数宏的分界必须与这条规则逐字一致.
  - `#include` 的三种写法(`"..."` 是 `TK_STR`; `<...>` 是 token 序列; 标识符走宏展开,
    CST 不展开)见 `preprocess.c:716-754`. CST 保留路径的 token 原样, 不解释文件名.
  - 条件指令 `#if` / `#ifdef` / `#ifndef` / `#elif` / `#else` / `#endif` 的配对与嵌套见
    `preprocess.c:899-954`; CST 只按语法配对, 不计算条件, 不丢弃任何分支.
  - `#pragma`(含 `once`), `#line`(含 GNU 行标记 `# 123 "file"`), `#error`, `#undef`,
    空指令 `#` 都是独立节点或指令行节点; 行尾多余 token 全部保留.
- **文法参考**(只借形状, 不借代码与语义):
  - `parse.c` 的声明 / 语句 / 表达式的产生式骨架在 `parse.c:46-79`(`declspec` /
    `declarator` / `typename` / `declaration` / `stmt` / `compound_stmt` / 优先级链 /
    `unary` / `postfix` / `primary` / `function_def` / `global_variable`).
  - `parse.c` 的 typedef / tag 分类 oracle(`find_typedef` / `find_tag` / `is_typename`,
    `parse.c:36-45`, `143-205`, `884-902`)依赖预处理后的名字表与类型, **CST 不复用**;
    CST 的歧义策略见判据 6 与 `RESULT.md` 的歧义清单.
- **驱动器接入点**(实测):
  - 旗标解析在 `main.c:175-183`(`-dump-tokens` / `-reconstruct`); 新旗标同区插入.
  - `cc1()` 的只读早退口在 `main.c:671-682`, 对 base_file 调 `must_tokenize_file` 后直接
    返回, 不跑预处理器; 三个 CST 旗标进同一分支.
  - 驱动侧的透传分支在 `main.c:928-929`(`-E || -M || -dump-tokens || -reconstruct`),
    新旗标必须加入, 保证不写目标文件, 不汇编.
  - 现有重建在 `main.c:595-602`(按流序写 trivia + 拼写), dump 的转义与分片在
    `main.c:495-587`; CST 的 dump 转义规则与 trivia.sh 的约定一致(空格也转义为 `\x20`).
  - `usage()`(`main.c:39-42`)只打印一行用法, 不列旗标, 无需改文案.
- **测试设施**:
  - `test/trivia.sh` 275 行, 94 项断言(-E 7 / dump 23 / round-trip 48 文件 + 12 边界
    样本 / 行号锚点 4), 由 `make test` 与 `make test-stage2` 调用(`Makefile:20-25`,
    `43-48`). `test/cst.sh` 照此形状新建: 全量 round-trip + 结构 dump 断言 + 不变量检查.
  - 语料 = `test/*.c` 41 个 + `include/*.h` 7 个 = 48 个; 边界样本 12 个(CRLF, 孤立 CR,
    BOM, 无末行换行, 空文件, 续行落在标识符 / pp-number / 字符串 / 字符 / 行注释 / 块注释 /
    指令行中, 续行 + \u, 深层混合).
  - 既有闸门: `make docker-test`(含自举) + `make docker-snapshot-diff`(raw `.s` 逐字节)
    + tinycc; 诊断 55 例, 形状 33 段.
- **语料与歧义(开工前判断)**: `test/*.c` 大量使用宏与 `include/` 的类型; 预处理前这些名字
  不可解析. 因此 CST 的结构覆盖必然有 `CST_UNPARSED` 跨度; 这不是失败, 是判据 2 与判据 6
  的设计结果. 全量无损必须 0 例外.

## 闸门口径

- 本线每步都是"行为不变"步: `make docker-test`(含自举)全绿 + `make docker-snapshot-diff`
  为空(本线不接编译路径, 预期语料不变, 不重置基线) + `test/cst.sh` 该步新增的断言(挂进
  `make test` 与 `make test-stage2`).
- 一步一提交; 每步汇报固定四项(改了什么 / 为什么 / 闸门结果 / 偏差). 计划内不做任何可观察
  行为变化; 若出现, 视为回归, 先查根因, 不重置快照.
- 红线: 不改 `tokenize.c` / `preprocess.c` / `parse.c` / `sema.c` / `type.c` / `codegen.c`;
  不改 `Token` / `File` 字段; 不新增 exit 点; 不新增文件域 static; 不引入新编译告警;
  `cst.c` 保持纯 C11, 能被 chibicc 自举编译.
- 本线不做(留给后续线): formatter 实现(阶段 6), 库化与公共头拆分(阶段 5), include 展开,
  宏展开保真, CST 到第 3 层 AST 的下降(阶段 3 已有独立管线).

## 阶段 C1: 无损骨架(行为不变)

- [x] **C0.1 归档 / 账本 / 基线**(闸门复验与基线回填完成, 见 `RESULT.md` 基线记录): 四条旧线已归档(本次归档把 trivia 线归入
  `PLAN-trivia.md` / `RESULT-trivia.md`, 并把 `PLAN.md` / `RESULT.md` 切到本线); `RESULT.md`
  建立 CST 设计账本(输入事实表 / 歧义清单 / 节点种类草案 / 指令分组规则 / 五条不变量)与
  基线占位. 本步剩余: 复验三道闸门(`make docker-test`, `make docker-snapshot-diff`,
  `make docker-test-thirdparty THIRDPARTY=tinycc`)并回填基线; 建立本地 A/B 规程
  (`git worktree add /tmp/cbase 443b4c2`, 对照 `-E` / `-S` / dump 输出); 确认 `test/cst.sh`
  的语料清单与 trivia.sh 一致.
- [x] **C1.1 叶子树与重建 / 校验通道**: `chibicc.h` 增 `CstKind` / `CstNode` 与三个函数
  声明; 新建 `cst.c`: `cst_parse` 对 `tokenize_file` 的直接输出建
  `CST_TRANSLATION_UNIT -> CST_TOKEN* + CST_EOF` 的叶子树(此时还没有任何语法分组),
  `cst_reconstruct` 按叶子序写 trivia + 拼写, `cst_dump` 按缩进打印种类与叶子拼写,
  `cst_verify` 检查"叶子按序 == token 流(指针相等, 每个 token 一次)". `main.c` 增
  `-dump-cst` / `-reconstruct-cst` / `-verify-cst`(同 `-dump-tokens` 的早退口与驱动透传,
  支持 `-o`). `test/cst.sh` 立项并挂进 Makefile 的两个测试目标; 第一段: 全量 48 文件
  round-trip 逐字节相等; 12 个边界样本;
  `-verify-cst` 全绿; 叶子树 dump 样本(空文件, 单 token, 多 token, EOF 收尾 trivia).
  验收: 两个二进制(stage1 / stage2)都过; snapshot diff 为空.
- [x] **C1.2 定界符与语句骨架**: 在叶子树上加 `CST_GROUP`(`()` / `[]` / `{}` 各自节点,
  开闭括号是显式叶子), 语句切分(顶层与块内按 `;` 切 `CST_STMT`), `CST_BLOCK`(花括号体),
  空语句, 以及 `CST_UNPARSED` 回退节点(未闭合定界符, 无法切分的跨度). 本步不做声明与
  表达式文法, 解析仍是全函数. `test/cst.sh` 第二段: 嵌套括号 / 花括号 / `for (;;)` 的
  骨架 dump, 空语句, 未闭合输入的 UNPARSED 与无损重建. 验收: round-trip 与
  `-verify-cst` 保持不变.

## 阶段 C2: 语法结构(无类型, 无名字绑定)

- [x] **C2.1 表达式结构**: 按 C 的优先级链建 `CST_BINARY` / `CST_UNARY` / `CST_POSTFIX`
  (调用, 下标, 成员, 自增自减) / `CST_CAST` / `CST_COND` / `CST_ASSIGN` / `CST_COMMA` /
  `CST_PAREN`(括号是显式叶子) / `CST_PRIMARY`(标识符, 数字, 字符串, 字符) /
  `CST_ARG_LIST` / 初始化列表. 类型转换与括号的消歧: 括号内以类型关键字 / 文件内 typedef /
  `struct` / `union` / `enum` / `typeof` / `_Atomic` 开头时按 cast, 否则按括号表达式;
  解析失败回退 `CST_UNPARSED`. 不查名字, 不算类型, 不折叠常量. `test/cst.sh` 第三段:
  优先级与结合性, 括号显式保留, 调用 / 下标 / 成员, 条件表达式与赋值链的 dump.
- [x] **C2.2 声明与语句结构**: `CST_DECL` / `CST_DECL_SPECS` / `CST_DECLARATOR` /
  `CST_INIT_DECLARATOR` / `CST_FUNC_DEF` / `CST_PARAM_LIST` / `CST_TYPE_NAME` /
  初始化器(含指派与嵌套花括号); 语句 `CST_IF` / `CST_SWITCH` / `CST_WHILE` / `CST_DO` /
  `CST_FOR` / `CST_CASE` / `CST_DEFAULT` / `CST_LABEL` / `CST_GOTO` / `CST_BREAK` /
  `CST_CONTINUE` / `CST_RETURN` / `CST_ASM` / `CST_EXPR_STMT`. 声明 vs 表达式语句:
  首 token 是类型关键字或文件内已见 typedef 名时才尝试声明, 否则按表达式; 任一方向失败
  即回退 `CST_UNPARSED`. 未知类型(`#include` 来的 `size_t` 等)不猜, 以 `CST_UNPARSED`
  覆盖. `test/cst.sh` 第四段:
  各类声明 / 函数定义 / 语句 / 初始化器的 dump, 歧义样本(`T *x;` 在 typedef 已知与未知
  两种情形, `(T)x`, `sizeof(T)`, `T(x);`), 全语料 smoke(建树不崩, 无损不变量保持).
  验收: round-trip 与 `-verify-cst` 全量不变.

## 阶段 C3: 预处理指令与收口

- [x] **C3.1 指令节点与条件分组**: 指令行按 `at_bol` 切分, 建 `CST_PP_DIRECTIVE` 家族:
  `#define`(对象宏 / 函数宏 / 变参宏, 参数表与替换列表是 token 叶子, 不解析成表达式),
  `#undef`, `#include`(引号形与尖括号形, 路径 token 原样), `#line`(含 GNU 行标记),
  `#pragma`(含 `once`), `#error`, 空指令. `#if` / `#ifdef` / `#ifndef` 与其
  `#elif` / `#else` / `#endif` 归入一个 `CST_PP_IF`, 每个分支是独立的
  `CST_PP_BRANCH`, **所有分支的 token 与子树都保留**, 条件不求值, 宏不展开; 嵌套按
  栈递归. 孤立的 `#elif` / `#else` / `#endif` 仍是可重建的指令节点或 `CST_UNPARSED`,
  不报错. `test/cst.sh` 第五段: 五类指令的 dump, 条件分支全保留, 嵌套条件, 指令行内
  续行 / 注释 / BOM / CRLF, 函数宏与对象宏的分界(名字后有无空白).
- [x] **C3.2 收口**: 全语料不变量(48 文件的叶子序列 == token 流; 重建 == 原文件)与
  `-verify-cst` 固化; dump 的黄金样本覆盖每一种节点(声明 / 语句 / 表达式 / 指令 /
  分支 / UNPARSED); `test/cst.sh` 的断言数统计入终态; AGENTS.md 更新
  (目标架构第 2 条标为已实现, 路线图第 2 条, 测试段加入 cst.sh, 代码地图新增 `cst.c`
  条目与 main.c 旗标说明); `PLAN.md` 勾选完成, `RESULT.md` 的终态验收逐项实测回填;
  终态闸门 = `make docker-test`(含自举) + snapshot diff 空 + tinycc EXIT=0.

## 开放决策(实施时由用户拍板)

- **结构深度**: 默认按判据 6 做 C1 骨架 + C2 声明 / 表达式结构. 若用户只需要 formatter 的
  语句级重排, 可停在 C1, 省掉歧义策略; 但 C2 的节点是阶段 6 的 C→C 与 formatter 主要消费面.
- **节点种类的命名与粒度**: 草案在 `RESULT.md`; 默认按语法范畴命名, 不为某一种格式化风格
  增设节点. 若 formatter 需要换行 / 对齐的额外信息, 应由消费者派生, 不进 CST.
- **CST 类型的头文件位置**: 默认进 `chibicc.h`(与现有内部头习惯一致), 阶段 5 拆公共头时
  一并迁移; 不在本线新建对外头.
- **驱动旗标命名**(`-dump-cst` / `-reconstruct-cst` / `-verify-cst`): 属未来库 API 面的
  前身, 库名与公共头定名时一并调整.
- **include 与宏**: 本线均为"不展开". 若后续需要"整棵翻译单元 CST"(include 展开但保留
  trivia), 那是新的一层, 不在阶段 2 的验收内.
- **`CST_UNPARSED` 的粒度**: 默认语句级回退, 表达式级回退在 C2.1 只用于单个操作数; 更细的
  错误恢复留给 formatter 阶段按需增加.

## 风险与对策

1. **预处理前的文法歧义(最高风险)**: typedef 名与普通标识符在预处理前不可区分, 宏与
   include 还会引入未知类型. 对策 = 判据 2 的无损与解析解耦 + `CST_UNPARSED` 回退 +
   歧义样本进 `test/cst.sh`; 结构覆盖按 best-effort 验收, 不追求零回退.
2. **token 重复 / 遗漏 / 乱序**: 每加一种产生式都可能把已消费的 token 再挂一次或漏挂.
   对策 = 每个产生式只从当前位置摘 token, 失败回溯时不重复建叶子(已建节点留在 arena 但
   不挂进树), `-verify-cst` 的指针序列检查在每一步跑.
3. **指令边界错误**: `at_bol` 的语义(块注释内换行不置位, 续行不置位), 函数宏的空白规则,
   `#include <...>` 的 token 序列, 空指令, 孤立 `#endif` 都是已知陷阱. 对策 = 逐类样本;
   CST 只依赖 `at_bol` / `has_space` / 行号, 不复制 `preprocess.c` 的代码(其 `skip_line`
   有既有怪癖, 不作参照).
4. **kind 混淆**: 预处理前的关键字是 `TK_IDENT`, 数字是 `TK_PP_NUM`; 误调
   `convert_pp_tokens` / `preprocess` 会让 CST 看到与 tokenize 不同的流. 对策 = 全用
   `equal()` 按拼写比较, 代码层面禁止调用预处理器; 测试用 `int` / `if` 等关键字断言.
5. **驱动回归**: 新旗标若没进早退口或透传分支, 会意外走编译路径或写目标文件. 对策 =
   照 `-dump-tokens` 的三处接入(旗标 / `cc1` 早退 / 驱动透传)逐处核对; `driver.sh` 与
   snapshot diff 复验.
6. **自举**: `cst.c` 会被 chibicc 编译两遍; 只用 C11 与项目已有设施, 不引入新头文件依赖;
   `make test-stage2` 里 cst.sh 必须过.
7. **内存与规模**: 失败回溯会留下不可达节点; arena 不回收, 单文件 dump 模式下可接受.
   不为此引入 free 或对象池.
8. **dump 稳定性**: 输出必须逐字节可锁; 缩进固定两空格, 叶子文本按 trivia.sh 的转义规则,
   空文本(EOF)省略文本字段. 任何格式调整都要同提交更新样本.

## 终态验收(本线完成时逐项实测填写)

- **通道**: `tokenize_file` 的直接输出可建 CST; 叶子按序 == token 流(指针相等, 每个 token
  一次); 叶子重建在 48 个语料文件与 12 个边界样本上逐字节成立, **例外清零**;
  `-verify-cst` 全绿.
- **结构**: 声明 / 语句 / 表达式 / 指令 / 条件分支 / `CST_UNPARSED` 的 dump 样本齐全;
  全语料建树不崩; 回退跨度与 `RESULT.md` 的歧义清单一致.
- **行为**: `make docker-test`(含自举)EXIT=0; snapshot diff 为空; 诊断 55 例 / 形状 33 段 /
  trivia 94 项不变; tinycc EXIT=0.
- **对比锚点**: 新增 `cst.c` 行数与 `test/cst.sh` 断言数; `main.c` 增量; `chibicc.h` 增量;
  `tokenize.c` / `preprocess.c` / `parse.c` / `sema.c` / `type.c` / `codegen.c` 全部零改动
  (红线守住).
