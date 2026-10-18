# RESULT.md - 宏展开 provenance 执行记录

本文件记录 `PLAN.md`(宏展开 provenance 线, 路线图阶段 5 的前置)各步骤的实际执行结果,
供审核与后续会话接续参考. 五条已归档线: 语法语义拆分见 `PLAN-split.md` / `RESULT-split.md`,
忠实层收尾见 `PLAN-faithful.md` / `RESULT-faithful.md`, codegen 直连见
`PLAN-codegen-direct.md` / `RESULT-codegen-direct.md`, trivia 保留见 `PLAN-trivia.md` /
`RESULT-trivia.md`, CST 见 `PLAN-cst.md` / `RESULT-cst.md` - 后者记录的终态是本线的量化
对比基线.

## 闸门口径(本线生效)

- 每步(均为行为不变步): `make docker-test`(含自举与诊断 55 例 + 形状 33 段 +
  trivia / CST 通道) + `make docker-snapshot-diff` 为空 + `test/expand.sh` 该步新增的
  断言(挂进 `make test` 与 `make test-stage2`).
- 一步一提交; 每步汇报固定四项(改了什么 / 为什么 / 闸门结果 / 偏差).
- 红线: 不改 `parse.c` / `sema.c` / `type.c` / `codegen.c`; 不改 `Token.origin` 语义;
  不改展开的 token 文本 / 顺序 / 类型; 不新增 exit 点; 不新增文件域 static; 不引入新编译
  告警; 新代码保持纯 C11 可自举.

## 基线记录(M0.1)

- 开工 HEAD: `1851528` (Revert "Add a temporary faithful AST dump channel ..."),
  工作树含 CST 线归档的未提交改动(两个 `git mv` + AGENTS.md 的 CST 终态段).
- `make docker-test`: EXIT=0 (stage1 + stage2 + 自举, 日志 `.cache/M0.1-baseline.log`).
- `make docker-snapshot-diff`: `snapshot diff: empty` (41 个 `test/*.c` 的汇编快照).
- 既有通道基线(同一次运行的汇总行): `test/trivia.sh` 7 个 -E 样本 + 23 个 dump 样本 +
  48 个 round-trip 文件 + 12 个边界样本 + 4 个锚点用例(合计 94 项断言),
  `test/cst.sh` 48 round-trip / 48 verify / 15 边界 / 61 dump 样本(305 项断言),
  `test/diagnostic.sh` 55 例, `test/shape.sh` 33 段.
- `test/expand.sh` 尚不存在, `Makefile` 未引用它, 因而不影响 `make test` 与
  `make test-stage2`(本步确认, M1.1 建立并挂入两个目标).

## 设计账本(开工前分析, 实施时逐项核对)

### 输入事实

| 事实 | 位置 | 对设计的影响 |
|---|---|---|
| `Token` 有 `loc` / `len`(扫描坐标) 与 `raw_loc` / `raw_len`(raw 坐标) | `chibicc.h` 87-122 | provenance 与源码区间分开存, 不挤占既有字段 |
| `Token.origin` 只在 `expand_macro` 的 object-like / function-like 路径写入 | `preprocess.c` 650, 678 | 保留其语义; 新链独立 |
| `origin` 只被 `file_macro` / `line_macro` 读取 | `preprocess.c` 1008-1019 | 改动面可控; `__FILE__` / `__LINE__` 是回归锚点 |
| `copy_token` 整结构拷贝; `add_hideset` / `append` 复制展开结果 | `preprocess.c` 91-162 | provenance 必须是 `Token` 字段, 旁表会因复制失效 |
| `Macro` 没有定义处 token | `preprocess.c` 43-51 | 增 `Macro.def` |
| `MacroArg` 没有序号 | `preprocess.c` 33-39 | 增序号供实参层使用 |
| function-like 读取实参后手里有 `rparen` | `preprocess.c` 665 | 调用范围零额外解析成本 |
| paste / stringize / 内建产物走 `tokenize(new_file(...))` | `preprocess.c` 221-244, 499-508 | 这些 token 没有真实源位置; provenance 由记录 + 拼写承担 |
| `join_adjacent_string_literals` 1160 行整体覆盖 token | `preprocess.c` 1140-1195 | 需保存 / 恢复 `prov` |
| 展开是内联拼接, 调用 token 被消费但名 token 存活 | `preprocess.c` 839-991 | 记录可持有名 token / `rparen` / `MacroArg` |

### 歧义清单

1. **嵌套实参**: E 的实参文本里又有宏 I; I 的实参再被消费. 用 record parent 会成环或需要
   mutation 共享记录, 因此采用 per-token 链, 全部 prepend.
2. **paste / stringize**: 产物没有真实源位置; provenance 归宏体层, 字符级重放靠记录 + 实参
   列表. 不承诺逐字节还原被拼接的字符.
3. **内建宏**: `__LINE__` / `__FILE__` / `__COUNTER__` / `__TIMESTAMP__` / `__BASE_FILE__`
   没有 `#define` 文本; 建 builtin 记录, 定义打 `?`.
4. **`#line`**: token 的 `line_no` 在 `preprocess` 末尾被加上 `line_delta`; dump 用
   `line_no - line_delta` 还原物理行.
5. **相邻字符串合并**: 合并后的 token 只保留第一个 literal 的 provenance; 第二个的链不保留.
6. **空宏展开**: 不产生 token, 因而不在 dump / verify 中出现; 不虚构条目.
7. **重定义 / `#undef`**: 记录持有展开时刻的 `Macro`; 旧展开仍指向旧定义. 测试覆盖.
8. **`-D` 定义**: 合成缓冲, `Macro.def` 为 NULL; dump 打 `?`.
9. **头文件定义 + 主文件调用**: 定义与调用文件可以不同; dump 打印两个文件名. 本机缺 Linux
   include 路径, 全量语料校验在 docker 跑.

### 数据模型与理由

- `MacroExpansion` 每次展开一条, 共享只读; 字段: `name` / `rparen` / `macro` / `args` /
  `arg_count` / `is_builtin`.
- `MacroProv` 每个 token 每层一个节点: `exp` / `arg_index` / `next`.
- 链按"最内层在前"; 所有挂接都是 `new_prov(E, layer, base)`.
- 与 Clang 的对照: Clang 用 location 的两种坐标 (spelling / expansion) 加 SLocEntry 树,
  外加 `PPCallbacks` / `PreprocessingRecord` 旁表; 它同样**没有宏调用 AST 节点**, 同样
  **不保留未选中的 `#if` 分支**. chibicc 的 CST 在"全分支保留"上强于 Clang; 本线补的是
  Clang 那套 provenance, 不复制它的 location 编码, 而是在 token 上挂链.

### dump 冻结口径

```
T <file>:<phys-line|@synth> <kind> <spelling> <chain>
```

- `phys-line = line_no - line_delta`; `@synth` 当 `file->raw == file->contents`.
- `<chain>` 为空时 `-`; 否则 entry 用 `>` 连接, 最内层在前.
- entry: `<macro>@<call-file>:<call-line>:<body|argN>:<def-file>:<def-line>`;
  `argN` 从 1 起; 缺失定义打 `?`.
- 转义复用 `print_escaped`; EOF 不输出.

### 不变量(每一步都测)

- **I1**: 由某次展开直接产生的 token, 其 prov 链头就是该展开, 层为 body 或对应实参.
- **I2**: `arg_index` 只能是 -1 或落在该展开的实参范围内; 实参列表非空.
- **I3**: prov 链只 prepend, 共享尾不可变; 任何 token 的链上不出现重复节点.
- **I4**: `-E`, 诊断锚点, `Token.origin` 语义, trivia / CST 输出逐字节不变.
- **I5**: 无新增 exit 点, 无新增文件域 static; 记录 / 链节点与 token 同 arena 生命周期.

## 关键结论(开工前分析, 供审核者速览)

- 本线是"加一层 provenance", 不是第二棵树, 也不是宏调用节点; 这一裁决与 Clang 的实际做法
  一致 (Clang 的 AST 里也没有宏调用节点).
- 记录必须挂在 `Token` 上: 展开结果会被 `add_hideset` / `append` 整体复制, 旁表会失效.
- 共享记录不能挂消费链父指针; per-token prepend 链是唯一同时满足"嵌套实参正确"与"记录只读"
  的简单形态.
- 行为不变是可验证的: 只增字段 / 记录 / 访问器 / 只读通道, 不改 token 文本与顺序, 不改
  parse / sema / codegen, 因此 assembly snapshot 应保持为空 diff.

## M0.1 归档 / 账本 / 基线

- 已完成:
  - `git mv PLAN.md PLAN-cst.md`, `git mv RESULT.md RESULT-cst.md`; 两个归档文件加归档说明.
  - 新建本 `RESULT.md` 与本线 `PLAN.md`(步骤全部未勾选), AGENTS.md 的路线图 / 开放决策 /
    代码地图同步到"provenance 线已立项".
  - 回填开工 HEAD `1851528` 与两道闸门结果(见上"基线记录"): docker-test EXIT=0,
    snapshot diff empty; 既有通道计数与汇总行一致.
  - 确认 `test/expand.sh` 尚未存在时 `make test` 不受影响(`Makefile` 未引用).
  - 账本(输入事实 / 歧义清单 / 数据模型 / 分支清单 / dump 冻结尾 / 不变量 I1-I5)与
    `PLAN.md` 一致, 本步未改动其口径.
- 偏差: 无. 本步无代码改动.

## M1.1 记录类型 / 访问器 / 通道骨架

- 改了什么:
  - `chibicc.h`: 前向声明 `MacroExpansion` / `MacroProv`; `Token` 增 `prov`(紧跟
    `origin`); 11 个访问器与 `expansion_dump` / `expansion_verify` 两个通道函数;
    `print_escaped` / `token_kind_name` 去 static 并在头文件声明(expansion_dump.c 复用
    同一套转义与 kind 名, 不复制实现).
  - `preprocess.c`: 定义 `MacroExpansion` / `MacroProv`(不透明, 只在文件内); `Macro` 增
    `def`; `read_macro_definition` 保存 `#define` 处的宏名 token; `new_eof` 显式清空
    `prov`; 访问器实现(参数列表按 `arg_count` 越界返回 NULL).
  - `expansion_dump.c`(新): dump 输出 `T <file>:<phys-line|@synth> <kind> <spelling> -`
    骨架(本步还没有链); verify 恒真(骨架, M2.2 实现检查). 只用头文件访问器与
    `print_escaped` / `token_kind_name`.
  - `main.c`: `-dump-expansions` / `-verify-expansions` 旗标; `cc1()` 在
    `preprocess(tok)` 之后早退(dump 走 `-o`, verify 失败返回 1), 不进入 sema;
    driver 的 pass-through 列表加入两个旗标.
  - `test/expand.sh`(新): harness + 3 个无宏 dump 样本(plain / directive / empty)与
    3 个 verify 样本; `Makefile` 的 `test` 与 `test-stage2` 各加一行.
- 为什么: provenance 的公共面先立起来(记录不透明, 访问器是未来库 API 的种子), 通道
  先能跑, 后续步骤只填记录写入点; 本步不建任何记录, 因此 dump 的 chain 全为 `-`.
- 闸门: `make docker-snapshot-diff` 输出 `snapshot diff: empty`; `make docker-test`
  EXIT=0(stage1 + stage2, 含 trivia / cst / expand); `test/expand.sh` 3 个 dump 样本 +
  3 个 verify 样本通过; 既有 trivia(94 项)与 CST(305 项)计数不变.
- 偏差: 无. 新文件自举通过(stage2 由 docker-test 覆盖).

## M1.2 对象宏 / 函数宏 body 层

- 改了什么:
  - `preprocess.c`: 新增 `new_prov`(只 prepend)与 `new_expansion`(建记录)、
    `count_macro_args`; `expand_macro` 的 object-like 分支建记录并在既有的 `origin`
    循环里给宏体 token 挂 `new_prov(e, -1, 调用名 token 的 prov)`; function-like 分支在
    `rparen` 之后建记录(写入 `name` / `rparen` / `macro` / `args` / `arg_count`), 同样在
    `origin` 循环里挂 body 层. `origin` 的语义与写入位置不变.
  - `expansion_dump.c`: `-dump-expansions` 输出完整 chain; entry 为
    `<macro>@<call-file>:<call-line>:<body|argN>:<def-file>:<def-line>`, 最内层在前用
    `>` 连接, 定义缺失打 `?:?`(`-D` / 内建).
  - `test/expand.sh`: 加 7 个 dump 样本(对象宏, 函数宏体, 嵌套, `#undef` / 重定义,
    `#line`, 空宏, 头文件定义)与 3 个 verify 样本.
- 为什么: 先把"展开产物从哪来"的最小闭环(调用点 / 定义 / 层)打通; 实参层留给 M1.3.
  本步 function-like 的 body 层在 `expand_macro` 的输出循环里统一挂, 对引用实参的宏会
  暂时把实参 token 也标成 body(本步没有这样的测试); M1.3 把挂接移进 `subst` 后按分支
  细分, 该临时行为消失.
- 闸门: `snapshot diff: empty`; `make docker-test` EXIT=0; `test/expand.sh`
  10 个 dump 样本 + 6 个 verify 样本通过(stage1 与 stage2 各一遍).
- 偏差: 无. 运行时实测的 dump 与本步推导的期望逐字节一致(重定义指向旧定义, `#line`
  还原物理行, 头文件定义显示 `./defs.h:1`).

## M1.3 实参层

- 改了什么:
  - `preprocess.c`: `MacroArg` 增 `index`; `read_macro_args` 按参数顺序赋 0 起序号(变参
    作为一个实参, 序号为命名参数个数); `subst` 增 `MacroExpansion *e` 参数, 按"记录写入
    清单"逐分支挂接: `#` 产物 / `##` 与 `,##__VA_ARGS__` 的逗号 / paste 产物 / 空实参时
    的体 token / `__VA_OPT__` 的体 token / 普通体 token 一律 body 层; 实参 token(含 `##`
    两侧未展开的实参和预扫描后的普通实参)按 `arg->index` 挂 arg 层. 普通实参的链以
    `preprocess2` 产物 token 自己的 prov 为尾(嵌套宏链因此保留在实参层之后).
  - `expand_macro` 的 function-like 输出循环只保留 `origin` 写入; provenance 挂接全部
    移进 `subst`(M1.2 的临时统一 body 层消失).
  - `test/expand.sh`: 加 12 个 dump 样本与 6 个 verify 样本, 覆盖每个实参 / 参数重复 /
    实参内嵌套宏 / `#`(单 token 与多 token) / `##`(两侧, 左侧空, 右侧体 token) /
    `,##__VA_ARGS__`(非空与空) / `__VA_OPT__` / 变参.
- 为什么: 实参层是"来自宏体还是第几个实参"的判据; `#` 与 `##` 的实参不预展开, 所以它
  们的实参层直接接调用点链; 普通实参先经 `preprocess2`, 其产物已经带内层展开的链, 新层
  按 prepend 规则放到链头(与 PLAN.md 的 A/F 例子一致).
- 闸门: `snapshot diff: empty`; `make docker-test` EXIT=0; `test/expand.sh`
  22 个 dump 样本 + 12 个 verify 样本通过(stage1 与 stage2).
- 偏差: 无. paste 的产物只保留 body 层, 被拼接的操作数 provenance 不留在结果 token 上
  (PLAN.md 已列为已知限制, 字符级重放靠记录与实参列表).

## M2.1 内建宏 / 合成 token

- 改了什么:
  - `preprocess.c`: `expand_macro` 的 handler 分支建 `is_builtin = true` 的记录(名 token /
    定义宏 / 无 rparen / 无实参), 在把 handler 产物接到调用点**之前**逐个挂 body 层
    (链尾是调用名 token 的 prov); `join_adjacent_string_literals` 第一遍的
    `*t = *tokenize_string_literal(t, basety)` 保存 / 恢复 `prov`, 合并后的宽字面量保留
    源 token 的链; 第二遍的 `*tok1 = *copy_token(tok1)` 本来就整结构拷贝, 不改.
  - `test/expand.sh`: 加 6 个 dump 样本(`__LINE__` / `__FILE__` / `__COUNTER__` /
    `__BASE_FILE__` / 嵌套 `__LINE__` / 相邻字符串合并), 3 个 chain 形状样本
    (`__DATE__` / `__TIMESTAMP__` / 宽字符串重扫)与 4 个 verify 样本.
- 为什么: handler 产物与 paste / stringize 一样没有源位置, 记录与链是唯一可查的来源;
  `__DATE__` / `__TIMESTAMP__` 的文本随环境变化, 只锁链的形状. 宽字符串重扫是
  `join_adjacent_string_literals` 里唯一整体覆盖 token 的点, 不保存 prov 会静默丢链.
- 闸门: `snapshot diff: empty`; `make docker-test` EXIT=0; `test/expand.sh`
  28 个 dump 样本 + 3 个 chain 样本 + 16 个 verify 样本通过(stage1 与 stage2).
- 偏差: 宽字符串重扫出的 token 沿用既有的 `line_no = 0`(重扫路径从不调用
  `add_line_numbers`), 这是本线之前就有的字段状态, 不在本线修; chain 形状样本只锁链,
  不锁该位置.

## M2.2 验证器 / 文档 / 终态

- 改了什么:
  - `expansion_dump.c`: `expansion_verify` 从骨架改为完整实现. 对每个有链的 token 检查:
    深度上界(1000)与链上节点唯一性(I3); 每层记录非空; 名 token 与宏拼写一致; arg_index
    为 -1 或落在 `[0, arg_count)`(I2); 每个实参列表非空且以 EOF 终止(带 1M token 上界);
    arg_index >= 0 时展开必须是 function-like(`end != name`)且不是内建. 失败用
    `fprintf(stderr, ...)` 打一条 `chibicc: <file>:<line|@synth>: invalid macro provenance:
    <what>` 并返回 false(exit 1, 不调用 error, 不新增 exit 点).
  - `preprocess.c`: `macro_expansion_arg` 在遍历中加 NULL 守卫(损坏的 arg_count 不会让
    验证器自己崩溃); 合法输入行为不变.
  - `test/expand.sh`: 全量语料段(48 个 `test/*.c` + `include/*.h`)逐个跑
    `-verify-expansions`; 本机用 `#include <stdio.h>` 探测, 失败则打印 skipped, docker 内
    必定执行. 汇总行增加 corpus 计数.
  - `AGENTS.md`: 路线图(provenance 线标记完成并写明终态), 构建与测试(新增 expand.sh 与两个
    本机可跑通道), 现状与代码地图(`Token.prov` / `MacroExpansion` / `MacroProv` /
    `preprocess.c` 写入点 / 新增 `expansion_dump.c` / `main.c` 旗标), 测试约定(expand.sh
    的段落结构), 已决事项(落地状态).
- 为什么: 验证器是链结构的守门人, 它必须只靠访问器判断, 并在失败时以退出码而不是
  进程终止报告(库化红线); 全量语料校验把"每个 token 的链都自洽"从样本推广到所有输入.
- 闸门中发现并修正的问题: 第一次终态 docker run 里 stage2 的全量段打印了 skipped.
  容器内手工探测确认原因: stage2 的 argv[0] 是 `./stage2/chibicc`, 自动 include 路径指向
  不存在的 `./stage2/include`, 而 `<stdio.h>` 链上的 `<stddef.h>` 由 chibicc 自带的
  `include/` 提供, 于是探测命令本身失败. 这是测试脚本的 include 路径问题, 不是编译器
  缺陷(同一容器里 stage1 的 48 个文件全部通过, stage2 的样本测试也全部通过). 修正:
  探测与全量段都带 `-Iinclude -Itest`, 与主测试集的编译命令一致; 复跑后两个阶段都验证
  48 个文件.
- 负向自测(手工, 不入库): 临时把 `new_prov` 的 body 层下标强制成 0, 对
  `#define M 1 / int x = M;` 跑 `-verify-expansions` 得到
  `chibicc: /tmp/neg.c:1: invalid macro provenance: argument index out of range` 与
  exit 1; 恢复源码并重建后同一命令 exit 0, 全部样本与语料通过. 这说明失败路径真的会触发,
  且失败不是静默的.
- 闸门: `snapshot diff: empty`; `make docker-test` EXIT=0(含 stage2 自举); `test/expand.sh`
  29 个 dump 样本 + 3 个 chain 样本 + 17 个 verify 样本 + 48 个语料文件通过(stage1 与
  stage2 各一遍, 两个阶段都报告 48 files verified).
- 偏差: 无. 本机全量段因缺 Linux 系统头按计划 skipped; docker 内 stage1 与 stage2 都是
  48 个文件全量通过.

## 终态验收

- 覆盖: 判据 1 的每一类产物都有 dump 样本 - 对象宏(obj), 函数宏体(fn), 嵌套(nest /
  arg_nested / builtin_nested), 内建宏(builtin_line / file / counter / base_file,
  date_shape / timestamp_shape), `#`(stringize / stringize_ws), `##`(paste /
  paste_left_empty / paste_body_rhs), `,##__VA_ARGS__`(va_comma / va_comma_empty),
  `__VA_OPT__`(va_opt), 变参(variadic). 空宏展开不产生 token 的已知限制由 empty_macro
  样本锁定. `subst` 记录清单的每一行至少有一个测试.
- 查询: 调用点(链的 call 位置), 调用范围(`macro_expansion_end`, verify 的 function-like
  检查), 定义(`?:?` 与 def 位置; redef / hdr / #line 样本锁定时序与跨文件),
  实参序号与实参列表(arg / arg_repeat / arg_nested / paste_left_empty / variadic),
  嵌套链(nest / arg_nested / builtin_nested)全部由 `test/expand.sh` 覆盖.
- 行为: `make docker-test` EXIT=0; `snapshot diff: empty`; `-E` / 诊断 55 例 /
  trivia 94 项 / CST 305 项逐字节不变(同一次 docker-test 全绿); `Token.origin` 的语义与
  唯一消费者未改; `parse.c` / `sema.c` / `type.c` / `codegen.c` 零改动.
- 验证器: docker 内 48 个语料文件全部通过 `-verify-expansions`(stage1 与 stage2 各一遍);
  本地片段(17 个 verify 样本)通过; 手工负向样本按预期 exit 1.
- 文档: AGENTS.md 的路线图 / 构建与测试 / 现状与代码地图 / 测试约定与终态一致; 本文件
  与 `PLAN.md` 的勾选状态一致.

## 提交一览

| 提交 | 步骤 | 内容 |
|---|---|---|
| `659eadb` | M0.1 | 归档 CST 线, 建立本线计划与记录, 基线闸门全绿 |
| `922711a` | M1.1 | 记录类型 / 访问器 / 两个只读通道骨架 + test/expand.sh |
| `d2af2d2` | M1.2 | 对象宏 / 函数宏 body 层与 dump 的完整链 |
| `da7e241` | M1.3 | subst 全分支的实参层(body / arg 细分, 嵌套链保留) |
| `68772e2` | M2.1 | 内建宏记录 / 合成 token 与相邻字符串合并的 provenance |
| (本提交) | M2.2 | 验证器 / 全量语料校验 / 文档与终态验收 |
