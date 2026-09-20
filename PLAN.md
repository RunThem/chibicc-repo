# PLAN.md - 忠实层收尾执行计划(阀门口径放宽线)

本文件细化"忠实层收尾"线: 在语法语义拆分线(见归档 `PLAN-split.md` / `RESULT-split.md`)的终态之上, 把汇编逐字节等价闸门放宽为"归一化 diff + 行为三闸门", 解除字节冻结造成的全部忠实层偏差, 使 parse.c 达到"表达式层无类型标注, 无名字绑定, 无常量求值"的第 3 层形态(AGENTS.md 目标架构). 本线仍属路线图阶段 3 + 4 的收尾; CST/trivia(阶段 1-2)与库化(阶段 5)在本线完成后由用户拍板.

**已拍板(2026-09-18, 用户决定)**: 常量求值不放在 parse 阶段 - parse 侧不保留任何 eval / const_expr / is_const_expr 调用; declarator 层允许构建"待补全"的类型记录(维度表达式/typeof 操作数等挂载体, 由 sema 补全), 但不做求值与判定.

新会话恢复方法: 通读 AGENTS.md -> 查看本文件勾选状态 -> `git log --oneline` 确认最后完成的步骤 -> 从第一个未勾选项继续; 开工前先跑 R0.3 的基线复验确认全绿.

行号一律以函数名为准(拆分线终态 02e5c13 的行号会随本线漂移). 与归档基线的对比锚点见"终态验收".

## 阀门口径与纪律

1. **闸门两级口径**: 常规步骤仍要求汇编快照逐字节 diff 为空; 标注 **[重组]** 的步骤允许汇编字节变化, 改以"归一化 diff 为空 + 行为三闸门全绿"为准, 并在该提交内重置 raw 快照基线(`make docker-snapshot`), 提交说明声明"含预期字节变化".
2. **行为三闸门**: `make docker-test`(test/*.c + driver.sh + 自举重跑), `make docker-test-thirdparty THIRDPARTY=tinycc`, R0.1 的诊断锁定测试. 三者构成行为面; 归一化 diff 构成形状面.
3. **codegen.c 维持零改动红线**: 语句链整形与泛型选择等用 sema 改写方案, 不改 gen_stmt.
4. **诊断规范**: 错误文案与插入符以 R0.1 锁定的行为为准; 时机后移只要"文案 + 锚点"不变即接受, 变化必须在该步偏差记录中说明并经用户确认. R2 各步移动检查时, 锚点经节点新增的 token 字段原样传递.
5. 一步一提交, 返工 = revert 单个提交. 本线不做(仍属库化阶段): 错误回调化, context 对象, 公共/内部头拆分; static 全局允许在 parse/sema 之间重新归属, 不做 context 化.
6. 实现要点分工: R2.1-R2.5 的两段式/忠实化是本线的主要**新**逻辑; R2.6 起以"删调用 + 改遍历"为主 - sema 的表达式降级 case(缩放/elvis/字符串/INCDEC/ASSIGN/FUNCALL 等)在拆分线已全部就位, 本线只是把触发时机从"构造现场"改为"遍历现场".
7. **常量求值清零口径**: R2 完成后 parse.c 不得出现 eval/eval2/const_expr/is_const_expr 调用; 检查方式: grep 计数留档 RESULT.md.

## 执行与汇报流程

- 按编号连续推进, **不等待用户逐步确认**; 汇报完一步, 自动开启下一步.
- 每步汇报固定四项: 改了什么 / 为什么改(在收尾目标中的位置) / 测试结果(三闸门 + diff 口径) / 偏差说明.
- 每步完成即勾选本文件对应 checkbox 并提交.

## R0 基线与闸门改造

- [x] **R0.1 诊断锁定测试**: 新增 test/diagnostic.c(或等价脚本), 对全部错误路径断言"stderr 文案 + 插入符位置"(编译失败输出逐字节比对), 先锁定拆分线终态行为; 4.2c 的两处新锚点(`too many arguments` 锚调用右括号, 块域 `void x = <初始化器>;` 报错延后)借此**拍板为规范**. 覆盖目标: parse.c 33 处 error_tok 全部 + sema.c 判定表 E 的 10 处代表性样例. 此测试此后每步必跑; R2 移动检查时同步更新期望锚点(仅当"文案 + 锚点"保真).
- [x] **R0.2 归一化 diff 目标**: Makefile 新增目标, 对 .s 规范化后 diff: `.L..N` 标签按首现顺序重编号(吃掉标签分配重排), 负 rbp 局部偏移按每函数首现顺序映射为序号(吃掉 lvar 顺序重排), `.loc`/`.file` 行折叠(吃掉语句链整形). raw diff(`docker-snapshot-diff`)保留不动. 验收: 对当前 HEAD, raw 与归一化 diff 均为空.
- [x] **R0.3 基线复验**: `make docker-test` + `make docker-test-thirdparty THIRDPARTY=tinycc` + raw/归一化 diff 双空 + 诊断测试全绿, 四项留档于 RESULT.md 作为本线基线记录.

## R1 控制流出 parse [重组]

- [x] **R1.1 标签分配移 sema**: ND_WHILE/ND_DO/ND_FOR/ND_SWITCH 的降级在 sema 分配 brk_label/cont_label; ND_LABEL/ND_LABEL_VAL 的 unique_label 同步移入; `new_unique_name` 计数器整体归 sema(parse 不再触碰 - 字符串物化与 static 局部匿名名已在此侧). 解锁拆分线 1.10 偏差的"计数器交织".
- [x] **R1.2 绑定与 stray 检查移 sema**: analyze 的语句下降是前序的(先循环后体), 维护"当前循环/switch"上下文, ND_BREAK/ND_CONTINUE 由 sema 填绑定目标; stray break/continue/case/default 四处检查随迁, **拆分线判定表 B 清空**; parse 删除 current_switch/brk_label/cont_label/gotos/labels 五个 static 与 resolve_goto_labels 壳(gotos/labels 清单归 sema).

## R2 表达式层语义全部后置 [重组, 最大阶段]

前置说明: R2.6 的"拆构造现场标注"要能落地, 前提是 parse 侧所有消费类型/常量/名字的决策点先行两段式化 - 即 R2.1-R2.5; 每步独立可绿(构造现场标注暂留, 被改造的决策点改为事后补齐), 最后 R2.6 一次性拆除.

- [x] **R2.1 成员访问两段式**: parse 的 `.`/`->` 不再查表, ND_MEMBER 只带成员名 token 与 is_arrow(字段已有); sema 的 ND_MEMBER case 解析成员链(含匿名成员展平), 三类校验("invalid pointer dereference"/"void pointer"/"not a struct nor a union")与 "no such member" 随迁, 锚定成员名 token, 并补插 DEREF. 拆分线判定表 C 的 struct_ref 5 处清空.
- [x] **R2.2 泛型选择与 builtin 折叠忠实化**: `_Generic` 改发忠实节点(controlling 表达式 + assoc 列表, 新增 NodeKind 或复用载体), 选择逻辑移 sema(判定表 D 第 3 处清); `__builtin_types_compatible_p`/`__builtin_reg_class` 的折叠移 sema(清拆分线 1.9 返工点). parse 侧对应的 add_type 消费点消失.
- [x] **R2.3 常量求值出 parse(第一批)**: 数组维度与 VLA 分类, case 的 begin/end(含 "empty case range" 检查), `aligned`/`_Alignas` 值, `typeof(expr)` 全部改记表达式节点由 sema 求值/判定后回填 - 表示载体: Type 加维度/typeof 表达式字段, VarAttr/ND_CASE 加表达式字段(命名实施时定); `array_dimension_type` oracle 消失; "variable-sized object may not be initialized" 移 ND_DECL 降级. 拆分线判定表 D 第 1/2 处清空. 位域宽度同类, 但因布局时序归 R2.5.
- [x] **R2.4 初始化器忠实化(最重步)**: designator(数组下标常量求值 + 成员名解析), brace elision, 定位与越界检查全部移 sema; Initializer 改为忠实 brace 记录(元素序列 + designator 表达式记录), sema 在降级时定位铺开. 拆分线判定表 C 的初始化器 6 处清空; parse 侧初始化器常量求值消失. 本步同时消除 parse 对结构 size 的初始化器消费, 为 R2.5 铺路.
- [x] **R2.5 位域宽度记录与布局后置**: Member 加宽度表达式字段, 位域宽度不再现算; `struct_decl/union_decl` 的布局调用点从解析现场移入 sema 遍历(反悔拆分线 4.2a 的"调用点仍在解析现场"), 嵌套结构布局顺序由 resolve 顺序保证.
- [x] **R2.6 拆构造现场标注 + resolve 前序遍历(原子收官)**: 删除 parse 全部构造现场 add_type/降级调用(拆分线 1.4/1.8/2.3/4.2c 时序原则作废); sema 改两遍 - 前序 **resolve 遍历**(作用域栈由树结构给出: ND_BLOCK push/pop, ND_FOR 的 init 声明独立成域)绑定 ND_IDENT, 求值并登记枚举常量(add_enum_const 的调用时机从解析现场迁入), 解析求值 R2.3/R2.5 挂到 Type/Member/ND_CASE 的 stash 表达式, 完成布局与类型补全; 再走标注 + 降级遍历. 拆分线 3.2b 原设计("树结构给出作用域 + 遍历绑定")就此落地. "undefined variable" 等报告时机后移, 锚定 ND_IDENT.tok, 文案不变.
- [x] **R2.7 typedef/tag 影子作用域**: parse 自持仅 typedef/tag 的作用域栈(C 词法 hack oracle, 永久保留), `find_typedef`/`find_tag`/`find_current_tag`/`push_tag_scope`/`enter_scope`/`leave_scope` 变 parse 私有; sema 的变量/枚举作用域由 resolve 遍历自管, 拆分线 4.2b 的"共享表"解体.
- [x] **R2.8 隐藏变量创建归 sema**: 复合字面量的域判定与 new_lvar/new_anon_gvar 移 ND_COMPOUND_LITERAL case(`in_file_scope` oracle 消失, 域由 resolve 上下文给出); 块域 static 的匿名全局创建改由 ND_DECL 降级路径完成(清算拆分线 2.3/3.4a 偏差).
- [ ] **R2.9 op 字段双职解除**: add_type 对 ND_ADD/ND_SUB 统一处理, "已缩放"标记机制删除, p-n 与 p-=n 的 conv/no-op cast 序列统一化(关闭拆分线 1.7 开放点).
- [ ] **R2.10 杂项归位**: return 隐式 cast 移 sema 的 ND_RETURN case(消除 parse 对 current_fn->ty 的最后依赖); current_fn static, `fn->locals = get_locals()`, builtin_alloca 归属收拾; parse 的函数语义残留清零.

## R3 语句链整形 [重组, 依赖 R0.2 的 .loc 折叠]

- [ ] **R3.1 声明链自然化**: declaration() 恢复外层 ND_BLOCK 包装(反悔拆分线 2.1 偏差), ND_DECL 锚点按新规范自定义并在 RESULT.md 记录, for-init 同步.
- [ ] **R3.2 VLA 前缀删除**: EXPR_STMT(NULL_EXPR) 兄弟语句消失, compute_vla_size 调用移入 sema 的 ND_DECL case(反悔拆分线 2.2 偏差).
- [ ] **R3.3 声明节点入链**: ND_TYPEDEF/ND_ENUM_CONST 进语句链并天然携带所在 ND_BLOCK(反悔拆分线 3.2a/3.4b 偏差; ND_ENUM_CONST 携带显式值表达式, 求值已在 R2.6 迁 sema); sema 在 analyze 时从链上**摘除**它们(codegen 零改动维持); scope_decls/add_scope_decl/get_scope_decls 三件套删除.

## R4 层 3 补课与文档收尾(与阀门无关, 可随时插队)

- [ ] **R4.1 隐式 cast 标记**: Node 加 is_implicit(或等价字段), parse 的显式 cast 与 sema 插入的隐式 cast 区分(AGENTS 层 3 承诺"隐式 cast 有标记"兑现; codegen 不读该字段, 字节不变, 常规口径).
- [ ] **R4.2 文档同步**: AGENTS.md 更新 - NodeKind 清单补 ND_IDENT/ND_TYPEDEF 及 R2.2 新增 kind, 硬性规则段的 static 清单更新, "现状与代码地图"的 parse.c/sema.c 描述重写("解析器只保留 typedef/tag 分类 oracle"的表述扩为完整第 3 层形态); 判定表 B/C/D 全清, 仅 A 保留的事实与时序原则/.loc 原则的废止在 RESULT.md 留档.

## 终态验收

- **parse.c**: 拆分线判定表 A 全保留(15 处), B/C/D 全清; current_fn/gotos/labels/brk_label/cont_label/current_switch/builtin_alloca 等 static 变量归零(仅剩 static 函数); 表达式构造路径无 add_type, 无名字绑定, 无常量求值(grep 口径见纪律 7) - 第 3 层"忠实, 无类型"成立; parse 侧仅剩 typedef/tag 分类 oracle 与声明符层"待补全"类型构建.
- **sema.c**: resolve 遍历 + 标注/降级遍历两段式; 标签分配, 全部隐藏变量创建, 结构布局, 常量求值, 泛型选择/初始化器定位归 sema.
- **codegen.c** 对 5f53ed0 仍零 diff.
- **四项闸门**: 行为三闸门全绿 + 归一化 diff 为空; raw 快照基线为本线终态的重置版.
- **对比锚点**(对 RESULT-split.md 末节, 本线完成后逐项填写): parse.c 2531 -> 预期 ~1900-2150; sema.c 1552 -> 预期 ~1850-2050; parse.c error_tok 33 -> **15**(判定表 A); chibicc.h 660 -> 随表达式载体字段微增; 拆分线"忠实层已知残留"清单(RNd 一节)清零; "错误诊断两处已知时机位移"节由 R0.1 测试接管后删除.
