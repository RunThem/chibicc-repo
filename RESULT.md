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
| 1 | `tokenize()` 501-508 行注释 | 跳过, `has_space = true` | trivia | 前置切片(行注释) | T1.1 |
| 2 | `tokenize()` 510-518 块注释 | 跳过(未闭合在 514 走 `error_at`), `has_space = true` | trivia | 前置切片(块注释) | T1.1 |
| 3 | `tokenize()` 520-526 换行 | 跳过, `at_bol = true`, `has_space = false` | trivia | 前置切片(换行, 置位语义不动) | T1.1 |
| 4 | `tokenize()` 528-533 空白 | 跳过, `has_space = true` | trivia | 前置切片(空白串) | T1.1 |
| 5 | `read_file` 670-672 补末行 `\n` | 没有末行换行的文件被加一字节 | File 级事实 | `File` 标记(合成换行) | T2.1 |
| 6 | `tokenize_file` 787-788 BOM | `p += 3`, `contents` 不含 | File 级事实 | `File` 标记(BOM) | T2.1 |
| 7 | `canonicalize_newline` 691-707 | `\r\n` / `\r` 就地改写为 `\n` | trivia + 语义 | `\r` / `\r\n` 归 trivia, 词法按 `\n` 计行 | T2.1 |
| 8 | `remove_backslash_newline` 710-734 | 就地删除 `\`+换行, 换行挪到下一个真实换行后 | trivia + 语义 | `\`+换行归 trivia, 词法透明; 行号语义照旧 | T2.1 |
| 9 | `convert_universal_chars` 747-776 | 全缓冲就地改写 `\uXXXX` / `\UXXXXXXXX` | trivia + 语义 | 标识符/字面量内联转换, 其余保原拼写 | T2.2(可裁) |

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
| (本提交) | T0.1 基线 | 复验三道闸门并留档(见上); A/B worktree `/tmp/cbase` 建立; `test/trivia.sh` 立项(首段 7 条 -E 归一化样本, 对两二进制通过)并挂进 `make test` / `make test-stage2` |

## 给审核者的提示

- 审核重心: `new_token` 之外不得出现 trivia 的赋值点(合成 token 走 `copy_token` 全字段拷贝);
  tokenize 四个丢弃点的 `at_bol` / `has_space` 置位逐字节照旧; trivia.sh 的 round-trip 例外
  清单只能缩小.
- 本线不改 parse.c / sema.c / type.c / codegen.c; 任何 preprocess.c 的改动都必须能归入账本表.
