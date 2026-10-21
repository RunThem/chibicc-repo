# PLAN.md - 库边界固化与产品化验证执行计划(路线图阶段 5 + 阶段 6)

目标: **把第 1-4 层做成一个可链接, 可重置, 可注册诊断的 C 库 `libchibicc`**, 编译器本体退化为
"驱动器 + x86-64 codegen" 的参考消费者. 本线结束时, 库外的软件可以只包含
`<chibicc/chibicc.h>`, 链接 `libchibicc.a` 或 `libchibicc.so`, 对一个翻译单元依次调用
tokenize / preprocess / parse / sema(或只走 CST / provenance), 并在长驻进程里用 `context`
重置整个 arena. **阶段 6 (原"参考消费者", 本文件称 L5) 从可选改为必选**: 公共 API 必须由
一个真实消费者验证, 不能只由合成测试证明.

2026-10-19 修订: 用户拍板把 CST 契约修复, 公共面收窄, 打包与真实消费者纳入本计划; 原来已勾选
的 L4 三项恢复为未完成, 因为 `RESULT.md` 自己列出的两条终态验收("公共头只暴露 D7"与 `nm`
清单一致)尚未满足. 修订后的顺序是 L3.0 -> L3.1b/c/d -> L3.2a/b -> L3.3b/c -> L3.4a/b/c ->
L3.5 -> L4 -> L5.

用户 2026-10-19 拍板的四件事:

- 库名与公共头: `libchibicc`, 公共头根 `<chibicc/...>`, 公共符号用 `chibicc_` / `Chibicc`
  前缀; 二进制仍叫 `chibicc`.
- ABI: **只承诺源码兼容, 不承诺 ABI**. 公共结构体字段直接暴露, 换字段即换版本.
- context 粒度: **一个 context 对应一个翻译单元**. 不同 context 可以在不同线程并存; 同一个
  context 不跨线程并发使用.
- 公共 `Node` / `Obj`: **只保留第 3/4 层字段**; codegen 的字段全部进 codegen 私有表.

本线终态(验收口径):

- `make libchibicc.a` 产出静态库; Linux 下另有 `libchibicc.so`; `chibicc` 链接静态库.
- `libchibicc/include/chibicc/` 下的公共头拆成 context / diagnostic / token / cst / ast /
  sema / preprocess / provenance / umbrella, 每个头可独立 `-fsyntax-only` 编译, 不包含内部头,
  不声明未加前缀的内部函数; `ChibiccContext` / `JmpFrame` / `HashMap` / `NodeArray` 不出现在
  公共面.
- 公共符号清单与 D7 一致(`.a` 的口径见 L3.2b), 由 `nm` 白名单和 `test/public-api.sh`
  校验; 共享库只导出 D7, 其余符号用 `-fvisibility=hidden` 隐藏.
- `ChibiccContext` 提供 `new` / `free` / `reset`, 诊断回调, include 路径与命令行宏配置;
  `reset` 释放全部 per-TU 内存并保留配置; 库层不再有裸 `calloc` / `malloc` / `realloc` /
  `strdup`, reset 后无泄漏.
- 库层不再有文件域语义 static, 不再有 `exit()` 调用点; 错误全部经诊断回调 + 返回 NULL.
- 既有行为逐字节不变: `make docker-test`(含自举), `make docker-snapshot-diff` 为空,
  诊断 55 例 / 形状 33 段 / trivia 94 项 / CST 305 项 / expand 全绿.
- 新增 `test/library.sh`: 用宿主 cc 链接 `libchibicc.a`, 验证 tokenize / parse / sema /
  CST / provenance 的最小使用路径, 诊断回调, context reset 与错误恢复; 新增
  `test/public-api.sh` 与 `make check-library` 覆盖头文件, 符号白名单, 负例与多 context.
- 打包面: `make install` 安装公共头与库, `libchibicc.pc` 可被 `pkg-config` 使用,
  `CHIBICC_VERSION` 与 `chibicc_version()` 可用; stage2 用 chibicc 重编库对象.
- L5 终态(不计入阶段 5 完成): 一个 CST formatter 能消费库并通过幂等, 编译等价与注释保真
  测试; CST 结构覆盖率与 typedef 供给有明确口径; AST 有 source range 或 CST 映射;
  诊断回调能拿到展开点信息; README 有库用法.

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

### 2026-10-19 复核(修订依据)

- 顶层 `.c` + `.h` 共 15,799 行; `codegen.c` 2,773 行在库外.
- 公共头 `libchibicc/include/chibicc/chibicc.h` 1,107 行, 约 118 个函数声明; 实际导出
  的 `T` 符号 117 个, 其中 90 个没有 `chibicc_` 前缀(`parse` / `error` / `add_type` /
  `hashmap_*` / `cst_*` 等), 静态库消费者可能与宿主符号碰撞.
- 公共头仍暴露 `HashMap` / `JmpFrame` / `ChibiccContext` 的全部字段 / `jmp_buf` /
  `chibicc_arena_*` / 短 typedef; 头部自己 `#define _POSIX_C_SOURCE` 并 include
  `glob.h` 等只属于驱动器的系统头; `MAX` / `MIN` / `__attribute__` 兜底也在公共面.
- `CstNode` 的 children 数组在 `cst.c:51` 用裸 `realloc` 分配, 不归 arena 管;
  `chibicc_context_reset` 只释放 arena, 因此 CST 解析后 reset 会泄漏 children 数组.
- CST 结构覆盖: 48 个语料文件里 29 个含 `CST_UNPARSED`; 按首个子节点分类, 731 个 span 以
  `ASSERT` 开头(GNU statement expression `({ ... })`), 另有 `va_list` / `pthread_t` /
  `MyInt` 等头文件 typedef 不在文件内 oracle 里导致的声明回退. round-trip 不受影响, 但
  formatter 的结构化覆盖率低于"完成"的预期.
- `chibicc_reconstruct` 仍在 `main.c:594`, 不在 D7 的库实现里; L3.4 的 provenance /
  reconstruct 逐字节断言未加.
- Makefile 只有 `libchibicc.a`, 没有 `.so` / `-fPIC` / 可见性 / SONAME / install /
  pkg-config / 版本宏; stage2 没有用 chibicc 重编库对象.
- README 仍是上游教学编译器叙述, 没有 `libchibicc` 用法; 修订前本文件的 L4.1-L4.3 曾
  勾选, 但 `RESULT.md` 的终态验收仍列两项未满足, 状态不一致.

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
- 公共头卫生: 不在头里 `#define _POSIX_C_SOURCE`; 该宏由 Makefile 的 `CFLAGS` 提供.
  不 include `glob.h` / `sys/wait.h` 等只属于驱动器的头; `MAX` / `MIN` 与
  `__attribute__` 兜底移入内部头. 每个公共头必须能单独 `-fsyntax-only` 编译.
- 内部头: 根目录 `chibicc.h` 保留为内部 umbrella, 包含公共头, 定义内部类型与声明,
  并给实现提供短名别名. `ChibiccContext` 的完整定义, `JmpFrame`, `HashMap`,
  `NodeArray` 只出现在内部头; `ParseScope` / `SemaScope` 的定义可以留在实现文件, 但都不
  出现在公共头.
- 类型: 公共面用 `Chibicc` 前缀(`ChibiccToken` / `ChibiccNode` / `ChibiccType` /
  `ChibiccObj` / `ChibiccCstNode` / ...); 结构体 tag 改成 `struct chibicc_*`,
  内部用 `typedef ChibiccNode Node;` 等别名, 15k 行实现不因改名变吵.
- 枚举常量保留层前缀 `TK_` / `ND_` / `TY_` / `CST_` / `REL_`, 不做二次改名.
- 函数: 只有公共面加 `chibicc_` 前缀, 由 `lib.c` 作为 facade 提供; 内部函数名不变.
  公共面清单见 D7, 并以 `test/public-api.sh` 的 `nm` 白名单为准.
- 不承诺 ABI; 版本从 `0.1` 起, 公共头提供 `CHIBICC_VERSION` 与 `chibicc_version()`,
  共享库 SONAME 为 `libchibicc.so.0`. 换字段即升版本.

### D6 目录与构建

- 不移动现有源文件; 新增 `lib.c`(公共 facade)与 `libchibicc/include/chibicc/`(公共头).
- 编译器构建增加 `-Ilibchibicc/include`; 不把 `libchibicc/include` 和自举头目录
  `include/` 混在一起(避免宿主 gcc 看到 chibicc 的 `stddef.h` 等自举头).
- `LIB_SRCS` = 除 `main.c` / `codegen.c` 外的全部 `.c`; `libchibicc.a` 由它们构建;
  Linux 下另建 PIC 的 `libchibicc.so`(macOS 只承诺 `.a`).
- `.so` 用 `-fPIC -fvisibility=hidden` 构建, D7 的函数用 `CHIBICC_API`
  (`__attribute__((visibility("default")))`) 标出, 其余符号不外露; SONAME 为
  `libchibicc.so.0`.
- `.a` 的内部符号策略是 L3.2b 的拍板项: 要么在源码层加统一前缀重命名, 要么接受
  "静态库内部符号可见"并在文档里声明. `.so` 的可见性不受这个选择影响.
- `chibicc` 链接 `libchibicc.a` + `main.o` + `codegen.o`.
- `make test` 与 `make test-stage2` 都跑 `test/library.sh`; stage2 用 chibicc 重新编译
  库对象, 证明新代码可自举, 并让 `test/library.sh` 链接 stage2 产出的库对象.
- `make install` 安装公共头, `.a` / `.so` 与 `libchibicc.pc`; `make check-library`
  跑 `test/library.sh` 与 `test/public-api.sh`.

### D7 公共 API 面

```
context:      chibicc_context_new / _free / _reset
              chibicc_context_set_diagnostic / _failed
              chibicc_context_add_include_path / _define / _undef
layer 1:      chibicc_tokenize_file / _tokenize_buffer / chibicc_reconstruct
preprocess:   chibicc_preprocess
layer 2:      chibicc_cst_parse / _cst_reconstruct / _cst_dump / _cst_verify
layer 3:      chibicc_parse
layer 4:      chibicc_sema
provenance:   chibicc_token_provenance / chibicc_macro_prov_* /
              chibicc_macro_expansion_* / chibicc_expansion_dump / _verify
version:      CHIBICC_VERSION / chibicc_version
```

`chibicc_reconstruct` 从 `main.c` 搬进库(当前 `main.c` 的 `reconstruct` 只依赖 token 链,
是第 1 层的公开能力). 内部函数(`tokenize_file` / `preprocess` / `parse` / `sema` /
`cst_*` / 访问器)保留原名, 由 facade 调用. `chibicc_context_activate` /
`chibicc_context_set_base_file` / `chibicc_arena_*` / `hashmap_*` 等不是 D7, 收进内部头.
D7 是唯一清单: 新增公共函数必须先改 D7 与 `test/public-api.sh` 的白名单, 不能只加
`CHIBICC_API`.

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

- [x] **L2.1 context 骨架与 facade**:
  新增 `lib.c`: `ChibiccContext` 定义(先放内部头), arena 块链, `active_ctx`(TLS),
  `new` / `free` / `reset` 骨架, 以及 D7 的公共包装函数(先声明在根 `chibicc.h`).
  `main.c` 创建 context, 所有入口经包装调用. 此步先不迁 static, 只建立边界.
  验收: 构建; 本机全部只读通道; docker-test; snapshot 为空.
- [x] **L2.2 tokenize 状态与文件表**:
  `current_file` / `input_files` / `at_bol` / `has_space` 进 context;
  `include_paths` 从 `main.c` 的全局搬进 context 配置, `search_include_paths` 读
  context. `chibicc_context_add_include_path` 生效.
  验收: `-E` / 诊断锚点 / trivia / CST 逐字节不变; docker-test.
- [x] **L2.3 preprocess 状态**:
  `macros` / `cond_incl` / `pragma_once` / `include_next_idx` 进 context;
  `init_macros` / `define_macro` 在 `preprocess` 入口按 context 配置执行.
  验收: `-E` 逐字节不变; expand 全绿; docker-test.
- [x] **L2.4 parse 作用域**:
  `parse.c` 的 `scope` 进 context; typedef/tag oracle 的语义不变.
  验收: 全语料 parse; docker-test.
- [x] **L2.5 sema 状态**:
  10 个 static 进 context; `new_unique_name` 的计数器随 context;
  `const_expr` 供 `#if` 使用时仍走同一个 context.
  验收: 全语料 sema; diagnostics; docker-test.
- [x] **L2.6 诊断回调与 longjmp**:
  加 `ChibiccDiagnostic` / 回调注册 / 默认处理器; `error` / `error_at` / `error_tok`
  改成"回调 + longjmp"; 公共入口捕获并返回 NULL; 驱动器返回 1. 无 active context 时
  保持旧的打印 + exit 路径. 默认处理器逐字节复现旧格式.
  验收: `test/diagnostic.sh` 55 例逐字节; `test/driver.sh` 退出码; 新增一个
  "注册回调后不再 exit" 的库测试(先放 `test/library.sh` 草稿); docker-test.
- [x] **L2.7 分配点收口**:
  库层 `calloc` / `strdup` / `strndup` / `realloc` 全部改走 arena(驱动器和 `lib.c`
  自己的 arena 实现除外); `format` 在有 context 时用 arena.
  验收: grep 库层零 `calloc` / `strdup`; 全部只读通道; docker-test; snapshot 为空.
- [x] **L2.8 状态收口验证**:
  grep 证明 `tokenize.c` / `preprocess.c` / `parse.c` / `sema.c` 无文件域可变 static
  (只读关键字表与 `active_ctx` 除外); `chibicc_context_reset` 后连续跑两个 TU 的
  测试(先放 library.sh).
  验收: 两条 grep 留档; docker-test.

## 阶段 L3: 契约修复 / 公共头 / 命名 / 构建

- [x] **L3.0 契约修复(CST arena 与状态簿记)**:
  `cst.c` 的 `cst_add_child` 改走 `chibicc_arena_realloc`(当前 `cst.c:51` 是裸
  `realloc`, `reset` 后泄漏 children 数组); 给 `test/library.sh` 加 "CST -> reset"
  循环与内存上界/LSan 检查. 本修订已把 L4.1-L4.3 恢复为未完成并与 `RESULT.md` 对齐;
  本步只做代码与测试, 不再改状态.
  验收: `test/library.sh` 在 reset 循环下无泄漏; 构建零新告警; docker-test.
- [x] **L3.1a 公共头路径拆分**:
  根 `chibicc.h` 改为内部 umbrella(`#define CHIBICC_INTERNAL` + include);
  类型与声明搬到 `libchibicc/include/chibicc/chibicc.h`; 构建加 `-Ilibchibicc/include`;
  消费者可以只 include `<chibicc/chibicc.h>`.
  验收: `test/library.sh` 用公共路径编译通过; docker-test.
- [x] **L3.1b 公共头拆分与 context 不透明化**:
  按 D5 拆成 context / diagnostic / token / cst / ast / sema / preprocess / provenance /
  umbrella; 把 `ChibiccContext` 的完整定义, `JmpFrame`, `HashMap`, `NodeArray` 移入
  根内部头; `ParseScope` / `SemaScope` / `CondIncl` 的定义可以留在实现文件, 但它们的
  前向声明从公共面移走; 公共面只留 D7 与公共 IR 类型.
  公共 struct 的字段保持可见(第 3/4 层消费者要读), 但 context 不透明.
  验收: 每个公共头单独 `cc -std=c11 -fsyntax-only -Wall -Wextra` 通过; 消费者只
  include `<chibicc/chibicc.h>` 能编译; `grep` 证明公共头无 `HashMap` / `JmpFrame` /
  `jmp_buf` / `chibicc_arena_*` / `chibicc_context_activate`.
- [x] **L3.1c 公共头卫生**:
  移除公共头里的 `#define _POSIX_C_SOURCE`(改由 Makefile `CFLAGS` 提供), `glob.h` /
  `sys/wait.h` 等驱动器专用 include, `MAX` / `MIN` 宏与 `__attribute__` 兜底; 公共头
  只 include 标准头与彼此.
  验收: 公共头在 `-std=c11` 下独立编译; 消费者先 include `<stdio.h>` 再 include
  公共头也能编译; `grep` 无宏污染.
- [x] **L3.1d 公共面自包含闸门**:
  新增 `test/public-api.sh`: 每个公共头单独编译; 用只含公共 include 路径的消费者
  链接 `libchibicc.a`; 断言消费者不引用内部符号. `nm` 白名单在 L3.2b 接入, 避免在
  `.a` 符号策略拍板前就把检查写死.
  验收: 本机与 docker 都通过; 消费者只依赖 D7.
- [x] **L3.2a 短 typedef / tag 与 reconstruct(部分完成)**:
  D7 的包装函数已带 `chibicc_` 前缀, 公共类型已有 `Chibicc*` 别名; 剩余: 把短
  typedef 与 `struct Token` / `struct Node` 等 tag 从公共面移走, 公共面只用
  `Chibicc*`, 实现侧用内部别名; `chibicc_reconstruct` 从 `main.c:594` 搬进 `lib.c`.
  验收: 公共头 `grep` 无短 typedef; `test/public-api.sh` 覆盖 reconstruct 逐字节断言.
- [x] **L3.2b 导出白名单与内部符号策略**:
  拍板 `.a` 内部符号策略: 源码级统一前缀重命名, 或接受静态库内部符号可见并在文档
  声明. 无论选哪种, `.so` 必须只导出 D7; `test/public-api.sh` 的 `nm` 检查按拍板
  结果实现(`.so` 严格白名单, `.a` 白名单或"D7 全部存在"两种口径).
  验收: `nm -D libchibicc.so` 只含 D7; `.a` 口径与文档一致; D7 的符号全部存在.
- [x] **L3.3a Makefile 静态库目标**:
  加 `libchibicc.a`(除 `main.c` / `codegen.c` 外的全部 `.c`, 含 `lib.c`);
  `chibicc` 链接 `main.o` + `codegen.o` + 静态库; `make test` 与 `make test-stage2`
  跑 `test/library.sh`; docker BOOT 排除本机 `libchibicc.a`.
  验收: `make clean && make`; `test/library.sh` 本机通过; docker 闸门见 L4.
- [x] **L3.3b 共享库**:
  Linux `libchibicc.so`(`-fPIC`, `-fvisibility=hidden`, `CHIBICC_API`, SONAME
  `libchibicc.so.0`); 用 visibility 或 version script 限制导出; `make clean && make`
  后 `nm -D` 校验.
  验收: `test/library.sh` 能链接 `.so`; `nm -D libchibicc.so` 只含 D7.
- [x] **L3.3c stage2 库对象与安装面**:
  stage2 用 chibicc 重编库对象, 让 `test/library.sh` 链接 stage2 的 `.a`; 新增
  `make install`, `libchibicc.pc`, `CHIBICC_VERSION` / `chibicc_version()`.
  验收: stage2 库对象能链接; `pkg-config --cflags --libs libchibicc` 可用; 安装目录
  只含公共头, `.a` / `.so` 与 `.pc`.
- [x] **L3.4a library.sh 断言补全**:
  当前覆盖: 只 include 公共头, tokenize buffer -> preprocess -> parse -> sema,
  CST verify, 诊断回调(不写 stderr / 不 exit), 错误后 `reset` 再跑第二个 TU.
  剩余: provenance 访问器与 `chibicc_reconstruct` 的逐字节断言; `tokenize_file` 路径;
  include 路径与 context `define` / `undef`.
- [x] **L3.4b 负例与多 context**:
  每个公共入口的错误返回(缺文件, 坏指令, 坏表达式, 坏类型), 回调只收一次且不 exit;
  同线程双 context 嵌套, 跨线程两个 context 并存; error 后 reset 再跑第二个 TU.
  验收: 上述路径全绿; 回调字段的 filename / line / column 有断言.
- [x] **L3.4c ASan/LSan 闸门**:
  宿主 cc 构建 library 消费者时加 `-fsanitize=address,undefined`; 跑 reset 循环与
  CST / parse / sema 路径; 目标是零泄漏零 UB. 这是 L3.0 泄漏回归的长期守卫.
- [x] **L3.5 阶段闸门**:
  `make docker-test`, `make docker-snapshot-diff`(空), `make check-library`,
  `make test-stage2`; 更新 `RESULT.md` 的本线记录与 `AGENTS.md` 的剩余项.
  验收: 全绿后才把 L4.1-L4.3 勾选, 阶段 5 才算完成.

## 阶段 L4: 终态

2026-10-19 修订: 以下三项此前已勾选, 但它们是阶段 5 的终态闸门, 不是已经完成的事实. 因为
L3.0-L3.5 仍未完成, 且 `RESULT.md` 的终态验收仍列两项未满足, 这里恢复为未完成; 只有当
L3.5 全绿后才可重新勾选.

- [x] **L4.1 全量闸门**:
  `make docker-test`, `make docker-snapshot-diff`(空), `make check-library`,
  `test/library.sh`, `make test-stage2`, 以及一次 `THIRDPARTY=tinycc`(可选, 时间允许时).
- [x] **L4.2 文档与代码地图**:
  AGENTS.md 的路线图(阶段 5 完成, 阶段 6 改为必选), 开放决策, 构建与测试(库目标,
  `.so`, install, pkg-config), 现状与代码地图(`lib.c`, 公共头拆分, context, arena)
  全部更新; README 增加库用法段落.
- [x] **L4.3 提交与推送**:
  按步骤提交, 最后 `git push`; 推送前确认工作树干净, 远端为当前分支.

## 阶段 L5: 产品化验证(原阶段 6, 必选)

L5 不属于阶段 5 的完成条件, 但它是库"可交付"的验收线. 用户 2026-10-19 拍板把它并入本计划,
不单独建文件. L5 的输入是 L3.5 通过后的公共面.

- [ ] **L5.1 CST formatter MVP**:
  新建 `tools/fmt.c`(或 `examples/`), 只消费第 1/2 层: tokenize_file -> cst_parse;
  对结构化节点输出规范化缩进与空白, 对 `CST_UNPARSED` 原样输出(保证仍是合法源码),
  注释与 trivia 保真. 验收: 对 `test/*.c` 与 `include/*.h` 做幂等
  (`format(format(x)) == format(x)`); 格式化前后 `-S` 快照等价; 注释数量不变;
  不调用预处理器.
- [ ] **L5.2 CST 结构覆盖率与 typedef 供给**:
  新增 `-cst-stats` 或 `test/cst-stats.sh` 统计每个文件的 `UNPARSED` 数量并锁 golden;
  覆盖 GNU statement expression `({ ... })`; 为头文件 typedef(`va_list` / `pthread_t`)
  提供 caller seed 或明确的 fallback 口径. 验收: golden 稳定; 新增样本覆盖上述三类;
  `test/cst.sh` 的 round-trip 不回归.
- [ ] **L5.3 source mapping 与诊断展开点**:
  给 AST 节点提供 source range 或到 `CstNode` 的映射(至少 `start` / `end` token);
  `ChibiccDiagnostic` 增加 token 或展开点字段(默认处理器忽略, 旧输出逐字节不变).
  验收: 一个改写示例能把 AST 子树映射回源码 span; 宏内错误的回调能拿到调用点.
- [ ] **L5.4 文档与示例**:
  README 顶部改为库定位; 新增 `libchibicc/README.md`, 写清 5 层能力, 生命周期(arena /
  reset / 指针失效), 错误模型, 线程模型, 宏保真限制, CST 的 `UNPARSED` 口径, 以及
  最小使用示例; 示例与 `test/library.sh` 保持同步.
- [ ] **L5.5 L5 闸门**:
  `make check-library`, formatter 幂等/等价, `-cst-stats` golden, ASan/LSan 全绿;
  更新 AGENTS.md 的阶段 6 状态.

## 闸门口径(本线生效)

- 每步: 构建零新告警; 本机只读通道(`-E` / `-dump-tokens` / `-reconstruct` /
  `-dump-cst` / `-reconstruct-cst` / `-verify-cst` / `-dump-expansions` /
  `-verify-expansions`)不回归.
- 阶段边界: `make docker-test` + `make docker-snapshot-diff` 为空.
- 行为不变的步骤(L1 全部, L2.1, L3.0): snapshot 必须为空; 任何指令序列变化都回退重做.
- L3.0 起: `test/library.sh` 含 CST -> reset 循环; 宿主侧 ASan/LSan 无泄漏.
- L3.1b 起: 每个公共头单独 `cc -std=c11 -fsyntax-only -Wall -Wextra` 通过;
  消费者只 include `<chibicc/chibicc.h>` 能编译.
- L3.2b 起: `nm libchibicc.a` 的 `T` 符号与 D7 白名单一致; L3.3b 起
  `nm -D libchibicc.so` 只含 D7.
- L3.4c 起: `make check-library` 在 ASan/LSan 下全绿; 双 context / 双线程路径通过.
- L5: formatter 幂等; 格式化前后 `-S` 快照等价; 注释数量不变; `-cst-stats` golden 稳定.
- 红线: 不新增 `exit()`(驱动器与默认处理器除外); 不新增文件域语义 static;
  不把 codegen 假设带回库层; 不改变诊断文案与插入符锚点; 不改变 `-E` 与 CST/trivia
  的逐字节输出; 新代码保持 C11 可自举; 库层不出现裸 `calloc` / `malloc` / `realloc` /
  `strdup`.

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
8. **CST arena 契约**: `cst.c:51` 的裸 `realloc` 在 reset 后泄漏 children 数组; L3.0
   改成 arena 分配, 并在 `library.sh` 加 reset 循环加 LSan. 这是当前唯一已知的库层
   分配点违反, 必须在拆头前修掉, 否则 L3.1b 的验证会建立在错误生命周期上.
9. **公共头可移植性**: `_POSIX_C_SOURCE`, `glob.h`, `sys/wait.h`, `MAX/MIN`,
   `__attribute__` 兜底都会污染消费者; L3.1c 逐项移除, 用"先 include `<stdio.h>` 再
   include 公共头"的消费者样本验证.
10. **静态库内部符号碰撞**: `libchibicc.a` 的 90 个无前缀 `T` 符号可能与宿主同名;
    L3.2b 必须拍板"源码级重命名"还是"文档声明 + 只保证 `.so` 隐藏". 建议先做 `.so`
    可见性, 把重命名作为可选的机械步, 不阻塞 L4.
11. **CST 结构覆盖**: 29/48 语料文件含 `CST_UNPARSED`, 731 个 span 来自 `ASSERT` 里的
    GNU statement expression, 另有头文件 typedef 导致的声明回退; L5.2 用 `-cst-stats`
    golden 锁住基线, 并为 statement expression 与 typedef seed 做显式决定. 不能把
    "round-trip 通过"当成"formatter 可用".
12. **没有真实消费者**: 合成 `library.sh` 只能证明管线通, 不能证明 API 好用; L5.1 的
    formatter 是必选验收, 不是示例工程. 如果 formatter 写不动, 说明公共面还缺东西,
    应回到 L3 补, 而不是降低 L5 标准.
13. **诊断展开点**: `error_tok` 仍用 `tok->file` / `tok->line_no`, 宏内错误指向宏体而不是
    调用点; L5.3 给 `ChibiccDiagnostic` 加 token 或展开点字段, 默认处理器保持旧输出
    逐字节不变.
14. **拆头与自举**: 公共头拆分后, stage2 的 chibicc 要能编译新头; L3.1b 每次提交都跑
    `make test-stage2`, 失败则先缩小拆头范围, 不退回单头.

## 终态验收(本线完成时逐项实测填写)

### 阶段 5 (L0-L4)

- 产物: `libchibicc.a` 与 Linux `libchibicc.so` 可链接; `chibicc` 链接静态库; stage2
  用 chibicc 重编的库对象可链接.
- API: 公共头拆成 D5 的 9 个头, 每个独立编译; `.so` 只暴露 D7 且 `nm -D` 白名单一致;
  `.a` 的内部符号策略按 L3.2b 的拍板执行并写入文档.
- 状态: `ChibiccContext` 不透明; 公共头无 `HashMap` / `JmpFrame` / `jmp_buf` /
  `chibicc_arena_*` / 短 typedef; 无 `_POSIX_C_SOURCE` 等宏污染.
- 生命周期: new / reset / free 三态; reset 后第二个 TU 正确; CST -> reset 循环在
  LSan 下无泄漏; 同线程双 context 与跨线程双 context 通过.
- 诊断: 回调注册后无 stderr 输出, 无进程退出; 默认回调下诊断 55 例逐字节不变;
  每个公共入口的错误返回有断言.
- 行为: docker-test 全绿; snapshot diff 为空; 自举通过; `make check-library` 全绿.
- 打包: `make install`, `libchibicc.pc`, `CHIBICC_VERSION` / `chibicc_version()` 可用.
- 文档: AGENTS.md, README, 本文件与 `RESULT.md` 一致.

### L5 (产品化验证)

- formatter: 对 `test/*.c` 与 `include/*.h` 幂等; 格式化前后 `-S` 快照等价; 注释数量不变.
- CST: `-cst-stats` golden 稳定; statement expression 与 typedef seed 有明确口径.
- 映射: AST 节点可回到源码 span; 宏内错误回调能拿到调用点.
- 文档: `libchibicc/README.md` 有 5 层能力, 生命周期, 错误模型, 线程模型, 宏保真限制,
  CST `UNPARSED` 口径与最小示例.
