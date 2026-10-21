# RESULT-pratt-parse.md - 表达式 Pratt 解析线执行记录

> 状态: 完成 (2026-10-21)
> 基线: 267fb6f (P0 开工前, 分支 `pratt-parse`)
> 计划: `plans/PLAN-pratt-parse.md` (执行期在仓库根目录, P6.3 归档)
> 终态提交: P1 86686a0, P2 a716f97, P3+P4 17b2838, P5+P6 (本条, 见 `git log`)

本记录按 P0-P6 记每步的实测, 闸门输出与偏差. 闸门在本机 macOS 上只能跑前端部分;
完整测试, 自举与 tinycc 在 Docker (OrbStack, amd64) 内跑, 那里是验收证据.

## P0 - 基线与开工准备

- `git rev-parse HEAD` = `267fb6f7a1bb89e0fe3cf6cbadaa4f77f143af6b`.
- `libchibicc/src/parse.c` 2332 行; 表达式区域 1230-2192; `test/*.c` 41 个.
- `make docker-test` (基线, 开工前): exit 0, 含自举与 stage2.
- `make docker-snapshot-diff`: `snapshot diff: empty`, 41 个 `.s`.
- 分支 `pratt-parse` 从 `267fb6f` 开出; 每步一个提交 (P3/P4 合为一个提交, 见偏差 7).

## P1 - AST 形态锁 (不改解析器)

- `main.c` 新增 `-dump-ast`: 在 `chibicc_parse` 之后, `chibicc_sema` 之前早退; 不写目标
  文件, 不汇编. 输出逐行 `<indent><Kind> @<位置> "<拼写>" [字段]`.
- 位置: 源 token 打 `@<file>:<line>:<col>`; `tok->prov` 非空的宏展开产物打 `@synth`
  (宏体位置或合成缓冲, 都不是表达式在源码中的位置).
- 字段是 parse 会写的那一层: `op=` (复合赋值), `is_post` / `addend`, `is_elvis`,
  `arrow`, `label`, `is_scope_block`, `is_implicit`, `default`, `val` / `fval`, `asm_str`,
  `ty_op` / `ty_op2` (只打 kind 与 base/return 链, 含数组声明长度与 unsigned/atomic 标记,
  不打 size/offset/地址), `name`, `attr` (storage class 与 `_Alignas`). 初始化器单独打印
  `INIT kind=LIST|EXPR|STR`, 列表项带 `comma_tok`, 指示符带 begin/end 表达式.
- `test/ast.sh`: 85 个片段逐字节锁定. 覆盖 65 个 parse 产生的 `NodeKind`; 69 个 kind 中
  未覆盖的 4 个 (`ND_NULL_EXPR` / `ND_VAR` / `ND_VLA_PTR` / `ND_MEMZERO`) 只由 sema 产生.
  三个语义非法但语法合法的输入 (`(struct S){0}` 不完整类型, `p * p`, 未知名) 证明通道
  与 sema 解耦. 初始 78 例, P4 后补 7 例 (三逗号右嵌套, `?:` 两侧赋值, cast 后缀,
  后缀链, 一元链, 字符串表达式).
- 接入 `make test` 与 `make test-stage2`; `AGENTS.md` 的只读通道清单补 `-dump-ast`.
- 闸门: `make docker-test` exit 0 (stage1 + stage2 各跑一遍 ast.sh); `docker-snapshot-diff`
  为空 diff (默认路径零行为变化).

## P2 - 特性宏与双构建 (代码路径尚未分叉)

- `libchibicc/src/chibicc.h`: `CHIBICC_PRATT_PARSE_FEATURE` 默认 0.
- Makefile: out-of-tree `pratt/` (库对象 + 驱动对象) 与 `stage2-pratt/`; 目标
  `chibicc-pratt`, `pratt/libchibicc.a`, `test-pratt`, `test-both`, `test-ab`,
  `test-stage2-pratt`, `check-library-pratt`, Docker 的 `docker-test-both`,
  `docker-test-pratt`, `docker-pratt-ab`, `docker-pratt-tinycc`. `BOOT` 与 `clean`
  排除/清理新目录; `.gitignore` 补 `chibicc-pratt` / `pratt` / `stage2-pratt`.
- `test/pratt-ab.sh`:
  - 默认模式: 41 个 `test/*.c` 的 `-dump-ast` 与 `-S`, 固定 `__DATE__` / `__TIME__` /
    `__TIMESTAMP__`.
  - `--thirdparty tinycc`: 钉住 `df67d86`, `./configure --cc=<old>` 只跑一次; shim 原样
    转发每条命令, 对含 `-c` 的命令去掉 `-c` / `-o` / 依赖生成 flag 后再跑 `-dump-ast`
    与 `-S`, 日志键由源路径与归一化参数决定; 两个构建各跑 `$make CC=cc test`; 两棵日志
    树 `diff -ru` 必须为空.
- `test/library.sh` 接受 `CHIBICC_PRATT_PARSE_FEATURE` (ASan 重编同一解析器);
  `test/public-api.sh` 接受 `CHIBICC_LIB`, 新增 `check-library-pratt`.
- 闸门: `make docker-test-both` exit 0 (test, test-stage2, test-pratt,
  test-stage2-pratt); `make docker-pratt-ab` exit 0 (corpus 41, tinycc 10 个翻译单元,
  两套 tinycc 测试通过). 此时尚未改任何解析代码, 是空基线.

## P3 - Pratt 接管二元层

- 具名 `BP_COMMA` 到 `BP_POSTFIX`; `infix_bp`; `expr_bp`; 三个入口
  `expr` / `assign` / `conditional` 改调 `expr_bp(0 / 1 / 2)`. 旧 `expr` 到 `mul` 保护.
- 发现并修正一处形状偏差: 计划表把逗号列为左结合, 但旧 `expr1` 的右操作数是 `expr`,
  树是 `a,(b,c)`. `test/control.c` 的 `assert(a, 1, 2, 3)` dump 暴露该差异; 改为
  `rbp = BP_COMMA - 1`, 与旧树逐字节一致.
- 局部证据 (macOS): 39 个可预处理语料文件的 AST 与 `-S` 全部相同 (2 个因缺 pthread.h
  跳过, docker 内 41 个全过); ast.sh 78 例; 诊断 68 例.

## P4 - Pratt 接管前缀, 后缀与 cast

- `expr_nud`: `(type)` cast / `(type){...}` 复合字面量, 一元 `+ - & * ! ~`, 前缀
  `++` / `--`, `&&label`, 其余交共享 `primary`; 一元 `+` 透明.
- `expr_bp` 的 led 段: 调用 (`funcall`), 下标 (`ND_SUBSCRIPT`, 索引 `expr`), `.` / `->`
  (`struct_ref`, 保留 `arrow_tok`), 后缀 `++` / `--`; 复合字面量后可接后缀.
- 旧 `cast`/`cast1`/`unary`/`unary1` 与 `postfix` 加 `#if !CHIBICC_PRATT_PARSE_FEATURE`;
  Pratt 侧只留薄 `unary` (`expr_bp(BP_CAST)`) 供 `primary` 的 `sizeof expr` /
  `_Alignof expr`. Pratt 块不再引用任何旧函数; 旧函数集合为空时仍能编译 (证据).
- 深度: 只有 `expr_bp` 计数; `test/library.sh` 的 2000 层括号仍报 `nesting too deep`.
- 证据:
  - ast.sh 85 例两模式逐字节相同; 诊断 68 例两模式逐字节相同.
  - Docker: corpus 41 个文件的 AST 与 `-S` 相同; tinycc 10 个翻译单元相同, 两套测试通过.
  - 随机表达式 400 例 (二元/一元/cast/三元/comma/赋值/后缀/调用/下标/成员, 5 层) 两模式
    dump 相同, 0 差异.

## P5 - 全闸门

- 最终运行 (最新树, ast.sh 85 例, 含文档):
  - `make docker-test-both`: exit 0 (driver 修复后重跑); stage1 + stage2 的默认与 Pratt
    四轮各跑一遍 ast.sh 85 例 (a85 出现 4 次), 以及 test / test-pratt / test-stage2 /
    test-stage2-pratt 的全部脚本与 library.sh.
  - `make docker-test-pratt`: exit 0 (修复 `test/driver.sh` 之后重跑; 首次运行暴露了
    该脚本硬编码 `./chibicc`, 见下方 P5 发现).
  - `make docker-pratt-ab`: exit 0; corpus 41 个文件 AST 与 `-S` 相同; tinycc 10 个 TU /
    10 个 `.s` 相同, 两个构建各通过 `$make CC=cc test`; 墙钟 4:18.86 (`time`).
  - `make docker-snapshot-diff`: `snapshot diff: empty` (41 个 `.s`, 默认路径零变化).
  - `make check-library` (本机, 默认): exit 0; `install: ok` (`pkg-config` 缺失时跳过
    `.pc` 检查). `make check-library-pratt`: exit 0, public-api 9 个头 + D7 全在.
  - Docker 内 `make check-library check-library-pratt`: exit 0; 两个归档的 D7 检查通过,
    `libchibicc.so exports exactly D7`, `install: pkg-config ... -lchibicc | 0.1.0`,
    `install: ok`.
- P5 发现并修复: `test/driver.sh` 第 19 行的 `-o` 用例硬编码 `./chibicc`, 单独跑
  `test-pratt` 时 `./chibicc` 不存在而失败; `docker-test-both` 因为同时构建了默认
  `./chibicc` 而掩盖了它. 改为 `$chibicc`; `docker-test-pratt` 与 `docker-test-both`
  在修复后重跑绿.
- 判据 1-7 的对照见下节.

## 判据对照

| 判据 | 证据 |
|---|---|
| 1 默认路径零变化 | `docker-snapshot-diff` 空; 默认构建 diagnostic 68 / warning / shape 34 / trivia / cst / expand / library 全绿; `libchibicc.a` 公共面不变 (`check-library` 默认通过) |
| 2 AST 等价 | corpus 41 文件 `-dump-ast` 相同; tinycc 10 个 `$(CC)` TU 相同; ast.sh 85 例两模式相同并冻结; 400 随机表达式 0 差异; dump 在 sema 前早退 |
| 3 发射等价 | corpus 41 文件 `-S` 相同; tinycc 同组 TU `-S` 相同; 两个 tinycc 构建各自通过 `$make CC=cc test`; `docker-test-both` 含两模式自举 |
| 4 旧路径可回退 | 默认 `make` 仍编译旧级联; 旧函数内容未改, 只在保护块内; `make clean && make` 不依赖 Pratt 产物 |
| 5 忠实层不破坏 | `ND_ASSIGN.op` / `is_elvis` / `ND_INCDEC` / `arrow_tok` / `ND_CAST.ty_op` / `ND_SUBSCRIPT` / `ND_GT` / `ND_GE` / `ND_COMPOUND_LITERAL` 的字段在 ast.sh 与两模式 A/B 中锁定; 一元 + 仍不建节点; 表达式不建类型, 不绑名, 不求值; 诊断锚点由 diagnostic.sh 逐字节锁定 |
| 6 无新 exit / static | Pratt 块 grep 无 `exit(` 与静态变量; 深度计数走 `chibicc_ctx()->parse_depth`; 只有 `error_tok` |
| 7 形态锁先于重写 | P1 在 P3 之前落地并跑绿; P1 提交 86686a0 早于 P2/P3+P4 |

## 偏差与说明

1. **逗号结合性**: 计划 4.3 表写左结合 (`rbp = lbp`). 旧树的逗号是右嵌套 (`expr1` 的
   右操作数是 `expr`), AST 等价要求保留该形状. Pratt 取 `rbp = BP_COMMA - 1`; 语义不变
   (逗号求值仍是左到右), 但树与旧路径一致. 不采用计划的左结合, 因为判据 2 要求逐字节
   相同.
2. **`-dump-ast` 字段是计划的超集**: 除计划列出的字段外, 还打印声明记录的 `name=` /
   `attr=` / `align_ty=` / `align=expr`, 以及字符串字面量作为表达式的用例. 这些是
   parse 会写且对等价性有用的字段; 不打印 sema 私有字段 (`init_resolved`, `var`,
   `func_ty`, `member`, `atomic_addr`) 与 parse 阶段恒 NULL 的字段.
3. **ast.sh 85 例** (计划约 70).
4. **Pratt 接受两类旧路径拒绝或误读的输入**:
   - 复合字面量直接接后缀 (`(int[]){1,2}[0]`): 旧 postfix 在建完复合字面量后直接返回,
     报 `expected ';'`; Pratt 的 led 循环接受.
   - `++(int)x` (无花括号, 非法 C): 旧 postfix 把 `(int)` 误当复合字面量, 建
     `ND_COMPOUND_LITERAL`, `x` 作为初始化表达式; Pratt 按 C 语法建 `ND_CAST`.
   两者都只影响旧路径本就不能正确接受的源码; 41 个语料文件与 tinycc 的合法源码中没有
   这种形态, 判据 2/3 不受影响. 若未来要求"拒绝集合也一致", 需另开决策加限制.
5. **深度阈值偏移**: 只有 `expr_bp` 计数后, 纯括号嵌套上限从 101 变为 511, `!` 链从
   253 变为 511 (`-` 串实测 1014/1022, 但 `--` 是单个 token, 不能按字符数直接比较).
   `MAX_PARSE_DEPTH` 仍为 512, library.sh 的 2000 层用例仍报错; 未改断言.
6. **tinycc 翻译单元数**: 实测 10 个 `$(CC) -c` 命令的 TU, 不是计划里提到的源树 26 个
   `.c`. 差额来自 Makefile 的 `ONE_SOURCE` / 后端选择; `lib/*.c` 由刚构建的 tcc 编译,
   按计划由 tinycc 测试套件覆盖. 命令集合由 shim 捕获, 未手工枚举.
7. **P3/P4 合并为一个提交**: P3 是 P4 前的中间态 (nud 暂时调旧 `cast`). 两步都做了本机
   验证 (P3: 39 文件 AST/S 相同, ast.sh, 诊断; P4: 同上加 400 随机表达式), 但中间态
   没有单独提交; 最终提交 17b2838 是 P4 态.
8. **检查目标的扩展**: `test/public-api.sh` 新增 `CHIBICC_LIB` 支持, Makefile 新增
   `check-library-pratt`, 以便把公共面与 D7 检查套在 `pratt/libchibicc.a` 上 (P5.1 的
   "两模式"); `install.sh` 的布局检查与解析器无关, 只在默认 `check-library` 跑一次.
9. **`-dump-ast` 的 `@synth` 口径**: 凡 `tok->prov` 非空即打 `@synth`, 包括宏体 token
   与 `__LINE__` 等内建展开的合成缓冲. 这是"位置不是源码表达式位置"的保守口径; 宏展开
   的原始位置仍可由 provenance 通道查询.

## 维护规则

- 任何表达式层改动必须同时通过 `test-ab` (41 文件 AST/汇编) 与 `test-pratt` /
  `test-stage2-pratt`; 只改一个模式会在 A/B 或对应测试里立刻暴露.
- 默认 `make` 与 `libchibicc.a` 永远是旧路径; 新路径只在 `make chibicc-pratt` 与
  `pratt/` 产物里. 删除旧路径需要另开决策 (`PLAN-pratt-parse.md` 的待拍板项).
- 新增表达式运算符或节点形状时, 同步更新 `test/ast.sh` 的冻结输出与
  `test/pratt-ab.sh` 的语料比较; 诊断锚点变化必须同步 `test/diagnostic.sh`.
