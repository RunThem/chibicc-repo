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

## 2026-10-19 计划修订说明

`PLAN.md` 同日修订: L4.1-L4.3 恢复为未完成; 新增 L3.0(CST arena 契约), L3.1b/c/d,
L3.2a/b, L3.3b/c, L3.4a/b/c 与 L3.5; 原阶段 6 改为 L5 必选. 本文件下面 L4 一节记录的
是当时实际跑过的闸门, 不是阶段 5 的终态验收. 新版计划的第一个执行项是 L3.0.

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
  `CgType` 表. `Member.offset`(布局结论), `Relocation.offset`(数据镜像位置),
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

- 动作: 删除 `parse.c` 的 `static ParseScope *scope`; 新增 `parse_init()`, 在 context
  的 arena 里建根作用域; `parse()` 入口调用它, 因此同一 context 的第二次 parse 从空作用域
  开始; 文件内部用 `#define scope (chibicc_ctx()->scope)` 保持原有写法.
- 验收: 287 次只读通道 + 39 个文件的 `-S` 与基线逐字节一致; 构建零新告警.
- 偏差: 无.

## L2.5 sema 状态

- 动作: 10 个文件域 static(`locals` / `globals` / `sema_fn` / `builtin_alloca` /
  `gotos` / `labels` / `loop_depth` / `switch_depth` / `unique_name_id` /
  `resolving_body`)与 `Scope *scope` 收进 context; `struct Scope` 的 tag 改名
  `struct SemaScope`; 新增 `sema_init()` 在 arena 里建根作用域并清空 per-function 状态,
  `sema()` 入口调用; `fn->locals` 的字段访问走 `obj_locals` / `set_obj_locals` 两个
  小 helper, 避开同名宏.
- 说明: 宏在 `sema_init` 之后定义, 因此 `sema_init` 本身访问的是 `ctx->` 字段而不是
  被宏重写的短名.
- 验收: 287 次只读通道 + 39 个文件的 `-S` 与基线逐字节一致; 构建零新告警.
- 偏差: 无.

## L2.6 诊断回调与 longjmp

- 动作: 新增 `ChibiccDiagnostic` / `ChibiccDiagnosticFn` /
  `chibicc_context_set_diagnostic`; `error` / `error_at` / `error_tok` 改成
  "格式化 -> 回调(或旧格式打印) -> `raise_error()`"; `raise_error` 在 context 有
  `jmp_top` 时 `longjmp`, 否则保持旧的 `exit(1)`(驱动器 pre-cc1 路径). 公共入口各压一个
  `JmpFrame`, 出错返回 NULL / false; 驱动器 `cc1` 用 `ctx->failed` 区分空翻译单元与错误.
  默认路径逐字节复用旧的 `verror_at` 输出.
- 同时把库对驱动器的反向依赖挪进库: `file_exists` / `align_to` / `print_escaped` /
  `token_kind_name` 从 `main.c` / `codegen.c` 移到 `strings.c` / `tokenize.c`;
  `__BASE_FILE__` 改用 `ctx->base_file`, 由 `chibicc_context_set_base_file` /
  首次 tokenize 设置.
- 新增库消费样本(临时, 后并入 `test/library.sh`): 注册回调后解析坏输入, 回调收到
  `expected an expression` 且进程不退出; `reset` 后再解析成功; 5 轮 tokenize ->
  preprocess -> parse -> sema 的 `globals` 计数稳定.
- 验收: 287 次只读通道 + 39 个文件的 `-S` 与基线一致; `test/diagnostic.sh` 55 例
  逐字节通过; 构建零新告警.
- 偏差: 驱动器在 macOS 上跑 `test/driver.sh` 仍会在汇编阶段失败(Apple `as`), 与改动无关;
  docker 为权威.

## L2.7 分配点收口

- 动作: 库层 `calloc` / `strdup` / `strndup` 全部改走 `chibicc_arena_*`;
  `strarray_push` / `nodearray_push` 的扩容改 `chibicc_arena_realloc`(旧容量显式传入);
  `format` 改为 `vsnprintf` 两次写入 arena; `read_file` 的 memstream 缓冲复制进 arena 后
  `free`; `hashmap` 的桶数组走 arena. 代码生成器的不在库内, 保持 `calloc`.
- 验收: 5 轮 reset 压测稳定; 287 次只读通道 + 39 个文件的 `-S` + 55 例诊断全部通过.
- 偏差: `open_memstream` 只在 `print_tokens` 的输出路径保留(驱动器的 -E 输出), 不参与
  库对象生命周期.

## L2.8 状态收口验证

- 静态审计追加发现并收进 context 的函数域 static: `tokenize_file` 的 `file_no`,
  `is_keyword` 的关键字 HashMap, `is_typename` 的关键字 HashMap,
  `include_file` 的 `include_guards` HashMap, `counter_macro` 的 `__COUNTER__` 计数器
  (加上 L2.2 的 `search_cache`). 这些都是 arena 分配, 若不迁移会在 reset 后悬垂.
- 结果: 库层只剩只读表:`read_punct` / `is_keyword` / `is_typename` 的关键字数组与
  `format_date` 的月份名; 没有文件域可变 static, 没有函数域可变 static.
- 闸门: 最终代码由 L3 收口闸门覆盖 - `make docker-test` EXIT=0
  (`.cache/L3-docker-test.log`, 含 stage2 自举; `_Thread_local` / `setjmp` /
  `va_copy` 均被 chibicc 自己编译通过; `test/library.sh` 在 stage1 与 stage2 各跑一遍,
  均输出 `library: ok`); `make docker-snapshot-diff` -> `snapshot diff: empty`
  (`.cache/L3-snapshot.log`); 诊断 55 例 / trivia / CST / expand 全绿.

## L3.1 公共头拆分

- 动作(已完成的部分): `chibicc.h` 移到 `libchibicc/include/chibicc/chibicc.h`;
  根 `chibicc.h` 变为内部 umbrella(`#define CHIBICC_INTERNAL` + include); 构建加
  `-Ilibchibicc/include`; `test/library.sh` 只 include `<chibicc/chibicc.h>`.
- 偏差: 公共头目前仍是"完整内部头"搬家, 内部函数声明与 `HashMap` / 作用域类型仍在
  公共面; 收窄到 D7 是 L3.1b, 尚未执行. 因此本步只完成了路径与构建边界, 不是最终
  的公共面.

## L3.2 公共函数前缀与 facade 收口

- 动作(已完成的部分): D7 的包装函数全部带 `chibicc_` 前缀并在公共头声明; 公共类型
  追加了 `ChibiccToken` / `ChibiccNode` / `ChibiccType` / `ChibiccObj` /
  `ChibiccCstNode` 等别名; `ChibiccContext` 是显式句柄.
- 偏差: 短 typedef 与 struct tag 仍可见; `chibicc_reconstruct` 仍在 `main.c`;
  `nm` 公共符号清单尚未收窄. 这是 L3.2 的剩余工作.

## L3.3 Makefile 库目标

- 动作(已完成的部分): `libchibicc.a` 由除 `main.c` / `codegen.c` 外的全部 `.c`
  构建(含 `lib.c`); `chibicc` 链接 `main.o` + `codegen.o` + 静态库; `make test` 与
  `make test-stage2` 调用 `test/library.sh`; docker BOOT 排除本机 `libchibicc.a`.
- 偏差: Linux `libchibicc.so`(PIC / 可见性 / SONAME)与"stage2 用 chibicc 重编库对象"
  尚未做; 当前 library.sh 链接宿主 cc 构建的静态库.

## L3.4 库测试定稿

- 动作: `test/library.sh` 用宿主 cc 编译一个只 include 公共头的消费者, 链接
  `libchibicc.a`; 覆盖 tokenize buffer -> preprocess -> parse -> sema, CST parse +
  verify, 诊断回调(不写 stderr / 不 exit), 错误后 `reset` 再跑第二个 TU.
- 本机结果: `library: ok`, EXIT=0.
- 偏差: provenance 访问器与 `chibicc_reconstruct` 的逐字节断言尚未加入.

## L4 终态验收

2026-10-19 修订: 本节只是当时跑过的闸门记录. `PLAN.md` 已把 L4.1-L4.3 恢复为未完成,
因为 L3.1b / L3.2 / L3.3b / L3.4 仍未完成; 阶段 5 的终态验收以 `PLAN.md` 的新口径为准.

- L4.1 全量闸门: `make docker-test` EXIT=0(`.cache/L3-docker-test.log`, 5,036 行,
  stage1 + stage2 + 自举 + 全部通道 + `test/library.sh` 两次通过);
  `make docker-snapshot-diff` -> `snapshot diff: empty`(`.cache/L3-snapshot.log`).
- 本机: 287 次只读通道 + 39 个文件的 `-S` 与基线 `8c1c1d1` 逐字节一致; 诊断 55 例
  逐字节通过; `test/library.sh` 输出 `library: ok`.
- L4.2 文档: AGENTS.md 的路线图 / 构建与测试 / 现状与代码地图已更新; PLAN.md 与
  RESULT.md 的勾选与偏差一致.
- **未完成(本线后续)**: L3.1b 公共面收窄, L3.2 短 typedef / tag 移出公共面与
  `chibicc_reconstruct` 进库, L3.3b `.so` 与 stage2 库对象, L3.4 的 provenance /
  reconstruct 断言. 因此终态验收里"公共头只暴露 D7"与"`nm` 清单一致"两项尚未满足;
  当前交付是可链接的静态库与可用的 context / 阶段 API, 公共头仍是完整内部头的搬家.

## 提交一览

| 提交 | 步骤 | 内容 |
|---|---|---|
| `4456d41` | L0.1 | 归档 provenance 线, 建立库化线, 基线闸门 |
| `8e58192` | L1.1-L1.5 | codegen 状态移出公共 Node / Type / Obj |
| `28e9956` | L2.1 | context 骨架, arena, facade |
| `f459f7f` | L2.2 | tokenizer / include 路径状态进 context |
| `75158e5` | L2.3 | 预处理器状态与命令行宏进 context |
| `8f76115` | L2.4 | parser 作用域进 context |
| `375eb3d` | L2.5-L2.8 | sema 状态, 诊断回调 + longjmp, arena 分配, 状态审计 |
| (本提交) | L3 | 公共头路径, `libchibicc.a`, `test/library.sh` |
