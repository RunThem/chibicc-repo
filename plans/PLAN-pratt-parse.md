# PLAN-pratt-parse.md - 表达式 Pratt 解析线

> 状态: 已完成 (2026-10-21; P0-P6 全部完成, 归档于 `plans/`)
> 范围: `libchibicc/src/parse.c` 的表达式层; 语句层, 声明层, sema, codegen 不动
> 回退: `CHIBICC_PRATT_PARSE_FEATURE` 默认 0; 旧解析路径永久保留, 回退 = 用默认构建重编
> 终态提交: P1 `86686a0`, P2 `a716f97`, P3+P4 `17b2838`, P5+P6 见本条归档提交

## 1. 目标与边界

### 1.1 目标

把表达式解析从"每个优先级一个函数"的级联改为一个 Pratt 循环 (`expr_bp` + 绑定力), 覆盖**全部表达式运算符**: 前缀, 后缀, cast, `?:`, 赋值, 逗号, 复合字面量, 语句表达式, `_Generic` 的最小接入. 语句 (`stmt` / `compound_stmt`) 与声明 (`declspec` / `declarator` / `type_suffix`) 保持现有递归下降不变.

旧实现完整保留在同一个文件里, 由特性宏选择:

- `CHIBICC_PRATT_PARSE_FEATURE=0` (默认): 走旧级联, 行为与当前提交逐字节一致.
- `CHIBICC_PRATT_PARSE_FEATURE=1`: 走 Pratt, 目标是与默认构建的 AST 与汇编逐字节一致.

### 1.2 边界

- 只改 `parse.c` 的表达式区域 (当前 1230-2192 行, 963 行, 含 `primary`).
- 共享不动的函数: `new_node` / `new_binary` / `new_unary` / `new_num`, 作用域与 typedef/tag oracle, `declspec` / `declarator` 全家, `init_record` / `designators`, `struct_members`, `attribute_list`, `struct_ref`, `funcall`, `generic_selection`, `primary`, 语句层与顶层 `parse`.
- 不新增公共 API, 不改公共头. `CHIBICC_PRATT_PARSE_FEATURE` 只在内部 umbrella `libchibicc/src/chibicc.h` 给默认值.
- 不删旧代码. 本线任何步骤都不删除旧级联函数, 只在 `#if !CHIBICC_PRATT_PARSE_FEATURE` 下编译.

### 1.3 非目标

- 不做运行时开关 (`-pratt`). 用户已定: 特性宏 + 独立构建.
- 不把 Pratt 推广到语句或声明层.
- 不借机修改 sema / codegen / type / tokenize / preprocess / cst.
- 不改变任何既有消费者可见行为: 默认构建的 `-E`, 诊断, 汇编, trivia, CST, provenance 通道全部保持现状.

## 2. 判据 (完成定义)

**判据 1 - 默认路径零行为变化**
- `CHIBICC_PRATT_PARSE_FEATURE=0` 构建的 `-S` 输出与基线快照逐字节相同 (`.cache/snapshot`).
- `test/diagnostic.sh` 68 例, `test/warning.sh`, `test/shape.sh` 34 段, `test/trivia.sh`, `test/cst.sh`, `test/expand.sh`, `test/library.sh` 全部通过.
- 默认构建的二进制不新增导出符号; `libchibicc.a` 的公共面不变.

**判据 2 - Pratt 路径与默认路径 AST 等价**
- 对全部 41 个 `test/*.c`, 两个构建的 `-dump-ast` 输出逐字节相同.
- 对 tinycc 真实构建中由 `$(CC)` 执行的每一个 `-c` 翻译单元 (tcc 自身与 Makefile 选中的后端源文件), 两个构建的 `-dump-ast` 输出逐字节相同. 命令集合不手工枚举, 由 4.8 的 shim 从 tinycc 的实际构建中捕获. `lib/*.c` 由刚构建的 tcc 自行编译 (libtcc1.a), 不经过 `$(CC)`, 不在本项范围, 由测试套件覆盖.
- `test/ast.sh` 的全部表达式形态用例在两个构建下输出相同, 且逐字节锁定.
- AST dump 必须在 `chibicc_parse` 之后, `chibicc_sema` 之前早退; 不写目标文件, 不汇编.
- tinycc 的 AST 对比在 docker 内是必过的完成条件; 本机缺失系统头时只允许开发循环跳过, 不能作为验收证据.

**判据 3 - Pratt 路径与默认路径发射等价**
- 对全部 41 个 `test/*.c`, 两个构建的 `-S` 输出逐字节相同 (使用 `SNAPSHOT_DATEFLAGS` 固定时间宏).
- 对 tinycc 同一组由 `$(CC)` 执行的 `-c` 命令, 两个构建的 `-S` 输出逐字节相同.
- 两个构建都必须以 `test/thirdparty/tinycc.sh` 为基准编译 tinycc, 并通过其测试套件 (`$make CC=cc test`); A/B 时只把 configure 合并为一次 (4.8). tinycc 是必选项, 不存在"可选"或"跳过"分支, 任一构建失败即本线未完成.
- `make docker-test` 与 `make test-stage2` 在 Pratt 构建下同样全绿.

**判据 4 - 旧路径可随时回退**
- 默认 `make` 永远构建旧路径; `make chibicc-pratt` 才构建新路径.
- 两个路径的源码同时存在于 `parse.c`, 不依赖 git revert.
- 任一时刻 `make clean && make` 得到与改动前等价的编译器.

**判据 5 - 忠实层不被破坏**
- 所有既有节点形状逐项保留: `ND_ASSIGN.op` (11 个复合赋值), `ND_COND.is_elvis`, `ND_INCDEC` 的 `is_post` / `addend`, `ND_MEMBER.arrow_tok`, `ND_CAST.ty_op` / `is_implicit`, `ND_SUBSCRIPT`, `ND_GT` / `ND_GE`, `ND_INCDEC` 前缀/后缀, 一元 `+` 不加节点, `ND_COMPOUND_LITERAL` 的 `ty_op` / `spec_decls` / `decl_init`.
- 表达式树不带类型, 不绑名字, 不求值; 不做任何降级. Pratt 只改"如何组合", 不改"组合成什么".
- 诊断锚点不变: 二元节点锚运算符 token, `?:` 锚 `?`, 赋值锚赋值符, 复合赋值锚 `+=` 等; 由 `test/diagnostic.sh` 与 `test/ast.sh` 双重锁定.

**判据 6 - 不新增 `exit` 与静态状态**
- Pratt 实现不调用 `exit`, 不新增文件域 static; 深度计数复用 `chibicc_ctx()->parse_depth`.
- 新增的诊断路径只允许通过现有 `error_tok`.

**判据 7 - 形态锁先于重写**
- `-dump-ast` 与 `test/ast.sh` 必须在任何 Pratt 代码进入 `parse.c` 之前落地并跑绿 (P1).
- 没有形态锁的等价性声明不接受; 汇编与诊断只是间接证据.

## 3. 现状锚点 (2026-10-21 实测)

- `libchibicc/src/parse.c` 2332 行.
- 表达式区域 1230-2192 行, 963 行, 含 `primary`:
  - 级联: `expr` / `expr1` (1230-1246), `assign` / `assign1` (1248-1330), `conditional` / `conditional1` (1332-1365), `logor` 到 `mul` (1367-1545).
  - 前缀与 cast: `cast` / `cast1` (1547-1582), `unary` / `unary1` (1584-1644).
  - 后缀: `postfix` (1903-1976).
  - 共享原子: `funcall` (1983-2008), `generic_selection` (2010-2052), `primary` (2054-2192).
- `primary` 有两处调用 `unary`: `sizeof expr` (2086) 与 `_Alignof expr` (2103). 这是共享函数与新路径的唯一硬接口, 见 4.2.
- `conditional` 是跨文件符号: 声明在 `libchibicc/src/chibicc.h:175`, 调用点在 `sema.c:2704`.
- 深度闸门: 8 个 wrapper 各自 `++chibicc_ctx()->parse_depth`, 上限 `MAX_PARSE_DEPTH 512`; `test/library.sh:207` 的 `check_parse_depth` 用 2000 层括号断言"报诊断且不崩", 不锁具体阈值.
- 测试语料: 41 个 `test/*.c`; 诊断 68 例; 形状 34 段; 自举与库闸门齐全.
- tinycc 闸门: `test/thirdparty/tinycc.sh` 固定提交 `df67d8617b7d1d03a480a28f9f901848ffbfb7ec`, 流程是 `./configure --cc=$chibicc`, `$make clean`, `$make`, `$make CC=cc test`; 源树顶层 26 个 `.c`, 另有 `lib/` 的 10 个 `.c` 与后端源文件. `$(CC)` 只编译 tcc 自身与 Makefile 选中的后端; `lib/*.c` 由刚构建的 tcc 编译成 `libtcc1.a`. 完整 TU 集合由 Makefile 决定, 不手工枚举.
- `test/thirdparty/common` 把 `chibicc` 固定为仓库根的 `./chibicc`; 要换编译器必须覆盖变量或经 shim, 见 4.8.
- 当前没有 AST dump 通道: `-dump-tokens` / `-dump-cst` / `-dump-expansions` 都不输出 `Node`.
- `AGENTS.md` 的设计原则含"不用宏/高阶函数合并解析器里有意的重复"与"可读性优先于巧妙"; 本线需要一条显式豁免, 见 4.6.
- `AGENTS.md` 代码地图写 `parse.c` 2148 行, 与现状 2332 行不符; 本线 P5 一并更正.

## 4. 设计

### 4.1 特性宏与双构建

宏名 `CHIBICC_PRATT_PARSE_FEATURE`, 内部头给默认值:

```c
// libchibicc/src/chibicc.h
#ifndef CHIBICC_PRATT_PARSE_FEATURE
#define CHIBICC_PRATT_PARSE_FEATURE 0
#endif
```

构建:

- 默认 `make` 不定义宏, `parse.c` 走旧路径.
- `make chibicc-pratt` 用 out-of-tree 对象 `pratt/**/*.o` 加 `-DCHIBICC_PRATT_PARSE_FEATURE=1`, 产出 `./chibicc-pratt`; 不覆盖 `./chibicc`, 不覆盖 `libchibicc.a`.
- 两套对象目录相互独立: `pratt/` 与 `stage2-pratt/` 的源规则与现有 `stage2/` 同构.
- `BOOT` 拷贝排除 `pratt` 与 `stage2-pratt`, 与 `stage2` 的处理一致.

理由: 原地切换宏会留下陈旧对象 (Makefile 不追踪 CFLAGS), 且会破坏默认构建; out-of-tree 是仓库已有的 `stage2/` 先例.

### 4.2 代码布局与保护块

`parse.c` 里三段保护块, 旧代码不动内容, 只加 `#if`:

1. 旧级联 `expr` 到 `unary1` (当前 1230-1644):
   `#if !CHIBICC_PRATT_PARSE_FEATURE` ... `#endif`.
   这一段包含 `expr` / `assign` / `conditional` 三个入口的旧定义; Pratt 模式下由新块提供同名 wrapper.
2. 旧 `postfix` (当前 1903-1976): `#if !CHIBICC_PRATT_PARSE_FEATURE` ... `#endif`.
3. 新 Pratt 实现: 放在 `primary` 之后, `parse_typedef` 之前 (当前 2192 附近), 整块 `#if CHIBICC_PRATT_PARSE_FEATURE` ... `#endif`.

共享函数 (`struct_members`, `attribute_list`, `struct_union_decl`, `struct_ref`, `funcall`, `generic_selection`, `primary`) 在两个保护块之间, 不属于任何保护块, 两个构建都编译.

关键接口:

- `primary` 的 `sizeof expr` / `_Alignof expr` 调用 `unary`. Pratt 模式下 `unary` 是一个薄 wrapper:
  `static Node *unary(Token **rest, Token *tok) { return expr_bp(rest, tok, BP_CAST); }`.
  旧模式下 `unary` 仍是原来的深度 wrapper. 两边的语义都等于"解析 cast 层操作数".
- 顶部已有的 `expr` / `assign` / `conditional` / `primary` / `funcall` 原型在两个模式下都保留. 旧模式里未被调用的 static 原型不产生 `-Wunused-function`; 不定义也不使用即可.
- `cast` / `postfix` 只在旧模式定义. `ND_COMPOUND_LITERAL` 的构造在新模式下由 Pratt nud 承担, 不复制 `postfix` 的其余分支.

### 4.3 绑定力与入口

循环约定: `while (lbp(tok) > min_bp)`, 左结合 rbp = lbp, 右结合 rbp = lbp - 1.

| 层 | 运算符 | lbp | 右操作数 min_bp | 结合 |
|---|---|---|---|---|
| 1 | `,` | 1 | 1 | 左 |
| 2 | `=` 与 11 个复合赋值 | 2 | 1 | 右 |
| 3 | `?:` | 3 | 2 (else 分支) | 右 |
| 4 | `\|\|` | 4 | 4 | 左 |
| 5 | `&&` | 5 | 5 | 左 |
| 6 | `\|` | 6 | 6 | 左 |
| 7 | `^` | 7 | 7 | 左 |
| 8 | `&` | 8 | 8 | 左 |
| 9 | `==` `!=` | 9 | 9 | 左 |
| 10 | `<` `<=` `>` `>=` | 10 | 10 | 左 |
| 11 | `<<` `>>` | 11 | 11 | 左 |
| 12 | `+` `-` | 12 | 12 | 左 |
| 13 | `*` `/` `%` | 13 | 13 | 左 |
| - | cast 前缀 | - | 14 | - |
| - | 一元前缀操作数 | - | 14 | - |
| - | 后缀 `(` `[` `.` `->` `++` `--` | 15 | - | - |

入口即三个 `min_bp`:

- `expr` = `expr_bp(0)`: 允许逗号.
- `assign` = `expr_bp(1)`: 允许赋值, 不允许逗号.
- `conditional` = `expr_bp(2)`: 允许 `?:`, 不允许赋值与逗号.

这与现有调用点分布一致: 条件, `for` 三段, `typeof` 操作数, 括号, 语句表达式, 下标用 `expr`; 初始化器, 实参, `_Generic` 关联, `atomic_*` 内建用 `assign`.

`?:` 中间分支用 `expr_bp(0)` (允许逗号), else 分支用 `expr_bp(2)` (右结合), elvis `a ?: b` 的 els 同样用 `expr_bp(2)`.

### 4.4 特殊形式清单

Pratt 实现按下面顺序处理, 每一项都有对应的形态锁用例:

1. **复合赋值**: 映射 `+=` -> `ND_ADD`, `-=` -> `ND_SUB`, `*=` -> `ND_MUL`, `/=` -> `ND_DIV`, `%=` -> `ND_MOD`, `&=` -> `ND_BITAND`, `|=` -> `ND_BITOR`, `^=` -> `ND_BITXOR`, `<<=` -> `ND_SHL`, `>>=` -> `ND_SHR`; 节点是 `ND_ASSIGN` 加 `op`, 锚点是运算符 token; 不许改写成 `a = a + b`.
2. **`?:` 与 elvis**: `ND_COND` 的 `cond` / `then` / `els`; `a ?: b` 只有 `cond` / `els` 且 `is_elvis = true`; 锚点 `?`.
3. **前缀 `++` / `--`**: `ND_INCDEC`, `is_post = false`, `addend = +1 / -1`; 操作数 `expr_bp(14)`, 与旧 `unary(rest, tok->next)` 等价 (因此 `++x++` 仍解析为 `++(x++)`).
4. **后缀 `++` / `--`**: `ND_INCDEC`, `is_post = true`; 在 led 循环里处理, 与旧 `postfix` 一致.
5. **`&&label`**: GNU labels-as-values, 是 `ND_LABEL_VAL`, 不是二元 `&&`; nud 先于任何 infix 判断.
6. **一元 `+`**: 透明, 不建节点, 直接返回 cast 层操作数.
7. **`(type)expr` / `(expr)` / `(type){...}`**: nud 见 `(` 时先 `is_typename(tok->next)`; 真则解析 `typename`; 后随 `{` 走 `init_record` 构造 `ND_COMPOUND_LITERAL` (带 `ty_op` / `spec_decls` / `decl_init`), 否则构造 `ND_CAST` (带 `ty_op`, `spec_decls`, `is_implicit = false`) 且操作数 `expr_bp(14)`; 假则交给 `primary` 处理括号与语句表达式.
8. **`sizeof` / `_Alignof`**: 两种形式留在共享 `primary`; 表达式形式经 `unary` wrapper 进入 Pratt.
9. **语句表达式 `({...})`, `_Generic`, `__builtin_*`**: 留在共享 `primary` / `generic_selection`, 不重写.
10. **后缀链**: 调用走共享 `funcall` (实参 `assign`), 下标构造 `ND_SUBSCRIPT` (索引 `expr`), `.` / `->` 走共享 `struct_ref` (保留 `arrow_tok`), 复合字面量之后仍可接后缀.
11. **`>` / `>=`**: 保持 `ND_GT` / `ND_GE`, 不在 parser 里换序.
12. **token 锚点**: 每个节点用旧路径同一个 token (二元用运算符 token, `?:` 用 `?`, 赋值用赋值符); AST dump 与诊断双重验证.
13. **不降级**: 逗号是 `ND_COMMA`, 赋值是 `ND_ASSIGN` + `op`, 自增自减是 `ND_INCDEC`, `x[y]` 是 `ND_SUBSCRIPT`.

### 4.5 `-dump-ast` 形态锁

驱动只读通道, 放在 `main.c`, 不进库公共面:

- 旗标 `-dump-ast`, 解析位置与 `-dump-expansions` 同级; `cc1()` 在 `chibicc_parse` 之后, `chibicc_sema` 之前早退.
- 输出逐行确定性文本, 不含地址, 不含 hashmap 顺序:

  ```
  <indent><Kind> @<file>:<line>:<col> "<spelling>" [fields]
  ```

  合成位置打 `@synth`. 拼写做 `\n` `\t` `\\` `\"` 转义.
- 字段只打印 parse 阶段会写的: `op=<Kind>` (ND_ASSIGN), `is_post=1` / `addend=-1` (ND_INCDEC), `is_elvis=1` (ND_COND), `arrow=1` (ND_MEMBER), `label=<name>` (ND_GOTO / ND_LABEL / ND_LABEL_VAL), `is_scope_block=1` (ND_BLOCK), `is_implicit=1` (ND_CAST), `ty_op=<类型签名>` / `ty_op2=<类型签名>` (只打 kind 与 base 链, 不打地址与 size), `val=<十进制>` / `fval=<%La>` (字面量), `asm_str` (ND_ASM).
- 子节点按固定顺序遍历: `lhs` `rhs` `cond` `then` `els` `init` `inc` `body` `args` `spec_decls` `begin_expr` `end_expr` `cas_addr` `cas_old` `cas_new`; 列表 (`next` 链) 由父级以兄弟行的方式展开.
- 初始化器单独 dump: `INIT kind=LIST|EXPR|STR @tok`, 列表项 (`items`) 带 `comma_tok`, 指示符链 (`designators`) 带 `begin` / `end` 表达式, 标量初始化器递归 dump `expr`, 字符串初始化器打印 `str_tok` 拼写. `ND_DECL` 的 `decl_init` 与 `ND_COMPOUND_LITERAL` 的 `decl_init` 都覆盖.
- 跳过 sema 私有字段 (`init_resolved`, `var`, `func_ty`, `member`, `atomic_addr`) 与 parse 阶段恒 NULL 的字段; 它们不属于第 3 层形态.

`test/ast.sh`:

- 用法 `test/ast.sh ./chibicc`, 由 `make test` 与 `make test-stage2` 调用.
- 沿用 `test/diagnostic.sh` 的 `snippet <name>` / `expect <name>` heredoc 风格, 逐字节 diff stdout.
- 用例约 70 个, 覆盖: 全部二元运算符; 11 个复合赋值; 左/右结合 (`a-b-c`, `a=b=c`, `a?b:c?d:e`); 逗号在语句, 括号, 实参, 初始化器中的区别; `?:` 中间逗号; elvis; 前缀/后缀自增自减; 一元 `+` 透明; 嵌套 cast; `sizeof` 两种形式; `_Alignof` 两种形式; 复合字面量加后缀; 语句表达式; `_Generic`; `&&label`; `f((a,b), c)` 与 `f(a,b)`; 下标; `.` 与 `->`; 数组与指定初始化器; 一个宏展开表达式的 `@synth` 锚点.
- 另加 3 个"只验 dump 不验语义"的用例: 语义非法的表达式 (如 `1 + (struct S){0}`) 仍能在 sema 之前 dump, 证明通道与语义解耦.

### 4.6 深度闸门

- 旧模式不变: 8 个 wrapper 继续计数.
- 新模式: 只有 `expr_bp` 计数 (`++` / `--` 成对, 单出口), 入口 wrapper 不重复计数; `unary` 薄 wrapper 不计数.
- `MAX_PARSE_DEPTH 512` 不变. 嵌套括号仍会经 nud `(` -> `primary` -> `expr` -> `expr_bp` 递归计数, `test/library.sh:207` 的 2000 层用例仍报 `nesting too deep`.
- 阈值行为相对旧模式会偏移 (左结合链原本就不递归, 变化的是每个操作数经过的固定帧数), 记录在 RESULT 的偏差一节, 不改 `library.sh` 的断言.

### 4.7 规则豁免 (需用户拍板)

`AGENTS.md` 的"不用宏/高阶函数合并解析器里有意的重复"与本线冲突. 建议的豁免文本:

> 表达式层允许一个绑定力表加一个 `expr_bp` 循环 (`parse.c`, `CHIBICC_PRATT_PARSE_FEATURE`). 分派使用 `if` / `switch` 与具名 `BP_*` 常量, 不使用函数指针数组; 语句与声明层继续沿用每产生式一个函数的递归下降. 旧级联保留在 `#if !CHIBICC_PRATT_PARSE_FEATURE` 下, 两个模式必须同时通过全部闸门.

理由是 Pratt 的核心收益是"优先级成为数据, 组合只有一个循环"; 用 `switch` + 常量而不是回调表, 保留可读性与可断点性, 与仓库风格不冲突.

### 4.8 tinycc A/B (必选)

tinycc 是本线唯一必选的真实项目闸门, 同时覆盖 AST 与发射. 做法不是手工枚举源文件, 而是让 shim 捕获 tinycc 构建实际执行的每一条 `-c` 命令:

1. `test/pratt-ab.sh <old> <new> --thirdparty tinycc` 在 `thirdparty/tinycc` 的钉住提交上工作; clone 与缓存规则沿用 `test/thirdparty/common`.
2. 只运行一次 `./configure --cc=<old>`, 生成同一份 `config.mak`; 两次构建都只跑 `$make clean` 与 `$make CC=<shim>`, 避免 configure 的编译器探测产生不确定输入.
3. shim 是临时目录里的 `chibicc` 外壳: 原命令原样转发; 若命令含 `-c`, 另外用同一组参数 (去掉 `-c`, `-o` 与依赖生成 flag, 追加 `SNAPSHOT_DATEFLAGS`) 运行 `-dump-ast` 与 `-S`, 产物写入按归一化参数哈希命名的日志文件.
4. 两个构建的日志目录用 `diff -ru` 比较, 必须为空: `.ast` 对应判据 2, `.s` 对应判据 3.
5. 每个构建完成后跑 `$make CC=cc test`; 两个构建的 tinycc 测试套件都必须通过, 对应判据 3.
6. 本目标只在 docker 内跑 (x86-64 Linux, 系统头, TinyCC 测试套件); 首次需要网络 clone, 之后复用 `thirdparty/tinycc` 缓存.

约定:

- 辅助运行不产生 `.o` / `.d`, 不写构建目录; 所有日志在临时目录, 结束后删除.
- 路径锚点 (`.file` / `.loc`, AST dump 的 `@file:line:col`) 在同一个构建目录内两次运行一致; 不把临时绝对路径带进日志.
- shim 只拦截 `-c`; 链接与 configure 探测原样转发.
- 日志的 key 由源文件相对路径与归一化参数决定, 与执行顺序无关; 两次构建必须产生相同的 key 集合.
- `libtcc1.a` 的 `lib/*.c` 由刚构建的 tcc 自行编译, 不经过 `$(CC)`, 因此不在 AST/汇编日志里; 它们由第 5 步的测试套件覆盖.

## 5. 步骤

### P0 - 基线与开工准备

- [x] P0.1 在 `main` 上确认三道闸门全绿: `make docker-test`, `make docker-snapshot-diff` (无基线时先 `make docker-snapshot`), `make test`.
- [x] P0.2 记录锚点: `wc -l libchibicc/src/parse.c`, 表达式区域行号, `git rev-parse HEAD`, 41 个 `test/*.c` 的清单.
- [x] P0.3 开分支 `pratt-parse`; 之后每一步一个提交, 提交内两模式都必须可构建.

验收: 基线绿, 锚点写入本文件 "现状锚点" 的执行记录.

### P1 - AST 形态锁 (不改 parse)

- [x] P1.1 `main.c`: `opt_dump_ast` 旗标, `parse_args` 分支, `print_ast_dump` 与 `Initializer` dump, `cc1()` 在 parse 之后 sema 之前早退.
- [x] P1.2 定义 dump 语法 (4.5) 并实现类型签名与转义.
- [x] P1.3 `test/ast.sh` 约 70 例, 生成默认构建的输出, 逐行人工复核后冻结; 接入 `make test` 与 `make test-stage2`.
- [x] P1.4 `AGENTS.md` 的只读通道清单加 `-dump-ast`.

验收: 默认构建 `-S` 快照 diff 为空; `make test` 绿; `-dump-ast` 覆盖所有 parse 产生的 `NodeKind` 与全部初始化器形态; dump 在语义非法输入上仍可用.

### P2 - 宏与双构建 (代码路径尚未分叉)

- [x] P2.1 `libchibicc/src/chibicc.h` 给 `CHIBICC_PRATT_PARSE_FEATURE` 默认 0.
- [x] P2.2 Makefile: `pratt/libchibicc.a`, `chibicc-pratt`, `pratt/test/%.exe`, `test-pratt`, `test-both`, `test-ab`, `stage2-pratt/chibicc`, `test-stage2-pratt`.
- [x] P2.3 `test/pratt-ab.sh <old> <new>`: 对 41 个 `test/*.c` 比较 `-dump-ast` 与 `-S`; 时间宏用 `SNAPSHOT_DATEFLAGS`; 失败时打印首个 diff 与文件名. 同脚本支持 `--thirdparty tinycc` 模式 (4.8): shim 捕获真实 `-c` 命令, 比较 AST 与 `-S`, 并在每个构建后跑 tinycc 测试套件.
- [x] P2.4 `test/library.sh` 支持 `CHIBICC_PRATT_PARSE_FEATURE=1`: 给宿主编译与 ASan 重编加 `-DCHIBICC_PRATT_PARSE_FEATURE=1`, 让 `CHIBICC_LIB=pratt/libchibicc.a` 可测.
- [x] P2.5 Docker 目标: `docker-test-both`, `docker-test-pratt`, `docker-pratt-ab` (含 tinycc A/B 与测试套件), `docker-pratt-tinycc`; `BOOT` 排除 `pratt` 与 `stage2-pratt`.

验收: 未改任何解析代码时 `test-ab` 两个构建的全部输出逐字节相同; 两个模式的 `test` / `test-stage2` / 库闸门都绿.

### P3 - Pratt 接管二元层 (逗号, 赋值, `?:`, 全部二元)

- [x] P3.1 具名常量 `BP_COMMA` 到 `BP_POSTFIX`, `infix_bp(tok, &op, &lbp, &rbp)` 与 `expr_bp(rest, tok, min_bp)`.
- [x] P3.2 nud 暂时调用旧 `cast`; 三个入口 wrapper (`expr` / `assign` / `conditional`) 在 Pratt 模式下改调 `expr_bp`; 旧 `expr` 到 `mul` 加 `#if !CHIBICC_PRATT_PARSE_FEATURE`.
- [x] P3.3 保持旧 `cast` / `unary` / `postfix` 两个模式都编译, 避免一次改太多.
- [x] P3.4 跑 `test-ab` 与 `ast.sh`; 逐个修掉 AST 差异 (预期为零).

验收: 判据 2 与判据 3 在二元层范围内成立; 诊断 68 例两模式全绿.

### P4 - Pratt 接管前缀, 后缀与 cast

- [x] P4.1 nud: `+` `-` `*` `&` `!` `~`, 前缀 `++` / `--`, `&&label`, `(type)` cast, 复合字面量; `primary` 继续作为原子解析器.
- [x] P4.2 led: 调用, 下标, `.` / `->`, 后缀 `++` / `--`; 复用 `funcall` 与 `struct_ref`.
- [x] P4.3 Pratt 模式定义薄 `unary` (`expr_bp(..., BP_CAST)`) 供 `primary` 的 `sizeof expr` / `_Alignof expr` 使用; 旧 `cast` / `unary` / `postfix` 加 `#if !CHIBICC_PRATT_PARSE_FEATURE`.
- [x] P4.4 去掉 P3 的临时共享, 确认 Pratt 模式没有对旧函数的引用 (`grep` + 编译告警).

验收: 判据 2, 3, 5 全量成立; 形态锁覆盖的特殊形式全部在两模式一致; 深度闸门用例通过.

### P5 - 全闸门与文档

- [x] P5.1 `make docker-test-both`, `make docker-test-pratt` 含自举; `make docker-pratt-ab` 必须包含 tinycc 的 AST/汇编 A/B 与测试套件; `make docker-snapshot-diff` (默认路径不变); `make check-library` 两模式.
- [x] P5.2 tinycc 必选: 两个构建各自编译 tinycc, 各自通过 `$make CC=cc test`; 记录 AST/汇编 diff 计数与耗时. 不存在可选分支.
- [x] P5.3 `AGENTS.md`: 构建与测试加新目标与宏; 代码地图写 parse.c 双路径与 2332 行; 设计原则加 4.7 的豁免; 测试约定加 `ast.sh` / `pratt-ab.sh`; 只读通道清单加 `-dump-ast`; 路线图加本线.
- [x] P5.4 `RESULT-pratt-parse.md`: 每步实测, AST/汇编 diff 计数, 深度阈值偏差, 与计划的偏离.

验收: 判据 1 到 7 全部有实测证据.

### P6 - 收尾 (不改默认)

- [x] P6.1 代码审读: 保护块边界, 共享函数归属, 未使用原型, `-Wall` 零新告警.
- [x] P6.2 默认仍是 0; 记录"旧路径是否永久保留"的现状 (是), 以及未来删除需另开决策.
- [x] P6.3 把 `PLAN-pratt-parse.md` 与 `RESULT-pratt-parse.md` 按仓库惯例归档到 `plans/` (本文件按用户要求先放根目录).

验收: 全部闸门绿; 文档与代码一致; 不留下临时脚本与未使用目标.

## 6. 闸门矩阵

| 闸门 | 默认 (0) | Pratt (1) |
|---|---|---|
| `make test` | 必须绿 | `make test-pratt` 必须绿 |
| `make test-stage2` | 必须绿 | `make test-stage2-pratt` 必须绿 |
| `make docker-test` | 必须绿 | `make docker-test-pratt` 必须绿 |
| `-S` 快照 diff | 对基线为空 | `test-ab` 与默认构建为空 |
| `-dump-ast` | `test/ast.sh` 锁定 | `test/ast.sh` + `test-ab` 与默认构建一致 |
| tinycc `-c` 的 `-dump-ast` | 不适用 | 与默认构建逐字节相同 |
| tinycc `-c` 的 `-S` | 不适用 | 与默认构建逐字节相同 |
| tinycc 测试套件 (`$make CC=cc test`) | 必须绿 | 必须绿 |
| 诊断 68 例 | 必须绿 | 必须绿 |
| `shape.sh` / `trivia.sh` / `cst.sh` / `expand.sh` | 必须绿 | 必须绿 |
| `test/library.sh` | 必须绿 | `CHIBICC_PRATT_PARSE_FEATURE=1` 必须绿 |
| `check-library` | 必须绿 | `pratt/libchibicc.a` 可用 |

本地开发循环 (macOS): `make && make chibicc-pratt && test/ast.sh ./chibicc && test/ast.sh ./chibicc-pratt`; 全语料 A/B 与运行测试必须在 Docker 内.

## 7. 回退与维护

- 回退: 默认构建就是旧路径; 不需要 git 操作. `make clean && make` 得到与改动前等价的二进制.
- 移除本线: 删除 Pratt 块与两处 `#if`, 恢复旧函数可见性即可; 旧代码内容从未被修改, 因此回退风险低.
- 维护规则 (写入 AGENTS.md): 任何表达式层改动必须同时通过两个模式; 只改一个模式会在 `test-ab` 或 `test-pratt` 立刻暴露.
- 双实现的成本是真实的: 旧路径在默认构建里持续被测试, 新路径在 Pratt 构建里持续被测试; 不接受"只测默认"的提交.

## 8. 风险与对策

| 风险 | 对策 |
|---|---|
| 两套实现漂移 | 每步跑 `test-ab`; 两个模式都进 `test-all` 与 Docker 闸门 |
| AST 差异不可见 | P1 先落 `-dump-ast`; 字段级覆盖含初始化器 |
| 共享 `primary` / `unary` 接口处理错 | 明确 `unary` 薄 wrapper; P4 的验收含 `sizeof expr` 与 `_Alignof expr` 用例 |
| 诊断锚点漂移 | `diagnostic.sh` 逐字节 + `ast.sh` 锚点字段 |
| 深度阈值变化 | 记录偏差; `library.sh:207` 只断言失败不崩 |
| 宏展开 token 的锚点不稳定 | dump 支持 `@synth`; 用一个宏表达式用例锁定 |
| tinycc clone 首次需要网络或缓存损坏 | 复用 `thirdparty/tinycc`; 损坏时删除子目录重拉; 提交钉住 `df67d86` |
| configure 探测不确定或两次构建的命令集不同 | 只 configure 一次, 两次共用 `config.mak`; shim 日志的 key 集合必须相同 |
| tinycc A/B 耗时 | 单次构建约 15 s 量级, A/B 叠加 `-dump-ast` 与 `-S` 后仍为分钟级; 接受 |
| 构建产物互相污染 | `pratt/` 与 `stage2-pratt/` 独立目录; `BOOT` 排除 |
| 规则冲突 | 4.7 的显式豁免, 由用户拍板后写入 AGENTS.md |

## 9. 待拍板与已决

已决 (2026-10-21, 用户):

- 采用**全表达式** Pratt, 语句与声明保持递归下降.
- 旧解析代码必须保留; 新代码通过特性宏开启, 可随时回退.
- 计划文件先放仓库根目录.
- 编译宏名 `CHIBICC_PRATT_PARSE_FEATURE`, 默认 0 (未定义即旧路径).

待拍板:

- 4.7 的 AGENTS.md 豁免文本是否按建议写入.
- `-dump-ast` 长期只做驱动通道, 还是以后提升为库公共 API.
- `make test` 是否保持只测默认路径 (`test-both` 作为闸门目标), 还是默认就跑双模式.
- 深度阈值偏移是否接受 (建议接受, 只记录).
- 旧路径永久保留, 还是 Pratt 稳定一段时间后另开线删除.

执行时采用 (2026-10-21, 按本计划的建议默认; 仍是可推翻的产品决策):

- 4.7 的豁免文本已写入 `AGENTS.md` 的设计原则.
- `-dump-ast` 只做驱动通道, 不进公共头.
- `make test` 只测默认路径; `test-both` / `docker-test-both` 是双模式闸门.
- 深度偏移接受并记录在 `RESULT-pratt-parse.md` 的偏差 5.
- 旧路径永久保留; 删除需另开线决策.

2026-10-21 follow-up (用户拍板): 两套实现的逗号都改为左结合, 即 `a, b, c` 建
`(a, b), c`. 旧 `expr1` 改为循环, Pratt 的 `rbp = BP_COMMA`; 默认构建的
`-dump-ast` 逗号形状随之改变, 汇编只移动了一处 `.loc` 行 (ndiff 为空), 基线
按 Makefile 说明重置. `test/ast.sh` 的 a79 已重新冻结. 详见
`RESULT-pratt-parse.md` 的 follow-up 一节.

2026-10-21 follow-up 2 (用户): `infix_bp` 的 if 链改为只读 `InfixOp` 绑定力表加
线性查找, 前缀/后缀仍是 `if`, 不使用函数指针数组. 行为与 AST 不变, 默认快照仍为空;
详见 `RESULT-pratt-parse.md`.

## 10. 记录

- 本文件是执行计划; 开工后每步的实测, diff 计数与偏差写入 `RESULT-pratt-parse.md`.
- 完成后按仓库惯例归档到 `plans/`, 并在 `plans/PLAN.md` 的路线图与本文件的状态行标注终态提交.
