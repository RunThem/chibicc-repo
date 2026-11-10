# RESULT.md - libcp 前端性能基线执行记录

本文件记录 `PLAN.md`(libcp 前端性能基线线)各步骤的实际执行结果, 供审核与后续会话接续
参考. 前一条主线(库边界固化与产品化验证)的记录见归档 `RESULT-library.md`; 本线开工 HEAD
为 `56a64d6`.

## 状态

- 本线尚未开工; B0-B4 均为未完成.
- 计划与归档文件已于 2026-11-10 建立: `PLAN.md` / `RESULT.md`; 旧主线归档为
  `PLAN-library.md` / `RESULT-library.md`.
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

(待 B0.1 起逐项填写.)
