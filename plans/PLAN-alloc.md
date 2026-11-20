# PLAN-alloc.md - libcp 分配模型整改(移除内部 arena)执行计划(已归档)

> 归档说明(2026-11-21): 本文件是已完成的 per-TU 分配模型线(移除内部 arena)的执行计划, 随该线收官归档; 现行计划见 `PLAN.md`, 现行记录见 `RESULT.md`. 文中的步骤勾选状态与闸门记录是该线终态, 不再随新线更新.

目标: 删除 `libcp/src/lib.c` 中的 64 KiB bump arena. 每个 per-TU 对象改为一次独立的
`LIBCP_*` 分配; context 用一条只含 `next` / `prev` 的登记链保存 `free` 需要的地址.
`libcp_context_reset` 与 `libcp_context_free` 沿链批量归还, 解析期间不释放. 库不再叠加
自己的 arena 或池化, 嵌入方的分配器 (包括 arena) 是唯一的分配策略层.

## 用户拍板 (2026-11-19)

- 删除内部 arena, 避免与嵌入方 arena 嵌套.
- 解析期间不做任何释放; 只在 context 边界一次性归还.
- 调用方不调用 `context_free` 时库不兜底, 由进程退出回收.
- reset 保留批量释放: 现行公共契约与测试要求 reset 归还全部 per-TU 分配; 若只让
  `context_free` 释放, 需另行修改契约与测试.

## 设计

- 分配: `LibcpTuAlloc` 头部 (两个链指针) + 对象; 头尺寸向上取整到
  `_Alignof(max_align_t)`, 保持分配器的对齐承诺.
- `libcp_tu_alloc/calloc/realloc/strdup/strndup`: 直接调用
  `LIBCP_MALLOC/CALLOC/REALLOC`; calloc 零初始化; realloc 先摘链再重挂, 并把
  `[min(old_size,new_size), new_size)` 清零; `NULL` 旧指针返回全零块.
- 释放: `free_tu_allocs` 只在 reset / free 调用; 配置与 context 自身仍走未登记的分配.
- 删除 `ARENA_BLOCK_SIZE` 与 `LIBCP_DEBUG_ARENA`; 73 处 `libcp_arena_*` 改名为
  `libcp_tu_*`.

## 步骤

- [x] A1 核心实现与机械改名.
- [x] A2 测试: self-test 改名; allocator.sh 加逐对象锁; library.sh 去掉
  `LIBCP_DEBUG_ARENA`; public-api deny 前缀加 `tu`.
- [x] A3 文档: AGENTS / README / docs 的 arena 表述改为 per-TU 分配.
- [x] A4 归档 benchmark 线, 建立本线记录.
- [x] A5 闸门: make test, make test-stage2, make check-context, 本机汇编快照 diff,
  make bench.

## 闸门

- `make test` 与 `make test-stage2` 全绿.
- `make check-context` 无访问器 / TLS / `cg_ctx`.
- 本机 41 份 `test/*.c` 汇编快照与改动前逐字节一致.
- `test/allocator.sh` 证明每个 per-TU 对象独立分配, reset 后活块回到基线.
- benchmark 结果记录在 `RESULT.md`.
