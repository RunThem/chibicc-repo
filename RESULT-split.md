# RESULT-split.md - 语法语义拆分执行记录(已归档)

> 归档说明(2026-09-18): 本文件是已完成的语法语义拆分线的执行记录, 现行执行记录见 `RESULT.md`. 文中的行数/error_tok 计数/判定表 A-E/终态验收节, 是拆分线终态(02e5c13)的量化基线, 供"忠实层收尾"线完成后逐项对比.
>
> 注记(2026-09-21, 收尾线 R4.2): 收尾线已完成, 逐项对比见 `RESULT.md` 的 R4.2 与 `PLAN.md` 的"终态验收". 以当时口径写在本文中的三处已被后续步骤作废, 正文保留为历史记录, 不再反映现状 - 判定表 B/C/D 已清空(对应诊断全部移入 sema.c, parse.c 只剩判定表 A 的 15 处), 时序原则与 .loc 原则均已废止(parse 不再于构造现场调用降级; 语句链形状与锚点不再被 .loc 字节冻结), "错误诊断的两处已知时机位移"已由 `test/diagnostic.sh` 的 44 例诊断锁定测试接管.

以下为原文.

# 语法语义拆分执行记录

本文件记录 PLAN.md 各步骤的实际执行结果, 供审核与后续会话接续参考. 以下记录到 4.2 为止, P4(收尾)全部完成, 语法语义拆分线(路线图阶段 3 + 4)的 22 步执行清单至此走完.

基线: 上游 commit 5f53ed0 的快照建立于 0.1; 本轮从 1f24ab7 开始推进. 每步的三道闸门(make docker-test 含自举 / 汇编快照逐字节 diff / 行为测试)均须全绿后才提交.

## 提交一览

| 提交 | 步骤 | 内容 |
|---|---|---|
| affaaf0 | 0.1 | 快照脚本 make docker-snapshot / docker-snapshot-diff |
| e1af16f | 0.2 | 建 sema.c(纯搬家) |
| 94f5e83 | 1.1 | ND_GT/ND_GE |
| 5154003 | 1.2 | elvis(is_elvis) |
| 5dd9e75 | 1.3a | ND_ASSIGN.op 复合赋值(+= -= 与 to_assign 搬家) |
| 4e30338 | 1.3b | ND_ASSIGN.op 复合赋值(其余 8 个 op=) |
| df196cf | 1.4 | ND_INCDEC |
| b57f2d6 | 1.5 | ND_SUBSCRIPT |
| 3fba1f0 | 1.6 | is_arrow |
| 295eb6f | 1.7 | 裸指针算术降级(new_add/new_sub 搬 sema) |
| 95849ac | 1.8 | ND_STRING |
| f8f14e5 | 1.9 | ND_SIZEOF/ND_ALIGNOF(compute_vla_size 搬 sema) |
| cb409be | 1.10 | ND_WHILE/ND_BREAK/ND_CONTINUE |
| 0e779ab | 2.1 | ND_DECL(init 降级函数搬 sema) |
| e90327a | 2.2 | VLA 忠实化(alloca 降级) |
| 539dea8 | 2.3 | ND_COMPOUND_LITERAL |
| (本轮) | 3.1 | locals/globals 清单持有权移 sema.c |
| (本轮) | 3.2a | ND_TYPEDEF 声明记录 |
| (本轮) | 3.2b | ND_IDENT 标识符翻转(bind_ident 移 sema) |
| (本轮) | 3.3 | begin_function 函数语义搬家 |
| (本轮) | 3.4a | resolve_labels/finalize_globals/array_dimension_type/declare_static_local 移 sema |
| (本轮) | 3.4b | 枚举忠实化(ND_ENUM_CONST) |
| (本轮) | 4.1 | 检查归位盘点(纯文档, 无代码变更) |
| (本轮) | 4.2a | 结构体/联合体布局计算移 sema(layout_struct/layout_union) |
| (本轮) | 4.2b | 作用域表与函数声明(redefinition)移 sema |
| (本轮) | 4.2c | 函数调用降级(lower_funcall)与残余类型检查移 sema |

## 各步详情

### 0.1 汇编快照脚本 (affaaf0)

- 改了什么: Makefile 新增 docker-snapshot(重新生成基线)与 docker-snapshot-diff(重新生成 + 逐字节 diff)两个目标. 快照在 amd64 容器内构建 chibicc 后对全部 41 个 test/*.c 跑 -S, 以 tar 流回宿主机解包到 .cache/snapshot/; diff 目标解包到 .cache/.snaptmp 再与基线比较. 产物目录已被 .gitignore 的 /.cache 覆盖, 无需新增忽略项.
- 为什么: 拆分线的等价性闸门, 之后每步结束跑 docker-snapshot-diff 验证汇编逐字节不变.
- 测试结果: 基线 41 个文件生成成功, 连续两次 diff 均为 empty.
- 偏差: 快照生成时用 -D 把 __TIMESTAMP__/__DATE__/__TIME__ 钉成固定值(Mon Jan  1 00:00:00 2024 等) — test/macro.c 会把时钟值编进字符串字面量, 不钉住则每次 diff 必非空. 固定值长度与内置宏一致(11/8/24 字符), 不影响 macro.c 的 strlen 断言, 行为测试不受影响. 调试中还发现 diff 目标初版会先用新快照覆盖基线目录再改名, 导致"无基线"误报, 已改为先解包到独立临时目录.

### 0.2 建 sema.c (e1af16f)

- 改了什么: 新文件 sema.c; type.c 的 get_common_type/usual_arith_conv/add_type 与 parse.c 的 eval 全家(eval/eval2/eval_rval/is_const_expr/const_expr/eval_double)原样搬入; 14 个节点/变量构造器(new_node/new_binary/new_unary/new_num/new_long/new_ulong/new_var_node/new_vla_ptr/new_cast/new_lvar/new_gvar/new_anon_gvar/new_string_literal/new_unique_name)去掉 static, 声明加入 chibicc.h; conditional 提升为跨文件(const_expr 需要); static 清单(locals/globals/scope 等)仍留 parse.c.
- 为什么: 把"语义分析"类别从解析器里物理分离出来, 建立后续降级逻辑的落点.
- 测试结果: make docker-test 全绿, 快照 diff 为空.
- 偏差: eval/eval2/eval_double/is_const_expr 也要进 chibicc.h — 计划只点名了 const_expr, 但 write_gvar_data 与数组维度处仍在直接调用它们.

### 1.1 ND_GT/ND_GE (94f5e83)

- 改了什么: NodeKind 新增 ND_GT/ND_GE; relational() 的 > 和 >= 不再交换操作数, 直接发新 kind; add_type 的比较 case 组里对 ND_GT/ND_GE 交换操作数并改写回 ND_LT/ND_LE(就地, fallthrough 到 usual_arith_conv), 产出与旧树逐节点一致.
- 为什么: 忠实化关系运算符的源码写法, 交换降级移入 sema.
- 测试结果: docker-test 全绿, 快照 diff 为空.
- 偏差: 无. (调试中曾把 ND_EQ/ND_NE 误并进带交换逻辑的 case 组导致 42==42 失败, 已分离.)

### 1.2 elvis (5154003)

- 改了什么: Node 新增 bool is_elvis; conditional() 的 GNU a ?: b 分支只发 ND_COND{is_elvis, cond=a, els=b}; sema 在 ND_COND case 对 is_elvis 节点就地展开为 tmp = a, tmp ? tmp : b(节点自身改写成 ND_COMMA, 复用其标注路径), 临时 lvar 与旧版同类同名.
- 为什么: 语法层保留 elvis 的原始写法, 临时变量展开降级到 sema.
- 测试结果: docker-test 全绿, 快照 diff 为空.
- 偏差: 无(时序加固在 1.4 一并做了, 见下).

### 1.3 复合赋值 (5dd9e75 + 4e30338)

- 改了什么: Node 新增 NodeKind op 字段(0 表示纯 =); to_assign 整体搬入 sema.c, member/atomic/plain 三分支结构原样保留, 由 node->op 驱动; assign() 的 10 个 op= 分支全部改发 ND_ASSIGN{op}(不再经 new_add/new_sub). 第一个提交切 += -= 与 to_assign 搬家, 第二个提交切其余 8 个并删除过渡兼容层.
- 为什么: 忠实化复合赋值(树里不再预建缩放后的算术节点), 缩放与三分支展开降级到 sema.
- 测试结果: 两个提交各自 docker-test 全绿, 快照 diff 为空.
- 偏差: 前缀 ++/-- 和 new_inc_dec 也在第一提交就切到了 ND_ASSIGN{op} 形状(计划只提 assign() 的 10 个分支) — to_assign 搬家后需要统一输入契约, 旧形状(裸算术节点)与新形状无法共存, 过渡兼容层在第二提交删除.
- 实现注记: sema 的 compound_op 对 +=/-= 经 new_add/new_sub 拿到缩放后的 rhs, 再用 new_binary 重建一个"未标注的新鲜节点"挂在 expr3/*tmp 左侧 — 这是复现旧输出的关键(旧 to_assign 的 new_binary(binary->kind, ...) 产物未标注, 后续 usual_arith_conv 会插入两个 no-op CAST; 直接复用 new_sub 的预标注节点会丢这两个 CAST, 快照随即漂移).

### 1.4 ND_INCDEC (df196cf)

- 改了什么: NodeKind 新增 ND_INCDEC, Node 新增 is_post/addend; 前缀(unary)与后缀(postfix)改发忠实节点; new_inc_dec 搬入 sema 作降级 — 后缀降为"加后补偿减再 cast"(new_inc_dec 原样), 前缀降为 to_assign(--i 用 ND_SUB{1}, i-- 用 ND_ADD{-1}, 与旧代码逐分支一致), 均就地改写节点.
- 为什么: 忠实化自增/自减的写法与前后缀区分, 展开降级到 sema.
- 测试结果: docker-test 全绿, 快照 diff 为空.
- 偏差(重要): parse 在构造现场立即调用 add_type 触发降级, 而不是等语句级 add_type. 原因: 降级会创建临时 lvar, 若推迟到语句末尾, lvar 创建顺序相对旧代码翻转, 栈偏移(汇编字节)随之改变 — 实测 atomic.s 的 for 循环 i++ 处漂移. elvis 因此同步加固为构造现场立即降级. 该时序原则预计对后续所有"降级会建 lvar"的节点都适用(如 2.3 复合字面量).
- 事故记录: 本步调试中, 用 sed 删 new_inc_dec 时误删了 struct_ref 的 return node; 与收尾括号, 造成越界返回垃圾 — macOS 上无症状, Linux 容器内 alignof/attribute/bitfield/builtin 四个测试静默失败. 已修复; 后续搬家一律用 Edit 工具而非按行 sed.

### 1.5 ND_SUBSCRIPT (b57f2d6)

- 改了什么: NodeKind 新增 ND_SUBSCRIPT; postfix() 的 [ 与 init_desg_expr 的数组下标分支改发(不再预建 *(x+y)); sema 在 add_type 就地降回 DEREF(ADD) — 经 new_add, 指针缩放与 1[p] 规范化与旧一致.
- 为什么: 忠实化下标写法, 缩放降级到 sema.
- 测试结果: docker-test 全绿, 快照 diff 为空.
- 偏差: 无.

### 1.6 is_arrow (3fba1f0)

- 改了什么: Node 新增 bool is_arrow; postfix() 的 -> 不再偷插 DEREF, struct_ref 增加箭头参数, 最外层 ND_MEMBER 带 is_arrow(匿名成员链的内层不带); sema 在 ND_MEMBER case 补插 DEREF 并标注. 结构校验移入 parse 的 struct_ref: 非指针("invalid pointer dereference")/void 指针("dereferencing a void pointer")/指向非结构体("not a struct nor a union")三类错误文案与锚点(箭头 token)与旧版逐字一致.
- 为什么: 忠实化 . 与 -> 的区分, DEREF 补插降级到 sema.
- 测试结果: docker-test 全绿, 快照 diff 为空; 错误文案用 stash 前后对比验证一致.
- 偏差: 无.

### 1.7 裸指针算术 (295eb6f)

- 改了什么: add() 直接发裸 ND_ADD/ND_SUB; new_add/new_sub 定义搬入 sema.c(static), 由 add_type 在 ND_ADD/ND_SUB case 首次标注时就地降级(缩放乘法/num+ptr 规范化/ptr-ptr 除法, 逐分支与今天一致), 随后做 usual arithmetic conversions; "invalid operands" 错误随之移入 sema, 仍锚定运算符 token.
- 为什么: 这是 P1 最后一块"parse 预降级" — 至此 add/sub/下标/复合赋值/自增全部走 sema 的统一降级路径.
- 测试结果: docker-test 全绿, 快照 diff 为空.
- 偏差: 无方案级偏差. 实现关键: 已缩放节点(复合赋值/下标/自增降级的产物)用非零 op 字段标记, add_type 看到 op != 0 就只做 conv 不再缩放. 原因是旧代码存在一个字节级不对称: 二进制 p-n(经 new_sub, 预标注)不走 conv, 而复合 p-=n(to_assign 里 new_binary 的新鲜节点)走 conv 并插入 no-op CAST — 必须分别复现. 共修了三处缺标记/错标记: splice 后新 MUL 子树未标注, a[i] 的 ADD 缺标记, new_inc_dec 补偿减法的 ADD 缺标记.

### 1.8 ND_STRING (95849ac)

- 改了什么: NodeKind 新增 ND_STRING; primary() 的 TK_STR 分支改发 ND_STRING{tok}(不再现场建匿名全局); sema 的 add_type 新增 ND_STRING case, 调用既有 new_string_literal 建匿名全局并就地改写为 ND_VAR. __func__/__FUNCTION__ 仍在 parse 期经 new_string_literal 建全局(随 3.3 搬家).
- 为什么: 字符串字面量的"匿名全局物化"是 parse 侧最后一批预降级之一; 忠实层保留字面量(token 即内容与类型), 物化降级到 sema.
- 测试结果: docker-test 全绿, 快照 diff 为空.
- 偏差: 无. 关键时序沿用 1.4 原则: parse 在构造现场立即 add_type 触发降级, 匿名全局创建点与旧代码逐点一致 — 该全局与标签共用 new_unique_name 计数器, 创建时机漂移会改变 .L..N 编号进而破坏快照.

### 1.9 ND_SIZEOF/ND_ALIGNOF (f8f14e5)

- 改了什么: NodeKind 新增 ND_SIZEOF/ND_ALIGNOF, Node 新增 Type *ty_op; primary() 四个分支(sizeof(type)/sizeof expr/_Alignof(type)/_Alignof expr)改发 — ty_op 载操作数类型, expr 形式的未求值操作数挂 lhs; compute_vla_size 原样搬 sema(去 static, chibicc.h 声明, declaration() 照旧跨文件调用); sema 折叠: 定长 -> new_ulong(ty->size / ty->align), VLA -> vla_size 引用(类型尚未算过尺寸则先经 compute_vla_size 建 vla_size lvar 并包 comma, 即旧 sizeof(int[n]) 的展开形状). __builtin_types_compatible_p/__builtin_reg_class 折叠暂留 parse(返工点).
- 为什么: sizeof/_Alignof 的常量折叠与 VLA 运行时尺寸计算属于语义; parse 只剩"操作数是什么"的忠实记录.
- 测试结果: docker-test 全绿, 快照 diff 为空.
- 偏差: (1) sizeof expr 形式遇到 vla_size 未算过的 VLA 类型时, 新代码就地计算, 旧代码在此为空指针解引用 — 该路径实际不可达(VLA 类型的 vla_size 总由声明处的 compute_vla_size 预先算好), 属顺带加固. (2) VLA comma 展开中合成节点的锚定 token 由旧的 ")" 换成 sizeof 关键字 token, 仅影响错误信息锚点, 无行为影响. 折叠就地改写节点并清空 lhs/ty_op, 树形与旧 ND_NUM/ND_VAR/ND_COMMA 逐节点一致; comma 情形 folded->ty 为空, 由下一次 add_type 走 ND_COMMA case 补齐(与旧代码相同).

### 1.10 ND_WHILE/ND_BREAK/ND_CONTINUE (cb409be)

- 改了什么: NodeKind 新增 ND_WHILE/ND_BREAK/ND_CONTINUE; stmt() 的 while 改发 ND_WHILE{cond, then}(不再直接发 ND_FOR), break/continue 改发忠实节点; sema 新增三个降级 case: ND_WHILE 就地改 kind 回 ND_FOR, ND_BREAK/ND_CONTINUE 就地改 kind 回 ND_GOTO. "stray break/continue" 检查暂留 parse(返工点), switch 仍占用 brk_label static.
- 为什么: while 不再在 parse 期冒充 for, break/continue 不再在 parse 期改写为 goto — 控制流的降级归 sema.
- 测试结果: docker-test 全绿, 快照 diff 为空.
- 偏差(重要): 计划写"sema 降回 ND_FOR(带合成 brk/cont 标签)", 实际标签仍在 parse 期分配(挂节点 brk_label/cont_label 字段), break/continue 也在 parse 期把绑定目标记到 unique_label 字段, sema 只改 kind. 原因: (1) add_type 自底向上, 到达 ND_BREAK 时外层循环尚未降级, sema 无从得知绑定目标 — break 绑定 switch 的情形也因此走同一路径; (2) 标签与字符串匿名全局共用 new_unique_name 计数器, 分配时机后移会改变 .L..N 编号交织, 快照即漂移. ND_WHILE 的忠实性体现在形状(cond + 体, 无 init/inc), 标签属辅助元数据; break 绑定 switch 的细节随 switch 忠实化(4.x)返工.

### 2.1 ND_DECL (0e779ab)

- 改了什么: NodeKind 新增 ND_DECL, Node 新增 Initializer *decl_init 字段; Initializer/InitDesg 两个结构搬入 chibicc.h; declaration() 每个声明符发一个 ND_DECL — 带初始值时 ND_DECL{var, decl_init}(decl_init 为 initializer() 建好的初始化器树, 其 tok 字段记初始化器首 token), 不再拍平 MEMZERO + comma 链; 无初始值时 ND_DECL{var, lhs = vla-size 树}. init_desg_expr/create_lvar_init 原样搬 sema; lvar_initializer 拆分: 解析半部由 parse 直接调 initializer(), 降级半部成为 sema 的 lvar_init_comma; gvar_initializer/write_gvar_data/read_buf/write_buf 搬 sema, gvar_initializer 拆出 gvar_init_data 供内部复用; initializer() 去 static 跨文件. 树上构造器全家(initializer2/array/struct/union/string_initializer 等)与 static 局部分支留在 parse(3.4 返工).
- 为什么: "初始化器拍平"是 parse 侧最后一批预降级之一; 忠实层保留声明节点(变量 + 初始化器树), 展开降级到 sema.
- 测试结果: docker-test 全绿, 快照 diff 为空.
- 偏差(重要): (1) 计划的"外层 ND_BLOCK 包装取消"未执行 — gen_stmt 对每个语句节点(含 ND_BLOCK)都先按 node->tok 打一行 .loc, 包装取消或 ND_DECL 降级出额外空语句都会改变 .loc 行序列, 字节闸门否决. 因此 declaration() 返回形状不变, for-init 同步免改; ND_DECL 占据旧 ES(init) 的链位(节点锚点 = 初始化器结束后的 token, 与旧 ES 一致), 无初始值时降级为旧 ES(vs) 语句(锚点 = 声明符后 token). (2) compute_vla_size 仍在 parse 期调用, 保证 vla_size lvar 创建先于被声明变量(指针指向 VLA 的声明依赖此顺序). (3) Initializer.tok 原是无用的残留字段, 现用来携带初始化器首 token 供降级锚定. (4) static 局部声明暂不发 ND_DECL(不进树, 3.4 翻转).

### 2.2 VLA 忠实化 (e90327a)

- 改了什么: declaration() 的 VLA 分支改发 ND_DECL{var}, 节点锚点 = ty->name; sema 的 ND_DECL case 新增 VLA 路径, 生成 `x = alloca(<size>)`(new_vla_ptr + new_alloca, 逐节点与旧 parse 产物一致); new_alloca 去 static 加 chibicc.h 声明(留在 parse.c, 属构造器 — extern 化 builtin_alloca 会与 codegen.c 的同名 static 函数冲突).
- 为什么: VLA 的 alloca 展开属降级; 忠实层只保留"此处声明了变量 x".
- 测试结果: docker-test 全绿, 快照 diff 为空.
- 偏差: "EXPR_STMT(NULL_EXPR) 前缀消失"未达成 — 该语句自身也是 .loc 字节输出的一部分, 只能保留为 parse 侧兄弟语句(compute_vla_size 的调用也必须留在 parse, 理由同 2.1 偏差(2)); 由 sema 生成的仅 alloca 赋值语句. array_dimensions 的 const_expr 判定按计划暂留 parse.

### 2.3 ND_COMPOUND_LITERAL (539dea8)

- 改了什么: NodeKind 新增 ND_COMPOUND_LITERAL; postfix() 的复合字面量分支改发 ND_COMPOUND_LITERAL{var, decl_init}, 锚点 = `(`; 域判定(scope->next == NULL)与隐藏变量创建留在 parse(块域 new_lvar, 文件域 new_anon_gvar), 初始化器树经 initializer() 直接建好挂 decl_init. sema 新增 case: 块域降级为 comma(MEMZERO + comma 链 @ init->tok, var 引用 @ `(`), 文件域调 gvar_init_data 后就地改写为 ND_VAR @ `(`; lvar_initializer 失去最后调用方, 删除(chibicc.h 同步).
- 为什么: 复合字面量的"隐藏变量物化 + 初始化展开"降级到 sema; 忠实层保留 `(type){...}` 写法本身.
- 测试结果: docker-test 全绿, 快照 diff 为空.
- 偏差: 计划写"sema 建隐藏 lvar(块内)或匿名全局(文件域)", 实际变量创建仍在 parse — 创建时序后移会翻转它与初始化器内临时变量(elvis/字符串匿名全局等)的分配顺序, 栈偏移即变. 唯一 token 锚点变化: 降级后 rhs 的 var 引用从"初始化器后 token"改为 `(` — 表达式无 .loc, 无错误文案依赖该锚点, 汇编不受影响.

### 3.1 清单持有权 (本轮)

- 改了什么: `locals`/`globals` 两个 static 列表与 `new_var`/`new_lvar`/`new_gvar` 三个构造器从 parse.c 移入 sema.c; 新增四个访问器 `get_locals/set_locals/get_globals/set_globals` 供 parse 侧读/重置列表(function() 的复位与 `fn->params`/`fn->locals` 取值, scan_globals 的遍历与改写, parse() 的复位/遍历/返回值); parse.c 新增 `push_var_scope(name, var)` 回调 - sema 建变量时仍借用 parse 的作用域表登记名字.
- 为什么: 变量清单是名字解析的载体, 先把它交给 sema, 3.2b 的解析作用域才有落点. parse 侧净减 43 行, 对清单只剩读写访问器调用.
- 测试结果: docker-test 全绿(含自举, 分钟级), 快照 diff 为空, 本机构建仅剩既有 codegen 格式告警.
- 偏差: 计划写"parse 经由已跨文件的 new_lvar 等间接使用", 实际 parse 仍需直接读/重置列表(function() 与 parse() 两处), 故保留四个访问器而非纯间接调用; `new_var` 的作用域登记以 `push_var_scope` 回调形式留在 parse(该调用随 4.2 删除). 属实现形态调整, 无行为差异.

### 3.2a ND_TYPEDEF 节点 (本轮)

- 改了什么: NodeKind 新增 ND_TYPEDEF; parse_typedef 每个声明符构造一个 ND_TYPEDEF 节点(tok = 声明符名字 token, ty = 被声明的类型), 经 sema 新增的 `add_typedef` 按源码顺序追加到 sema 持有的 `typedefs` 链(配 `get_typedefs` 读取); parse 侧 `push_scope(...)->type_def = ty` 的 oracle 登记原样保留.
- 为什么: 忠实层要能表达"此处有一个 typedef 声明(名字 + Type)", 供 sema 重建作用域取代 oracle(4.2). P3.2b 的 resolve 遍历仍以 oracle 判定 typedef, 本步只做记录.
- 测试结果: docker-test 全绿(含自举), 快照 diff 为空; 本机 `-S` 对含块域 typedef 的小样例逐字节不变. 记录内容用临时 fprintf 验证: test/typedef.c 产出 8 条, 名字与 Type kind(4=int/12=array/14=struct)均正确, 文件域与块域都在列, 顺序为源码序; 验证后已移除该临时代码.
- 偏差(重要): 计划写"发声明形状节点", 节点实际**不进语句链**, 而是进 sema 持有的侧链. 原因: compound_stmt 对链上每个语句都调 add_type, 而 gen_stmt 对链上每个节点先打一行 .loc(codegen 零改动的硬约束), 块域 typedef 今天不产生任何节点, 一旦入链就多出一行 .loc, 快照闸门必红. 实测确认: 含块域 typedef 的函数体里, ND_BLOCK 的 .loc 已落在 typedef 所在行, 入链必然产生重复 .loc 行. 因此忠实层保留"声明记录", 承载在侧链而非树内 - 与 2.1/2.2 因 .loc 字节冻结而做的取舍同源. 作用域归属(哪条记录属于哪个块)当前不记录, 待 4.2 由 sema 重建作用域时按需扩展(届时节点可挂到所属 ND_BLOCK 或由 resolve 遍历顺序对齐).

### 3.2b ND_IDENT 标识符翻转 (本轮)

- 改了什么: NodeKind 新增 ND_IDENT; `primary()` 的 TK_IDENT 分支不再自己查表, 只发 ND_IDENT{tok}, 随即在同一现场调 `add_type(node)` 触发 sema 绑定; sema 新增 `bind_ident`(add_type 的 ND_IDENT case): 经 parse 新增的查询 `find_ident(tok, &enum_ty, &enum_val)` 拿结果 - 变量/函数 -> 就地改写为 ND_VAR 并 `ty = var->ty`; 枚举常量 -> 就地改写为 ND_NUM 并 `ty = ty_int`; 两者皆无 -> 保持原有两条错误("implicit declaration of a function" / "undefined variable", 同一 token). "static inline" 的 refs 收集随之移入 sema(经 parse 新增的 `get_current_fn()` 取当前函数), 语义与旧 primary 逐字一致. parse 侧 `find_var` 保留给 `find_typedef`(typedef 分类 oracle).
- 为什么: 语法期无法区分"变量/函数/枚举常量"的引用, 解析器不再做名字解析 - 查表判定、错误、refs 收集全部归 sema. 本步过后 parse 只保留 typedef/tag 分类用的查表.
- 测试结果: docker-test 全绿(含自举), 快照 diff 为空. 另做了行为对照(旧二进制 worktree 于 HEAD 编译后逐字节 diff): 覆盖 enum/typedef/块与 for 作用域遮蔽/while+switch+case 范围/复合赋值全家/条件表达式/字符串/复合字面量/VLA/sizeof/_Alignof/static inline 链(含 driver.sh 的 6 条 liveness 用例)/全局初始化器引用函数 的样例 -S 输出与 stderr 全等; 5 个非法样例(未定义变量/隐式声明/typedef 名当变量/`x + ;`/enum 变量)的错误文案与插入符位置全等.
- 偏差(重要, 与计划相反): 计划要求"发未解析名字节点"后由 sema 的 resolve **遍历**(块/for 作用域由树结构给出)统一绑定, 并预期错误触发时机后移; 实际实现是**在构造现场立即绑定**(primary -> add_type -> bind_ident), 未解析态只存在于构造的那一瞬间. 原因是不写死动态作用域就会出错: (1) 名字查找依赖 parse 的作用域栈, 而该栈是动态的 - `for (int i = 0; i < 3; i++)` 的 i 在 for 语句解析完就被 leave_scope 弹出, 而它的使用点要等到语句级 add_type 才被访问, 延迟绑定必然报 "undefined variable"(实测: 延迟版本在这条 for 上直接编译失败, 已回退); (2) 反之, 若把绑定提前到 leave_scope 前, 又要为 sizeof/自增自减/字符串等 parse 期 add_type 的构造点各补一次 resolve, 等于把作用域边界重新硬编码进 parse. 因此"树结构给出作用域 + 遍历绑定"要等 4.2 把 add_type 重构成 analyze() 单遍时才成立, 本步只把决策权交给 sema. 副产品: 错误触发时机与今天完全相同(比计划预期更保守, 无回归风险), `x + ;` 这类 doubly-invalid 的输入仍报原错误. sema 侧也**没有**自建作用域栈 - 它查询 parse 的 oracle(计划写的"经传递供 sema 查询"), 双重作用域条目因此尚未产生, 3.1/3.2a 的清单与记录是为 4.2 重建作用域准备的.

### 3.3 begin_function 函数语义搬家 (本轮)

- 改了什么: sema 新增 `begin_function(fn, ty)` - 收拢 function() 里的变量创建: `set_locals(NULL)`, 参数 lvar(原 create_param_lvars 原样搬入 sema, 改名不改体), 大 struct/union 返回值的隐藏缓冲, `fn->params = get_locals()`, variadic 的 `__va_area__`, `__alloca_size__`, 以及 `__func__`/`__FUNCTION__` 两个字符串字面量(创建走 new_string_literal, 名字登记改由既有回调 `push_var_scope` 完成). parse 的 function() 只剩: 声明符解析, redeclaration 诊断, new_gvar/属性, `current_fn = fn`, `enter_scope()`, 调 begin_function, skip("{"), 解 body, `fn->locals = get_locals()`, leave_scope, resolve_goto_labels. `get_ident` 去 static 进 chibicc.h(create_param_lvars 用它取参数名, 保持原来的 TK_IDENT 校验).
- 为什么: 函数定义的作用域与栈帧布局(参数/隐藏参数/va_area/alloca_bottom)属语义, parse 侧不再决定"这个函数需要哪些变量"; 顺序硬性保持 - lvar 与匿名全局的创建次序直接决定栈偏移与 .L..N 编号.
- 测试结果: docker-test 全绿(含自举), 快照 diff 为空. 行为对照同 3.2b(旧 HEAD 二进制 vs 新二进制): 4 个样例 -S 与 stderr 全等, 其中 d.c 专门覆盖变参(va_list/va_arg)、>16 字节 struct 返回值(隐藏缓冲)、__func__/__FUNCTION__、多参数调用.
- 偏差: (1) `fn->locals = get_locals()` 留在 parse 未随搬家 - 它只是把 sema 的清单读回来赋给 Obj, 与 3.1 的访问器用法同性质; (2) `current_fn` 仍留 parse: stmt() 的 return 分支要用 `current_fn->ty->return_ty` 插隐式 cast, 这处 parse 期语义残留未在 3.3 计划范围内, 记入 4.1 盘点; (3) `__func__` 登记从 `push_scope(name)->var = new_string_literal(...)` 改为 `push_var_scope(name, new_string_literal(...))`, 求值次序与作用域表内容不变(匿名名 ".L..N" 与 "__func__" 的入表顺序一致).

### 3.4a 清返工点(搬家三项) (本轮)

- 改了什么: (1) `resolve_labels(gotos, labels)` 进 sema - 匹配逻辑原样搬, parse 侧的 resolve_goto_labels 变成"调 sema + 清空两个 static 列表"的壳(表仍归 parse, 遵 PLAN 纪律 4"static 全局仅在 P3.1 做持有权搬家"); `mark_live`/`scan_globals` 搬 sema 并合并为 `finalize_globals()`, parse() 末尾只留一句调用; `find_func` 去 static 进 chibicc.h(sema 的 mark_live 要用, 与 find_ident 同属 oracle 类接口). (2) `array_dimension_type(base, expr)` 进 sema - array_dimensions 尾部的 `TY_VLA || !is_const_expr` 判定原样搬走, parse 只收集维度表达式. (3) `declare_static_local(name, ty)` 进 sema - 块域 static 变量的匿名全局创建 + 名字登记. 另: eval 全家(eval/eval2/eval_double/is_const_expr)在 parse 不再直接调用后全部收回 static(0.2 时因 parse 要用才提升), chibicc.h 相应删除四个声明, 文件内补前置声明.
- 为什么: 这四项都是 0.2-2.3 各步刻意留下的返工点; 至此 parse 侧只剩语法与 oracle 查询, 整单元的收尾(活跃函数标记/冗余 tentative 定义清理)与类型构建的语义判定都在 sema.
- 测试结果: docker-test 全绿(含自举), 快照 diff 为空. 行为对照: e.c 专门覆盖块域 static(含循环内 static 计数)、goto/标签(含 `&&label` 的 labels-as-values)、二维 VLA, 与旧二进制逐字节相同.
- 偏差: (1) `resolve_labels` 取参数而非把 gotos/labels 两个 static 搬进 sema - 遵纪律 4(持有权搬家只在 3.1 做), 清空动作因此留在 parse 的壳函数里; (2) 计划写"static 局部 gvar 创建移 sema(ND_DECL 降级时建匿名全局)", 实际是 `declare_static_local()` 在声明现场被调用, 匿名全局仍在 parse 期创建 - 若改由 ND_DECL 降级时创建, 就要为块域 static 声明新增一个语句节点, 而它今天不产生任何节点, 入链必然多一行 .loc(与 2.1/2.2/3.2a 同一条字节闸门约束); 创建时机也不能后移: 匿名名与 .L..N 计数器、globals 链顺序(反向)共同决定输出. 因此本步只把"创建 + 登记"的所有权交给 sema, 保持现场调用.

### 3.4b 枚举忠实化 (本轮)

- 改了什么: NodeKind 新增 ND_ENUM_CONST(名字 token + 可选显式值于 lhs + 求值结果于 val); enum_specifier 每个成员建一个节点(显式值用 conditional() 解析后挂 lhs, 不再由 parse 调 const_expr), 交给 sema 新增的 `add_enum_const(node, ty, &val)`: 有显式值则 `eval(lhs)`, 否则取运行值, 写回 node->val, 推进运行值, 经 parse 新增的 `push_enum_scope(name, ty, val)` 登记名字. 3.2a 的 typedef 记录链与枚举常量记录合并为一条"声明记录"链(sema 侧 `add_scope_decl`/`get_scope_decls`, 原 add_typedef/get_typedefs 更名), 记录按源码顺序串联, 供 4.2 重建作用域.
- 为什么: 枚举常量的"值求值 + 作用域登记"是语义, 语法层只需要忠实记下"这里声明了常量 X, 显式值是 expr"; 与 3.2a 的 typedef 记录合为一条链, 才能在 4.2 按源码顺序重放一个作用域里的全部引入名字.
- 测试结果: docker-test 全绿(含自举), 快照 diff 为空. 行为对照 f.c 覆盖: 隐式/显式值交替、显式值引用同枚举前面的成员(`W = X + Y`)、负值、文件域与块域枚举、枚举常量作数组维度与 case 标签、typedef 上的匿名 enum - 与旧二进制逐字节相同且 stderr 相同. 记录内容用临时 fprintf 验证(X=0/Y=1/Z=5/W=1/V=2/U=3/P=-1/Q=0/L1=3/L2=7/L3=8, 显式标记正确), 验证后已移除.
- 偏差: (1) 记录仍走 sema 侧链而非语句链, 理由同 3.2a(.loc 字节冻结); (2) 计划只写"发声明节点", 实际额外做了记录链的合并与更名 - 属实现形态调整, 无行为差异, 3.2a 记录中的接口名(add_typedef/get_typedefs)以此为准; (3) 枚举值的求值时机与今天完全一致(仍在解析该成员的那一刻, 因为后续成员与同一声明内的数组维度都依赖它), 未出现计划未预期的时机后移.

### 4.1 检查归位盘点 (本轮)

- 改了什么: 只改文档, 无代码变更. 逐条盘点了 parse.c 内全部 43 处 `error_tok` 与残余的类型依赖, 按"文法可判 / 解析器上下文 / 建树必需 / 需要类型"四类归档, 并对需要类型的 10 处给出 4.2 的执行清单; 核对 `test/driver.sh` 的 36 条断言与 `test/*.c`, 确认没有任何一处断言错误文案或失败退出码.
- 为什么: P4 是收官前的定位步; 4.2 要按本步判定执行(布局翻转 + 类型依赖清除), 判定先落成文字, 避免 4.2 边改边定. 判定标准: 能从 token/文法推出的留 parse; 需要类型、成员表或符号表才能判定的移 sema.
- 测试结果: 无代码变更. 开工前复验基线 `make docker-test` 全绿(含自举), `make docker-snapshot-diff` 为空.
- 偏差: 无.

#### 判定表 A: 留 parse - 文法/词法可判(15 处)

| 位置 | 文案 | 理由 |
|---|---|---|
| 283 `get_ident` | expected an identifier | TK_IDENT 校验, 词法 |
| 370 / 410 `declspec` | storage class specifier / `_Alignas` is not allowed in this context | `attr == NULL` 即抽象声明符上下文, 由调用点决定, 文法 |
| 385 `declspec` | typedef may not be used together with ... | 指示符关键字组合, 文法 |
| 528 `declspec` | invalid type | 指示符组合表(如 `int int`), 文法 |
| 726 / 728 `enum_specifier` | unknown enum type / not an enum tag | tag 名字解析, 文法分类 oracle(AGENTS 明确保留) |
| 805 `declaration` / 2607 / 2646 / 2694 | (variable/typedef/function) name omitted | 抽象声明符出现在必须有名字处, 文法 |
| 964 `struct_designator` | expected a field designator | `.` 后必须 ident, 文法 |
| 1310 `array_initializer1` | expected string literal | 文法 |
| 2068 `attribute_list` | unknown attribute | 属性名表, 文法 |
| 2594 `primary` | expected an expression | 文法 |

#### 判定表 B: 留 parse - 解析器上下文状态(4 处)

| 位置 | 文案 | 理由 |
|---|---|---|
| 1382 / 1409 | stray case / stray default | `current_switch` 是解析器自身的嵌套状态 |
| 1518 / 1529 | stray break / stray continue | `brk_label` / `cont_label` 同上 |

同属本类(非 error_tok, 但同为"必须留 parse 的语义残留"): `brk_label/cont_label/unique_label` 的分配与 break/continue 的绑定目标记录. 理由与 1.10 偏差一致 - 标签与字符串匿名全局共用 `new_unique_name` 计数器, `.L..N` 编号是汇编字节的一部分, 分配时机后移到 sema 必然改变编号. 清零动作(sema 的 `resolve_labels` 由 parse 的壳调用)同此.

#### 判定表 C: 留 parse - 建树必需, 解析器必须先拿到结果(11 处)

| 位置 | 文案 | 理由 |
|---|---|---|
| 944 / 949 / 951 `array_designator` | array designator index exceeds array bounds / range is empty | 解析器要用 begin/end 索引 `init->children[]` 建树, 值必须现算; 越界是顺带诊断 |
| 983 `struct_designator` / 990 / 1018 `designation` | struct has no such member / array index in non-array initializer / field name not in struct or union initializer | 初始化器按成员 `idx` 定位子节点, 必须先解析出 Member* |
| 2226 / 2228 / 2230 / 2233 / 2241 `struct_ref` | invalid pointer dereference / dereferencing a void pointer / not a struct nor a union / no such member | 1.6 既定设计: 成员链(含匿名成员展平)在 parse 期解析成 ND_MEMBER 链并挂 Member*, sema 只补插 DEREF. 移到 sema 需要把 ND_MEMBER 改成携带名字 token 的两段式, 超出 4.2 范围 |

#### 判定表 D: 留 parse - 常量求值/语法选择的顺带诊断(3 处)

| 位置 | 文案 | 理由 |
|---|---|---|
| 823 `declaration` | variable-sized object may not be initialized | VLA 分类已由 parse 经 `array_dimension_type` 拿到, 判定退化为"声明符后是否紧跟 `=`", 属语法位置 |
| 1392 `stmt` | empty case range specified | begin/end 两个常量值由 parse 现算(case 标签要写进节点供 codegen) |
| 2438 `generic_selection` | controlling expression type not compatible ... | `is_compatible` 的结果决定**选中哪个分支**, 是语法选择而非事后诊断 |

#### 判定表 E: 移 sema - 4.2 执行(10 处 + 两项无文案的语义)

| 位置 | 文案 | 归属 |
|---|---|---|
| 2352 `funcall` | not a function | 类型判定 |
| 2368 / 2384 `funcall` | too many / too few arguments | 需要形参表 |
| (无文案) `funcall` 的实参隐式 cast 与 float 提升 | - | 降级, 是 parse 侧最后一处 lowering |
| 1935 `unary` | cannot take address of bitfield | 需要 `lhs->member->is_bitfield` |
| 803 / 864 `declaration` | variable declared void | 类型判定(两处: 静态分支前/普通声明后) |
| 862 `declaration` | variable has incomplete type | 需要布局后的 `ty->size` |
| 2653 / 2655 / 2657 `function` | redeclared as a different kind of symbol / redefinition of %s / static declaration follows a non-static declaration | 文件域符号表语义 |
| (无文案) `struct_decl` / `union_decl` 的成员偏移/位域/size/align 布局 | - | PLAN 4.2 点名项 |

#### driver.sh 核对结果

36 条断言全部作用于**产物与编译器输出文案**(`.comm foo` / `main:` / `f1:` 等标识符存活性 / `-M` 依赖文件内容 / `file` 的 ELF 类型), 无一条断言 `error:` 文案、锚点列号或失败退出码; `test/*.c` 亦只断言运行期行为(`ASSERT`). 结论: 错误文案与锚点不在测试闸门内, 但仍按既往纪律逐字保留 - 4.2 移动判定表 E 的检查时, 锚点 token 通过节点现有字段(`ND_DECL.tok` / `ND_FUNCALL.tok` / `ND_MEMBER` 的运算符 token)原样传递; 仅 `too many arguments` 一处锚点会从"越界实参后的第一个 token"退化为调用右括号(`ND_FUNCALL.tok`), 该处会在 4.2 记录为偏差.

### 4.2a 结构体/联合体布局移 sema (本轮)

- 改了什么: sema.c 新增 `layout_struct` / `layout_union`(成员偏移/位域落位/size/align 计算原样搬入, `align_down` 一并搬为 sema 的 static); 两个函数自带 `if (ty->size < 0) return;` 的未完成类型守卫; parse.c 的 `struct_decl` / `union_decl` 收缩为"设 kind + 调布局", 删掉内联的 30 余行布局循环; chibicc.h 增两行声明.
- 为什么: 成员偏移与 size/align 是类型系统的语义产物, 不是语法形状. 解析器只负责建成员表(名字/类型/位域宽度/属性), 布局由 sema 算 - PLAN 4.2 点名项. 调用点仍在解析现场(`struct_union_decl` 返回后立即调), 因为后续解析(声明符的 `size < 0` 判定, 初始化器的 `new_initializer`, 数组维度)当场就要用布局结果, 且嵌套结构的布局顺序必须保持内层先于外层.
- 测试结果: docker-test 全绿(含自举), 快照 diff 为空. 本机 `-S` 对含位域/零宽位域/`packed`/`aligned`/前向声明/自引用/联合体 的样例与 HEAD 二进制逐字节相同.
- 偏差: 无. 判定表 E 的"结构体布局"项按计划完成.

### 4.2b 作用域表与函数声明移 sema (本轮)

- 改了什么: (1) `VarScope` / `Scope` 结构、`scope` static、`enter_scope` / `leave_scope` / `find_var` / `push_scope` 从 parse.c 搬入 sema.c, 连同变量与枚举常量的登记(`new_var` / `declare_static_local` / `add_enum_const` 直接写表)与查询(`bind_ident` 直接读表) - 三个回调式接口 `push_var_scope` / `push_enum_scope` / `find_ident` 与查询接口 `find_func` 就此消失. (2) parse 侧只留文法需要的 oracle: `find_typedef`(typedef 名字分类), `find_tag` / `find_current_tag` / `push_tag_scope`(tag 引用与定义), `in_file_scope`(复合字面量的域判定), 以及块结构的 `enter_scope` / `leave_scope`. (3) 函数声明与重定义三条诊断移入 sema 的 `declare_function`(`find_func` 随之收为 sema 内部); `VarAttr` 从 parse.c 移入 chibicc.h, 成为解析器产出、sema 消费的共享声明属性. (4) `parse_typedef` 的两步登记(`add_scope_decl` + `push_scope(type_def)`)合并为 sema 的 `add_typedef`, 与 `add_enum_const` 对称; `add_scope_decl` 收回 sema 内部.
- 为什么: 3.2b 的偏差记录里承诺"作用域重建"由 4.2 完成, 但实测(3.2b 偏差(1))表明延迟绑定不可行 - 解析期的名字解析必须继续在构造现场进行. 本步于是改为"表搬家": 表本身(名字解析的载体)归 sema, parse 只驱动块结构并保留文法分类 oracle. 3.2b 记录的"双重作用域条目"问题(parse 持有表、sema 借道回调登记)至此消失, 不再需要推迟到后续步骤.
- 测试结果: docker-test 全绿(含自举), 快照 diff 为空. 行为对照(旧 HEAD 二进制 vs 新二进制, 逐字节比对 .s 与 stderr): t42.c 覆盖 typedef/枚举/嵌套与遮蔽/块域与 for 作用域/tag 前向声明与重定义/自引用结构/位域/联合体/static 局部/复合字面量/`?:`/sizeof/_Alignof/static inline 链 - 全等; e1-e6 覆盖函数重定义/重声明类别冲突/static 声明跟随非 static/未定义变量/块内 typedef 遮蔽/成员不存在 - 错误文案与插入符全等.
- 偏差: `get_scope_decls` 保留为导出接口但当前无调用者 - 它是 3.2a/3.4b 记录链(ND_TYPEDEF/ND_ENUM_CONST 的源码序串联)的读取口, 其"供 4.2 重建作用域"的原始用途已随本步的表搬家失效; 记录链本身仍保留, 作为忠实声明记录(阶段 3 要求的节点形态)的载体, 若后续判定无用可连同 `add_scope_decl` 一起删除.

### 4.2c 函数调用降级与残余类型检查移 sema (本轮)

- 改了什么: (1) sema.c 新增 `lower_funcall(node, tok)` - callee 判定("not a function", 锚点 `fn->tok`)、实参逐个转成形参类型(结构体/联合体形参不转)、变参尾部的 float 提升为 double、数量检查("too many/few arguments", 锚点调用位置)、`node->func_ty` / `node->ty` 赋值、以及 struct/union 返回值的 `ret_buffer` 创建, 全部由它完成; parse.c 的 `funcall` 只剩"解析实参列表 + 建节点 + 调 sema", 不再读形参表. (2) `unary` 的 `&` 去掉类型检查, 位域取址诊断移入 sema 的 ND_ADDR case(锚点 = `&` token, 与旧一致). (3) 声明的 `variable declared void` / `variable has incomplete type` 移入 sema 的 ND_DECL 降级(静态局部分支无 AST 节点, 由 `declare_static_local` 自带 void 检查); 后者原样跳过 VLA 分支(旧代码同样 continue 跳过), 未完成类型检查在初始化器之后(灵活数组成员的补全才使类型完整).
- 为什么: 实参隐式转换是 parse 侧最后一处 lowering(PLAN 1.3-1.7 把缩放/复合赋值/自增/下标全部搬走后的遗漏项), 也是 PLAN 4.2 点名的"funcall 实参数量与类型"; 三条类型诊断同属判定表 E. 调用点仍在解析现场(`lower_funcall` 在 funcall 构造后立即调用): `ret_buffer` 会创建 lvar, 创建时机后移会翻转它与后续临时变量的 locals 链顺序, 栈偏移即变(同 1.4/2.1/2.3 的时序原则).
- 测试结果: docker-test 全绿(含自举), 快照 diff 为空. 行为对照(c1-c10 + v1-v5, 旧 HEAD 二进制 vs 新二进制): 实参过多/过少、非函数调用、位域取址、`void x;`、`int x[];`、`static void x;`、struct 返回值、变参 float 提升、未完成 struct、`static void x = ...` - 汇编与 stderr 全等; 差异仅限下述偏差条列出的 `void x = <初始化器>;` 与 "too many arguments" 两类诊断时机.
- 偏差(错误诊断的时机位移, 仅 "variable declared void" 一处): 该检查从"解析到声明符之后立即"改为"ND_DECL 降级时", 因此对**块域非静态**的 `void x = <初始化器>;` 有三点可见变化: (1) 插入符由 `=` 变为初始化器结束后的 token(ND_DECL 的锚点定义, 见 2.1 偏差(1)); (2) 若初始化器本身也非法, 报出的错误随之改变 - 实测 `void x = undefined_thing;` 旧版报 "variable declared void", 新版先报 "undefined variable"; (3) `void x;`(无初始化器)插入符不变, 块域 static 路径(`static void x = ...;` 仍由 `declare_static_local` 在声明现场检查)与文件域行为均不变. 另有 "too many arguments" 一处插入符位移: 锚点由"越界实参之后的第一个 token"变为调用右括号 - 新实现按实参表统一判定数量, 不再在解析到越界实参的那一刻报错, 而 `ND_FUNCALL.tok` 即右括号. 两处均无测试覆盖; 若需恢复原诊断时机与插入符, 只能在 parse 侧保留对应判定, 与"检查归 sema"的目标冲突.

## 给审核者的提示

- 审核重心建议放 sema.c: add_type 的降级 case(GT/GE, COND elvis, ASSIGN/INCDEC/SUBSCRIPT/ADD/SUB/MEMBER, STRING, SIZEOF/ALIGNOF, WHILE/BREAK/CONTINUE, DECL, COMPOUND_LITERAL, IDENT, ADDR 的位域检查, FUNCALL)与 to_assign/compound_op/new_add/new_sub/compute_vla_size/vla_size_expr/lvar_init_comma/gvar_init_data/bind_ident/begin_function/resolve_labels/finalize_globals/array_dimension_type/declare_static_local/add_enum_const/add_typedef/declare_function/layout_struct/layout_union/lower_funcall.
- **P4 收尾状态**: parse.c 2531 行(基线 3368, 减少 837; PLAN 终态估计 2100 未完全达到, 差额集中在声明符/类型构建本身 - 解析器必须建 Type 对象才能继续解析), sema.c 1552 行(PLAN 估计 1300, 超出部分是 P1-P3 各步的实际搬家量), chibicc.h 660 行; codegen.c 相对 5f53ed0 仍是 0 行 diff. parse.c 的 `error_tok` 从 43 处减到 33 处, 与 4.1 判定表 E 的 10 处完全对应.
- **名字解析的最终形态(4.2b 后)**: 作用域表(`VarScope`/`Scope`/`scope`)归 sema 持有, 变量与枚举常量由 sema 直接登记与查询; parse 侧只剩文法 oracle - `find_typedef`(typedef 名字分类), `find_tag`/`find_current_tag`/`push_tag_scope`(tag 引用与定义), `enter_scope`/`leave_scope`/`in_file_scope`(块结构驱动), `get_current_fn`/`get_ident`(refs 归属与拼写). 3.2b 记录的回调式接口(`push_var_scope`/`push_enum_scope`/`find_ident`/`find_func`)已全部消失.
- **声明记录侧链(3.2a/3.4b, 现状)**: typedef 与枚举常量以 ND_TYPEDEF/ND_ENUM_CONST 节点记在 sema 的 `scope_decls` 链上(源码顺序), 不进语句链 - 因为块域 typedef/枚举声明今天不产生语句节点, 入链就会多出 .loc 行. 该链现已无树内消费者(原定的"重建作用域"用途被 4.2b 的表搬家取代), 保留是为承载阶段 3 要求的忠实声明记录; `get_scope_decls` 是它当前的唯一读取口.
- **3.2b 的关键约束(值得单独记住)**: 标识符**不能**推迟到一个后置的 resolve 遍历里绑定. parse 的作用域是动态栈(`for` init 的变量在语句解析完就出栈), 而使用点要等到语句级 add_type 才被访问; 同时 sizeof/自增自减/字符串等构造点又要求解析期就能拿到类型. 两条合起来把绑定钉在构造现场. 这也是 4.2 最终选择"表搬家"而非"重建作用域 + 遍历绑定"的原因.
- **parse 侧残余语义(4.1 判定表 A-D, 33 处, 收官保留)**: 文法类 15 处(名字省略/指示符组合/属性名/tag 类型不符 等), 解析器上下文 4 处(stray case/default/break/continue), 建树必需 11 处(初始化器 designator 的越界与成员定位, 成员访问的类型与存在性 - 1.6 既定设计: 成员链含匿名成员展平必须现解析, ND_MEMBER 挂 Member*), 常量求值/语法选择顺带 3 处. 另有三处非 error_tok 的 parse 期语义残留: stmt() 的 return 分支经 `current_fn->ty->return_ty` 插隐式 cast, resolve_goto_labels 壳里清空 gotos/labels, `fn->locals = get_locals()`.
- 一个待拍板的设计点: op 字段现在对 ND_ADD/ND_SUB 兼任"已缩放"标记. 若接受快照里这类 no-op cast 差异(或在后续阶段统一), 该标记机制可简化; 当前为字节级等价而保留.
- 时序原则(1.4 确立, 1.8 扩展, 4.2 沿用): 凡降级会创建 lvar 或匿名 gvar 的节点, parse 构造现场立即触发降级(`add_type` 或直接调 sema 的搬家函数, 如 4.2c 的 `lower_funcall`), 保证创建顺序与旧代码一致; 1.10 进一步表明, 与这些名字共用 new_unique_name 计数器的标签分配同样必须留在 parse.
- .loc 原则(2.1 发现, 3.2a/3.4a/3.4b 反复命中): gen_stmt 对每个语句节点(含 ND_BLOCK)先按 node->tok 打一行 .loc, 因此语句链的形状与锚点被字节冻结. 由此否决了四处计划原文: 2.1 的"外层 ND_BLOCK 取消", 2.2 的"EXPR_STMT 前缀消失", 3.2a/3.4b 的"声明节点进树", 3.4a 的"static 局部走 ND_DECL 降级建 gvar".
- 忠实层的已知残留: ND_WHILE/ND_FOR/ND_DO 带 parse 期分配的 brk/cont 标签, ND_BREAK/ND_CONTINUE 带 parse 期记录的绑定目标(unique_label) - 标签与匿名全局共用 new_unique_name, `.L..N` 编号是汇编字节的一部分, 因此这部分是字节级等价所要求的 parse 期语义残留, 清理需先接受快照重置. 另有: ND_DECL 的 vla-size 兄弟语句(2.2 已述), scope_decls 侧链无作用域归属(3.2a/3.4b).
- 错误诊断的两处已知时机位移(4.2c, 无测试覆盖): `too many arguments` 的锚点从越界实参后的第一个 token 变为调用右括号; 块域非静态的 `void x = <初始化器>;` 的 `variable declared void` 延后到初始化器之后(插入符随 ND_DECL 锚点变化, 初始化器本身非法时报错内容也会变). 其余全部错误路径经逐字节对照与旧二进制一致.
- codegen.c 全程零改动: git diff 5f53ed0..HEAD -- codegen.c 为空.
- 常用命令: make docker-test(全量 + 自举), make docker-snapshot-diff(快照 diff), make docker-snapshot(重置基线, 仅在刻意的行为变化后).
- 逐字节对照的土办法(3.2b 起常用): `git worktree add /tmp/cbase <commit> && make -C /tmp/cbase`, 再对同一组 .c 用两个二进制跑 -S, diff 汇编与 stderr; 样例覆盖 enum/typedef/作用域遮蔽/循环/switch/复合赋值/字符串/复合字面量/VLA/sizeof/static inline/全局初始化器, 4.2 起加 位域/属性/packed/tag 前向声明与重定义/函数重定义/实参数量与类型.
