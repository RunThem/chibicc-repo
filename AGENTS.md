# AGENTS.md - chibicc(C 前端库 fork)

本仓库是 Rui Ueyama 的 chibicc(小型 C11 编译器, 约 9k 行)的 fork. 库化已完成:

**C 前端(tokenize / preprocess / parse / sema)是一个可复用的库**. chibicc 本体退化为"驱动器 + x86-64 codegen", 作为这个库的参考消费者. 上游 README(含 "Design principles" 一节)描述的是上游的教学定位; 与本文件冲突时, 以本文件为准. 库文档在 `docs/`.

库名 `libcp`(展开为 "Library C Parse", 是品牌 token 而不是描述), 二进制仍叫 `chibicc`, 预定义宏 `__chibicc__` 不变; 公共头根 `libcp/include/libcp/`, 公共符号 `libcp_` / `Libcp` 前缀; 库实现全部在 `libcp/src/`(含 facade `lib.c` 与内部 umbrella `libcp.h`), 根目录只保留驱动器 `main.c`, `codegen.c` 与驱动器内部头 `driver.h`. 只承诺源码兼容, 不承诺 ABI.

## 全局约定

语言: 本文件, 文档, 代码注释与回显给用户的回复统一使用中文, 代码标识符与专有名词保留英文原文; 提交信息例外, 见下.
标点: 所有输出(写入文件的注释 / 文档, 以及回显给用户的回复), 无论中文还是英文, 一律使用英文标点(, . : ; ( ) - _ /), 不使用中文标点(如 , 。 ： ； （ ） 、).
提交信息: 使用英文祈使句(如 `Fix ...`, `Add ...`), 首字母大写, 不加句号; 这是中文输出规则的唯一例外, 与仓库历史保持一致.
计划与记录: 现行线的计划 / 记录固定在 `plans/PLAN.md` / `plans/RESULT.md`; 已完成的线归档为 `plans/PLAN-<主题>.md` / `plans/RESULT-<主题>.md`, 文件头加归档说明.
单一事实来源: `docs/` 拥有描述性事实(架构, 目录, 目标, 测试矩阵, 环境细节); 本文件拥有规范性约束与执行流程. 文档与代码冲突时, API 与行为以公共头与测试为准; 流程以本文件为准; 重复内容改为链接.

## 计划, 执行与提交

- 用户要求计划书时, 先把完整计划写入 `plans/PLAN.md` 并展示全文; 展示后必须停下等待明确批准, 不得在展示后直接开始执行.
- 未获批准前只做只读调查与计划写作; 不改代码, 不提交.
- 批准后严格按 PLAN 的步骤推进, 一步一提交: 每步完成改动与该步闸门后立即单独提交; 禁止把多个步骤合成一个大提交, 禁止整条线一次提交.
- 每步提交前必须确认该步的测试与验收标准通过; 不通过就留在本步修复, 不进入下一步.
- 所有修复与特性直接提交到 `main`, 不新开分支, 不走 PR 流程, 除非用户明确要求.
- 执行中若范围, 设计或验收标准变化, 先更新 `PLAN.md` 说明偏差与原因, 再次请求批准; 不得默默改范围.
- `plans/RESULT.md` 随每步更新, 记录实际改动, 测试结果与偏差; 收线时补齐验收结论与对后续计划的影响.

## 计划与记录格式

- 新建计划: 复制 `plans/PLAN-TEMPLATE.md` 到 `plans/PLAN.md`; 执行记录: 复制 `plans/RESULT-TEMPLATE.md` 到 `plans/RESULT.md`. 模板中的章节是必含项, 不得删减.
- 两份文件都使用中文与英文标点, 步骤用 `- [ ]` / `- [x]`; 计划书不得把 "优化", "整理", "完善" 这类无法验收的词单独作为一步.
- 若某步偏差导致验收未通过, 必须写明是否阻塞后续步骤; 影响后续计划的, 在 `PLAN.md` 中同步修订并再次请求批准.

## 目标架构与库边界

流水线按中间表示分 5 层: token 流(保留 trivia), lossless CST, 忠实语法 AST, sema 标注树, x86-64 汇编. 前 4 层属于库; 第 5 层是 `codegen.c`, 不属于库. 设计原则见 `docs/01-overview.md` 1.5, 层间契约见 `docs/02-architecture.md` 2.2, 目录地图见 `docs/02-architecture.md` 2.10.

## 面向库的硬性规则

- 不新增 `exit()`: 库层错误走诊断回调 + longjmp 到公共入口; 默认处理器逐字节复现旧输出; 新代码不得引入新的 exit 点(驱动器与默认处理器路径除外).
- 不新增静态全局状态: 语义状态全部在 `LibcpContext`(parse.c 只剩只读关键字表缓存); 新代码把状态放进 context.
- 库层无隐式上下文: 新增或修改的内部函数若直接或传递地触碰 per-TU 分配, 格式化, 容器, 诊断或 context 字段, 必须以 `LibcpContext *ctx` 作首参. 不得新增 `libcp_ctx()` 或 TLS; `make check-context` 是必跑门; `codegen.c` 不得有 `cg_ctx`.
- API 纪律: 公共头(`libcp/include/libcp/`)与内部头(`libcp/src/libcp.h`)分离, 不泄漏编译器内部假设(Linux 路径, 单次进程生命周期, 直接 exit 等).
- 后端整形只允许出现在 codegen.c: 指针缩放, 读写回环, 临时槽, 语句重排, 标签与唯一名分配都不是库层的事; 库层只做标注与语义结论.
- 检查与诊断留在库层: 左值性, 实参个数, 位域取地址, VLA 不得初始化, 不完整类型等检查不得随后端搬走; 文案与锚点以 `test/diagnostic.sh` 为准.

## 已定边界(新决策仍由用户拍板, 不擅自推翻)

- 命名: `libcp` / `<libcp/...>` / `libcp_` / `Libcp` / `LIBCP_`; 库内私有类型同样带 `Libcp` / `struct libcp_*`; 二进制 `chibicc`, 预定义宏 `__chibicc__` 与 `plans/**` 的旧名不回溯改写.
- 公共 IR: `LibcpNode` / `LibcpObj` 只保留第 3/4 层字段; codegen 状态在 `codegen.c` 私有表; sema 只标注, 不整形.
- context: 一个 context 一个翻译单元; `new` / `free` / `reset` 三态; reset 保留配置并逐个归还全部 per-TU 分配; 不提供逐对象 free 的公共 API, 不做内部池化, 不做 per-phase arena.
- 宏保真: 消费展开后的 token 流, 不引入宏调用节点; 宏保真由 CST 与 provenance 承担; 放弃宏展开内部的外科式编辑.
- CST 只建在预处理之前: 不做展开流 CST, 也不从 CST 反向还原宏展开前的 token 流.
- 公共面不提供 per-node user data 扩展点, 也不提供 trivia 分段迭代器: 两个消费者(formatter 与 c2c)都只需要只读遍历.
- c2c 只是 `make test` / `make test-stage2` 的回归闸门: 不进入 `all`, 不安装, 不做产品化(改写 / 插桩 / 重构 / 源码回写都不在范围).
- 分配器: `<libcp/allocator.h>` 的四个 `LIBCP_*` 宏; 单开关 `LIBCP_DEFAULT_ALLOCATOR`, 未定义时嵌入方必须提供全部四个, 不做逐宏回退; 编译期配置, 只有重编库才生效.
- 公共面只暴露 D7; `.a` 的内部符号统一 `libcp_` 前缀, 但不是 API, 消费者不得调用.

## 测试路径与构建

测试前先运行 `uname -s` 与 `uname -m`, 在汇报中写明宿主与选择的路径; 不得跳过判定直接测试.

- `Linux` + `x86_64`: 原生路径. 全量测试用 `make test-all`; 第三方用 `make test-thirdparty`; 汇编等价性用同一 commit 的临时 worktree 生成 `test/*.c` 基线快照并逐字节 diff; `make bench` 只在此路径运行. 禁止在此宿主运行 `make docker-image`, `make docker-test`, `make docker-test-thirdparty`, `make docker-snapshot-diff`.
- 其它宿主: Docker 路径. 首次 `make docker-image`; 全量测试用 `make docker-test`; 第三方用 `make docker-test-thirdparty`; 汇编等价性用 `make docker-snapshot-diff`. 非 x86-64 宿主可用 `-E` / `-S` / `-dump-*` 只验前端, 见 `docs/10-build-install.md` 10.8.
- Rosetta 下 tcc 的 106_pthread / 112_backtrace / 113_btdll 会不稳定 SIGSEGV, 与 chibicc 无关; `test/thirdparty/make` 是 PATH shim, 真 x86-64 Linux 不需要.
- 容器内构建与测试必须在可写层 `/work`: tcc 的 `-run`/bcheck 只认 overlayfs, virtiofs 或 docker volume 上确定性 SIGSEGV; 挂载目录只放源码与 clone 缓存.
- 构建: `make` 产 `chibicc` 与 Linux 下的 `libcp.so`; `make libcp.a` 只产静态库; `make install PREFIX=... [DESTDIR=...]` 装公共头, 库与 `libcp.pc`; `make check-library` 跑公共面与打包闸门; `make check-context` 跑隐式上下文审计.
- 格式: 改完代码必须跑 `make format`(clang-format 20), 不手工调格式; 只读闸门 `make format-check`.
- 每次改动过三道闸门: 全量测试含自举(原生 `make test-all` 或 Docker `make docker-test`), 汇编等价性 diff, 该改动新增的针对性测试. 行为变化单独提交.
- `make test` 与 `make test-stage2` 的目标, 完整测试矩阵与 Docker 细节见 `docs/10-build-install.md` 10.8, `docs/10-build-install.md` 10.10 与 `docs/10-build-install.md` 10.12.
- `make bench` 构建并运行前端分阶段 benchmark(`bench/run.sh`, 仅 Linux 原生), 采集 reset / tokenize / preprocess / parse / sema 的时间与分配器统计; `make bench-tool` 只构建, 不进入 `all` / `test` / `install`.

## 设计原则

可读性优先于巧妙, 不做顺手的"改进"; per-TU 内存直接走 `LIBCP_*`, 库不叠加 arena; codegen 没有优化 pass; `include/` 是自举头, 通过 `-Iinclude` 使用. 完整说明见 `docs/01-overview.md` 1.5.

## 测试约定

- 每个特性对应一个 `test/<feature>.c`, 用 `test/test.h` 的 `ASSERT(expected, expr)` 并链接 `test/common`; 新用例加进对应的特性文件, 仅在必要时扩展 `test/test.h` / `test/common`.
- 行为锁都在 `test/*.sh`, 由 `make test` 与 `make test-stage2` 各跑一遍; 改 codegen 的发射形必须同步更新 `test/shape.sh`, `subst` 新增分支必须同步更新 `test/expand.sh`, UNPARSED golden 用 `test/cst-stats.sh update` 更新.
- 库内自测在 `test/self/`, 使用 `.a` 内部符号, 不属于公共 API.
- `test/thirdparty/*.sh` 很慢且仅限 Linux, 不属于 `make test`.
