# RESULT.md - 语法语义拆分执行记录

本文件记录 PLAN.md 各步骤的实际执行结果, 供审核与后续会话接续参考. 以下记录到 1.10 为止, P1(表达式层忠实化)全部完成, 下一步为 P2 2.1 ND_DECL.

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

## 给审核者的提示

- 审核重心建议放 sema.c: add_type 的降级 case(GT/GE, COND elvis, ASSIGN/INCDEC/SUBSCRIPT/ADD/SUB/MEMBER, STRING, SIZEOF/ALIGNOF, WHILE/BREAK/CONTINUE)与 to_assign/compound_op/new_add/new_sub/compute_vla_size/vla_size_expr.
- 一个待拍板的设计点: op 字段现在对 ND_ADD/ND_SUB 兼任"已缩放"标记. 若接受快照里这类 no-op cast 差异(或在 4.2 收官时统一), 该标记机制可简化; 当前为字节级等价而保留.
- 时序原则(1.4 确立, 1.8 扩展): 凡降级会创建 lvar 或匿名 gvar 的节点, parse 构造现场立即 add_type 触发降级, 保证创建顺序与旧代码一致; 1.10 进一步表明, 与这些名字共用 new_unique_name 计数器的标签分配同样必须留在 parse.
- 忠实层的已知残留: ND_WHILE/ND_FOR/ND_DO 带 parse 期分配的 brk/cont 标签, ND_BREAK/ND_CONTINUE 带 parse 期记录的绑定目标(unique_label) — 这些是字节级等价所要求的 parse 期语义残留, 待 3.x/4.x 清理.
- codegen.c 全程零改动: git diff 5f53ed0..HEAD -- codegen.c 为空.
- 常用命令: make docker-test(全量 + 自举), make docker-snapshot-diff(快照 diff), make docker-snapshot(重置基线, 仅在刻意的行为变化后).
