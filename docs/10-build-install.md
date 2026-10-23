# 10 构建, 安装与测试

## 10.1 环境要求

- C11 编译器. 开发和测试主要用 clang / gcc.
- POSIX 基础环境: 文件系统, `FILE`. 库层不使用线程局部存储; 测试套件用 pthread (`-pthread`).
- GNU make.
- 完整测试需要 Linux x86-64. 生成的目标代码是 x86-64 System V, 在 macOS ARM64 上不能汇编或运行.
- 可选: Docker 或 OrbStack, 用于跑 amd64 容器里的完整闸门.

库本身没有第三方依赖. 它只链接 C 标准库.

## 10.2 构建

```sh
make                # 构建 chibicc; Linux 下同时构建 libchibicc.so
make libchibicc.a   # 只构建静态库
```

产物:

| 文件 | 说明 |
|---|---|
| `libchibicc.a` | 静态库, 全部平台 |
| `libchibicc.so.0` | 共享库, 仅 Linux |
| `libchibicc.so` | 指向 `libchibicc.so.0` 的符号链接, 仅 Linux |
| `chibicc` | 参考编译器, 链接 `libchibicc.a` |
| `libchibicc.pc` | pkg-config 文件, 由 `make` 生成 |
| `pratt/libchibicc.a` | Pratt 表达式解析构建的静态库, 仅显式构建 |
| `stage2/libchibicc.a` | 自举构建的静态库 |

编译选项在 Makefile 顶部:

```make
CFLAGS=-std=c11 -g -fno-common -Wall -Wno-switch \
       -Ilibchibicc/include -Ilibchibicc/src
```

## 10.3 目标一览

| 目标 | 作用 |
|---|---|
| `all` | 构建 `chibicc`, Linux 下加 `.so` |
| `chibicc` | 参考编译器 |
| `libchibicc.a` | 静态库 |
| `libchibicc.so` | 共享库, 仅 Linux |
| `chibicc-pratt` | Pratt 表达式解析构建的编译器 |
| `pratt/libchibicc.a` | Pratt 构建的静态库 |
| `test` | 完整测试套件 |
| `test-stage2` | 自举测试 |
| `test-pratt` | Pratt 构建的测试套件 |
| `test-both` | 默认与 Pratt 两套测试 |
| `test-ab` | 两套构建的 AST 与汇编 A/B |
| `test-stage2-pratt` | Pratt 构建的自举 |
| `check-library` | 公共面与打包闸门 |
| `check-library-pratt` | Pratt 库的公共面闸门 |
| `install` | 安装头文件, 库与 pkg-config |
| `clean` | 清理构建产物 |
| `docker-image` | 构建 amd64 测试镜像 |
| `docker-test` | 容器内完整测试与自举 |
| `docker-snapshot-diff` | 汇编快照零 diff |
| `docker-test-thirdparty` | 真实项目构建 |
| `docker-pratt-ab` | 容器内 Pratt A/B |
| `docker-pratt-tinycc` | 容器内 tinycc 真实构建 |

## 10.4 安装

```sh
make install PREFIX=/usr/local
make install PREFIX=/opt/libchibicc DESTDIR=/tmp/stage
```

安装布局:

```text
$PREFIX/include/chibicc/
  chibicc.h
  allocator.h
  context.h
  diagnostic.h
  token.h
  preprocess.h
  provenance.h
  cst.h
  ast.h
  sema.h
$PREFIX/lib/libchibicc.a
$PREFIX/lib/libchibicc.so.0        (Linux)
$PREFIX/lib/libchibicc.so          (Linux, 符号链接)
$PREFIX/lib/pkgconfig/libchibicc.pc
```

`test/install.sh` 校验:

- 恰好 10 个公共头.
- `libchibicc.a` 存在.
- `libchibicc.pc` 存在.
- Linux 下 `.so` 和符号链接存在.
- pkg-config 的版本等于 `CHIBICC_VERSION`.
- 用 pkg-config 的 flags 能编译并运行一个消费者.

## 10.5 pkg-config

`libchibicc.pc.in` 的内容:

```text
prefix=@PREFIX@
exec_prefix=${prefix}
libdir=${exec_prefix}/lib
includedir=${prefix}/include

Name: libchibicc
Description: Reusable C front end (tokenize, preprocess, CST, parse, sema)
Version: @VERSION@
Cflags: -I${includedir}
Libs: -L${libdir} -lchibicc
```

使用:

```sh
cc app.c $(pkg-config --cflags --libs libchibicc)
```

非标准前缀:

```sh
export PKG_CONFIG_PATH=/opt/libchibicc/lib/pkgconfig
pkg-config --cflags --libs libchibicc
```

版本:

```sh
pkg-config --modversion libchibicc   # 0.1.0
```

## 10.6 静态库与共享库

### 10.6.1 静态库

- 全部内部函数都是 `T` 符号, 没有源码级重命名.
- 公共头只声明 36 个 D7 函数.
- 消费者**不得调用**未声明的内部函数. 它们在 `.a` 里可见, 但不是 API.
- 风险: 内部符号可能与宿主程序同名, 造成符号碰撞. 链接静态库时要注意.

这是 L3.2b 的拍板结果: 不做源码级重命名, 用文档声明约束.

### 10.6.2 共享库

只在 Linux 构建:

```make
libchibicc.so.0: $(LIB_SRCS:.c=.pic.o)
	$(CC) $(CFLAGS) -shared -Wl,-soname,libchibicc.so.0 -o $@ $^

%.pic.o: %.c ...
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c -o $@ $<
```

- 所有对象用 `-fPIC -fvisibility=hidden` 编译.
- 只有 D7 函数经 `CHIBICC_API` 导出.
- `nm -D libchibicc.so` 只含 D7.
- SONAME 是 `libchibicc.so.0`.

macOS 只承诺 `.a`. 没有 `.dylib` 目标.

### 10.6.3 版本

- `CHIBICC_VERSION` 在公共伞头里, 当前 `0.1.0`.
- Makefile 的 `VERSION` 必须与它同步.
- 只承诺源码兼容, 不承诺 ABI.
- 改公共结构体字段就是换版本.

## 10.7 自举

```sh
make test-stage2
```

流程:

1. 用宿主 `cc` 构建 `chibicc`.
2. 用 `chibicc` 编译库源码, 产出 `stage2/libchibicc.a`.
3. 用 `chibicc` 编译驱动器和测试.
4. 跑全部测试, 包括 `test/library.sh` 链接 `stage2/libchibicc.a`.

自举证明库代码能被自己编译. `setjmp` 与 `va_copy` 都在这条路径上被验证.

Pratt 版本:

```sh
make test-stage2-pratt
```

用 `chibicc-pratt` 重编全部库对象, 再跑测试.

## 10.8 测试矩阵

`make test` 依次运行:

| 测试 | 内容 |
|---|---|
| `test/*.exe` | 每个特性一个 C 测试, 覆盖语言功能 |
| `test/driver.sh` | 命令行选项 |
| `test/diagnostic.sh` | 68 例诊断文案与插入符逐字节 |
| `test/warning.sh` | 警告文案与插入符 |
| `test/shape.sh` | 34 段发射形状回归 |
| `test/ast.sh` | 85 例第 3 层 AST 形态逐字节 |
| `test/trivia.sh` | token / trivia / 重建 / `-E` 锚点 |
| `test/cst.sh` | 305 项 CST 断言与 61 个 dump 样本 |
| `test/expand.sh` | 29 个 provenance dump 样本, 3 个链形状样本, 17 个 verify 样本, 全量语料 |
| `test/library.sh` | 公共面消费者, 错误路径, 双 context, ASan/UBSan |
| `test/allocator.sh` | 用替换分配器重编库, 活块归零 |
| `test/self-test.sh` | 库内自测: hashmap / arena / sema 状态重置 |

`make test` 需要能汇编和运行 x86-64 Linux 目标. 在 macOS 上会在汇编阶段失败. 本机只做前端检查:

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

## 10.9 公共面闸门

```sh
make check-library
```

运行四个脚本:

1. `test/library.sh`: 用宿主 `cc` 链接 `libchibicc.a`, 验证:
   - tokenize / preprocess / parse / sema.
   - CST 与 provenance.
   - 诊断回调.
   - 错误返回.
   - reset 复用.
   - 同线程双 context 与跨线程双 context.
   - ASan + UBSan 重编库对象, 检查零 UB, 活字节上界, free 后回基线.
2. `test/allocator.sh`: 用 `CHIBICC_MALLOC` / `CHIBICC_CALLOC` / `CHIBICC_REALLOC` / `CHIBICC_FREE` 的计数替换版重编整个库, 验证替换分配器确实被调用, 且 `chibicc_context_free` 后活块归零.
3. `test/public-api.sh`: 验证:
   - 每个公共头单独 `-fsyntax-only -Wall -Wextra`.
   - 只看到公共头的消费者能编译链接.
   - 消费者对象不引用内部符号.
   - `.a` 中 D7 全部存在.
   - Linux `.so` 只导出 D7.
4. `test/install.sh`: 验证安装布局与 pkg-config.

## 10.10 Docker 闸门

本机是 macOS ARM64, 完整测试必须进 Linux 容器.

```sh
make docker-image
make docker-test
make docker-snapshot-diff
```

- `docker-test` 在 amd64 容器里跑完整测试, 自举, 全部只读通道和库闸门.
- `docker-snapshot-diff` 对全部语料和自举产物比较汇编快照, 必须输出 `snapshot diff: empty`.
- 仓库只读挂载, 源码复制到容器可写层 `/work` 后构建, 本机产物不受影响.
- 首次 `docker-image` 约 90 秒.

真实项目:

```sh
make docker-test-thirdparty
THIRDPARTY=tinycc make docker-test-thirdparty
```

依次构建 tinycc, libpng, git, sqlite, cpython. 很慢, 不属于 `make test`.

Pratt A/B:

```sh
make docker-pratt-ab
make docker-pratt-tinycc
```

`test/pratt-ab.sh` 对 41 个语料文件比较 `-dump-ast` 与 `-S`; `--thirdparty tinycc` 捕获 tinycc 构建的每条 `-c` 命令, 比较 AST 与汇编, 并让两个构建各跑 tinycc 测试套件.

## 10.11 在别的项目里使用

### 10.11.1 直接链接静态库

```sh
cc -std=c11 -I/path/to/libchibicc/include app.c /path/to/libchibicc.a -o app
```

适合:

- 单文件或小项目.
- 不想处理动态库路径.
- 能接受 `.a` 的内部符号可见.

### 10.11.2 安装后使用 pkg-config

```sh
make install PREFIX=/opt/libchibicc
cc app.c $(PKG_CONFIG_PATH=/opt/libchibicc/lib/pkgconfig \
           pkg-config --cflags --libs libchibicc)
```

### 10.11.3 把库源码嵌进项目

不推荐, 但可行. 编译 `libchibicc/src/*.c`, 加 `-Ilibchibicc/include -Ilibchibicc/src`, 不要编译 `main.c` / `codegen.c`. 这样可以只保留需要的层, 但会失去版本管理.

### 10.11.4 共享库

Linux 下链接 `-lchibicc`, 运行时需要 `LD_LIBRARY_PATH` 或安装到系统路径. 共享库只导出 D7, 没有内部符号碰撞风险.

### 10.11.5 替换分配器

`<chibicc/allocator.h>` 只有两个构建期状态:

- 定义 `CHIBICC_DEFAULT_ALLOCATOR`: 四个宏展开为 C 库的 `malloc` / `calloc` / `realloc` / `free`. 默认构建就是这样.
- 不定义它: 嵌入方提供 `CHIBICC_MALLOC` / `CHIBICC_CALLOC` / `CHIBICC_REALLOC` / `CHIBICC_FREE` 四个宏. 构建 libchibicc 时缺一个就在包含处 `#error`, 不会静默回落到 libc.

Makefile 的 `ALLOCATOR_CFLAGS` 默认是 `-DCHIBICC_DEFAULT_ALLOCATOR`. 自定义构建覆盖它即可, 下面是 mimalloc 的例子:

```sh
# -include 让每个库编译单元看到 mimalloc 的声明; -D 定义的是对象式
# 宏, 因此 CHIBICC_MALLOC(size) 展开成 mi_malloc(size), 其余同理.
make clean
make libchibicc.a ALLOCATOR_CFLAGS='-include mimalloc.h -DCHIBICC_MALLOC=mi_malloc -DCHIBICC_CALLOC=mi_calloc -DCHIBICC_REALLOC=mi_realloc -DCHIBICC_FREE=mi_free'
```

`mimalloc.h` 必须在编译器的 include 路径里; 不在的话把 `-I/usr/local/include` 加进 `ALLOCATOR_CFLAGS`, 或把 `-include` 的路径写成绝对路径.

如果要连 `chibicc` 驱动器一起构建, 链接时还要给出 mimalloc:

```sh
make ALLOCATOR_CFLAGS='-include mimalloc.h -DCHIBICC_MALLOC=mi_malloc -DCHIBICC_CALLOC=mi_calloc -DCHIBICC_REALLOC=mi_realloc -DCHIBICC_FREE=mi_free' LDFLAGS='-lmimalloc'
```

要点:

- 不要同时定义 `CHIBICC_DEFAULT_ALLOCATOR`, 否则四个宏会被 libc 版本覆盖(重定义警告), 自定义分配器不会生效.
- 自定义构建的文件必须是库的每个编译单元都能看到的, 并且链接时提供这四个函数.
- 替换实现必须满足 libc 的对齐, 清零与 `realloc` 失败语义, 见 9.3.11.
- `make test-stage2` 的 stage2 规则固定传 `-DCHIBICC_DEFAULT_ALLOCATOR`, 即自举检查始终用默认分配器; 自定义分配器用宿主 cc 构建 `libchibicc.a`, 并可用 `test/allocator.sh` 的计数版验证.
- 只给消费者加宏不会影响预编译的库.

## 10.12 常见问题

**`make test` 在 macOS 上失败, `as` 报错.**

这是预期的. chibicc 生成 x86-64 ELF 汇编, Apple 的 `as` 不接受. 用 Docker, 或只跑前端通道.

**找不到 `<chibicc/chibicc.h>`.**

编译时加 `-Ilibchibicc/include`. 安装后加 `-I$PREFIX/include`. 头文件目录结构是 `include/chibicc/*.h`, 不是 `include/*.h`.

**链接时报未定义符号.**

确认链接了 `libchibicc.a` 或 `-lchibicc`. 如果用的是 stage2 或 pratt 库, 确认路径.

**`pkg-config` 找不到 `libchibicc`.**

设置 `PKG_CONFIG_PATH` 指向 `$PREFIX/lib/pkgconfig`.

**运行时报 `libchibicc.so.0` 找不到.**

把 `$PREFIX/lib` 加到 `LD_LIBRARY_PATH`, 或运行 `ldconfig`.

**`#include <stdio.h>` 找不到.**

库不安装系统 include 路径. 消费者要自己 `chibicc_context_add_include_path(ctx, "/usr/include")`, 以及目标平台需要的其他路径. 参考驱动器的 `add_default_include_paths`.

**`make check-library` 在 macOS 上通过吗.**

通过. `test/install.sh` 在非 Linux 上跳过 `.so` 检查. `test/library.sh` 用宿主 `cc` 编译和运行, 不需要 x86-64 目标代码.

**如何只构建库, 不构建编译器.**

`make libchibicc.a`. 这不会编译 `main.c` / `codegen.c`.
