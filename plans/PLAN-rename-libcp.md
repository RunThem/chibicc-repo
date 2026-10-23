# PLAN-rename-libcp.md - 库改名 libcp

> 状态: 执行中 (2026-10-23); 执行记录见 `plans/RESULT-rename-libcp.md`
> 基线: `main` @ `e0e6884`
> 分支: `main` (单条机械改名, 不分阶段)
> 范围: 库的包名 / 路径 / 公共符号 / 打包身份; 二进制名与上游血统名不变
> 回退: 改名是一个提交, `git revert` 即完全恢复
> 目标: `libchibicc` / `<chibicc/...>` / `chibicc_` / `Chibicc` /
>        `CHIBICC_` 改为 `libcp` / `<libcp/...>` / `libcp_` / `Libcp` /
>        `LIBCP_`; 二进制仍是 `chibicc`

## 1. 目标与边界

### 1.1 目标

把库的商品名与命名空间从上游的 `chibicc` 前缀换成 `libcp`, 使它作为可嵌入的
C 前端库有自己的身份, 同时保留参考消费者的名字与上游血统:

- 包名 / repo 身份 / 产物: `libcp`, `libcp.a`, `libcp.so.0`,
  `libcp.pc`, `-lcp`.
- 公共头根: `libcp/include/libcp/`, umbrella `<libcp/libcp.h>`.
- 公共与内部符号: `libcp_*` 函数, `Libcp*` 类型, `LIBCP_*` 宏.
- 安装布局: `${PREFIX}/include/libcp/*.h`, `${PREFIX}/lib/libcp.a`,
  `${PREFIX}/lib/pkgconfig/libcp.pc`.
- 二进制仍是 `chibicc` (参考消费者) 与 `chibicc-pratt`; README 说明分工,
  类似 `libclang` + `clang`.
- 行为零变化: `-E`, 诊断文案与锚点, 汇编, 以及 `-dump-*` / `-verify-*` 的
  输出逐字节不变.

### 1.2 边界

本线允许改:

- 目录与文件名: `libchibicc/` -> `libcp/`, `libchibicc/include/chibicc/` ->
  `libcp/include/libcp/`, `libchibicc/src/chibicc.h` -> `libcp/src/libcp.h`,
  `libchibicc.pc.in` -> `libcp.pc.in`.
- 全部公共头, 内部 umbrella, 库源码, `main.c` / `codegen.c` / `driver.h` 的
  标识符与 include 路径.
- `Makefile`, `.gitignore`, `test/*.sh`, `test/self/self-test.c`.
- `README.md`, `AGENTS.md`, `docs/**`.
- 新增 `plans/PLAN-rename-libcp.md` 与 `plans/RESULT-rename-libcp.md`.

### 1.3 非目标

- 不改二进制名 (`chibicc`, `chibicc-pratt`) 与任何 `./chibicc` 调用.
- 不改预定义宏 `__chibicc__`: 它标识编译器实现, 不是库的身份; 改了会改变
  `-E` 与所有消费者的可见行为.
- 不改 GitHub repo 名与 remote (`chibicc-repo`); 那是外部操作, 由用户另行决定.
- 不改 `plans/**` 与 `CodeReview-Result.md` 里的历史记录: 它们记录的是当时
  的代码状态, 改写历史会让归档失真.
- 不改任何语义, 不改 codegen 的发射形状, 不做顺手的重命名或优化.
- 不引入兼容层 (旧名 alias / 旧头转发): 没有外部消费者, 不做双名共存.

## 2. 判据 (完成定义)

**判据 1 - 活代码面零旧名**
- 在 `libcp/`, `test/`, `main.c`, `codegen.c`, `driver.h`, `Makefile`,
  `.gitignore`, `README.md`, `AGENTS.md`, `docs/` 上运行
  `git grep -i chibicc`, 结果只能落在 4.4 的保留清单里.

**判据 2 - 构建与产物**
- `make` 产出 `chibicc` 与 `libcp.a`; Linux 下还有 `libcp.so.0` 与
  `libcp.so` 符号链接.
- `make install PREFIX=...` 安装 10 个公共头到 `include/libcp/`,
  `libcp.a` 到 `lib/`, `libcp.pc` 到 `lib/pkgconfig/`.

**判据 3 - 公共面闸门**
- `make check-context` 与 `make check-library` 全绿; 后者的
  `test/public-api.sh` 里 D7 的 36 个名字是 `libcp_*`, `.so` 只导出这一集.

**判据 4 - 行为逐字节不变**
- `make docker-snapshot-diff` 输出 `snapshot diff: empty`
  (基线 `.cache/snapshot`, 41 个文件).
- `make docker-test` 全绿 (含自举, stage2 用改名后的源码重编).

## 3. 现状锚点 (2026-10-23 实测, `main` @ `e0e6884`)

### 3.1 规模

- 受跟踪文件 144 个, 含名字的 87 个, 3043 行.
- 需要改名的路径 23 个: `libchibicc/src/` 12 个, `libchibicc/include/chibicc/`
  10 个, `libchibicc.pc.in` 1 个.
- 标识符: 74 个 `chibicc_*` 函数, 29 个 `Chibicc*` 类型, 28 个 `CHIBICC_*`
  宏; 公共面 D7 是 36 个函数.
- 分布: `libcp` 涉及的活面 = `libchibicc/src` 823 + `libchibicc/include` 385 +
  `test` 648 + `Makefile` 187 + `README.md` 59 + `AGENTS.md` 215 +
  `docs` 792 + `main.c` 27 + `codegen.c` 40 + `driver.h` 7; 归档 `plans/` 753.

### 3.2 必须区分对待的形态

- 裸 `chibicc` 487 处: 绝大多数是二进制名 (`./chibicc`, `$chibicc`,
  `driver.sh` 的 `grep -q chibicc`) 与上游项目名; 不能全局替换.
- `__chibicc__` 7 处: 预定义宏, 含 `libchibicc/include/chibicc/context.h` 的
  `#if defined(__chibicc__)` 守卫; 不能被 `chibicc_` 规则误伤.
- `-lchibicc` 9 处: 不含 `libchibicc` 子串, 必须单独替换成 `-lcp`.
- 临时目录前缀 (`chibicc-diag-XXXXXX` 等): 属于测试脚本的 scratch 名字,
  保留.
- `CodeReview-Result.md` 与 `plans/**`: 归档, 保留旧名.

### 3.3 闸门基线

- `.cache/snapshot` 存在, 41 个 `.s` 文件, 是 phase 0 建立的汇编基线.
- `main` 与 `origin/main` 齐平, 无未推送提交.

## 4. 名字与替换规则

### 4.1 名字

- 库名 `libcp`, 展开 `Library C Parse`. 它是品牌 token 而不是描述: 描述由
  `README.md` 首段与 `libcp.pc` 的 `Description` 承担
  (`Reusable C front end (tokenize, preprocess, CST, parse, sema)`).
- 命名空间 `libcp_` (函数) / `Libcp` (类型) / `LIBCP_` (宏). `lib` 进符号是
  有意为之, 与 `libusb_*` / `libssh2_*` 同风格.
- 二进制 `chibicc` 保留: 它仍是上游 chibicc 的驱动器与 x86-64 后端, 库是抽出来
  的前端. README 首段说明这个分工.
- 文档里不出现 "Clang" 作为本库的展开或类比.

### 4.2 替换表与顺序

顺序不可交换; 脚本按此顺序对每个活文件做替换.

| # | 旧 | 新 | 说明 |
|---|---|---|---|
| 1 | `libchibicc` | `libcp` | 必须先做, 否则 8 会产生 `liblibcp` |
| 2 | `CHIBICC_` | `LIBCP_` | 宏, 含头文件守卫与 allocator 配置 |
| 3 | `Chibicc` | `Libcp` | 类型 |
| 4 | `chibicc_` | `libcp_` | 函数 |
| 5 | `chibicc/chibicc.h` | `libcp/libcp.h` | umbrella include (含路径形态) |
| 6 | `<chibicc/` | `<libcp/` | 公共 include |
| 7 | `include/chibicc` | `include/libcp` | 安装布局与路径 |
| 8 | `chibicc.h` | `libcp.h` | 内部 umbrella 与零散头名 |
| 9 | `-lchibicc` | `-lcp` | 链接参数 |
| 10 | 手工 | | 头文件列表里的 `chibicc` 项, 与目录改名 |

### 4.3 先保护再还原

替换前把下列形态换成占位符, 全部规则跑完后还原:

- `__chibicc__` (否则规则 4 会命中其中的 `chibicc_`, 变成 `__libcp__`).
- `rui314/chibicc` 及其它指向上游的 URL 路径 (否则规则 7 之外的路径规则会
  把上游链接改成不存在的地址).

### 4.4 保留清单

| 形态 | 原因 |
|---|---|
| `chibicc`, `chibicc-pratt`, `./chibicc`, `$chibicc` | 二进制名, 本线不改 |
| `ghcr`/`chibicc:amd64` Docker 镜像名 | 构建环境名, 沿用 |
| `chibicc-diag-XXXXXX` 等 mktemp 前缀 | 测试脚本 scratch 名, 无对外含义 |
| `__chibicc__` | 编译器实现标识, 改了就是行为变化 |
| `chibicc-specific` (main.c 注释) | 描述驱动器行为, 与库名无关 |
| `Dockerfile` 注释里的 chibicc | 描述被构建的编译器, 不是库 |
| 上游 README 正文与 `rui314/chibicc` 链接 | 血统与署名 |
| `plans/**`, `CodeReview-Result.md` | 历史归档 |

### 4.5 文档同步

- `README.md`: 标题保留上游正文, 在顶部加一行本 fork 的身份说明; 库小节
  `## libchibicc: the C front end as a library` 改为 `## libcp`, 示例、
  构建命令、pkg-config 名、头路径、`CHIBICC_VERSION` 全部更新.
- `AGENTS.md`: 全局约定里的库名与命名前缀, 路线图与现状里的路径, 构建与测试
  命令, 代码地图; 新增一条 2026-10-23 的"已定"记录, 指向本计划与结果.
- `docs/**`: 机械替换加逐处 prose 校对 (`libchibicc 文档` -> `libcp 文档`,
  `.pc` 与 install 路径, 36 个 D7 名字).

## 5. 步骤

### R0 - 基线与准备

- [ ] R0.1 记录 `git rev-parse HEAD`, 确认工作区干净 (`main` 与
  `origin/main` 齐平), `.cache/snapshot` 基线存在.
- [ ] R0.2 写本计划并单独提交.

### R1 - 路径改名 (git mv)

- [ ] R1.1 `libchibicc/` -> `libcp/`.
- [ ] R1.2 `libcp/include/chibicc/` -> `libcp/include/libcp/`.
- [ ] R1.3 `libcp/include/libcp/chibicc.h` -> `libcp/include/libcp/libcp.h`.
- [ ] R1.4 `libcp/src/chibicc.h` -> `libcp/src/libcp.h`.
- [ ] R1.5 `libchibicc.pc.in` -> `libcp.pc.in`.

### R2 - 标识符替换

- [ ] R2.1 用 4.2 的顺序与 4.3 的保护, 对活文件跑替换脚本; 归档目录排除.
- [ ] R2.2 逐条核对残留: `git grep -i chibicc` 的结果全部能归入 4.4.

### R3 - 手工修正

- [ ] R3.1 `Makefile`: `LIB_HDR` 指向 `libcp.h`; `install` 的
  `include/libcp` 与 `libcp.pc.tmp`; `clean` 与 `BOOT` 的排除项; SONAME.
- [ ] R3.2 `test/install.sh` 与 `test/public-api.sh` 的头文件列表:
  umbrella 项从 `chibicc` 改为 `libcp`, `libcp/include/libcp` 的复制路径.
- [ ] R3.3 `test/allocator.sh` 的分配器宏与重编路径; `test/library.sh` 的
  `-lcp` 与 ASan 重编命令; `test/self-test.sh` 与 `test/self/self-test.c`.
- [ ] R3.4 `test/context-audit.sh` 与 `test/public-api.sh` 的
  **禁名模式**改为 `libcp_ctx` / `libcp_active_ctx` /
  `libcp_context_activate` / `libcp_arena_*`; 改名后要确认这些模式仍然
  能命中目标 (负向用例: 手写一个 `libcp_ctx` 声明应被报告).
- [ ] R3.5 `.gitignore` 的产物名.

### R4 - 文档

- [ ] R4.1 `README.md` 的库小节与顶部身份说明.
- [ ] R4.2 `AGENTS.md` 的命名约定、路线图、构建与测试、代码地图, 加决策记录.
- [ ] R4.3 `docs/**` 的机械替换与 prose 校对.

### R5 - 闸门

- [ ] R5.1 `make` (macOS 只保证 `.a`), `make check-context`,
  `make check-library`.
- [ ] R5.2 `make docker-test` (含自举).
- [ ] R5.3 `make docker-snapshot-diff` 为空.
- [ ] R5.4 Pratt 面: `make test-ab` 或 `make docker-test-both` 的默认集合
  (改名同时触及两条表达式路径的宏 `CHIBICC_PRATT_PARSE_FEATURE`).

### R6 - 收尾

- [ ] R6.1 写 `plans/RESULT-rename-libcp.md`, 勾完本计划的复选框.
- [ ] R6.2 提交并推送.

## 6. 闸门

| 闸门 | 命令 | 通过条件 |
|---|---|---|
| 构建 | `make` | `chibicc` 与 `libcp.a` 生成 |
| 上下文审计 | `make check-context` | 0 命中, 且审计模式仍有效 |
| 公共面 | `make check-library` | 头自包含, D7 齐, install 布局正确 |
| 行为 | `make docker-test` | 全绿, 含自举 |
| 汇编等价 | `make docker-snapshot-diff` | `snapshot diff: empty` |
| Pratt A/B | `make docker-test-both` 或 `make test-ab` | 两条路径输出相同 |

## 7. 回退

改名是一个提交 (计划与结果各一个). `git revert <rename-commit>` 恢复旧命名;
由于没有外部消费者与 ABI 承诺, 不存在兼容性残留.
