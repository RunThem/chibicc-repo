# RESULT-explicit-context.md - 显式 ChibiccContext 传参线执行记录

> 计划: `plans/PLAN-explicit-context.md`
> 分支: `explicit-context`
> 基线: `0138b5a`
> 终态: E0-E11 全部完成; 每个 E 步骤一个提交 (E10 两个), 见下

## E0 - 基线与开工准备

### E0.1 基线复验 (2026-10-23)

- `make docker-test`: 绿 (含自举 + stage2 + self-test).
- `make check-library` / `test/self-test.sh`: 在 docker-test 内全绿.
- `make docker-snapshot-diff`: **首次非空**, 仅 `string.s`. 原因: 基线快照
  (`.cache/snapshot/string.s`, 2026-10-23 00:56) 早于 `5bc0827` (01:09) 与
  `0138b5a` (08:05); 后者给 `test/string.c` 增加 6 行 (字符串字面量起始的
  初始化器), 因此新快照多出对应字符串对象的 `.data`. 判定为快照过期, 不是
  行为漂移. 按 E0.1 执行 `make docker-snapshot` 重置, 之后
  `make docker-snapshot-diff` 为空 (41 个文件).
- 锚点 (与计划 3.x 一致): `cst.c` 2393, `hashmap.c` 145, `lib.c` 473,
  `parse.c` 2654, `preprocess.c` 1520, `provenance.c` 182, `sema.c` 3162,
  `strings.c` 82, `tokenize.c` 1050, `type.c` 161, `unicode.c` 200,
  `chibicc.h` 395, `codegen.c` 2820, `main.c` 1435; 41 个 `test/*.c`.
- 隐式上下文计数: `chibicc_ctx` 出现在 `tokenize.c` 29, `parse.c` 21,
  `sema.c` 15, `preprocess.c` 13, `lib.c` 1 (定义 + 访问器).

### E0.4 审计脚本

- 新增 `test/context-audit.sh` 与 `make check-context` /
  `make check-context-pratt`; 支持 `CHIBICC_LIB`, 独立于 `check-library`.
- 报告模式在 E0-E9 使用; E10.6 起两个目标与 `test` / `test-pratt` 都跑
  `--strict`.
- TLS 检查只匹配声明形行, 避免把编译器关键字表里的
  `"_Thread_local"` / `"__thread"` 字符串算成命中.

### E0 附带提交: `0e1f524` 重命名 cst.c 的局部 ctx

计划要求 `ChibiccContext *ctx` 作首参, 但 `cst.c` 的 `CstContext` 参数已经叫
`ctx` (61 处, 含 `CstParser.ctx`). 该文件先做一个纯重命名提交 (`ctx` ->
`cst`), 否则 E5 无法进行. 这是计划未列出的准备步骤, 不改行为.

## E1 - 原语层 (`1aa2ae3`)

- `chibicc_arena_alloc/calloc/strdup/strndup/realloc`, `format`,
  `strarray_push`, `nodearray_push`, `hashmap_put/put2/delete/delete2`,
  `rehash`, `get_or_insert_entry` 加 `ctx`; arena 包装不再读
  `chibicc_active_ctx`; NULL 兜底保留到 E10.
- `test/self/self-test.c` 的 `arena_test` / `hashmap_test` 显式收 ctx.
- 202 个调用点插入 (`chibicc_ctx()` 或 `ctx`).
- 闸门: 本机 diagnostic / warning / shape / ast / trivia / cst / expand /
  library / self-test / check-library 全绿.

## E2 - 诊断层 (`1d957dd`)

- `error`, `error_at`, `error_tok`, `warn_tok`, `verror_at`, `vformat`,
  `raise_error` 加 `ctx`; 四个 `ChibiccContext *ctx = chibicc_ctx()` 局部
  声明删除; `raise_error` 只看 `ctx->jmp_top`.
- `unreachable()` 在迁移期改为 `error(chibicc_ctx(), ...)`, E10 再改为
  `error(ctx, ...)`.
- 185 个诊断调用点 (171 处由工具插入, 其余在 lib.c/main.c 已显式).
- `test/library.sh` 新增 `check_context_scope`: 回调里
  `new` + `define` + `tokenize` + `preprocess` + `free` 另一个 context,
  断言外层错误仍返回且外层 context 之后能跑完整管线.
  **负对照**: 把 `raise_error` 临时改回读 TLS 指针, `test/library.sh` 退出 1;
  恢复后为 0.

## E3 - type.c (`ee4e508`)

- `new_type` 与 9 个构造器 (`copy_type`, `pointer_to`, `func_type`,
  `array_of`, `vla_of`, `array_of_dim`, `typeof_placeholder`, `enum_type`,
  `struct_type`) 加 `ctx`; 4 个谓词不变. 49 个调用点.
- `type.c` 的 `chibicc_ctx()` 为 0.

## E4 - tokenize.c (`2b5ed0b`)

- 21 个函数加 `ctx` (入口, 文件/token 构造器, scan buffer, 字面量读取,
  `is_keyword`, `convert_pp_*`, `new_file*`), 135 个调用点.
- `read_file` 的 `chibicc_active_ctx` 局部改名: 删除该局部, 直接用参数;
  `new_token` 的 4 处 `chibicc_ctx()->current_file` 改为 `ctx->`.
- `tokenize.c` 的访问器引用为 0. `test/trivia.sh` (-dump-tokens /
  -reconstruct / -E) 与 diagnostic / ast 全绿.

## E5 - cst.c (`86f9d1d`)

- 70 个建树函数加 `ctx`: `cst_parse`, `new_cst_node` 与全部分组 / 指令解析
  函数, children 数组的 arena realloc 同步; 455 个调用点.
- `cst_reconstruct` / `cst_verify` 保持无 ctx: 只读树, 不分配, 不报诊断.
- `test/cst.sh` 305 项 / 61 dump 样本 / 全量 round-trip 与 verify 全绿.

## E6 - parse.c (`964f13f`)

- 73 个函数加 `ctx` (全部三条表达式路径: 旧级联, Pratt `expr_bp` / `nud` /
  `led` 与共享 shim, 语句与声明层); `new_node` / `new_binary` / `new_unary` /
  `new_num` / `get_ident` / `parse` / `parse_init` 同步.
- `#define scope (chibicc_ctx()->scope)` -> `(ctx->scope)`; `parse_depth`
  经参数访问.
- **工具修正**: 第一版重写器只看默认模式的 AST, 漏掉只存在于 Pratt 模式的
  调用点 (parse.c:2393) 与只存在于 Pratt 的 `expr_bp` / `nud` / shim 定义与
  前向声明. 修正后默认与 Pratt 两个构建都无 `chibicc_ctx`, 18 个
  `expr_bp(ctx, ...)`.
- 闸门: `test/ast.sh` 两个构建相同; 39/41 语料的本机 A/B (AST + `-S`) 相同,
  2 个因 macOS 缺 pthread 头跳过 (Docker 内全量).

## E7 - sema.c (`1289cb5`)

- 92 个函数加 `ctx`; 12 个访问器宏 (`scope` / `locals` / `globals` /
  `sema_fn` / `builtin_alloca` / `gotos` / `labels` / `loop_depth` /
  `switch_depth` / `unique_name_id` / `resolving_body` 等) 改为 `ctx->...`;
  `label_gotos` 交接直接读 `ctx`; `new_cast` 加 `ctx`, 482 个调用点.
- `test/self-test.sh` 的 sema-state 通过.

## E8 - preprocess.c (`70e4471`)

- 51 个函数加 `ctx` (宏, include, 展开, 内建处理器, `#if`); 3 个访问器宏改
  `ctx->...`; `macro_handler_fn` 改为收 ctx 的函数指针类型; 134 个调用点.
- 计划 4.1 的纯读例外有两处不成立, 按传递规则转换并在此记录:
  `decode_utf8` 与 `display_width` (内部 `error_at`) 和 `expansion_dump`
  (`token_kind_name`). 顺带 `read_ident` 加 `ctx` (调用 `decode_utf8`).
- `CHIBICC_TEST_BUILTINS` 只在 `test/library.sh` 的 ASan 轮定义, AST 库看不到:
  `multi_macro` 与 `add_builtin` 的调用手工补齐.

## E9 - codegen (`fbd1c00`)

- `codegen(ChibiccContext *ctx, Obj *prog, FILE *out)`; `driver.h` 与
  `main.c` 同步. 30 个发射函数加 `ctx` (17 个入口及其调用链:
  `gen_addr` / `gen_expr` / `gen_stmt` / `new_label` / `scale_rhs` /
  `new_add` / `new_sub` / `compute_vla_size` / `vla_size_expr` /
  `init_desg_expr` / `create_lvar_init` / `lvar_init_comma` / `expand_decl` /
  `combine` / `to_assign` / `new_inc_dec` / `shape_node` 等), 157 个调用点.
- `codegen.c` 无 `chibicc_ctx`, 无 `cg_ctx`; `output_file` / `depth` /
  `current_fn` / cg 表未动.
- **汇编等价**: 用 `0138b5a` 的 worktree 构建基线, 39/41 个本机可编译语料的
  `-S` 逐字节相同, 2 个因缺 Linux 系统头跳过; shape.sh 34 段全绿.

## E10 - 删除隐式状态 (`3ec117e`)

- 删除 `chibicc_active_ctx` / `chibicc_ctx` / `chibicc_context_activate`;
  `ENTER_RET` / `ENTER_VOID` 只压 `JmpFrame`, `LEAVE` 只弹栈; `new` /
  `reset` / `free` 不再碰全局.
- arena 的 NULL 兜底与 `format` 的 malloc 分支删除;
  `CHIBICC_DEBUG_ARENA` 的 `arena_alloc` 加 `assert(ctx)`; realloc 的
  `old == NULL` 语义保留 (kept = 0 走 arena).
- **计划 4.1 的例外修正**: `hashmap_get` / `hashmap_get2` / `get_entry`
  必须加 `ctx`, 因为 `get_entry` 的 `unreachable()` 在 E10 变成
  `error(ctx, ...)`. 25 个调用点.
- `test/library.sh` 的线程注释改为"contexts carry no shared state";
  `strings.c` 的 malloc 注释同步.
- `make check-context` / `check-context-pratt` 转严并接入 `test` /
  `test-pratt`; 本机两者均为 0.
- 负对照之外的回归证据: 删 TLS 后 `check_threads` (跨线程双 context) 与
  `check_nested` / `check_context_scope` 全绿, ASan+UBSan 轮零 UB.

## E10.7 - const 硬化 (`de15468`)

- `predefined_defs` 加 `const`, 遍历指针改 `const PredefinedDef *`.

## E11 - 全闸门

### E11.1 默认构建

- `make docker-test` (含自举 + stage2 + library + self-test + context-audit):
  **绿** (EXIT=0, `.cache/logs/E11-docker-test.log`); 容器内日志含
  `context-audit: ...: 0` 六行.
- `make docker-snapshot-diff`: **空**, 41 个文件
  (`.cache/logs/E11-snapshot.log`).
- `make check-context` / `make check-library`: 绿 (本机).
- `test/self-test.sh`: 绿.

### E11.2 Pratt

- `make docker-test-both` (test + test-pratt + stage2 + stage2-pratt):
  **绿** (EXIT=0, `.cache/logs/E11-docker-test-both.log`).
- `make docker-pratt-ab`: **绿** (EXIT=0,
  `.cache/logs/E11-pratt-ab.log`): corpus 41 个文件的 `-dump-ast` 与 `-S`
  逐字节相同; tinycc 10 个 translation unit 的 AST 与汇编逐字节相同, 两个构建
  各自的测试套件均通过.
- `make check-library-pratt` / `make check-context-pratt`: 绿 (本机).
- 本机手工 A/B: 39/41 语料 (2 个缺 Linux 系统头) 的 AST 与 `-S` 相同.

### E11.3 真实项目

- `make docker-test-thirdparty THIRDPARTY=tinycc`: **绿** (EXIT=0,
  `.cache/logs/E11-tinycc.log`). tinycc 的构建与测试套件全部通过; 这是本线的
  硬性收尾闸门.

### 规模

- 19 个文件 (库源码, `codegen.c`, `main.c`, `driver.h`, 4 个测试脚本),
  `2946 insertions(+) / 2844 deletions(-)` (不含计划 / RESULT / 文档).
- 583 个定义中 410 个以 `ChibiccContext *ctx` 为首参: libchibicc 378
  (cst 70, hashmap 9, lib 31, parse 77, preprocess 53, provenance 1, sema 93,
  strings 3, tokenize 29, type 10, unicode 2), codegen 30, self-test 2;
  其余是纯谓词 / 纯输出 / 只读访问器 / 驱动器函数.
- 内部头 71 个原型改为收 ctx.
- 计划的估工是 225/344 个函数与 1863 个调用点; 实测函数更多, 因为 (a) 计划
  的 4.1 纯读例外在传递规则下站不住 (hashmap_get, unicode, provenance,
  `unreachable` 的用户), (b) Pratt 两个模式各有一套定义与前向声明.

## `.a` 内部符号: 签名变化的清单

`libchibicc/src/chibicc.h` 里 71 个内部原型改为以 `ChibiccContext *ctx`
为首参 (静态函数不导出, 不在此列):

```text
add_type array_of array_of_dim chibicc_arena_alloc chibicc_arena_calloc
chibicc_arena_realloc chibicc_arena_strdup chibicc_arena_strndup
chibicc_context_set_base_file conditional const_expr convert_pp_tokens
copy_type cst_dump cst_parse decode_utf8 define_macro display_width
enum_type error error_at error_tok expansion_dump format func_type
get_common_type get_ident get_input_files hashmap_delete hashmap_delete2
hashmap_get hashmap_get2 hashmap_put hashmap_put2 init_macros new_alloca
new_arith new_binary new_cast new_file new_file_len new_long new_node
new_num new_ulong new_unary new_var_node new_vla_ptr nodearray_push
parse parse_init pointer_to preprocess search_include_paths sema
sema_label_gotos sema_state_clear sema_store_label_gotos skip
strarray_push struct_type token_kind_name tokenize tokenize_file
tokenize_string_literal typeof_placeholder undef_macro unique_name_next
usual_arith_conv vla_of warn_tok
```

按 L3.2b 的 `.a` 策略, 这些符号可见但不属于公共 API; 公共头与 D7 未变,
`test/public-api.sh` / `test/install.sh` / `.so` 导出检查全绿.

## 判据 1 的字面 grep 说明

`grep -n '\b_Thread_local\b|\b__thread\b' libchibicc/src libchibicc/include`
不是空的: 8 行命中全部是编译器关键字表里的字符串常量与注释
(`cst.c` 3, `tokenize.c` 1, `parse.c` 4), 不是存储声明. 可执行的判据是
`test/context-audit.sh` 的声明形检查 (0 命中) 与 `nm` 检查; 删掉这些关键字
字符串会破坏 C11 解析, 所以保留.

## 偏离计划的事项 (逐条)

1. **E0 快照重置**: 见 E0.1; 原因是基线快照早于两个行为提交.
2. **cst.c 重命名提交**: 计划未列, 但 `ctx` 参数名与 `CstContext` 冲突.
3. **hashmap_get 家族加 ctx**: 计划 4.1 列为纯读; `get_entry` 的
   `unreachable()` 在 E10 变成 ctx 诊断, 传递规则要求加参.
4. **decode_utf8 / display_width / expansion_dump 加 ctx**: 同一原因
   (`error_at`, `token_kind_name`).
5. **`unreachable()` 宏的两段式迁移**: E2-E9 展开为
   `error(chibicc_ctx(), ...)`, E10 起 `error(ctx, ...)`; 计划没有点名这个
   宏, 但不这样中间态无法在单步内编译.
6. **AST 重写器为一次性工具**: 放在仓库外 (`/tmp`), 未提交; 每步用 clang
   `-ast-dump=json` 枚举定义 / 调用点 / 前向声明, 编译器兜底. 不在仓库留下
   生成器或未使用目标.
7. **E6/E8 的漏点修正**: Pratt 专属节点与 `CHIBICC_TEST_BUILTINS` 的节点在
   第一版 DB 中不可见, 修正后重新生成; 相关提交已 amend 到自洽状态.

## 未覆盖 / 留给后续线

- `codegen.c` 的 `output_file` / `depth` / `current_fn` / cg 表仍是文件域状态
  (计划 1.3 明确不处理); 并发发射不支持.
- 错误模型未动: 无 jmp frame 时 `raise_error` 仍 `exit(1)`.
