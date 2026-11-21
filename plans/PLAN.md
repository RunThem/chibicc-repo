# PLAN.md - chibicc 详细 --help/-h 执行计划

状态: 已批准 (2026-11-22).

## 1. 目标与边界

- 一句话目标: `chibicc -h` / `chibicc --help` 输出按类别分组, 覆盖全部受支持选项及其参数的详细帮助; 同时修正 `-I` / `-idirafter` 的分离参数绑定, 让帮助描述与实际行为一致, 并由 `test/driver.sh` 在 stage1 / stage2 两条自举路径上锁定.
- 允许改动: `main.c`; `test/driver.sh`; `docs/02-architecture.md` 2.9; `plans/PLAN.md`; `plans/RESULT.md`; 归档 `plans/PLAN-docs-calibration.md` / `plans/RESULT-docs-calibration.md`; `/tmp` 下的汇编基线快照.
- 非目标: 不改 `libcp/`, 公共头, `codegen.c`, `Makefile`, `README.md` 与其他 docs; 不新增命令行选项 (不加 `-v` / `--version`, man page, shell completion); 不改版本号; 不改链接器组装逻辑; 不把选项表复制进 docs; 不修 `-l` / `-o` / `-x` 的既有连写形式.
- 允许的文档链接目标: `docs/02-architecture.md` 2.9. 本线只在该节增加一段说明, 不新增链接.

## 2. 用户拍板 (2026-11-22)

- 已确认: 用户要求 `--help/-h` 提供足够详细的帮助, 格式参考 tinycc, 列出所有支持的选项以及相关参数.
- 已确认: 用户选择在本线一并修正 `-I <dir>` 与 `-idirafter <dir>` 的分离参数形式, 作为独立的行为修正步骤 (S2).
- 未决项: 无.
- 待批准: 本计划的 4 个步骤, 范围边界, 每步闸门与全线验收标准.

## 3. 现状锚点

- 开工 HEAD: `d761c8d` (`Finish the docs calibration sweep`).
- 宿主: `Linux` / `x86_64`, 原生路径 (`uname -s` 为 `Linux`, `uname -m` 为 `x86_64`).
- 实测命令与数字:
  - `./chibicc --help`: 向 stderr 打印 `chibicc [ -o <path> ] <file>`, 退出 0.
  - `./chibicc -h`: 打印 `unknown argument: -h`, 退出 1.
  - `./chibicc -E -I <dir> file.c`: 头文件找不到; 连写形式 `-I<dir>` 成功. 分离形式的 `<dir>` 被当成输入文件.
  - `./chibicc -idirafter <dir> ...`: include 路径里加入的是选项名本身, `<dir>` 被当成输入文件; 既有 `test/driver.sh` 的 `-idirafter` 用例因搜索优先级恰好通过, 没有捕获该缺陷.
  - `make test-all`: 退出 0; 末尾为 `fmt lock: 48 files idempotent, lines and comments kept, 41 files -S equivalent` 与 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
  - `make format-check`: 退出 0 (`clang-format` 22.1.8, 满足 20+ 要求).
  - `make check-context`: 退出 0; accessors 0, thread-local 0, `cg_ctx` 0, accessor symbols 0.
  - `git status --porcelain`: 计划写作前工作树干净.
- 汇编基线: 在 `d761c8d` 的临时 worktree 生成 41 份 `/tmp/libcp-help-base-ElClCm/s/*.s`; 本线每步用它做逐字节 diff.
- 估计与假设: 帮助正文约 80-100 行; 本线不改库与 codegen, 快照 diff 应为空.
- 基线闸门: 上述 `make test-all`, `make format-check`, `make check-context`, 41 份快照.

## 4. 设计决定

### D1 - 帮助正文的位置与格式

- 选择: 帮助正文作为 `main.c` 内的 `static const char[]` 字符串, 版本行用 `LIBCP_VERSION` 插值; 采用 tinycc 式的分组两栏布局, 选项列固定宽度, 描述列左对齐; 正文使用中文与英文标点.
- 理由: 驱动器是参考消费者, 不是库; 帮助不是公共 API 的一部分, 不引入运行时注册表或新文件. 静态正文最易读, 也便于 `test/driver.sh` 按行锁定.
- 影响: 新增选项时必须同步正文; `main.c` 中的注释与 `test/driver.sh` 的选项清单共同提醒.

### D2 - 触发, 输出流与退出码

- 选择: 在参数校验之前扫描 `argv`; 命中 `-h` 或 `--help` 时向 stdout 打印完整帮助并 `exit(0)`, 其他参数被忽略; 缺少参数等用法错误仍向 stderr 打印简短用法与 `Try 'chibicc --help' for more information.` 并 `exit(1)`.
- 理由: 与 GNU / tcc 习惯一致; 帮助是正常输出, 不是诊断; `chibicc -h -o` 这类不完整组合也应先给出帮助.
- 影响: 替换现有 `usage(0)` 到 stderr 的行为; `test/driver.sh` 的既有 `--help` 用例改用 stdout 断言.

### D3 - 覆盖面与分组

- 选择: 帮助按功能分组, 覆盖 `parse_args` 接受的全部选项, 包含兼容性忽略组与内部 `-cc1*` 组; 同时说明输入文件类型, `-` 标准输入, 各模式默认输出与退出码.
- 理由: 用户要求 "所有支持的选项以及相关参数"; 忽略组与内部组不列会留下未知项, 但必须明确标注 "无效果" 与 "内部使用".
- 影响: 选项清单即用户可见事实; docs 不再复制第二份表.

帮助分组与覆盖范围:

| 分组 | 选项 |
|---|---|
| 用法与输入 | `file...`, `.c` / `.s` / `.o` / `.a` / `.so`, `-` |
| 输出与模式 | `-c`, `-S`, `-E`, `-o <path>`, `-x <lang>`, `-###`, `-h`, `--help` |
| 预处理 | `-I <dir>`, `-idirafter <dir>`, `-D <name>[=<value>]`, `-U <name>`, `-include <file>` |
| 依赖 | `-M`, `-MD`, `-MMD`, `-MF <file>`, `-MT <target>`, `-MQ <target>`, `-MP` |
| 链接 | `-l<lib>`, `-L <dir>`, `-Wl,<arg>[,<arg>]`, `-Xlinker <arg>`, `-s`, `-static`, `-shared` |
| 代码生成 | `-fcommon`, `-fno-common`, `-fpic`, `-fPIC` |
| 兼容性 (接受但忽略) | `-O<n>`, `-W<name>`, `-g<n>`, `-std=<std>`, `-ffreestanding`, `-fno-builtin`, `-fno-omit-frame-pointer`, `-fno-stack-protector`, `-fno-strict-aliasing`, `-m64`, `-mno-red-zone`, `-w` |
| 只读通道 | `-dump-tokens`, `-reconstruct`, `-dump-cst`, `-reconstruct-cst`, `-verify-cst`, `-dump-expansions`, `-verify-expansions`, `-dump-ast` |
| 内部选项 | `-cc1`, `-cc1-input <file>`, `-cc1-output <file>` |
| 结尾说明 | 成功 / 失败退出码 |

### D4 - `-I` 与 `-idirafter` 的行为修正

- 选择: 在 `parse_args` 中为 `-I` 增加精确匹配分支, 消费下一个参数; 把 `-idirafter` 的 `argv[i++]` 改为 `argv[++i]`. 该修正单独提交, 先于帮助实现.
- 理由: 当前两个选项都会把参数留给输入文件列表, 帮助无法如实描述; 这是选项绑定缺陷, 不是新特性.
- 影响: `chibicc -I <dir>` 与 `chibicc -idirafter <dir>` 开始按预期工作; 既有连写形式与搜索优先级不变; 行为变化单独提交, 便于回退.

### D5 - 测试落点

- 选择: 扩展 `test/driver.sh`, 不新增脚本, 不改 `Makefile`. 帮助用例覆盖输出流, 退出码, `-h` 与 `--help` 逐字节相同, 全部选项出现, 帮助优先于其他参数, 缺少参数时的提示; 选项修正用例覆盖两种分离写法.
- 理由: `test/driver.sh` 是命令行选项的行为锁, 且 `make test` 与 `make test-stage2` 都会运行它, 自动获得自举覆盖.
- 影响: 不新增测试矩阵条目; `docs/10-build-install.md` 10.8 的 `test/driver.sh` 行仍然准确.

### D6 - docs 的最小更新

- 选择: 只在 `docs/02-architecture.md` 2.9 增加一段: 驱动器提供 `-h` / `--help`, 输出全部选项, 选项清单以该输出与 `test/driver.sh` 为准; 不复制分组表.
- 理由: 遵守单一事实来源; 帮助正文是用户可见的权威清单, docs 只做导航.
- 影响: 后续新增选项只需改帮助与测试, 不必回头维护 docs 表.

## 5. 步骤

### S1 - 归档 docs 校准线并建立本线计划与记录

- 改什么: 从 `HEAD` 取出现行 `plans/PLAN.md` 与 `plans/RESULT.md`, 写入 `plans/PLAN-docs-calibration.md` 与 `plans/RESULT-docs-calibration.md`, 文件头各加归档说明; 把本文件写入 `plans/PLAN.md` 并把状态改为已批准; 从模板建立本线 `plans/RESULT.md`, 记录 S1.
- 为什么: 当前 `PLAN.md` / `RESULT.md` 属于已收官的 docs 校准线; AGENTS 要求已完成线归档, 现行线固定使用 `PLAN.md` / `RESULT.md`.
- 目的与效果: `plans/` 中只有一对现行计划与记录, docs 校准线以归档形式保留终态.
- 如何测试: `test/docs-links.sh`; `ls plans/PLAN*.md plans/RESULT*.md`; 检查两个归档文件头.
- 验收标准: `docs-links: ok`; 归档文件含归档说明与终态步骤; 新 `PLAN.md` 状态为已批准; 工作树在该步提交后干净.
- 提交边界: 本步一个提交, 提交信息 `Archive the docs calibration line and add the help plan`.

### S2 - 修正 `-I` / `-idirafter` 的分离参数形式

- 改什么: `main.c` 增加 `-I` 精确匹配并消费下一个参数, 保留 `-I<dir>` 前缀分支; `-idirafter` 改为 `argv[++i]`; `test/driver.sh` 新增分离写法用例.
- 为什么: 两个选项当前都会把参数当作输入文件, `-idirafter` 连搜索路径都是选项名; 帮助若按预期语义描述就是假话.
- 目的与效果: `chibicc -E -I <dir> file.c` 能找到头文件; `chibicc -E -idirafter <dir> file.c` 能通过位于 `<dir>` 的头文件找到内容, 且 `<dir>` 不再进入输入列表.
- 如何测试: `test/driver.sh ./chibicc`; `make test-all`; 用当前 `chibicc` 重新生成 41 份汇编与 `/tmp/libcp-help-base-ElClCm/s` 逐字节 diff; `make format` 与 `make format-check`.
- 验收标准: 新增用例在 stage1 与 stage2 均通过; 既有 `-idirafter` 优先级用例仍通过; 快照 diff 为空; `format-check` 通过.
- 提交边界: 本步一个提交, 提交信息 `Fix the separated arguments of -I and -idirafter`.

### S3 - 实现详细 `-h` / `--help`, 并补测试与 docs 导航

- 改什么: `main.c` 增加帮助正文, 版本头, `-h` / `--help` 预扫描与 stdout 输出; 简短用法改为 `Usage: chibicc [options] file...` 并加 `Try 'chibicc --help' ...`; `test/driver.sh` 替换既有 `--help` 用例, 增加输出流, 退出码, 两种写法一致, 全部选项出现, 帮助优先, 缺少参数提示等用例; `docs/02-architecture.md` 2.9 增加一段导航说明.
- 为什么: 现有帮助只有一行, `-h` 直接报未知参数; 用户要求足够详细且覆盖全部选项.
- 目的与效果: 帮助按 D3 的九组输出, 每条列出选项与参数, 默认输出, 输入类型与退出码; stdout 是完整帮助, stderr 为空, 退出 0.
- 如何测试: `./chibicc --help > /tmp/help.txt 2>/tmp/help.err`; `./chibicc -h | diff - /tmp/help.txt`; `test/driver.sh ./chibicc`; `make test-all`; 快照 diff; `make format`; `make format-check`; `make check-context`; `test/docs-links.sh`.
- 验收标准: `-h` 与 `--help` 输出逐字节相同且 stderr 为空, 退出 0; 帮助含 D3 表中每个选项 (由 `test/driver.sh` 的清单锁定); `chibicc -h -o <out> <file>` 不产生输出文件; `chibicc -o` 退出非 0 且 stderr 含用法与 `Try 'chibicc --help'`; stage1 与 stage2 用例通过; 快照 diff 为空; `format-check` 与 `check-context` 通过; `docs-links` 通过.
- 提交边界: 本步一个提交, 提交信息 `Add a detailed --help/-h option summary`.

### S4 - 收线记录与全线闸门

- 改什么: 补齐 `plans/RESULT.md` 的提交一览, 各步记录, 验收表, 偏差与后续影响; 不改代码.
- 为什么: AGENTS 要求收线时补齐实际改动, 测试结果, 偏差与对后续计划的影响.
- 目的与效果: 本线的计划与执行记录可独立追溯.
- 如何测试: 重新运行全线闸门, 把命令与关键输出写入 RESULT.
- 验收标准: `make test-all`, `make format-check`, `make check-context`, `test/docs-links.sh` 全部退出 0; 41 份快照 diff 为空; 工作树干净.
- 提交边界: 本步一个提交, 提交信息 `Finish the help line`.

## 6. 闸门

### 每步闸门

- S1: `test/docs-links.sh`; 归档文件与现行文件布局检查; `git status`.
- S2: `make format`; `make format-check`; `test/driver.sh ./chibicc`; `make test-all`; 41 份快照 diff; 提交前确认新用例在 stage1 / stage2 都跑过.
- S3: 同上, 加 `make check-context`; 帮助输出流与选项清单的专项检查; `test/docs-links.sh`.
- S4: `make test-all`; `make format-check`; `make check-context`; `test/docs-links.sh`; 快照 diff; `git status`.

### 全线闸门

- 全量测试含自举: `make test-all` 退出 0.
- 汇编等价性: 41 份 `test/*.c` 的 `-S` 输出与 `d761c8d` 基线逐字节 diff 为空.
- 针对性测试: `test/driver.sh` 的帮助与 `-I` / `-idirafter` 用例在 stage1 与 stage2 均通过.
- `make format-check` 与 `make check-context` 通过.
- `test/docs-links.sh` 通过; docs 只改 2.9 一段.
- 不新增库层 `exit()`, 不新增静态全局语义状态; 帮助字符串是只读常量.

## 7. 风险与回退

- 风险: 帮助与 `parse_args` 漂移. 缓解: `main.c` 帮助正文上方注明同步义务; `test/driver.sh` 的选项清单锁定全部选项; 新增选项的步骤必须同时改两处.
- 风险: `-I` 精确分支改变既有连写行为. 缓解: 分支顺序为先精确后前缀; 既有 `-I<dir>` 用例与快照 diff 覆盖.
- 风险: `-idirafter` 修正破坏既有优先级测试. 缓解: 保留原优先级用例, 新增只存在于 idirafter 目录的头文件用例.
- 风险: 帮助正文过宽或中文标点混入. 缓解: 选项列固定宽度, 描述列左对齐; 提交前全文检索中文标点; `format` 只影响 C 代码, 不改字符串内容.
- 风险: docs 复制选项表造成第二事实来源. 缓解: D6 只加导航段.
- 回退点: S3 可单独回退到 S2 提交; S2 可单独回退到 S1 提交; S1 归档提交独立, 回退不影响源码.

## 8. 执行流程

1. 计划写入 `plans/PLAN.md` 并展示全文, 停下等待明确批准.
2. 批准后按步骤推进, 一步一提交; 每步先跑该步闸门, 通过后再提交.
3. 每步提交后立即更新 `plans/RESULT.md`, 记录实际改动, 命令, 结果与偏差.
4. 范围, 设计或验收标准变化时, 先更新 `plans/PLAN.md` 说明原因, 再次请求批准.
5. 收线时补齐 RESULT 的验收表, 偏差与后续影响.

## 9. 计划书自检

- 每步写全六项: 改什么, 为什么, 目的与效果, 如何测试, 验收标准, 提交边界.
- 每步能独立提交并通过该步闸门; 行为修正 (S2) 与帮助实现 (S3) 分开提交.
- 没有把 "优化", "整理", "完善" 单独作为一步.
- 验收标准均可由命令或逐字节 diff 判定.
