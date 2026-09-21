# PLAN.md - codegen 直连 sema 产物执行计划(取消 sema 降级线)

新目标(用户 2026-09-21 拍板): **修改 codegen, 让它直接消费 sema 的产物; sema 阶段不再降级.**

前两条线已归档: 语法语义拆分线见 `PLAN-split.md` / `RESULT-split.md`, 忠实层收尾线见
`PLAN-faithful.md` / `RESULT-faithful.md`. 本线从忠实层终态(20abd43: parse.c 2148 行 +
sema.c 3145 行 + chibicc.h 704 行, codegen.c 相对 5f53ed0 零 diff)起步, 是 AGENTS.md 目标架构
第 4 层的边界重划. CST/trivia(阶段 1-2)与库化(阶段 5)仍待用户拍板, 与本线正交.

本线结束时: **sema 的产物 = 忠实语法树 + 标注**(名字绑定, 类型, 编译期必需的语义结论, 布局,
语言规定存在的对象); 一切"为某个后端发射服务的整形"与"只为实现服务的槽"在 codegen 一侧完成.

新会话恢复方法: 通读 AGENTS.md -> 查看本文件勾选状态 -> `git log --oneline` 确认最后完成的步骤
-> 从第一个未勾选项继续; 开工前先跑 A0.1 的四闸门复验. 行号一律以函数名为准.

## 目标边界与判据

**判据 1 - 语义结论 vs 发射便利**
- 留 sema: 编译期必须做出的结论 - 类型转换与隐式 cast 标记, 常量求值, 名字与成员绑定,
  `_Generic` 选择, `sizeof`/`_Alignof` 结论, 枚举绑定, 结构体布局, 数组维度.
- 移 codegen: 只为把树变成"某个后端好发射的形状"的改写 - 指针算术缩放, 复合赋值与自增自减的
  读写回环, `>`/`>=` 交换操作数, `->` 补插 DEREF, `while` 变 `for`, break/continue 变 goto,
  `x[y]` 变 `*(x+y)`, 声明展开成赋值链, 返回值缓冲, elvis 临时量.

**判据 2 - 检查与诊断留 sema**
全部诊断与其锚点留在 sema 的标注/检查路径; 现夹在降级代码里的检查必须在新位置显式保留(赋值给
数组的 `not an lvalue`, 调用者不是函数, 实参个数, 位域取地址, VLA 不得初始化, 不完整类型, 语句
表达式末语句形状). `test/diagnostic.sh` 的 44 例逐字节锁定是这一条的量尺.

**判据 3 - 对象物化按语义归属**
C 语言规定存在的对象(字符串字面量的匿名全局, 复合字面量的无名字对象, 参数 / `__va_area__` /
`__alloca_size__` / 大结构体返回缓冲参数等 ABI 对象)归 sema; 只为实现服务的槽(调用者的返回缓冲,
VLA 尺寸变量, 读写回环与 elvis 的临时量)归 codegen, 计入帧布局.

**判据 4 - 结论记在节点上, 不复制下游逻辑**
一个改写若结论只是"另一个已有子树的引用"或"一个数值"(泛型选择, sizeof/alignof, 两个类型
builtin, 字符串物化后的对象), 就把结论写进节点字段, 求值器与 codegen 各自读, 不再让 `add_type`
的整形替两边代劳.

## 现状锚点(开工前实测)

- sema 的整形分三处: `add_type`(case 分派: ADD/SUB 缩放, ASSIGN.op, INCDEC, GT/GE, SUBSCRIPT,
  MEMBER 补 DEREF, COND elvis, FUNCALL, STRING, SIZEOF/ALIGNOF, TYPES_COMPATIBLE/REG_CLASS,
  GENERIC, DEREF-of-func, DECL, COMPOUND_LITERAL), `type_chain`(链上记录摘除 + for-init 的
  ND_BLOCK 包装), `analyze`(标签分配, ND_WHILE→ND_FOR, break/continue→goto, case 链, label 名).
- **求值器搭了整形的便车**: `eval2`/`is_const_expr` 开头就调 `add_type`, 于是求值 switch 见到的
  都是整形后的形状(GT→LT, SUBSCRIPT→DEREF, STRING→VAR, SIZEOF→NUM, GENERIC→选中项); preprocess
  的 `#if` 经 `const_expr` 走同一条路. 整形一移走, 求值器必须自己认忠实形态 - 这是本线第一处
  "新逻辑", 不是搬家. `write_gvar_data`(全局初始化序列化)经 `eval2`/`eval_rval` 受同一影响.
- **顺序敏感面**: 全局匿名对象(字符串 / 复合字面量 / 块域 static)的创建顺序决定 `.data` 块序,
  归一化 diff 不折叠块序, 因此物化必须留在标注遍的同一遍历位置; 局部隐藏对象的顺序只影响栈偏移,
  归一化 diff 免疫(第 3/4 类规则), 因此槽创建可自由搬迁.
- codegen 今天已自算 `pass_by_stack`(`push_args`); sema 写入而 codegen 读取的字段: `ret_buffer`,
  `brk_label`/`cont_label`, `unique_label`/`label`, `case_next`/`default_case`, `func_ty`,
  `Type::vla_size`.
- `codegen()` 入口顺序是 `assign_lvar_offsets → emit_data → emit_text`; 块域 static 初始化器里的
  `&&label` 经 `Relocation.label` 指向 `node->unique_label`, 所以标签名必须在 `emit_data` 之前定下.

## 闸门口径

- **[搬迁] 步骤**(整形逻辑换位置, 发射形态不变): 硬闸门 = `make docker-test`(含自举) +
  `test/diagnostic.sh`(44 例逐字节) + `make docker-snapshot-ndiff` 为空; 同提交内
  `make docker-snapshot` 重置 raw 基线(`-diff` 只作可读参考).
- **[就地] 步骤**(发射形态变化): 硬闸门 = `make docker-test` + 诊断锁定 +
  `make test-thirdparty THIRDPARTY=tinycc` + 该步新增的形状断言; `docker-snapshot-ndiff` 允许
  非空, 但差异必须逐条归入预期模式并记入 `RESULT.md`.
- 一步一提交, 返工 = revert 单个提交; 每步汇报固定四项(改了什么 / 为什么 / 闸门结果 / 偏差).
- 红线互换: codegen 的"零改动红线"本线作废(本线的目标就是改 codegen); 新红线 = **库层
  (parse.c / sema.c / type.c / preprocess.c / tokenize.c)不得再出现后端整形**(指针缩放, 读写回环,
  临时槽, 语句重排, 标签与唯一名分配).
- 本线不做(仍属库化阶段 5): 错误回调化, context 对象, 公共/内部头拆分, arena reset.

## 阶段 A: 边界搬迁(整形归 codegen, 发射形态不变)

- [ ] **A0.1 账本与基线**: `RESULT.md` 建"整形清单表"(逐项: 现位置 / 判据归属 / 目标位置 / 验收
口径, 初版已随本文件提交, 实施时逐项核对), 复验四闸门(docker-test / 诊断锁定 / ndiff 空 /
tinycc)并留档为本线基线.
- [ ] **A1.1 求值器认忠实形态(表达式层)**: `eval2`/`eval_rval`/`eval_double`/`is_const_expr` 补
忠实 case - ND_GT/ND_GE(就地判), ND_SUBSCRIPT(按下标取址), ND_MEMBER 的 arrow(指针值 + 偏移),
ND_STRING(读物化对象名), ND_SIZEOF/ND_ALIGNOF(定长取 size/align, VLA 情形不参与常量),
ND_TYPES_COMPATIBLE/ND_REG_CLASS(读结论), ND_GENERIC(读选中项), ND_INCDEC 与带 op 的 ND_ASSIGN
(非常量, 保持 "not a compile-time constant" 文案). 本步先落地: A3.1 起到 A9.1 的搬运之前, 这些
case 触达不到(整形仍在前), 属"先埋好", 无害; `#if` 路径随 `const_expr` 一并覆盖.
- [ ] **A1.2 结论字段**: Node 增设结论槽(`_Generic` 的选中项; sizeof/alignof 与两个类型 builtin
的结论), `add_type` 写结论而不替换节点; 求值器与 codegen 分别读. 本步只加字段与写入而不改消费
方(消费方仍是整形替换), 因此常规口径、零行为变化.
- [ ] **A2.1 标签与控制流归 codegen**: codegen 新增标签计数器与"循环/switch 标签栈", `analyze`
的整段搬入 codegen 的整形遍(标签分配, ND_WHILE→ND_FOR, break/continue→ND_GOTO, case 链,
ND_LABEL 与 `&&label` 的名字); sema 只保留 stray 四检查与 goto/label 配对检查(纯检查, 不写名字);
`codegen()` 入口改为"整形遍 → assign_lvar_offsets → emit_data → emit_text". 验收: ndiff 空
(标签重排由归一化吃掉).
- [ ] **A3.1 成员与下标**: ND_SUBSCRIPT 的 `*(x+y)` 降级, ND_MEMBER 的 arrow DEREF 补插搬
codegen; sema 保留 `resolve_member` 的绑定、匿名成员展平与三类校验(锚点不变), 保留
`invalid pointer dereference`/`dereferencing a void pointer`/`not a struct nor a union`/
`no such member`/`cannot take address of bitfield` 检查; `*foo`(函数指针消解, 6.5.3.2p4)按判据 1
留 sema 定型, 不再替换节点(codegen 现有 ND_DEREF 路径天然可用).
- [ ] **A4.1 算术与比较**: `new_add`/`new_sub`/`scale_rhs`/`new_arith`/`combine` 与 GT/GE 交换搬
codegen; sema 保留 `invalid operands` 检查与 `usual_arith_conv` 的隐式 cast(判据 1/4).
- [ ] **A5.1 复合赋值与自增自减**: `to_assign`/`compound_op`/`new_inc_dec` 搬 codegen - 普通与
位域成员情形可落成"地址一次求值 + 读改写", 原子 op= 的 do-while + CAS 语句构造进 codegen 的整形
遍(用 codegen 自己的槽与标签); sema 保留"赋值给数组"等检查.
- [ ] **A6.1 函数调用**: `lower_funcall` 的检查(不是函数, 实参个数, float 提升)与实参转换(隐式
cast, 判据 1)留 sema, `func_ty` 标注留 sema; 调用者返回缓冲(`ret_buffer`)的槽创建搬 codegen.
- [ ] **A7.1 VLA**: `compute_vla_size`/`vla_size_expr` 与 `Type::vla_size` 的写入搬 codegen(字段
注释改标"codegen 侧缓存"), VLA 指针的缩放搬 codegen; sema 保留维度求值与"VLA 不得初始化"检查.
- [ ] **A8.1 声明记录与初始化器**: `type_chain` 的记录摘除(ND_TYPEDEF/ND_ENUM_CONST/
ND_GVAR_DECL/ND_FUNCDEF)与 for-init 的 ND_BLOCK 包装搬 codegen - 记录保留在 sema 输出里(库消费者
可见, 是"直连"的应有之义); codegen 把 ND_DECL 展开成 0..n 条语句(VLA 的 alloca 赋值, MEMZERO +
赋值回环, 无初始化器的定长对象不产语句), 块域 static 的数据镜像仍由 sema 序列化、只把"记录出链"
交给 codegen. 语句表达式的值语义随之简化: 末语句是记录(声明)即无值, 报 "statement expression
returning void is not supported"(与锁定用例 e11 同锚点).
- [ ] **A8.2 初始化器结果的消费侧**: `ResolvedInit`/`InitDesig` 移入 `chibicc.h` 并命名归位(如
`InitTree`/`InitPath`) - sema 的 resolve 遍继续构建它(designator 求值、brace elision、柔性数组成员
是语义结论), `create_lvar_init`/`lvar_init_comma`/`init_desg_expr` 搬 codegen; 文件域序列化
(`write_gvar_data`/`gvar_init_data`)留 sema.
- [ ] **A8.3 复合字面量**: 无名字对象的创建留 sema 的 resolve 遍(顺序不变, `.data` 块序保真),
数据序列化留 sema, "初始化链 + 对象引用"的整形归 codegen.
- [ ] **A9.1 结论类的消费侧**: ND_STRING 物化后只记 `node->var` 而不改 kind(位置与顺序不变,
判据 3/顺序敏感面); ND_SIZEOF/ND_ALIGNOF/ND_TYPES_COMPATIBLE/ND_REG_CLASS/ND_GENERIC 不再被
替换, 消费方读结论字段; ND_COND 的 elvis 降级搬 codegen(优先用现有 `.L.else/.L.end` 计数标签
就地发射: 条件值留在 rax, 无需临时槽; 若字节口径需要槽则在整形遍建槽).
- [ ] **A9.2 add_type 收尾核对**: `add_type` 至此应为纯标注遍 - 填 `ty`, 插隐式 cast, 做检查,
写结论, 物化(位置不变), 绑定成员; 逐项核对账本表, 确认没有任何 case 改写树形状. 若前面按 case 族
推进时分批落地, 本步做收口核对与偏差记录.
- [ ] **A10.1 归属清扫与文档**: `chibicc.h` 逐字段标注归属(codegen 写入的字段: `ret_buffer`,
`brk_label`/`cont_label`, `unique_label`/`label`, `case_next`/`default_case`, `pass_by_stack`,
`Type::vla_size`, 结论槽), sema.c/codegen.c 文件头注释重写, 库层"无整形"的 grep 佐证留档(parse.c/
sema.c 的整形点计数、codegen 新增行数), AGENTS.md 的目标架构/现状段/硬性规则按终态更新.
- [ ] **A10.2 针对性回归**: 补测试覆盖忠实形态的发射面: `a[i] += j++`(下标 + 复合赋值 + 后缀
自增), `p ?: q`, 大结构体返回值缓冲, VLA 尺寸复用与 `sizeof(VLA)`, `_Generic` 结果, 位域 op=,
原子 op=, `&&label` 的块域 static 初始化器, 语句表达式末语句为记录.

## 阶段 B: 就地化(让"直连"名副其实, 可裁)

阶段 A 之后 codegen 仍先把 sema 的树整回旧形状再发射; 阶段 B 把"只需换个发射方式"的项从整形遍
搬进发射点, 让整形遍只剩"需要语句重排或新槽"的少数项(原子 op=, 返回缓冲, VLA, elvis). A 完成
即已达成"取消 sema 降级"; B 由用户决定是否做.

- [ ] **B1.1 发射点直读(表达式层)**: ND_SUBSCRIPT, ND_MEMBER(arrow), ND_GT/ND_GE, ND_STRING,
ND_SIZEOF/ND_ALIGNOF, ND_GENERIC 在 `gen_addr`/`gen_expr` 就地处理, 对应整形 case 删除; 每项一个
提交([就地] 口径).
- [ ] **B1.2 发射点直读(语句层)**: ND_WHILE, ND_BREAK/ND_CONTINUE(标签栈), ND_DECL(初始化链
就地摊成语句)由 `gen_stmt` 直接处理, 整形遍对应部分删除.
- [ ] **B1.3 形状断言**: 对 .s 加模式断言(下标不再出现中间 DEREF 形, elvis 不再出现槽, while
不再出现 FOR 形等), 与行为闸门共同作为阶段 B 的验收.

## 开放决策(实施时由用户拍板)

- **阶段 B 是否做**: A 是"取消 sema 降级"的充分条件; B 让它名副其实, 但引入 [就地] 口径与形状
断言成本.
- **结论字段的形态**: 泛型选中项与 sizeof 结论是各占一个 Node 字段, 还是复用 `ty_op`/`lhs` 等既有
槽(需注释区分).
- **`InitTree` 的公共性**: 初始化器解析结果进 `chibicc.h` 即成库 API 面(命名与不透明程度待定);
若不暴露, 只能让 codegen 反向依赖 sema 的遍历函数, 与库化方向冲突.
- **标签分配归属**: 本计划把标签名分配放 codegen(它同时负责 break/continue 的形状); 若希望 sema
继续持有名字, 只把形状补上, 也可行(代价是 sema 仍需持有计数器的交织顺序).
- **库 API 面**: 本线让函数体内的声明记录留在树里; 顶层记录链是否也作为 sema 的返回值暴露(现在
是 `Obj *sema(Node *)`), 属库化阶段 5 的 API 设计.

## 风险与对策

1. **`add_type` 一分为二是大爆炸点**: 对策 = A3.1 起按 case 族逐批搬运(A3.1 下标与成员, A4.1
   算术与比较, A5.1 复合赋值与自增自减, A6.1 调用, A7.1 VLA, A8.x 声明与初始化器, A9.1 结论类),
   每批"标注侧减法 + 整形侧加法"同一提交、独立可绿; 每批都用 ndiff 校验"发射形态未变"; 出现无法
   归因的差异时以 `git worktree add /tmp/cbase <上一个绿提交>` 逐字节对照两个二进制的 .s.
2. **诊断漂移**: 每次提交跑 `test/diagnostic.sh`; 夹在降级里的检查(A5.1 的 `not an lvalue`,
   A6.1 的实参个数/不是函数, A7.1 的 VLA 不得初始化)先在新位置显式落地, 再删降级代码.
3. **`.data` 块序**: 物化留标注遍(A9.1 的字符串物化只改"记 var"不改 kind、不改遍历位置).
4. **帧布局与 ABI**: 参数表顺序(`fn->params`, pass-by-stack 参数的 ABI 偏移)不得动; 返回缓冲与
   VLA 尺寸变量属 `locals` 侧新增, 只影响栈偏移(ndiff 第 3/4 类免疫).
5. **原子 op= 的语句构造**: 今天依赖 sema 的 `new_lvar`/`new_unique_name`/ND_DO 标签; 搬进
   codegen 时用 codegen 自己的槽与标签分配, 并核对 CAS 循环的跳出语义与 `.L..N` 名字.

## 终态验收(本线完成时逐项实测填写)

- **库层**: `add_type` 无改写树形状的 case(逐项对账本表); sema.c 无缩放/读写回环/槽创建/语句
  重排/标签分配; 求值器与全局序列化在忠实形态上工作.
- **codegen**: 整形遍只含"需要语句重排或新槽"的项(阶段 A 后含全部, 阶段 B 后为少数); 帧布局与
  ABI 与路线图红线一致.
- **闸门**: `docker-test`(含自举) + 诊断 44 例 + `tinycc` + 快照口径逐阶段记录.
- **对比锚点**: parse.c 2148 / sema.c 3145 / chibicc.h 704 / codegen.c 1595 → 实测填入.
