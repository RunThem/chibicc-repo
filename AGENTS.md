# AGENTS.md - chibicc(C 前端库 fork)

本仓库是 Rui Ueyama 的 chibicc(小型 C11 编译器, 约 9k 行)的 fork. 库化已完成:

**C 前端(tokenize / preprocess / parse / sema)是一个可复用的库**. chibicc 本体退化为"驱动器 + x86-64 codegen", 作为这个库的参考消费者. 上游 README(含 "Design principles" 一节)描述的是上游的教学定位; 与本文件冲突时, 以本文件为准. 库文档在 `docs/`.

库名 `libcp`(展开为 "Library C Parse", 是品牌 token 而不是描述), 二进制仍叫 `chibicc`, 预定义宏 `__chibicc__` 不变; 公共头根 `libcp/include/libcp/`, 公共符号 `libcp_` / `Libcp` 前缀; 库实现全部在 `libcp/src/`(含 facade `lib.c` 与内部 umbrella `libcp.h`), 根目录只保留驱动器 `main.c` 与 `codegen.c`. 只承诺源码兼容, 不承诺 ABI.

## 全局约定

语言: 本文件及所有输出统一使用中文, 代码标识符与专有名词保留英文原文.
标点: 所有输出(写入文件的注释 / 文档, 以及回显给用户的回复), 无论中文还是英文, 一律使用英文标点(, . : ; ( ) - _ /), 不使用中文标点(如 , 。 ： ； （ ） 、).

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
- 库层无隐式上下文: 新增或修改的内部函数若直接或传递地触碰 arena, 格式化, 容器, 诊断或 context 字段, 必须以 `LibcpContext *ctx` 作首参. 不得新增 `libcp_ctx()` 或 TLS; `make check-context` 是必跑门; `codegen.c` 不得有 `cg_ctx`.
- API 纪律: 公共头(`libcp/include/libcp/`)与内部头(`libcp/src/libcp.h`)分离, 不泄漏编译器内部假设(Linux 路径, 单次进程生命周期, 直接 exit 等).
- 后端整形只允许出现在 codegen.c: 指针缩放, 读写回环, 临时槽, 语句重排, 标签与唯一名分配都不是库层的事; 库层只做标注与语义结论.
- 检查与诊断留在库层: 左值性, 实参个数, 位域取地址, VLA 不得初始化, 不完整类型等检查不得随后端搬走; 文案与锚点以 `test/diagnostic.sh` 为准.

## 已定边界(新决策仍由用户拍板, 不擅自推翻)

- 命名: `libcp` / `<libcp/...>` / `libcp_` / `Libcp` / `LIBCP_`; 库内私有类型同样带 `Libcp` / `struct libcp_*`; 二进制 `chibicc`, 预定义宏 `__chibicc__` 与 `plans/**` 的旧名不回溯改写; 只承诺源码兼容.
- 公共 IR: `LibcpNode` / `LibcpObj` 只保留第 3/4 层字段; codegen 状态在 `codegen.c` 私有表; sema 只标注, 不整形.
- context: 一个 context 一个翻译单元; `new` / `free` / `reset` 三态; reset 保留配置并释放全部 per-TU 内存; 不做逐对象 free, 不做 per-phase arena.
- 宏保真: 消费展开后的 token 流, 不引入宏调用节点; 宏保真由 CST 与 provenance 承担; 放弃宏展开内部的外科式编辑.
- CST 只建在预处理之前: 不做展开流 CST, 也不从 CST 反向还原宏展开前的 token 流.
- 公共面不提供 per-node user data 扩展点, 也不提供 trivia 分段迭代器: 两个消费者(formatter 与 c2c)都只需要只读遍历.
- c2c 只是 `make test` / `make test-stage2` 的回归闸门: 不进入 `all`, 不安装, 不做产品化(改写 / 插桩 / 重构 / 源码回写都不在范围).
- 分配器: `<libcp/allocator.h>` 的四个 `LIBCP_*` 宏; 单开关 `LIBCP_DEFAULT_ALLOCATOR`, 未定义时嵌入方必须提供全部四个, 不做逐宏回退; 编译期配置, 只有重编库才生效.
- 公共面: 10 个公共头只声明 D7; `.a` 的内部符号统一 `libcp_` 前缀但不是 API(消费者不得调用, 不承诺源码兼容); `.so` 只导出 D7.

## 构建与测试

- `make` - 构建 `chibicc` 二进制(纯 C11, 无外部依赖; CFLAGS 定义在 Makefile 中).
- `make check-context` - 隐式上下文审计(`test/context-audit.sh --strict`): 库层无访问器 / 无 TLS / `.a` 无访问器符号, `codegen.c` 无 `cg_ctx`; 由 `make test` 一并调用.
- `make test` - 用 chibicc 自身编译所有 `test/*.c`, 用 `cc -pthread` 链接 `test/common` 后逐个运行, 再依次跑 `test/driver.sh`(命令行选项), `test/diagnostic.sh`(诊断逐字节), `test/warning.sh`(警告逐字节), `test/shape.sh`(发射形状), `test/ast.sh`(第 3 层 AST 形态), `test/trivia.sh`(trivia 通道), `test/cst.sh`(CST 通道), `test/expand.sh`(provenance), `test/library.sh`(公共面与 ASan), `test/allocator.sh`(分配器替换), `test/self-test.sh`(库内自测), `test/cst-stats.sh`(UNPARSED golden), 最后 `make fmt` + `test/fmt.sh`(formatter)与 `make c2c` + `test/c2c.sh`(第 3 层回归闸门, 不随 `make` / `make install` 发布).
- `make test-stage2` - 自举检查: 用 chibicc 编译它自己, 再重跑全部测试.
- `make test-all` - 以上两项合计. `make clean` 用于清理.
- 单独运行某个特性测试: `make test/sizeof.exe && ./test/sizeof.exe`(模式规则会自动处理).
- 只验证前端(本机无 Linux 后端): `-E`(预处理), `-S`(汇编文本), `-dump-tokens` / `-reconstruct`(第 1 层), `-dump-cst` / `-reconstruct-cst` / `-verify-cst`(第 2 层), `-dump-expansions` / `-verify-expansions`(provenance), `-dump-ast`(第 3 层, parse 后 sema 前早退).
- 库构建: `make libcp.a`; Linux 下 `make` 也产 `libcp.so`(PIC, 只导出 D7, SONAME `libcp.so.0`; macOS 只承诺 `.a`), `chibicc` 链接静态库; `make install PREFIX=... [DESTDIR=...]` 装 10 个公共头, 库与 `libcp.pc`, `pkg-config --cflags --libs libcp` 可用; `LIBCP_VERSION` / `libcp_version()` 为 `0.1.0`. `make test-stage2` 用 chibicc 重编库对象(`stage2/libcp.a`). `make check-library` 跑 library / allocator / public-api / install 四个脚本(头文件自包含, 消费者只依赖 D7, `.a` 的 D7 齐全, `.so` 只导出 D7, install / pkg-config 布局); `test/library.sh` 最后一轮用 ASan+UBSan 重编库对象, 查 UB 与 reset 后活字节归零.
- 每次改动必须过三道闸门: `make docker-test`(含自举), 汇编等价性 diff(`make docker-snapshot-diff`), 该改动新增的针对性测试. 行为变化必须单独提交.

## 平台陷阱与 Docker 测试环境(本工作区为 macOS ARM64)

chibicc 生成的是 x86-64 System V / GAS / ELF 汇编, 且 `main.c` 硬编码了 Linux 路径(`/usr/include/x86_64-linux-gnu` 等)与 gcc/`ld` 调用. 编译器本体在本机**可以构建**, 但它生成的代码无法在本机汇编或运行, 直接跑 `make test` 会在汇编阶段失败.

完整测试通过 Docker 运行(OrbStack, amd64 容器走 Rosetta, 本机已具备):

- `make docker-image` - 构建依赖镜像 `chibicc:amd64`(ubuntu:22.04, amd64, 一次性约 90 s).
- `make docker-test` - 自研测试集 + 自举, 分钟级; 仓库只读挂载, 构建与测试在容器可写层, 本机构建产物不受影响.
- `make docker-test-thirdparty` - 五个第三方脚本(tinycc libpng git sqlite cpython, 由小到大); `THIRDPARTY=git` 可单跑; 很慢且首次需要网络; clone 缓存在 `thirdparty/`(.gitignore, 损坏就删).
- Rosetta 下 tcc 的 106_pthread / 112_backtrace / 113_btdll 会不稳定 SIGSEGV, 与 chibicc 无关; `test/thirdparty/make` 是 PATH shim(文件名必须是 make), 测试前移除这三个用例; 真 x86-64 Linux 不需要.
- 构建与测试必须在容器可写层(/work): tcc 的 `-run`/bcheck 只认 overlayfs, virtiofs 或 docker volume 上确定性 SIGSEGV; 挂载目录只放源码与 clone 缓存, 也避免 darwin/linux 产物互相污染.
- 镜像内 `git@github.com:` 已重写为 HTTPS; chibicc 只调用 `as` / `ld` 并按 Ubuntu 路径找 crt/libgcc.

## 代码地图

- `libcp/include/libcp/` - 10 个公共头; 消费者只 include `<libcp/libcp.h>`.
- `libcp/src/libcp.h`, `lib.c` - 内部 umbrella 与不透明类型定义; facade(context / arena / 诊断边界 / 公共入口).
- `libcp/src/tokenize.c`, `preprocess.c`, `provenance.c` - 第 1 层与预处理; provenance 记录与两个只读通道(不接编译路径).
- `libcp/src/cst.c` - 第 2 层; 必须在 `libcp_preprocess` 之前建树(预处理会改写输入链).
- `libcp/src/parse.c`, `sema.c`, `type.c` - 第 3/4 层与类型系统.
- `main.c`, `driver.h`, `codegen.c` - 驱动器, 驱动器内部头, x86-64 发射(codegen 不属于库).
- `libcp/src/hashmap.c`, `unicode.c`, `strings.c` - 基础设施.
- `tools/fmt.c`, `tools/c2c.c` - CST formatter 与第 3 层回归闸门(c2c 不安装).
- `include/`, `test/` - 自举头文件; 特性测试与 `test/*.sh` 行为锁.

## 设计原则

- 可读性优先于巧妙. 除用户明确要求外不做顺手的"改进", 不重组无关代码, 不用宏/高阶函数合并解析器里有意的重复. 表达式层是唯一豁免: 一个只读绑定力表(`InfixOp`)加一个 `expr_bp` 循环; 前缀/后缀仍是 `if` 分支, 具名 `BP_*`, 不用函数指针数组, 语句与声明层每产生式一个函数.
- 内存: 整块分配, 从不逐对象 `free`; 一个 context 一个翻译单元一个 arena, 整体 `libcp_context_new/free/reset`; 底层块来自 `<libcp/allocator.h>` 的 `LIBCP_*`; 简单但慢的算法可接受.
- codegen 没有优化 pass - 故意的, 保留.
- `include/` 是 chibicc 自带的自举头(`stddef.h`, `stdarg.h`, `stdbool.h`, `float.h`, `stdalign.h`, `stdatomic.h`, `stdnoreturn.h`), 通过 `-Iinclude` 使用.

## 测试约定

- 每个特性对应一个 `test/<feature>.c`, 用 `test/test.h` 的 `ASSERT(expected, expr)` 并链接 `test/common`; 新用例加进对应的特性文件, 仅在必要时扩展 `test/test.h` / `test/common`.
- 行为锁都在 `test/*.sh`, 由 `make test` 与 `make test-stage2` 各跑一遍; 改 codegen 的发射形必须同步更新 `test/shape.sh`, `subst` 新增分支必须同步更新 `test/expand.sh`, UNPARSED golden 用 `test/cst-stats.sh update` 更新.
- 库内自测在 `test/self/`, 使用 `.a` 内部符号, 不属于公共 API.
- `test/thirdparty/*.sh` 很慢且仅限 Linux, 不属于 `make test`.
