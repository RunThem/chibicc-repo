# RESULT.md - trivia 保留执行记录

本文件记录 `PLAN.md`(trivia 保留线, 路线图阶段 1)各步骤的实际执行结果, 供审核与后续会话接续
参考. 前三条线已归档: 语法语义拆分见 `PLAN-split.md` / `RESULT-split.md`, 忠实层收尾见
`PLAN-faithful.md` / `RESULT-faithful.md`, codegen 直连见 `PLAN-codegen-direct.md` /
`RESULT-codegen-direct.md` - 后者记录的终态(5f2af8a)是本线的量化对比基线.

## 闸门口径(本线生效)

- 每步(均为行为不变步): `make docker-test`(含自举与诊断 55 例 + 形状 33 段) +
  `make docker-snapshot-diff` 为空(raw 逐字节) + `test/trivia.sh` 该步新增的断言.
- 计划内行为变化步(唯一候选: T2.2 注释内 \u 的显示): 单独提交, trivia.sh 同提交锁定新行为,
  偏差逐条记入本文件; 汇编快照仍要求为空.
- 本地 A/B 规程(出现无法归因的差异时): `git worktree add /tmp/cbase <上一个绿提交>`, 两份
  二进制对同一输入逐字节对照 .s 与 -E / dump 输出.

## 基线记录(T0.1, 复验于 e8c8608)

- 起始状态: HEAD = e8c8608(线间归档提交; 代码与其父 5f2af8a 相同, 只动文档), tokenize.c 805 行,
  chibicc.h 832 行, main.c 791 行, preprocess.c 1208 行.
- docker-test(`make docker-test`): EXIT=0. 40 个 test/*.c 特性测试在 stage1/stage2 两遍全过,
  driver.sh / diagnostic.sh(55 例逐字节, 阶段锁定行 3888/3988) / shape.sh(33 段, 3967/4023) /
  trivia.sh(7 样本, 3983/4032)在 ./chibicc 与 ./stage2/chibicc 上各跑一遍, 自举完成.
  首次复验(挂 trivia.sh 前)同为 EXIT=0.
- snapshot-diff(`make docker-snapshot-diff`): `snapshot diff: empty`(raw 逐字节, 41 文件,
  0 条 diff). 基线 .cache/snapshot 为 B2 终态提交内重置的快照, 本线预期全程不重置.
- tinycc(`make docker-test-thirdparty THIRDPARTY=tinycc`): EXIT=0, tests2 的 86 个用例跑完
  (最后一例 111_conversion; Rosetta shim 跳过 106_pthread/112_backtrace/113_btdll),
  日志 10 处 "failed as expected" 是 tcc 自身的预期失败记账.
- -E 锁定样本: 7 条进 test/trivia.sh 首段(丢注释 + 折空白, 续行拼接, 行注释吞续行, 补末行换行,
  丢 BOM, 空文件, 空行群). 脚本生成的输入与实测探针逐字节一致(7/7 cmp 相等); 对 ./chibicc 与
  A/B 基线 /tmp/cbase/chibicc 两个二进制均通过.
- A/B 规程: `git worktree add /tmp/cbase e8c8608` 已建并本机构建(该二进制可跑 -E 与后续 dump);
  前端输出以两二进制逐字节对照. .s 侧等价性由快照链保证: 基线的 .s = .cache/snapshot(B2 终态
  重置), HEAD 的 .s == .cache/snapshot(本次 diff 为空), 故 HEAD 与基线在 .s 上逐字节相等.
- 账本核对: 丢弃点清单表 9 行逐行核对无误 - 四个丢弃点在 tokenize() 501-508 / 510-518 /
  520-526 / 528-533, 五个前置换写在 read_file 670-672, tokenize_file 787-788,
  canonicalize_newline 691-707, remove_backslash_newline 710-734, convert_universal_chars
  747-776.

## trivia 丢弃点清单表(账本初版, T0.1 核对, 实施时逐项打勾)

| # | 站点 | 现状行为 | 归属 | 目标形态 | 步 |
|---|---|---|---|---|---|
| 1 | `tokenize()` 501-508 行注释 | 跳过, `has_space = true` | trivia | 前置切片(行注释) | T1.1 ✓ |
| 2 | `tokenize()` 510-518 块注释 | 跳过(未闭合在 514 走 `error_at`), `has_space = true` | trivia | 前置切片(块注释) | T1.1 ✓ |
| 3 | `tokenize()` 520-526 换行 | 跳过, `at_bol = true`, `has_space = false` | trivia | 前置切片(换行, 置位语义不动) | T1.1 ✓ |
| 4 | `tokenize()` 528-533 空白 | 跳过, `has_space = true` | trivia | 前置切片(空白串) | T1.1 ✓ |
| 5 | `read_file` 补末行 `\n` | 没有末行换行的文件被加一字节 | 扫描缓冲 | 只进扫描缓冲; raw 里不存在, 故重建不补 | T2.1 ✓ |
| 6 | `tokenize_file` BOM 跳过 | `p += 3`, `contents` 不含 | trivia | 首 token 的前置 trivia(raw 坐标), 重建含 BOM | T2.1 ✓ |
| 7 | `canonicalize_newline` 691-707 | `\r\n` / `\r` 就地改写为 `\n` | trivia + 语义 | 扫描缓冲保改写; raw 保 `\r` / `\r\n` 进 trivia, 行号用 lines 表 | T2.1 ✓ |
| 8 | `remove_backslash_newline` 710-734 | 就地删除 `\`+换行, 换行挪到下一个真实换行后 | trivia + 语义 | 扫描缓冲保删除; raw 保续行进 trivia/拼写; 行号按"挪换行"语义复现 | T2.1 ✓ |
| 9 | `convert_universal_chars` 747-776 | 全缓冲就地改写 `\uXXXX` / `\UXXXXXXXX` | 扫描缓冲 | 扫描缓冲保改写; raw 保原拼写(重建原样, -E 与显示不变) - T2.2 因此不再必要 | T2.1 ✓ |

行 1-4 的"四个跳过点只字未动"是 T1.1 的实现方式: 字节的认领落在 token 产生点(见下).
行 5-9 在 T2.1 的落点是 `make_scan_buffer`: 扫描缓冲的文字效果逐字节保留(行 5-9 的"现状行为"
一栏就是它的规范), 同时产出 `map` / `lines` 两张 raw 坐标的表; raw 文本原样留给重建与 dump.
计划里的"File 标记"字段没有采用 - BOM 与末行换行分别由前置 trivia 与 raw 缺省自然表达.

## T1.1 trivia 通道与 -dump-tokens(已提交)

- 改了什么:
  - `chibicc.h`: `Token` 增 `trivia` / `trivia_len` - 前置被跳过字节的切片, 判据 4 的表示
    (指向缓冲的 (start, len), 不复制文本; 种类不存, 由 dump 从字节再派生).
  - `tokenize.c`: `new_token` 增 `trivia` 参数并算 `trivia_len = start - trivia`; 10 个 token
    产生点(4 个字面量辅助 + pp-num/ident/punct/EOF)带上前置切片, 消费后把起点推进到 `p`;
    四个跳过点只字未动 - 字节从"丢弃"变为"被下一个 token 认领", `at_bol` / `has_space` 的
    置位语义逐字未变; EOF 收尾部 trivia; `tokenize_string_literal` 的新 token 继承源 token
    的切片(同 loc 同 buffer, 经 `*t = *` 回到流里).
  - `main.c`: 新旗标 `-dump-tokens`(仿 `-hashmap-test`, 只读): cc1 里对 base_file 的
    `tokenize` 直接输出后返回(判据 3: 预处理之前, 不跑预处理器), 驱动侧与 `-E` 同批透传
    且不汇编. 输出每 token 一行 `T <line> <offset> <kind> <text>`, 每条 trivia 一行
    `R <line> <offset> <kind> <text>`(lc/bc/nl/ws 从字节派生, 排在所属 token 之前);
    文本转义 \n \t \r \\ 与非可见字节 \xHH.
- 计划外的一个格式决定: 空格也转义为 \x20. 理由: 文本列只有一格, 空格原样输出则"空文本"与
  "一个空格"不可分辨, 且行尾空白会被编辑工具剥除, 破坏逐字节锁定的稳定性. 影响面仅
  -dump-tokens 这一新增只读输出, 无既有消费者.
- 闸门: `make docker-test` EXIT=0(自举完成; trivia.sh 17 例 = 7 -E + 10 dump 在 ./chibicc 与
  ./stage2/chibicc 上各过一遍, 日志 3999/4052 行; 诊断 55 例与形状 33 段照旧双跑全绿);
  `make docker-snapshot-diff` = `snapshot diff: empty`. 本地 A/B(/tmp/cbase 的 e8c8608 二进制
  vs 本步二进制): `-E` 46 个语料文件逐字节相同(2 个文件本地无系统头不可预处理 - atomic.c /
  tls.c, 两个二进制的失败 stderr 逐字节相同); `-S` 39 个 test/*.c 逐字节相同(同样跳过 2 个).
- 偏差: 无既有可观察行为变化; 唯一计划外项 = dump 的空格转义(见上).

## T1.2 重建与 round-trip(已提交)

- 改了什么: `main.c` 增只读旗标 `-reconstruct`(与 `-dump-tokens` 同一 cc1 早退口, 同一份
  `must_tokenize_file`)- 按流序输出每个 token 的前置切片与拼写(EOF 的尾部 trivia 随其切片
  输出, 即"重建输入文本"); 驱动侧同 `-E` 透传, 不汇编.
- 重建合同(判据 3): 只覆盖 `tokenize(file)` 的直接输出(预处理之前, 单文件). 全量断言覆盖
  `test/*.c` 与 `include/*.h`(44 个文件; `.h` 走 `-xc`, 否则驱动的扩展名分派会拒绝),
  例外四条与计划一致:
  - `test/literal.c`, `include/stdarg.h`, `include/stdatomic.h` - 反斜杠续行(T2.1 退役)
  - `test/unicode.c` - universal chars(T2.2 退役)
- 边界样本 6 条锁"当前已知偏差"(T2.1/T2.2 退役对应 pass 时改成等于输入): CRLF 折 \n;
  孤立 CR 折 \n; 补末行换行; 丢 BOM; 续行删除且换行数挪到下一个真实换行之后
  (`1 +\` NL `2\n` -> `1 +2\n\n`); `\u00e9` 就地改写为 UTF-8 两字节. 语料实测: 例外恰为
  上列四文件(全语料无 CR/BOM, 除锚点外无续行/\u), 无第五条.
- 闸门: `make docker-test` EXIT=0(自举完成; trivia.sh 67 例 = 7 -E + 10 dump + 44 文件
  round-trip + 6 边界样本, 在 ./chibicc 与 ./stage2/chibicc 上各过一遍, 日志 4076/4152 行)
  `make docker-snapshot-diff` = `snapshot diff: empty`. 本地 A/B: `-S` 39/39 逐字节一致
  (本步只加驱动旗标, 既有路径零改动).
- 偏差: 无既有可观察行为变化.

## T2.1 CRLF 与续行归 trivia: 设计变更与实施(已提交)

- **与计划的设计偏离(重要, 审核重点)**: 计划设想"让缓冲就是原文件 + 词法各扫描点把 `\`+换行当
  透明 + File 级事实字段". 实施时的分析表明该路线会波及 token 恒等与显示路径: token 的拼写
  (`loc`/`len`)一旦覆盖续行, `equal()` / `is_keyword`(哈希) / `read_punct` / `print_tokens`(-E)
  以及诊断的拼接行显示与插入符列宽都会失真 - 而这些路径由红线(parse.c 零改动, 诊断逐字节,
  -E 归一化)保护. 因此改用**双缓冲 + 偏移映射**:
  - `File` 增 `raw`(文件原样字节)与 `map`(scan 偏移 -> raw 偏移)与 `lines`(raw 偏移 -> 行号);
    `contents` 仍是今天的扫描缓冲 - **三个破坏性 pass 的文字效果逐字节保留**(canonicalize 的
    CRLF/CR 归一、remove 的删除、\u 改写、BOM 跳过、末行补 `\n`), 只是由 `make_scan_buffer`
    一遍完成并同时填两张表. 一切既有消费者(-E, 解析, sema, codegen, 诊断显示)看到的字节
    与今天完全相同.
  - token 的 trivia/拼写坐标换到 **raw**: `Token.trivia` / `trivia_len`(前置被跳过字节)与
    `raw_loc` / `raw_len`(拼写)都是 raw 坐标; `reconstruct` / `-dump-tokens` 因此按原始文本
    重建与展示. 计划里的"File 级事实字段"不需要了: BOM 成为首 token 的前置 trivia, 合成末行
    换行在 raw 里不存在.
  - 唯一需要复现的旧可观察量是**行号**: `lines` 表按被退役的"挪换行"语义填充(续行内的换行
    推迟到下一个真实换行(或文件尾)才计入), `add_line_numbers` 与 `error_at` 改用它; 扫描
    缓冲不变, 所以 `verror_at` 的拼接行显示与插入符列宽天然保持.
- 改了什么: `chibicc.h`(File 3 字段 + Token 2 字段 + `is_line_break`/`line_break_len` 导出);
  `tokenize.c`(求 raw 坐标的 `raw_off`/`raw_end`/`raw_end_of`, `make_scan_buffer` 取代三个
  pass, `read_file` 返回原样文本+长度, `new_file` 对合成缓冲建恒等表, `add_line_numbers` /
  `error_at` 用 lines 表; `tokenize_string_literal` 扫描时临时切到 `tok->file` 并恢复旧字段值,
  保持它既有的(有瑕疵的)file/filename 行为逐字节不变); `main.c`(dump 改 raw 坐标, trivia
  分片加 `cont` / `bom`, `nl` 覆盖 `\n` / `\r\n` / `\r`, R 行行号取 lines 表; `reconstruct`
  用 raw 拼写).
- 闸门: `make docker-test` EXIT=0(自举完成; trivia.sh 89 例 = 7 -E + 21 dump + 48 文件 round-trip
  + 10 边界样本 + 4 锚点, 在 ./chibicc 与 ./stage2/chibicc 上各过一遍, 日志 4112/4199 行;
  诊断 55 例与形状 33 段照旧双跑全绿); `make docker-snapshot-diff` = `snapshot diff: empty`
  (扫描缓冲的文字效果逐字节保留的直接证据). 本地: 全语料 48 文件 round-trip 零例外; A/B 对
  e8c8608 二进制 -E 46/46、-S 39/39 逐字节相同(macro.c 用快照的固定时间宏后亦相同); 诊断
  55 例与形状 33 段全过; 锚点(p2b/p2/p3 诊断, p2c 的 .loc)逐字节一致并进 trivia.sh 锁定.
- trivia.sh: 21 个 dump 样本(新增 CRLF / 孤立 CR / BOM / 七类扫描点的续行), 全量 round-trip
  48 文件(例外清单清零), 10 个边界样本全部转正(期望 == 输入), 4 个行号/插入符锚点.
- 计划内的行为变化: 无. T2.2(universal chars 保原拼写)随之**不再必要**: raw 坐标已让
  \u 原样重建, 扫描缓冲保留改写使得 -E 与诊断显示维持原样(用户若要 -E/CST 层面看到 \u 原
  拼写, 是后续线的事, 与第 1 层合同无关). 该开放决策已由用户路线图吸收, 见 PLAN.md 注记.

## 关键结论(开工前分析, 供审核者速览)

- 判据 1 的落点: 既有字段零改动; 新通道 = 每个 token 的前置切片 + `File` 级两个事实;
  `has_space` / `at_bol` 的全部语义点(preprocess.c 77/83/86, 233, 374, 469/479, 610/611,
  652/653, 680/681 与 main.c 438-440)不动.
- 判据 3 的落点: 重建合同只覆盖预处理前; `paste` / `stringize` / 宏展开产物 /
  `tokenize_string_literal` 的重扫都不承诺 trivia.
- 行号是本线最重的行为约束(实测: 续行内报逻辑行起始的物理行号 p2b:2, 之后报物理行号 p2:4);
  快照含 `.loc` 且诊断含行号, 双重锁定.
- -E 的归一化(丢注释, 折空白)保持原样, 由 trivia.sh 锁定; "保留注释的 formatter 输出"不是
  本线的目标(那是第 2 层消费者的自由).
- 合成 token 只经 `copy_token` 的全字段拷贝这一处派生, 新字段天然随拷贝; `paste` 重入
  `tokenize` 于无末行换行的合成缓冲, 属判据 3 的例外面.

## 提交一览

| 提交 | 步骤 | 内容 |
|---|---|---|
| (已归档) | 线间归档 | codegen 直连线归档为 `PLAN-codegen-direct.md` / `RESULT-codegen-direct.md`, 新建本线与账本 |
| 9312b10 | T0.1 基线 | 复验三道闸门并留档(见上); A/B worktree `/tmp/cbase` 建立; `test/trivia.sh` 立项(首段 7 条 -E 归一化样本, 对两二进制通过)并挂进 `make test` / `make test-stage2` |
| c911de9 | T1.1 | `Token` 增 trivia 切片(四个跳过点未动); `-dump-tokens` 与 T/R 行 dump; trivia.sh 第二段 10 样本; docker-test + snapshot-diff 全绿; 本地 A/B -E 46/46 与 -S 39/39 逐字节一致 |
| 2ddbf76 | T1.2 | `-reconstruct` 与全量 round-trip; trivia.sh 第三段(44 文件 + 6 边界样本); 语料实测: 例外恰为计划列的四文件 |
| (本提交) | T2.1 | 双缓冲 + raw 坐标(设计偏离, 见 T2.1 一节): File 增 raw/map/lines, Token 坐标换 raw, make_scan_buffer 取代三个 pass; 全量 48 文件 round-trip 零例外; -E/-S/诊断/锚点逐字节不变; trivia.sh 扩到 21 dump + 10 边界转正 + 4 锚点 |

## 给审核者的提示

- 审核重心: `new_token` 之外不得出现 trivia 的赋值点(合成 token 走 `copy_token` 全字段拷贝);
  tokenize 四个丢弃点的 `at_bol` / `has_space` 置位逐字节照旧; trivia.sh 的 round-trip 例外
  清单只能缩小.
- 本线不改 parse.c / sema.c / type.c / codegen.c; 任何 preprocess.c 的改动都必须能归入账本表.
