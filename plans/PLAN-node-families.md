# PLAN-node-families.md - LibcpNode 字段族拆分执行计划(已归档)

> 归档说明(2026-11-21): 本文件是已完成的 LibcpNode 字段族拆分线的执行计划, 随该线收官归档; 现行计划见 `PLAN.md`, 现行记录见 `RESULT.md`. 文中的步骤勾选状态与闸门记录是该线终态, 不再随新线更新.

目标: 只拆 `LibcpNode`. 把 69 种 kind 共用的 46 个平铺字段改为"公共头 + 命名族
union": 每个节点仍是一次固定大小分配, `kind` 仍是运行时标签, 但每个 kind 只接触
一个字段族. 其他公共结构体 (`LibcpType`, `LibcpObj`, `LibcpToken`, `LibcpContext`)
本线不动.

## 用户拍板 (2026-11-21)

- 接受公共 API 的源码级变更: `node->cond` 改为 `node->ctrl.cond`.
- 只拆 `LibcpNode`; 其他结构体暂不拆.
- 公共结构体布局变化按仓库既有约定升版本: `0.1.0` -> `0.2.0`.

## 量化基线

- `LibcpNode` 当前 400 字节, 46 个顶层字段, 69 种 kind.
- 全仓库约 1,100 处 `->field` 访问; 其中约 610 处落在非公共头字段上 (编译器错误驱动迁移 613 处).
- 单节点含 per-TU 头从 416 字节降到 160 字节. 初版 probe 的 614,516 是 parse 后与 sema 后
  两次遍历的累计值, 实际节点分配量以 bench 的 parse/sema 计数为准; bench 实测 sqlite
  peak 从 725.92 MB 降到 631.97 MB, 约 -13%.
- `atomic_addr` 全仓库零引用; `atomic_expr` 只在 `range.c` 与 `c2c.c` 被读,
  没有任何写入点. 两者先删.

## 设计

公共头保留:

- `kind`, `next`, `ty`, `tok`
- `lhs`, `rhs`, `ty_op`, `spec_decls`

其余字段按族放进匿名 union, 族名注入到 `LibcpNode`, 字段名不再注入:

```c
union {
  LibcpNodeCtrl ctrl;
  LibcpNodeDecl decl;
  LibcpNodeValue value;
  LibcpNodeCase case_;
  LibcpNodeAccess access;
  LibcpNodeJump jump;
  LibcpNodeAssign assign;
  LibcpNodeIncDec incdec;
  LibcpNodeAsm asm_;
  LibcpNodeCas cas;
};
```

访问形式为 `node->ctrl.cond`, `node->decl.attr`, `node->value.val`.

### 族划分

| 族 | 字段 | kind |
|---|---|---|
| `ctrl` | `cond, then, els, init, inc, args, func_ty, generic_sel, is_elvis` | COND, IF, FOR, WHILE, DO, SWITCH, FUNCALL, GENERIC |
| `decl` | `body, is_scope_block, attr, name_tok, decl_init, init_resolved, var` | BLOCK, STMT_EXPR, FUNCDEF, DECL, GVAR_DECL, COMPOUND_LITERAL, VAR, VLA_PTR, STRING, MEMZERO |
| `value` | `val, fval, ty_op2, is_implicit` | NUM, SIZEOF, ALIGNOF, TYPES_COMPATIBLE, REG_CLASS, CAST, ENUM_CONST |
| `case_` | `begin, end, is_default, begin_expr, end_expr, colon_tok` | CASE |
| `access` | `member, arrow_tok` | MEMBER |
| `jump` | `label` | GOTO, LABEL, LABEL_VAL |
| `assign` | `op` | ASSIGN |
| `incdec` | `is_post, addend` | INCDEC |
| `asm_` | `asm_str` | ASM |
| `cas` | `cas_addr, cas_old, cas_new` | CAS |

只使用公共头的 kind: 二元/一元/下标/逗号/逻辑运算, `ND_RETURN`, `ND_EXPR_STMT`,
`ND_GOTO_EXPR`, `ND_NULL_EXPR`, `ND_BREAK`, `ND_CONTINUE`, `ND_IDENT`, `ND_TYPEDEF`,
`ND_GENERIC_ASSOC`, `ND_EXCH`.

### 尺寸

- 公共头 64 字节, union 最大成员 `decl` 80 字节, 合计 144 字节, 16 字节对齐.
- 固定大小分配: `libcp_new_node` 不变; `ND_IDENT -> ND_VAR/ND_NUM` 与
  `ND_INCDEC -> ...` 这类原地 kind 改写不会越界.
- `long double fval` 仍在 union 内, 分配器的 16 字节对齐契约不变.

### 族查询

新增公共函数:

```c
typedef enum {
  LIBCP_NF_NONE,
  LIBCP_NF_CTRL,
  LIBCP_NF_DECL,
  LIBCP_NF_VALUE,
  LIBCP_NF_CASE,
  LIBCP_NF_ACCESS,
  LIBCP_NF_JUMP,
  LIBCP_NF_ASSIGN,
  LIBCP_NF_INCDEC,
  LIBCP_NF_ASM,
  LIBCP_NF_CAS,
} LibcpNodeFamily;

LIBCP_API LibcpNodeFamily libcp_node_family(LibcpNodeKind kind);
```

它进 D7 白名单, `.so` 导出与公共 API 文档同步更新.

### 通用 walker

以下函数会不加区分地读取所有子节点字段, 必须改为按族遍历:

- `main.c`: `ast_node`, `ast_fields`
- `libcp/src/range.c`: `walk`
- `tools/c2c.c`: `collect_node`
- `libcp/src/sema.c`: `check_children`, `resolve_children`
- `codegen.c`: `shape_children`

遍历的节点集合必须与改造前一致; `main.c:ast_node` 还必须在保持原有全局顺序的前提下
加族判断, 因为 `test/ast.sh` 逐字节锁定输出.

## 步骤

- [x] A1 归档分配模型线, 建立本线 `PLAN.md` / `RESULT.md`.
- [x] A2 删除 `atomic_addr` / `atomic_expr`, 同步删除 `range.c` / `c2c.c` 的读取.
- [x] A3 增加 `LibcpNodeFamily` 与 `libcp_node_family`, 在原 fat struct 上把六个
  通用 walker 改为按族遍历; `ast.sh` 与 `c2c.sh` 输出逐字节不变.
- [x] A4 引入十个族 struct 与 union, 迁移约 610 处字段访问; 审计所有非 switch
  的跨族读取; `sizeof(LibcpNode)` 应为 144.
- [x] A5 文档与版本: `docs/02`, `docs/06`, `docs/09`, `docs/10`; D7 白名单加
  `libcp_node_family`; `LIBCP_VERSION` 与 `Makefile` `VERSION` 升到 `0.2.0`.
- [x] A6 闸门: `make format`, `make test-all`, `make docker-snapshot-diff`,
  `make bench`, `make check-context`.

## 闸门

- `make test-all` 全绿 (含自举).
- `test/ast.sh`, `test/c2c.sh`, `test/diagnostic.sh`, `test/shape.sh` 输出逐字节不变.
- 汇编快照与改动前逐字节一致.
- `make bench` 记录 sqlite peak 与总时间; 目标 peak 约 -20%, 时间不显著劣化.
- `test/library.sh` 的 ASan+UBSan 无回归.
- 全仓库不再有对非活动族的读取.

## 非目标

- 不拆 `LibcpType`, `LibcpObj`, `LibcpToken`, `LibcpContext`.
- 不把 sema 标注搬到 sidecar.
- 不按 69 种 kind 建独立 struct, 不引入节点堆上 payload 指针.
- 不改变任何语义或发射形状.
