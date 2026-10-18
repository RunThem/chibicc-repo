# PLAN.md - 宏展开 provenance 执行计划(路线图阶段 5 的前置线)

目标(用户 2026-10-18 拍板): **让预处理后 token 流里的每个 token 都能回答"它从哪里来" -
由哪次宏展开产生, 调用点与调用范围在哪, 用的是哪一份定义, 来自宏体还是第几个实参, 以及完整的
外层嵌套链; 并且不改变编译路径的任何可观察行为.**

同日对 AGENTS.md 开放决策"语法层 AST 对宏调用的保真策略"的裁决: 第 3/4 层消费展开后的
token 流, **不引入宏调用节点**; 宏保真由第 2 层 CST(未展开文本, 全部条件分支)与 token
provenance 共同承担; **明确放弃在宏展开内部做外科式编辑**. C→C 消费者对展开内节点的默认
策略是映射到实参区间或整个调用范围后在调用点改写; 无法映射时以诊断拒绝. 修改宏体本身是
另一回事 - 去 `#define` 定义处改, 接受全局影响. 与 Clang / libclang 的对照与理由见
`RESULT.md` 的设计账本.

五条已归档线: 语法语义拆分线见 `PLAN-split.md` / `RESULT-split.md`, 忠实层收尾线见
`PLAN-faithful.md` / `RESULT-faithful.md`, codegen 直连线见 `PLAN-codegen-direct.md` /
`RESULT-codegen-direct.md`, trivia 保留线见 `PLAN-trivia.md` / `RESULT-trivia.md`, CST 线见
`PLAN-cst.md` / `RESULT-cst.md`. 本线从 CST 线终态起步, 是库化(阶段 5)的前置; 库化本身的
错误回调化 / context 收敛 / 公共头拆分仍待用户拍板.

本线结束时: **`MacroExpansion` 记录在每次展开时建立, 每个由展开产生的 token 携带一条
provenance 链; 调用点宏名 token, function-like 的右括号, 展开时刻的 `Macro`, 实参序号与
实参 token 列表均可通过 `chibicc.h` 的访问器查询; `-dump-expansions` / `-verify-expansions`
两个只读通道可跑; `-E`, 诊断锚点, 汇编快照, trivia / CST 通道逐字节不变; 新代码能被
chibicc 自举.**

新会话恢复方法: 通读 AGENTS.md -> 查看本文件勾选状态 -> `git log --oneline` 确认最后完成
的步骤 -> 从第一个未勾选项继续; 开工前先跑 M0.1 的闸门复验. 行号一律以函数名为准.

## 目标边界与判据

**判据 1 - provenance 覆盖**
- 对象宏, 函数宏(宏体与每个实参), 嵌套宏, 内建宏(`__LINE__` / `__FILE__` / `__COUNTER__` /
  `__TIMESTAMP__` / `__BASE_FILE__`), `#` 与 `##` 的产物都携带 provenance.
- 空宏展开不产生 token, 因而不出现在记录中; 这是已知限制, 不虚构条目.

**判据 2 - 可查询的事实**
- 调用点: 宏名 token (拼写, 文件, 物理行).
- 调用范围: function-like 的 `)` token; object-like / 内建以宏名 token 为范围.
- 定义: 展开**时刻**的 `Macro` 指针(arena 存活; `#undef` / 重定义后旧展开仍指向旧定义),
  以及 `#define` 处的宏名 token.
- 实参: 第几个实参(0 起)与该实参的原始 token 列表.
- 嵌套: 一条从最内层到最外层的链, 逐层可走.

**判据 3 - 行为不变**
- `-E` 输出, `error_tok` / `warn_tok` 的锚点与文案, `test/diagnostic.sh` 55 例,
  `test/shape.sh` 33 段, `test/trivia.sh`, `test/cst.sh` 逐字节不变.
- `make docker-snapshot-diff` 为空: 不改 codegen, 不改 token 文本 / 顺序 / 类型.
- `Token.origin` 的现有语义与唯一消费者(`__FILE__` / `__LINE__`)不改.

**判据 4 - 库化红线**
- 不新增 `exit()` 点; `-verify-expansions` 失败返回 1, 不调用 `error`.
- 不新增文件域 static: 记录与链节点都从 token 可达, 不建旁表, 不用计数器.
- 不改 `parse.c` / `sema.c` / `type.c` / `codegen.c`; `preprocess.c` 只增记录与查询,
  不改展开的 token 文本与顺序.
- 内存一律 `calloc` 的 arena 式分配, 不 `free`; 记录与 token 同生命周期.
- 新代码保持 C11 可被 chibicc 自举.

**判据 5 - 只读通道**
- `-dump-expansions` 在 `preprocess()` 之后运行, 每个非 EOF token 一行; `-o` 生效.
- `-verify-expansions` 无输出, 成功 0 / 失败 1; 加入 driver 的 pass-through, 不写目标文件,
  不调用汇编器 / 链接器.
- 两者挂进 `test/expand.sh`, 并由 `make test` 与 `make test-stage2` 各跑一遍.

**判据 6 - 公共面收敛**
- `chibicc.h` 只暴露不透明的 `MacroExpansion` / `MacroProv` 与访问器; `Macro` /
  `MacroArg` / `MacroParam` 仍留在 `preprocess.c`, 不进入公共面.
- 访问器是未来库 API 的种子; 本线不拆分公共头, 不改库名.

## 现状锚点(开工前实测)

- `chibicc.h`: `Token` (87-122) 有 `loc` / `len`(扫描坐标), `trivia` / `raw_loc` / `raw_len`
  (raw 坐标), `file`, `filename`, `line_no`, `line_delta`, `at_bol`, `has_space`, `hideset`,
  `origin`. `File` (62-84) 的 `raw` 与 `contents` 在真实文件上分开分配, 在合成缓冲上同一指针
  (`new_file` 的注释, `tokenize.c:759-762`).
- `Token.origin`: 由 `expand_macro` 的 object-like / function-like 两条路径写入
  (`preprocess.c:650`, `:678`); 全仓库只有 `file_macro` / `line_macro` 读它
  (`preprocess.c:1008-1019`), 用来走到最外层宏名 token. 其余地方不读.
- `copy_token` (`preprocess.c:91-96`) 用 `*t = *tok` 整结构拷贝, 只重置 `next`;
  `new_eof` (98-103), `add_hideset` (138-148), `append` (151-162) 都会复制展开结果.
  **结论**: provenance 必须作为 `Token` 字段, 不能做按 token 地址索引的旁表.
- `Macro` (43-51): `name` / `is_objlike` / `params` / `va_args_name` / `body` / `handler`;
  没有定义处 token. `MacroArg` (33-39): `next` / `name` / `is_va_args` / `tok`; 没有序号.
  `MacroParam` (27-31). 全部 arena, 从不释放.
- 展开是内联拼接 (`preprocess2`, `expand_macro`): 调用 token 被消费后丢弃, 但名 token 仍存活,
  其 `next` 链仍连着 `(` / 实参 / `)` / 后续 token.
- `expand_macro` (`preprocess.c:630-683`): 先 hideset 检查, 再 handler 分支 (639-643),
  object-like (646-655), function-like (662-682). function-like 在读实参后手里有 `rparen`
  (`Token *rparen = tok;`, 665).
- `subst` (`preprocess.c:518-626`): 逐分支产出 token; 完整清单见"记录写入清单".
- 合成 token: `new_str_token` (221-224), `new_num_token` (241-244), `paste` (499-508) 都是
  `tokenize(new_file(tmpl->file->name, tmpl->file->file_no, buf))`; `file` 是合成 `File`,
  `line_no` 是合成缓冲的行号(通常 1), `origin` / 新字段初始为 NULL.
- `join_adjacent_string_literals` (1140-1195): 1160 行 `*t = *tokenize_string_literal(t, basety)`
  会丢掉整个 token 的字段; 1189 行 `*tok1 = *copy_token(tok1)` 保留字段.
- `preprocess` (1198-1208): 最后一遍 `t->line_no += t->line_delta`.
- `main.c`: `cc1()` (689-765); `preprocess(tok)` 在 734; `-M` / `-MD` 在 737-741;
  `-E` 在 743-747; 新通道的接入点是 735 (preprocess 之后, `-M` / `-E` 之前);
  driver 的 pass-through 在 955-960; 通道旗标在 178-201; dump 辅助
  `print_escaped` (481-497), `print_dump_line` (502-510), `token_kind_name` (512-523).
- `Makefile`: `test` 的 shell 段在 20-26; `test-stage2` 在 44-50; 新脚本要同时加进两处.
  `SRCS=$(wildcard *.c)` 自动纳入新源文件, stage2 规则 (`Makefile:35-37`) 自动自举.
- 测试 harness: `test/trivia.sh:15-26` 与 `test/cst.sh:18-29` 的 `check` 模式; 语料循环对比
  原文件; dump 期望用 `printf` 生成. 新脚本沿用同一形态.
- 平台: 本机 macOS 缺 Linux include 路径, `preprocess` 无法处理 `<stdio.h>`; 自包含片段可在
  本机跑, 全量语料校验只在 docker 里跑(用 include 探测决定是否跳过).

## 数据模型

### 记录

```c
// preprocess.c, 不透明; chibicc.h 只前向声明
struct MacroExpansion {
  Token *name;        // 调用点宏名 token
  Token *rparen;      // function-like 的 ')' ; object-like / 内建为 NULL
  Macro *macro;       // 展开时刻的定义 (arena 存活, #undef / 重定义后仍可查)
  MacroArg *args;     // 实参列表; object-like / 内建为 NULL
  int arg_count;
  bool is_builtin;
};
```

### 链节点

```c
// preprocess.c, 不透明
struct MacroProv {
  MacroExpansion *exp;
  int arg_index;      // -1 = 来自宏体; >= 0 = 第几个实参 (0 起)
  MacroProv *next;    // 更外层的上下文
};
```

`Token` 增一个字段 `MacroProv *prov;` (紧跟 `origin`). `Macro` 增 `Token *def;` -
`#define` 处的宏名 token; `-D` 与 handler 型内建为 NULL.

### 挂接规则

**全部用 prepend, 链按"最内层在前"存储.** 一次展开 `E` 的产物挂接:

- 宏体 token / `#` / `##` 产出的 token / 内建 handler 的产物:
  `prov = new_prov(E, -1, name_token->prov)`.
- 实参 token: `prov = new_prov(E, k, arg_token->prov)`.
- 未展开 token: `prov = NULL`.

为什么 prepend 就够: 宏调用点总是位于产生实参文本或名 token 的那个上下文**之内**, 所以新展开
一定是链上最内层的一层. 把 `E` 放在链头, 原链作为不可变的尾, 既不改动共享节点, 也不需要
父指针. 例:

```c
#define A F(1)
#define F(x) x
A
```

`1` 先由 A 的宏体产生(链 `[A body]`), 再作为 F 的实参被消费(链 `[F arg1, A body]`); 最内层
是 F 的实参层, 外层是 A. 编辑时先看 F 的调用范围, 再向外走到 A, 与词法包含关系一致.

**为什么不给 `MacroExpansion` 挂 parent:** 一个记录被该次展开的多个 token 共享; 用 parent
表达"消费链"在嵌套实参里会成环, 或必须 mutation 共享记录. per-token 链只 prepend, 记录永久
只读, 任意嵌套都正确. 代价是每个 token 每层一个 16 字节节点 (arena).

### 边界处理

- `new_eof` 显式清空 `prov` (终止符不携带 provenance).
- `paste` / `new_str_token` / `new_num_token` 新建 token 的 `prov` 初始为 NULL, 由调用点设置.
- `join_adjacent_string_literals` 的 1160 行: 保存 / 恢复 `prov`; 合并后的字符串保留第一个
  literal 的 provenance.
- `copy_token` 整结构拷贝, `prov` 自动传播; 这是选 token 字段而非旁表的根本原因.
- `Token.origin` 原样保留, 不参与新链; 现有 `__FILE__` / `__LINE__` 行为不变.

## 记录写入清单 (`subst` 全分支)

| 编号 | 位置 / 分支 | 产物 | 挂接 |
|---|---|---|---|
| E1 | `expand_macro` handler 分支 | 内建宏产物 | `new_prov(B, -1, tok->prov)` |
| E2 | `expand_macro` object-like | 宏体 token | `new_prov(E, -1, tok->prov)` |
| E4 | `expand_macro` function-like | 宏体 token | `new_prov(E, -1, macro_token->prov)` |
| S1 | `#` stringize | 合成 TK_STR | body |
| S2a | `,##__VA_ARGS__` 空变参 | 无 | 无 |
| S2b | `,##__VA_ARGS__` 非空 | 逗号 | body |
| S3c-i | `##` RHS 是实参 | paste 结果 body; 其余实参 token arg | 见左 |
| S3c-ii | `##` RHS 是空实参 | 无 | 无 |
| S3d | `##` RHS 是体 token | paste 结果 | body |
| S4a-i | 参数 + `##`, 左空, RHS 是参数 | RHS 实参 token | arg |
| S4a-ii | 参数 + `##`, 左空, RHS 是体 token | RHS 体 token | body |
| S4b | 参数 + `##`, 左非空 | 实参 token (最后一个随后成 paste LHS) | arg; paste 结果 body |
| S5a | `__VA_OPT__` 有变参 | 体 token | body |
| S5b | `__VA_OPT__` 无变参 | 无 | 无 |
| S6 | 普通参数 | 预扫描后复制 | arg |
| S7 | 普通体 token | 复制 | body |
| S8 | 循环结束 | 体 EOF 终止符 | 由 `add_hideset` / `append` 复制, 不挂 |

补充:
- `read_macro_arg_one` 只做复制, 不挂 provenance; 挂接发生在 `subst` 的消费点.
- `__VA_OPT__` 的 token 来自体, 归 body.
- paste 的结果是体层产物; 它的操作数 provenance 不丢, paste 前的实参 token 已经是 arg 层,
  但 paste 后只剩结果 token, 字符级重放要靠记录 + 实参列表 (已知限制).
- 任何新增分支必须同时更新本表与 `test/expand.sh`.

## 查询接口 (`chibicc.h`)

```c
typedef struct MacroExpansion MacroExpansion;
typedef struct MacroProv MacroProv;

MacroProv *token_provenance(Token *tok);
MacroProv *macro_prov_next(MacroProv *p);
MacroExpansion *macro_prov_expansion(MacroProv *p);
int macro_prov_arg_index(MacroProv *p);

Token *macro_expansion_name(MacroExpansion *e);
Token *macro_expansion_end(MacroExpansion *e);      // rparen, 无则 name
char *macro_expansion_macro(MacroExpansion *e);     // 宏名
Token *macro_expansion_def(MacroExpansion *e);      // #define 名 token, 无则 NULL
Token *macro_expansion_arg(MacroExpansion *e, int i);
int macro_expansion_arg_count(MacroExpansion *e);
bool macro_expansion_is_builtin(MacroExpansion *e);
```

- `Macro` / `MacroArg` / `MacroParam` 不出现在头文件里.
- `macro_expansion_arg` 返回该实参的原始 token 列表(含 EOF 终止符); 越界返回 NULL.
- `macro_expansion_def` 对内建 / `-D` 返回 NULL; 调用方自行处理.

## 只读通道

### `-dump-expansions`

在 `preprocess()` 之后运行; 每个非 EOF token 一行:

```
T <file>:<phys-line|@synth> <kind> <spelling> <chain>
```

- `phys-line = tok->line_no - tok->line_delta` (去掉 `#line` 差值, 得物理行).
- `@synth` 判定: `tok->file->raw == tok->file->contents` (合成 `File` 的判据; 真实文件两者
  是独立分配).
- `<kind>` 复用 `token_kind_name`; `<spelling>` 复用 `print_escaped`.
- `<chain>`: `-` 表示无 provenance; 否则若干 entry 用 `>` 连接, 顺序与 prov 链一致
  (最内层在前). entry 形如:

```
<macro>@<call-file>:<call-line>:<body|argN>:<def-file>:<def-line>
```

  `argN` 从 1 起; 定义缺失(内建 / `-D`)打 `?`; 调用 / 定义的文件名用 `file->name`.
- dump 不打印 `rparen` 与实参列表本身; 它们由访问器提供, 由 `-verify-expansions` 校验.

### `-verify-expansions`

无输出; 成功 0, 失败 1. 对每个有 prov 的 token 检查:

- 链节点非空, `exp` 非空, 深度有界且链上节点不重复.
- `exp->name` / `exp->macro` 非空; `name` 的拼写与 `macro->name` 一致.
- `arg_index` 为 -1 或落在 `[0, arg_count)`; 实参 token 列表非空.
- `arg_index == -1` 时 `exp->rparen` 可为 NULL; `arg_index >= 0` 时 `exp` 必须是 function-like
  (`!macro->is_objlike`) 且不是内建.
- 失败时用 `fprintf(stderr, ...)` 报一条含物理位置的消息, 返回 false (不新增 exit 点).

实现放在新文件 `expansion_dump.c`, 只用上面的访问器; `main.c` 只增旗标与调用.

## 闸门口径

- 本线每步都是"行为不变"步: `make docker-test`(含自举)全绿 + `make docker-snapshot-diff`
  为空(本线不改 parse / sema / codegen, 预期语料不变, 不重置基线) + `test/expand.sh` 该步
  新增的断言(挂进 `make test` 与 `make test-stage2`).
- 一步一提交; 每步汇报固定四项(改了什么 / 为什么 / 闸门结果 / 偏差). 计划内不做任何可观察
  行为变化; 若出现, 视为回归, 先查根因, 不重置快照.
- 红线: 不改 `parse.c` / `sema.c` / `type.c` / `codegen.c`; 不改 `Token.origin` 语义;
  不改展开的 token 文本 / 顺序 / 类型; 不新增 exit 点; 不新增文件域 static; 不引入新编译
  告警; 新代码保持纯 C11 可自举.
- 本线不做: 宏调用节点; 宏展开内部的外科式编辑; formatter; 库化与公共头拆分(阶段 5);
  CST 改动; 代码生成改动.

## 阶段 M1: 数据结构与 body 层(行为不变)

- [x] **M0.1 归档 / 账本 / 基线**: CST 线已归档为 `PLAN-cst.md` / `RESULT-cst.md`(本次
  完成); 本文件与新的 `RESULT.md` 已建立. 本步剩余: 在 `RESULT.md` 回填基线(当前 HEAD,
  `make docker-test` 与 `make docker-snapshot-diff` 结果), 建立设计账本(输入事实 / 歧义
  清单 / 数据模型 / 分支清单 / dump 冻结尾 / 不变量 I1-I5), 确认 `test/expand.sh` 尚未
  存在时 `make test` 不受影响.
  验收: 无代码改动; 两道闸门全绿; 账本与计划一致.

- [x] **M1.1 记录类型 / 访问器 / 通道骨架**:
  `chibicc.h` 前向声明 `MacroExpansion` / `MacroProv`, `Token` 增 `prov`, 声明 11 个访问器
  与两个通道函数; `preprocess.c` 定义结构, `Macro` 增 `def`, 在 `read_macro_definition`
  保存定义处名 token, 实现 `new_prov` 与访问器; `main.c` 增 `-dump-expansions` /
  `-verify-expansions` 旗标, `cc1()` 在 `preprocess(tok)` 之后接入, driver pass-through
  加两个旗标; 新建 `expansion_dump.c`(dump 先输出 `T ... -` 骨架, verify 恒真);
  新建 `test/expand.sh`(harness + 3 个无宏样本)并挂进两个 Makefile 目标.
  验收: 既有通道逐字节不变; 新骨架通道可跑; stage1 / stage2 都通过; snapshot 为空.

- [x] **M1.2 对象宏 / 函数宏 body 层**:
  在 `expand_macro` 的 E2 / E4 建立记录; 宏体 token 挂 body; 写 name / rparen / macro /
  args / arg_count; `origin` 循环保持不动.
  测试: 对象宏, 函数宏体, 嵌套对象宏(A 体里调 B), 重定义 / `#undef` 后旧展开仍指向旧定义,
  `#line` 后的物理行, 头文件定义 + 主文件调用(docker), 空宏.
  验收: 新断言全过; `-E` 与诊断不变; snapshot 为空.

- [x] **M1.3 实参层**:
  `MacroArg` 增序号; `subst` 按"记录写入清单"逐分支挂 body / arg; 普通参数经 `preprocess2`
  预扫描后逐个 prepend.
  测试: 每个实参, 参数重复出现, 嵌套宏在实参内, `#`, `##`(两侧 / 仅一侧 / 空实参),
  `,##__VA_ARGS__`, `__VA_OPT__`, 变参.
  验收: 新断言全过; snapshot 为空.

## 阶段 M2: 内建宏, 合成 token 与收口

- [x] **M2.1 内建宏 / 合成 token**:
  E1 handler 建 builtin 记录; paste / stringize 归 body; `join_adjacent_string_literals`
  的 1160 行保存 / 恢复 `prov`; 相邻字符串合并保留第一个 literal 的 provenance.
  测试: `__LINE__`, `__FILE__`, `__COUNTER__`, `__BASE_FILE__`, stringize, paste,
  相邻字符串; `__DATE__` / `__TIMESTAMP__` 只测记录形状, 不测取值.
  验收: 新断言全过; `-E` 与诊断不变; snapshot 为空.

- [x] **M2.2 验证器 / 文档 / 终态**:
  实现 `-verify-expansions`; 在 docker 里对 `test/*.c` + `include/*.h` 全量跑(本地用
  `#include <stdio.h>` 探测, 失败则跳过并在摘要注明 skipped); 收口 `test/expand.sh`
  计数; 更新 AGENTS.md 的代码地图与测试统计; 在 `RESULT.md` 写逐步记录, 提交表与终态验收.
  终态闸门: `make docker-test`, `make docker-snapshot-diff` 为空, `test/expand.sh`
  (本地片段 + docker 全量), 可选 `THIRDPARTY=tinycc`.
  验收: 判据 1-6 逐项实测; 文档与代码一致.

## 开放决策

本线不新增开放决策. 宏保真策略已于 2026-10-18 由用户拍板(消费展开流 + provenance;
不做宏调用节点; 不做宏内编辑). 余下与库化相关, 但不阻塞本线的决策仍是: 库名与公共头命名,
公共 `Node` 的形状(codegen 字段如何移出公共面), arena 整体 reset 的 API 形态.

## 风险与对策

1. **漏掉 `subst` 分支** -> 按"记录写入清单"逐项对照, 每行至少一个测试;
   `-verify-expansions` 校验实参序号与名称一致性.
2. **共享链被 mutation** -> 只 prepend, 共享尾不可变; 用"参数重复 + 嵌套实参"测试锁住;
   代码里不出现对 `prov` 尾节点的写.
3. **`tokenize_string_literal` 就地覆盖丢 provenance** -> 保存 / 恢复; 相邻字符串测试.
4. **`new_eof` 携带链** -> 显式清空; EOF 不输出.
5. **dump 路径 / 行号不稳定** -> 测试在临时目录用相对文件名; 物理行 = `line_no - line_delta`;
   `#line` 样本锁定.
6. **macOS 缺 Linux include 路径** -> 片段测试自包含; 全量 verify 用 include 探测决定是否
   跳过; docker 为权威.
7. **自举** -> 每步跑 stage2; 只用 C11 / chibicc 已支持的写法; 新文件不引入复杂初始化.
8. **内建宏取值不稳定** -> `__DATE__` / `__TIMESTAMP__` 只断言记录存在与形状, 不断言文本.

## 终态验收(本线完成时逐项实测填写)

- 覆盖: 判据 1 的每一类产物都有 dump 样本; `subst` 清单每一行至少一个测试.
- 查询: 调用点名 / 范围 / 定义 / 实参序号 / 实参列表 / 嵌套链全部由 `test/expand.sh` 断言.
- 行为: `make docker-test` EXIT=0; `make docker-snapshot-diff` 输出
  `snapshot diff: empty`; `-E` / 诊断 / trivia / CST 逐字节不变.
- 验证器: 全量语料 `-verify-expansions` 通过(docker); 本地片段通过.
- 文档: AGENTS.md 的路线图 / 现状 / 测试约定与本线终态一致; `RESULT.md` 完成.
