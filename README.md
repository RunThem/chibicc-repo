# libcp: C 前端库

`libcp` 是一个可复用的 C11 前端库. 它把 C 的词法, 预处理, 语法与语义做成四层中间表示, 每层都有明确的输入输出与公共入口, 消费者可以只取自己需要的那一层. 本仓库是 [chibicc](https://github.com/rui314/chibicc) 的 fork: 仓库里的 `chibicc` 二进制是这个库的参考消费者, 不再是项目的重点.

`libcp` 展开为 "Library C Parse". 它是品牌 token, 不是描述 - 上面那句才是描述.

## 提供什么

| 层 | 产物 | 典型用途 |
|---|---|---|
| 第 1 层 | 保留 trivia 的 token 流, 可按原字节重建输入 | 格式化, 源码位置映射 |
| 第 2 层 | lossless CST, 建在预处理之前, 指令与每个分支都保留 | 注释敏感的改写, codemod |
| 第 3 层 | 忠实语法 AST, 不绑名字, 不给类型 | C 转 C, 语法级 lint |
| 第 4 层 | sema 标注树, 名字已绑定, 类型已计算 | 类型相关分析, 静态分析, 新后端 |

预处理是横切层: 它把第 1 层的 token 流展开成第 3 层的输入, 产物 token 带 provenance 链, 可以追到每一次宏展开和实参. 第 2 层不经过预处理, 这样"逐字节重建源文件"才成立. 第 5 层是 x86-64 汇编, 由仓库里的 `codegen.c` 产生, 不属于库.

一个 `LibcpContext` 服务一个翻译单元. 它持有 include 路径, 命令行宏, 诊断回调与全部 per-TU 分配; `libcp_context_reset` 归还全部分配并保留配置. 库层错误经诊断回调报告, 公共入口返回 NULL, 不退出进程. `libcp_reconstruct` 能把 token 流换回原始字节.

## 最小用法

```c
#include <libcp/libcp.h>

LibcpContext *ctx = libcp_context_new();

LibcpToken *tok = libcp_tokenize_buffer(ctx, "<input>", src);
tok = libcp_preprocess(ctx, tok);           /* 预处理 */
LibcpNode *ast = libcp_parse(ctx, tok);     /* 第 3 层 */
LibcpObj *prog = libcp_sema(ctx, ast);      /* 第 4 层 */

libcp_context_free(ctx);
```

CST 走另一条入口: `libcp_cst_parse(ctx, raw)`, 输入是 `libcp_tokenize_*` 的原始输出. 完整的可编译示例与预期输出见 [docs/README.md](docs/README.md) 的"五分钟跑通".

## 构建与安装

```sh
make                     # 构建 chibicc 参考消费者; Linux 下同时构建 libcp.so
make libcp.a             # 只要静态库
make install PREFIX=/usr/local
```

公共头装在 `$(PREFIX)/include/libcp`, 入口是 `<libcp/libcp.h>`. 安装后可用 pkg-config:

```sh
cc -std=c11 demo.c $(pkg-config --cflags --libs libcp) -o demo
```

库没有外部依赖, 除 C 标准库外只要求 POSIX 的文件与线程基础能力. 分配器是编译期开关, 见 `<libcp/allocator.h>`.

## 测试

先确认宿主再选路径 (`uname -s`, `uname -m`):

| 宿主 | 全量测试 | 汇编等价性 | benchmark |
|---|---|---|---|
| Linux + x86-64 | `make test-all` | `make asm-diff BASE=<ref>` | `make bench` |
| 其它宿主 | `make docker-test` | `make docker-snapshot-diff` | 不适用 |

`make test` 需要能汇编并运行 x86-64 目标; 其它宿主可以只用前端通道 (`-E`, `-dump-tokens`, `-dump-cst`, `-dump-ast`). 第三方项目的慢速验证用 `make test-thirdparty`, 仅 Linux, 不进入 `make test`. 细节见 [docs/10-build-install.md](docs/10-build-install.md).

## 仓库里还有什么

| 路径 | 内容 |
|---|---|
| `main.c`, `codegen.c` | 参考消费者的驱动器与 x86-64 后端, 不属于库 |
| `tools/fmt.c` | 保留行与注释的 CST formatter, 由 `make fmt-tool` 构建 |
| `tools/c2c.c` | C 转 C 打印器, 只作 `make test` 的回归闸门, 不安装 |
| `bench/` | 前端分阶段 benchmark, 仅 Linux |
| `test/` | 特性测试与逐字节行为锁 |
| `thirdparty/` | 真实项目的慢速构建验证 |

`chibicc` 也能当编译器用: `./chibicc -o hello hello.c`. 它的 `-E`, `-dump-cst`, `-reconstruct`, `-dump-ast` 等前端通道可以直接观察每一层.

## 前身

本仓库是 [chibicc](https://github.com/rui314/chibicc) 的 fork. 上游是 Rui Ueyama 从 2019 年开始写的教学编译器: 目标是让读者按 commit 学习一个 C 编译器怎么从"只接受一个整数"长到 C11, 每个 commit 对应一本讲 C 编译器的书的一节. 本 fork 保留上游的代码风格与大部分实现, 把目标改成库: 前端四层可复用, 跨层状态收进 context, 错误改走回调, 仓库里的编译器退化为参考消费者.

上游的 C11 特性覆盖基本保留: 浮点与 long double, 位域, VLA, 复合字面量, 线程局部变量, 原子变量, 指定初始化器, 结构体按值传参与返回. 上游不支持的特性这里同样不支持: 复数, K&R 风格函数原型, GCC 风格内联汇编; digraph 与 trigraph 有意不支持.

## 文档

入口是 [docs/README.md](docs/README.md). 按用途:

- 格式化与源码级改写: [03 token 层](docs/03-layer1-token.md), [05 CST 层](docs/05-layer2-cst.md).
- C 转 C 与语法级工具: [06 语法层](docs/06-layer3-parse.md).
- 类型相关分析与新后端: [07 语义层](docs/07-layer4-sema.md), [09 API 参考](docs/09-api-reference.md).
- 宏展开追踪: [04 预处理与 provenance](docs/04-preprocess.md).
- 当前缺陷与边界: [11 缺陷与边界](docs/11-limitations.md).

## 状态与边界

- 只承诺源码兼容, 不承诺 ABI: 公共结构体字段直接暴露, 换字段就是换版本.
- 版本号见 `LIBCP_VERSION` 与 `libcp_version()`, 兼容性约定与文档基线见 [docs/README.md](docs/README.md).
- 单翻译单元: 不读目标文件, 不做跨 TU 分析.
- 首错返回: 一个入口报出第一个错误就返回, 没有"带错误继续"的部分树.
- 一个 context 不能跨线程并发; 不同 context 可以在不同线程并存.
- 不做优化, 不做增量解析.

## 参与

问题, 计划与执行记录都在 GitHub Issues (`RunThem/chibicc-repo`). 开发约定, 分支流程与闸门见 [AGENTS.md](AGENTS.md); 构建与测试的完整说明见 [docs/10-build-install.md](docs/10-build-install.md).

## 许可

MIT, 见 [LICENSE](LICENSE).
