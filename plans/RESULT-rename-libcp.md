# RESULT-rename-libcp.md - 库改名 libcp 执行记录

> 状态: 已完成 (2026-10-23)
> 基线: `main` @ `e0e6884`
> 改名提交: `3a46b6c`
> 计划: `plans/PLAN-rename-libcp.md`

## 1. 结果

库的商品名与命名空间从 `libchibicc` 换成 `libcp`, 行为零变化:

| 项 | 旧 | 新 |
|---|---|---|
| 包 / 产物 | `libchibicc.a`, `libchibicc.so.0`, `-lchibicc` | `libcp.a`, `libcp.so.0`, `-lcp` |
| pkg-config | `libchibicc.pc` | `libcp.pc` |
| 头路径 | `<chibicc/chibicc.h>` | `<libcp/libcp.h>` |
| 安装 | `$PREFIX/include/chibicc/` | `$PREFIX/include/libcp/` |
| 公共函数 | `chibicc_*` (D7 36 个) | `libcp_*` |
| 公共类型 | `Chibicc*` | `Libcp*` |
| 宏 | `CHIBICC_*` | `LIBCP_*` |
| 内部 umbrella | `libchibicc/src/chibicc.h` | `libcp/src/libcp.h` |

规模: 52 个文件改动 (其中 23 个路径由 `git mv` 改名), 2815 处标识符替换, 4 处手改
(两处 allocator 注释里的头路径, 两个公共头列表), 1 处 ASCII 框图对齐.

## 2. 保留下来的旧名

| 形态 | 理由 |
|---|---|
| `chibicc` / `chibicc-pratt` 二进制与 `./chibicc` 调用 | 参考消费者, 仍是上游驱动器 + x86-64 后端 |
| `chibicc:amd64` 镜像名, `Dockerfile` 注释 | 构建环境名 |
| `chibicc-XXXXXX` 临时目录前缀 | 测试脚本 scratch 名 |
| `__chibicc__` | 预定义宏, 标识编译器实现; 改了会改变 `-E` 与消费者可见行为 |
| `rui314/chibicc` 链接与上游 README 正文 | 血统与署名 |
| `plans/**`, `CodeReview-Result.md` | 历史归档, 不回溯改写 |

## 3. 闸门

| 闸门 | 命令 | 结果 |
|---|---|---|
| 构建 | `make -j8` | `chibicc` + `libcp.a`; 只有 5 条既有的 `-Wformat` 警告 |
| 上下文审计 | `make check-context` | 7 项全 0 |
| 公共面 (macOS) | `make check-library` | library / allocator / public-api / install 全 ok; pkg-config 未安装, 该项跳过 |
| 公共面 (Linux) | 容器内 `make check-library` | `install: pkg-config: ... -lcp | 0.1.0`, `install: ok`, `libcp.so exports exactly D7` |
| 行为 | `make docker-test` | 全绿, 含 stage2 自举与 self-test (arena / hashmap / sema-state) |
| 汇编等价 | `make docker-snapshot-diff` | `snapshot diff: empty` (41 个文件) |
| Pratt 双路径 | `make docker-test-both` | 全绿 (test, test-pratt, stage2, stage2-pratt) |

审计的负向对照, 确认禁名模式没有被改名架空:

- 在 `libcp/src/` 放一个 `int libcp_ctx(void);` 后 `make check-context` 退出 2, 并报
  `accessor references in libcp/ (want 0)`; 删除后回到 0.
- 把带 `libcp_ctx` 符号的对象 `ar` 进 `libcp.a` 的副本,
  `LIBCP_LIB=... test/context-audit.sh --strict` 退出 1, 并报
  `accessor symbols in ... (want 0)`.

## 4. 与计划的偏离

- 计划 R5.1 只写了本机 `make check-library`; 本机没有 pkg-config, `test/install.sh` 会跳过
  `.pc` 与 pkg-config 检查, 因此补跑了容器内的同一目标 (见 3). 这是对覆盖面的补充, 不是
  设计变化.
- 没有其它偏离.

## 5. 文档

- `README.md`: 顶部加 fork 身份说明 (`libcp` 是库, `chibicc` 是参考消费者, `libcp` 读作
  "Library C Parse" 且是品牌 token, 不是描述); 库小节, 示例, 构建与 pkg-config 命令更新.
- `AGENTS.md`: 命名约定, 路线图, 构建与测试, 代码地图更新; 新增 2026-10-23 的已定记录
  (含 `__chibicc__` 与归档不回溯的理由); 现状段加"库改名已完成".
- `docs/`: 13 个文件全部更新; `docs/README.md` 记录改名与新的文档基线; `docs/12` 的时间线
  与收尾整理段记录改名提交; 修正一处 ASCII 框图对齐.

## 6. 后续

- GitHub repo 名仍是 `chibicc-repo`. 改名属于外部操作 (remote URL 与用户侧 clone 都会变),
  留给用户决定; 代码与文档不依赖 repo 名.
- L5 产品化验证仍未开工; 它的消费者代码从 `<libcp/libcp.h>` 开始写.
