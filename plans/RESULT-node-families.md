# RESULT-node-families.md - LibcpNode 字段族拆分执行记录(已归档)

> 归档说明(2026-11-21): 本文件是已完成的 LibcpNode 字段族拆分线的执行记录, 随该线收官归档; 现行计划见 `PLAN.md`, 现行记录见 `RESULT.md`. 文中的提交与数值是该线终态, 不再随新线更新.

本文件记录 `PLAN.md` (LibcpNode 字段族拆分) 的实际执行结果. 本线开工 HEAD 为
`c22c7c3`.

## 用户决定 (2026-11-21)

- 接受 `node->cond` 改为 `node->ctrl.cond` 的公共 API 源码级变更.
- 只拆 `LibcpNode`; 其他结构体本线不动.
- 版本从 `0.1.0` 升到 `0.2.0`.

## 步骤

- [x] A1 归档分配模型线, 建立本线计划与记录.
- [x] A2 删除 `atomic_addr` / `atomic_expr`.
- [x] A3 族查询与通用 walker 改造.
- [x] A4 族 struct 与 union, 字段访问迁移.
- [x] A5 文档与版本.
- [x] A6 闸门.

## 实现

- 删除 `atomic_addr` (零引用) 与 `atomic_expr` (无写入点); 同步删除 `range.c` 与
  `c2c.c` 的读取.
- `libcp/include/libcp/ast.h`:
  - 公共头保留 `kind`, `next`, `ty`, `tok`, `lhs`, `rhs`, `ty_op`, `spec_decls`.
  - 新增 `LibcpNodeCtrl`, `LibcpNodeDecl`, `LibcpNodeValue`, `LibcpNodeCase`,
    `LibcpNodeAccess`, `LibcpNodeJump`, `LibcpNodeAssign`, `LibcpNodeIncDec`,
    `LibcpNodeAsm`, `LibcpNodeCas` 十个族 struct.
  - `LibcpNode` 用匿名 union 持有这十个族; 访问形式为 `node->ctrl.cond`,
    `node->decl.attr`, `node->value.val` 等.
  - 新增公共 `LibcpNodeFamily` 与 `libcp_node_family(kind)`; 进 D7 白名单.
- `sizeof(LibcpNode)` 从 400 字节降到 144 字节, 仍为 16 字节对齐; 节点仍是一次
  固定大小分配, `libcp_new_node` 未按 kind 改变尺寸.
- 通用 walker 按族遍历: `main.c` 的 `ast_node`/`ast_fields`, `range.c` 的 `walk`,
  `c2c.c` 的 `collect_node`, `sema.c` 的 `check_children`/`resolve_children` 与
  `libcp_add_type` 的通用下降, `codegen.c` 的 `shape_children`.
- 字段访问迁移: 编译器错误驱动的脚本完成 613 处 `->field` 替换; 随后手工审计
  非 switch 的跨族读取, 并给 `ast_fields` 的 `ty_op2` / `name_tok` 加族判断.
- `test/library.sh` 内嵌消费者跟随新字段名.
- 版本: `LIBCP_VERSION` 与 `Makefile` `VERSION` 升到 `0.2.0`; 文档 01/06/07/08/09/
  10/11/12/README 与 AGENTS 的版本/函数计数同步.
- D7 从 38 个函数变为 39 个; `.a` 内 `libcp_` 前缀外部符号从 130 变为 131.

## 闸门结果

- `make format` 与 `make format-check`: 通过 (本机 clang-format 22.1.8).
- `make test`: 全绿.
- `make test-stage2`: 全绿 (chibicc 自己编译含匿名 union 的公共头).
- `make test-all`: 全绿.
- `make check-context`: 通过, 访问器/TLS/`cg_ctx` 均为 0.
- `make check-library`: 通过; `library` / `allocator` / `public-api` / `install` 全绿,
  `libcp.so` 恰好导出 39 个 D7 函数, pkg-config 报告 `0.2.0`.
- `test/ast.sh`: 85 例逐字节一致.
- `test/c2c.sh`: 46 文件 normalized-AST-equal, 54 文件汇编一致, 81 例逐字节一致,
  65 个 kind 覆盖.
- 汇编快照: 以 HEAD `c22c7c3` 的临时 worktree 为基线, 本机生成 41 份 `test/*.c`
  快照, 与当前树逐字节 diff 为空.
- `make bench` (repeat 5, 同机对照 `RESULT-alloc.md` 的 after 行):

  | group  | total                    | peak                    | 分配请求           |
  |--------|--------------------------|-------------------------|--------------------|
  | tests  | 279.94 ms -> 278.48 ms   | 17.57 MB -> 17.12 MB    | 109.62 MB -> 100.70 MB |
  | lib    | 1.32 s -> 1.29 s         | 51.38 MB -> 47.50 MB    | 518.54 MB -> 496.11 MB |
  | driver | 245.78 ms -> 243.73 ms   | 50.91 MB -> 46.83 MB    | 95.89 MB -> 89.46 MB   |
  | sqlite | 2.14 s -> 2.07 s         | 725.92 MB -> 631.97 MB  | 726.12 MB -> 632.17 MB |

  sqlite peak 下降 93.95 MB, 约 -12.9%; 时间与分配次数不劣化 (总调用次数
  3,221,155 不变).

## 偏差

- Docker 快照不可用: 本机没有 `chibicc:amd64` 镜像与 `.cache/snapshot` 基线;
  按分配模型线的先例, 用本机临时 worktree 的 41 份快照对比, 结果为空.
- 本机 clang-format 为 22.1.8, CI 固定 20; `make format` / `make format-check`
  在本机通过. 本次未改动的文件没有被 22 重新格式化.
- `PLAN.md` 初版的 614,516 "可达节点" 是 parse 后与 sema 后两次遍历的累计值,
  实际节点分配量以 bench 的 parse/sema 计数为准; PLAN 已按实测 peak 修正.

## 提交

本线在分支 `node-field-families` 上分步提交:

- `811b029` Archive the allocation line and add the node split plan
- `505d0bf` Split LibcpNode into a common header and field families
- `e57e52e` Document the LibcpNode field families
- 本记录所在的提交.
