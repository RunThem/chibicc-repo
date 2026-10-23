# PLAN-explicit-context.md - 显式 ChibiccContext 传参线

> 状态: 已拍板, 待开工 (计划书 2026-10-23)
> 基线: `main` @ `0138b5a`
> 分支: `explicit-context`
> 范围: `libchibicc/` 内部函数与 `codegen.c` 的隐式上下文; 公共 API, 公共头与
>        D7 不变
> 回退: 每步一个提交; E10.1-E10.6 (删除 TLS 与访问器) 作为一个提交, E10.7
>        (const 硬化) 单独; 两者都可单独 revert
> 目标: 删除 `chibicc_active_ctx` / `chibicc_ctx()` / `chibicc_context_activate`,
>        每个触碰 context 的内部函数把 `ChibiccContext *ctx` 作为首参

## 1. 目标与边界

### 1.1 目标

把库内部的"当前 context"从线程局部指针改为显式参数. 本线结束时:

- `libchibicc/` 里没有任何 `_Thread_local` / `__thread`, 没有全局或文件域的 context
  指针, 没有 `chibicc_ctx()` 访问器.
- 每个直接或传递地触碰 arena, 诊断, `format`, `strarray_push` / `nodearray_push`,
  hashmap 变更, 或 `ChibiccContext` 字段的函数, 都以 `ChibiccContext *ctx` 为首参.
- 公共 API (`<chibicc/chibicc.h>` 的 D7 函数, 公共头, `.so` 导出) 逐字节不变; 变化只
  发生在库内部函数与内部 umbrella `libchibicc/src/chibicc.h`.
- `codegen.c` 也没有 context 指针: `codegen(ctx, ...)` 把 `ctx` 显式传给每个需要
  库构造器 / 诊断 / `format` 的发射函数及其调用链, 不保留 `cg_ctx`.
- 既有行为逐字节不变: `-E`, 诊断, 汇编, AST / CST / trivia / provenance 通道全部与
  基线相同.
- 线程契约不变, 但从"TLS 保证"变成"结构保证": 不同 context 可以在不同线程并存,
  同一 context 不跨线程并发; 诊断回调里嵌套使用另一个 context (包括创建, 配置, 释放)
  不再可能扰动外层调用.

### 1.2 边界

本线允许改:

- `libchibicc/src/*.c` 的所有内部函数签名, 调用点与内部头原型.
- `libchibicc/src/chibicc.h` 的内部声明 (删除 `chibicc_ctx` / `chibicc_active_ctx` /
  `chibicc_context_activate`, 给内部函数加 `ctx` 首参).
- `main.c` / `driver.h` / `codegen.c`: 驱动器把已有的全局 `ctx` 显式传给库函数;
  `codegen()` 增加 `ctx` 首参, 并把它穿透到全部需要库构造器 / 诊断 / `format` 的
  发射函数与调用链, 不保留 `cg_ctx`.
- `test/self/self-test.c`: 直接调用内部符号, 改为显式传 `ctx`.
- `test/library.sh`: 新增嵌套 context 回归用例; 现有用例语义不变.
- 文档: `AGENTS.md`, `docs/01-overview.md`, `docs/02-architecture.md` 2.7,
  `docs/10-build-install.md`, `docs/11-limitations.md` 11.4, `context.h` 的注释.

本线不做:

- 不改公共头里的任何函数签名, 不改 `Chibicc*` 类型, 不改 D7 清单.
- 不改任何语义, 不改 codegen 的发射形状, 不做顺手的重命名或优化.
- 不引入对象携带的 context (`File` / `HashMap` / `StringArray` 存 `ctx`), 不引入
  per-phase 状态结构; 唯一形态是首参.
- 不把 `provenance.c` / `unicode.c` 里不分配的只读函数改成带 `ctx` (见 4.1).
- 不处理错误模型本身 (`chibicc_context_define` 在无 jmp frame 时仍 `exit(1)`); 这是
  独立问题, 本线只保持现状.

### 1.3 非目标

- 不做 L5 formatter / C→C 打印器.
- 不删除 Pratt 旧表达式路径; 两条路径都要转换并通过 A/B.
- 不清理 codegen 的非 context 文件域状态 (`output_file` / `depth` / `current_fn` /
  cg 表); 它们不是 context 指针, 与本线无关.
- 不改变"一个 context 一个翻译单元"的粒度.

## 2. 判据 (完成定义)

**判据 1 - 库层零隐式上下文**
- `rg -n 'chibicc_ctx\s*\(|chibicc_active_ctx|chibicc_context_activate' libchibicc/src
  libchibicc/include` 为空.
- `rg -n 'chibicc_ctx\s*\(' libchibicc/src libchibicc/include codegen.c main.c` 为空;
  codegen 与驱动器不保留任何等价的 context 访问器, 包括 `cg_ctx`.
- `rg -n '\b_Thread_local\b|\b__thread\b' libchibicc/src libchibicc/include` 为空.
- `nm libchibicc.a` 不再出现 `chibicc_ctx`, `chibicc_active_ctx`,
  `chibicc_context_activate` 三个符号 (Darwin 去掉前导下划线后比对); Pratt 构建
  对 `pratt/libchibicc.a` 做同样检查.
- `test/context-audit.sh` (新增, 独立目标) 把以上各项变成永久闸门.

**判据 2 - 行为逐字节不变**
- `make docker-snapshot-diff` 为空 (全部 `test/*.c` 的 `-S`).
- `test/diagnostic.sh` 68 例, `test/warning.sh`, `test/shape.sh` 34 段,
  `test/ast.sh` 85 例, `test/trivia.sh`, `test/cst.sh` 305 项 / 61 个 dump 样本,
  `test/expand.sh` 29 + 3 + 17 + 48 个样本, 全部通过且输出与基线逐字节相同.
- `make docker-test` (含自举) 与 `make test-stage2` 全绿.
- 每一步提交单独可构建, 且本步涉及的闸门为绿; 行为变化不允许发生在步骤内部.

**判据 3 - 公共面不变**
- D7 清单, 公共头, `.so` 导出与基线一致; `test/public-api.sh` 与 `test/install.sh`
  全绿.
- `.a` 的内部符号签名变化属于 L3.2b 已拍板的策略 (内部符号可见但不得调用),
  `RESULT-explicit-context.md` 记录受影响的符号清单.
- `chibicc` 二进制仍只链接 `libchibicc.a`, 自举产物可运行.

**判据 4 - 线程与嵌套语义**
- `test/library.sh` 的跨线程双 context (`check_threads`) 通过; 此时它证明的是"没有
  共享状态", 不再依赖 TLS.
- 新增 `check_context_scope`: 诊断回调里执行 `chibicc_context_new` +
  `chibicc_context_define` + `chibicc_context_free`, 外层错误仍展开到自己的 frame,
  外层 context 随后可继续跑完整管线.
- ASan + UBSan 轮零 UB, reset 活字节闸门不增长.

**判据 5 - 自举与 Pratt 双路径**
- stage2 用 chibicc 重编的库对象 (`stage2/libchibicc.a`) 通过 `test/library.sh` 与
  `test/self-test.sh`.
- Pratt 路径 (`CHIBICC_PRATT_PARSE_FEATURE=1`) 与默认路径的 `-dump-ast` 和 `-S` 对
  41 个 `test/*.c` 逐字节相同; `make docker-pratt-ab` 的 tinycc A/B 与测试套件通过.
- tinycc 的 `test/thirdparty/tinycc.sh` 构建与测试套件通过, 且是硬性收尾闸门,
  任一失败即本线未完成.
- 默认与 Pratt 两个构建的 `test/ast.sh` 输出相同.

**判据 6 - 可审计的机械性**
- 每一步只做签名, 参数与调用点的机械改动; 不做逻辑重写, 不改控制流, 不改表达式.
- 每步提交用 `git diff -w` 审读; 非机械差异必须在 `RESULT` 里逐条解释.
- 中间状态允许未转换的函数继续用 `chibicc_ctx()`; 一个文件被宣布"转换完成"后,
  该文件内 `chibicc_ctx()` 必须为 0.

## 3. 现状锚点 (2026-10-23 实测, `main` @ `0138b5a`)

### 3.1 规模

- 库源码: `cst.c` 2393, `hashmap.c` 145, `lib.c` 473, `parse.c` 2654,
  `preprocess.c` 1520, `provenance.c` 182, `sema.c` 3162, `strings.c` 82,
  `tokenize.c` 1050, `type.c` 161, `unicode.c` 200 行; 内部头 `chibicc.h` 395 行.
- 消费者: `main.c` 1435 行, `codegen.c` 2820 行.

### 3.2 隐式上下文的使用面

- `chibicc_ctx()` 80 处: `tokenize.c` 31, `parse.c` 21, `sema.c` 15,
  `preprocess.c` 13.
- `chibicc_arena_*` 82 处: `preprocess.c` 20, `tokenize.c` 14, `parse.c` 13,
  `sema.c` 13, `lib.c` 10, `strings.c` 5, `cst.c` 3, `hashmap.c` 2, `type.c` 2.
- 以下计数是出现次数 (含定义行, 原型与 `__attribute__((format...))`), 只用于估工:
  `error_tok` 135, `error` 29, `error_at` 17, `warn_tok` 4, `format` 28.
- 会分配的容器与构造器: `strarray_push` 58, `hashmap_put` / `put2` 16,
  `hashmap_delete` / `delete2` 4, `nodearray_push` 5, `new_node` 73, `new_binary` 62,
  `new_unary` 34, `new_num` 11, `new_cast` 9, `new_cst_node` 67,
  `new_token` 9, `copy_type` 7.
- 正则调用图粗算: 75 个函数直接触碰隐式状态, 225/344 个函数传递依赖,
  1863/5204 个调用点需要多传一个参数. 该数字只用于估工, 执行时以编译器和每一步的
  grep 为准.
- 静态状态审计: 库层没有在运行期写入的文件域 static; 文件域表只有
  `preprocess.c:1264` 的 `predefined_defs` 与 `parse.c:2259` 的
  `infix_ops` (后者已是 `const`), 都在加载期初始化, 之后只读.
  `predefined_defs` 目前没有 `const`, 建议在 E10.7 补上 (单独提交).
  函数内 static 只有 10 处, 全部是加载期初始化的只读表 (`tokenize.c`
  255/273, `parse.c` 981/1757, `preprocess.c` 1202,
  `unicode.c` 73/99/122/135/178). `type.c` 的 `ty_void` ... `ty_ldouble`
  是跨 context 共享的只读对象. 这两类不需要 ctx, 也不会在删 TLS 后引入数据竞争.

### 3.3 消费者侧的耦合

- `main.c` 有进程级 `static ChibiccContext *ctx`; 直接调用内部符号 55 次
  `strarray_push`, 13 次 `format`, 12 次 `error`; `chibicc_context_new()` 之后
  依赖它把 active 置位.
- `codegen.c` 有 17 个函数调用库的构造器, 诊断或 `format`: `gen_addr`, `gen_expr`,
  `gen_stmt`, `new_label`, `scale_rhs`, `new_add`, `new_sub`, `compute_vla_size`,
  `vla_size_expr`, `init_desg_expr`, `create_lvar_init`, `lvar_init_comma`,
  `expand_decl`, `combine`, `to_assign`, `new_inc_dec`, `shape_node`. 其中
  `new_binary` 28 处, `new_unary` 19, `new_node` 9, `new_num` 6, `new_cast` 1,
  `error_tok` 7, `format` 1, `nodearray_push` 1.
- 全显式的代价: 这 17 个函数共有 149 个直接调用点 (`gen_expr` 51, `shape_node`
  18, `gen_stmt` 14, `gen_addr` 9, 其余 57), 且 `gen_expr` / `gen_stmt` /
  `gen_addr` 是递归核心, `ctx` 会继续传给它们的调用者. 具体闭包由编译器在 E9
  枚举, 不手工维护清单.
- `codegen.c` 已有非 context 的文件域状态 (`output_file`, `depth`, `current_fn`,
  `cg_nodes` / `cg_types` / `cg_objs`), 本线不动它们; 但不再增加 context 指针.
- `test/self/self-test.c` 直接用内部符号: `chibicc_arena_calloc`,
  `chibicc_arena_realloc`, `format`; 它依赖 `chibicc_context_new` 把 active
  置位的副作用 (`chibicc_context_activate` 当前没有调用者).

### 3.4 文档与已有契约

- `docs/02-architecture.md` 2.7 明确写了 `_Thread_local active_ctx` 与"约 400 个
  内部函数通过这个指针访问状态, 不显式穿参"; 本线完成后这段必须重写.
- `docs/01-overview.md` 114-118 与 `docs/11-limitations.md` 11.4 描述同一模型.
- `docs/10-build-install.md` 6 把"线程局部存储"列为环境前提.
- `AGENTS.md` 27 行把 TLS 访问器写成"本线唯一接受的隐式状态"; 92 行起的
  代码地图也按旧模型描述 `lib.c` / `chibicc_ctx`.
- `test/public-api.sh` 已禁止消费者引用 `chibicc_ctx` 与
  `chibicc_context_activate`; 它们从未进入公共头.

### 3.5 闸门基线

- `.cache/snapshot` 存在; `make docker-snapshot-diff` 为空.
- `make docker-test`, `make check-library`, `test/self-test.sh` 在基线上全绿.
- Pratt 线的 A/B 与 tinycc 目标存在: `make docker-pratt-ab`,
  `make docker-test-thirdparty THIRDPARTY=tinycc`.

## 4. 设计

### 4.1 参数约定

- 形态: `Ret f(ChibiccContext *ctx, ...)`. `ctx` 永远是第一个参数, 不加 `const`
  (context 会被写).
- 判定规则: 函数体直接或传递地做以下任一件事, 就需要 `ctx`:
  - 调用 `chibicc_arena_alloc` / `calloc` / `strdup` / `strndup` / `realloc`;
  - 调用 `format`, `strarray_push`, `nodearray_push`;
  - 调用 `hashmap_put` / `hashmap_put2` / `hashmap_delete` / `hashmap_delete2`;
  - 调用 `error` / `error_at` / `error_tok` / `warn_tok`;
  - 读或写 `ChibiccContext` 字段 (宏表, 作用域, `current_file`, `parse_depth`,
    `failed`, `jmp_top` 等).
- 纯读函数保持原签名, 不为了统一而加参数:
  `is_integer`, `is_flonum`, `is_numeric`, `is_compatible`, `align_to`,
  `print_escaped`, `file_exists`, `hashmap_get` / `hashmap_get2`, `fnv_hash`,
  `match`, `get_entry`, unicode 的编解码, `provenance.c` 的只读访问器,
  `codegen` 内部的 `cg` / `cg_type` / `cg_obj` 查表.
- 函数体内不得再出现 `chibicc_ctx()`; 需要的 context 只能来自参数.
- 公共 D7 函数已经以 `ctx` 为首参, 签名不动; 它们负责推入 `JmpFrame` 并把 `ctx`
  传给内部入口.

### 4.2 迁移机制

采用"原语先显式, 再逐文件向上"的顺序, 中间态保留 `chibicc_ctx()` 作为适配器:

1. 先把分配, 字符串, hashmap, 诊断这些叶子函数改成收 `ctx`; 未转换的调用方先传
   `chibicc_ctx()`. 这一步不改行为, 只让每个隐式依赖在调用点可见.
2. 再按依赖自底向上转换模块: `type.c` -> `tokenize.c` -> `cst.c` -> `parse.c` ->
   `sema.c` -> `preprocess.c`. 一个函数拿到 `ctx` 后, 函数体内的
   `chibicc_ctx()` 替换成参数; 它的调用方如果还没转换, 继续传 `chibicc_ctx()`,
   否则传自己作用域里的 `ctx`.
3. 再把 `ctx` 穿透 codegen 的发射函数 (E9); 这之前 codegen 用临时的
   `chibicc_ctx()` 适配器.
4. 最后删除访问器与 TLS, 清理驱动与自测.

理由: 每一步都能用编译器枚举遗漏的调用点; 未转换的边界只剩 `chibicc_ctx()` 调用,
可以用 grep 精确统计; 不会出现"参数加了但没人用, 行为仍靠 TLS"的假完成状态.
代价是部分调用点会被改两次 (先 `chibicc_ctx()`, 后 `ctx`), 估计在总改动量的
10-15%, 接受.

禁止的做法:

- 引入 `#define CTX chibicc_ctx()` 之类的宏来少写字; 这会把隐式状态藏回去.
- 用结构体字段携带 `ctx` (例如 `HashMap.ctx`) 来代替参数; 这是另一种隐式状态.
- 在转换一个函数时顺手改逻辑, 重命名或调整顺序.

### 4.3 arena 与分配

- `chibicc_arena_alloc(ctx, size, align)`,
  `chibicc_arena_calloc(ctx, count, size)`,
  `chibicc_arena_strdup(ctx, s)`, `chibicc_arena_strndup(ctx, s, n)`,
  `chibicc_arena_realloc(ctx, old, old_size, new_size)`.
- `ctx` 必须非空. 当前 `arena_alloc(NULL, ...)` 退回 `calloc` 的兜底在 E1-E8
  期间保留用于过渡, E10 删除; 删除后 `CHIBICC_DEBUG_ARENA` 构建加 `assert(ctx)`.
- `format(ctx, fmt, ...)` 总是从 arena 分配, 删除"没有 context 时 malloc"的分支.
- `strarray_push(ctx, arr, s)`, `nodearray_push(ctx, arr, node)`.
- `hashmap_put(ctx, map, key, val)`, `hashmap_put2`, `hashmap_delete`,
  `hashmap_delete2`; 内部的 `rehash` / `get_or_insert_entry` 也收 `ctx`.
  `hashmap_get` / `hashmap_get2` 不变.

### 4.4 诊断与错误边界

- `error(ctx, fmt, ...)`, `error_at(ctx, loc, fmt, ...)`,
  `error_tok(ctx, tok, fmt, ...)`, `warn_tok(ctx, tok, fmt, ...)`;
  `verror_at(ctx, ...)`, `vformat(ctx, ...)`, `raise_error(ctx)` 同步改.
- `raise_error(ctx)` 保留"没有 jmp frame 就 `exit(1)`"的驱动器行为; 不再检查
  `ctx` 是否为空. `error_at` 继续读 `ctx->current_file`.
- `ENTER_RET` / `ENTER_VOID` 只负责推入 / 弹出 `JmpFrame`; 不再保存和设置 active
  context. `LEAVE` 只弹出 frame.
- `chibicc_context_new` / `reset` 只建 / 重置状态, 不再改变任何全局; `free` 不再
  检查 active 指针.
- 删除 `chibicc_context_activate`; 它当前没有调用者, 且它的语义 (留下未配对的
  active) 与本线冲突.

### 4.5 共享构造器

`new_node` / `new_binary` / `new_unary` / `new_num` (parse.c) 与 `new_cast`
(sema.c) 被 sema 和 codegen 调用, 不能只在定义文件里改. 处理方式:

- `new_node(ctx, kind, tok)`, `new_binary(ctx, kind, lhs, rhs, tok)`,
  `new_unary(ctx, kind, expr, tok)`, `new_num(ctx, val, tok)`;
  `new_cast(ctx, expr, ty)`.
- `new_node` 家族在 E6 与 `parse.c` 一起改; `new_cast` 在 E7 与 `sema.c` 一起改.
  已转换的调用方传自己的 `ctx`; E6 时 sema 还没转换, 先传 `chibicc_ctx()`,
  E7 再换成 `ctx`; codegen 在 E9 之前先传 `chibicc_ctx()`, E9 全显式后传 `ctx`.
- 构造器内部只把 `chibicc_ctx()` 换成 `ctx`, 不改变节点形状, 锚点或字段.

### 4.6 驱动与 codegen

- `main.c` 已有进程级 `static ChibiccContext *ctx`; 它是驱动器的单 context.
  本线把 `format` / `error` / `strarray_push` 的每个调用点改成显式命名 `ctx`,
  不新增全局.
- `codegen.c` 不使用 `cg_ctx` 之类的文件域 context 指针. `codegen(ChibiccContext
  *ctx, Obj *prog, FILE *out)` 接收驱动器的 ctx, 并把它作为首参传给每个调用库
  构造器 / 诊断 / `format` / `nodearray_push` 的发射函数; 这些函数再把它传给
  自己的调用者, 直到 `gen_expr` / `gen_stmt` / `gen_addr` 这组递归核心.
- 不需要 ctx 的 codegen 函数 (纯文本输出, 寄存器名, cg 表查表等) 保持原签名;
  判定规则与 4.1 相同.
- `output_file` / `depth` / `current_fn` / cg 表保留; 它们不是 context.
- `test/shape.sh`, 快照 diff 与 tinycc 必须证明发射结果不变.
- 全显式的代价: 17 个函数与至少 149 个直接调用点, 加上这些函数的调用者;
  E9 用编译器枚举, 不做手工清单.

### 4.7 context 生命周期与嵌套

- `chibicc_context_new`: `calloc` context, `init_macros(ctx)`, 返回. 不再设置 active.
- `reset`: 释放 arena, 清空 per-TU 状态, `init_macros(ctx)`, 重放 `macro_ops`, 返回.
- `free`: 释放 arena 与配置; 不再检查 / 清空 active.
- 诊断回调里调用另一个 context 的公共入口 (包括 `new` / `define` / `reset` /
  `free`) 只影响那个 context; 外层 `raise_error(ctx)` 仍用外层自己的 frame.
- 这一条用 `test/library.sh` 的 `check_context_scope` 锁定; 旧实现在回调里
  `new` + `define` + `free` 后会把 active 留成 NULL, 外层错误直接 `exit(1)`.

### 4.8 共享状态复查

删 TLS 的前提是库层没有别的隐式共享状态. 开工与收尾各做一次:

- 文件域 static: 目标为 0 个运行期写入项; 只允许函数原型与加载期只读表.
  `predefined_defs` 在 E10.7 加 `const`, 让编译器而不是约定来保证.
- 函数内 static: 只允许加载期初始化的只读表; 不允许惰性初始化的缓存. 当前 10 处
  全部满足.
- 全局对象: `type.c` 的内建 `Type` 是只读共享, 不需要 ctx; 任何新写的全局对象
  必须先证明只读.
- 把这些检查写进 `test/context-audit.sh`, 防止后续提交把隐式状态加回来.

### 4.9 文档同步

- `libchibicc/include/chibicc/context.h`: 线程模型注释改为"内部函数显式收 ctx,
  无 TLS; 嵌套回调安全".
- `docs/02-architecture.md` 2.7: 重写为显式传参模型, 删除"约 400 个函数不显式穿参"
  与"new/free 修改 active 指针".
- `docs/01-overview.md` 49 与 114-118: 删除"唯一隐式状态"的 TLS 描述.
- `docs/10-build-install.md` 6: 不再把线程局部存储列为库的运行前提 (测试套件仍用
  pthread).
- `docs/11-limitations.md` 11.4: 更新为"无 TLS; 一个 context 一个线程".
- `AGENTS.md` 27 行, 92 行起的代码地图, 以及路线图: 规则改为"库层无隐式上下文",
  代码地图删除 `chibicc_ctx()` 与 active_ctx 的描述, 路线图加入本线.
- `README.md` 的 thread-local 链接是上游教学历史的引用 (讲 C 语言特性如何实现),
  不是库的 context 模型, 不改.

## 5. 步骤

### E0 - 基线与开工准备

- [ ] E0.1 在 `main` 上复验: `make docker-test`, `make docker-snapshot-diff`,
  `make check-library`, `test/self-test.sh`. 快照缺失或过期时先
  `make docker-snapshot`, 并在 `RESULT` 记录重置原因.
- [ ] E0.2 记录锚点: `git rev-parse HEAD`, 3.1 / 3.2 的行数与计数, 41 个
  `test/*.c`, 各测试的基线数字.
- [ ] E0.3 开分支 `explicit-context`; 每一步一个提交; 每个提交单独可构建.
- [ ] E0.4 建立独立的 `test/context-audit.sh` 与 `make check-context`:
  E0 版本只打印当前的三个符号与 TLS 行并以 0 退出 (报告模式), E10 转为断言失败;
  脚本接受 `CHIBICC_LIB` (默认 `libchibicc.a`), 以便对 `pratt/libchibicc.a`
  复用; 不并入 `make check-library`.
- [ ] E0.5 把实测锚点补进 3.x, 并确认 4.1 的参数约定与 4.2 的迁移顺序和当前代码
  一致; 后续提交按此审读.

验收: 基线全绿; 锚点与审计脚本入库; 后续每一步都能用同一套命令复验.

### E1 - 原语层: arena, strings, hashmap

- [ ] E1.1 `chibicc_arena_alloc` / `calloc` / `strdup` / `strndup` / `realloc`
  增加 `ctx` 首参; 删除内部的 `chibicc_active_ctx` 读取; 82 个调用点, 除 `lib.c`
  传自己的 `ctx` 外, 其余先传 `chibicc_ctx()`. 暂时保留 NULL 兜底.
- [ ] E1.2 `format(ctx, ...)`, `strarray_push(ctx, ...)`,
  `nodearray_push(ctx, ...)`; 28 + 58 + 5 个调用点. `main.c` 传全局 `ctx`;
  codegen 的 1 个 `format` 与 1 个 `nodearray_push` 先传 `chibicc_ctx()`,
  E9 再换成 `ctx`.
- [ ] E1.3 `hashmap_put` / `put2` / `delete` / `delete2` 与内部 `rehash` /
  `get_or_insert_entry` 增加 `ctx`; 20 个调用点先传 `chibicc_ctx()`.
  `hashmap_get` / `get2` 不变.
- [ ] E1.4 `test/self/self-test.c`: `arena_test(ctx)` / `hashmap_test(ctx)`,
  `format(ctx, ...)`.

验收: 构建通过; `test/diagnostic.sh`, `test/ast.sh`, `test/cst.sh`,
`test/trivia.sh`, `test/expand.sh`, `test/shape.sh`, `test/library.sh`,
`make check-library` 全绿; 转换过的原语内部 `chibicc_ctx()` 为 0.

### E2 - 诊断层

- [ ] E2.1 `error`, `error_at`, `error_tok`, `warn_tok`, `verror_at`,
  `vformat`, `raise_error` 增加 `ctx` 首参; 185 个调用点. `lib.c` 传 `ctx`,
  其他未转换文件传 `chibicc_ctx()`; `main.c` 传全局 `ctx`; codegen 的 7 个
  `error_tok` 先传 `chibicc_ctx()`, E9 再换成 `ctx`.
- [ ] E2.2 `raise_error(ctx)` 只在 `ctx->jmp_top == NULL` 时 `exit(1)`; 删除
  `ctx` 为空的判断; `error_at` 继续用 `ctx->current_file`.
- [ ] E2.3 在 `test/library.sh` 的 `check_nested` 旁增加 `check_context_scope`:
  回调里 `new` + `define` + `free` 另一个 context; 断言外层错误仍返回而不是退出,
  外层 context 之后仍能跑完整管线.

验收: 诊断 68 例逐字节不变; `test/warning.sh`; `test/library.sh` 的错误返回,
嵌套与 ASan 轮; 诊断函数内部 `chibicc_ctx()` 为 0.

### E3 - type.c

- [ ] E3.1 内部 `new_type` 与 9 个对外构造器加 `ctx`: `copy_type`,
  `pointer_to`, `func_type`, `array_of`, `vla_of`, `array_of_dim`,
  `typeof_placeholder`, `enum_type`, `struct_type`. 4 个谓词 (`is_integer`,
  `is_flonum`, `is_numeric`, `is_compatible`) 不变.
- [ ] E3.2 调用点: parse / sema / cst 等未转换文件先传 `chibicc_ctx()`;
  `main` / `lib` 传 `ctx`.
- [ ] E3.3 `type.c` 内 `chibicc_ctx()` 清零; 内建 `Type` 只读对象的注释保留.

验收: 构建; `test/ast.sh`; `test/shape.sh`; `test/library.sh`; `type.c` 的
`chibicc_ctx()` 为 0.

### E4 - tokenize.c

- [ ] E4.1 入口与构造器加 `ctx`: `tokenize_file`, `tokenize`, `new_file_len`,
  `new_file`, `new_token`, `add_line_numbers`, `make_scan_buffer`, 以及所有
  `read_*` / `is_keyword` 辅助. 31 个 `chibicc_ctx()` 与 14 个 arena 调用点.
- [ ] E4.2 调用方: `lib.c` 的 `chibicc_tokenize_file` /
  `chibicc_tokenize_buffer*` 传 `ctx`; preprocess 的 include 路径先传
  `chibicc_ctx()`; `cst` 不调用 tokenize.
- [ ] E4.3 `ctx->current_file`, `input_files`, `at_bol`, `has_space` 的写入改为
  参数 `ctx`.

验收: `test/trivia.sh` (`-dump-tokens` / `-reconstruct` / `-E` 锚点);
`test/diagnostic.sh`; `-dump-ast` 冒烟; `test/library.sh`;
`tokenize.c` 的 `chibicc_ctx()` 为 0.

### E5 - cst.c

- [ ] E5.1 `cst_parse(ctx, tok)`; `new_cst_node(ctx, ...)` 与全部建树 / 分组 /
  指令解析函数加 `ctx`; 67 个 `new_cst_node` 调用点.
- [ ] E5.2 `strarray_push` 与 arena 调用点改传 `ctx`; children 数组的 arena
  realloc 同步.
- [ ] E5.3 `lib.c` 的 `chibicc_cst_*` facade 传 `ctx`.

验收: `test/cst.sh` 305 项 / 61 个 dump 样本 / 全量 round-trip / verify;
`test/library.sh` 的 CST 与 ASan 轮; `cst.c` 的 `chibicc_ctx()` 为 0.

### E6 - parse.c

- [ ] E6.1 入口与全部语法函数加 `ctx`: `parse`, `parse_init`,
  `is_typename` (读 `ctx->typename_map`), `enter_scope` / `leave_scope`,
  `add_typedef` / `add_declared_name` / tag 作用域, `declspec` / `declarator`
  全家, 语句层, 初始化器, `primary`.
- [ ] E6.2 两条表达式路径都转换: 默认级联 (`expr` ... `mul`) 与
  `CHIBICC_PRATT_PARSE_FEATURE=1` 的 `expr_bp` / nud / led; `InfixOp` 表不变.
- [ ] E6.3 节点构造器 `new_node` / `new_binary` / `new_unary` / `new_num`
  加 `ctx`; parse 与 sema 的调用点传各自作用域的 `ctx` 或 `chibicc_ctx()`;
  codegen 的 62 个调用点先传 `chibicc_ctx()`, E9 再换成 `ctx`.
- [ ] E6.4 `parse_depth` 的递增 / 递减改走参数 `ctx`; 512 上限不变.

验收: `test/ast.sh` 85 例 (两个构建); `test/pratt-ab.sh` 41 个语料的 AST 与
`-S` 逐字节相同; `test/diagnostic.sh`; `test/library.sh` 的深度用例;
`parse.c` 的 `chibicc_ctx()` 为 0.

### E7 - sema.c

- [ ] E7.1 `sema(ctx, toplevel)`, `const_expr(ctx, tok)`, `eval` / `eval2` /
  `eval_rval` / `eval_double` / `is_const_expr`, `add_type`, `type_chain`,
  `bind_ident`, `push_scope`, `analyze_function`, `resolve_*`, `check_*`,
  `write_gvar_data`, `layout_struct` / `layout_union` 等全部加 `ctx`.
- [ ] E7.2 `new_cast(ctx, expr, ty)`; sema 内 8 个调用点传 `ctx`, codegen 的
  1 个先传 `chibicc_ctx()`, E9 再换成 `ctx`.
- [ ] E7.3 `ctx->locals` / `globals` / `sema_fn` / `builtin_alloca` / 作用域表 /
  `gotos` / `labels` / `loop_depth` / `switch_depth` / `unique_name_id` /
  `resolving_body` 的访问改为参数 `ctx`.
- [ ] E7.4 preprocess 的 `const_expr` 调用点先改为 `const_expr(chibicc_ctx(), tok)`,
  等 E8 再换成 `const_expr(ctx, tok)`.

验收: `test/library.sh` 全管线; `test/self-test.sh` 的 sema-state;
`test/ast.sh`; `test/shape.sh`; `test/diagnostic.sh`; `sema.c` 的
`chibicc_ctx()` 为 0.

### E8 - preprocess.c

- [ ] E8.1 `preprocess(ctx, tok)`, `init_macros(ctx)`, `define_macro` /
  `undef_macro(ctx, ...)`, `expand_macro`, `subst`, `read_macro_definition`,
  `include_file`, `search_include_paths`, `eval_const_expr` 等全部加 `ctx`.
- [ ] E8.2 `const_expr(ctx, tok)` 的调用点从 `chibicc_ctx()` 换成 `ctx`.
- [ ] E8.3 `copy_token` 的 14 个调用点, `format` 的 8 个, `error_tok` 的 26 个,
  arena 的 20 个, hashmap 的 4 个全部显式传 `ctx`.
- [ ] E8.4 `chibicc_preprocess` facade 传 `ctx`; `init_macros` 在
  `chibicc_context_new` / `reset` 里由 `ctx` 调用.

验收: `test/expand.sh` 29 + 3 + 17 + 48; `-E` 逐字节; `test/trivia.sh`;
`test/library.sh` 的 include / define; `test/diagnostic.sh`;
`preprocess.c` 的 `chibicc_ctx()` 为 0.

### E9 - codegen 全显式

- [ ] E9.1 `codegen(ChibiccContext *ctx, Obj *prog, FILE *out)`; `driver.h` 与
  `main.c` 更新; 确认 codegen 没有 `cg_ctx` 或任何 context 文件域指针.
- [ ] E9.2 把 `ctx` 穿透 17 个调用库构造器 / 诊断 / `format` /
  `nodearray_push` 的函数, 并继续传给它们的调用者: `gen_addr`, `gen_expr`,
  `gen_stmt`, `new_label`, `scale_rhs`, `new_add`, `new_sub`,
  `compute_vla_size`, `vla_size_expr`, `init_desg_expr`, `create_lvar_init`,
  `lvar_init_comma`, `expand_decl`, `combine`, `to_assign`, `new_inc_dec`,
  `shape_node`. 以编译器报错枚举调用点, 不做手工清单.
- [ ] E9.3 把 codegen 里 E1-E8 期间使用的临时 `chibicc_ctx()` (1 个 `format`,
  1 个 `nodearray_push`, 7 个 `error_tok`, 62 个 `new_node` 家族构造器,
  1 个 `new_cast`) 全部换成 `ctx`; 确认 `codegen.c` 的 `chibicc_ctx()` 为 0.
- [ ] E9.4 不需要 ctx 的函数保持原签名; `output_file` / `depth` /
  `current_fn` / cg 表不动.

验收: `test/shape.sh` 34 段; `make docker-snapshot-diff` 为空; `test/ast.sh`
与 `test/pratt-ab.sh` 两构建一致; `make docker-test` (含自举与 stage2);
`make test-stage2`; 本机可跑的测试.

### E10 - 删除隐式状态

- [ ] E10.1 删除 `libchibicc/src/chibicc.h` 的 `chibicc_active_ctx` /
  `chibicc_ctx` / `chibicc_context_activate` 声明; 删除 `lib.c` 的 TLS 定义,
  访问器与 activate.
- [ ] E10.2 `ENTER_RET` / `ENTER_VOID` 去掉 `_prev` 与 active 保存 / 恢复;
  `LEAVE` 只弹 `jmp_top`. `chibicc_context_new` / `reset` / `free` 不再触碰
  active.
- [ ] E10.3 删除 arena 在 `ctx == NULL` 时退回 `calloc` / `realloc` 的分支
  (保留 `old == NULL` 的重分配语义) 与 `format` 的 malloc 分支;
  `CHIBICC_DEBUG_ARENA` 下加 `assert(ctx)`.
- [ ] E10.4 `main.c` 删除对 `chibicc_context_new` 副作用的依赖; 所有内部调用
  显式传 `ctx`; 确认没有 `chibicc_context_activate` 调用.
- [ ] E10.5 确认 `test/self/self-test.c` 全部走显式 `ctx` (E1.4 已改), 不再依赖
  `chibicc_context_new` 的副作用; `test/library.sh` 的注释从
  "active context is thread-local" 改为"contexts carry no shared state".
- [ ] E10.6 `test/context-audit.sh` 转严: 三个符号, TLS 关键字, codegen /
  `main.c` 的访问器引用, 以及 codegen 的 `cg_ctx` 必须为 0; Makefile 提供
  `check-context` 与 `check-context-pratt`, 并接入 `test-all` 与 Docker 闸门.
- [ ] E10.7 (可选, 单独提交) 给 `predefined_defs` 加 `const`, 把
  `preprocess.c` 的遍历指针改成 `const PredefinedDef *`; 这是审计口径的硬化,
  与 ctx 改动分开.

验收: 判据 1 与判据 4 全部通过; `make check-context`; `make check-library`;
`test/library.sh`; `test/self-test.sh`; 本机全量可跑的测试.

### E11 - 全闸门, 文档与归档

- [ ] E11.1 默认构建: `make docker-test` (含自举), `make docker-snapshot-diff`,
  `make check-context`, `make check-library`, `test/self-test.sh`.
- [ ] E11.2 Pratt: `make docker-test-both`, `make docker-pratt-ab`
  (41 语料 + tinycc 的 AST / `-S` A/B 与测试套件), `make check-library-pratt`,
  `make check-context-pratt`, `make test-stage2-pratt`.
- [ ] E11.3 真实项目 (硬性): `make docker-test-thirdparty THIRDPARTY=tinycc`;
  任一失败即本线未完成.
- [ ] E11.4 文档: 4.9 列出的文件全部更新; `docs/02-architecture.md` 2.7 重写;
  `AGENTS.md` 的规则, 代码地图与路线图同步; `docs/09-api-reference.md` 注明
  `.a` 内部符号签名不承诺兼容.
- [ ] E11.5 `plans/RESULT-explicit-context.md`: 每步实测, 改动量, 受影响的
  `.a` 内部符号, 与计划的偏离, 以及 codegen 全显式的调用点统计.
- [ ] E11.6 合并回 `main`; 确认 `make clean && make` 与基线等价.

验收: 判据 1 到 6 全部有实测证据; 没有临时代码, 脚本或未使用目标.

## 6. 闸门矩阵

| 闸门 | 默认 | Pratt | 说明 |
|---|---|---|---|
| `make docker-test` | 必须绿 | `make docker-test-pratt` | 含自举与 stage2 |
| `make test-stage2` | 必须绿 | `make test-stage2-pratt` | 自举库对象 |
| `make docker-snapshot-diff` | 必须为空 | 不适用 | 默认路径的 `-S` |
| `test/pratt-ab.sh` | 不适用 | 41 语料 + tinycc | AST 与 `-S` |
| `make docker-pratt-ab` | 不适用 | 必须绿 | tinycc A/B 与测试套件 |
| `make check-library` | 必须绿 | `make check-library-pratt` | 公共面, install, ASan |
| `make check-context` | 必须绿 | `make check-context-pratt` | 独立审计: 无 TLS / 无访问器 / codegen |
| `test/library.sh` | 必须绿 | 必须绿 | 线程与嵌套 |
| `test/self-test.sh` | 必须绿 | 必须绿 | arena / hashmap / sema |
| `test/diagnostic.sh` | 68 例 | 68 例 | 逐字节 |
| `test/warning.sh` | 必须绿 | 必须绿 | 逐字节 |
| `test/shape.sh` | 34 段 | 34 段 | 发射形状 |
| `test/ast.sh` | 85 例 | 85 例 | 两构建相同 |
| `test/trivia.sh` / `cst.sh` / `expand.sh` | 必须绿 | 必须绿 | 只读通道 |
| `make docker-test-thirdparty THIRDPARTY=tinycc` | 硬性 | 硬性 | 真实项目, 失败即未完成 |

本地开发循环 (macOS): `make && make libchibicc.a && make check-context &&
test/library.sh && test/self-test.sh && test/ast.sh ./chibicc`; 全量语料, 自举,
Pratt A/B 与 tinycc 必须在 Docker 内.

## 7. 回退与维护

- 每一步一个提交, 顺序执行. 任一步失败可以只 revert 该步; 前面步骤的显式参数
  仍然成立, 后面的步骤依赖它.
- E10.1-E10.6 是删除 TLS 的那一个提交. 若之后发现必须恢复 TLS: revert 该提交会
  恢复访问器与 active 指针, 但已加的 `ctx` 参数保留 (冗余但无害); 或者 revert
  整条线. E10.7 是独立的 `const` 提交, 与 TLS 无关. E9 的 codegen 全显式是
  独立提交, 失败只回退 E9, 前面的库层改动仍成立.
- 本线不引入特性宏, 不保留双实现; 默认构建就是终态. 需要对照时用
  `git tag` 标记最后一个 E 步骤.
- 维护规则 (写入 `AGENTS.md`): 库层新增函数若触碰 arena / 诊断 / context 字段,
  必须把 `ChibiccContext *ctx` 作为首参; 不得新增 `chibicc_ctx()` 或 TLS;
  `test/context-audit.sh` 是每步必跑的门.
- `.a` 的内部符号签名变化不承诺源码兼容; 公共头与 D7 才是契约. E11.4 把它写入
  `docs/09-api-reference.md`, E11.5 在 `RESULT` 记录受影响的符号.

## 8. 风险与对策

| 风险 | 对策 |
|---|---|
| 迁移中漏传 ctx, 退回 TLS 或 NULL | E1-E2 的适配器让每个依赖显式; `CHIBICC_DEBUG_ARENA` 断言; E10 的 grep 与 `nm` 闸门 |
| 改动量大 (约 1800 个调用点), 审读困难 | 每步只做签名 / 参数; `git diff -w`; 每步单独提交; 不做逻辑改动 |
| 同一调用点被改两次 | 接受 10-15% 的重复; 不为省改动而跳过显式化 |
| 诊断行为漂移 | `diagnostic.sh` 逐字节; `raise_error` 的 exit 分支保持 |
| codegen 的 17 个函数与 149 个直接调用点 | E9 全显式, 不用 `cg_ctx`; 编译器枚举调用链; 每改一组跑 `shape.sh` |
| codegen 的递归核心 (`gen_expr` / `gen_stmt` / `gen_addr`) 级联大 | 先改 17 个入口, 让编译器列出上层调用者; 分小提交; 快照与 tinycc 兜底 |
| main.c 的 55 个 `strarray_push` 漏传 | 编译器报错枚举; 驱动器已有全局 `ctx`, 不新增状态 |
| Pratt 两条表达式路径不一致 | E6 同时转换; `pratt-ab.sh` 与 `ast.sh` 双构建 |
| 自举失败 | 每步跑 `make test-stage2`; 失败时先缩小该步范围 |
| 公共 ABI 误改 | 公共头零签名变化; `public-api.sh` / `install.sh` / `.so` 白名单 |
| 删 TLS 后暴露别的共享状态 | 4.8 的静态审计; `context-audit.sh`; 线程测试 |
| `chibicc_context_new` / `reset` 无 frame 时仍 exit | 保持现状; 不把错误模型改造混进本线 |
| 性能回退 | 只多一个指针参数; 快照与测试无差异; 不做优化 |
| 与 L5 或其他线冲突 | 单独分支; 先合并 main 再开工; 冲突时逐文件重放机械改动 |

## 9. 已决事项

2026-10-23 用户拍板:

- 采用显式传递 `ChibiccContext *` 的方案, 作为本线的唯一形态.
- `codegen.c` 不接受 `cg_ctx` 例外; `ctx` 穿透 17 个需要的发射函数及其调用链.
- `test/context-audit.sh` 与 `make check-context` 独立成目标, 不并入
  `make check-library`.
- tinycc 作为硬性收尾闸门, 失败即本线未完成.
- E10 一次删除 TLS 与访问器; 不保留 `CHIBICC_SINGLE_THREADED` 兼容构建.
- 分支名定为 `explicit-context`.
- `.a` 内部符号签名不承诺兼容; 写入 `docs/09-api-reference.md` 与 `RESULT`.

无待拍板项. 后续线: 去掉 codegen 的 context 指针不等于支持多翻译单元并发发射;
`output_file` / `depth` / `current_fn` / cg 表仍是进程级状态. 若需要并发发射,
另开线. 若将来需要单线程兼容构建, 也另开线.

## 10. 记录

- 本文件是执行计划; 开工后每一步的实测, 改动量, 闸门结果与偏差写入
  `plans/RESULT-explicit-context.md`.
- 新会话恢复: 通读 `AGENTS.md` -> 查看本文件勾选状态 -> `git log --oneline`
  确认最后完成的步骤 -> 从第一个未勾选项继续; 开工前先跑 E0.1 的基线复验.
- 行号与计数以函数名和当步实测为准; 本文的 3.x 数字是 2026-10-23 的基线.
