# RESULT.md - AGENTS.md 与 docs 去重执行记录

本文件记录 `PLAN.md` (AGENTS.md 与 docs 去重) 的实际执行结果. 本线开工 HEAD 为 `1fb97d5`, 宿主为 `Linux` / `x86_64`, 日期 2026-11-21.

## 用户决定 (2026-11-21)

- 用户要求执行上一轮冗余分析中的去重方向.
- 未决项"是否把 53 行的 `计划与记录格式` 移到 `plans/` 模板"拍板为"写成模板".
- 未决项"是否在 CI 增加文档链接检查"拍板为"增加".
- 用户于 2026-11-21 批准执行; 按 S1-S7 一步一提交.

## 提交一览

| 步骤 | hash | subject |
|---|---|---|
| S1 | `992cc2c` | Archive the node field families line and add the dedup plan |
| S2 | `f4ef947` | Replace duplicated architecture sections with docs links |
| S3 | `7abaa5a` | Merge the test path, build, and environment sections |
| S4 | `43cfb2d` | Deduplicate the settled boundaries and declare docs ownership |
| S5 | `dc8cf4b` | Move the plan and result formats into templates |
| S6 | `c76df98` | Add the document link check to tests and CI |
| S7 | 本记录所在提交 | Record the AGENTS and docs dedup line |

## 各步记录

### S1 - 归档旧线并建立本线与基线

- 做了什么: 把已完成的 LibcpNode 字段族线归档为 `plans/PLAN-node-families.md` 与 `plans/RESULT-node-families.md`, 文件头加归档说明; 新建本线 `plans/PLAN.md` 与 `plans/RESULT.md`; 在 `1fb97d5` 的临时 worktree 生成 41 份汇编基线.
- 测试: `uname -s` 为 `Linux`, `uname -m` 为 `x86_64`, 走原生路径; `make test-all` 退出 0; 基线 41 份 `.s`, 当前树 41 份 `.s`, `diff -ru` 为空; `git diff --check` 通过.
- 偏差: 归档文件在新树中显示为新增, 固定槽位 `plans/PLAN.md` 显示为修改, 因为归档后立即复用了原路径; 归档文件正文与旧内容一致, 只多两行头部.
- 验收: 归档文件存在且带归档说明; 新 PLAN 与 RESULT 标题正确; 全量测试与快照 diff 通过; 本步提交只含 `plans/`.

### S2 - 架构, 设计原则与代码地图改为链接

- 做了什么: 把 `目标架构与库边界` 压成一段五层名称与库边界; 删除逐文件代码地图; 把 `设计原则` 压成一段结论; 根目录描述补上 `driver.h`; 章节引用按 D8 写全文件名.
- 测试: `wc -l AGENTS.md` 为 154; `grep` 命中 `docs/01-overview.md` 与 `docs/02-architecture.md`; `hashmap.c` 与 `range.c` 不再出现; `driver.h` 在开头描述命中; `git diff --check` 通过; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 无.
- 验收: 三节合计 6 行; 三个 docs 引用存在; 代码地图与逐层解释消失; 根目录描述包含 `main.c`, `driver.h`, `codegen.c`; 行数 154, 小于等于 160; 闸门全绿.

### S3 - 测试路径, 构建测试与平台陷阱合并

- 做了什么: 把 `测试路径判定`, `构建与测试`, `平台陷阱与测试环境` 合并为 `测试路径与构建`; 删除逐目标说明, 完整测试顺序与重复的 Docker 目标列表; 保留宿主判定, 六个精确命令, 三条闸门, `make bench` 仅原生的限制, Rosetta 下 tcc 三个用例与容器 `/work` 可写层陷阱; 细节链接到 docs/10 10.8, 10.10 与 10.12.
- 测试: `wc -l AGENTS.md` 为 127; `Rosetta` 与 `/work` 各 1 次; 六个命令全部命中; `test/driver.sh` 不再出现; `git diff --check` 通过; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 无. `docs/10` 没有 bench 采集项与 Rosetta 陷阱, 因此这些操作信息留在 `AGENTS.md`, 只把环境细节链接到 docs.
- 验收: 只剩一个测试路径小节; 六个命令保留; bench 宿主限制保留; Rosetta 与 `/work` 各只出现一次; 完整顺序表消失; 行数 127, 小于等于 140; 闸门全绿.

### S4 - 已定边界去重与单一事实来源规则

- 做了什么: 在 `全局约定` 增加 `docs/` 与 `AGENTS.md` 的单一事实来源规则; 删除命名条目末尾重复的 ABI 句; 把公共面条目从"10 个公共头, `.a` 符号, `.so` 导出"收敛为"只暴露 D7, 内部符号不是 API"; 九个关键决策全部保留.
- 测试: `wc -l AGENTS.md` 为 128; `grep -c "只承诺源码兼容"` 为 1; 九个决策词全部命中; `10 个公共头` 与 `.so 只导出 D7` 不再出现; `git diff --check` 通过; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 无.
- 验收: single-owner 规则存在且指向 `docs/`; 九个关键决策保留; 重复 ABI 句只剩头部; 公共面细节移出; 行数 128, 小于等于 135; 闸门全绿.

### S5 - 计划与记录格式模板化

- 做了什么: 新增 `plans/PLAN-TEMPLATE.md` 与 `plans/RESULT-TEMPLATE.md`, 把 PLAN 的 9 项必含内容, 每步 6 项模板, RESULT 的 8 项必含内容与每步记录模板写成可复制文件; `AGENTS.md` 的 52 行格式正文压成 3 条指针.
- 测试: 两个模板存在; `grep "PLAN-TEMPLATE"` 与 `grep "RESULT-TEMPLATE"` 在 `AGENTS.md` 命中; `每步模板` 不再出现; 六个 PLAN 字段与六个 RESULT 字段在模板中命中; `wc -l AGENTS.md` 为 81; `git diff --check` 通过; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 无.
- 验收: 两个模板存在且字段完整; `AGENTS.md` 不再有完整格式正文; 固定槽位与批准流程仍在; 行数 81, 小于等于 100; 闸门全绿.

### S6 - 文档链接检查脚本与 CI

- 做了什么: 新增 `test/docs-links.sh`, 检查本地 markdown 链接与 `AGENTS.md` 的文件/章节引用; 加入 `Makefile` 的 `test` 目标; 新增 `.github/workflows/docs.yml`; `docs/10-build-install.md` 10.8 增加一行; `AGENTS.md` 测试约定增加一句.
- 测试: `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; 临时目录的两个负例均返回非零; `make test-all` 日志第 87-88 行显示脚本被调用且通过, 退出 0; `python3 -c 'import yaml...'` 解析 workflow 通过; 41 份快照 diff 为空; `git diff --check` 通过.
- 偏差: 无. 链接检查只进 `make test`, 不进 `test-stage2`, 因为脚本不依赖编译器产物; 这一点已写入 `AGENTS.md`.
- 验收: 当前树通过; 两个负例被检出; `make test` 包含脚本; workflow 在 push main 与 pull request 触发; docs/10 10.8 有对应行; AGENTS 82 行, 小于等于 100; 闸门全绿.

### S7 - 全线闸门与 RESULT 收尾

- 做了什么: 跑全线闸门; 修正 S6 记录中一个被链接检查当成真实链接的示例; 补齐提交表, 各步记录, 验收对照, 偏差与后续影响.
- 测试: `make format-check` 退出 0; `make check-context` 全 0; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; 41 份快照 diff 为空; `git diff --check` 通过; `git diff --stat 1fb97d5..HEAD` 只含允许文件.
- 偏差: S6 记录里的示例 markdown 链接被 `test/docs-links.sh` 当成真实链接, S7 改成纯文本描述; 这是闸门按预期工作, 不影响代码或验收.
- 验收: 全线闸门全绿; `AGENTS.md` 82 行; 48 个引用可达; 九个决策保留; 七步提交与 PLAN 一一对应.

## 验收

逐条对照 `PLAN.md` 的全线闸门:

| 项目 | 结果 | 证据 |
|---|---|---|
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与 `1fb97d5` 基线 diff 为空 |
| `make check-context` | 通过 | accessors 0, TLS 0, `cg_ctx` 0, accessor symbols 0 |
| `make format-check` | 通过 | 退出 0 |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| 改动范围 | 通过 | `git diff --stat 1fb97d5..HEAD` 只含 `AGENTS.md`, `plans/`, `test/docs-links.sh`, `.github/workflows/docs.yml`, `Makefile`, `docs/10-build-install.md` |
| 最终行数 | 通过 | `AGENTS.md` 82 行, 在 75-100 |
| docs 引用 | 通过 | 48 个引用全部可达; 6 个章节引用标题存在 |
| 九个关键决策 | 通过 | 9 个 grep 全部命中 |

各步验收:

| 步骤 | 结果 |
|---|---|
| S1 | 通过: 归档与新建计划, 全量测试与快照基线 |
| S2 | 通过: 三节压到 6 行, 代码地图移除, `driver.h` 修正 |
| S3 | 通过: 合并测试路径, 保留六个命令与两条 docs 未覆盖的陷阱 |
| S4 | 通过: single-owner 规则与九个决策 |
| S5 | 通过: 两个模板与 3 条指针 |
| S6 | 通过: 链接脚本, 负例, `make test`, CI workflow |
| S7 | 通过: 全线闸门与记录 |

## 偏差与原因

- S1: 归档文件在 Git 树中显示为新增, `plans/PLAN.md` 显示为修改, 不是纯 rename. 原因: 归档后立即在固定槽位写入新计划. 影响: 归档正文完整, 只多两行头部; 不阻塞验收.
- S3: 在 S3 当时 `grep -c "/work"` 为 1; 收线时为 2, 因为 S6 新增的 `.github/workflows/docs.yml` 含 `workflows` 子串. 实际容器 `/work` 陷阱仍只有一处, 不是重复. 影响: 仅影响 grep 计数, 不影响验收.
- S7: S6 记录里的示例 markdown 链接被 `test/docs-links.sh` 当成真实链接, 首次全线检查失败. 原因: 记录文本使用了真实链接语法. 处理: 改成纯文本描述后通过. 影响: 闸门按预期工作, 不阻塞后续步骤.
- 无范围变更; 未修改 `PLAN.md`.

## 后续影响

- `AGENTS.md` 现在 82 行, 描述性事实由 `docs/` 独占, 计划与记录格式由 `plans/PLAN-TEMPLATE.md` 与 `plans/RESULT-TEMPLATE.md` 承载.
- `test/docs-links.sh` 已进入 `make test` 与 `.github/workflows/docs.yml`; 以后改 `docs/` 章节号或移动文件时, 本地与 CI 会一起报错.
- 模板文件是长期设施; 新建计划与记录时复制, 不再把格式正文写回 `AGENTS.md`.
- 本线没有遗留的产品代码改动; 不需要用户再拍板的事项.

## 闸门实测

### S1

- `uname -s` / `uname -m`: `Linux` / `x86_64`.
- `make test-all`: 退出 0, 末尾为 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 基线快照: `1fb97d5` 临时 worktree 生成 41 份 `/tmp/libcp-snapshot-baseline/*.s`.
- 当前树快照: 41 份 `/tmp/libcp-snapshot-current/*.s`; `diff -ru /tmp/libcp-snapshot-baseline /tmp/libcp-snapshot-current` 无输出, 输出 `snapshot diff: empty`.
- `git diff --check`: 通过.

### S2

- `wc -l AGENTS.md`: 154.
- `grep -n "docs/01-overview.md\|docs/02-architecture.md" AGENTS.md`: 命中 `docs/01-overview.md` 1.5, `docs/02-architecture.md` 2.2 与 `docs/02-architecture.md` 2.10.
- `grep -n "hashmap.c\|range.c" AGENTS.md`: 无命中.
- `grep -n "driver.h" AGENTS.md`: 命中开头根目录描述.
- `make test-all`: 退出 0.
- 快照: `diff -ru /tmp/libcp-snapshot-baseline /tmp/libcp-snapshot-current` 无输出, 输出 `snapshot diff: empty`.

### S3

- `wc -l AGENTS.md`: 127.
- `grep -c "Rosetta" AGENTS.md`: 1; `grep -c "/work" AGENTS.md`: 1.
- 命令存在性: `make test-all` 2 行, `make docker-test` 3 行, `make docker-snapshot-diff` 2 行, `make test-thirdparty` 1 行, `make bench` 2 行, `make format` 1 行.
- `grep -n "test/driver.sh" AGENTS.md`: 无命中.
- `make test-all`: 退出 0.
- 快照: `snapshot diff: empty`.

### S4

- `wc -l AGENTS.md`: 128.
- `grep -c "只承诺源码兼容" AGENTS.md`: 1.
- 九个决策词: `libcp`, `LibcpNode`, `new` / `free` / `reset`, `宏保真`, `CST 只建在预处理之前`, `per-node user data`, `c2c 只是`, `LIBCP_DEFAULT_ALLOCATOR`, `只承诺源码兼容` 全部命中.
- `grep -n "10 个公共头\|\.so 只导出 D7" AGENTS.md`: 无命中.
- `make test-all`: 退出 0.
- 快照: `snapshot diff: empty`.

### S5

- `wc -l AGENTS.md`: 81; `plans/PLAN-TEMPLATE.md`: 75; `plans/RESULT-TEMPLATE.md`: 40.
- `test -f` 两个模板: 通过.
- `grep -n "PLAN-TEMPLATE\|RESULT-TEMPLATE" AGENTS.md`: 命中第 29 行.
- `grep -n "每步模板" AGENTS.md`: 无命中.
- 模板字段: PLAN 的六个步骤字段与 RESULT 的六个记录字段全部命中.
- `make test-all`: 退出 0.
- 快照: `snapshot diff: empty`.

### S6

- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- 负例: 临时目录含 `docs/missing.md` 与一个指向 `missing.md` 的 markdown 链接, 脚本输出 3 条缺失并退出 1.
- `make test-all` 日志: 第 87 行 `test/docs-links.sh`, 第 88 行 `docs-links: ok (48 references checked)`; 退出 0.
- `grep -n "test/docs-links.sh"`: `Makefile:151`, `docs/10-build-install.md:211`, `AGENTS.md:81`.
- `python3 -c 'import yaml; yaml.safe_load(...)'`: `yaml: ok`.
- 快照: `snapshot diff: empty`.

### S7

- `make format-check`: 退出 0 (本机 clang-format 22.1.8; CI 固定 20).
- `make check-context`: 退出 0; accessors 0, TLS 0, `cg_ctx` 0, accessor symbols 0.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 日志第 87-88 行 `test/docs-links.sh` 与 `docs-links: ok (48 references checked)`; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --stat 1fb97d5..HEAD`: 11 个文件, 只含 `AGENTS.md`, `plans/`, `test/docs-links.sh`, `.github/workflows/docs.yml`, `Makefile` 与 `docs/10-build-install.md`.
- `wc -l AGENTS.md`: 82.
- 九个决策 grep: 9 个全部命中.
- `git log --oneline 1fb97d5..HEAD`: S1-S6 六个提交; S7 为本记录所在提交.
