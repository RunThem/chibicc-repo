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

## M1.1 记录类型 / 访问器 / 通道骨架(未开工)

待填.

## M1.2 对象宏 / 函数宏 body 层(未开工)

待填.

## M1.3 实参层(未开工)

待填.

## M2.1 内建宏 / 合成 token(未开工)

待填.

## M2.2 验证器 / 文档 / 终态(未开工)

待填.

## 提交一览

| 提交 | 步骤 | 内容 |
|---|---|---|
| 待填 | M0.1 | 归档 CST 线, 建立本线计划与记录 |

## 终态验收(本线完成时逐项实测)

- 覆盖 / 查询 / 行为 / 验证器 / 文档五项, 按 `PLAN.md` 的"终态验收"逐条填写.
