# PLAN-pratt-default.md - Pratt 单一路径与内部符号前缀

> 状态: 已完成 (2026-10-25); 执行记录见 `plans/RESULT-pratt-default.md`
> 基线: `main` @ `e8d6f3d`
> 分支: `main` (阶段 1 / 阶段 2 两个执行提交)
> 范围: 删除旧级联表达式解析器, Pratt 成为唯一路径; 归档内全部外部内部符号
>        统一 `libcp_` 前缀, 21 个门面薄包装合并
> 回退: 每阶段一个提交, `git revert` 即完全恢复
> 拍板: 用户 2026-10-25 (方案与撞名处理经 AskUserQuestion 确认)

## 1. 目标与边界

### 1.1 背景

Pratt 线 (`plans/PLAN-pratt-parse.md`) 落地后表达式层长期双路径并存: 默认
`libcp.a` 仍是旧级联, `make chibicc-pratt` out-of-tree 构建新路径, 两套测试
与 A/B 闸门维持等价. 库化线 L3.2b 当时拍板 `.a` 内部符号不做源码级重命名
("文档声明 + 只保证 `.so` 隐藏"), 源码级重命名作为"可选机械步"押后. 本线把
两件事一起收掉.

### 1.2 用户拍板 (2026-10-25)

1. **Pratt 唯一化**: 直接迁移到 Pratt, 默认(旧级联)解析器删除, 不保留
   双构建. `chibicc-pratt` 二进制与全部 pratt 构建目标随之消失 (对
   2026-10-23 "二进制 chibicc-pratt 保留原名" 记录的修订).
2. **内部符号加前缀**: `libcp.a` 的 106 个无前缀外部符号 (93 函数 + 13 个
   `ty_*` 变量) 统一加 `libcp_` 前缀; 21 个与公共 API 撞名的薄包装对采用
   **合并**方案 - 删掉 `lib.c` 的一行转发包装, 实现原地改用公共名.
3. **"AST 改写器盲区"不设独立代码阶段**: 经讲解确认它是 2026-10-23 显式
   ctx 线一次性改写工具的视野盲区 (Pratt 分支不可见 / `LIBCP_TEST_BUILTINS`
   不可见 / 跨模式重名函数), 不是库代码缺陷; 随本线阶段 1 结构性溶解
   (双模式消失 => 盲区 1/3 消失; 盲区 2 只剩 `multi_macro` 测试钩子, 由
   `test/library.sh` 的 ASan 轮继续覆盖).

### 1.3 边界

本线允许改:

- `libcp/src/parse.c` (删旧级联与守卫), `libcp/src/libcp.h` (删宏, 改内部
  声明), 其余 `libcp/src/*.c` (符号改名与 21 个实现改用公共名).
- `Makefile`, `.gitignore`, `.github/workflows/ci.yml` (注释).
- `test/*.sh` (删 pratt 逻辑, 更新 public-api.sh 黑名单),
  `test/self/self-test.c`, 删除 `test/pratt-ab.sh`.
- `codegen.c`, `main.c` (内部调用点改名).
- `AGENTS.md`, `docs/**` 的现状描述; `plans/12` 类历史小节原文保留.

### 1.4 非目标

- 不把可 static 化的符号改 static (超出"加前缀"授权).
- 不改公共 API 形状: D7 的 36 个函数签名与语义不变 (21 个合并对仅定义位置
  从 `lib.c` 移到实现文件).
- 不做性能优化, 不改 codegen 发射形状, 行为零变化 (汇编快照空 diff).
- 不回溯改写 `plans/**` 归档.
- 不处理 L5 (产品化验证线), 保持独立.

## 2. 判据 (完成定义)

**判据 1 - 单一路径**
- `git grep -n LIBCP_PRATT_PARSE_FEATURE` 在 `libcp/`, `test/`, `Makefile`,
  `main.c`, `codegen.c`, `docs/`, `AGENTS.md`, `.github/` 上零命中 (仅
  `plans/**` 归档保留).
- `make` 产出 `chibicc` 与 `libcp.a`; `make test` 全绿; 不存在
  `chibicc-pratt` / `pratt/` / `stage2-pratt/` 相关目标与条目.

**判据 2 - 行为逐字节不变**
- `make docker-test` 全绿 (自举 + stage2 + 全部通道).
- 汇编快照 diff 为空 (`snapshot diff: empty`) - 这同时是"Pratt 输出 ==
  旧级联输出"的权威等价证据 (pratt-ab.sh 已证 41 语料 + tinycc 逐字节等价,
  删除后由快照基线继续锁).

**判据 3 - 归档符号面**
- `nm -g libcp.a` (剥 Darwin 前导下划线): 外部符号 100% 以 `libcp_` 开头,
  零裸名残留; 数量 = 36 个 D7 + 内部符号 (实测记入 RESULT).
- 容器内 `make check-library` 全绿: `.so` 导出恰好等于 D7 (合并方案的关键
  闸门), D7 在 `.a` 全部存在, install / pkg-config 布局不变.

**判据 4 - 审计与测试面**
- `make check-context` 0 命中; `test/public-api.sh` 的消费者黑名单更新为
  新前缀名且负向控制有效.

## 3. 现状锚点 (2026-10-25 实测, `main` @ `e8d6f3d`)

### 3.1 Pratt 双路径落点

- 宏定义: `libcp/src/libcp.h:55-61` (`#define LIBCP_PRATT_PARSE_FEATURE 0`).
- `libcp/src/parse.c` (2654 行): 5 个前置声明守卫块 (74-110) + 3 段旧级联
  定义体 (1245-1566, 1568-1668, 1917-2002, 共 509 行 21 个函数) + 1 段
  Pratt 路径 (2217-2511, 295 行). 共享代码 (expr_stmt / struct_ref /
  funcall / generic_selection / primary) 在守卫外.
- `conditional` 是非 static 导出符号, 两个分支各一份定义 (1354 / 2508),
  声明在 `libcp.h:182`, 被 `sema.c` 的 `const_expr` 调用 - 删旧路径后
  Pratt 版保留.
- Makefile: `PRATT_*` 变量, `pratt/%.o` 模式规则, `chibicc-pratt`,
  `test-pratt`, `test-both`, `test-ab`, `check-library-pratt`,
  `check-context-pratt`, `stage2-pratt` 全段, 4 个 docker 目标, clean /
  BOOT / .PHONY 条目. `make test` / `test-all` / `check-library` / CI 均
  不依赖它们.
- 测试: `test/pratt-ab.sh` (205 行, 整文件删除), `test/library.sh` 与
  `test/self-test.sh` 的 pratt 环境变量逻辑, ast.sh / context-audit.sh /
  public-api.sh / self-test.c 的双构建注释.
- 文档: `docs/06` (§6.4.2 整节, §6.8 条目), `docs/01:182`, `docs/08:222`,
  `docs/10` (5 处), `docs/12` (§12.11 历史 + §12.14 现状句); `plans/**`
  归档不动.

### 3.2 内部符号面

- 公共 D7: 36 个函数 (context 11, token 4, preprocess 1, cst 4, ast 1,
  sema 1, provenance 13, version 1), 与 `test/public-api.sh:120-155` 一致.
- 无前缀外部符号 106 个: cst.c 4, hashmap.c 6, parse.c 8, preprocess.c 16,
  provenance.c 2, sema.c 16, strings.c 6, tokenize.c 17, type.c 13 函数 +
  13 个 `ty_*` 变量, unicode.c 5.
- 已带 `libcp_` 前缀的内部符号 6 个 (`libcp_arena_*` 5 +
  `libcp_context_set_base_file`), 不在公共头.
- 机械加前缀撞名 D7 的 21 对 (全部是 `lib.c` 薄包装转发同名内部实现的
  模式): preprocess, parse, sema, tokenize_file, cst_parse /
  cst_reconstruct / cst_dump / cst_verify, token_provenance,
  macro_prov_next / macro_prov_expansion / macro_prov_arg_index,
  macro_expansion_name / _end / _macro / _def / _arg / _arg_count /
  _is_builtin, expansion_dump / expansion_verify.
- 库外消费者: `codegen.c` 调用 23 个内部函数 + `ty_long` / `ty_ulong`;
  `main.c` 调用 11 个 + `libcp_context_set_base_file`; `test/self/self-test.c`
  用 4 个裸名 + 2 个 `libcp_arena_*`.
- `test/public-api.sh:103` 的消费者黑名单是裸名, 必须同步更新.

### 3.3 改写器盲区 (第三点) 现状

记录在 `plans/RESULT-explicit-context.md:97-101,119-120,243-247`. 当时已修
(双模式数据库合并, `multi_macro` 手工补齐, 按 (file,name) 解析重名), 现由
经验性闸门锁住 (test-pratt 全套含 ASan, ast.sh 双构建逐字节, pratt-ab.sh
跨构建等价). 本线阶段 1 后: 盲区 1/3 随双模式消失而结构性消失; 盲区 2
(`LIBCP_TEST_BUILTINS` 的 `multi_macro`, 仅 ASan 轮编译) 保留现状, 由
`test/library.sh` ASan 轮覆盖, context-audit 的文本 grep 对 `#ifdef` 块同样
生效.

## 4. 步骤

### S0 - 计划落档

- [x] S0.1 写本计划并单独提交.

### S1 - 删除旧级联, Pratt 唯一化

- [x] S1.1 `parse.c`: 删 3 段旧级联定义体与 5 个前置声明守卫块; 去掉 Pratt
  段的 `#if` / `#endif` 包装, 内容原样保留; 共享代码不动.
- [x] S1.2 `libcp.h`: 删宏定义 (55-61).
- [x] S1.3 `Makefile`: 删全部 pratt 目标 / 规则 / 变量 / clean / BOOT /
  .PHONY / docker 条目.
- [x] S1.4 `git rm test/pratt-ab.sh`; 简化 `test/library.sh` /
  `test/self-test.sh` (归档从四套变两套); 清理 ast.sh / context-audit.sh /
  public-api.sh / self-test.c / main.c 注释里的双构建说法.
- [x] S1.5 `.gitignore` 删 3 个条目; `ci.yml` 头注释改写.
- [x] S1.6 docs 现状描述同步 (06 重写 §6.4.2, 01 / 08 / 10 / 12).
- [x] S1.7 AGENTS.md: 路线图加本线条目, 已决事项补记 chibicc-pratt 移除,
  构建与测试 / 现状 / 设计原则 / 测试约定相应更新.
- [x] S1.8 闸门: 本机 `make` + `make check-context` + ast/trivia/cst/
  expand/diagnostic 前端通道; docker `docker-test` + 容器 `check-library`
  + `docker-test-thirdparty THIRDPARTY=tinycc`; grep 判据 1.
- [x] S1.9 提交 "Make the Pratt parser the only expression path".

### S2 - 内部符号 libcp_ 统一前缀 + 合并 21 个薄包装

- [x] S2.1 前置核实公共头 D7 声明的 LIBCP_API / visibility 形态, 确认定义
  移出 `lib.c` 后 `.so` 导出仍成立; 必要时在合并定义处显式加 LIBCP_API.
- [x] S2.2 85 个无冲突符号 (72 函数 + 13 个 `ty_*` 变量) 词边界受限改名,
  范围 `libcp/src/*.c` + `libcp.h` + `codegen.c` + `main.c` +
  `test/self/self-test.c`; 逐符号 diff 审查, 重点核对 parse / sema / error /
  format / equal / skip / consume / conditional 等常见词的注释误伤.
- [x] S2.3 21 个撞名对逐一确认纯转发后合并: 删 `lib.c` 包装, 实现原地改用
  公共名; 内部 umbrella 头删旧声明; 有实际逻辑的保留包装并记录 (预计 0).
- [x] S2.4 `test/public-api.sh` 黑名单更新为新前缀名, 负向控制有效.
- [x] S2.5 AGENTS.md 已决事项补记 (废止 L3.2b 不重命名策略) + docs 符号
  策略句子同步.
- [x] S2.6 闸门: `nm -g libcp.a` 判据 3; docker `docker-test` (自举改名后
  源码) + 容器 `check-library` (.so exact-D7) + `check-context`.
- [x] S2.7 提交 "Prefix all internal library symbols with libcp_".

### S3 - 记录归档

- [x] S3.1 写 `plans/RESULT-pratt-default.md`: 执行记录, 最终归档符号统计,
  21 个合并对清单, 三盲区最终状态; 勾完本计划复选框.
- [x] S3.2 AGENTS.md 路线图勾选; 提交 "Record the Pratt-only line and mark
  the plan complete".

## 5. 闸门

| 闸门 | 命令 | 通过条件 |
|---|---|---|
| 构建 | `make` | `chibicc` 与 `libcp.a` 生成, 无 pratt 产物 |
| 上下文审计 | `make check-context` | 0 命中 |
| 前端通道(本机) | `test/{ast,trivia,cst,expand,diagnostic}.sh ./chibicc` | 全绿逐字节 |
| 行为 | `make docker-test` | 全绿 (自举 + stage2) |
| 汇编等价 | `make docker-snapshot-diff` | `snapshot diff: empty` |
| 公共面 | 容器 `make check-library` | D7 齐, `.so` 恰好 D7, install 布局 |
| 第三方 | `make docker-test-thirdparty THIRDPARTY=tinycc` | tinycc 全绿 |
| 符号面 | `nm -g libcp.a` | 100% `libcp_` 前缀 |

## 6. 风险与对策

1. **改名误伤注释 / 字符串**: 词边界受限替换 + 逐符号 diff 审查 + 全量
   闸门兜底; 常见词符号逐处人工核对.
2. **合并后 `.so` 导出破坏**: 容器 `check-library` 的 exact-D7 diff; 可见性
   不随公共头声明继承时在定义处显式 `LIBCP_API`.
3. **自举失败**: 阶段 1 是纯删除, 阶段 2 是纯标识符改名, chibicc 编译的是
   同构 C 源码; `docker-test` 内含 stage2 直接验证.
4. **Pratt 行为回归**: pratt-ab.sh 已锁 41 语料 + tinycc 逐字节等价, 本线
   删除的只是未编译分支; 快照 diff 与 tinycc 第三方闸门兜底.
5. **docker 环境**: 先 `open -a OrbStack`; 日志落文件, 不接管道.

## 7. 回退

阶段 1 / 阶段 2 各为一个提交, `git revert` 单独恢复; 无兼容层, 无 ABI
承诺, 不存在混合状态.
