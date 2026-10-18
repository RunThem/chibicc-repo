# PLAN.md - 库边界固化执行计划(路线图阶段 5)

目标: **把第 1-4 层做成一个可链接, 可重置, 可注册诊断的 C 库 `libchibicc`**, 编译器本体退化为
"驱动器 + x86-64 codegen" 的参考消费者. 本线结束时, 库外的软件可以只包含
`<chibicc/chibicc.h>`, 链接 `libchibicc.a`, 对一个翻译单元依次调用 tokenize / preprocess /
parse / sema(或只走 CST / provenance), 并在长驻进程里用 `context` 重置整个 arena.

用户 2026-10-19 拍板的四件事:

- 库名与公共头: `libchibicc`, 公共头根 `<chibicc/...>`, 公共符号用 `chibicc_` / `Chibicc`
  前缀; 二进制仍叫 `chibicc`.
- ABI: **只承诺源码兼容, 不承诺 ABI**. 公共结构体字段直接暴露, 换字段即换版本.
- context 粒度: **一个 context 对应一个翻译单元**. 不同 context 可以在不同线程并存; 同一个
  context 不跨线程并发使用.
- 公共 `Node` / `Obj`: **只保留第 3/4 层字段**; codegen 的字段全部进 codegen 私有表.

本线终态(验收口径):

- `make libchibicc.a` 产出静态库; Linux 下另有 `libchibicc.so`; `chibicc` 链接静态库.
- `libchibicc/include/chibicc/` 下的公共头自包含, 不包含内部头, 不声明未加前缀的内部函数.
- `ChibiccContext` 提供 `new` / `free` / `reset`, 诊断回调, include 路径与命令行宏配置;
  `reset` 释放全部 per-TU 内存并保留配置.
- 库层不再有文件域语义 static, 不再有 `exit()` 调用点; 错误全部经诊断回调 + 返回 NULL.
- 既有行为逐字节不变: `make docker-test`(含自举), `make docker-snapshot-diff` 为空,
  诊断 55 例 / 形状 33 段 / trivia 94 项 / CST 305 项 / expand 全绿.
- 新增 `test/library.sh`: 用宿主 cc 链接 `libchibicc.a`, 验证 tokenize / parse / sema /
  CST / provenance 的最小使用路径, 诊断回调, context reset 与错误恢复.

五条已归档线: 语法语义拆分见 `PLAN-split.md` / `RESULT-split.md`, 忠实层收尾见
`PLAN-faithful.md` / `RESULT-faithful.md`, codegen 直连见 `PLAN-codegen-direct.md` /
`RESULT-codegen-direct.md`, trivia 保留见 `PLAN-trivia.md` / `RESULT-trivia.md`, CST 见
`PLAN-cst.md` / `RESULT-cst.md`; provenance 线(本线的前置)见 `PLAN-provenance.md` /
`RESULT-provenance.md`. 本线从 provenance 线终态 `8c1c1d1` 起步.

新会话恢复方法: 通读 AGENTS.md -> 查看本文件勾选状态 -> `git log --oneline` 确认最后完成
的步骤 -> 从第一个未勾选项继续; 开工前先跑 L0.1 的闸门复验. 行号一律以函数名为准.

## 现状锚点(开工前实测)

- 顶层 `.c` + `.h` 共 15,080 行(不含 `test/` 与 `include/` 的自举头).
- `chibicc.h` 987 行, 89 个非 static 函数声明; 真正的公共 API 预计收窄到约 30 个.
- 库层的文件域语义 static 共 19 个: `tokenize.c` 4(`current_file` / `input_files` /
  `at_bol` / `has_space`), `preprocess.c` 4(`macros` / `cond_incl` / `pragma_once` /
  `include_next_idx`), `parse.c` 1(`scope`, typedef/tag oracle), `sema.c` 10
  (`locals` / `globals` / `sema_fn` / `builtin_alloca` / `gotos` / `labels` / `loop_depth` /
  `switch_depth` / `unique_name_id` / `resolving_body`).
- `error` / `error_at` / `error_tok` 的调用点约 145 处; 库层分配点约 60 处
  (`calloc` + `strdup` / `strndup`).
- 公共 `Node` 里的 codegen 字段: `is_lowered`(`chibicc.h:551`), `brk_label` / `cont_label`
  (`569-570`), `pass_by_stack`(`598`), `ret_buffer`(`602`), `unique_label`(`612`), 以及
  `case_next` / `default_case`. `goto_next` 由 sema 的配对检查与 codegen 共用.
- `Obj` 的帧字段: `offset`(`chibicc.h:289`)与 `stack_size`(`309`); `va_area` /
  `alloca_bottom` 是 sema 物化的 ABI 对象, 保留.
- 唯一的跨层字段耦合: `Relocation.label` 是 `char **`(`chibicc.h:333`),
  `sema.c:2408` 把 `&node->unique_label` 交出去, `codegen.c:2230` 再填名字.
- 驱动器把 `include_paths` 定义在 `main.c`(`StringArray include_paths`), `preprocess.c`
  直接读它; 这是库与驱动器的隐藏共享状态, 本线一并收进 context.
- 基线闸门在 L0.1 实测填写(HEAD `8c1c1d1`, 工作树只有 `.zcode/` 与 `.zcodeignore`).

## 设计决定

### D1 公共 Node / Obj

公共 `Node` 只保留"描述源码写法或语义结论"的字段. codegen 的 per-node 状态进
`codegen.c` 私有的指针键表 `CgNode`, 首次访问时懒建:

```
{brk_label, cont_label, unique_label, ret_buffer, pass_by_stack, is_lowered,
 case_next, default_case}
```

`goto_next` 从 `Node` 消失: sema 的配对检查用 sema 私有的 `NodeLink` 链, codegen 的整形遍
自己从树遍历重建. `Obj` 的 `offset` / `stack_size` 进 `codegen.c` 的 `ObjInfo` 表;
`va_area` / `alloca_bottom` 作为 sema 物化的 ABI 对象留在 `Obj`.

`Relocation.label` 改成 `Node *label_node`: sema 只记录"这个地址指向哪个标签节点",
codegen 在发射数据时从自己的表里解析 `.L` 名. 这是 `unique_label` 能真正搬走的前提.

不采用公共 `Node` 里的 `void *backend` / `void *user`. 它会把后端假设永久留在公共结构里,
所有权和复制语义都不清楚; 如果以后需要工具挂数据, 另开一次有明确 owner 的决策.

### D2 context 与 active context

公共签名显式带 `ChibiccContext *`; 内部实现用 `_Thread_local ChibiccContext *active_ctx`
作为访问器, 公共入口进入时保存旧值, 退出时恢复. 理由: 库层递归函数约 400 个, 全量显式穿参
是一次高风险机械改造; 状态已经全部收进 context, active 指针只是访问器, 不是语义状态.
契约: 不同 context 可在不同线程并存; 同一 context 不跨线程并发; 同一线程嵌套调用通过
保存 / 恢复支持. 这是本线唯一接受的隐式状态, 在公共文档里写明.

所有 per-TU 状态进 context: tokenize 的当前文件与位置标志, preprocess 的宏表 / 条件栈 /
pragma once / include_next 下标, parse 的 typedef/tag 作用域, sema 的 10 个 static,
`include_paths` 与命令行宏.

### D3 arena

一个 context 一个块链 arena; 所有 per-TU 对象(tokens / files / nodes / types / Obj /
CST / MacroExpansion / 诊断文本 / 中间字符串)从 arena 分配. API 只有:

- `chibicc_context_new` / `chibicc_context_free`
- `chibicc_context_reset`: 释放全部 arena 块, 清空 per-TU 状态, 重新初始化内建宏,
  重新应用记录的 include 路径与命令行宏; 保留诊断回调. 所有由该 context 返回的指针立即失效.
- 不提供逐对象 free; 不做 per-phase arena. formatter 只要 token/CST 时可以直接用第二个
  context, 不需要在同一 context 里丢掉 AST 保留 token.

### D4 诊断

`ChibiccDiagnostic { filename, line, column, severity, message }` 经
`chibicc_context_set_diagnostic(ctx, fn, user)` 注册. 内部 `error` / `error_at` /
`error_tok` 改成: 格式化消息 -> 调回调 -> `longjmp` 回最近的公共入口; 警告只调回调.
默认回调逐字节复现今天的 stderr 输出(文件:行: 源码行 + 插入符), 驱动器因此在
`test/diagnostic.sh` 上保持逐字节不变. 公共入口在错误时返回 NULL; 驱动器 `cc1` 返回 1,
进程退出码仍为 1. 错误后 context 处于不一致状态, 文档要求先 `reset` 再复用.

没有 active context 时(驱动器解析命令行等), `error` / `error_at` 保持旧行为: 打印并
`exit(1)`. 这是驱动器的路径, 不是库的路径.

### D5 公共头与命名

- 公共头: `libchibicc/include/chibicc/{chibicc.h, context.h, diagnostic.h, token.h,
  cst.h, ast.h, sema.h, preprocess.h, provenance.h}`; umbrella 为
  `<chibicc/chibicc.h>`. 只包含标准头与彼此, 不包含根目录的 `chibicc.h`.
- 内部头: 根目录 `chibicc.h` 保留为内部 umbrella, 包含公共头, 定义内部类型与声明,
  并给实现提供短名别名.
- 类型: 公共面用 `Chibicc` 前缀(`ChibiccToken` / `ChibiccNode` / `ChibiccType` /
  `ChibiccObj` / `ChibiccCstNode` / ...); 结构体 tag 改成 `struct chibicc_*`,
  内部用 `typedef ChibiccNode Node;` 等别名, 15k 行实现不因改名变吵.
- 枚举常量保留层前缀 `TK_` / `ND_` / `TY_` / `CST_` / `REL_`, 不做二次改名.
- 函数: 只有公共面加 `chibicc_` 前缀, 由新文件 `lib.c` 作为 facade 提供; 内部函数名不变.
  公共面清单见 D7.
- 不承诺 ABI; 版本从 `0.1` 起, 共享库 SONAME 为 `libchibicc.so.0`. 换字段即升版本.

### D6 目录与构建

- 不移动现有源文件; 新增 `lib.c`(公共 facade)与 `libchibicc/include/chibicc/`(公共头).
- 编译器构建增加 `-Ilibchibicc/include`; 不把 `libchibicc/include` 和自举头目录
  `include/` 混在一起(避免宿主 gcc 看到 chibicc 的 `stddef.h` 等自举头).
- `LIB_SRCS` = 除 `main.c` / `codegen.c` 外的全部 `.c`; `libchibicc.a` 由它们构建;
  Linux 下另建 PIC 的 `libchibicc.so`(macOS 只承诺 `.a`).
- `chibicc` 链接 `libchibicc.a` + `main.o` + `codegen.o`.
- `make test` 与 `make test-stage2` 都跑 `test/library.sh`; stage2 用 chibicc 重新编译
  库对象, 证明新代码可自举.

### D7 公共 API 面

```
context:      chibicc_context_new / _free / _reset
              chibicc_context_set_diagnostic
              chibicc_context_add_include_path / _define
layer 1:      chibicc_tokenize_file / _tokenize_buffer / chibicc_reconstruct
preprocess:   chibicc_preprocess
layer 2:      chibicc_cst_parse / _cst_reconstruct / _cst_dump / _cst_verify
layer 3:      chibicc_parse
layer 4:      chibicc_sema
provenance:   chibicc_token_provenance / chibicc_macro_prov_* /
              chibicc_macro_expansion_* / chibicc_expansion_dump / _verify
```

`chibicc_reconstruct` 从 `main.c` 搬进库(当前 `main.c` 的 `reconstruct` 只依赖 token 链,
是第 1 层的公开能力). 内部函数(`tokenize_file` / `preprocess` / `parse` / `sema` /
`cst_*` / 访问器)保留原名, 由 facade 调用.

## 阶段 L0: 归档与本线建立

- [x] **L0.1 归档 / 计划 / 基线**:
  `PLAN.md` / `RESULT.md` 已 `git mv` 为 `PLAN-provenance.md` / `RESULT-provenance.md`;
  本文件与新的 `RESULT.md` 建立; AGENTS.md 的路线图, 开放决策, 构建与测试, 代码地图按
  本线更新. 跑一次基线: `make docker-test`, `make docker-snapshot-diff`(必须
  `snapshot diff: empty`), 记录到 `RESULT.md`. 一步一提交.
  验收: 文档与代码一致; 基线日志留档; 工作树只含本步改动.

## 阶段 L1: 公共 Node / Obj 收口(行为不变)

- [x] **L1.1 codegen per-node 表**:
  在 `codegen.c` 加指针键的 `CgNode` 表, 接管 `is_lowered` / `brk_label` / `cont_label` /
  `pass_by_stack` / `ret_buffer` / `case_next` / `default_case` / ND_CASE 的生成标签;
  `pass_by_stack` 若只在 `push_args` 内写后即读, 改成局部变量, 不进表. 先保留字段定义,
  只改读写点.
  验收: 构建; 本机 `-E` / `-dump-tokens` / `-dump-cst` / `-verify-expansions`;
  `make docker-test`; snapshot 为空.
- [x] **L1.2 标签值链路**:
  `Relocation.label`(`char **`)拆成 `char *label`(已知符号名)与 `Node *label_node`
  ([GNU] `&&label`, 由 codegen 在发射时解析); `eval2` / `eval_rval` 的 out-param 改为
  `LabelRef *`; `write_gvar_data` 记录两者; codegen 发射数据时按 `label` 优先,
  否则从表里取 `unique_label`. 这一步之后 `unique_label` 只属于 codegen.
  验收: 含 `&&label` 的用例(control / commonsym 相关)通过; snapshot 为空; docker-test.
- [x] **L1.3 `goto_next` 出公共面**:
  sema 的 goto/label 配对检查改用 `NodeArray`; `Obj.label_gotos` 从单链表改为
  `NodeArray`; codegen 整形遍用自己的 `NodeArray` 收集标签. `Node.goto_next` 删除.
  验收: control 测试; snapshot; docker-test.
- [x] **L1.4 Obj 帧字段与 Type 的 VLA 槽**:
  `Obj.offset` / `stack_size` 进 `codegen.c` 的 `CgObj` 表, `Type.vla_size` 进
  `CgType` 表; `va_area` / `alloca_bottom` 与 `Member.offset` 不动(前者是 sema
  物化的 ABI 对象, 后者是布局结论).
  验收: snapshot; docker-test.
- [x] **L1.5 收口**:
  从 `chibicc.h` 删除 L1.1-L1.4 的字段与注释; 用 grep 证明公共头无 codegen 字段名
  (`is_lowered` / `brk_label` / `cont_label` / `ret_buffer` / `unique_label` /
  `pass_by_stack` / `case_next` / `default_case` / `goto_next` / `Obj.offset` /
  `stack_size` / `Type.vla_size`). 更新文件头注释.
  验收: 编译零告警; docker-test; snapshot 为空; 提交.

## 阶段 L2: context / arena / 诊断

- [ ] **L2.1 context 骨架与 facade**:
  新增 `lib.c`: `ChibiccContext` 定义(先放内部头), arena 块链, `active_ctx`(TLS),
  `new` / `free` / `reset` 骨架, 以及 D7 的公共包装函数(先声明在根 `chibicc.h`).
  `main.c` 创建 context, 所有入口经包装调用. 此步先不迁 static, 只建立边界.
  验收: 构建; 本机全部只读通道; docker-test; snapshot 为空.
- [ ] **L2.2 tokenize 状态与文件表**:
  `current_file` / `input_files` / `at_bol` / `has_space` 进 context;
  `include_paths` 从 `main.c` 的全局搬进 context 配置, `search_include_paths` 读
  context. `chibicc_context_add_include_path` 生效.
  验收: `-E` / 诊断锚点 / trivia / CST 逐字节不变; docker-test.
- [ ] **L2.3 preprocess 状态**:
  `macros` / `cond_incl` / `pragma_once` / `include_next_idx` 进 context;
  `init_macros` / `define_macro` 在 `preprocess` 入口按 context 配置执行.
  验收: `-E` 逐字节不变; expand 全绿; docker-test.
- [ ] **L2.4 parse 作用域**:
  `parse.c` 的 `scope` 进 context; typedef/tag oracle 的语义不变.
  验收: 全语料 parse; docker-test.
- [ ] **L2.5 sema 状态**:
  10 个 static 进 context; `new_unique_name` 的计数器随 context;
  `const_expr` 供 `#if` 使用时仍走同一个 context.
  验收: 全语料 sema; diagnostics; docker-test.
- [ ] **L2.6 诊断回调与 longjmp**:
  加 `ChibiccDiagnostic` / 回调注册 / 默认处理器; `error` / `error_at` / `error_tok`
  改成"回调 + longjmp"; 公共入口捕获并返回 NULL; 驱动器返回 1. 无 active context 时
  保持旧的打印 + exit 路径. 默认处理器逐字节复现旧格式.
  验收: `test/diagnostic.sh` 55 例逐字节; `test/driver.sh` 退出码; 新增一个
  "注册回调后不再 exit" 的库测试(先放 `test/library.sh` 草稿); docker-test.
- [ ] **L2.7 分配点收口**:
  库层 `calloc` / `strdup` / `strndup` / `realloc` 全部改走 arena(驱动器和 `lib.c`
  自己的 arena 实现除外); `format` 在有 context 时用 arena.
  验收: grep 库层零 `calloc` / `strdup`; 全部只读通道; docker-test; snapshot 为空.
- [ ] **L2.8 状态收口验证**:
  grep 证明 `tokenize.c` / `preprocess.c` / `parse.c` / `sema.c` 无文件域可变 static
  (只读关键字表与 `active_ctx` 除外); `chibicc_context_reset` 后连续跑两个 TU 的
  测试(先放 library.sh).
  验收: 两条 grep 留档; docker-test.

## 阶段 L3: 公共头 / 命名 / 构建

- [ ] **L3.1 公共头拆分**:
  建 `libchibicc/include/chibicc/` 的 9 个头; 把公共类型定义搬过去, 类型名改
  `Chibicc*`, tag 改 `struct chibicc_*`; 根 `chibicc.h` 变内部 umbrella, 加
  `typedef ChibiccNode Node;` 等别名. 公共头只含标准头与公共头.
  验收: 一个只 include `<chibicc/chibicc.h>` 的 TU 用宿主 cc `-fsyntax-only` 通过;
  公共头 grep 无内部符号(`new_node` / `HashMap` / `ctx` / `Initializer` 定义);
  docker-test.
- [ ] **L3.2 公共函数前缀与 facade 收口**:
  D7 清单的包装函数全部带 `chibicc_` 前缀并只在公共头声明; 内部函数名保持不变;
  `chibicc_reconstruct` 从 `main.c` 搬进库; `main.c` 只用公共头 + codegen 内部头.
  验收: `nm libchibicc.a` 的公共符号清单与 D7 一致; `test/library.sh` 通过;
  docker-test.
- [ ] **L3.3 Makefile 库目标**:
  加 `libchibicc.a`, Linux `libchibicc.so`(`-fPIC`, `CHIBICC_API` 可见性,
  SONAME `libchibicc.so.0`); `chibicc` 链接静态库; `make test` 与 `make test-stage2`
  跑 `test/library.sh`; stage2 用 chibicc 重编库对象.
  验收: `make clean && make libchibicc.a && make`; `make test`(docker);
  `make test-stage2`(docker); `test/library.sh` 在本机与 docker 都通过.
- [ ] **L3.4 库测试定稿**:
  `test/library.sh` 覆盖: 只 include 公共头的消费者; tokenize buffer -> CST 重建;
  preprocess -> parse -> sema 的最小路径; provenance 访问器; 诊断回调(不写 stderr,
  不 exit); 错误后 `reset` 再跑第二个 TU; `chibicc_reconstruct` 逐字节.
  验收: 本机与 docker 都过; 断言数计入 `RESULT.md`.

## 阶段 L4: 终态

- [ ] **L4.1 全量闸门**:
  `make docker-test`, `make docker-snapshot-diff`(空), `test/library.sh`,
  `make test-stage2`, 以及一次 `THIRDPARTY=tinycc`(可选, 时间允许时).
- [ ] **L4.2 文档与代码地图**:
  AGENTS.md 的路线图(阶段 5 完成), 开放决策(三条已决), 构建与测试(库目标),
  现状与代码地图(`lib.c`, 公共头, context, arena)全部更新; README 只改与库使用相关的段落.
- [ ] **L4.3 提交与推送**:
  按步骤提交, 最后 `git push`; 推送前确认工作树干净, 远端为当前分支.

## 闸门口径(本线生效)

- 每步: 构建零新告警; 本机只读通道(`-E` / `-dump-tokens` / `-reconstruct` /
  `-dump-cst` / `-reconstruct-cst` / `-verify-cst` / `-dump-expansions` /
  `-verify-expansions`)不回归.
- 阶段边界: `make docker-test` + `make docker-snapshot-diff` 为空.
- 行为不变的步骤(L1 全部, L2.1): snapshot 必须为空; 任何指令序列变化都回退重做.
- 红线: 不新增 `exit()`(驱动器与默认处理器除外); 不新增文件域语义 static;
  不把 codegen 假设带回库层; 不改变诊断文案与插入符锚点; 不改变 `-E` 与 CST/trivia
  的逐字节输出; 新代码保持 C11 可自举.

## 风险与对策

1. **longjmp 与自举**: `setjmp.h` 在 stage2 下要能被 chibicc 编译. L2.6 第一次提交就
   跑 `make test-stage2`; 失败则退回"回调 + 返回错误码"的最小集合(仅公共入口), 不改
   145 个内部调用点的方案.
2. **TLS 自举**: `_Thread_local` 已由 `test/tls.c` 证明可用; L2.1 先在 stage2 下验证,
   失败则退化为普通 `static` 指针并文档化"单线程 context".
3. **诊断逐字节**: 默认处理器单独一条路径, 复用现有的 `verror_at` 输出函数; 每次改完
   先跑 `test/diagnostic.sh`.
4. **状态迁移漏改**: 每个 static 迁移后用 grep 确认旧名字只出现在 context 字段与
   accessor; 分文件提交.
5. **arena 生命周期**: reset 后旧指针失效是契约; library.sh 用"reset 后重新解析"验证,
   不做悬垂指针的防御性检查.
6. **codegen 表查找开销**: 只在 codegen 内, 不影响库消费者; 用局部 `CgNode *ci` 缓存
   连续访问.
7. **公共头泄漏内部类型**: L3.1 后用 `cc -fsyntax-only` 单头编译 + `nm` 清单双向核对.

## 终态验收(本线完成时逐项实测填写)

- 产物: `libchibicc.a` 可链接; Linux `libchibicc.so` 可链接; `chibicc` 链接静态库.
- API: 公共头只暴露 D7; 消费者只 include `<chibicc/chibicc.h>` 即可完成
  tokenize -> preprocess -> parse -> sema 与 CST / provenance 路径.
- 生命周期: context new / reset / free 三态测试通过; reset 后第二个 TU 正确.
- 诊断: 回调注册后无 stderr 输出, 无进程退出; 默认回调下诊断 55 例逐字节不变.
- 行为: docker-test 全绿; snapshot diff 为空; 自举通过.
- 文档: AGENTS.md, README, 本文件与 `RESULT.md` 一致.
