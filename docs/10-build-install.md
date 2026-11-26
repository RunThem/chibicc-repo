# 10 构建, 安装与测试

## 10.1 环境要求

- C11 编译器. 开发和测试主要用 clang / gcc.
- POSIX 基础环境: 文件系统, `FILE`. 库层不使用线程局部存储; 测试套件用 pthread (`-pthread`).
- GNU make.
- 完整测试需要 Linux x86-64. 生成的目标代码是 x86-64 System V, 在 macOS ARM64 上不能汇编或运行.
- 可选: Docker 或 OrbStack, 用于跑 amd64 容器里的完整闸门.

库本身没有第三方依赖. 它只依赖 libc, 其中包括 POSIX.1-2008 扩展 (`open_memstream`, `localtime_r`, `strnlen`, `dirname` 等).

## 10.2 构建

```sh
make                # 构建 chibicc; Linux 下同时构建 libcp.so
make libcp.a   # 只构建静态库
```

产物:

| 文件 | 说明 |
|---|---|
| `libcp.a` | 静态库, 全部平台 |
| `libcp.so.0` | 共享库, 仅 Linux |
| `libcp.so` | 指向 `libcp.so.0` 的符号链接, 仅 Linux |
| `chibicc` | 参考编译器, 链接 `libcp.a` |
| `libcp.pc` | 仓库根的 pkg-config 文件, 仅由 `make libcp.pc` / `make check-library` 生成, `make`(all) 不构建它; `make install` 直接把生成结果装进 `$PREFIX/lib/pkgconfig/libcp.pc` |
| `stage2/libcp.a` | 自举构建的静态库 |
| `bench/libcp_bench.a` | benchmark 用的库归档, 独立 `-O2` 构建并接 `bench/alloc_shim.h`; 仅 `make bench-tool` / `make bench` |
| `bench/libcp-bench` | benchmark harness; 仅 `make bench-tool` / `make bench` |
| `bench/obj/` | benchmark 的逐编译单元对象; 仅 `make bench-tool` / `make bench` |

编译选项在 Makefile 顶部:

```make
ALLOCATOR_CFLAGS ?= -DLIBCP_DEFAULT_ALLOCATOR
BASE_CFLAGS=-std=c11 -g -fno-common -Wall -Wno-switch
LIB_CFLAGS=$(BASE_CFLAGS) $(ALLOCATOR_CFLAGS) -Ilibcp/include -Ilibcp/src
DRIVER_CFLAGS=$(BASE_CFLAGS) $(ALLOCATOR_CFLAGS) -Ilibcp/include

BENCH_CFLAGS=-std=c11 -O2 -g -fno-common -Wall -Wno-switch \
  -D_POSIX_C_SOURCE=200809L -Ilibcp/include -Ilibcp/src \
  -include bench/alloc_shim.h
```

`LIB_CFLAGS` 用于库对象, 带内部 include; `DRIVER_CFLAGS` 用于 `main.c` / `codegen.c`, 只带公共 include. 两者都带 `ALLOCATOR_CFLAGS`, 因为驱动器包含公共伞头, 分配器配置必须与库一致. `CFLAGS` 保留为 `LIB_CFLAGS` 的别名, 供链接命令与测试工具使用. stage2 规则按同样的方式拆分 include, 并同样带上 `ALLOCATOR_CFLAGS`. 驱动器这一侧的约束由 `test/driver-seam.sh` 锁定: 它检查四个驱动器源文件不含内部 include, 再用只含公共头的临时目录重编驱动器对象.

`BENCH_CFLAGS` 刻意不含 `ALLOCATOR_CFLAGS`: benchmark 的库对象必须走 `bench/alloc_shim.h` 的四个 `LIBCP_*` 宏, 而不是 `LIBCP_DEFAULT_ALLOCATOR`.

## 10.3 目标一览

| 目标 | 作用 |
|---|---|
| `all` | 构建 `chibicc`, Linux 下加 `.so` |
| `chibicc` | 参考编译器 |
| `libcp.a` | 静态库 |
| `libcp.so` | 共享库, 仅 Linux |
| `fmt-tool` | 构建 `fmt`, CST formatter MVP(`tools/fmt.c`), 链接 `libcp.a` |
| `c2c` | C 转 C 打印器(`tools/c2c.c`), 仅测试回归闸门, 不安装 |
| `bench-tool` | 只构建 `bench/libcp-bench`, 不运行也不安装 |
| `bench` | 构建并运行 `bench/run.sh`, 仅 Linux |
| `format` | 用 `.clang-format` 格式化 `libcp/` 与驱动器源码(不涉及测试; 需要 clang-format 20 或更新版本, 缺失时跳过, 版本低于 20 时失败) |
| `format-check` | 只读格式闸门, 检查是否已按 clang-format 20+ 格式化(缺失或版本过低时失败) |
| `test` | 完整测试套件, 不含 `check-library` / thirdparty / bench |
| `test-all` | `test` + `test-stage2` |
| `test-stage2` | 自举测试 |
| `test-thirdparty` | 本机真实项目构建(仅 x86-64 Linux) |
| `check-library` | 公共面与打包闸门 |
| `check-context` | 隐式上下文审计 |
| `check-style` | 全部 tracked 文本文件的中文标点闸门与 HEAD 提交信息检查 |
| `asm-diff` | 比较 `BASE=<ref>` 构建的 `chibicc` 与当前工作树构建的 `chibicc` 的 `-S` 产物 |
| `install` | 安装头文件, 库, pkg-config 文件与 LICENSE |
| `clean` | 清理构建产物 |
| `docker-image` | 构建 amd64 测试镜像 |
| `docker-test` | 容器内 `make test-all` |
| `docker-snapshot` | 生成汇编快照基线 |
| `docker-snapshot-diff` | `test/*.c` 的汇编快照零 diff |
| `docker-test-thirdparty` | 真实项目构建 |

## 10.4 安装

```sh
make install PREFIX=/usr/local
make install PREFIX=/opt/libcp DESTDIR=/tmp/stage
```

安装布局 (以下路径都位于 `$(DESTDIR)$(PREFIX)` 下):

```text
$PREFIX/include/libcp/
  libcp.h
  allocator.h
  context.h
  diagnostic.h
  token.h
  preprocess.h
  provenance.h
  cst.h
  ast.h
  backend.h
  sema.h
$PREFIX/lib/libcp.a
$PREFIX/lib/libcp.so.0        (Linux)
$PREFIX/lib/libcp.so          (Linux, 符号链接)
$PREFIX/lib/pkgconfig/libcp.pc
$PREFIX/share/licenses/libcp/LICENSE
```

`test/install.sh` 校验:

- 恰好 11 个公共头.
- `libcp.a` 存在.
- `libcp.pc` 存在.
- `LICENSE` 存在.
- Linux 下 `.so` 和符号链接存在.
- pkg-config 的版本等于 `LIBCP_VERSION`.
- 用 pkg-config 的 flags 能编译并运行一个消费者.

## 10.5 pkg-config

`libcp.pc.in` 的内容:

```text
prefix=@PREFIX@
exec_prefix=${prefix}
libdir=${exec_prefix}/lib
includedir=${prefix}/include

Name: libcp
Description: Reusable C front end (tokenize, preprocess, CST, parse, sema)
Version: @VERSION@
Cflags: -I${includedir}
Libs: -L${libdir} -lcp
```

使用:

```sh
cc app.c $(pkg-config --cflags --libs libcp)
```

非标准前缀:

```sh
export PKG_CONFIG_PATH=/opt/libcp/lib/pkgconfig
pkg-config --cflags --libs libcp
```

版本:

```sh
pkg-config --modversion libcp   # 0.3.0
```

## 10.6 静态库与共享库

### 10.6.1 静态库

- 归档内全部已定义全局符号统一 `libcp_` 前缀(2026-10-25 起; 当前 157 个, 其中 67 个是公共函数).
- 公共头只声明 67 个 D7 函数.
- 消费者**不得调用**未声明的内部函数. 它们在 `.a` 里可见, 但不是 API.
- 前缀统一后与宿主符号碰撞的风险基本消除; 内外的判据是"公共头是否声明".

这是 L3.2b 的拍板结果(不重命名)被 2026-10-25 的符号前缀线修订后的现状.

### 10.6.2 共享库

只在 Linux 构建:

```make
libcp.so.0: $(LIB_SRCS:.c=.pic.o)
	$(CC) $(CFLAGS) -shared -Wl,-soname,libcp.so.0 -o $@ $^

%.pic.o: %.c ...
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c -o $@ $<
```

- 所有对象用 `-fPIC -fvisibility=hidden` 编译.
- 只有 D7 函数经 `LIBCP_API` 导出.
- `nm -D --defined-only libcp.so` 只含 D7.
- SONAME 是 `libcp.so.0`.

macOS 只承诺 `.a`. 没有 `.dylib` 目标.

### 10.6.3 版本

- `LIBCP_VERSION` 在公共伞头里, 当前 `0.3.0`.
- Makefile 的 `VERSION` 必须与它同步.
- 只承诺源码兼容, 不承诺 ABI.
- 改公共结构体字段就是换版本.

## 10.7 自举

```sh
make test-stage2
```

流程:

1. 用宿主 `cc` 构建 `chibicc`.
2. 用 `chibicc` 编译库源码, 产出 `stage2/libcp.a`; 库对象带内部 include.
3. 用 `chibicc` 编译驱动器和测试; 驱动器对象只带公共 include.
4. 跑 `test-stage2` 的行为测试套件, 包括 `test/driver-seam.sh` (用 `stage2/chibicc` 重编驱动器对象), `test/library.sh` 与 `test/public-api.sh` 链接 `stage2/libcp.a`, `test/self-test.sh`, `test/outside.sh` (用 `LIBCP_LIB` 选择 `stage2/libcp.a`), 以及 `stage2/fmt` / `stage2/c2c`; 后两者由宿主 `cc` 链接 `stage2/libcp.a` 构建. 它不跑 `test/allocator.sh`, 打包闸门 `install.sh`, 隐式 context 审计 `test/context-audit.sh` 与文档闸门 `test/docs-links.sh` / `test/docs-facts.sh`.

自举证明库代码能被自己编译. `setjmp` 与 `va_copy` 都在这条路径上被验证.

## 10.8 测试矩阵

`make test` 覆盖以下类别. 表按类别列出, 不表示执行顺序; 实际最先跑的是 `test/docs-links.sh`, `test/style.sh` 与 `test/docs-facts.sh`, 随后按 `TESTS` 顺序跑 C 测试, 再跑行为脚本, 其中 `$(MAKE) fmt-tool` 与 `$(MAKE) c2c` 是两个显式构建步骤.

| 测试 | 内容 |
|---|---|
| `test/*.exe` | 每个特性一个 C 测试, 覆盖语言功能 |
| `test/driver-seam.sh` | 驱动器 seam: `main.c` / `codegen.c` / `driver.h` / `driver-util.h` 不含内部 include, 驱动器对象只用临时公共头目录编译; `make test` 用宿主 `cc`, `make test-stage2` 用 `stage2/chibicc` |
| `test/driver.sh` | 命令行选项 |
| `test/diagnostic.sh` | 75 例诊断文案与插入符逐字节 |
| `test/warning.sh` | 警告文案与插入符 |
| `test/shape.sh` | 34 段发射形状回归 |
| `test/ast.sh` | 86 例第 3 层 AST 形态逐字节 |
| `test/trivia.sh` | token / trivia / 重建 / `-E -P` 归一化与默认 `-E` 行标记 |
| `test/cst.sh` | 48 个文件 round-trip / 48 个 verify / 15 个边界样本 / 65 个 dump 样本 |
| `test/cst-stats.sh` | CST `UNPARSED` 计数与 `test/cst-stats.golden` 逐行比对 |
| `test/expand.sh` | 31 个 provenance dump 样本, 3 个链形状样本, 17 个 verify 样本, 全量语料 |
| `test/library.sh` | 公共面消费者, 错误路径, 双 context, token / AST dump, ASan/UBSan |
| `test/allocator.sh` | 用替换分配器重编库, 活块归零 |
| `test/self-test.sh` | 库内自测: hashmap / per-TU 分配 / sema 状态重置 |
| `test/outside.sh` | 树外消费者: 只用临时公共头目录与归档, 完成一次降级与 x86-64 发射并走通诊断回调; 默认与 stage2 归档各跑一遍 |
| `test/context-audit.sh` | 隐式 context 审计 (`--strict`, 由 `make test` 调用) |
| `test/docs-links.sh` | 本地 markdown 链接与 AGENTS 章节引用 (由 `make test` 调用) |
| `test/docs-facts.sh` | 当前矩阵文档的机械事实: 诊断 / AST / dump 计数, 公共头数量与版本, 安装布局, D7 数量 (文档专用, 由 `make test` 调用, 不进 `test-stage2`) |
| `test/style.sh` | 全部 tracked 与 untracked 文本文件的中文标点与 HEAD 提交信息检查 (无 `.git` 时用 `find` 扫描并跳过提交检查) |
| `test/fmt.sh` | formatter 幂等 / 行数 / 注释数量 / 有系统头时执行 `-S` 等价 (`make fmt-tool` 构建; 容器内为权威, 原生 x86-64 Linux 也执行) |
| `test/c2c.sh` | C 转 C 打印回归 (`make c2c` 构建) |

`make test` 需要能汇编和运行 x86-64 Linux 目标. 在非 x86-64 Linux 宿主上会在汇编阶段失败. 这些宿主只做前端检查:

```sh
./chibicc -E input.c
./chibicc -S input.c
./chibicc -dump-tokens input.c
./chibicc -reconstruct input.c
./chibicc -dump-cst input.c
./chibicc -reconstruct-cst input.c
./chibicc -verify-cst input.c
./chibicc -dump-expansions input.c
./chibicc -verify-expansions input.c
./chibicc -dump-ast input.c
```

这些通道不需要运行目标代码.

`-E` 默认输出 GCC 形式的行标记 (`# <line> "<file>"`), 在起始, 文件切换与行号跳变处重同步; `-P` 关闭标记, 输出保持归一化文本. 标记逻辑在驱动器 `main.c`, 库不提供 `-E` 输出函数; 宏展开产物的调用点坐标由预处理在展开时冻结在宏名 token 上.

### 10.8.1 前端 benchmark

```sh
make bench-tool   # 只构建 bench/libcp-bench
make bench        # 构建并运行 bench/run.sh, 仅 Linux
```

- `bench/run.sh` 自己检查 `uname -s`; 非 Linux 直接报错, 不采集计时.
- `BENCH_REPEAT` 默认 5, 可覆盖; 正式计时前有 1 轮预热.
- 四组语料: `tests` (`test/*.c` 加 `test/common`), `lib` (`libcp/src/*.c`), `driver` (`main.c` / `codegen.c`), `sqlite` (`thirdparty/sqlite/sqlite3.c`; 该文件不存在时整组跳过).
- 每组输出每个文件的 reset / tokenize / preprocess / parse / sema / total 的 min 与 median 时间, 以及该阶段的分配字节数, 调用次数与峰值.
- 输出开头有环境块与语料指纹 (字节数与 SHA-256), 同一份 stdout 可以自描述地比较.
- 独立 `BENCH_CFLAGS` (`-O2`) 与 `bench/alloc_shim.h`; 不进入 `all` / `test` / `install`.
- CI 只跑 `make bench-tool` 验证可构建, 不采集计时.

## 10.9 公共面闸门

```sh
make check-library
```

运行四个脚本; 前置构建是 `libcp.a`, Linux 下还有 `libcp.so` 与 `libcp.pc` (目标依赖会先构建它们).

1. `test/library.sh`: 用宿主 `cc` 链接 `libcp.a`, 验证:
   - tokenize / preprocess / parse / sema.
   - CST 与 provenance.
   - 诊断回调.
   - 错误返回.
   - reset 复用.
   - 同线程双 context 与跨线程双 context.
   - 64 次 `CST -> reset` 循环的活字节上界为 32 KiB.
   - Linux 下若存在 `libcp.so`, 再用共享库链接同一消费者并运行时验证.
   - ASan + UBSan 重编库对象, 检查零 UB, 活字节上界, free 后回到基线附近 (允许 4 KiB 容差).
2. `test/allocator.sh`: 用 `LIBCP_MALLOC` / `LIBCP_CALLOC` / `LIBCP_REALLOC` / `LIBCP_FREE` 的计数替换版重编整个库, 验证替换分配器确实被调用, 每个 per-TU 对象是一次独立分配, reset 后活块回到基线, 且 `libcp_context_free` 后活块归零.
3. `test/public-api.sh`: 验证:
   - 每个公共头单独 `-fsyntax-only -Wall -Wextra`.
   - 只看到公共头的消费者能编译链接.
   - 消费者对象不引用内部符号; deny list 与 `test/outside.sh` 共用 `test/public-api.deny`.
   - `.a` 中 D7 全部存在.
   - Linux `.so` 只导出 D7.
4. `test/install.sh`: 验证安装布局与 pkg-config; 没有 `pkg-config` 时跳过 `.pc` 版本与消费者检查并成功返回.

`test-stage2` 另用 `LIBCP_LIB=stage2/libcp.a` 跑一遍 `test/public-api.sh` (跳过 `.so` 检查), 与 `test/outside.sh` 一起锁住自举归档的同一公共面.

隐式 context 审计单独运行:

```sh
make check-context
```

它先构建 `libcp.a`, 再以 `--strict` 运行 `test/context-audit.sh`: 检查 `codegen.c` / `main.c` 没有 `libcp_ctx()` 访问器与 `cg_ctx`, `.a` 里没有访问器符号. `make test` 也会调用它.

## 10.10 Docker 闸门

非 x86-64 Linux 宿主 (例如 macOS ARM64 / Intel macOS / Linux arm64) 上, 完整测试必须进 Linux 容器; x86-64 Linux 宿主直接跑 `make test-all`.

```sh
make docker-image
make docker-test
make docker-snapshot-diff
```

- `docker-test` 在 amd64 容器里运行 `make test-all`(即 `test` + `test-stage2`), 包括全部只读通道, 但不运行 `make check-library` 的打包闸门.
- `docker-snapshot-diff` 对 `test/*.c` 的全部语料比较 stage1 `chibicc` 的汇编快照, 必须输出 `snapshot diff: empty`; 当前不包含自举产物.
- `docker-test` 与 `docker-snapshot*` 把仓库只读挂载 (`-v $(CURDIR):/src:ro`), 源码复制到容器可写层 `/work` 后构建, 本机产物不受影响.
- `docker-test-thirdparty` 需要把 clone 缓存写回 `thirdparty/`, 因此用读写挂载 (`-v $(CURDIR):/src`), 并在容器结束后把 `/work/thirdparty` 的新内容同步回仓库.
- 首次 `docker-image` 约 90 秒.

真实项目:

```sh
make docker-test-thirdparty
THIRDPARTY=tinycc make docker-test-thirdparty
```

依次构建 tinycc, libpng, git, sqlite, cpython. 很慢, 不属于 `make test`.

真正的 x86-64 Linux 宿主可以直接跑本地版:

```sh
make test-thirdparty
THIRDPARTY="tinycc libpng" make test-thirdparty
```

本地版不把 `test/thirdparty` 放进 PATH, 也不把源码拷到 `/work`. Rosetta 下 tcc 的 106_pthread / 112_backtrace / 113_btdll 会不稳定 SIGSEGV, 与 chibicc 无关, 因此 `test/thirdparty/make` 是只为 Rosetta 服务的 PATH shim, 真 x86-64 Linux 不需要.

若 `thirdparty/cpython` 上次是用 chibicc 构建的, 可以直接用 `thirdparty/cpython/python -m unittest <test>` 做单条验证; 全量 `test/thirdparty/cpython.sh` 会先 `git reset --hard` 再重建. 脚本给 `test_peg_generator` 的扩展链接追加 `-Wl,-Bsymbolic`, 因为 chibicc 不支持 `-fvisibility=hidden`; 原因与被否决的方案见 [ADR 0002](adr/0002-cpython-peg-bsymbolic.md).

容器内构建与测试必须在容器可写层 `/work` 进行: tcc 的 `-run`/bcheck 只认 overlayfs, 在 virtiofs 或 docker volume 上会确定性 SIGSEGV; 挂载目录只放源码与 clone 缓存.

### 10.10.1 CI 闸门

`.github/workflows/` 有三个 workflow:

| workflow | job / 命令 | 覆盖 |
|---|---|---|
| `test.yml` | `make test-all`, `make check-library`, `make bench-tool` | 原生 x86-64 Linux 的单元, 自举, 公共面与 benchmark 构建 |
| `test.yml` | `make test-thirdparty THIRDPARTY="tinycc libpng git sqlite"` | 真实项目子集; 依赖前一个 job, 超时 120 分钟, 缓存 `thirdparty/` |
| `format.yml` | 安装 `clang-format==20.1.8` 后 `make format-check` | 只读格式闸门 |
| `docs.yml` | `bash test/docs-links.sh` | 文档链接与 AGENTS 章节引用 |

`make test-all` 覆盖 `test/style.sh` (由 `make test` 调用).

cpython 只在手工 Docker 闸门里跑, 不进 CI. 没有 macOS job.

## 10.11 在别的项目里使用

### 10.11.1 直接链接静态库

```sh
cc -std=c11 -I/path/to/libcp/include app.c /path/to/libcp.a -o app
```

适合:

- 单文件或小项目.
- 不想处理动态库路径.
- 能接受 `.a` 的内部符号可见.

### 10.11.2 安装后使用 pkg-config

```sh
make install PREFIX=/opt/libcp
cc app.c $(PKG_CONFIG_PATH=/opt/libcp/lib/pkgconfig \
           pkg-config --cflags --libs libcp)
```

### 10.11.3 把库源码嵌进项目

不推荐, 但可行. 编译 `libcp/src/*.c`, 加 `-Ilibcp/include -Ilibcp/src`, 不要编译 `main.c` / `codegen.c`. `libcp/src/libcp.h` 会定义 `LIBCP_INTERNAL`, 因此还必须给 `-DLIBCP_DEFAULT_ALLOCATOR`, 或提供全部四个 `LIBCP_*` 宏 (见 10.11.5). 这样可以只保留需要的层, 但会失去版本管理.

### 10.11.4 共享库

Linux 下链接 `-lcp`, 运行时需要 `LD_LIBRARY_PATH` 或安装到系统路径. 共享库只导出 D7, 没有内部符号碰撞风险.

### 10.11.5 替换分配器

`<libcp/allocator.h>` 只有两个构建期状态:

- 定义 `LIBCP_DEFAULT_ALLOCATOR`: 四个宏展开为 C 库的 `malloc` / `calloc` / `realloc` / `free`. 默认构建就是这样.
- 不定义它: 嵌入方提供 `LIBCP_MALLOC` / `LIBCP_CALLOC` / `LIBCP_REALLOC` / `LIBCP_FREE` 四个宏. 构建 libcp 时缺一个就在包含处 `#error`, 不会静默回落到 libc.

Makefile 的 `ALLOCATOR_CFLAGS` 默认是 `-DLIBCP_DEFAULT_ALLOCATOR`. 自定义构建覆盖它即可, 下面是 mimalloc 的例子:

```sh
# -include 让每个库编译单元看到 mimalloc 的声明; -D 定义的是对象式
# 宏, 因此 LIBCP_MALLOC(size) 展开成 mi_malloc(size), 其余同理.
make clean
make libcp.a ALLOCATOR_CFLAGS='-include mimalloc.h -DLIBCP_MALLOC=mi_malloc -DLIBCP_CALLOC=mi_calloc -DLIBCP_REALLOC=mi_realloc -DLIBCP_FREE=mi_free'
```

`mimalloc.h` 必须在编译器的 include 路径里; 不在的话把 `-I/usr/local/include` 加进 `ALLOCATOR_CFLAGS`, 或把 `-include` 的路径写成绝对路径.

如果要连 `chibicc` 驱动器一起构建, 链接时还要给出 mimalloc:

```sh
make ALLOCATOR_CFLAGS='-include mimalloc.h -DLIBCP_MALLOC=mi_malloc -DLIBCP_CALLOC=mi_calloc -DLIBCP_REALLOC=mi_realloc -DLIBCP_FREE=mi_free' LDFLAGS='-lmimalloc'
```

要点:

- 不要同时定义 `LIBCP_DEFAULT_ALLOCATOR`, 否则四个宏会被 libc 版本覆盖(重定义警告), 自定义分配器不会生效.
- 自定义构建的文件必须是库的每个编译单元都能看到的, 并且链接时提供这四个函数.
- 替换实现必须满足 libc 的对齐, 清零与 `realloc` 失败语义, 见 9.3.15.
- `make test-stage2` 的 stage2 规则使用同一个 `ALLOCATOR_CFLAGS` (默认 `-DLIBCP_DEFAULT_ALLOCATOR`), 库对象与驱动器对象都带上它.
- `test/allocator.sh` 只验证替换机制本身 (四个宏被调用, 每次 per-TU 分配独立, reset 后活块归零); 自定义分配器要让它生效, 仍需自己的 shim 头与链接参数, 不能只靠这个脚本.
- 只给消费者加宏不会影响预编译的库.

## 10.12 常见问题

**`make test` 在非 x86-64 Linux 宿主上失败, `as` 报错.**

这是预期的. chibicc 生成 x86-64 ELF 汇编, 其他宿主 (包括 macOS) 的汇编器不接受. 用 Docker, 或只跑前端通道.

**找不到 `<libcp/libcp.h>`.**

编译时加 `-Ilibcp/include`. 安装后加 `-I$PREFIX/include`. 头文件目录结构是 `include/libcp/*.h`, 不是 `include/*.h`.

**链接时报未定义符号.**

确认链接了 `libcp.a` 或 `-lcp`. 如果用的是 stage2 库, 确认路径.

**`pkg-config` 找不到 `libcp`.**

设置 `PKG_CONFIG_PATH` 指向 `$PREFIX/lib/pkgconfig`.

**运行时报 `libcp.so.0` 找不到.**

把 `$PREFIX/lib` 加到 `LD_LIBRARY_PATH`, 或运行 `ldconfig`.

**`#include <stdio.h>` 找不到.**

库不安装系统 include 路径. 消费者要自己注册目标平台需要的路径; 系统目录用 `libcp_context_add_include_path_with_origin(ctx, "/usr/include", LIBCP_INCLUDE_SYSTEM)`, 这样才能与用户 `-I` 目录区分. 参考驱动器的 `add_default_include_paths`.

**`make check-library` 在非 Linux 宿主上通过吗.**

通过. `test/install.sh` 在非 Linux 上跳过 `.so` 检查; 没有 `pkg-config` 时跳过 `.pc` 版本与消费者检查并成功返回. `test/library.sh` 用宿主 `cc` 编译和运行, 不需要 x86-64 目标代码.

**如何只构建库, 不构建编译器.**

`make libcp.a`. 这不会编译 `main.c` / `codegen.c`.
