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
| S3 | 本记录所在提交 | Align the overview design principles with the current tree |

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

全线验收待 S14 补齐.

## 偏差与原因

- S1: 计划里的快照命令缺少 `-Itest` 与固定时间宏, 第一次生成在 `test/macro.c` 失败并留下部分 `.s`. 原因: 计划沿用了上一线的简写, 没有抄全 `Makefile` 的 `SNAPSHOT_RUN`. 处理: 清空临时目录, 按 `Makefile` 的命令重新生成基线. 影响: 不阻塞后续步骤; 后续每步沿用同一命令.
- 归档文件在 Git 中显示为新增, 固定槽位 `plans/PLAN.md` 与 `plans/RESULT.md` 显示为修改, 因为归档后立即复用了原路径. 归档正文与旧内容一致, 只多头部四行.
- S2: 无偏差.
- S3: 无偏差. 字段族大小写成 "x86-64 构建下", 因为 `sizeof(LibcpNode)` 随 ABI 变化.
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
