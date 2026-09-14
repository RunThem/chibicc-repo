# RESULT.md - 语法语义拆分执行记录

本文件记录 PLAN.md 各步骤的实际执行结果, 供审核与后续会话接续参考. 以下记录到 3.3 为止, P3(名字解析出解析器)进行中, 下一步为 3.4 清返工点.

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

## 给审核者的提示

- 审核重心建议放 sema.c: add_type 的降级 case(GT/GE, COND elvis, ASSIGN/INCDEC/SUBSCRIPT/ADD/SUB/MEMBER, STRING, SIZEOF/ALIGNOF, WHILE/BREAK/CONTINUE, DECL, COMPOUND_LITERAL)与 to_assign/compound_op/new_add/new_sub/compute_vla_size/vla_size_expr/lvar_init_comma/gvar_init_data.
- 一个待拍板的设计点: op 字段现在对 ND_ADD/ND_SUB 兼任"已缩放"标记. 若接受快照里这类 no-op cast 差异(或在 4.2 收官时统一), 该标记机制可简化; 当前为字节级等价而保留.
- 时序原则(1.4 确立, 1.8 扩展): 凡降级会创建 lvar 或匿名 gvar 的节点, parse 构造现场立即 add_type 触发降级, 保证创建顺序与旧代码一致; 1.10 进一步表明, 与这些名字共用 new_unique_name 计数器的标签分配同样必须留在 parse.
- .loc 原则(2.1 发现): gen_stmt 对每个语句节点(含 ND_BLOCK)先按 node->tok 打一行 .loc, 因此语句链的形状与锚点被字节冻结 — ND_DECL 只能占据既有 ES(init)/ES(alloca) 的链位, 外层 ND_BLOCK 与 vla-size 前缀语句不可取消(PLAN 中"外层 ND_BLOCK 包装取消"与"EXPR_STMT 前缀消失"由字节闸门否决, 见 2.1/2.2 偏差); 无初始值的 ND_DECL 携带 vla-size 树于 lhs 并降级为该语句.
- 忠实层的已知残留: ND_WHILE/ND_FOR/ND_DO 带 parse 期分配的 brk/cont 标签, ND_BREAK/ND_CONTINUE 带 parse 期记录的绑定目标(unique_label) — 这些是字节级等价所要求的 parse 期语义残留, 待 3.x/4.x 清理. 另有: static 局部声明不发节点(3.4), ND_DECL 的 vla-size 兄弟语句(2.2 已述).
- codegen.c 全程零改动: git diff 5f53ed0..HEAD -- codegen.c 为空.
- 常用命令: make docker-test(全量 + 自举), make docker-snapshot-diff(快照 diff), make docker-snapshot(重置基线, 仅在刻意的行为变化后).
