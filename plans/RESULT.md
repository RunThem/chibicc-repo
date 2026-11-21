# RESULT.md - chibicc 详细 --help/-h 执行记录

本文件记录 `PLAN.md` (chibicc 详细 --help/-h) 的实际执行结果. 本线开工 HEAD 为 `d761c8d` (`Finish the docs calibration sweep`), 宿主为 `Linux` / `x86_64` (原生路径), 日期 2026-11-22.

## 用户决定 (2026-11-22)

- 用户要求 `--help/-h` 提供足够详细的帮助, 格式参考 tinycc, 列出所有支持的选项以及相关参数.
- 用户选择一并修正 `-I <dir>` 与 `-idirafter <dir>` 的分离参数形式.
- 用户批准本计划的 4 个步骤, 范围边界, 每步闸门与全线验收标准; 批准后最后推送 `main`.
- 未决项: 无.

## 提交一览

| 步骤 | hash | subject |
|---|---|---|
| S1 | `a51ef7a` | Archive the docs calibration line and add the help plan |
| S2 | 本记录所在提交 | Fix the separated arguments of -I and -idirafter |
| S3 | 待执行 | Add a detailed --help/-h option summary |
| S4 | 待执行 | Finish the help line |

## 各步记录

### S1 - 归档 docs 校准线并建立本线计划与记录

- 做了什么: 从 `HEAD` 取出现行 `plans/PLAN.md` 与 `plans/RESULT.md`, 写入 `plans/PLAN-docs-calibration.md` 与 `plans/RESULT-docs-calibration.md`, 文件头各加归档说明; 把本线计划写入 `plans/PLAN.md` 并把状态改为已批准; 从模板建立本线 `plans/RESULT.md`.
- 测试: `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; 归档正文与 `HEAD` 版本除标题与归档说明四行外逐行相同; `plans/` 中 `PLAN-docs-calibration.md` 与 `RESULT-docs-calibration.md` 已就位; 工作树只含 `plans/` 的四个文件.
- 偏差: 无.
- 验收: 归档文件存在且带归档说明; 新 `PLAN.md` 状态为已批准; 本步提交只含 `plans/`.

### S2 - 修正 `-I` / `-idirafter` 的分离参数形式

- 做了什么: `main.c` 在 `-I<dir>` 前缀分支之前增加 `-I` 精确匹配, 消费下一个参数; `-idirafter` 的 `argv[i++]` 改为 `argv[++i]`; `test/driver.sh` 增加 `-I <dir>` 与 `-idirafter <dir>` 两个分离写法用例, 后者只把 `only-idirafter` 放在 idirafter 目录, 确保目录真正进入搜索路径而不是输入列表.
- 测试: `test/driver.sh ./chibicc` 输出 `OK`, 两个新用例均 `passed`; `make test-all` 退出 0; 41 份汇编快照与 `d761c8d` 基线 `diff -ru` 为空; `make format-check` 与 `git diff --check` 通过.
- 偏差: 无. 实测 `-idirafter` 的目录仍排在 `-I` 之后, 但排在内建系统路径之前, 与 GCC 的 "after system directories" 不同; 本步只修参数绑定, 不改顺序, 帮助将按实际顺序描述.
- 验收: 分离写法在 stage1 与 stage2 均通过; 既有 `-idirafter` 优先级用例仍通过; 快照 diff 为空; `format-check` 通过.

## 验收

- 待收线时补齐.

## 偏差与原因

- 待收线时补齐.

## 后续影响

- 待收线时补齐.

## 闸门实测

### S1

- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- 归档正文: `git show HEAD:plans/PLAN.md` 与 `PLAN-docs-calibration.md` 去掉标题与归档说明后 `diff` 为空; `RESULT-docs-calibration.md` 同样为空.
- 归档行数: `PLAN-docs-calibration.md` 337 行, `RESULT-docs-calibration.md` 522 行; 旧文件分别为 335 与 520 行, 均只多标题改写与归档说明两行.
- `git status --short`: 只含 `plans/PLAN.md`, `plans/RESULT.md`, `plans/PLAN-docs-calibration.md`, `plans/RESULT-docs-calibration.md`.
- 新 `PLAN.md` 状态: `已批准 (2026-11-22)`.

### S2

- `test/driver.sh ./chibicc`: 输出 `OK`; 新增 `testing -I separated argument ... passed` 与 `testing -idirafter separated argument ... passed`; 既有两个 `-idirafter` 用例仍通过.
- `-idirafter` 顺序探针: 在临时目录放伪造的 `stdio.h`, `chibicc -idirafter <dir> -E -xc -` 选中该目录, 证明它排在 `-I` 之后, 内建系统路径之前.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 汇编快照: 41 份 `.s`, 与 `/tmp/libcp-help-base-ElClCm/s` 的 `diff -ru` 为空.
- `make format-check`: 退出 0.
- `git diff --check`: 通过.
