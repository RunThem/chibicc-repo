# libcp 代码与工程审核报告 (2026-10-24)

- 审核对象: HEAD `0efd93b` (main), 工作树干净 (仅 `.zcodeignore` 未跟踪).
- 范围: 全仓库代码审核 + 工程审核, 六条线并行: (1) LibcpContext 与错误边界, (2) 内存与分配器宏, (3) 公共 API 面 / 改名完整性 / 打包, (4) 解析器双路径与 sema, (5) 构建系统与测试组织, (6) docs/ 文档一致性.
- 方法: 源码精读 + 规则合规 grep 电池 + 元审核 (闸门脚本本身的严谨性); 另在 /tmp 干净副本 (`git archive HEAD`) 做本机构建与闸门验证, 对关键疑点写了最小消费者实测复现. 未修改仓库任何文件.
- 分级: S = 必须立即修 (红线破坏 / 数据丢失); A = 应尽快修 (公共契约破坏 / 重要事实错误); B = 建议修; C = 打磨与知悉项.

## 总体结论

代码库处于很高的工程水准: AGENTS.md 自定的硬性规则 (无新增 exit, 无静态全局状态, ctx 首参纪律, 分配器宏覆盖, 库层无发射整形, 命名前缀) 逐条核实全部达标; 改名 (libchibicc -> libcp) 在全部活文件中零漏改; 文档 617 处 API 引用零错名, 关键数字精确到个位; 测试闸门矩阵完整且被 make 正确接线.

发现 1 个 A 级 (公共 API `libcp_context_define/undef` 缺错误边界帧, 非法宏体实测会 `exit(1)` 杀死嵌入方进程), 7 个 B 级 (3 处文档过时 + 无 CI + install 不带 LICENSE + 测试增量依赖缺口 + .zcodeignore 悬置), 其余为 C 级. 无 S 级.

---

## 一, 发现清单

### [A1] libcp_context_define / libcp_context_undef 无错误边界帧, 非法宏体直接终止宿主进程

- 位置: `libcp/src/lib.c:192-206`; 错误路径 `define_macro` (`libcp/src/preprocess.c:1145`) -> `tokenize_nested` -> `error_tok` -> `raise_error` (`libcp/src/tokenize.c:70-76`).
- 问题: 这两个公共配置函数没有 `ENTER_RET/ENTER_VOID` 帧. 它们对消费者提供的宏体字符串做完整词法分析, 拼写非法 (如未闭合字符串, 非法字符) 时 `raise_error` 发现 `ctx->jmp_top == NULL`, 直接 `exit(1)`.
- 实测 (最小消费者, 链接 HEAD 构建的 libcp.a):
  ```
  libcp_context_new + libcp_context_set_diagnostic(cb)
  libcp_context_define(ctx, "A", "\"oops");
  ```
  输出: 回调收到 `"unclosed string literal"` 诊断之后进程 `exit=1`, define 之后的代码未执行. 即: 诊断回调被调用了, 但进程仍然被杀 - 即使嵌入方注册了回调也无法拦截.
- 违反点: AGENTS.md "不新增 exit() 调用 (驱动器与默认处理器路径除外)" 与公共错误模型 ("每个公共入口是错误边界, 错误经 longjmp 回公共入口, 返回失败值"). `libcp_context_reset` (`libcp/src/lib.c:270-276`) 重放 `macro_ops` 走同一路径 (op 首次已通过词法, 风险较小, 但根因相同).
- 触发面: 嵌入方用 define 注入 `-D` 类配置 (典型: 把用户命令行原样传进来) 时, 一个非法拼写就杀死宿主进程. formatter/linter 类宿主进程正是本库的目标消费者.
- 建议: (a) define/undef 包 `ENTER_VOID` 并在失败时置 `failed` (可考虑改签名返回 bool), 或 (b) 入库前先做一次不触发错误的预校验; (c) 在 `test/library.sh` 补一例锁定: "define 非法宏体 -> 不退出, failed 置位, 回调恰好一次".

### B 级

- **[B1] 无任何 CI 配置**. 根目录无 `.github/`, 全部闸门 (docker-test, snapshot diff, check-library) 依赖手工运行; "每阶段三道闸门全绿"的安全模型目前完全靠纪律. 建议: 最低成本加一个 GitHub Actions (Linux runner 原生 x86-64 直接 `make test-all`, macOS job 只做 `make chibicc` + 前端通道).
- **[B2] `make install` 不随附授权文件**. LICENSE 存在且无问题 (上游作者本人添加的 MIT, commit `5b142b1`, fork 完整继承 512 commit 历史, 授权链清晰), 但 `Makefile:231-245` 只安装头 + 库 + pc, 下游经 install/pkg-config 消费时拿不到许可文本, 对定位为可复用库的项目是分发合规缺口. 建议: install 增加一行安装 LICENSE.
- **[B3] `test/%.exe` 规则缺 `test/common` / `test/test.h` 依赖** (`Makefile:85-87`, pratt/stage2 变体同, `Makefile:104-107, 174-177, 208-211`). 改 test/common 后增量 `make test` 静默跑旧二进制. docker 全新树流程不受影响 (权威闸门安全), 仅本地增量构建命中. 建议: `$(TESTS): test/common test/test.h` 一行补齐.
- **[B4] `.zcodeignore` 悬置**: 未跟踪也不忽略, 且其"同步自 .gitignore"段缺后来新增的 7 条 (`/chibicc-pratt /pratt /stage2-pratt /libcp.a /libcp.so* /libcp.pc` 等). 两种收法: 提交并补同步 (与已提交的 AGENTS.md 同属一个工具生态), 或加入 .gitignore 承认是个人配置; 当前状态最差.
- **[B5] `docs/05-layer2-cst.md:141-154`** 的 `cst_parse` 入口片段未跟上显式 context 线: 仍展示 `cst_parse(Token *tok)` + 局部 `ctx` 命名; 实际 `cst.c:2235-2241` 是 `cst_parse(LibcpContext *ctx, Token *tok)`, 局部名已改 `cst` (提交 `0e1f524`/`86f9d1d`). 按 cst.c 现状更新.
- **[B6] `docs/06-layer3-parse.md:229-234`** 的产生式签名片段同类过时: `stmt(Token **rest, ...)` 等 4 行缺 `LibcpContext *ctx` 首参 (实际 `parse.c:67-73/803/1027/1247/1275`, 提交 `964f13f`). 同步补 ctx.
- **[B7] `docs/10-build-install.md:28`** 称 `libcp.pc` "由 `make` 生成" 不成立: `ALL` 不含它, 只有 `make libcp.pc` / `make check-library` / `make install` (经 .tmp) 才生成 (`Makefile:35-41, 228-229`). 改为准确条件.

### C 级

文档与事实性:
- **[C1] AGENTS.md 代码地图行数过时**: parse.c 写 2799 (实际 2654), sema.c 写 2992 (实际 3161), cst.c 写 2254 (实际 2393); 显式 context 线改签名后未回填. docs/ 没有这个病 (docs/12 的行数是阶段史终点, 标注清晰).
- **[C2] `docs/02-architecture.md:286-292`** cc1 示意块以"库的标准用法"名义展示内部短类型名 (`Node`/`Obj`, 公共名 `LibcpNode`/`LibcpObj`), 消费者照抄会编译失败; 且 `codegen(prog, out)` 与实际 `codegen(ctx, prog, output_buf)` 不一致. 加一句"短名来自驱动器的内部 umbrella"即可.
- **[C3] 文档无版本化 / 基线演进约定**: docs 基线钉死在改名提交, 0.1.0 之后文档如何跟随代码演进没有约定. 一段说明即可.

构建与测试:
- **[C4] `test-all` 不在 .PHONY** (`Makefile:386` 清单缺它).
- **[C5] docker BOOT 排除清单缺 `.codegraph`** (每次 run 拷整个索引目录进容器, 纯浪费 I/O; `.dockerignore` 倒是排了, 两处不同步); `.dockerignore` 也缺 `pratt/` `stage2-pratt/` 等.
- **[C6] 编译标志变化不触发重编** (无 -MMD / stamp): 先 `make` 再 `make ALLOCATOR_CFLAGS=...` 会静默不生效, 需 `make clean` (docs/10 的示例已以 clean 开头, 文档路径已缓解).
- **[C7] 两个环境缺失 exit-0 跳过**: `test/expand.sh` (缺 Linux 系统头时语料全量校验跳过) 与 `test/install.sh` (缺 pkg-config 时 .pc 检查跳过). 都有显式提示且 docker 为权威, 但本地绿闸门不区分"全跑"与"部分跳过" - 已知取舍, 提请知悉.
- **[C8] 闸门覆盖的两个低风险缺口**: `allocator.sh` 从不针对 `pratt/libcp.a` 跑; `context-audit.sh` 不针对 `stage2/libcp.a` 跑. "模式无关"论证成立, 风险低.
- **[C9] thirdparty 脚本硬编码 SSH 地址** (`test/thirdparty/*.sh:2`), HTTPS 重写只在镜像内; 本地无 SSH key 的机器跑 `make test-thirdparty` 会在 clone 失败. docker-first 设计的已知边界.
- **[C10] 杂项**: 中断的 install 残留 `libcp.pc.tmp` 且 clean 不删; 根目录 `libcp.pc` 不随 PREFIX 变化重生成; `install.sh` 硬编码"10 个公共头"清单是 tripwire (新增公共头需同步); `make clean` 的 find 会进 thirdparty/ 删 clone 内 .o (无害); CFLAGS 无 -O2/-Wextra (上游风格, `-Wno-switch` 保留).
- **[C11] codegen.c 在 macOS clang 下有 5 条 -Wformat 警告** (`codegen.c:900,908,910,917,930`, `%lu`/`%ld` 配 `uint64_t`/`int64_t`): Linux 下 uint64_t == unsigned long, 无警告且行为正确, 属宿主噪音; 若在意可用 PRId64/PRIu64 消除.

语义观察:
- **[C12] `libcp_context_set_base_file` 不跨 reset**: `clear_state` 将 `base_file` 清空 (`libcp/src/lib.c:234`), 而 include_paths / macro_ops / diag / predefined_macros 都持久. 名义上是配置 setter 却按 per-TU 状态处理, 语义不对称; 若有意, 建议在 context.h 注明.
- **[C13] `arena_alloc` 的 `start + size > b->cap` 在 size 接近 SIZE_MAX 时理论回绕** (`libcp/src/lib.c:73`); 现实输入被"文件大于 int 拒收"(C12 门) 约束, 仅作加固备注.

---

## 二, 已核实无误项

硬性规则 (AGENTS.md 自定纪律):
- exit 点: 库内唯一 `exit(1)` 在 `raise_error` (`tokenize.c:75`), 属"无公共入口帧时的兜底"允许位置; 其余 exit 全在驱动器 main.c. (A1 是"缺帧导致兜底被公共配置 API 触达", 非新 exit 点.)
- 文件域 static 数据: libcp/src 仅两个只读表 (`parse.c:2259` 绑定力表, `preprocess.c:1261` 预定义宏表), 完全合规; sema.c 零 static 数据; codegen.c 的 static (私有表/标签计数/cast 表) 与 main.c 的选项 static 属消费者侧, 为 AGENTS 认可的形态.
- ctx 首参纪律: hashmap/strings 全部公共辅助首参 ctx (align_to/print_escaped/file_exists 为纯函数豁免); `make check-context` 本机通过 (codegen 文件域 ctx 指针 0, .a 访问器符号 0); libcp/ 内无 `_Thread_local` (命中的全是关键字表字符串); 无 `libcp_active_ctx`/`cg_ctx` 残留.
- 库层无发射整形: `is_lowered`/`brk_label`/`cont_label`/`unique_label` 在 libcp/src 全部 0 处; codegen 词汇只存在于消费者.
- 改名完整性: 活文件 (libcp/ docs/ test/ 根 *.c *.h Makefile README .gitignore) 中旧名命中全部为有意保留 (`__chibicc__` 宏及其文档叙述, 改名记录本身); 零漏改. 二进制名与 `__chibicc__` 按裁决保留.
- 版本三处一致: Makefile `VERSION=0.1.0` == `LIBCP_VERSION "0.1.0"` (公共伞头) == libcp.pc @VERSION@, 且 install.sh 反向校验 pkg-config 报告值.

内存与分配器:
- 库内全部分配经 LIBCP_* 宏 (config_strdup/宏 op 字符串均 LIBCP_MALLOC/CALLOC 且 context_free 逐项归还); open_memstream 缓冲归 libc, `free(buf)` 配对正确且文本被复制进 arena (自定义分配器下仍正确); allocator.h 契约 (单开关, 缺宏 #error, 无逐宏回退, 头自包含) 与文档一致; arena 16 字节对齐 (long double, UBSan 修过), calloc 乘法溢出防护, realloc 的 arena 语义与零尾契约有注释; `config_push` OOM 时不悬垂.
- 生命周期: reset = free_arena + clear_state (12 个 sema 字段在 `sema_state_clear` 单点清理, 含上次审计补的 label_gotos/unique_name_id) + 重放 macro_ops; free 对 new 的全部分配逐项对账; "回调里创建/使用/释放另一个 context"由 `test/library.sh` 的 `check_context_scope` (行 628) 锁定; ASan 轮 (LIBCP_DEBUG_ARENA) 覆盖 arena 内溢出.

公共 API 与打包:
- 10 个公共头逐个单独编译通过, 守卫统一 `LIBCP_*_H`, 系统依赖仅 stdbool/stdint/stdio/stdlib; LibcpContext 公共面仅前向声明; D7 36 函数, `make check-library` 本机通过 (public-api / D7 存在性 / install 布局, pkg-config 缺失时 .pc 项跳过); README.md 已带 fork 状态块与 "libcp as a library" 章节 (26 处提及), 外部读者定位不误导.

解析器与 sema:
- 双路径 `#if` 配对完整: 级联独占 3 段 (`parse.c:1245-2002`) + 5 个小声明块, Pratt 独占 1 段 (`2217-2511`), 共享代码都在条件块外; 绑定力表 31 行覆盖全部中缀 (11 个复合赋值全部右结合, 逗号左结合, `?:` 独立分支含 elvis), 下标内层用完整 `expr` 与 C 文法一致; 深度保护 (MAX_PARSE_DEPTH 512) 两路径都在 (Pratt 侧 `2392/2490` 单点计数); 两模式等价性有 test-ab (41 语料 AST+汇编逐字节) 与 tinycc 闸门背书.
- `LIBCP_TEST_BUILTINS` 已全量改名并被 library.sh 的 ASan 轮使用; ast.sh 在默认与 pratt 两个构建各跑一遍 (Makefile:95/115).

文档 (docs/ 全 13 文件穷举):
- 改名零非法残留 (5 处命中全为合法叙述); 617 处 API 符号引用零错名, 16+ 签名抽验一致; 99 个代码块中两个完整示例 `cc -fsyntax-only` 通过; 事实数字全部对上 (36 公共函数 / 10 头 / 55 CST 节点 / 47 预定义宏 / 11 provenance 访问器 / 68 诊断 / 85 AST / 34 形状 / 61+29-3-17 dump 样本 / 41+48 语料两口径 / 0.1.0); 安装布局与 Makefile 逐行一致; 交叉引用 (含 AGENTS 所指 10.11.5 mimalloc 配方) 存在; 表述红线 (不类比 Clang, CST 建在预处理前, 不承诺 ABI) 七处口径一致.

测试与构建组织:
- 15 个 test/*.sh 无孤儿 (全部被 make 目标接线, 覆盖矩阵见审核过程); pratt 四套产物目录互斥无混链 (`make -n` 证实); 头依赖"改任一头全量重编"保守正确; 并行 -j 安全 (mkdir -p, 无共享写路径, mktemp+trap 幂等); 脚本健壮性 (新脚本 set -eu, 老脚本显式失败累积, 全部 trap 清理); 快照基线机制健全 (时间宏固定值, 逐字节 diff, 无基线显式报错); .gitignore 覆盖完整.

---

## 三, 验证记录 (本机, macOS ARM64 宿主)

- `git archive HEAD` 干净副本构建: `make -j8` exit 0 (产物 chibicc + libcp.a; codegen.c 5 条 -Wformat 警告, 见 C11).
- `make check-context`: 通过 (codegen 文件域 ctx 指针 0, .a 访问器符号 0).
- `make check-library`: 通过 (library / allocator / public-api / install; pkg-config 缺失, .pc 检查按设计跳过).
- 只读通道冒烟: `-E` / `-verify-cst` / `-verify-expansions` / `-dump-ast` 全部正常.
- A1 实测: 最小消费者复现 exit(1) (见 A1).
- 未运行 (需 Docker 或小时级时间): `make docker-test` 全量, `test-stage2` 系列, thirdparty; 各线 RESULT 归档记录显示最近一次全绿.

## 四, 建议处理顺序

1. A1: define/undef 加错误边界 + library.sh 补锁定用例 (公共契约, 改动小).
2. B5/B6/B7: 文档三处过时片段 (一次性小改).
3. B3: 测试 .exe 规则补依赖 (一行).
4. B2: install 带 LICENSE (一行).
5. B4: .zcodeignore 二选一收掉.
6. B1: CI (独立小项, 随时).
7. C 项按顺手; C1 (AGENTS 行数) 可在下一次 AGENTS 更新时顺带.

---

## 五, 修复记录 (2026-10-25)

按上文处理顺序全部处理, 闸门见文末. 逐项:

- **[A1] 已修**: `libcp_context_define` 现在有自己的错误边界帧(手写 JmpFrame 而非 ENTER_VOID, 以便失败路径释放 op 串), 且改为**先 apply 后 record** - `apply_macro_op` 抛错时 op 串的 `=` 处仍是 NUL, 半应用的 op 若进入 `macro_ops`, reset 重放会把该名静默定义成 `1`; 先 apply 保证只有通过的 op 被记录. `libcp_context_undef` 包 ENTER_VOID(undef_macro 今天不可能抛错, 属对称防御). 公共签名不变(void), 失败经 `libcp_context_failed` 可查. 配套: main.c 在 `parse_args` 后检查 failed 并 exit(1), 驱动器对坏 `-D` 的可观察行为(诊断字节 + exit 1)不变; `test/library.sh` 新增 `check_define_bad_body`(define 非法宏体 -> 不退出, failed 置位, 回调恰好一次, 名字未被定义 - 同时锁住 record 时序, 之后 reset 重放与后续 define 正常). 顺带做了 C13 的 arena 溢出加固(`size > b->cap - start` 与 `size > SIZE_MAX - align` 守卫).
- **[B1] 已修**: 新增 `.github/workflows/ci.yml` - Linux job 原生 x86-64 跑 `make test-all` + `make check-library`; macOS job 跑 `make chibicc` + trivia / cst / expand / ast 四个前端通道. Pratt 双路径与 thirdparty 仍走手工 Docker 闸门(CI 时长考虑).
- **[B2] 已修**: `make install` 增加 `$(PREFIX)/share/licenses/libcp/LICENSE`; `test/install.sh` 断言其存在, docs/10 的安装布局与目标一览同步.
- **[B3] 已修**: 四个 `%.exe` 模式规则(default / pratt / stage2 / stage2-pratt)补 `test/common test/test.h` 前置依赖.
- **[B4] 已修(取第二案)**: `.zcodeignore` 加入 `.gitignore`, 承认为个人工具配置; 其"同步自 .gitignore"段补齐缺失的 7 条. 若日后想共享给所有 ZCode 用户, 删掉 .gitignore 里那一行再提交即可.
- **[B5] 已修**: docs/05 的 `cst_parse` 片段按 cst.c 现状(`LibcpContext *ctx` 首参, 局部名 `cst`)更新.
- **[B6] 已修**: docs/06 的产生式签名片段补 `LibcpContext *ctx` 首参, 正文加一句说明.
- **[B7] 已修**: docs/10 的 `libcp.pc` 行改为准确条件(仅 `make libcp.pc` / `check-library` / `install` 生成, `all` 不含).
- **[C1] 已修**: AGENTS.md 代码地图行数更新(parse.c 2654, sema.c 3161, cst.c 2393); 路线图节的"终态"行数是各线完成时的历史事实, 不动.
- **[C2] 已修**: docs/02 的 cc1 片段改用公共名(`LibcpNode` / `LibcpObj`)与真实 codegen 签名 `codegen(ctx, prog, out)`, 并注明 codegen 是驱动器函数不属于库.
- **[C3] 已修**: docs/README 的"版本与状态"新增演进约定一条(文档与代码同一提交更新, 版本提升做全量校对并刷新基线行).
- **[C4] 已修**: `test-all` 补进 .PHONY.
- **[C5] 已修**: Makefile 的 BOOT 排除清单补 `./.codegraph`; `.dockerignore` 补 pratt / stage2-pratt / chibicc-pratt / libcp.a / libcp.so* / libcp.pc.
- **[C11] 已修**: codegen.c 五处改用 `(unsigned long)` / `(long)` 显式 cast(Linux x86-64 上 uint64_t/int64_t 就是 long/unsigned long, 输出字节不变, 不引入 inttypes.h 依赖 - 自举不需要新头).
- **[C12] 已修(按现状注明)**: `libcp_context_set_base_file` 实际只在内部伞头 `libcp/src/libcp.h` 声明(不在公共 context.h, 不是 D7 面), 注释加在内部声明处: base_file 是 per-TU 状态, reset 清空, 与 include_paths / macro_ops 不同.
- **[C13] 已修**: 见 A1 条目末尾.
- **C10 部分已修**: clean 增加 `libcp.pc.tmp`. 其余维持: 根目录 `libcp.pc` 不随 PREFIX 重生成(生成文件的常规语义), install.sh 的头清单 tripwire 是有意的, clean 的 find 进 thirdparty 无害, 不加 -O2/-Wextra 是上游风格.
- **未动(知悉项)**: C6(编译标志变化不触发重编, 文档已缓解, 加 -MMD 属侵入性改造), C7(本地跳过与全跑不区分), C8(allocator.sh 不针对 pratt, context-audit 不针对 stage2, 模式无关论证成立), C9(thirdparty SSH 地址, docker-first 设计边界).

验证(2026-10-25, macOS ARM64 宿主): 本机 `make` 零告警(C11 消除 5 条 -Wformat), `make check-context` 与 check-library 四脚本(library / allocator / public-api / install)通过(本机无 pkg-config, .pc 项按设计跳过, LICENSE 断言已跑), trivia / cst / expand / ast 通道通过, 坏 `-D` 的诊断字节与 exit 1 逐字节保持; Docker 内 `docker-test`(test-all 含自举)与 `docker-test-both` 全绿, `docker-snapshot-diff` 为空 diff.

插曲(如实记录): C13 的第一版(`size > b->cap - start`)在修复过程中**引入过堆越界** - 当 arena 块被填满且 cap 不是 align 的倍数时, `start = round_up(used, align)` 会超过 cap, 无符号减法 `cap - start` 回绕成巨大值使条件恒假, 不再开新块, 写到块尾之外. `libcp/src/cst.c`(约 70KB)触发自定义 cap 块, 被 `docker-test` 的 stage2 自举当场抓获(chibicc 静默 exit 1); 已改为先判 `start > b->cap` 再做减法, 与原式在全部可达状态上等价. 教训: library.sh 的 ASan 轮用 `LIBCP_DEBUG_ARENA`(每对象独立分配), 对 arena_alloc 自身的越界是盲区 - 它看的是 arena *之内*的溢出, 不是 arena *分配器*的越界; 这类 bug 只有真实 arena 路径的大输入(docker 自举 / 大文件)能暴露.
