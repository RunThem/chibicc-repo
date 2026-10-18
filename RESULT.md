# RESULT.md - 库边界固化执行记录

本文件记录 `PLAN.md`(路线图阶段 5, 库边界固化)各步骤的实际执行结果, 供审核与后续会话
接续参考. 前置线与已归档线见 `PLAN.md` 开头的列表; provenance 线的记录在
`RESULT-provenance.md`, 其终态 `8c1c1d1` 是本线的量化对比基线.

## 闸门口径(本线生效)

- 每步: 构建零新告警; 本机只读通道不回归; 行为不变步要求汇编快照为空 diff.
- 阶段边界: `make docker-test` + `make docker-snapshot-diff` 为空.
- 一步一提交; 每步汇报固定四项(改了什么 / 为什么 / 闸门结果 / 偏差).
- 红线: 不新增 `exit()`(驱动器与默认处理器除外); 不新增文件域语义 static; 不改变
  诊断文案与插入符锚点; 不改变 `-E` / trivia / CST 的逐字节输出; 新代码保持 C11 可自举.

## 基线记录(L0.1)

- 开工 HEAD: `8c1c1d1` (Rename expansion_dump.c to provenance.c).
- 工作树: 只有未跟踪的 `.zcode/` 与 `.zcodeignore`; 源码无改动.
- 顶层源码规模: 15,080 行(`wc -l *.c *.h`, 不含 `test/`).
- 库层 static: 19 个(`tokenize.c` 4 / `preprocess.c` 4 / `parse.c` 1 / `sema.c` 10).
- `error_*` 调用点约 145 处; 库层分配点约 60 处.
- `make docker-test`: EXIT=0 (stage1 + stage2 + 自举; 日志 `.cache/L0.1-docker-test.log`, 5,028 行).
- `make docker-snapshot-diff`: `snapshot diff: empty` (日志 `.cache/L0.1-snapshot.log`, 41 个
  `test/*.c` 的汇编快照).
- 既有通道基线: 诊断 55 例 / 形状 33 段(各跑 stage1 与 stage2), trivia / CST / expand
  全绿(同一次 docker-test).


## 设计账本(开工前, 实施时逐项核对)

### 输入事实

- `chibicc.h` 987 行, 89 个非 static 函数; 真正的公共 API 约 30 个.
- 公共 `Node` 的 codegen 字段与位置见 `PLAN.md` 的"现状锚点".
- `Relocation.label` 是 `char **`, 是 `unique_label` 能搬走的唯一硬耦合.
- `Obj.offset` / `stack_size` 是 codegen 的帧布局结论; `va_area` / `alloca_bottom` 是
  sema 物化的 ABI 对象.
- `include_paths` 是 `main.c` 的全局, `preprocess.c` 直接读.

### 关键判断

1. 公共 `Node` 用"源码/语义留, 发射排布走"作为唯一判据; 不引入 `void *backend`.
2. context 用显式公共签名 + 内部 TLS active 访问器; 这是本线唯一接受的隐式状态,
   理由是库层递归函数数量与改造风险.
3. arena 一个 context 一条块链; reset 保留配置, 释放全部 per-TU 内存; 不做 per-phase.
4. 诊断走"回调 + longjmp 到公共入口", 保持 145 个内部 `noreturn` 调用点不动.
5. 公共头与实现分离, 但实现侧用 typedef 别名保持短名; 只有公共面加前缀.
6. 库产物 `libchibicc.a`(所有平台) + `libchibicc.so`(Linux), 二进制链接静态库.

### 不变量(每一步都测)

- `-E` / 诊断 / trivia / CST / provenance 的输出逐字节不变(L2.6 默认处理器路径除外,
  它必须复现旧输出).
- 汇编快照为空 diff(L1 全部与 L2 行为不变步).
- 库层不新增 `exit()`; 不新增文件域语义 static.
- 公共头不包含内部头; 内部头不进入安装面.

## L0.1 归档 / 计划 / 基线

- 动作: `PLAN.md` -> `PLAN-provenance.md`; `RESULT.md` -> `RESULT-provenance.md`;
  新建本文件与 `PLAN.md`; AGENTS.md 更新路线图 / 开放决策 / 构建与测试 / 代码地图.
- 基线: HEAD `8c1c1d1`; `make docker-test` EXIT=0; `make docker-snapshot-diff`
  `snapshot diff: empty`; 工作树只有 `.zcode/` 与 `.zcodeignore`.
- 偏差: 无.

## L1.1 codegen per-node 表

(待执行)

## L1.2 标签值链路

(待执行)

## L1.3 goto_next 出公共面

(待执行)

## L1.4 Obj 帧字段出公共面

(待执行)

## L1.5 公共 Node / Obj 收口

(待执行)

## L2.1 context 骨架与 facade

(待执行)

## L2.2 tokenize 状态与文件表

(待执行)

## L2.3 preprocess 状态

(待执行)

## L2.4 parse 作用域

(待执行)

## L2.5 sema 状态

(待执行)

## L2.6 诊断回调与 longjmp

(待执行)

## L2.7 分配点收口

(待执行)

## L2.8 状态收口验证

(待执行)

## L3.1 公共头拆分

(待执行)

## L3.2 公共函数前缀与 facade 收口

(待执行)

## L3.3 Makefile 库目标

(待执行)

## L3.4 库测试定稿

(待执行)

## L4 终态验收

(待执行)

## 提交一览

| 提交 | 步骤 | 内容 |
|---|---|---|
| (待填) | L0.1 | 归档 provenance 线, 建立库化线 |
