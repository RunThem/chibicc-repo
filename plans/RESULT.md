# RESULT.md - libcp 前端性能基线执行记录

本文件记录 `PLAN.md`(libcp 前端性能基线线)各步骤的实际执行结果, 供审核与后续会话接续
参考. 前一条主线(库边界固化与产品化验证)的记录见归档 `RESULT-library.md`; 本线开工 HEAD
为 `56a64d6`.

## 状态

- B0.1 已完成; B1-B4 尚未开工.
- 计划与归档文件已于 2026-11-10 建立: `PLAN.md` / `RESULT.md`; 旧主线归档为
  `PLAN-library.md` / `RESULT-library.md` (提交 `70b8ebf`).
- 计划: `PLAN.md`.

## 闸门口径(本线生效)

- 每步: 构建零新告警.
- B1/B2: 单文件, 单组, sqlite 三种路径都能跑; 失败文件不产生行且退出码 0.
- B3: `make test` 与 `make check-context` 全绿; `make clean` 后无 bench 产物.
- B4: `make bench` 产出 `tests` / `lib` / `driver` / `sqlite` 四组; 记录环境块, 表格
  与 shim 开销对照.
- 全程: `libcp/`, `main.c`, `codegen.c` 零 diff.

## 开工基线(B0 前)

- HEAD: `56a64d6` (Update the docs to match the current code).
- 工作树: 干净.
- 默认构建: `CFLAGS` 无 `-O`; benchmark 将使用独立 `-O2` 构建.
- 语料: `test/*.c` 41 个 + `test/common`; `libcp/src/*.c`; `main.c` + `codegen.c`;
  `thirdparty/sqlite/sqlite3.c` 存在.
- 本机: Ubuntu 24.04 devcontainer, `cc` 13.3.0, nproc 2.

## 执行记录

### B0.1 文档与基线复验

- 改动: `AGENTS.md` 的全局约定新增"计划与记录"条目(现行线 `plans/PLAN.md` /
  `plans/RESULT.md`, 已完成的线归档为 `PLAN-<主题>.md` / `RESULT-<主题>.md`).
  `PLAN.md` / `RESULT.md` 的建立与旧线归档已在上一步 `70b8ebf` 完成.
- 闸门: 本步在 `70b8ebf` 上运行, 工作树只含 `AGENTS.md` 改动.
  - `make -j2`: 退出码 0, `-Wall` 无告警.
  - `make test`: 退出码 0; 41 个 `test/*.c` 用例全部运行, 其后各 `test/*.sh`
    行为锁全绿(`cst-stats` 45 UNPARSED / 48 文件, `library` ok, `allocator` ok,
    `fmt` 48 文件幂等, `c2c` 81 个 golden 样本).
  - `make check-context`: 退出码 0 (codegen 无 `cg_ctx`, `.a` 无访问器符号).
- 偏差: B0.1 的"更新 AGENTS.md 的最小条目"按归档语境解释为新增计划 / 记录约定;
  `bench` 的 `make bench` / `make bench-tool` 条目留到 B3.3, 避免提前写入尚不存在的
  目标.
