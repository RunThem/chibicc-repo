# RESULT.md - 按当前代码校准 docs 执行记录

本文件记录 `PLAN.md` (按当前代码校准 docs) 的实际执行结果. 本线开工 HEAD 为 `c57acbe` (`Record the AGENTS and docs dedup line`), 宿主为 `Linux` / `x86_64` (原生路径), 日期 2026-11-21.

## 用户决定 (2026-11-21)

- 用户要求"根据代码来更新 docs"; 事实来源是当前公共头, 实现, 测试与构建系统, 不是旧计划或旧文档.
- 用户批准本计划的 14 个步骤, 范围边界, 每步闸门与全线验收标准; 本线只改 docs 与 plans, 发现的实现缺陷只记录不修.
- 未决项: 无.

## 提交一览

| 步骤 | hash | subject |
|---|---|---|
| S1 | `aeb769d` | Archive the agents dedup line and add the docs calibration plan |
| S2 | `538d194` | Refresh the README baseline and five-minute example |
| S3 | `34e99ec` | Align the overview design principles with the current tree |
| S4 | `c2890e8` | Correct the architecture contracts, error model, and directory map |
| S5 | `164e724` | Calibrate the token and preprocessor layer docs |
| S6 | `61d112b` | Describe the CST declaration and directive shapes |
| S7 | 本记录所在提交 | Correct the parse node counts and field paths |

## 各步记录

### S1 - 归档上一线并建立本线与快照基线

- 做了什么: 从 `HEAD` 取出现行 `plans/PLAN.md` 与 `plans/RESULT.md`, 写入 `plans/PLAN-agents-dedup.md` 与 `plans/RESULT-agents-dedup.md`, 文件头各加两行归档说明; 新建本线 `plans/PLAN.md` (状态改为已批准) 与 `plans/RESULT.md`; 在 `c57acbe` 的临时 worktree `/tmp/libcp-docs-base` 生成 41 份汇编基线 `/tmp/libcp-docs-base-s/*.s`.
- 测试: `uname -s` 为 `Linux`, `uname -m` 为 `x86_64`, 走原生路径; `make test-all` 退出 0; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make format-check` 与 `make check-context` 退出 0; 41 份当前树 `.s` 与基线 `diff -ru` 无输出, 输出 `snapshot diff: empty`; `git diff --check` 通过.
- 偏差: 计划 S1 写的快照命令是 `./chibicc -S <file>`, 实测 `test/macro.c` 的 `#include M13 >` 需要 `-Itest`, 且 `test/macro.c` 会把 `__DATE__` / `__TIME__` 编进字符串, 必须固定. 实际改用 `Makefile` 的 `SNAPSHOT_RUN` 等价命令: `./chibicc -Iinclude -Itest -D__TIMESTAMP__=... -D__DATE__=... -D__TIME__=... -S -o <out> <file>`. 这是测试命令修正, 不影响范围与验收.
- 验收: 归档文件存在且带归档说明; 新 PLAN 与 RESULT 标题正确; 41 份快照 diff 为空; 本步提交只含 `plans/`.

### S2 - `docs/README.md` 基线与状态

- 做了什么: 基线从 `9b06d35` (2026-11-07) 改为 `c57acbe` (2026-11-21); 阶段 5 的函数数改为"阶段 5 时 36 个, L5 后 38 个, 0.2.0 后 39 个"; `tools/fmt.c` 说明补上宿主 `fmt` 与自举 `stage2/fmt` 都不进 `all` / `install`; 五分钟示例的循环加 `if (o->is_function)`, 预期输出改为只列 `global: square, function=1`.
- 测试: `git cat-file -e c57acbe` 通过; `grep` 命中 `0.2.0`, `c57acbe`, `36 个`, `38 个`, `39 个`; 从文档代码块抽出示例编译运行, 输出恰好一行 `global: square, function=1`; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 无.
- 验收: 基线行只出现 `c57acbe`; 36/38/39 的时间线清楚; 示例编译并只输出一行; `docs-links` 通过.

### S3 - `docs/01-overview.md` 设计原则与成熟度

- 做了什么: 1.5.6 删除"链表当集合, 查找 O(n)"的全库概括与"一个 `LibcpNode` 容纳所有字段, 不做 union"; 改写为"token / 对象 / 声明链仍用链表; 名字与 tag 作用域, 宏表, `pragma once`, include guard 用哈希表; `LibcpNode` 是公共头加十个字段族的匿名 union, x86-64 下 144 字节"; 把"解析期间不释放"改为"不显式 `LIBCP_FREE`, `LIBCP_REALLOC` 可能搬移或归还旧块, 对象级释放在 `reset` / `free` 边界". 1.9 改为"根目录的库外 C 源文件只有 `main.c` 与 `codegen.c`, 驱动器内部头是 `driver.h`". 1.10 的 309 改为 313.
- 测试: `! grep -n "不做 union"` 通过; `grep -n "313"` 命中第 181 行; `grep -n "库外 C 源文件"` 命中第 169 行; `grep -n "十个字段族\|144 字节"` 命中第 106 行; `sizeof(LibcpNode)` 实测 144; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 无. 144 字节补了 "x86-64 构建下" 限定, 因为结构体大小随 ABI 变化.
- 验收: "不做 union" 消失; 313 出现且与 `test/cst.sh` 的 313 行 `testing` 一致; 根目录表述只限定库外 C 源; `docs-links` 通过.

### S4 - `docs/02-architecture.md` 层间契约, 生命周期, 错误模型与目录

- 做了什么: 2.4.4 明确 `ND_TYPEDEF` 的声明类型在 `ty`, `ND_CAST` / `ND_DECL` / `ND_GVAR_DECL` / `ND_FUNCDEF` / `ND_ENUM_CONST` 的目标类型在 `ty_op`; 2.4.5 把 `var`, `val`, `generic_sel`, `is_implicit`, `init_resolved` 改为 `decl.var`, `value.val`, `ctrl.generic_sel`, `value.is_implicit`, `decl.init_resolved`; 2.5.3 改为"不显式 `LIBCP_FREE`, `LIBCP_REALLOC` 可能搬移或归还旧块, 逐块释放在 `reset` / `free`"; 2.6.1 补 `libcp_expansion_verify` 直接写 stderr 的例外与默认处理器的两种输出; 2.9 删除"本章后面的例子"; 2.10 把 `AGENTS.md` 的"开放决策"改为"已定边界", 目录地图补 `bench/`, `docs/`, 根级 `Makefile` / `README.md` / `AGENTS.md` / `Dockerfile` / `libcp.pc.in` / `LICENSE`, `plans/` 改为三态描述, `ast.h` 补 `LibcpNodeFamily` 与 `libcp_node_family`, `allocator.h` 补构建期开关.
- 测试: `! grep -n "开放决策"` 与 `! grep -n "本章后面"` 通过; `grep -n "bench/"` 命中第 349 行; `grep -n "decl.var\|value.val\|ctrl.generic_sel"` 命中第 133, 135 行; `grep -n "LIBCP_DEFAULT_ALLOCATOR"` 命中第 321 行; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 无. 目录地图额外补了 `LICENSE`, 它是已跟踪的根级文件.
- 验收: 2.4 的类型字段归属与 `parse.c` 一致; 2.5.3 不再声称零释放; 2.6 明确 verify 例外; 2.10 包含 `bench/`, `docs/`, 根级构建文件与三态 `plans/`; 旧字段名在该节清零; `docs-links` 通过.

### S5 - `docs/03-layer1-token.md` 与 `docs/04-preprocess.md`

- 做了什么: 03 的 3.2.2 / 3.3.2 把 `File.*` 改为 `LibcpFile.*`; 3.3.3 / 3.5 说明 `TK_PP_NUM` 只覆盖 pp-number 数值字面量, 字符常量在词法阶段就是 `TK_NUM` 并带 `val` / `ty`; 3.4.4 / 3.6 / 3.8 把 byte-exact 重建限定为 `tokenize_file` 与已处于扫描缓冲形态的内存输入, 写出原始 `\uXXXX` 会让 `trivia_len` 为负并指向 11.5.8. 04 的 4.4.3 把 provenance 挂接拆成函数宏 `subst` 与内建/对象宏 `expand_macro` 两类; 4.4.2 / 4.8 说明 `#include_next` 用 context 全局 `include_next_idx`, 不是 per-file 栈, 重复调用可能再次命中同一目录.
- 测试: `grep -n "TK_PP_NUM"` 的四处都只描述数值字面量; `grep -n "LibcpFile"` 命中 3.2.2 / 3.3.2; 无裸 `File.`; `grep -n "subst\|expand_macro"` 命中两类挂接; `grep -n "include_next_idx"` 命中 4.4.2 与 4.8; `/tmp/ucn_probe.c` 对 `"\\u00e9 x\n"` 实测第二个 token 的 `trivia_len = -5`; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 无. 11.5.8 在 S12 才新增, 本步先建立引用, 全线验收时该子节已存在.
- 验收: 03 不再把字符常量写成 pp-num; 03 的 byte-exact 声明有输入形态限定; 04 的 provenance 挂接分三类; 04 的 `#include_next` 不再是 per-file 栈描述; `docs-links` 通过.

### S6 - `docs/05-layer2-cst.md` 声明形状与指令节点

- 做了什么: 5.4.3 改为"声明拆成 `CST_DECL_SPECS`, 一个或多个 `CST_DECLARATOR`, 分号; 有初始化器时该声明符包在 `CST_INIT_DECLARATOR` 中"; 5.4.4 补 `#include_next` 在 `cst_pp_kind` 中没有专门分支, 落到 `CST_PP_DIRECTIVE`; 5.9 的 typedef 限制补 `libcp_cst_seed_typedef` 预置, 并说明该配置跨 reset 保留.
- 测试: `./chibicc -dump-cst` 对 `int x;` 输出 `DECL -> DECL_SPECS, DECLARATOR, ;`, 对 `int y = 1;` 输出 `DECL -> DECL_SPECS, INIT_DECLARATOR(DECLARATOR, =, PRIMARY), ;`; `grep -n "CST_PP_DIRECTIVE"` 命中 5.4.4; `grep -n "seed_typedef"` 命中 5.2.4 示例与 5.9; CST 种类计数 57 不变; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 无.
- 验收: 无初始化器声明不再写成 `CST_INIT_DECLARATOR`; `#include_next` 的落点明确; `seed_typedef` 缓解存在; 57 种节点计数不变; `docs-links` 通过.

### S7 - `docs/06-layer3-parse.md` 计数, 字段路径与示例

- 做了什么: 6.3.1 把解析器产出从 66 改为 65, 不产出列表从 3 种改为 4 种并加入 `ND_NULL_EXPR`, 写明"46 个直接构造 + 21 个 `infix_ops[].kind` - 2 个重复 = 65"的计数方法; 6.2.1 / 6.2.2 / 6.2.3 / 6.2.4 / 6.4.3 / 6.4.4 把 `name_tok`, `attr`, `decl_init`, `body`, `op`, `is_post`, `is_elvis`, `arrow_tok`, `is_implicit`, `Member.width_expr` 改为字段族路径; 6.7.2 / 6.7.4 示例改用 `decl.name_tok` / `decl.body` / `decl.decl_init`; 6.8 改写 `ND_IDENT.var`, 并把文法诊断写为 A 组 15 例 / 21 个调用点 / 17 条消息.
- 测试: 用脚本合并 `libcp_new_node` / `libcp_new_unary` / `libcp_new_binary` 的 46 个 kind 与 `infix_ops[].kind` 的 21 个, 去重 2 个后为 65; 69 减去不产出的 `ND_VAR` / `ND_VLA_PTR` / `ND_MEMZERO` / `ND_NULL_EXPR` 也为 65; `awk` 统计 `test/diagnostic.sh` 的 A 组 15 个 `snippet`; `parse.c` 有 21 个 `libcp_error_tok` 调用点与 17 条消息; `/tmp/parse_example.c` 用 `cc -std=c11 -fsyntax-only -Wall -Wextra -Ilibcp/include` 通过; `! grep -n "66 种"` 通过; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 计划写"46 个字面量构造种类加 19 个 `infix_ops[].kind`"; 实测 `infix_ops[].kind` 去重后是 21 个, 与 46 个直接构造种类有 `ND_ASSIGN` / `ND_COND` 两个重复, 因此文档按 46 + 21 - 2 = 65 写. 这是计数方法修正, 结论 65 不变.
- 验收: 69 与 65 的口径分开写清; `ND_NULL_EXPR` 在不产出列表; 06 中不再有裸 `name_tok` / `body` / `decl_init` / `ND_IDENT.var`; 示例通过语法检查; `docs-links` 通过.

## 验收

S1 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| 归档文件 | 通过 | `plans/PLAN-agents-dedup.md` 193 行, `plans/RESULT-agents-dedup.md` 185 行, 各带两行归档说明 |
| 新 PLAN / RESULT | 通过 | 标题分别为 `PLAN.md - 按当前代码校准 docs 执行计划` 与 `RESULT.md - 按当前代码校准 docs 执行记录` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与 `c57acbe` 基线 diff 为空 |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make format-check` | 通过 | 退出 0 |
| `make check-context` | 通过 | accessors 0, `cg_ctx` 0, accessor symbols 0 |
| 改动范围 | 通过 | 本步提交只含 `plans/` |

S2 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| 基线 | 通过 | `docs/README.md` 只写 `c57acbe` (2026-11-21); `git cat-file -e c57acbe` 通过 |
| 函数计数 | 通过 | 阶段 5 36 个, L5 后 38 个, 0.2.0 后 39 个; 与 `test/public-api.sh` 的 39 个 D7 名单一致 |
| 五分钟示例 | 通过 | 从文档抽出的示例编译运行, 输出恰好 `global: square, function=1` |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与基线 diff 为空 |

S3 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| "不做 union" 清零 | 通过 | `! grep -n "不做 union" docs/01-overview.md` |
| 字段族事实 | 通过 | 十个族名与 `libcp/include/libcp/ast.h` 的 union 一致; `sizeof(LibcpNode)` 实测 144 |
| CST check 计数 | 通过 | 313 与 `test/cst.sh` 的 313 行 `testing` 一致 |
| 根目录表述 | 通过 | 只限定"库外 C 源文件"为 `main.c` 与 `codegen.c` |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与基线 diff 为空 |

S4 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| 2.4 字段归属 | 通过 | `ND_TYPEDEF` 用 `ty`, 其余五个记录与 `ND_CAST` 用 `ty_op`, 与 `parse.c` 一致 |
| 2.5.3 释放语义 | 通过 | 改为"不显式 `LIBCP_FREE`", 保留 `reset` / `free` 逐块释放 |
| 2.6 错误模型 | 通过 | `libcp_expansion_verify` 的 stderr 例外与默认处理器两种输出都写明 |
| 2.10 目录地图 | 通过 | 含 `bench/`, `docs/`, 根级构建文件与三态 `plans/`; `ast.h` 与 `allocator.h` 条目已补 |
| 旧字段与旧引用 | 通过 | `开放决策` 与 `本章后面` 清零; `init_resolved` / `generic_sel` / `is_implicit` 只以族路径出现 |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与基线 diff 为空 |

S5 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| 字符常量种类 | 通过 | `tokenize.c` 的 `read_char_literal` 产出 `TK_NUM`; 文档不再把它算进 `TK_PP_NUM` |
| `LibcpFile` 字段名 | 通过 | 3.2.2 四处改为 `LibcpFile.*`; 无裸 `File.` |
| byte-exact 输入限定 | 通过 | 3.4.4 / 3.6 / 3.8 写明原始 `\uXXXX` 例外并指向 11.5.8 |
| UCN 实测 | 通过 | `/tmp/ucn_probe.c` 输出第二个 token `trivia_len=-5` |
| provenance 归属 | 通过 | 函数宏在 `subst`, 内建/对象宏在 `expand_macro` |
| `#include_next` | 通过 | 4.4.2 表与 4.8 都写 context 全局 `include_next_idx` |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与基线 diff 为空 |

S6 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| 声明形状 | 通过 | `-dump-cst` 显示无初始化器是 `DECLARATOR`, 有初始化器是 `INIT_DECLARATOR` |
| `#include_next` 落点 | 通过 | `cst_pp_kind` 无专门分支; 文档写 `CST_PP_DIRECTIVE` |
| `seed_typedef` 缓解 | 通过 | 5.9 写明预置方式与跨 reset 保留 |
| CST 种类计数 | 通过 | `cst.h` 枚举 57 项, 文档计数不变 |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与基线 diff 为空 |

S7 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| 69 / 65 计数 | 通过 | 69 个枚举 kind; 直接构造 46 + infix 21 - 重复 2 = 65 |
| 不产出列表 | 通过 | `ND_VAR`, `ND_VLA_PTR`, `ND_MEMZERO`, `ND_NULL_EXPR` 四种 |
| 字段路径 | 通过 | 06 中的声明字段都写成 `decl.*`, `access.*`, `ctrl.*`, `value.*`, `incdec.*` |
| 示例可编译 | 通过 | `/tmp/parse_example.c` 通过 `cc -fsyntax-only -Wall -Wextra` |
| 诊断计数 | 通过 | A 组 15 例; `parse.c` 21 个调用点 / 17 条消息 |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与基线 diff 为空 |

全线验收待 S14 补齐.

## 偏差与原因

- S1: 计划里的快照命令缺少 `-Itest` 与固定时间宏, 第一次生成在 `test/macro.c` 失败并留下部分 `.s`. 原因: 计划沿用了上一线的简写, 没有抄全 `Makefile` 的 `SNAPSHOT_RUN`. 处理: 清空临时目录, 按 `Makefile` 的命令重新生成基线. 影响: 不阻塞后续步骤; 后续每步沿用同一命令.
- 归档文件在 Git 中显示为新增, 固定槽位 `plans/PLAN.md` 与 `plans/RESULT.md` 显示为修改, 因为归档后立即复用了原路径. 归档正文与旧内容一致, 只多头部四行.
- S2: 无偏差.
- S3: 无偏差. 字段族大小写成 "x86-64 构建下", 因为 `sizeof(LibcpNode)` 随 ABI 变化.
- S4: 无偏差. 目录地图补了计划未列出的 `LICENSE`, 因为它是已跟踪的根级文件.
- S5: 无偏差. 03 的 11.5.8 引用在 S12 新增该子节前暂不可达; 全线验收时会补齐.
- S6: 无偏差.
- S7: 计划写 `infix_ops[].kind` 为 19 个, 实测去重后 21 个; 文档按 46 + 21 - 2 = 65 写, 结论不变.
- 无范围变更.

## 后续影响

- 本线只改 `docs/` 与 `plans/`; 审计发现的实现缺陷只写入 `docs/11-limitations.md` 与本文的后续项, 不修代码.
- 后续项候选: `libcp_tokenize_buffer` 的 NUL 截断, `libcp_context_define` 的 provenance `macro` 名带 `=body`, reset 后 stdin 缓存失效, 原始 `\uXXXX` memory 输入的 raw 坐标错位, `codegen.c` 文件头 (a)/(b) 注释与实现不一致.
- 41 份汇编基线固定在 `/tmp/libcp-docs-base-s`; 每步用同一命令重建 `/tmp/libcp-docs-cur-s` 并 diff.

## 闸门实测

### S1

- `uname -s` / `uname -m`: `Linux` / `x86_64`.
- `make test-all`: 退出 0, 末尾为 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 基线快照: `c57acbe` 临时 worktree `/tmp/libcp-docs-base` 生成 41 份 `/tmp/libcp-docs-base-s/*.s`.
- 当前树快照: 41 份 `/tmp/libcp-docs-cur-s/*.s`; `diff -ru /tmp/libcp-docs-base-s /tmp/libcp-docs-cur-s` 无输出, 输出 `snapshot diff: empty`.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make format-check`: 退出 0.
- `make check-context`: 退出 0; accessors 0, `cg_ctx` 0, accessor symbols 0.
- `git diff --check`: 通过.
- 归档行数: `plans/PLAN-agents-dedup.md` 193, `plans/RESULT-agents-dedup.md` 185; 旧文件分别为 191 与 183, 均只多头部四行.

### S2

- `git cat-file -e c57acbe`: 通过.
- `grep -n "0.2.0\|c57acbe\|36 个\|38 个\|39 个" docs/README.md`: 命中第 9, 10, 12 行.
- 文档示例: 抽出代码块编译, 运行输出一行 `global: square, function=1`, 与预期 diff 为空.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.

### S3

- `! grep -n "不做 union" docs/01-overview.md`: 通过.
- `grep -n "313" docs/01-overview.md`: 第 181 行, `test/cst.sh` 共 313 次 check.
- `grep -n "库外 C 源文件" docs/01-overview.md`: 第 169 行.
- `sizeof(LibcpNode)`: 144 (x86-64).
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.

### S4

- `! grep -n "开放决策" docs/02-architecture.md`: 通过.
- `! grep -n "本章后面" docs/02-architecture.md`: 通过.
- `grep -n "bench/" docs/02-architecture.md`: 第 349 行.
- `grep -n "decl.var\|value.val\|ctrl.generic_sel" docs/02-architecture.md`: 第 133, 135 行.
- `grep -n "LIBCP_DEFAULT_ALLOCATOR" docs/02-architecture.md`: 第 321 行.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.

### S5

- `grep -n "TK_PP_NUM" docs/03-layer1-token.md`: 第 77, 150, 192, 228 行, 只描述 pp-number 数值字面量; 第 228 行补字符常量例外.
- `grep -n "LibcpFile" docs/03-layer1-token.md`: 第 42-45, 97, 121, 165, 167 行; 无裸 `File.`.
- `grep -n "subst\|expand_macro" docs/04-preprocess.md`: 第 136, 177, 183, 189 行; 函数宏与内建/对象宏分开.
- `grep -n "include_next_idx" docs/04-preprocess.md`: 第 151, 375 行.
- UCN 复现: `/tmp/ucn_probe.c` 对 `"\\u00e9 x\n"` 输出 `kind=1 raw_len=6`, 随后 `kind=0 trivia_len=-5`.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.

### S6

- `./chibicc -dump-cst /tmp/cst1.c`: `DECL` 下是 `DECL_SPECS`, `DECLARATOR`, `;`.
- `./chibicc -dump-cst /tmp/cst2.c`: `DECL` 下是 `DECL_SPECS`, `INIT_DECLARATOR(DECLARATOR, =, PRIMARY)`, `;`.
- `grep -n "CST_PP_DIRECTIVE" docs/05-layer2-cst.md`: 第 98, 186 行; 第 186 行写明 `#include_next` 的落点.
- `grep -n "seed_typedef" docs/05-layer2-cst.md`: 第 156, 311, 325 行.
- CST 种类: `cst.h` 枚举 57 项.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.

### S7

- 节点计数: `ast.h` 69 个 `ND_*`; 直接构造 46 个, `infix_ops[].kind` 21 个, 重复 `ND_ASSIGN` / `ND_COND` 两个, 合并 65 个; 不产出 `ND_VAR` / `ND_VLA_PTR` / `ND_MEMZERO` / `ND_NULL_EXPR`.
- 诊断计数: `test/diagnostic.sh` A 组 15 个 `snippet`; `parse.c` 21 个 `libcp_error_tok` 调用点, 17 条不同消息.
- 示例: `/tmp/parse_example.c` 通过 `cc -std=c11 -fsyntax-only -Wall -Wextra -Ilibcp/include`.
- `! grep -n "66 种" docs/06-layer3-parse.md`: 通过.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.

- `! grep -n "开放决策" docs/02-architecture.md`: 通过.
- `! grep -n "本章后面" docs/02-architecture.md`: 通过.
- `grep -n "bench/" docs/02-architecture.md`: 第 349 行.
- `grep -n "decl.var\|value.val\|ctrl.generic_sel" docs/02-architecture.md`: 第 133, 135 行.
- `grep -n "LIBCP_DEFAULT_ALLOCATOR" docs/02-architecture.md`: 第 321 行.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.
