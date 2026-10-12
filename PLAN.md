# PLAN.md - trivia 保留执行计划(路线图阶段 1)

目标(用户 2026-10-12 拍板): **修改 tokenize, 让 token 流保留空白/注释/换行等 trivia(不丢弃);
文本级信息完整 - 预处理前的 token 流可逐字节重建输入文件.**

前三条线已归档: 语法语义拆分线见 `PLAN-split.md` / `RESULT-split.md`, 忠实层收尾线见
`PLAN-faithful.md` / `RESULT-faithful.md`, codegen 直连线见 `PLAN-codegen-direct.md` /
`RESULT-codegen-direct.md`. 本线从 codegen 直连终态(5f2af8a: parse.c 2148 行 + sema.c 2992 行
+ chibicc.h 832 行 + codegen.c 2677 行)起步, 是 AGENTS.md 目标架构第 1 层(Token 流, trivia
保留), 服务 formatter 与第 2 层 CST. CST(阶段 2)与库化(阶段 5)在本线完成后由用户拍板, 与本线
正交.

本线结束时: **每个 token 携带它之前被跳过的字节(trivia); 在 tokenize 的直接输出上, 按流序
拼接这些字节与 token 拼写, 逐字节等于输入文件; 一切既有消费者(预处理器, 解析器, sema,
codegen, 全部测试)的可观察行为逐字节不变.**

新会话恢复方法: 通读 AGENTS.md -> 查看本文件勾选状态 -> `git log --oneline` 确认最后完成的
步骤 -> 从第一个未勾选项继续; 开工前先跑 T0.1 的闸门复验. 行号一律以函数名为准.

## 目标边界与判据

**判据 1 - trivia 是新增通道, 不换既有字段, 不动既有语义**
- 新增: 每个 token 的"前置被跳过字节"切片(表示见判据 4), 加上文件级文本事实(合成末行换行,
  BOM).
- 不动: `loc` / `len` / `file` / `line_no` / `at_bol` / `has_space` 的语义与全部读取者 - 预处理
  器的 9 个语义点与驱动器的 `print_tokens`(全部列在现状锚点), 以及诊断的行号与插入符. 既有
  字段一个不改.

**判据 2 - 保真在"重建合同", 不在行为**
本线不改 -E 的归一化输出, 也不把 trivia 塞进任何既有输出; 文本级完整性只以一条合同度量:
"重建 == 原文件字节", 由新增的 token 层测试承载(dump 断言 + 重建断言 + -E 回归锁定).

**判据 3 - 合同的适用面: 预处理前, 单文件**
重建合同只覆盖 `tokenize(file)` 的直接输出. 预处理中合成/展开的 token(`paste` 重入 tokenize
的合成缓冲, `stringize`, `__LINE__` 族, 宏替换/展开结果, `tokenize_string_literal` 的重扫)
不承诺 trivia; 宏调用的保真策略属 AGENTS 的开放决策, 由第 2 层 CST 解, 本线不触碰.

**判据 4 - 表示取最小**
trivia 一律以指向缓冲的切片表示(不复制文本, 不逐项建对象): 一个 token 之前的被跳过字节在
词法上必然是连续区间(线性扫描), 一个 (start, len) 即够; 种类(空白串/换行/行注释/块注释/
反斜杠换行)从区间内容可再派生, 不存冗余. 文件级事实挂 `File`. 新状态不许新增文件域 static.

## 现状锚点(开工前实测)

- **四个丢弃点**(`tokenize()` 的扫描循环, 500-533 行): 行注释 501-508, 块注释 510-518(未闭合
  在 514 走 `error_at`), 换行 520-526, 空白 528-533. 四个点都只置 `at_bol`/`has_space` 后
  前移, 字节被丢弃.
- **五个前置换写**(先于 tokenize 运行, 在 `tokenize_file` / `read_file` 侧): `read_file` 补
  末行 `\n`(670-672, 没有末行换行的文件被加一个字节); BOM 跳过(787-788, `p += 3`,
  `contents` 不含 BOM); `canonicalize_newline`(691-707, `\r\n` / `\r` 就地处写为 `\n`);
  `remove_backslash_newline`(710-734, 就地删除 `\`+换行, 并把删掉的换行数补到下一个真实换行
  之后); `convert_universal_chars`(747-776, 就地处把 `\uXXXX` / `\UXXXXXXXX` 改写为 UTF-8
  字节).
- **行号语义(实测, T2 必须逐字节保持)**: `line_no` 是"拼接后缓冲"上的行号, 由
  `remove_backslash_newline` 的"挪换行"技巧塑形. 实测三例: 续行内的 token 报逻辑行起始的
  物理行号 - `int x = 1 +\` 换行 `@;` 报 `p2b.c:2`, 且插入符行显示拼接后的文本
  `  int x = 1 +@;`; 续行之后的 token 报物理行号(`p2.c:4`); `.loc` 同源(p2c: 续行合并处
  `.loc 1 2`).
- **-E 今天已经在重排文本(实测)**: 注释被丢弃, 连续空白折叠为单个空格(`int  a ;  /* c1 */
  // c2` 输出 `int a ;`); 续行直接拼接(`1 +\` 换行 `2` 输出 `1 +2`); 任何输入的输出都以换行
  结尾; BOM 不输出; 行注释吞掉续行后的文本(`// first \` 换行 `still comment` 整段被吞).
- **`has_space` / `at_bol` 的全部语义点**(改动前必须知道谁在依赖): preprocess.c:77/83/86
  (指令判定), 233(`copy_line`), 374(函数宏识别: 名字后必须无空白才是调用), 469/479
  (`join_tokens` 的 stringize 插空格), 610/611 与 652/653 与 680/681(替换/展开结果继承调用
  点的 `at_bol`/`has_space`); main.c:438-440(`print_tokens`).
- **合成 token 点**: 源 token 的唯一工厂是 tokenize.c 的 `new_token`(100-112); 其余经
  preprocess.c 的 `copy_token`(91-95, `*t = *tok` 全字段拷贝)派生 - `new_eof` / `add_hideset`
  / `append` / `subst` / `expand_macro` 都走它; `paste`(499)重入 `tokenize` 于一个 `format`
  出的合成缓冲(该缓冲没有末行换行); `tokenize_string_literal`(479, 唯一调用者在
  preprocess.c)重扫既有 token 的拼写(UTF-16/32 变体).
- **一处既有边界(实测入档, 不改)**: `##` 拼出 `//` 或 `/*` 时的行为由 paste 的合成缓冲决定
  (`C(/,*)` 报 `unclosed block comment`, 锚在 paste 点, 显示合成文本 `/*`); 新字段不得改变
  这条路径.
- **语料实测**: `test/*.c` 与 `include/*.h` 无 CRLF, 无 BOM; universal chars 只在
  `test/unicode.c`(15-16 行, 字符串字面量内); 反斜杠续行只在 `test/literal.c`(2 处),
  `include/stdarg.h`(7 处), `include/stdatomic.h`(2 处).
- **闸门设施**: 快照语料 = 全部 `test/*.c` 经 `-S`(Makefile 的 `docker-snapshot`, 含
  `.file`/`.loc`); 诊断 55 例(`test/diagnostic.sh`, 612 行), 形状 33 段(`test/shape.sh`,
  340 行), 命令行(`test/driver.sh`, 312 行), 三者由 `make test` / `make test-stage2` 调用.
- **驱动旗标先例**: `-hashmap-test`(main.c:314)是既有的调试模式旗标; 新测试通道挂驱动器侧
  符合既有惯例, 也是未来库 API 面的前身.

## 闸门口径

- 本线每步都是"行为不变"步: `make docker-test`(含自举 - 自举产物也过全部脚本) 全绿 +
  `make docker-snapshot-diff` 为空(raw 逐字节; 本线预期语料不变, 不重置基线) + 该步新增的
  trivia 断言(`test/trivia.sh`, 随各步增量添加, 挂进 `make test` 与 `make test-stage2`).
- 若某步产生计划内的可观察行为变化(唯一候选: T2.2 的注释内 `\u` 显示): 单独提交, 同一提交内
  以 trivia.sh 锁定新行为, 偏差逐条记入 `RESULT.md`; 汇编快照仍必须为空(本线不改发射).
- 一步一提交; 每步汇报固定四项(改了什么 / 为什么 / 闸门结果 / 偏差).
- 红线: 不改诊断文案与插入符锚点; 不改 -E 的归一化; 不改行号语义(`.loc` 与 `__LINE__` 同源);
  不新增文件域 static(新状态进 `File` / `Token`); 不引入新 exit 点; parse.c / sema.c /
  type.c / codegen.c 零改动.
- 本线不做(留给后续线): CST(阶段 2), 库化(阶段 5), 宏调用的保真策略; trivia 的正式消费者
  (首个消费者在第 2 层; 本线只建通道与测试).

## 阶段 T1: trivia 通道(行为不变)

- [x] **T0.1 账本与基线**: `RESULT.md` 建"trivia 丢弃点清单表"(逐项: 站点 / 现状行为 / 归属 /
  目标形态 / 验收口径; 初版已随本文件提交, 9 行在 T0.1 逐行核对), 复验闸门(docker-test /
  snapshot-diff / tinycc)并留档为本线基线; 建立本地 A/B 规程(`git worktree add /tmp/cbase
  e8c8608`, 逐字节对照 .s 与 -E / dump 输出, 同旧例); `test/trivia.sh` 立项并挂进 Makefile
  两个测试目标, 首段锁定 -E 的归一化样本 7 条(由现状锚点的实测样本转正: 注释与空白折叠,
  续行拼接, 行注释吞续行, 无末行换行补出, BOM 丢弃, 空文件, 空行群). 完成记录见
  `RESULT.md` 的"基线记录".
- [x] **T1.1 trivia 通道(四个丢弃点)**: `Token` 增前置被跳过字节的切片(判据 4), `new_token`
  从跳过循环接收区间; 四个丢弃点改为"记录 + 前移"(实现落在 token 产生点: 跳过点的字节由
  下一个 token 认领, 四个跳过点本身与 `at_bol` / `has_space` 置位逐字未动), `has_space` 的
  置位语义原样保留; EOF token 携带尾部 trivia; 驱动器增旗标 `-dump-tokens`(仿
  `-hashmap-test`, 只读) - 每个 token 一行 `T <line> <offset> <kind> <text>`, 每条 trivia
  一行 `R <line> <offset> <kind> <text>`, 文本按 C 字面量式转义(`\n` `\t` `\r` `\\` 与
  不可见字节 `\xHH`, 实现时空格也转义为 `\x20` 以锁行尾, 见 `RESULT.md` 偏差记录), 保证一
  token 一行, 可逐字节锁定. trivia.sh 第二段: 对 snippet 断言 dump(单行注释, 跨行块注释,
  空白与制表符, 空行群, 注释与空白混合, 行首/行尾 trivia, 文件尾 trivia 共 10 样本).
  完成记录见 `RESULT.md` 的 T1.1 一节.
- [x] **T1.2 重建与 round-trip(平凡子集)**: 重建函数(按流序拼接 前置切片 + 拼写; 驱动器增
  旗标 `-reconstruct`, 输出原始字节, 与 `-dump-tokens` 共用同一 cc1 早退口)与 trivia.sh 第三段:
  对 `test/*.c` 与 `include/*.h` 全量断言"重建 == 原文件"(44 个文件, `.h` 用 `-xc` 进 C 路径);
  例外以显式清单跳过并注明消除步骤 - 本步例外 = 计划列的四文件(三处续行: `test/literal.c`,
  `include/stdarg.h`, `include/stdatomic.h`; `test/unicode.c` 的 \u), 语料实测无第五条;
  CRLF / 孤立 CR / BOM / 无末行换行 / 续行 / \u 六个边界样本以"当前已知偏差"锁定(T2.1/T2.2
  退役对应 pass 时改为等于输入). 完成记录见 `RESULT.md` 的 T1.2 一节.

## 阶段 T2: 原始文本保真(逐字节重建), 可裁

T1 之后通道成立, 但缓冲仍是"被五个 pass 改写过的文本"; T2 把破坏性改写退役, 让缓冲就是原文
件, 重建例外清零. T2.1 是主体; T2.2 是否做由用户拍板.

- [x] **T2.1 CRLF 与反斜杠换行归 trivia**: 实施时改为**双缓冲 + 偏移映射**(设计与计划不同, 理由与
  全貌见 `RESULT.md` 的 T2.1 一节): 扫描缓冲的文字效果逐字节保留(词法各扫描点无需透明化,
  token 恒等与显示路径零改动), 新增的是 raw 文本与两张表 - `File.raw`(原样字节),
  `File.map`(scan 偏移 -> raw 偏移), `File.lines`(raw 偏移 -> 行号, 按被退役 pass 的"挪换行"
  语义填充); `make_scan_buffer` 一遍产出扫描缓冲 + 两表并取代 `canonicalize_newline` /
  `remove_backslash_newline` / `convert_universal_chars`; `read_file` 返回原样文本(BOM 跳过与
  末行补 `\n` 折进扫描缓冲, BOM 成为首 token 的前置 trivia, 合成换行在 raw 里不存在);
  token 的 trivia/拼写坐标换到 raw(`Token.trivia` / `trivia_len` / `raw_loc` / `raw_len`);
  `-dump-tokens` 按 raw 展示(trivia 分片加 `cont` / `bom`, `nl` 覆盖三型换行), `-reconstruct`
  按 raw 拼接. 行号语义按现状锚点逐字节保持(p2 / p2b / p2c / p3 样本已在 trivia.sh 钉住,
  另见全语料 A/B). round-trip 例外清零(含 CRLF / 孤立 CR / BOM / 无末行换行 / 三处续行文件 /
  `test/unicode.c` - 48 个文件全量成立).
- [x] **T2.2 universal chars 保真(已由 T2.1 的设计吸收, 无需单独实施)**: raw 坐标让 `\uXXXX` /
  `\UXXXXXXXX` 原样重建(重建 == 输入, `test/unicode.c` 无例外); 扫描缓冲保留就地改写, 因此
  -E 与诊断显示维持原样, 计划里"唯一的计划内可观察行为变化"不再发生. 用户若要 -E / CST 层面
  看到 \u 原拼写, 属第 2 层(CST 建在 raw 文本上)的自由, 与本层合同无关. 若用户裁决仍需在
  本层做, 可在此重开: 代价与步骤同原计划(扫描点内联转换 + 一处显示变化).

## 阶段 T3: 收口(测试与文档)

- [ ] **T3.1 trivia 测试收口**: trivia.sh 三段齐全(dump 断言 / 全量 round-trip == 原文件 /
  -E 与诊断锚点的锁定样本), 边界样本目录全覆盖(CRLF, 孤立 CR, BOM, 无末行换行, 空文件, 只含
  注释, 续行落在字符串/注释/指令中, 续行 + \u, 深层混合), 例外清单清空或列明不可覆盖项与
  理由.
- [ ] **T3.2 文档与终态**: AGENTS.md 的目标架构第 1 条, 代码地图(tokenize.c 条目), 测试段与
  构建段按终态更新; 本文件勾选与 `RESULT.md` 的终态实测(对比锚点: tokenize.c / chibicc.h /
  main.c / preprocess.c 行数, trivia.sh 行数).

## 开放决策(实施时由用户拍板)

- **T2.2 做不做**(universal chars 原拼写): 代价是词法点增加与一处计划内行为变化; 不做则把
  \u 例外留到 CST.
- **trivia 的字段形态**: 默认取判据 4 的单切片 + 派生函数; 若希望 dump 与消费者直接拿到类型
  化序列, 升级为链(代价是每个 token 一个对象).
- **驱动旗标命名**(`-dump-tokens` / `-reconstruct`): 属未来库 API 面的前身, 库名与公共头
  定名时一并调整.
- **宏调用保真策略**(AGENTS 既有开放决策): 本线以判据 3 回避, 由第 2 层 CST 处理.

## 风险与对策

1. **行号语义漂移(最高风险)**: 续行与 CRLF 的行号/插入符由两个 pass 的"挪换行"技巧塑形;
   对策 = 现状锚点样本(p2 / p2b / p2c 与 p3 的 CRLF 诊断)先钉进 trivia.sh, T2.1 只认这些
   断言; 每步用 `docker-snapshot-diff`(含 `.loc`)与诊断 55 例复验.
2. **计划外的显示差异**: 诊断打印缓冲里的行文本, 任何让缓冲与今天不同的改动都可能改变显示
   (如 CRLF 的 `\r`, 注释内 \u); 对策 = 差异要么是 T2.2 的计划内变化(锁定 + 记偏差), 要么
   视为回归; "`\r` 不进入打印行"在 T2.1 内单独实测钉住.
3. **词法透明化漏点(T2.1)**: `\`+换行需在 7 类扫描点透明(实测: 今天的行注释会吞掉续行后的
   文本; 字符串扫描经 `string_literal_end` 的跳过); 对策 = 逐类样本进 trivia.sh, 语料里三处
   真实续行文件进全量 round-trip, macro 测试与 f 系列诊断保底.
4. **paste 的重入路径**: `paste` 重入 `tokenize` 于合成缓冲(无 trivia, 判据 3); `copy_token`
   全字段拷贝天然携带新字段; 对策 = 只测不改, 以 macro 诊断与 f 系列用例复验; p9 / p10 的既
   有边界入档不改.
5. **-E 归一化漂移**: `print_tokens` 是 `at_bol` / `has_space` 的最大消费者; 对策 = T0.1 建
   -E 锁定样本, 每步复验.
6. **自举回路**: tokenize.c 与 chibicc.h 的改动被自举编译两遍(`make test-stage2`); 对策 =
   每步跑含自举的 `docker-test`, 自举产物同样过 trivia.sh.
7. **内存与接口**: trivia 只存切片, 重建输出走 arena buffer; 不新增 static; dump / reconstruct
   模式不引入新 exit; 合成 token 的 trivia 为空, 读取方(测试)按空处理.

## 终态验收(本线完成时逐项实测填写)

- **通道**: 四个丢弃点全部转为 trivia; "前置切片 + 拼写"覆盖文件每个字节; 重建合同在
  `test/*.c` + `include/*.h` + 边界样本上成立, 例外清空(或按 T2.2 的裁决列明).
- **行为**: 诊断 55 例, 形状 33 段, -E 锁定样本, `.s` 快照逐字节不变(计划内偏差逐条记录).
- **闸门**: `make docker-test`(含自举) + `docker-snapshot-diff` 空 + `tinycc`(终态跑一次) +
  trivia.sh 挂进两个测试目标.
- **对比锚点**: tokenize.c 805 / chibicc.h 832 / main.c 791 / preprocess.c 1208 -> 实测填入;
  新增 test/trivia.sh 行数.
