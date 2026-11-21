# RESULT.md - 按当前代码校准 docs 执行记录

本文件记录 `PLAN.md` (按当前代码校准 docs) 的实际执行结果. 本线开工 HEAD 为 `c57acbe` (`Record the AGENTS and docs dedup line`), 宿主为 `Linux` / `x86_64` (原生路径), 日期 2026-11-21.

## 用户决定 (2026-11-21)

- 用户要求"根据代码来更新 docs"; 事实来源是当前公共头, 实现, 测试与构建系统, 不是旧计划或旧文档.
- 用户批准本计划的 14 个步骤, 范围边界, 每步闸门与全线验收标准; 本线只改 docs 与 plans, 发现的实现缺陷只记录不修.
- 未决项: 无.

## 提交一览

| 步骤 | hash | subject |
|---|---|---|
| S1 | `aeb769d` | Archive the agents dedup line and add the docs calibration plan |
| S2 | `538d194` | Refresh the README baseline and five-minute example |
| S3 | `34e99ec` | Align the overview design principles with the current tree |
| S4 | `c2890e8` | Correct the architecture contracts, error model, and directory map |
| S5 | `164e724` | Calibrate the token and preprocessor layer docs |
| S6 | `61d112b` | Describe the CST declaration and directive shapes |
| S7 | `dd5ea36` | Correct the parse node counts and field paths |
| S8 | `05ed9a4` | Align the sema field paths and serialization docs |
| S9 | `e0dd3b7` | Document the codegen shaping order and emission branches |
| S10 | `9c061c8` | Correct the public API semantics and examples |
| S11 | `c436f9b` | Document the build, bench, and CI entry points |
| S12 | `9be7a79` | Record the input, macro, and lifecycle limitations |
| S13 | `bdd693d` | Extend the project timeline and historical notes |
| S14 | 本记录所在提交 | Finish the docs calibration sweep |

## 各步记录

### S1 - 归档上一线并建立本线与快照基线

- 做了什么: 从 `HEAD` 取出现行 `plans/PLAN.md` 与 `plans/RESULT.md`, 写入 `plans/PLAN-agents-dedup.md` 与 `plans/RESULT-agents-dedup.md`, 文件头各加两行归档说明; 新建本线 `plans/PLAN.md` (状态改为已批准) 与 `plans/RESULT.md`; 在 `c57acbe` 的临时 worktree `/tmp/libcp-docs-base` 生成 41 份汇编基线 `/tmp/libcp-docs-base-s/*.s`.
- 测试: `uname -s` 为 `Linux`, `uname -m` 为 `x86_64`, 走原生路径; `make test-all` 退出 0; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make format-check` 与 `make check-context` 退出 0; 41 份当前树 `.s` 与基线 `diff -ru` 无输出, 输出 `snapshot diff: empty`; `git diff --check` 通过.
- 偏差: 计划 S1 写的快照命令是 `./chibicc -S <file>`, 实测 `test/macro.c` 的 `#include M13 >` 需要 `-Itest`, 且 `test/macro.c` 会把 `__DATE__` / `__TIME__` 编进字符串, 必须固定. 实际改用 `Makefile` 的 `SNAPSHOT_RUN` 等价命令: `./chibicc -Iinclude -Itest -D__TIMESTAMP__=... -D__DATE__=... -D__TIME__=... -S -o <out> <file>`. 这是测试命令修正, 不影响范围与验收.
- 验收: 归档文件存在且带归档说明; 新 PLAN 与 RESULT 标题正确; 41 份快照 diff 为空; 本步提交只含 `plans/`.

### S2 - `docs/README.md` 基线与状态

- 做了什么: 基线从 `9b06d35` (2026-11-07) 改为 `c57acbe` (2026-11-21); 阶段 5 的函数数改为"阶段 5 时 36 个, L5 后 38 个, 0.2.0 后 39 个"; `tools/fmt.c` 说明补上宿主 `fmt` 与自举 `stage2/fmt` 都不进 `all` / `install`; 五分钟示例的循环加 `if (o->is_function)`, 预期输出改为只列 `global: square, function=1`.
- 测试: `git cat-file -e c57acbe` 通过; `grep` 命中 `0.2.0`, `c57acbe`, `36 个`, `38 个`, `39 个`; 从文档代码块抽出示例编译运行, 输出恰好一行 `global: square, function=1`; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 无.
- 验收: 基线行只出现 `c57acbe`; 36/38/39 的时间线清楚; 示例编译并只输出一行; `docs-links` 通过.

### S3 - `docs/01-overview.md` 设计原则与成熟度

- 做了什么: 1.5.6 删除"链表当集合, 查找 O(n)"的全库概括与"一个 `LibcpNode` 容纳所有字段, 不做 union"; 改写为"token / 对象 / 声明链仍用链表; 名字与 tag 作用域, 宏表, `pragma once`, include guard 用哈希表; `LibcpNode` 是公共头加十个字段族的匿名 union, x86-64 下 144 字节"; 把"解析期间不释放"改为"不显式 `LIBCP_FREE`, `LIBCP_REALLOC` 可能搬移或归还旧块, 对象级释放在 `reset` / `free` 边界". 1.9 改为"根目录的库外 C 源文件只有 `main.c` 与 `codegen.c`, 驱动器内部头是 `driver.h`". 1.10 的 309 改为 313.
- 测试: `! grep -n "不做 union"` 通过; `grep -n "313"` 命中第 181 行; `grep -n "库外 C 源文件"` 命中第 169 行; `grep -n "十个字段族\|144 字节"` 命中第 106 行; `sizeof(LibcpNode)` 实测 144; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 无. 144 字节补了 "x86-64 构建下" 限定, 因为结构体大小随 ABI 变化.
- 验收: "不做 union" 消失; 313 出现且与 `test/cst.sh` 的 313 行 `testing` 一致; 根目录表述只限定库外 C 源; `docs-links` 通过.

### S4 - `docs/02-architecture.md` 层间契约, 生命周期, 错误模型与目录

- 做了什么: 2.4.4 明确 `ND_TYPEDEF` 的声明类型在 `ty`, `ND_CAST` / `ND_DECL` / `ND_GVAR_DECL` / `ND_FUNCDEF` / `ND_ENUM_CONST` 的目标类型在 `ty_op`; 2.4.5 把 `var`, `val`, `generic_sel`, `is_implicit`, `init_resolved` 改为 `decl.var`, `value.val`, `ctrl.generic_sel`, `value.is_implicit`, `decl.init_resolved`; 2.5.3 改为"不显式 `LIBCP_FREE`, `LIBCP_REALLOC` 可能搬移或归还旧块, 逐块释放在 `reset` / `free`"; 2.6.1 补 `libcp_expansion_verify` 直接写 stderr 的例外与默认处理器的两种输出; 2.9 删除"本章后面的例子"; 2.10 把 `AGENTS.md` 的"开放决策"改为"已定边界", 目录地图补 `bench/`, `docs/`, 根级 `Makefile` / `README.md` / `AGENTS.md` / `Dockerfile` / `libcp.pc.in` / `LICENSE`, `plans/` 改为三态描述, `ast.h` 补 `LibcpNodeFamily` 与 `libcp_node_family`, `allocator.h` 补构建期开关.
- 测试: `! grep -n "开放决策"` 与 `! grep -n "本章后面"` 通过; `grep -n "bench/"` 命中第 349 行; `grep -n "decl.var\|value.val\|ctrl.generic_sel"` 命中第 133, 135 行; `grep -n "LIBCP_DEFAULT_ALLOCATOR"` 命中第 321 行; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 无. 目录地图额外补了 `LICENSE`, 它是已跟踪的根级文件.
- 验收: 2.4 的类型字段归属与 `parse.c` 一致; 2.5.3 不再声称零释放; 2.6 明确 verify 例外; 2.10 包含 `bench/`, `docs/`, 根级构建文件与三态 `plans/`; 旧字段名在该节清零; `docs-links` 通过.

### S5 - `docs/03-layer1-token.md` 与 `docs/04-preprocess.md`

- 做了什么: 03 的 3.2.2 / 3.3.2 把 `File.*` 改为 `LibcpFile.*`; 3.3.3 / 3.5 说明 `TK_PP_NUM` 只覆盖 pp-number 数值字面量, 字符常量在词法阶段就是 `TK_NUM` 并带 `val` / `ty`; 3.4.4 / 3.6 / 3.8 把 byte-exact 重建限定为 `tokenize_file` 与已处于扫描缓冲形态的内存输入, 写出原始 `\uXXXX` 会让 `trivia_len` 为负并指向 11.5.8. 04 的 4.4.3 把 provenance 挂接拆成函数宏 `subst` 与内建/对象宏 `expand_macro` 两类; 4.4.2 / 4.8 说明 `#include_next` 用 context 全局 `include_next_idx`, 不是 per-file 栈, 重复调用可能再次命中同一目录.
- 测试: `grep -n "TK_PP_NUM"` 的四处都只描述数值字面量; `grep -n "LibcpFile"` 命中 3.2.2 / 3.3.2; 无裸 `File.`; `grep -n "subst\|expand_macro"` 命中两类挂接; `grep -n "include_next_idx"` 命中 4.4.2 与 4.8; `/tmp/ucn_probe.c` 对 `"\\u00e9 x\n"` 实测第二个 token 的 `trivia_len = -5`; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 无. 11.5.8 在 S12 才新增, 本步先建立引用, 全线验收时该子节已存在.
- 验收: 03 不再把字符常量写成 pp-num; 03 的 byte-exact 声明有输入形态限定; 04 的 provenance 挂接分三类; 04 的 `#include_next` 不再是 per-file 栈描述; `docs-links` 通过.

### S6 - `docs/05-layer2-cst.md` 声明形状与指令节点

- 做了什么: 5.4.3 改为"声明拆成 `CST_DECL_SPECS`, 一个或多个 `CST_DECLARATOR`, 分号; 有初始化器时该声明符包在 `CST_INIT_DECLARATOR` 中"; 5.4.4 补 `#include_next` 在 `cst_pp_kind` 中没有专门分支, 落到 `CST_PP_DIRECTIVE`; 5.9 的 typedef 限制补 `libcp_cst_seed_typedef` 预置, 并说明该配置跨 reset 保留.
- 测试: `./chibicc -dump-cst` 对 `int x;` 输出 `DECL -> DECL_SPECS, DECLARATOR, ;`, 对 `int y = 1;` 输出 `DECL -> DECL_SPECS, INIT_DECLARATOR(DECLARATOR, =, PRIMARY), ;`; `grep -n "CST_PP_DIRECTIVE"` 命中 5.4.4; `grep -n "seed_typedef"` 命中 5.2.4 示例与 5.9; CST 种类计数 57 不变; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 无.
- 验收: 无初始化器声明不再写成 `CST_INIT_DECLARATOR`; `#include_next` 的落点明确; `seed_typedef` 缓解存在; 57 种节点计数不变; `docs-links` 通过.

### S7 - `docs/06-layer3-parse.md` 计数, 字段路径与示例

- 做了什么: 6.3.1 把解析器产出从 66 改为 65, 不产出列表从 3 种改为 4 种并加入 `ND_NULL_EXPR`, 写明"46 个直接构造 + 21 个 `infix_ops[].kind` - 2 个重复 = 65"的计数方法; 6.2.1 / 6.2.2 / 6.2.3 / 6.2.4 / 6.4.3 / 6.4.4 把 `name_tok`, `attr`, `decl_init`, `body`, `op`, `is_post`, `is_elvis`, `arrow_tok`, `is_implicit`, `Member.width_expr` 改为字段族路径; 6.7.2 / 6.7.4 示例改用 `decl.name_tok` / `decl.body` / `decl.decl_init`; 6.8 改写 `ND_IDENT.var`, 并把文法诊断写为 A 组 15 例 / 21 个调用点 / 17 条消息.
- 测试: 用脚本合并 `libcp_new_node` / `libcp_new_unary` / `libcp_new_binary` 的 46 个 kind 与 `infix_ops[].kind` 的 21 个, 去重 2 个后为 65; 69 减去不产出的 `ND_VAR` / `ND_VLA_PTR` / `ND_MEMZERO` / `ND_NULL_EXPR` 也为 65; `awk` 统计 `test/diagnostic.sh` 的 A 组 15 个 `snippet`; `parse.c` 有 21 个 `libcp_error_tok` 调用点与 17 条消息; `/tmp/parse_example.c` 用 `cc -std=c11 -fsyntax-only -Wall -Wextra -Ilibcp/include` 通过; `! grep -n "66 种"` 通过; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 计划写"46 个字面量构造种类加 19 个 `infix_ops[].kind`"; 实测 `infix_ops[].kind` 去重后是 21 个, 与 46 个直接构造种类有 `ND_ASSIGN` / `ND_COND` 两个重复, 因此文档按 46 + 21 - 2 = 65 写. 这是计数方法修正, 结论 65 不变.
- 验收: 69 与 65 的口径分开写清; `ND_NULL_EXPR` 在不产出列表; 06 中不再有裸 `name_tok` / `body` / `decl_init` / `ND_IDENT.var`; 示例通过语法检查; `docs-links` 通过.

### S8 - `docs/07-layer4-sema.md` 字段, 布局与序列化

- 做了什么: 7.2.1 把成员偏移从 `LibcpType` 移到 `LibcpMember.offset` / `LibcpMember.bit_offset`; 7.3.2 补 `LibcpType` 的其余字段与 `LibcpMember` 的 `next` / `tok` / `width_expr`; 7.4.1 示例改用 `n->decl.body` / `n->decl.var`; 7.4.4 说明 `layout_union` 只取最大 size/align, 位域按底层类型参与, 匿名成员按普通成员参与; 7.4.5 说明绑定改写发生在 resolve pass, `libcp_add_type` 的 `ND_STRING` 分支只物化匿名全局并设置 `decl.var`; 7.4.6 区分 `char a[] = "abc"` 的逐元素字节与 `char *p = "abc"` 的 `LibcpRelocation`; 7.5 / 7.6 的 `init_data`, `init_resolved`, `decl_init`, `ND_SIZEOF.val`, `ND_GENERIC.generic_sel` 改为字段族路径.
- 测试: `grep -nP 'LibcpType[^.]*\.(offset|bit_offset)'` 无命中; `! grep -n "n->body\|n->var"` 通过; `/tmp/sema_example.c` 用 `cc -std=c11 -fsyntax-only -Wall -Wextra -Ilibcp/include` 通过; `layout_union` 实现实测只比较 `mem->align` 与 `mem->ty->size`; `write_gvar_data` 的数组分支逐元素写, 指针路径走 `LibcpRelocation`; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 无. 额外把同文件中残留的裸 `init_resolved` / `decl_init` / `init_data` 统一成字段族路径.
- 验收: 成员偏移只出现在 `LibcpMember`; 字段表补全或明确为关键字段; 示例可编译; 字符串序列化区分两种路径; `layout_union` 不再声称专门处理位域/匿名成员; `docs-links` 通过.

### S9 - `docs/08-codegen.md` 整形遍与发射

- 做了什么: 8.5.1 按实现改写六条不变量: 表达式/声明改写后序, 循环与 switch 在进入子树前分配标签 (前序), 每函数上下文, 不重入, 嵌套函数只访问一次, 初始化器走 `shape_init_exprs`; 补一句 `codegen.c` 文件头的 (a)/(b) 注释与实现不一致. 8.5.2 把 `op != 0` / `arrow_tok` / `is_elvis` 改为 `assign.op` / `access.arrow_tok` / `ctrl.is_elvis`, `case` 链改为 `case_`, 后缀 `++` 改为 `new_add(..., -addend)` 反向加减并 cast, 新增"声明记录链过滤"与"VLA 声明"两行, `goto` 行补 `ND_LABEL_VAL`. 8.5.3 把指针缩放拆成发射点与整形遍两条路径. 8.5.4 写清 `label_base` 与局部 `label_seq`, 以及发射器的 `.L.begin` / `.L.else` / `.L.end` / `.L.false` / `.L.true` / `.L.return` 与 `count()` 数字标签. 8.5.5 改为 4096 桶指针键哈希表. 8.6.1 写清 `.comm`, `.data` / `.tdata`, `.bss` / `.tbss` 三类全局. 8.6.2 补 `is_live == false` 的 static inline 过滤, 分开描述 `new_add` / `new_sub`. 8.6.4 写明 `-g` / `-O*` / `-W*` 接受但忽略. 8.8.1 补字段族与 `libcp_node_family`, 忠实节点补 `ND_LABEL_VAL`.
- 测试: 对照 `codegen.c` 的 `shape_node` / `shape_chain` / `expand_decl` / `new_inc_dec` / `new_add` / `new_sub` / `emit_data` / `emit_text` / `resolve_label_refs`; `grep -n "assign.op\|access.arrow_tok\|ctrl.is_elvis"` 命中; `grep -n "is_live"` 命中; `grep -n "4096"` 命中; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 无. `codegen.c` 文件头 (a)/(b) 注释不改, 只在文档与 RESULT 后续项记录.
- 验收: 六条不变量与实现一致且明确说明前序标签; 8.5.2 覆盖链过滤与 VLA 展开; 指针缩放归属正确; 标签, 全局发射, `is_live`, `-g` 描述正确; 旧字段清零; `docs-links` 通过.

### S10 - `docs/09-api-reference.md` API 语义与示例

- 做了什么: 9.3.3 的 reset 清单补 stdin 缓存; 9.3.4 补 `libcp_expansion_verify` 直接写 stderr 的例外; 9.3.7 / 9.9.2 记录 `context_define` 对象宏的 `macro` 名是 `NAME=body` 并指向 11.7.8; 9.4.1 把 stdin 缓存限定为同一 TU, reset 后读到空流; 9.4.2 改为 `tokenize_buffer` 用 `strlen`, 内嵌 NUL 静默截断, 要检测 NUL 用 `_len`; 9.4.4 的 byte-exact 声明补输入形态限定; 9.6.5 区分普通校验失败与内部错误边界; 9.7.2 补 `access.arrow_tok`, `case_.colon_tok` 与空子树 NULL 行为; 9.11 限定 `cst_verify` 与 `context_*` 行; 9.12 把"reset 后一切失效"改为带静态 name/value 例外; 9.14.3 过滤隐藏返回缓冲区参数; 9.14.8 补 C++ `char *` 限制.
- 测试: `/tmp/define_probe.c` 实测 `context_define("FOO","42")` 的 provenance `macro` 为 `FOO=42`; `/tmp/nul_probe.c` 实测 `tokenize_buffer` 3 个 token 且 `failed=0`, `_len` 返回 NULL 且 `failed=1`; `/tmp/stdin_probe.c` 实测 reset 前 3 个 token, reset 后 0 个; `/tmp/api_example.c` 通过 `cc -fsyntax-only -Wall -Wextra`; `! grep -n "reset 后一切失效"` 通过; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 无. 9.4.4 与 9.12 一并修正, 使 byte-exact 与生命周期结论与 03 / 9.3.10 一致.
- 验收: 09 的每条 API 语义与头文件或实测一致; `tokenize_buffer` NUL 与 stdin 生命周期不再误导; `context_define` 缺陷有链接; `cst_verify` / `context_*` 返回值行正确; source range 锚点完整; 示例可编译; `docs-links` 通过.

### S11 - `docs/10-build-install.md` 构建, 测试, bench 与 CI

- 做了什么: 10.2 补 `BENCH_CFLAGS` 与 `bench/libcp_bench.a` / `bench/libcp-bench` / `bench/obj` 产物; 10.3 补 `bench-tool` / `bench`, `fmt-tool` 注明产出 `fmt`, `format` 注明低于 20 失败, `test` 注明不含 `check-library` / thirdparty / bench; 10.4 注明安装路径都在 `$(DESTDIR)$(PREFIX)` 下; 10.6.1 改为"全部已定义全局符号", 10.6.2 改为 `nm -D --defined-only`; 10.7 补 `test/self-test.sh`, 跳过列表补 `context-audit.sh` / `docs-links.sh`, 说明 `stage2/fmt` / `stage2/c2c` 由宿主 `cc` 链接 `stage2/libcp.a`; 10.8 说明表按类别而非顺序, `docs-links.sh` 第一条, 补 `fmt-tool` / `c2c` 构建步骤与 `fmt.sh` 的 `-S` 条件, 新增 10.8.1 benchmark; 10.9 补 `check-library` 前置构建, `check-context` 段落, `library.sh` 的共享库消费者与 64 次 `CST -> reset` 的 32 KiB 上界; 10.10 区分只读与读写挂载, 补本机 `test-thirdparty`, 新增 10.10.1 CI 闸门; 10.11.3 补分配器开关与 `LIBCP_INTERNAL`; 10.11.5 限定 `test/allocator.sh` 只验证替换机制.
- 测试: `grep -n "10.8.1\|bench-tool\|check-context\|32 KiB\|--defined-only\|docker-test-thirdparty\|cpython"` 命中; `bash -n bench/run.sh` 通过; `make bench-tool` 退出 0; `nm -D --defined-only libcp.so | wc -l` 为 39; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 无. CI 表把 `test.yml` 的两个 job 分成两行, 便于区分单元闸门与 thirdparty 依赖关系.
- 验收: bench 目标与测量项完整; 10.3 表与 `Makefile` 一致; `check-library` / `check-context` 有前置与正文; Docker 挂载描述分目标; CI 章节存在; 嵌入命令可编译; `docs-links` 通过.

### S12 - `docs/11-limitations.md` 限制与已知缺陷

- 做了什么: 11.2.1 改为 per-TU 指针失效并补静态表例外; 11.5.6 把 stdin 缓存限定为同一 reset 周期; 11.5.7 拆分 `tokenize_file` / `_len` 报 NUL 与 `tokenize_buffer` 静默截断; 新增 11.5.8 原始 `\uXXXX` 破坏 raw 坐标; 新增 11.7.8 `-D` / `context_define` 的 provenance 宏名带 `=body`; 11.8.2 补 `libcp_cst_seed_typedef`; 11.9.1 改写 `ND_IDENT.var`; 11.9.3 标题改为 `decl.init_resolved`; 11.11.3 限定字符串类参数为 `char *`; 新增 11.11.5 `context.h` 注释与实现不一致; 11.12 删除"字符串驻留"; 11.14 增加四行.
- 测试: `/tmp/ucn_probe.c` 复现 `trivia_len=-5`; `/tmp/define_probe.c` 复现 `macro=FOO=42`; `/tmp/nul_probe.c` 复现 `buffer` 静默截断与 `_len` 报错; `/tmp/stdin_probe.c` 复现 reset 后空流; `! grep -n "字符串驻留"` 与 `! grep -n "ND_IDENT.var"` 通过; `grep -n "11.5.8\|11.7.8\|11.11.5"` 命中; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 无. 11.2.1 也按同一例外修正, 避免与 11.11.5 / docs/09 冲突.
- 验收: 四条新缺陷有观察行为, 冲突契约, 规避与建议; 旧字段清零; `seed_typedef` 缓解存在; 11.14 表覆盖; `docs-links` 通过.

### S13 - `docs/12-project-stages.md` 时间线与阶段事实

- 做了什么: 12.2 时间线补 2026-10-26 c2c 回归闸门, 2026-11-01 类型名统一, 2026-11-10 benchmark, 2026-11-19 per-TU 分配替换 arena, 2026-11-21 字段族, 2026-11-21 AGENTS/docs 去重; 12.8 的 309 改为 313, 61/65 dump 样本口径与 `test/cst.sh` 对齐; 12.10 的 L2 / L3.0 与 arena 对齐缺陷标注为当时实现, 并说明 2026-11-19 已被 per-TU 分配替换; 12.15 增加字段族与 per-TU 分配两行; 12.16 的 `plans/` 描述改为现行 / 归档 / 模板三态.
- 测试: `git cat-file -e` 对 `53785b9` / `7dc2126` / `b70287e` / `505d0bf` / `c57acbe` 全部通过; `git log -1 --date=short` 的日期与时间线一致; `grep -n "2026-11"` 命中五条; `grep -n "arena"` 每处都有"当时"或"已替换"限定; `grep -n "309"` 无命中, `grep -n "313"` 命中; `grep -n "PLAN-TEMPLATE"` 命中; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; 41 份快照 diff 为空.
- 偏差: 计划把 309 的位置写成 12.5, 实际在 12.8 (CST 阶段); 按实际位置修正, 结论不变. c2c 终态提交 `754b0cd` 的日期是 2026-10-26, 与计划一致.
- 验收: 时间线含 2026-11 五条以上; arena 不再被写成现行; 309 清零; 12.15 含字段族与分配模型; `plans/` 三态描述正确; `docs-links` 通过.

### S14 - 全线一致性校对与 RESULT 收尾

- 做了什么: 对全 `docs/` 做旧口径与字段 sweep, 修掉 `docs/12` 历史段残留的裸 `File.*`; 在 `docs/12` 时间线追加本线 (终态 `plans/RESULT.md`); 补齐提交表, 全线验收对照, 偏差与后续项; 重跑全线闸门.
- 测试: 39 个公共函数 / 57 个 `CST_*` / 69 个 `ND_*` / 65 个解析器产出 / 47 个预定义宏 / 313 次 CST check / 68 例诊断全部由脚本或测试输出复核; `! grep -rn "不做 union\|66 种\|309\|字符串驻留\|ND_IDENT.var"` 通过; `! grep -rnP '(?<!Libcp)(Obj|File)\.'` 只剩 codegen 私有的 `CgObj.*`; `bash test/docs-links.sh` 输出 `docs-links: ok (48 references checked)`; `make test-all` 退出 0; `make format-check` 与 `make check-context` 退出 0; 41 份快照 diff 为空; `git diff --check` 通过; `git diff --stat c57acbe..HEAD -- ':!docs' ':!plans'` 为空.
- 偏差: sweep 发现 `docs/12` 的 trivia 历史段仍用裸 `File.*`, 本步改为 `LibcpFile.*`; 这是收尾修正, 不改变计划范围.
- 验收: 全线闸门全绿; 旧字段与旧计数清零; `docs-links` 通过; `git diff` 只含 `docs/` 与 `plans/`; RESULT 完整且包含代码缺陷后续项.

## 验收

S1 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| 归档文件 | 通过 | `plans/PLAN-agents-dedup.md` 193 行, `plans/RESULT-agents-dedup.md` 185 行, 各带两行归档说明 |
| 新 PLAN / RESULT | 通过 | 标题分别为 `PLAN.md - 按当前代码校准 docs 执行计划` 与 `RESULT.md - 按当前代码校准 docs 执行记录` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与 `c57acbe` 基线 diff 为空 |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make format-check` | 通过 | 退出 0 |
| `make check-context` | 通过 | accessors 0, `cg_ctx` 0, accessor symbols 0 |
| 改动范围 | 通过 | 本步提交只含 `plans/` |

S2 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| 基线 | 通过 | `docs/README.md` 只写 `c57acbe` (2026-11-21); `git cat-file -e c57acbe` 通过 |
| 函数计数 | 通过 | 阶段 5 36 个, L5 后 38 个, 0.2.0 后 39 个; 与 `test/public-api.sh` 的 39 个 D7 名单一致 |
| 五分钟示例 | 通过 | 从文档抽出的示例编译运行, 输出恰好 `global: square, function=1` |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与基线 diff 为空 |

S3 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| "不做 union" 清零 | 通过 | `! grep -n "不做 union" docs/01-overview.md` |
| 字段族事实 | 通过 | 十个族名与 `libcp/include/libcp/ast.h` 的 union 一致; `sizeof(LibcpNode)` 实测 144 |
| CST check 计数 | 通过 | 313 与 `test/cst.sh` 的 313 行 `testing` 一致 |
| 根目录表述 | 通过 | 只限定"库外 C 源文件"为 `main.c` 与 `codegen.c` |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与基线 diff 为空 |

S4 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| 2.4 字段归属 | 通过 | `ND_TYPEDEF` 用 `ty`, 其余五个记录与 `ND_CAST` 用 `ty_op`, 与 `parse.c` 一致 |
| 2.5.3 释放语义 | 通过 | 改为"不显式 `LIBCP_FREE`", 保留 `reset` / `free` 逐块释放 |
| 2.6 错误模型 | 通过 | `libcp_expansion_verify` 的 stderr 例外与默认处理器两种输出都写明 |
| 2.10 目录地图 | 通过 | 含 `bench/`, `docs/`, 根级构建文件与三态 `plans/`; `ast.h` 与 `allocator.h` 条目已补 |
| 旧字段与旧引用 | 通过 | `开放决策` 与 `本章后面` 清零; `init_resolved` / `generic_sel` / `is_implicit` 只以族路径出现 |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与基线 diff 为空 |

S5 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| 字符常量种类 | 通过 | `tokenize.c` 的 `read_char_literal` 产出 `TK_NUM`; 文档不再把它算进 `TK_PP_NUM` |
| `LibcpFile` 字段名 | 通过 | 3.2.2 四处改为 `LibcpFile.*`; 无裸 `File.` |
| byte-exact 输入限定 | 通过 | 3.4.4 / 3.6 / 3.8 写明原始 `\uXXXX` 例外并指向 11.5.8 |
| UCN 实测 | 通过 | `/tmp/ucn_probe.c` 输出第二个 token `trivia_len=-5` |
| provenance 归属 | 通过 | 函数宏在 `subst`, 内建/对象宏在 `expand_macro` |
| `#include_next` | 通过 | 4.4.2 表与 4.8 都写 context 全局 `include_next_idx` |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与基线 diff 为空 |

S6 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| 声明形状 | 通过 | `-dump-cst` 显示无初始化器是 `DECLARATOR`, 有初始化器是 `INIT_DECLARATOR` |
| `#include_next` 落点 | 通过 | `cst_pp_kind` 无专门分支; 文档写 `CST_PP_DIRECTIVE` |
| `seed_typedef` 缓解 | 通过 | 5.9 写明预置方式与跨 reset 保留 |
| CST 种类计数 | 通过 | `cst.h` 枚举 57 项, 文档计数不变 |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与基线 diff 为空 |

S7 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| 69 / 65 计数 | 通过 | 69 个枚举 kind; 直接构造 46 + infix 21 - 重复 2 = 65 |
| 不产出列表 | 通过 | `ND_VAR`, `ND_VLA_PTR`, `ND_MEMZERO`, `ND_NULL_EXPR` 四种 |
| 字段路径 | 通过 | 06 中的声明字段都写成 `decl.*`, `access.*`, `ctrl.*`, `value.*`, `incdec.*` |
| 示例可编译 | 通过 | `/tmp/parse_example.c` 通过 `cc -fsyntax-only -Wall -Wextra` |
| 诊断计数 | 通过 | A 组 15 例; `parse.c` 21 个调用点 / 17 条消息 |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与基线 diff 为空 |

S8 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| 成员偏移归属 | 通过 | 只出现在 `LibcpMember.offset` / `LibcpMember.bit_offset` |
| 字段表 | 通过 | `LibcpType` 其余字段与 `LibcpMember` 的 `next` / `tok` / `width_expr` 已列 |
| 示例可编译 | 通过 | `/tmp/sema_example.c` 通过 `cc -fsyntax-only -Wall -Wextra` |
| 字符串序列化 | 通过 | 数组逐元素写字节; 指针写 `LibcpRelocation` |
| `layout_union` | 通过 | 只取最大 size/align, 无位域/匿名成员专门分支 |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与基线 diff 为空 |

S9 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| 六条不变量 | 通过 | 表达式后序, 循环/switch 前序, 每函数上下文, 不重入, 只访问一次, `shape_init_exprs` |
| 注释不同步 | 通过 | 文档写明 `codegen.c` 文件头 (a)/(b) 与实现不一致, 未改源码 |
| 8.5.2 覆盖 | 通过 | 字段族路径, `case_`, 后缀 `new_add(-addend)`, 链过滤, VLA 展开, `ND_LABEL_VAL` |
| 指针缩放 | 通过 | 普通算术/下标在发射点, `op=` 与 `++/--` 在整形遍 |
| 标签 | 通过 | `label_base` + `label_seq`; 发射器 `.L.*` 与 `count()` 数字标签 |
| 全局发射 | 通过 | `.comm`, `.data` / `.tdata`, `.bss` / `.tbss` |
| `is_live` 与 `-g` | 通过 | static inline 过滤; `-g` / `-O*` / `-W*` 接受但忽略 |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与基线 diff 为空 |

S10 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| stdin 生命周期 | 通过 | reset 清单含 stdin 缓存; 实测 reset 前 3 token, reset 后 0 token |
| expansion_verify 例外 | 通过 | 9.3.4 写明直接写 stderr |
| `context_define` 缺陷 | 通过 | 实测 `macro=FOO=42`; 9.3.7 / 9.9.2 指向 11.7.8 |
| `tokenize_buffer` NUL | 通过 | 实测 `buffer` 静默截断且 `failed=0`; `_len` 报错且 `failed=1` |
| `cst_verify` 返回值 | 通过 | 普通 false 不置 `failed`; 内部边界才可能 false + `failed` |
| source range 锚点 | 通过 | 文档含 `decl.name_tok` / `access.arrow_tok` / `case_.colon_tok` / `spec_decls` 与 NULL 行为 |
| 示例可编译 | 通过 | `/tmp/api_example.c` 通过 `cc -fsyntax-only -Wall -Wextra` |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与基线 diff 为空 |

S11 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| bench 目标 | 通过 | 10.2 / 10.3 / 10.8.1 含 `bench-tool`, `bench`, 四组语料, 分阶段指标与 `BENCH_REPEAT` 默认 5 |
| `make bench-tool` | 通过 | 退出 0, 产出 `bench/libcp_bench.a` 与 `bench/libcp-bench` |
| 10.3 表 | 通过 | 与 `Makefile` 目标一致, 含 bench 与 fmt 产物 |
| check-library / check-context | 通过 | 前置构建与脚本正文都已写 |
| Docker 挂载 | 通过 | `docker-test` / `docker-snapshot*` 只读, `docker-test-thirdparty` 读写并回写缓存 |
| CI 章节 | 通过 | 10.10.1 列出 `test.yml` / `format.yml` / `docs.yml`; cpython 仅手工 Docker |
| 嵌入命令 | 通过 | 10.11.3 要求 `-DLIBCP_DEFAULT_ALLOCATOR` 或四个宏; `LIBCP_INTERNAL` 来源已写 |
| 导出符号 | 通过 | `nm -D --defined-only libcp.so | wc -l` 为 39 |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与基线 diff 为空 |

S12 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| stdin 生命周期 | 通过 | 11.5.6 限定同一 reset 周期; 11.14 补 reset 后空流 |
| NUL 行为拆分 | 通过 | `tokenize_file` / `_len` 报错; `tokenize_buffer` 静默截断 |
| UCN raw 坐标 | 通过 | 11.5.8 记录观察, 契约冲突, 规避与修复方向 |
| `-D` provenance 名 | 通过 | 11.7.8 记录 `FOO=42` 与修复方向 |
| `seed_typedef` | 通过 | 11.8.2 写明预置与跨 reset 保留 |
| 旧字段与旧机制 | 通过 | `ND_IDENT.var` 与"字符串驻留"清零 |
| `context.h` 注释 | 通过 | 11.11.5 记录冲突并以 docs/09 9.3.10 / 9.12 为准 |
| 11.14 速查表 | 通过 | 增加 NUL, `-D` 宏名, reset 后 stdin, UCN raw 坐标四行 |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与基线 diff 为空 |

S13 阶段验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| 时间线 | 通过 | 补 2026-10-26 与 2026-11 五条; 提交日期与 `git log` 一致 |
| CST 计数 | 通过 | 309 清零, 313 与 `test/cst.sh` 一致 |
| arena 时态 | 通过 | L2 / L3.0 / 对齐缺陷都标为当时实现或已替换 |
| 12.15 现状表 | 通过 | 增加字段族与 per-TU 分配两行 |
| `plans/` 三态 | 通过 | 现行 / 归档 / 模板 |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与基线 diff 为空 |

全线验收:

| 项目 | 结果 | 证据 |
|---|---|---|
| `make test-all` | 通过 | 退出 0, 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered` |
| `make format-check` | 通过 | 退出 0 |
| `make check-context` | 通过 | accessors 0, `cg_ctx` 0, accessor symbols 0 |
| `bash test/docs-links.sh` | 通过 | `docs-links: ok (48 references checked)` |
| 汇编快照 diff | 通过 | 41 份 `.s` 与 `c57acbe` 基线 diff 为空 |
| `git diff --check` | 通过 | 无空白错误 |
| 改动范围 | 通过 | `git diff --stat c57acbe..HEAD` 只含 `docs/` 与 `plans/` |
| 代码零改动 | 通过 | `git diff --stat c57acbe..HEAD -- ':!docs' ':!plans'` 为空 |
| 计数一致 | 通过 | 39 个公共函数, 57 个 `CST_*`, 69 个 `ND_*`, 65 个解析器产出, 47 个预定义宏, 313 次 CST check, 68 例诊断 |
| 旧口径清零 | 通过 | `不做 union`, `66 种`, `309`, `字符串驻留`, `ND_IDENT.var` 均无命中 |
| 提交与计划对应 | 通过 | S1-S14 共 14 个提交, 与 PLAN 的 14 步一一对应 |

## 偏差与原因

- S1: 计划里的快照命令缺少 `-Itest` 与固定时间宏, 第一次生成在 `test/macro.c` 失败并留下部分 `.s`. 原因: 计划沿用了上一线的简写, 没有抄全 `Makefile` 的 `SNAPSHOT_RUN`. 处理: 清空临时目录, 按 `Makefile` 的命令重新生成基线. 影响: 不阻塞后续步骤; 后续每步沿用同一命令.
- 归档文件在 Git 中显示为新增, 固定槽位 `plans/PLAN.md` 与 `plans/RESULT.md` 显示为修改, 因为归档后立即复用了原路径. 归档正文与旧内容一致, 只多头部四行.
- S2: 无偏差.
- S3: 无偏差. 字段族大小写成 "x86-64 构建下", 因为 `sizeof(LibcpNode)` 随 ABI 变化.
- S4: 无偏差. 目录地图补了计划未列出的 `LICENSE`, 因为它是已跟踪的根级文件.
- S5: 无偏差. 03 的 11.5.8 引用在 S12 新增该子节前暂不可达; 全线验收时会补齐.
- S6: 无偏差.
- S7: 计划写 `infix_ops[].kind` 为 19 个, 实测去重后 21 个; 文档按 46 + 21 - 2 = 65 写, 结论不变.
- S8: 无偏差. 同文件残留的裸 `init_resolved` / `decl_init` / `init_data` 一并改为字段族路径.
- S9: 无偏差. `codegen.c` 文件头 (a)/(b) 注释与实现不一致, 本线只记录, 不改源码.
- S10: 无偏差. 9.4.4 与 9.12 一并修正, 使 byte-exact 与生命周期结论与 03 / 9.3.10 一致.
- S11: 无偏差. CI 表按 job 拆行, 同一个 workflow 可能出现两次.
- S12: 无偏差. 11.2.1 一并补上静态表例外.
- S13: 计划把 309 的位置写成 12.5, 实际在 12.8; 已按实际位置修正, 结论不变.
- 无范围变更.

## 后续影响

- 本线只改 `docs/` 与 `plans/`; 以下实现或头文件缺陷只记录, 待另立计划, 是否修由用户拍板:
  1. `libcp_tokenize_buffer` 用 `strlen` 求长度: 内嵌 NUL 静默截断 (`libcp/src/lib.c:361`). 文档 9.4.2 / 11.5.7 已按当前行为写. 修复方向: 保留 `strlen` 语义并要求调用者用 `_len`, 或新增显式长度入口; 两者都是 API 决策.
  2. `-D` / `libcp_context_define` 的 provenance `macro` 名带 `=body` (`libcp/src/lib.c:172-207` 与 `libcp/src/preprocess.c:418`). 修复方向: `add_macro` 复制 name, 或在 `define_macro` 入口复制; 修好后 `libcp_expansion_verify` 的名字一致性检查才能覆盖 `-D`.
  3. 原始 `\uXXXX` 的内存输入破坏 raw 坐标 (`libcp/src/tokenize.c` 的 `raw_end_of` / `new_token`). 修复方向: `raw_end_of` 只在扫描缓冲形态按 UCN 计算, 或统一走 `make_scan_buffer`.
  4. reset 清空 stdin 缓存, reset 后第二次 `tokenize_file("-")` 读到空流. 文档 9.4.1 / 11.5.6 已写明; 是否改为跨 reset 保留缓存是行为决策.
  5. `context.h` 的预定义宏生命周期注释与实现不一致 (`libcp/include/libcp/context.h:43-48`); 建议改注释, 或在 reset 时把静态项复制进 per-TU 分配.
  6. `codegen.c` 文件头 (a)/(b) 注释与实现不一致; 建议改注释, 不改整形顺序.
  7. `test/fmt.sh` 顶部注释仍说 `-S` 等价只在容器内跑, 实际有系统头时也跑; 建议改注释 (本线不改测试脚本).
- 41 份汇编基线固定在 `/tmp/libcp-docs-base-s`; 每步用同一命令重建 `/tmp/libcp-docs-cur-s` 并 diff.

## 闸门实测

### S1

- `uname -s` / `uname -m`: `Linux` / `x86_64`.
- `make test-all`: 退出 0, 末尾为 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 基线快照: `c57acbe` 临时 worktree `/tmp/libcp-docs-base` 生成 41 份 `/tmp/libcp-docs-base-s/*.s`.
- 当前树快照: 41 份 `/tmp/libcp-docs-cur-s/*.s`; `diff -ru /tmp/libcp-docs-base-s /tmp/libcp-docs-cur-s` 无输出, 输出 `snapshot diff: empty`.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make format-check`: 退出 0.
- `make check-context`: 退出 0; accessors 0, `cg_ctx` 0, accessor symbols 0.
- `git diff --check`: 通过.
- 归档行数: `plans/PLAN-agents-dedup.md` 193, `plans/RESULT-agents-dedup.md` 185; 旧文件分别为 191 与 183, 均只多头部四行.

### S2

- `git cat-file -e c57acbe`: 通过.
- `grep -n "0.2.0\|c57acbe\|36 个\|38 个\|39 个" docs/README.md`: 命中第 9, 10, 12 行.
- 文档示例: 抽出代码块编译, 运行输出一行 `global: square, function=1`, 与预期 diff 为空.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.

### S3

- `! grep -n "不做 union" docs/01-overview.md`: 通过.
- `grep -n "313" docs/01-overview.md`: 第 181 行, `test/cst.sh` 共 313 次 check.
- `grep -n "库外 C 源文件" docs/01-overview.md`: 第 169 行.
- `sizeof(LibcpNode)`: 144 (x86-64).
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.

### S4

- `! grep -n "开放决策" docs/02-architecture.md`: 通过.
- `! grep -n "本章后面" docs/02-architecture.md`: 通过.
- `grep -n "bench/" docs/02-architecture.md`: 第 349 行.
- `grep -n "decl.var\|value.val\|ctrl.generic_sel" docs/02-architecture.md`: 第 133, 135 行.
- `grep -n "LIBCP_DEFAULT_ALLOCATOR" docs/02-architecture.md`: 第 321 行.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.

### S5

- `grep -n "TK_PP_NUM" docs/03-layer1-token.md`: 第 77, 150, 192, 228 行, 只描述 pp-number 数值字面量; 第 228 行补字符常量例外.
- `grep -n "LibcpFile" docs/03-layer1-token.md`: 第 42-45, 97, 121, 165, 167 行; 无裸 `File.`.
- `grep -n "subst\|expand_macro" docs/04-preprocess.md`: 第 136, 177, 183, 189 行; 函数宏与内建/对象宏分开.
- `grep -n "include_next_idx" docs/04-preprocess.md`: 第 151, 375 行.
- UCN 复现: `/tmp/ucn_probe.c` 对 `"\\u00e9 x\n"` 输出 `kind=1 raw_len=6`, 随后 `kind=0 trivia_len=-5`.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.

### S6

- `./chibicc -dump-cst /tmp/cst1.c`: `DECL` 下是 `DECL_SPECS`, `DECLARATOR`, `;`.
- `./chibicc -dump-cst /tmp/cst2.c`: `DECL` 下是 `DECL_SPECS`, `INIT_DECLARATOR(DECLARATOR, =, PRIMARY)`, `;`.
- `grep -n "CST_PP_DIRECTIVE" docs/05-layer2-cst.md`: 第 98, 186 行; 第 186 行写明 `#include_next` 的落点.
- `grep -n "seed_typedef" docs/05-layer2-cst.md`: 第 156, 311, 325 行.
- CST 种类: `cst.h` 枚举 57 项.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.

### S7

- 节点计数: `ast.h` 69 个 `ND_*`; 直接构造 46 个, `infix_ops[].kind` 21 个, 重复 `ND_ASSIGN` / `ND_COND` 两个, 合并 65 个; 不产出 `ND_VAR` / `ND_VLA_PTR` / `ND_MEMZERO` / `ND_NULL_EXPR`.
- 诊断计数: `test/diagnostic.sh` A 组 15 个 `snippet`; `parse.c` 21 个 `libcp_error_tok` 调用点, 17 条不同消息.
- 示例: `/tmp/parse_example.c` 通过 `cc -std=c11 -fsyntax-only -Wall -Wextra -Ilibcp/include`.
- `! grep -n "66 种" docs/06-layer3-parse.md`: 通过.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.

### S8

- `grep -nP 'LibcpType[^.]*\.(offset|bit_offset)' docs/07-layer4-sema.md`: 无命中.
- `! grep -n "n->body\|n->var" docs/07-layer4-sema.md`: 通过.
- `/tmp/sema_example.c`: `cc -std=c11 -fsyntax-only -Wall -Wextra -Ilibcp/include` 通过.
- `layout_union`: `sema.c` 只比较 `mem->align` 与 `mem->ty->size`, 再 `libcp_align_to`.
- 字符串路径: `write_gvar_data` 的 `TY_ARRAY` 分支逐元素递归, 指针分支经 `eval2` 写 `LibcpRelocation`.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.

### S9

- 不变量对照: `shape_node` 对循环/switch 先取标签再 `shape_children`; 表达式改写先 `shape_children`; `shape_init_exprs` 显式遍历初始化器.
- 字段与分支: `grep -n "assign.op\|access.arrow_tok\|ctrl.is_elvis" docs/08-codegen.md` 命中第 113, 119, 195 行.
- `is_live`: `codegen.c` 的 `emit_text` 跳过 `!fn->is_live`.
- 私有表: `CG_BUCKETS` 为 4096, 三张指针键哈希表.
- 标签: `label_base = libcp_unique_name_next(ctx)`, `new_label` 用 `label_base + label_seq++`.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.

### S10

- `context_define` 探针: `/tmp/define_probe.c` 输出 `token=42 macro=FOO=42`.
- NUL 探针: `/tmp/nul_probe.c` 输出 `buffer: tokens=3 failed=0`; `buffer_len: result=null failed=1`.
- stdin 探针: `printf 'int a;\n' | /tmp/stdin_probe` 输出 `first tokens=3`, `after reset tokens=0`.
- 示例: `/tmp/api_example.c` 通过 `cc -std=c11 -fsyntax-only -Wall -Wextra -Ilibcp/include`.
- `! grep -n "reset 后一切失效" docs/09-api-reference.md`: 通过.
- `grep -n "11.7.8"` / `grep -n "tokenize_buffer_len"`: 命中 9.3.7, 9.4.2, 9.9.2.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.

### S11

- `grep -n "10.8.1\|bench-tool\|check-context\|32 KiB\|--defined-only\|docker-test-thirdparty\|cpython" docs/10-build-install.md`: 全部命中.
- `bash -n bench/run.sh`: 通过.
- `make bench-tool`: 退出 0, 产出 `bench/libcp_bench.a` 与 `bench/libcp-bench`.
- `nm -D --defined-only libcp.so | wc -l`: 39.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.

### S12

- UCN 探针: `/tmp/ucn_probe.c` 输出第二个 token `trivia_len=-5`.
- `-D` 探针: `/tmp/define_probe.c` 输出 `macro=FOO=42`.
- NUL 探针: `/tmp/nul_probe.c` 输出 `buffer: tokens=3 failed=0`; `_len` 返回 NULL 且 `failed=1`.
- stdin 探针: reset 前 3 个 token, reset 后 0 个.
- `! grep -n "字符串驻留" docs/11-limitations.md`: 通过.
- `! grep -n "ND_IDENT.var" docs/11-limitations.md`: 通过.
- `grep -n "11.5.8\|11.7.8\|11.11.5"`: 命中第 179, 249, 434 行.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.

### S13

- `git cat-file -e` 对 `53785b9` / `7dc2126` / `b70287e` / `505d0bf` / `c57acbe`: 全部存在.
- `git log -1 --date=short`: 2026-11-01, 2026-11-10, 2026-11-19, 2026-11-21, 2026-11-21.
- `grep -n "arena" docs/12-project-stages.md`: 每处都有"当时"或"已替换"限定.
- `! grep -n "309" docs/12-project-stages.md`: 通过; `grep -n "313"` 命中第 282 行.
- `grep -n "PLAN-TEMPLATE"`: 命中第 485 行.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- `make test-all`: 退出 0; 末尾 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.

### S14

- `make test-all`: 退出 0; 末尾 `c2c s-equiv: 54 files assembly-identical` 与 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- `make format-check`: 退出 0.
- `make check-context`: 退出 0; accessors 0, `cg_ctx` 0, file-scope pointer 0, accessor symbols 0.
- `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
- 快照: 41 份 `.s`, `snapshot diff: empty`.
- `git diff --check`: 通过.
- 范围: `git diff --stat c57acbe..HEAD` 为 17 个文件, 只含 `docs/` 与 `plans/`; `git diff --stat c57acbe..HEAD -- ':!docs' ':!plans'` 为空.
- 计数: `d7=39`, `cst=57`, `nd=69`, `parser=65`, `predefined=47`, `cst_checks=313`, `diagnostics=68` (`diagnostic lock: 68 cases byte-exact`).
- 旧口径: `不做 union`, `66 种`, `309`, `字符串驻留`, `ND_IDENT.var` 在 `docs/` 无命中; 裸 `Obj.` / `File.` 只剩 `CgObj.offset` / `CgObj.stack_size`, 是 codegen 私有类型.
