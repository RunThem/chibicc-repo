# PLAN-l5.md - L5 产品化验证 (formatter / CST 覆盖 / source mapping / 文档)

> 状态: 进行中; 执行记录见 `plans/RESULT-l5.md`
> 基线: `main` @ `c530431`
> 分支: `main` (W1-W6 每步一个提交)
> 范围: CST 结构覆盖, CST formatter MVP (第一个真实消费者), AST source
>        range 与诊断展开点, 文档定位; 顺带关闭两个挂起的未决项
> 拍板: 用户 2026-10-25 ("可以, 按你的来"; 顺序与"在 formatter 过程中拍
>        板相关方案"的策略经确认)

## 1. 目标与边界

### 1.1 背景

L5 是路线图唯一未开工项(`plans/PLAN.md` 阶段 L5, 原阶段 6, 2026-10-19
改必选)。风险清单第 12 条的定位: 合成的 `test/library.sh` 只能证明管线
通, 不能证明 API 好用; **formatter 写不写得动是公共面的真验收** - 写不动
说明公共面缺东西, 回到 L3 补, 不降低 L5 标准。第 11 条: 不能把
"round-trip 通过"当成"formatter 可用", 覆盖率要有 golden 锁。

### 1.2 决策的关闭与时机(本计划一并处理)

- **关闭 (2026-10-25): 展开流 CST**。维持现状: CST 建在预处理前; 展开后
  的语法形态用第 3 层 `libcp_parse`; 展开 token 的来源用 provenance 的
  11 个访问器。理由见 AGENTS.md 未决项里的分析(反向还原不可行: 未选中
  分支 / 指令行与 include 边界 / 零产出展开 / 合成拼写 / 实参间 trivia
  均无法由 provenance 补回)。若将来需要展开流的树形表示, 新增独立
  expanded-view 产物, 不改第 2 层含义。
- **时机后置: per-node user data 扩展点**。formatter(消费第 1/2 层)落地
  后凭 CST 侧的真实手感提候选方案待拍板; 公共 `Node` / `Obj`(第 3/4 层)
  侧的完整证据要等第二个消费者(C→C 打印器, 不在本线, 列为后续)。本线
  不擅自引入 `void *backend`。
- **D7 的增长口径**: 本线预计追加 `libcp_cst_seed_typedef`(W2)与 L5.3 的
  source-range 能力(0-2 个函数/字段, W5 时按需定)。全部是源码兼容的追加;
  `test/public-api.sh` 的 D7 清单同步, `.so` 恰好导出 D7 的闸门不变。

### 1.3 边界

允许改: `libcp/src/cst.c`, `libcp/include/libcp/cst.h`(kind 与函数追加),
`libcp/src/lib.c` / `libcp.h`(新函数落点), `libcp/include/libcp/diagnostic.h`
(字段追加), `libcp/src/sema.c` 或新文件(range 遍历), `main.c`(只读通道
接线如有), `tools/fmt.c`(新增), `Makefile`, `test/cst.sh`,
`test/cst-stats.sh`(新增), `test/fmt.sh`(新增), `test/public-api.sh`
(D7 清单), `README.md`, `docs/**`, `AGENTS.md`。

### 1.4 非目标

- 不改预处理 / parse / sema / codegen 的任何行为; 汇编快照必须空 diff。
- 不做全量 C 文法覆盖: UNPARSED 机制保留, 只消灭两大成因(见 3.1);
  剩余零星 UNPARSED 由 golden 锁住现状。
- 不重排换行 / 不改注释内容 / 不接预处理: formatter MVP 是行保持的空白
  规范化(见 4.1)。
- 不做 C→C 打印器(第二个消费者, 另行立项)。
- 不引入宏调用节点, 不动宏保真策略(2026-10-18 已决)。

## 2. 判据 (完成定义)

**判据 1 - CST 覆盖 golden**
- `test/cst-stats.sh` 对全部 48 个语料文件(test/*.c + include/*.h)统计每
  文件 UNPARSED 数并 golden 锁定; W1 后总数从 770 显著下降(预期 < 60,
  剩 typedef 类与零星), 由 `make test` / `test-stage2` 调用。

**判据 2 - formatter MVP**
- `make fmt` 产出 `tools/fmt`; 对全部语料:
  (a) 幂等 `format(format(x)) == format(x)`;
  (b) 行数不变, 注释数量不变(逐文件比对);
  (c) 不调用预处理, 不写目标文件;
  (d) docker 内 `format 后 -S` 与 `原 -S` 逐字节一致(时间宏固定) -
  即行保持策略下编译产物不变, 含 `__LINE__` 语义。
- `test/fmt.sh` 由 `make test` / `test-stage2` 调用。

**判据 3 - 行为零变化**
- `make docker-test` 全绿(自举 + stage2); `docker-snapshot-diff` 输出
  `snapshot diff: empty`; diagnostic.sh 68 例 / trivia / cst / expand /
  ast 通道逐字节不变(cst.sh 允许**新增** dump 样本, 既有样本不变)。

**判据 4 - 公共面**
- `make check-library` 全绿; D7 变化仅限 1.2 列明的追加; `.so` 恰好
  导出 D7; 头自包含。

**判据 5 - source mapping 与诊断展开点**
- AST 子树能映射回源码 span(start / end token); 宏内错误的诊断回调
  能拿到调用点; 默认诊断输出逐字节不变。

**判据 6 - 记录**
- RESULT 记录 API 手感发现清单(公共面缺口 / 别扭点 / 补救建议), 作为
  对 L3 的回溯依据。

## 3. 现状锚点 (2026-10-25 实测, `main` @ `c530431`)

### 3.1 UNPARSED 实测(48 语料文件, 共 770 span, 30 文件含 / 18 干净)

- **成因一(主导): GNU statement expression `({ ... })`**。cst.c 的
  `cst_parse_primary`(libcp/src/cst.c:319-349)`(` 分支只走
  `cst_parse_comma`, `(` 后跟 `{` 即失败, 整条语句回落 UNPARSED。
  实测: arith.c 的 61 个 UNPARSED 全部来自 `ASSERT(7, ({ int i=2; i+=5;
  i; }));` 类语句(76 条无 `({` 的 ASSERT 全部解析正常); 文件分布
  initializer.c 114, struct.c 85, control.c 75, arith.c 61, variable.c
  45, constexpr.c 41 ... 与 ASSERT 密度一致。
- **成因二: include 提供的 typedef**。`cst_collect_typedefs`(cst.c:245)
  只收集本 TU 的 `typedef` 定义, 注释明言 "an include-provided name
  stays unknown and is not guessed"。`va_list ap;` 在 varargs.c 8 处,
  function.c 类似(`pthread_t` 不在语料, 但同一口径)。
- 其余零星(个别复杂初始化器 / 属性)由 golden 锁现状。

### 3.2 公共面现状

- cst.h: 55 个 `LibcpCstKind` + `LibcpCstNode {kind, tok, children,
  child_count}` + 4 个函数(parse / reconstruct / dump / verify)。
- token.h 公开 `kind` / `loc` / `len` / `trivia` / `trivia_len` /
  `raw_loc` / `raw_len` - formatter 的原料齐备(注释与空白在 trivia,
  拼写在 raw_loc)。
- diagnostic.h: `LibcpDiagnostic` 无 token / 展开点字段; `error_tok`
  用 `tok->file` / `tok->line_no` 定位(宏内错误指向宏体)。
- AST: 每节点只有代表 token(anchor), 无 end token, 无 range API;
  docs/11 明言"计算节点的 start / end 要自己遍历"。

### 3.3 测试与构建现状

- `test/cst.sh`: check() 断言机制 + round-trip / verify / dump 三类,
  61 个 dump 样本覆盖全部 55 kind。
- Makefile: `make test` 串 12 个脚本; 无 tools/ 目录; `-dump-cst` 是
  驱动器只读通道。

## 4. 设计决策

### 4.1 formatter MVP 策略: 行保持的空白规范化

`-S` 快照等价(判据 2d)有一个硬约束: 换行移动会改 `__LINE__` /
`#line` 语义打破快照(2026-10 L5 分析已证)。因此 MVP 策略:

- **不增删换行**: 每个 token 前置 trivia 中的换行数量保持原样; 只重排
  行内空白(连续空白折叠为一个)与缩进(按 CST 层级重算, 制表统一)。
- **注释原样字节**: trivia 中的注释(`/* */` / `//`)作为不可分字节块
  原样输出, 不参与折叠; 注释所在行的缩进同样规范化。
- **UNPARSED / PP_* 原样输出**: 这些跨度内叶子按 trivia+拼写原样发射
  (仍是合法源码, 判据 2d 不受影响)。
- **字符串 / 字面量拼写不动**: 只动 token 之间的空白, 不动任何拼写。
- 缩进单位: 2 空格(与上游 chibicc / 本库源码一致), 每层 BLOCK /
  PP_BRANCH / PP_IF 加一级。
- 幂等性由策略保证: 规范形对"行内空白折叠 + 层级缩进"是不动点。

### 4.2 typedef 供给: caller seed API(推荐, 替代启发式猜声明)

`libcp_cst_seed_typedef(LibcpContext *ctx, const char *name)`: 把
include 提供的 typedef 名注入 CST 的 typedef 表(与
`libcp_context_define` 同族: 消费者知道自己 target 的头, 库不猜)。
理由: 启发式(`IDENT IDENT ;` 猜声明)与 CST "不猜"的既有哲学冲突且
会把真表达式误判; seed 让覆盖率统计(golden)与工具(格式化 va_list
声明)都能拿到结构。消费者按需 seed; 不 seed 时现状不变。
D7 +1。

### 4.3 stats 通道: 脚本实现, 不扩 D7

`test/cst-stats.sh` 直接统计 `-dump-cst` 输出的 `UNPARSED` 行(节点
行即 kind 名), 生成并比对 golden(PLAN.md L5.2 原文两选项之一)。
不加 `-cst-stats` 驱动器旗标, 不加公共函数。

### 4.4 source range: 子树遍历取极值, 不改 parser

`libcp_node_source_range(const LibcpNode *node, LibcpToken **start,
LibcpToken **end)`: 遍历子树收集锚点 token, 按可比坐标(所属文件 +
偏移)取极小 / 极大。不改 parse.c 的构造现场, 不加字段。若实现时发现
公共面缺"token 的可比坐标"(如文件标识不公开), 加最小只读访问器
(W5 拍板点, 预计 0-1 个)。

### 4.5 诊断展开点: 字段追加, 默认输出不变

`LibcpDiagnostic` 追加 `const LibcpToken *tok`(锚点)与
`const LibcpMacroProv *prov`(展开链头, 源 token 为 NULL)两个字段;
默认处理器忽略新字段, 输出逐字节不变(diagnostic.sh 锁定)。回调消费者
可沿 prov 链走到最外层调用点。

## 5. 步骤(每步: 改动 -> 本机闸门 -> docker 闸门 -> 提交)

### W0 - 计划落档与决策关闭

- [ ] W0.1 写本计划; AGENTS.md 路线图 L5 改"进行中"并指向本计划,
  已决事项关闭"展开流 CST"(2026-10-25, 引用 1.2)。提交
  "Add the L5 productization plan"。

### W1 - statement expression 覆盖

- [ ] W1.1 `cst_parse_primary` 的 `(` 分支: `(` 后跟 `{` 时解析复合块
  (复用块级 item 解析), 新增 `CST_STMT_EXPR`(cst.h kind 追加);
  dump / reconstruct / verify 不需要新逻辑(叶子显式, 全 lossless)。
- [ ] W1.2 `test/cst.sh`: 新增 `({ ... })` 的 round-trip / verify /
  dump 样本(空体 / 单语句 / 声明 + 多语句 / 嵌套); 语料全量
  round-trip 不回归。
- [ ] W1.3 闸门: 本机 cst.sh + ast/trivia/expand/diagnostic 通道;
  docker-test + snapshot 空 diff。提交 "Cover GNU statement
  expressions in the CST"。

### W2 - typedef seed API

- [ ] W2.1 `libcp/include/libcp/cst.h` + 实现(`libcp/src/cst.c` 的
  typedef 表经 context 传递, 或挂在 parse 入口的选项; 实现时以最小
  面为准); `test/public-api.sh` D7 清单 +1。
- [ ] W2.2 `test/cst.sh`: seed 后 `va_list ap;` 成结构的 dump 样本;
  `test/library.sh` 消费者加一个 seed 用例。
- [ ] W2.3 闸门同 W1 + 容器 check-library(D7 变化过闸)。提交
  "Add libcp_cst_seed_typedef for include-provided names"。

### W3 - 覆盖率 golden

- [ ] W3.1 `test/cst-stats.sh`: 统计 48 语料文件每文件 UNPARSED 数,
  golden 文件入库; W1/W2 后的实测数即为基线(写入本计划 8 节)。
  `make test` / `test-stage2` 接线。
- [ ] W3.2 提交 "Lock the CST UNPARSED coverage golden"。

### W4 - formatter MVP(第一个真实消费者)

- [ ] W4.1 `tools/fmt.c`: 只走 tokenize_file -> (seed 由 CLI 选项供给)
  -> cst_parse -> 按 4.1 策略发射; `main(int argc, char **argv)`
  独立二进制, 链接 `libcp.a`, 公共头 only。
- [ ] W4.2 `Makefile`: `make fmt`(默认不进 `all`); `test/fmt.sh`:
  幂等 / 行数不变 / 注释数量不变 / UNPARSED 与指令行原样, 由
  `make test` / `test-stage2` 接线。
- [ ] W4.3 docker: `format 后 -S == 原 -S` 逐字节(时间宏固定, 41 语料);
  连同 docker-test / snapshot 空一起作为行为闸门。
- [ ] W4.4 API 手感发现清单记入工作笔记(RESULT 用)。提交 "Add the
  CST formatter MVP as the first real consumer"。

### W5 - source range 与诊断展开点

- [ ] W5.1 `libcp_node_source_range`(4.4 方案; 需要的最小访问器按需
  拍板); 一个改写示例(tools/ 或 test 内)把 AST 子树映射回源码 span。
- [ ] W5.2 `LibcpDiagnostic` 追加 tok / prov 字段; 默认输出不变;
  一个"宏内错误回调拿到调用点"的用例进 `test/library.sh`。
- [ ] W5.3 闸门: diagnostic.sh 68 例逐字节 + 全套 + 容器
  check-library。提交 "Add node source ranges and diagnostic
  expansion points"。

### W6 - 文档与收尾

- [ ] W6.1 README 顶部改为库定位(上游正文保留在下方); docs/ 增补
  formatter 章(能力 / 策略 / 已验证示例 / API 手感结论); docs/09
  D7 表同步; docs/12 时间线加行。
- [ ] W6.2 终闸门: `make docker-test` + snapshot + 容器
  check-library + (一次) tinycc thirdparty; 写 `plans/RESULT-l5.md`
  (含 API 手感清单与 user-data 扩展点的候选结论), 勾完本计划,
  AGENTS.md 勾选。提交 "Record the L5 line and mark the plan
  complete"。

## 6. 闸门

| 闸门 | 命令 | 通过条件 | 步骤 |
|---|---|---|---|
| CST 通道 | `test/cst.sh ./chibicc` | 全量 round-trip + verify + 既有样本不变 + 新样本 | W1-W3 |
| 覆盖 golden | `test/cst-stats.sh ./chibicc` | golden 一致 | W3+ |
| formatter | `test/fmt.sh ./chibicc` | 幂等 / 行数 / 注释数 / 原样跨度 | W4+ |
| 行为 | `make docker-test` | 全绿(自举 + stage2) | 每步 |
| 汇编等价 | `make docker-snapshot-diff` | `snapshot diff: empty` | 每步 |
| formatter 等价 | docker `-S` diff(时间宏固定) | 空 | W4+ |
| 公共面 | 容器 `make check-library` | D7 齐(含新增), `.so` 恰好 D7 | W2/W5/终 |
| 第三方 | `docker-test-thirdparty THIRDPARTY=tinycc` | 全绿 | 终态 |

## 7. 风险与对策

1. **formatter 幂等性破坏**(规范形非不动点): fmt.sh 逐文件 diff 二次
   输出, 定位到具体 span; 策略上只做折叠与缩进两类变换。
2. **行保持下仍打破 -S 等价**: 唯一可疑点是 directive 行与续行内的
   空白(如 `#  define` 折叠成 `#define` 改变预处理行为)- PP_* 节点
   与 UNPARSED 原样输出即规避; W4.3 逐字节验证。
3. **cst.c 新文法回归**: 叶子序列由 round-trip + verify 逐指针锁,
   新形状只增 dump 样本; 编译路径不感知 cst.c。
4. **D7 追加破坏公共面闸门**: public-api.sh 清单同步; `.so` exact-D7
   在容器验证; 追加均为源码兼容。
5. **statement expression 内含 typedef 声明**: 块内 item 解析已依赖
   同一 TU 的 typedef 收集, `({ int x=2; ... })` 内建声明是 decl 起始
   关键字, 不依赖 seed; seed 只影响 include 提供的名字。
6. **source range 的可比坐标缺口**: W5 先探公共面, 缺则加最小访问器
   (拍板点), 不改 parser 构造现场。
7. **计划文档里 L5.4 的旧目标(`libchibicc/README.md`)**: 已被 docs/
   13 文件取代, W6 按现状执行(README 顶部 + docs 增补), 不再新建。

## 8. 基线数据(执行时回填)

- W1 前: 770 UNPARSED / 30 文件(3.1 分布)。
- W1 后: ___ (预期剩 typedef 类 + 零星, 目标 < 60)。
- W1+W2 后(seed va_list): ___ 。
- formatter 首版手感记录: 见 RESULT。

## 9. 回退

每步一个提交, `git revert` 单独恢复; D7 的追加若需回退, 连同
public-api.sh 清单一起; formatter 是新增目录与目标, 删除即回退。
