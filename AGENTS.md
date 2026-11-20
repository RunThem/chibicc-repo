# AGENTS.md - chibicc(C 前端库 fork)

本仓库是 Rui Ueyama 的 chibicc(小型 C11 编译器, 约 9k 行)的 fork. 库化已完成:

**C 前端(tokenize / preprocess / parse / sema)是一个可复用的库**. chibicc 本体退化为"驱动器 + x86-64 codegen", 作为这个库的参考消费者. 上游 README(含 "Design principles" 一节)描述的是上游的教学定位; 与本文件冲突时, 以本文件为准. 库文档在 `docs/`.

库名 `libcp`(展开为 "Library C Parse", 是品牌 token 而不是描述), 二进制仍叫 `chibicc`, 预定义宏 `__chibicc__` 不变; 公共头根 `libcp/include/libcp/`, 公共符号 `libcp_` / `Libcp` 前缀; 库实现全部在 `libcp/src/`(含 facade `lib.c` 与内部 umbrella `libcp.h`), 根目录只保留驱动器 `main.c` 与 `codegen.c`. 只承诺源码兼容, 不承诺 ABI.

## 全局约定

语言: 本文件, 文档, 代码注释与回显给用户的回复统一使用中文, 代码标识符与专有名词保留英文原文; 提交信息例外, 见下.
标点: 所有输出(写入文件的注释 / 文档, 以及回显给用户的回复), 无论中文还是英文, 一律使用英文标点(, . : ; ( ) - _ /), 不使用中文标点(如 , 。 ： ； （ ） 、).
提交信息: 使用英文祈使句(如 `Fix ...`, `Add ...`), 首字母大写, 不加句号; 这是中文输出规则的唯一例外, 与仓库历史保持一致.
计划与记录: 现行线的计划 / 记录固定在 `plans/PLAN.md` / `plans/RESULT.md`; 已完成的线归档为 `plans/PLAN-<主题>.md` / `plans/RESULT-<主题>.md`, 文件头加归档说明.

## 计划, 执行与提交

- 用户要求计划书时, 先把完整计划写入 `plans/PLAN.md` 并展示全文; 展示后必须停下等待明确批准, 不得在展示后直接开始执行.
- 未获批准前只做只读调查与计划写作; 不改代码, 不提交.
- 批准后严格按 PLAN 的步骤推进, 一步一提交: 每步完成改动与该步闸门后立即单独提交; 禁止把多个步骤合成一个大提交, 禁止整条线一次提交.
- 每步提交前必须确认该步的测试与验收标准通过; 不通过就留在本步修复, 不进入下一步.
- 所有修复与特性直接提交到 `main`, 不新开分支, 不走 PR 流程, 除非用户明确要求.
- 执行中若范围, 设计或验收标准变化, 先更新 `PLAN.md` 说明偏差与原因, 再次请求批准; 不得默默改范围.
- `plans/RESULT.md` 随每步更新, 记录实际改动, 测试结果与偏差; 收线时补齐验收结论与对后续计划的影响.

## 计划与记录格式

格式从现有 `plans/PLAN-*.md` / `plans/RESULT-*.md` 总结. 两份文件都使用中文与英文标点, 步骤用 `- [ ]` / `- [x]`.

### PLAN.md 必含

1. 标题与状态: `# PLAN.md - <主题>执行计划`; 归档后改为 `PLAN-<主题>.md`, 文件头加归档说明.
2. 目标与边界: 一句话目标, 允许改动的范围, 非目标.
3. 用户拍板: 日期与已确认决定; 未决项单独列出.
4. 现状锚点: 开工 commit, 宿主, 实测命令与数字; 实测与估计分开.
5. 设计决定: 每条写选择, 理由, 影响.
6. 步骤: 每步必须写全下面六项; 步骤必须能独立提交并通过该步闸门, 否则说明拆分不够细, 重写计划.
7. 闸门: 全线闸门与每步闸门.
8. 风险与回退: 失败模式, 缓解, 回退点.
9. 执行流程: 等待批准, 一步一提交, 更新 RESULT.

每步模板:

```markdown
#### S1 - <名称>
- 改什么: <文件与行为>
- 为什么: <问题与约束>
- 目的与效果: <验收时能观察到的结果>
- 如何测试: <命令, 用例, 快照或 bench>
- 验收标准: <可判定条件>
- 提交边界: 本步一个提交
```

计划书不得把 "优化", "整理", "完善" 这类无法验收的词单独作为一步.

### RESULT.md 必含

1. 标题, 开工 HEAD, 宿主, 日期.
2. 用户决定.
3. 提交一览: hash + subject, 与 PLAN 步骤一一对应.
4. 各步记录: 做了什么, 实际改动, 测试命令与结果, 与计划的偏差.
5. 验收: 逐条对照 PLAN 的验收标准, 写通过/不通过及证据.
6. 偏差与原因: 计划 vs 实际; 原因; 对验收的影响.
7. 后续影响: 遗留项, 风险, 需要用户拍板的事项.
8. 闸门实测: 完整命令与关键输出/数字.

每步记录模板:

```markdown
### S1 - <名称> (`<hash>`)
- 做了什么: ...
- 测试: ...
- 偏差: ...
- 验收: ...
```

若某步偏差导致验收未通过, 必须写明是否阻塞后续步骤; 影响后续计划的, 在 `PLAN.md` 中同步修订并再次请求批准.

## 目标架构与库边界

流水线按"中间表示"划成 5 层; 每层是一个干净的阶段边界, 也是库对外暴露的一种能力:

1. **Token 流(trivia 保留)** - 空白/注释/换行作为 trivia 挂在 token 上; 拼写/位置/编码完整; 预处理前的 token 流可逐字节重建输入文件.
2. **CST(lossless)** - 建在预处理**之前**的原始文本上: 预处理指令与条件分支都是节点, trivia 挂靠, 逐字节还原源码. 前置位置是边界: 预处理不可逆, 展开后的语法形态归第 3 层, 源码映射归 provenance.
3. **语法层 AST(忠实, 无类型)** - 从展开流下降, 保留源码写法(如 `a[i]` 不降成 `*(a+i)`, 隐式 cast 有标记); 不降级, 不绑名, 不算类型; 宏调用不建节点, 宏保真归 CST 与 provenance.
4. **sema 产物(纯标注)** - 名字解析, 类型检查与标注, 常量求值, 布局与语义结论; 语言规定存在的对象在此物化; 不做任何为发射服务的整形.
5. **汇编(codegen)** - x86-64 System V / GAS / ELF, 不属于库; 消费第 4 层标注树, 自己决定整形与发射.

## 面向库的硬性规则

- 不新增 `exit()`: 库层错误走诊断回调 + longjmp 到公共入口; 默认处理器逐字节复现旧输出; 新代码不得引入新的 exit 点(驱动器与默认处理器路径除外).
- 不新增静态全局状态: 语义状态全部在 `LibcpContext`(parse.c 只剩只读关键字表缓存); 新代码把状态放进 context.
- 库层无隐式上下文: 新增或修改的内部函数若直接或传递地触碰 per-TU 分配, 格式化, 容器, 诊断或 context 字段, 必须以 `LibcpContext *ctx` 作首参. 不得新增 `libcp_ctx()` 或 TLS; `make check-context` 是必跑门; `codegen.c` 不得有 `cg_ctx`.
- API 纪律: 公共头(`libcp/include/libcp/`)与内部头(`libcp/src/libcp.h`)分离, 不泄漏编译器内部假设(Linux 路径, 单次进程生命周期, 直接 exit 等).
- 后端整形只允许出现在 codegen.c: 指针缩放, 读写回环, 临时槽, 语句重排, 标签与唯一名分配都不是库层的事; 库层只做标注与语义结论.
- 检查与诊断留在库层: 左值性, 实参个数, 位域取地址, VLA 不得初始化, 不完整类型等检查不得随后端搬走; 文案与锚点以 `test/diagnostic.sh` 为准.

## 已定边界(新决策仍由用户拍板, 不擅自推翻)

- 命名: `libcp` / `<libcp/...>` / `libcp_` / `Libcp` / `LIBCP_`; 库内私有类型同样带 `Libcp` / `struct libcp_*`; 二进制 `chibicc`, 预定义宏 `__chibicc__` 与 `plans/**` 的旧名不回溯改写; 只承诺源码兼容.
- 公共 IR: `LibcpNode` / `LibcpObj` 只保留第 3/4 层字段; codegen 状态在 `codegen.c` 私有表; sema 只标注, 不整形.
- context: 一个 context 一个翻译单元; `new` / `free` / `reset` 三态; reset 保留配置并逐个归还全部 per-TU 分配; 不提供逐对象 free 的公共 API, 不做内部池化, 不做 per-phase arena.
- 宏保真: 消费展开后的 token 流, 不引入宏调用节点; 宏保真由 CST 与 provenance 承担; 放弃宏展开内部的外科式编辑.
- CST 只建在预处理之前: 不做展开流 CST, 也不从 CST 反向还原宏展开前的 token 流.
- 公共面不提供 per-node user data 扩展点, 也不提供 trivia 分段迭代器: 两个消费者(formatter 与 c2c)都只需要只读遍历.
- c2c 只是 `make test` / `make test-stage2` 的回归闸门: 不进入 `all`, 不安装, 不做产品化(改写 / 插桩 / 重构 / 源码回写都不在范围).
- 分配器: `<libcp/allocator.h>` 的四个 `LIBCP_*` 宏; 单开关 `LIBCP_DEFAULT_ALLOCATOR`, 未定义时嵌入方必须提供全部四个, 不做逐宏回退; 编译期配置, 只有重编库才生效.
- 公共面: 10 个公共头只声明 D7; `.a` 的内部符号统一 `libcp_` 前缀但不是 API(消费者不得调用, 不承诺源码兼容); `.so` 只导出 D7.

## 测试路径判定(每次测试前)

测试前必须先运行 `uname -s` 与 `uname -m`, 并在汇报中写明宿主与选择的路径; 不得跳过判定直接测试.

- `Linux` + `x86_64`: 原生路径. 全量测试用 `make test-all`; 第三方用 `make test-thirdparty`; 汇编等价性用同一 commit 的临时 worktree 生成 `test/*.c` 基线快照并逐字节 diff. 禁止在 x86-64 Linux 上运行 `make docker-image`, `make docker-test`, `make docker-test-thirdparty`, `make docker-snapshot-diff`.
- 其它宿主: Docker 路径. 全量测试用 `make docker-test`; 第三方用 `make docker-test-thirdparty`; 汇编等价性用 `make docker-snapshot-diff`. 首次需要 `make docker-image`.
- `make bench` 只在 Linux 原生运行; 其它宿主不做计时.

## 构建与测试

- 修改完代码后必须执行 `make format`(clang-format 20, 见下), 由它统一格式化, 不手工调格式.
- `make` - 构建 `chibicc` 二进制(纯 C11, 无外部依赖; CFLAGS 定义在 Makefile 中).
- `make check-context` - 隐式上下文审计(`test/context-audit.sh --strict`): 库层无访问器 / 无 TLS / `.a` 无访问器符号, `codegen.c` 无 `cg_ctx`; 由 `make test` 一并调用.
- `make fmt-tool` - 构建 CST formatter(`tools/fmt.c`, 产物 `fmt`), 由 `make test` 调用后接 `test/fmt.sh`.
- `make format` - 用仓库根的 `.clang-format` 格式化 `libcp/`(源码与公共头)以及 `main.c` / `codegen.c`, 不含测试与 `tools/`; 无 clang-format 时打印提示并跳过, 但版本低于 20 时报错退出. 目标名是 `format` 而不是 `fmt`, 因为 `fmt` 是 formatter 二进制.
- `make format-check` - 只读格式闸门(`clang-format --dry-run --Werror`), 不改文件; CI 由 `.github/workflows/format.yml` 安装固定版本的 clang-format 20 后运行它.
- `make test` - 用 chibicc 自身编译所有 `test/*.c`, 用 `cc -pthread` 链接 `test/common` 后逐个运行, 再依次跑 `test/driver.sh`(命令行选项), `test/diagnostic.sh`(诊断逐字节), `test/warning.sh`(警告逐字节), `test/shape.sh`(发射形状), `test/ast.sh`(第 3 层 AST 形态), `test/trivia.sh`(trivia 通道), `test/cst.sh`(CST 通道), `test/expand.sh`(provenance), `test/library.sh`(公共面与 ASan), `test/allocator.sh`(分配器替换), `test/self-test.sh`(库内自测), `test/cst-stats.sh`(UNPARSED golden), 最后 `make fmt-tool` + `test/fmt.sh`(formatter)与 `make c2c` + `test/c2c.sh`(第 3 层回归闸门, 不随 `make` / `make install` 发布).
- `make test-stage2` - 自举检查: 用 chibicc 编译它自己, 再重跑全部测试.
- `make test-all` - 以上两项合计. `make clean` 用于清理.
- `make bench` - 构建并运行前端分阶段 benchmark(`bench/run.sh`, 仅 Linux, 其它宿主由脚本拒绝); 采集 reset / tokenize / preprocess / parse / sema 的时间与分配器统计, 输出环境块, 语料指纹与固定表格. `make bench-tool` 只构建(`-O2` + `bench/alloc_shim.h`, 不进入 `all` / `test` / `install`).
- 单独运行某个特性测试: `make test/sizeof.exe && ./test/sizeof.exe`(模式规则会自动处理).
- 只验证前端(非 x86-64 Linux 宿主不能在本机汇编/运行): `-E`(预处理), `-S`(汇编文本), `-dump-tokens` / `-reconstruct`(第 1 层), `-dump-cst` / `-reconstruct-cst` / `-verify-cst`(第 2 层), `-dump-expansions` / `-verify-expansions`(provenance), `-dump-ast`(第 3 层, parse 后 sema 前早退).
- 库构建: `make libcp.a`; Linux 下 `make` 也产 `libcp.so`(PIC, 只导出 D7, SONAME `libcp.so.0`; macOS 只承诺 `.a`), `chibicc` 链接静态库; `make install PREFIX=... [DESTDIR=...]` 装 10 个公共头, 库与 `libcp.pc`, `pkg-config --cflags --libs libcp` 可用; `LIBCP_VERSION` / `libcp_version()` 为 `0.2.0`. `make test-stage2` 用 chibicc 重编库对象(`stage2/libcp.a`). `make check-library` 跑 library / allocator / public-api / install 四个脚本(头文件自包含, 消费者只依赖 D7, `.a` 的 D7 齐全, `.so` 只导出 D7, install / pkg-config 布局); `test/library.sh` 最后一轮用 ASan+UBSan 重编库对象, 查 UB 与 reset 后活字节归零.
- 每次改动必须过三道闸门: 全量测试含自举(按"测试路径判定"选原生 `make test-all` 或 Docker `make docker-test`), 汇编等价性 diff(x86-64 Linux 用本机基线 worktree 快照, 其它宿主用 `make docker-snapshot-diff`; Docker 基线固定在 amd64 Ubuntu 容器内, 逐字节比较), 该改动新增的针对性测试. 行为变化必须单独提交.

## 平台陷阱与测试环境(先按宿主选路径)

chibicc 生成的是 x86-64 System V / GAS / ELF 汇编, 且 `main.c` 硬编码了 Linux 路径(`/usr/include/x86_64-linux-gnu` 等)与 gcc/`ld` 调用. 先确认宿主, 再选测试路径:

- **x86-64 Linux 宿主**(`uname -s` 为 `Linux`, `uname -m` 为 `x86_64`; 含 devcontainer 与 CI): 生成物可直接由本机 `as` / `ld` 消费, 直接跑 `make test-all`, 不要再用 Docker - 容器只增加启动开销, 测试结果与本机一致. 第三方脚本用 `make test-thirdparty`(仅 x86-64 Linux 可用, 不要加 `test/thirdparty` 的 make shim).
- **其它宿主**(macOS ARM64 / Intel macOS / Linux arm64 等): 编译器本体仍可构建, 但生成的代码无法在本机汇编或运行, 直接跑 `make test` 会在汇编阶段失败; 完整测试走下面的 Docker 路径.

非 x86-64 Linux 宿主通过 Docker 运行完整测试(macOS 上用 OrbStack, amd64 容器走 Rosetta):

- `make docker-image` - 构建依赖镜像 `chibicc:amd64`(ubuntu:22.04, amd64, 一次性约 90 s).
- `make docker-test` - 自研测试集 + 自举, 分钟级; 仅非 x86-64 Linux 宿主需要; 仓库只读挂载, 构建与测试在容器可写层, 本机构建产物不受影响.
- `make docker-test-thirdparty` - 五个第三方脚本(tinycc libpng git sqlite cpython, 由小到大); 仅非 x86-64 Linux 宿主需要; `THIRDPARTY=git` 可单跑; 很慢且首次需要网络; clone 缓存在 `thirdparty/`(.gitignore, 损坏就删).
- Rosetta 下 tcc 的 106_pthread / 112_backtrace / 113_btdll 会不稳定 SIGSEGV, 与 chibicc 无关; `test/thirdparty/make` 是 PATH shim(文件名必须是 make), 测试前移除这三个用例; 真 x86-64 Linux 不需要.
- 构建与测试必须在容器可写层(/work): tcc 的 `-run`/bcheck 只认 overlayfs, virtiofs 或 docker volume 上确定性 SIGSEGV; 挂载目录只放源码与 clone 缓存, 也避免 darwin/linux 产物互相污染.
- 镜像内 `git@github.com:` 已重写为 HTTPS; chibicc 只调用 `as` / `ld` 并按 Ubuntu 路径找 crt/libgcc.

## 代码地图

- `libcp/include/libcp/` - 10 个公共头; 消费者只 include `<libcp/libcp.h>`.
- `libcp/src/libcp.h`, `lib.c` - 内部 umbrella 与不透明类型定义; facade(context / per-TU 分配 / 诊断边界 / 公共入口).
- `libcp/src/tokenize.c`, `preprocess.c`, `provenance.c` - 第 1 层与预处理; provenance 记录与两个只读通道(不接编译路径).
- `libcp/src/cst.c` - 第 2 层; 必须在 `libcp_preprocess` 之前建树(预处理会改写输入链).
- `libcp/src/parse.c`, `sema.c`, `type.c` - 第 3/4 层与类型系统.
- `main.c`, `driver.h`, `codegen.c` - 驱动器, 驱动器内部头, x86-64 发射(codegen 不属于库).
- `libcp/src/hashmap.c`, `unicode.c`, `strings.c` - 基础设施.
- `tools/fmt.c`, `tools/c2c.c` - CST formatter 与第 3 层回归闸门(c2c 不安装).
- `bench/bench.c`, `bench/run.sh`, `bench/alloc_shim.{h,c}` - 前端分阶段 benchmark(tests / lib / driver / sqlite 四组语料), 不属于库.
- `include/`, `test/` - 自举头文件; 特性测试与 `test/*.sh` 行为锁.

## 设计原则

- 可读性优先于巧妙. 除用户明确要求外不做顺手的"改进", 不重组无关代码, 不用宏/高阶函数合并解析器里有意的重复. 表达式层是唯一豁免: 一个只读绑定力表(`InfixOp`)加一个 `expr_bp` 循环; 前缀/后缀仍是 `if` 分支, 具名 `BP_*`, 不用函数指针数组, 语句与声明层每产生式一个函数.
- 内存: 每个 per-TU 对象直接来自 `<libcp/allocator.h>` 的 `LIBCP_*`; context 用一条只含链指针的登记链把它们串起来, `libcp_context_new/free/reset` 在边界批量归还; 解析期间不释放; 库不叠加自己的 arena 或池化, 嵌入方的分配器 (包括 arena) 是唯一的分配策略层; 简单但慢的算法可接受.
- codegen 没有优化 pass - 故意的, 保留.
- `include/` 是 chibicc 自带的自举头(`stddef.h`, `stdarg.h`, `stdbool.h`, `float.h`, `stdalign.h`, `stdatomic.h`, `stdnoreturn.h`), 通过 `-Iinclude` 使用.

## 测试约定

- 每个特性对应一个 `test/<feature>.c`, 用 `test/test.h` 的 `ASSERT(expected, expr)` 并链接 `test/common`; 新用例加进对应的特性文件, 仅在必要时扩展 `test/test.h` / `test/common`.
- 行为锁都在 `test/*.sh`, 由 `make test` 与 `make test-stage2` 各跑一遍; 改 codegen 的发射形必须同步更新 `test/shape.sh`, `subst` 新增分支必须同步更新 `test/expand.sh`, UNPARSED golden 用 `test/cst-stats.sh update` 更新.
- 库内自测在 `test/self/`, 使用 `.a` 内部符号, 不属于公共 API.
- `test/thirdparty/*.sh` 很慢且仅限 Linux, 不属于 `make test`.
