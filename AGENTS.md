# AGENTS.md - chibicc

Rui Ueyama 的 chibicc: 一个小型 C11 编译器(约 9k 行, 扁平目录布局), 作为一本编译原理书籍的参考实现编写. 除 `test/` 与 `include/` 外没有其他子目录. 动手改代码前先阅读 `README.md`(尤其是 "Design principles" 一节).

## 全局约定

语言: 本文件及所有输出统一使用中文, 代码标识符与专有名词保留英文原文.
标点: 所有输出(写入文件的注释 / 文档, 以及回显给用户的回复), 无论中文还是英文, 一律使用英文标点(, . : ; ( ) - _ /), 不使用中文标点(如 , 。 ： ； （ ） 、).

## 构建与测试

- `make` - 构建 `chibicc` 二进制(纯 C11, 无外部依赖; CFLAGS 定义在 Makefile 中).
- `make test` - 用 chibicc 自身编译所有 `test/*.c`, 用 `cc -pthread` 链接 `test/common` 后逐个运行, 最后运行 `test/driver.sh ./chibicc`(命令行选项检查).
- `make test-stage2` - 自举检查: 用 chibicc 编译它自己, 再重跑全部测试.
- `make test-all` - 以上两项合计. `make clean` 用于清理.
- 单独运行某个特性测试: `make test/sizeof.exe && ./test/sizeof.exe`(模式规则会自动处理).
- 没有 Linux 后端时只验证前端: `./chibicc -E file.c`(仅预处理)或 `./chibicc -S file.c`(输出汇编文本).

## 平台陷阱与 Docker 测试环境(本工作区为 macOS ARM64)

chibicc 生成的是 x86-64 System V / GAS / ELF 汇编, 且 `main.c` 硬编码了 Linux 路径(`/usr/include/x86_64-linux-gnu` 等)与 gcc/`ld` 调用. 编译器本体在本机**可以构建**, 但它生成的代码无法在本机汇编或运行, 直接跑 `make test` 会在汇编阶段失败. 只做前端检查可用 `./chibicc -E file.c`(仅预处理)或 `./chibicc -S file.c`(输出汇编文本).

完整测试通过 Docker 运行(OrbStack, amd64 容器走 Rosetta, 本机已具备):

- `make docker-image` - 构建依赖镜像 `chibicc:amd64`(ubuntu:22.04, amd64, 一次性约 90 s).
- `make docker-test` - 自研测试集 + 自举, 分钟级. 仓库以只读挂载, 源码拷贝到容器可写层后构建运行, 本机构建产物不受影响.
- `make docker-test-thirdparty` - 依次跑 test/thirdparty 五个脚本(tinycc libpng git sqlite cpython, 由小到大); `THIRDPARTY=git` 可单跑一个. tinycc 实测约 15 s; git/sqlite/cpython 的测试套件需要数小时, 首次运行需要网络 clone. clone 缓存保存在仓库的 `thirdparty/` 目录(已被 .gitignore 忽略, 跨运行复用; 若某个 clone 损坏, 直接删掉对应子目录重新拉).
- Rosetta 下 tcc 的 106_pthread/112_backtrace/113_btdll 三个用例(信号/TLS/线程类)会不稳定 SIGSEGV, 与 chibicc 无关; `test/thirdparty/make` 是一个 PATH shim(文件名必须是 make), 会在 make test 前移除这三个用例的源文件使其被跳过, 其余 110+ 用例照常. 真正的 x86-64 Linux 上无需此 shim.
- 为什么一切都要在容器可写层(/work)跑: 实测 tcc 的 `-run`/bcheck 运行时只认 overlayfs - 在 virtiofs 挂载路径或 docker volume(ext4)上会确定性 SIGSEGV(tinycc 112_backtrace 用例, 失败点还会漂移). 因此挂载目录只提供源码与 clone 缓存, 构建与测试全部发生在 /work, 这同时避免 darwin/linux 产物互相污染.
- 镜像内已把 `git@github.com:` 全局重写为 HTTPS(thirdparty 脚本用的是 SSH 地址, 容器里没有 SSH key); chibicc 运行时只调用 `as` 和 `ld` 并按 Ubuntu 路径找 crt/libgcc, 镜像里的 gcc 工具链满足.

## 架构(流水线; 所有共享类型都定义在 `chibicc.h` 中)

各阶段顺序很重要 - 每个阶段消费上一阶段产生的 token 列表 / 语法树:

1. `tokenize.c` - 把源码字符串切分为 token 列表.
2. `preprocess.c` - 做宏展开与预处理指令, 输入输出都是 token 列表.
3. `parse.c` - 递归下降解析器, 把 token 解析成带类型的 AST. 最大的源文件; 大多数语言特性在这里落地.
4. `codegen.c` - 把 AST 翻译成 x86-64 汇编文本.

辅助文件: `main.c`(驱动: 选项解析, 文件 I/O, 调用 `as`/`ld`), `type.c`(类型系统), `hashmap.c`(字符串驻留哈希表), `unicode.c`(UTF 编码表), `strings.c`(字符串辅助函数). 关键结构体: `Token`, `Obj`(变量/函数), `Node`(AST 节点), `Type` - 均定义在 `chibicc.h` 中.

## 设计原则(有意为之, 不要去"修复")

- 可读性优先于巧妙. 解析器里大量相似的函数是有意的重复; 不要用宏 / 高阶函数重构合并.
- 内存: 一律用 `calloc` 分配, 从不调用 `free`. 简单但慢的算法是可接受的设计. codegen 中没有优化 pass - 这是故意的.
- `include/` 存放 chibicc 自带的自举头文件(`stddef.h`, `stdarg.h`, `stdbool.h`, `float.h`, `stdalign.h`, `stdatomic.h`, `stdnoreturn.h`), 通过 `-Iinclude` 使用.
- 上游把每个 commit 当作书的一节, 且会重写历史; 不要在无关改动中重组或重新排版已有代码.

## 测试约定

- 每个特性对应一个 `test/<feature>.c`; 测试使用 `test/test.h` 中的 `ASSERT(expected, expr)`(打印表达式与结果, 失败即退出), 并链接 `test/common` 以获得 `assert()` 辅助函数与共享符号. 新测试加到对应的特性文件里; 仅在必要时才扩展 `test/test.h`/`test/common`.
- `test/thirdparty/*.sh` 用 chibicc 构建真实项目(git, sqlite, libpng, cpython, tinycc) - 很慢, 仅限 Linux, 不属于 `make test`.
