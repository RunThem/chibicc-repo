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
| S2 | `a62e223` | Fix the separated arguments of -I and -idirafter |
| S3 | `a392aec` | Add a detailed --help/-h option summary |
| S4 | 本记录所在提交 | Finish the help line |

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

### S3 - 实现详细 `-h` / `--help`, 并补测试与 docs 导航

- 做了什么: `main.c` 增加 `help_text` 静态正文与 `print_help()`, 版本头取 `LIBCP_VERSION`; `parse_args()` 在校验参数之前扫描 `-h` / `--help`, 命中即向 stdout 打印并退出 0; `usage()` 改为 `Usage: chibicc [options] file...` 并在失败时加 `Try 'chibicc --help' for more information.`; `test/driver.sh` 用七个用例替换旧的一行检查, 并用 44 条模式锁定帮助中的全部选项; `docs/02-architecture.md` 2.9 增加一段导航说明.
- 测试: `./chibicc --help` 退出 0, stdout 80 行, stderr 0 字节; `./chibicc -h` 与 `--help` 逐字节相同; `test/driver.sh ./chibicc` 与 `make test-all` 中 stage1 / stage2 均输出 `--help lists every option ... passed`; `make test-all` 退出 0; 41 份汇编快照 diff 为空; `make format-check`, `make check-context`, `test/docs-links.sh` 通过.
- 偏差: 无. 帮助正文最初漏写 `Usage:` 两行, 由新增的 `--help prints usage` 用例当场捕获并补上; 这是实现过程中的修正, 不影响计划范围.
- 验收: 帮助覆盖 D3 表中的每个选项; 输出流, 退出码, 两种写法一致, 帮助优先, 缺少参数提示均通过; stage1 / stage2 用例通过; docs 导航段存在; 全线闸门通过.

## 验收

| 项目 | 结果 | 证据 |
|---|---|---|
| `-h` / `--help` 详细帮助 | 通过 | stdout 80 行, 含版本头, `Usage: chibicc`, 九组选项与退出码 |
| 输出流与退出码 | 通过 | 两种写法均退出 0, stdout 有内容, stderr 0 字节; `cmp` 逐字节相同 |
| 选项覆盖 | 通过 | `test/driver.sh` 的 44 条模式覆盖 `parse_args` 的全部选项, 含兼容性忽略组与内部 `-cc1*` 组 |
| 帮助优先 | 通过 | `chibicc -h -o <out> test/common` 退出 0 且不产生 `<out>` |
| 用法错误提示 | 通过 | `chibicc -o` 退出 1, stderr 含 `Usage: chibicc [options] file...` 与 `Try 'chibicc --help'` |
| `-I <dir>` 分离写法 | 通过 | stage1 与 stage2 的 `-I separated argument` 用例通过 |
| `-idirafter <dir>` 分离写法 | 通过 | stage1 与 stage2 的 `-idirafter separated argument` 用例通过; 既有优先级用例仍通过 |
| 全量测试含自举 | 通过 | `make test-all` 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编等价性 | 通过 | 41 份 `test/*.c` 的 `-S` 输出与 `d761c8d` 基线逐字节 diff 为空 |
| 格式与上下文审计 | 通过 | `make format-check` 退出 0; `make check-context` 输出 accessors 0, thread-local 0, `cg_ctx` 0, accessor symbols 0 |
| docs 链接 | 通过 | `docs-links: ok (48 references checked)` |
| 改动范围 | 通过 | 只改 `main.c`, `test/driver.sh`, `docs/02-architecture.md` 2.9 与 `plans/`; `libcp/`, 公共头, `codegen.c`, `Makefile` 未动 |

## 偏差与原因

- S1: 无偏差. 归档正文与 `HEAD` 版本逐行相同, 只改标题并加归档说明.
- S2: 计划只要求修正分离参数绑定; 实测 `-idirafter` 的目录排在 `-I` 之后但排在内建系统路径之前, 与 GCC 的 "after system directories" 不同. 本步没有改顺序, 帮助按实际行为描述; 该差异记录在后续影响.
- S3: 帮助正文最初漏写 `Usage:` 两行, 新增的 `--help prints usage` 用例当场失败并暴露问题; 补上后通过. 这是实现过程中的修正, 不改变范围与验收.
- S4: 无偏差. RESULT 初次统计把帮助行数写成 76, 选项模式写成 43; 实测为 80 与 44, 本步更正.
- 无范围变更; 没有修改 `PLAN.md` 的步骤或验收标准.

## 后续影响

- `-idirafter` 的顺序仍与 GCC 不同: 当前在 `-I` 之后, 内建系统路径之前. 若要对齐 GCC, 需要在 `add_default_include_paths()` 之后追加 idirafter 目录; 这是独立的行为决策, 本线不做.
- `-l<lib>` 仍只支持连写形式; 帮助没有宣称 `-l <lib>` 可用. 若要对齐 GCC 分离写法, 需要另立计划并补测试.
- 帮助正文是 `main.c` 的静态字符串, 新增或修改选项时必须同步 `help_text` 与 `test/driver.sh` 的模式清单; 后者会在遗漏时失败.
- `-h` / `--help` 的版本头取 `LIBCP_VERSION`; Makefile 的 `VERSION` 必须继续与公共头同步, 否则帮助头会显示库版本而不是驱动器版本.
- 兼容性选项仍然是接受但忽略, 尤其 `-w` 不会抑制警告; 帮助已明确写出.
- 内部 `-cc1` / `-cc1-input` / `-cc1-output` 已列入帮助, 但只供驱动器自调用; 帮助已标注不要直接使用.
- 41 份汇编基线固定在 `/tmp/libcp-help-base-ElClCm/s`; 该路径是临时目录, 新线需要按同样命令重新生成.
- 用户要求最后推送 `main`; 本线收线提交后执行 `git push origin main`.

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

### S3

- `./chibicc --help`: 退出 0; stdout 含版本头, `Usage: chibicc`, 九组选项与 `退出码`; stderr 0 字节.
- `./chibicc -h`: 退出 0; 与 `--help` 的 `cmp` 为空; stderr 0 字节.
- `./chibicc -o`: 退出 1; stderr 为 `Usage: chibicc [options] file...` 与 `Try 'chibicc --help' for more information.`.
- `./chibicc -h -o <out> test/common`: 退出 0, `<out>` 不存在.
- `grep -nP '[，。：；（）【】、《》“”‘’！？]' /tmp/help.txt`: 无命中.
- `test/driver.sh ./chibicc`: 输出 `OK`; 七个帮助用例全部 `passed`; 44 条选项模式全部命中.
- `make test-all`: 退出 0; stage1 与 stage2 各有一次 `testing --help lists every option ... passed` 与 `testing -idirafter separated argument ... passed`; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 汇编快照: 41 份 `.s`, 与 `/tmp/libcp-help-base-ElClCm/s` 的 `diff -ru` 为空.
- `make format-check`: 退出 0.
- `make check-context`: 退出 0; accessors 0, thread-local 0, `cg_ctx` 0, accessor symbols 0.
- `test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `git diff --check`: 通过.

### S4

- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 汇编快照: 41 份 `.s`, 与 `/tmp/libcp-help-base-ElClCm/s` 的 `diff -ru` 为空.
- `make format-check`: 退出 0.
- `make check-context`: 退出 0; accessors 0, thread-local 0, `cg_ctx` 0, accessor symbols 0.
- `test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `git diff --check`: 通过.
- 工作树: 本步只改 `plans/RESULT.md`.
