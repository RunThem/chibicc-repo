# PLAN.md - AGENTS.md 与 docs 去重执行计划

状态: 待批准 (2026-11-21, 第二次提交审批).

目标: 让 `AGENTS.md` 只保留 agent 专用的规范与流程, 把与 `docs/` 重复的参考性内容改为一句结论加章节链接, 把计划与记录格式写成模板, 增加文档链接 CI 闸门, 并修掉已经出现的漂移.

## 1. 目标与边界

- 一句话目标: 在不丢失 agent 所需操作信息的前提下, 把 `AGENTS.md` 从 176 行压到约 75-100 行, 参考性事实由 `docs/` 独占, 格式要求由 `plans/*-TEMPLATE.md` 承载.
- 允许改动: `AGENTS.md`; `plans/PLAN.md`; `plans/RESULT.md`; 归档 `plans/PLAN-node-families.md` 与 `plans/RESULT-node-families.md`; 新增 `plans/PLAN-TEMPLATE.md` 与 `plans/RESULT-TEMPLATE.md`; 新增 `test/docs-links.sh`; 新增 `.github/workflows/docs.yml`; `Makefile` 的 `test` 目标; `docs/10-build-install.md` 10.8 的一行测试矩阵; 生成到 `/tmp` 的汇编快照.
- 非目标: 不改 `docs/` 正文与结构, 除 S6 在 `docs/10-build-install.md` 10.8 增加一行; 不检查外部 URL; 不引入第三方链接检查器; 不改任何 C 源码, 公共头与测试程序; 不改变构建, 测试或发射行为, 除 `make test` 增加文档链接检查; 不把文档链接检查加入 `test-stage2`; 不在本线做其他模板化.
- 允许的文档链接目标: `docs/01-overview.md` 1.5, `docs/02-architecture.md` 2.2, `docs/02-architecture.md` 2.10, `docs/10-build-install.md` 10.8, `docs/10-build-install.md` 10.10, `docs/10-build-install.md` 10.12, `plans/PLAN-TEMPLATE.md`, `plans/RESULT-TEMPLATE.md`.

## 2. 用户拍板 (2026-11-21)

- 用户在看到上一轮冗余分析后要求"开始吧", 即执行 `AGENTS.md` 去重.
- 已确认方向: `AGENTS.md` 与 `docs/` 职责分开; 保留规范与流程; 压缩或链接参考性内容; 不把 `AGENTS.md` 并入 `docs/`.
- 未决项"是否把 53 行的 `计划与记录格式` 移到 `plans/` 模板"拍板为"写成模板": 迁到 `plans/PLAN-TEMPLATE.md` 与 `plans/RESULT-TEMPLATE.md`, `AGENTS.md` 只留指针.
- 未决项"是否在 CI 增加文档链接检查"拍板为"增加": 新增 `test/docs-links.sh`, 加入 `make test`, 并由 `.github/workflows/docs.yml` 运行.
- 待批准: 本计划的步骤, 行数目标, 模板文件名与 CI 工作流形式.
- 未决项: 无.

## 3. 现状锚点

- 开工 HEAD: `1fb97d5` (`Require reviewed plans, stepwise mainline commits, and host-aware tests`).
- 宿主: `Linux` / `x86_64`, 原生路径; 实测 `uname -s` 为 `Linux`, `uname -m` 为 `x86_64`.
- `AGENTS.md`: 176 行, 17,648 字节, 12 个二级节.
- 当前 `plans/PLAN.md` 与 `plans/RESULT.md` 是已完成的 LibcpNode 字段族拆分线, A1-A6 全部勾选.
- 当前 CI: `.github/workflows/test.yml` 运行 `make test-all`, `make check-library` 与 `make bench-tool`; `.github/workflows/format.yml` 运行 `make format-check`. 没有文档链接检查.
- `test-stage2` 直接列出测试脚本, 不调用 `make test`; 因此文档链接检查只进 `make test`, 不进 `test-stage2`.
- 已实测漂移:
  - `AGENTS.md` 的代码地图没有 `libcp/src/range.c`; `docs/02-architecture.md` 2.10 有.
  - `AGENTS.md` 开头写"根目录只保留驱动器 `main.c` 与 `codegen.c`", 实际根目录还有 `driver.h`; 同一文件的代码地图也列了 `driver.h`.
  - `AGENTS.md` 把 `test/cst-stats.sh` 写在 `test/self-test.sh` 之后; `Makefile` 的 `test` 目标实际在 `test/cst.sh` 之后立即运行它.
- 已实测链接基线: 用一次性 bash 循环检查 `AGENTS.md`, `README.md`, `docs/*.md`, `plans/PLAN.md`, `plans/RESULT.md` 的本地 markdown 链接, 当前缺失目标为 0. 新脚本可以对当前树直接通过.
- 估计重复规模 (未逐字计数): 5 个参考性小节约 45-55 行可以由链接替代; 文件内部重复约 10 行; `计划与记录格式` 另有 53 行可以迁到模板.
- 基线闸门: 未在计划阶段重跑. `plans/RESULT-node-families.md` 的 A6 记录 `make test-all`, `make check-context`, `make check-library` 与 41 份本机汇编快照全绿; 本线 S1 会重新实测并写入新 `RESULT.md`.

## 4. 设计决定

### D1 - docs 独占描述性事实, AGENTS 独占规范与流程

- 选择: 架构解释, 文件地图, 目标列表, 测试矩阵, 环境细节放 `docs/`; `AGENTS.md` 只留判断标准与操作约束, 细节用"文件加章节号"链接.
- 理由: 两份内容的读者与生命周期不同; 重复已经导致 `range.c` 缺失, `driver.h` 矛盾和测试顺序错误.
- 影响: `AGENTS.md` 变短; agent 需要细节时按链接读 `docs/`.

### D2 - 保留 AGENTS 的操作自足性

- 选择: 精确命令 (`make test-all`, `make docker-test`, `make docker-snapshot-diff`, `make test-thirdparty`, `make bench`, `make format`), 三条闸门, 硬性规则, 测试路径判定保留在 `AGENTS.md`.
- 理由: agent 可能不点链接; 操作命令和 must 级约束不能外移.
- 影响: 压缩有下限, 模板迁出后目标 75-100 行, 不追求更短.

### D3 - 链接用文件加章节号, 不用 URL 锚点

- 选择: 写成 `docs/10-build-install.md` 10.8, 不写 `#108-测试矩阵`.
- 理由: 中文标题的 URL 锚点脆弱; 章节号稳定且可读.
- 影响: 少数重编号需要人工同步, 由 D7 的脚本兜底.

### D4 - 计划与记录格式模板化

- 选择: 把 `AGENTS.md` 的 `计划与记录格式` 迁到 `plans/PLAN-TEMPLATE.md` 与 `plans/RESULT-TEMPLATE.md`; `AGENTS.md` 只留固定槽位, 复制模板, 批准流程三句.
- 理由: 用户拍板"写成模板"; 模板可以直接复制, 同时减少每次 session 自动加载的 53 行.
- 影响: 新建计划与记录时复制模板; 模板中的章节是必含项; 本线最终行数目标改为 75-100.

### D5 - 归档已完成的 LibcpNode 线

- 选择: `plans/PLAN.md` 与 `plans/RESULT.md` 改名为 `plans/PLAN-node-families.md` 与 `plans/RESULT-node-families.md`, 文件头加归档说明; 新线占用固定槽位.
- 理由: A1-A6 全部完成, 固定槽位需要给新线; 仓库既有归档格式可直接沿用.
- 影响: 历史保留; 新 `RESULT.md` 从零记录本线.

### D6 - 文档改动的等价性验证

- 选择: 每个步骤都跑 `make test-all` 和本机汇编快照 diff; S1 在开工 commit 上用临时 worktree 生成 41 份基线, 后续步骤重建并逐字节比较.
- 理由: 仓库要求每次改动过三道闸门; 纯 markdown 改动虽然不可能改变发射, 仍按同一标准验证.
- 影响: 每步多花数分钟; 基线只生成一次, 后续复用.

### D7 - 文档链接检查进 CI

- 选择: 新增 `test/docs-links.sh`, 检查 `AGENTS.md`, `README.md`, `docs/*.md`, `plans/PLAN.md` 与 `plans/RESULT.md` 的本地 markdown 链接, 以及 `AGENTS.md` 中的 `docs/` 文件与章节引用; 加入 `make test`; 新增 `.github/workflows/docs.yml`, 在 push main 与 pull request 时运行.
- 理由: 用户拍板"增加"; 新的 `AGENTS.md` 用文件加章节号链接, 需要闸门防止失效.
- 影响: `make test-all` 多一个快速检查; CI 多一个独立 job; 不检查外部 URL, 不扫归档计划, 避免历史噪音.

### D8 - 链接检查的范围与写法

- 选择: `AGENTS.md` 中的 docs 章节引用重复写文件名, 例如 `docs/02-architecture.md` 2.2 与 `docs/02-architecture.md` 2.10, 让脚本逐项验证; 归档计划与模板不进入扫描.
- 理由: 控制误报; 明确可验证的引用形式; 避免中文章节锚点.
- 影响: `AGENTS.md` 的链接写法略有重复, 但可被脚本逐项检查.

## 5. 步骤

### S1 - 归档旧线并建立本线与基线

- 改什么: `git mv plans/PLAN.md plans/PLAN-node-families.md`, `git mv plans/RESULT.md plans/RESULT-node-families.md`; 两个归档文件加归档说明; 新建本线 `plans/PLAN.md` 与 `plans/RESULT.md`; 在 `1fb97d5` 的临时 worktree 生成 `/tmp/libcp-snapshot-baseline` 的 41 份 `.s`.
- 为什么: 旧线已完成; 新线需要固定槽位; 后续步骤需要开工基线.
- 目的与效果: `plans/PLAN.md` 与 `plans/RESULT.md` 指向本线; 旧线内容完整保留; 基线快照可复用于每步 diff.
- 如何测试: `test -f plans/PLAN-node-families.md && test -f plans/RESULT-node-families.md`; `head -2` 检查归档说明; `git diff --check`; `make test-all`; 生成基线并在当前树重建快照, `diff -ru` 为空.
- 验收标准: 归档文件除头部外与旧内容一致; 新 PLAN 与 RESULT 存在且标题正确; `make test-all` 全绿; 基线快照 41 个文件, 与当前树快照 diff 为空; 本步提交只含 `plans/`.
- 提交边界: 本步一个提交.

### S2 - 架构, 设计原则与代码地图改为链接

- 改什么: 压缩 `目标架构与库边界`, `设计原则`, `代码地图` 三节; 开头根目录描述补 `driver.h`; 删除逐文件代码地图与五层解释, 保留五层名称, "codegen 不属于库" 与文档链接; 链接按 D8 写成 `docs/01-overview.md` 1.5, `docs/02-architecture.md` 2.2 与 `docs/02-architecture.md` 2.10.
- 为什么: `docs/01-overview.md` 1.5 与 `docs/02-architecture.md` 2.2 与 2.10 已经拥有这些内容; 现有拷贝已经漂移, 漏掉了 `range.c`.
- 目的与效果: `AGENTS.md` 不再维护第二份代码地图和设计原则; 根目录描述与 `docs/01-overview.md` 1.9 一致.
- 如何测试: `grep -n "docs/01-overview.md"` 与 `grep -n "docs/02-architecture.md"` 命中; `! grep -n "hashmap.c"` 与 `! grep -n "range.c"`; `grep -n "driver.h"` 命中开头描述; `wc -l AGENTS.md` 小于等于 160; `make test-all`; 快照 diff.
- 验收标准: 三节合计不超过 8 行; 三个 `docs/` 引用目标存在; 代码地图与逐层解释不再出现; 根目录描述包含 `main.c`, `driver.h`, `codegen.c`; 行数小于等于 160; 闸门全绿.
- 提交边界: 本步一个提交.

### S3 - 测试路径, 构建测试与平台陷阱合并

- 改什么: 把 `测试路径判定`, `构建与测试`, `平台陷阱与测试环境` 三节合并成一节 `测试路径与构建`; 保留宿主判定, 原生与 Docker 选择, 精确命令, 三条闸门, `make bench` 仅原生的限制, Rosetta 下 tcc 的三个用例与容器 `/work` 可写层这两条 docs 未覆盖的操作陷阱; 删除逐目标说明, 测试矩阵, 重复的 Docker 目标列表与重复的宿主判定; 细节链接到 `docs/10-build-install.md` 10.8, `docs/10-build-install.md` 10.10 与 `docs/10-build-install.md` 10.12.
- 为什么: 三节覆盖同一主题, 相互重复, 且与 `docs/10` 重复; 测试顺序已经写错.
- 目的与效果: 测试路径只有一处描述; 不再复述 `make test` 的内部顺序; 精确命令仍可直接复制执行; docs 没有的 Rosetta 与 `/work` 陷阱仍保留.
- 如何测试: `grep -c "Rosetta"` 等于 1; `grep -c "/work"` 等于 1; `grep -n` 对 `make test-all`, `make docker-test`, `make docker-snapshot-diff`, `make test-thirdparty`, `make bench`, `make format` 全部命中; `wc -l AGENTS.md` 小于等于 140; `make test-all`; 快照 diff.
- 验收标准: 只剩一个测试路径小节; 六个命令全部保留; `make bench` 的宿主限制保留; Rosetta 与 `/work` 各只出现一次; 不再出现 `test/driver.sh` 到 `test/c2c.sh` 的完整顺序表; 行数小于等于 140; 闸门全绿.
- 提交边界: 本步一个提交.

### S4 - 已定边界去重与单一事实来源规则

- 改什么: 在文件开头或 `全局约定` 增加一段 single-owner 规则; 精简 `已定边界`, 删除与开头重复的命名与 ABI 句子, 删除纯 `docs/` 事实 (公共头数量, `.a` 与 `.so` 导出细节); 保留命名, 公共 IR, context, 宏保真, CST 时机, 无 per-node user data, c2c 范围, 分配器开关, 源码兼容等决策.
- 为什么: `已定边界` 是 agent 的决策护栏, 但当前混入了 `docs/` 已经维护的描述; 命名与 ABI 在文件里出现两次.
- 目的与效果: 每条边界都是"不得擅自推翻的决定"; 文件开头明确单一事实来源; 重复句消失.
- 如何测试: single-owner 句子命中; `grep -c "只承诺源码兼容"` 等于 1; 关键决策词全部命中; `wc -l AGENTS.md` 小于等于 135; `make test-all`; 快照 diff.
- 验收标准: single-owner 规则存在且指向 `docs/`; 九个关键决策全部保留; 重复的命名与 ABI 句只剩一处; `已定边界` 不再包含公共头计数与 `.so` 导出细节; 行数小于等于 135; 闸门全绿.
- 提交边界: 本步一个提交.

### S5 - 计划与记录格式模板化

- 改什么: 新增 `plans/PLAN-TEMPLATE.md` 与 `plans/RESULT-TEMPLATE.md`, 内容来自当前 `AGENTS.md` 的 `计划与记录格式`, 改成可复制模板; `AGENTS.md` 的 53 行压缩为 4-6 行指针, 保留固定槽位, 归档命名, 复制模板与批准流程.
- 为什么: 用户拍板"写成模板"; 减少自动加载内容; 模板可复用.
- 目的与效果: 新建计划与记录直接复制模板; `AGENTS.md` 只留规则; 格式要求仍完整可查.
- 如何测试: `test -f plans/PLAN-TEMPLATE.md && test -f plans/RESULT-TEMPLATE.md`; `grep -n "PLAN-TEMPLATE" AGENTS.md` 与 `grep -n "RESULT-TEMPLATE" AGENTS.md` 命中; `! grep -n "每步模板" AGENTS.md`; `wc -l AGENTS.md` 小于等于 100; `make test-all`; 快照 diff.
- 验收标准: 两个模板存在, 含 PLAN 的 9 项必含内容, 每步 6 项模板, RESULT 的 8 项必含内容与每步记录模板; `AGENTS.md` 不再有完整格式正文; 固定槽位与批准流程仍在; 行数小于等于 100; 闸门全绿.
- 提交边界: 本步一个提交.

### S6 - 文档链接检查脚本与 CI

- 改什么: 新增 `test/docs-links.sh`; 加入 `Makefile` 的 `test` 目标; 新增 `.github/workflows/docs.yml`; `docs/10-build-install.md` 10.8 增加一行 `test/docs-links.sh`; `AGENTS.md` 的测试约定增加一句文档链接闸门.
- 为什么: 用户拍板"增加"; 防止 `docs/` 章节引用与本地链接失效.
- 目的与效果: 本地 `make test` 与 CI 都能在链接失效时失败; 脚本打印缺失目标与来源文件.
- 如何测试: `bash test/docs-links.sh` 通过; 在临时目录构造缺失文件与缺失章节, 脚本返回非零; `make test`; `make test-all`; 快照 diff; `git diff --check`.
- 验收标准: 脚本在当前树通过; 两个负例都被检出; `make test` 包含该脚本; `.github/workflows/docs.yml` 在 push main 与 pull request 触发; `docs/10-build-install.md` 10.8 有对应行; `AGENTS.md` 行数小于等于 100; 闸门全绿.
- 提交边界: 本步一个提交.

### S7 - 全线闸门与 RESULT 收尾

- 改什么: 更新 `plans/RESULT.md`, 补齐每步提交 hash, 测试命令与结果, 偏差, 验收对照与闸门实测; 若前六步有遗漏, 在本步修正并记录.
- 为什么: `AGENTS.md` 要求 `RESULT.md` 随每步更新, 收线时补齐验收结论与后续影响.
- 目的与效果: 本线的计划, 提交, 闸门与偏差可逐项追溯; 后续线知道哪些内容由 `docs/` 与模板独占.
- 如何测试: `make format-check`; `make test-all`; `make check-context`; `bash test/docs-links.sh`; 本机 41 份快照 diff; `git diff --check`; `git diff 1fb97d5..HEAD --stat` 只含允许的文件; 链接目标逐个 `test -f`.
- 验收标准: 全部闸门全绿; 最终 `AGENTS.md` 行数在 75-100; 每个 `docs/` 引用都有对应文件与章节; 提交历史与 PLAN 步骤一一对应; RESULT 的验收表逐条通过.
- 提交边界: 本步一个提交.

## 6. 闸门

### 每步闸门

- `make test-all` 全绿, 含自举.
- 本机 41 份 `test/*.c` 汇编快照与 `1fb97d5` 基线逐字节 diff 为空.
- 该步的针对性 grep, 行数, 链接与 `git diff --check` 全部通过.
- 本步提交只触及允许的文件; 不含 `.o`, `.exe`, `.cache` 与 `/tmp` 快照等产物.

### 全线闸门

- `make test-all` 全绿.
- 汇编快照 diff 为空.
- `make check-context` 全绿.
- `make format-check` 全绿.
- `bash test/docs-links.sh` 全绿.
- `git diff 1fb97d5..HEAD --stat` 只含 `AGENTS.md`, `plans/`, `test/docs-links.sh`, `.github/workflows/docs.yml`, `Makefile` 与 `docs/10-build-install.md`.
- 最终 `AGENTS.md` 为 75-100 行, 不含逐文件代码地图, 不含完整测试矩阵, 不含重复 Docker 目标列表, 不含完整计划与记录格式正文.
- `docs/01-overview.md` 1.5, `docs/02-architecture.md` 2.2, `docs/02-architecture.md` 2.10, `docs/10-build-install.md` 10.8, `docs/10-build-install.md` 10.10 与 `docs/10-build-install.md` 10.12 的引用全部可达.
- 九个关键决策仍在: `libcp` 命名, 公共 IR, context 三态, 宏保真, CST 在预处理之前, 无 per-node user data, c2c 仅回归闸门, 分配器四宏, 源码兼容.

## 7. 风险与回退

- 风险: 压缩过度, agent 失去必要的操作信息. 缓解: D2 保留精确命令与三条闸门; D4 保留模板指针; S2-S6 用 grep 验收; 失败时回退到上一步提交.
- 风险: `docs/` 章节号变化导致链接失效. 缓解: D3 用文件加章节号; D7 的脚本检查文件与章节; S7 逐个复核.
- 风险: 链接检查脚本误报. 缓解: 只扫 `AGENTS.md`, `README.md`, `docs/*.md`, `plans/PLAN.md`, `plans/RESULT.md`; 跳过外部 URL, 锚点, 归档计划与模板; S6 先跑当前树验证零误报.
- 风险: 模板与 `AGENTS.md` 再次双写. 缓解: D4 把格式正文只放模板; `AGENTS.md` 只留指针; S5 验收检查 `每步模板` 已从 `AGENTS.md` 消失.
- 风险: `make test-all` 或快照生成受环境污染. 缓解: 记录完整命令与输出; 失败时不提交, 留在本步修复; 不把失败写成通过.
- 风险: 归档旧线时误伤未提交改动. 缓解: 开工前 `git status` 只有 `.zcode/`; 只 `git mv` 两个已跟踪文件.
- 回退点: 每步一个提交; 整线可 `git reset --hard 1fb97d5`; 归档文件可单独恢复.

## 8. 执行流程

1. 计划写入 `plans/PLAN.md` 并展示全文, 停下等待明确批准.
2. 批准后按 S1-S7 顺序推进, 一步一提交; 每步先跑该步闸门, 通过后再提交.
3. 每步提交后立即更新 `plans/RESULT.md`, 记录实际改动, 命令, 结果与偏差.
4. 范围, 设计或验收标准变化时, 先更新 `plans/PLAN.md` 说明原因, 再次请求批准.
5. 收线时补齐 RESULT 的验收表, 偏差与后续影响; 不在本线顺手做其他模板化或额外 CI.
