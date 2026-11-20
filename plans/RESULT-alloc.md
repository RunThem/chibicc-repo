# RESULT-alloc.md - libcp 分配模型整改执行记录(已归档)

> 归档说明(2026-11-21): 本文件是已完成的 per-TU 分配模型线(移除内部 arena)的执行记录, 随该线收官归档; 现行计划见 `PLAN.md`, 现行记录见 `RESULT.md`. 文中的提交与数值是该线终态, 不再随新线更新.

本文件记录 `PLAN.md`(移除内部 arena)的实际执行结果. 本线开工 HEAD 为 `2a142e4`.

## 用户决定 (2026-11-19)

- 删除库内 arena, 避免与嵌入方 arena 嵌套.
- 解析期间不做任何释放; 只在 context 边界批量归还; 调用方不调用 `libcp_context_free`
  时库不兜底, 由进程退出回收.
- 保留 `libcp_context_reset` 的批量释放: 现行公共契约与测试要求 reset 归还全部
  per-TU 分配.

## 实施

- `libcp/src/lib.c`: 删除 64 KiB bump arena, `ARENA_BLOCK_SIZE` 与
  `LIBCP_DEBUG_ARENA`; 新增 `LibcpTuAlloc` 链头部, `link_tu_alloc` /
  `unlink_tu_alloc` / `free_tu_allocs`, 以及 `libcp_tu_alloc/calloc/realloc/strdup/strndup`.
- `libcp/src/libcp.h`: context 字段 `arena` 改为 `tu_allocs`; 删除
  `LibcpArenaBlock`; 声明 `libcp_tu_*`.
- 8 个阶段源文件的 73 处 `libcp_arena_*` 调用改名为 `libcp_tu_*`; `libcp_tu_alloc`
  去掉 `align` 参数 (分配器承诺任意类型对齐, 头部尺寸按 `_Alignof(max_align_t)` 取整).
- 配置 (`include_paths`, `macro_ops`, `cst_seeds`) 与 context 自身仍走未登记的
  `LIBCP_*`, 由 `libcp_context_free` 释放.
- `test/self/self-test.c`: `arena_test` 改为 `alloc_test`, 保留 realloc 契约.
- `test/allocator.sh`: 新增 200 条声明的逐对象分配锁; reset 后活块回到
  `context_new` 后的基线.
- `test/library.sh`: 去掉 `-DLIBCP_DEBUG_ARENA`; ASan 现在直接覆盖每个对象.
- `test/public-api.sh`: 内部符号 deny 前缀加入 `tu`.
- 文档: `AGENTS.md`, `README.md`, `docs/` 的 arena 表述改为 per-TU 分配; benchmark
  线归档为 `PLAN-benchmark.md` / `RESULT-benchmark.md`.

## 闸门

- `make test`: 通过 (41 个 `test/*.c`, 全部行为锁, fmt 与 c2c).
- `make test-stage2`: 通过 (自举后重跑全部测试).
- `make check-context`: accessors 0, TLS 0, `cg_ctx` 0, `.a` accessor symbols 0.
- 本机汇编快照: 改动前在 `2a142e4` 生成 41 份 `test/*.c` 的 `.s`; 改动后用同一命令
  重建, `diff -ru` 为空.
- `test/allocator.sh`: `allocator: ok`; 200 条声明产生至少 200 个独立活块,
  reset 后回到基线, `context_free` 后活块为 0.
- `test/self-test.sh`: `alloc: ok`, `hashmap: ok`, `sema-state: ok`.
- `test/public-api.sh`: 全部公共头自包含, D7 齐全, 消费者不引用内部符号.
- `test/library.sh`: 两轮均 `library: ok` (含 ASan + UBSan, reset 活字节闸门).
- `make format`: 已运行. 本地 clang-format 为 22.1.8, CI 固定 20; 22 对未改动的
  `codegen.c` 产生 16 行对齐差异, 已还原, 使其不进入本线 diff. 本线改动的文件通过
  本地格式检查.
- Docker: `make docker-image` 因 registry 连接被 reset 失败, 未运行
  `make docker-snapshot` / `make docker-snapshot-diff`; 按 AGENTS 的 x86-64 Linux
  原生路径用上述本机快照替代.

## Benchmark

- 环境: Debian, kernel 7.2.15, i7-8700K, nproc 2, cc 16.2.0, glibc 2.43,
  repeat 5 (warmup 1).
- 同环境对照: 在临时 worktree `/tmp/libcp-bench-before` 检出 `2a142e4` 跑一次, 当前
  工作树跑一次, 语料与 flags 相同.
- 数值 (before -> after):

  | group  | total          | reset            | peak              | calls              |
  |--------|----------------|------------------|-------------------|--------------------|
  | tests  | 256.44 ms -> 279.94 ms | 11.33 ms -> 41.93 ms | 17.11 MB -> 17.57 MB | 3,274 -> 557,769 |
  | lib    | 1.36 s -> 1.32 s       | 41.78 ms -> 182.81 ms | 50.31 MB -> 51.38 MB | 15,310 -> 2,191,335 |
  | driver | 264.50 ms -> 245.78 ms | 9.86 ms -> 33.63 ms  | 49.80 MB -> 50.91 MB | 2,812 -> 401,971 |
  | sqlite | 1.86 s -> 2.14 s       | 5.02 ms -> 264.44 ms | 684.77 MB -> 725.92 MB | 17,074 -> 3,221,155 |

- 分配请求总量 (before -> after): tests 105.30 MB -> 109.62 MB, lib 508.33 MB ->
  518.54 MB, driver 93.77 MB -> 95.89 MB, sqlite 684.77 MB -> 726.12 MB.
- 结论: 分配调用次数从每 TU 的少量 arena 块变成每个对象一次; reset 现在遍历并释放
  全部 per-TU 分配, sqlite 的 reset 从 5.02 ms 升到 264.44 ms, total +15%. 峰值上升
  约 3-6% (每个对象 16 字节链头部). tokenize / preprocess / parse / sema 的时间在
  噪声内或略降. 嵌入方提供 arena 时, 池化发生在嵌入方, 库不再嵌套.
- 原始输出: `.cache/bench/2026-11-19-before.txt` / `.cache/bench/2026-11-19-after.txt`.

## 提交

- `b70287e` Replace the internal arena with tracked per-TU allocations
- `42f6d3d` Document the per-TU allocation model
- 本记录所在的收尾提交 (归档 benchmark 线, 建立本线 PLAN 与 RESULT).

## 偏差

- Docker snapshot 不可用 (registry 网络), 用本机同一命令的 41 份汇编快照对比.
- 本地 clang-format 22 与 CI 的 20 不同; 未改动的 `codegen.c` 按 22 有格式差异, 已还原.
