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

- 动作: `codegen.c` 新增指针键的 `CgNode` / `CgType` / `CgObj` 三张表(4096 桶链地址);
  `is_lowered` / `brk_label` / `cont_label` / `pass_by_stack` / `ret_buffer` /
  `case_next` / `default_case` / ND_CASE 的生成标签全部改走 `cg(node)`.
- 说明: ND_CASE 之前借用公共 `Node.label` 存 codegen 分配的名字, 现在存进
  `cg(node)->unique_label`; `Node.label` 只留拼写名.
- 验收: 本机 287 次只读通道运行(`-dump-tokens` / `-dump-cst` / `-reconstruct` /
  `-reconstruct-cst` / `-verify-cst` / `-dump-expansions` / `-verify-expansions`,
  41 个 `test/*.c` × 7)与基线 `8c1c1d1` 的产物逐字节一致; 本机可编译的 39 个文件的
  `-S` 汇编也与基线逐字节一致; `make docker-test` EXIT=0; snapshot 为空.
- 偏差: `pass_by_stack` 仍在表里(它在 `push_args` 的两遍扫描之间需要持久化).

## L1.2 标签值链路

- 动作: `Relocation.label`(`char **`)拆成 `char *label`(序列化时已知的符号名)与
  `Node *label_node`([GNU] `&&label`); sema 的 `eval2` / `eval_rval` out-param 改为
  文件内私有的 `LabelRef { char *name; Node *node; }`; codegen 发射数据时
  `rel->label ? rel->label : cg(rel->label_node)->unique_label`.
- 说明: 原实现把 `&node->unique_label` 或 `&node->var->name` 存进 `char **`, 是
  sema 写 codegen 字段的唯一硬耦合; 拆开后 sema 不再持有 codegen 槽的地址.
- 验收: control / commonsym 相关用例; snapshot 为空; docker-test 全绿.

## L1.3 goto_next 出公共面

- 动作: sema 的 `gotos` / `labels` 改为 `NodeArray`; `Obj.label_gotos` 从单链表改为
  `NodeArray`; codegen 的 `cg_labels` 改为私有的 `NodeArray`; `Node.goto_next` 删除.
- 验收: control 用例; snapshot 为空; docker-test 全绿.

## L1.4 Obj 帧字段与 Type 的 VLA 槽

- 动作: `Obj.offset` / `stack_size` 进 `CgObj` 表; `codegen.c` 的全部读取走
  `obj_offset()` / `obj_stack_size()`, 赋值写 `cg_obj(...)`; `Type.vla_size` 进
  `CgType` 表. `Member.offset`(布局结论)、`Relocation.offset`(数据镜像位置)、
  `va_area` / `alloca_bottom`(sema 物化的 ABI 对象)保持不动.
- 验收: snapshot 为空; docker-test 全绿.

## L1.5 公共 Node / Obj 收口

- 动作: 从 `chibicc.h` 删除上述字段与注释; `Type` 只保留 `vla_len`(维度表达式),
  `Obj` 只保留 sema 结论; 文件头注释改述"库层只留源码与语义".
- 验证: `grep 'is_lowered\|brk_label\|cont_label\|pass_by_stack\|ret_buffer\|unique_label\|case_next\|default_case\|goto_next\|vla_size\|stack_size' chibicc.h`
  无命中; `grep goto_next *.c *.h` 无命中; 构建零新告警.
- 闸门: `make docker-test` EXIT=0(`.cache/L1-docker-test.log`);
  `make docker-snapshot-diff` -> `snapshot diff: empty`(`.cache/L1-snapshot.log`).
- 偏差: 计划只列了 Node 的五个字段与 Obj 的帧字段; 实施时补上了 ND_CASE 的生成标签
  与 `Type.vla_size`, 它们同样是 codegen 私有状态.

## L2.1 context 骨架与 facade

- 动作: 新增 `lib.c` - `ChibiccContext` 骨架, 块链 arena(`chibicc_arena_alloc/calloc/
  strdup/strndup/realloc`), `_Thread_local chibicc_active_ctx` 访问器,
  `chibicc_context_new/free/reset/activate/set_diagnostic/add_include_path/define/undef`,
  以及 `chibicc_tokenize_file` / `_tokenize_buffer` / `_preprocess` / `_parse` / `_sema` /
  `_cst_*` / `_expansion_*` 包装. `main.c` 创建 context, 所有入口经包装调用.
- 说明: 公共包装显式收 `ChibiccContext *`; 实现侧通过 active 指针访问状态. 这是
  PLAN D2 的落地形态, 本步还没有迁移任何 static, 也没有注册 longjmp 帧.
- 验收: 构建; 本机 287 次只读通道 + 39 个文件的 `-S` 与基线逐字节一致.
- 偏差: 无.

## L2.2 tokenize 状态与文件表

- 动作: 删除 `tokenize.c` 的 `current_file` / `input_files` / `at_bol` / `has_space`,
  全部改走 `chibicc_ctx()`; `input_files` 变成 context 里的可增长数组(NULL 结尾),
  `get_input_files()` 返回它; `include_paths` 从 `main.c` 的全局搬进 context 配置
  (`chibicc_context_add_include_path`), `search_include_paths` / `search_include_next`
  读 context; 同时把 `search_include_paths` 里的函数域 `static HashMap cache` 收进
  context(`search_cache`), 它也是跨 TU 的隐藏状态.
- 验收: 本机 287 次只读通道 + 39 个文件的 `-S` 与基线逐字节一致; 构建零新告警.
- 偏差: 无. `input_files` 的容量按 `file_no + 2` 增长, 保持原有 NULL 结尾契约.

## L2.3 preprocess 状态

- 动作: 删除 `preprocess.c` 的 `macros` / `cond_incl` / `pragma_once` static, 统一走
  `chibicc_ctx()`; `chibicc_context_new` 在 arena 建立后调用 `init_macros`;
  `chibicc_context_define` / `_undef` 以**有序**操作列表记录命令行宏
  (`D:name=body` / `U:name`) 并立即应用, `reset` 清空后重放该列表;
  `main.c` 的 `-D` / `-U` 改走 context 接口.
- 说明: 顺序保留 `-DX -UX -DX` 的语义; `search_cache` 已在 L2.2 收进 context.
- 验收: 287 次只读通道与基线逐字节一致; `-E` 样本一致; `-DX=1 -UX -DX=2`
  的预处理结果与基线一致; 构建零新告警.
- 偏差: 无.

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
