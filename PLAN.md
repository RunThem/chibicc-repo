# PLAN.md - codegen 直连 sema 产物执行计划(取消 sema 降级)

目标(用户 2026-09-21 拍板): **修改 codegen, 让它直接消费 sema 的产物; sema 阶段不再降级.**

前两条线已归档: 语法语义拆分线见 `PLAN-split.md` / `RESULT-split.md`, 忠实层收尾线见
`PLAN-faithful.md` / `RESULT-faithful.md`. 本线从忠实层终态(20abd43: parse.c 2148 行 +
sema.c 3145 行 + chibicc.h 704 行, codegen.c 相对 5f53ed0 零 diff)起步, 是 AGENTS.md 目标架构
第 4 层的边界重划. CST/trivia(阶段 1-2)与库化(阶段 5)仍待用户拍板, 与本线正交.

本线结束时: **sema 的产物 = 忠实语法树 + 标注**(名字绑定, 类型, 编译期必需的语义结论, 布局,
语言规定存在的对象); 一切"为某个后端发射服务的整形"与"只为实现服务的槽"在 codegen 一侧完成.

新会话恢复方法: 通读 AGENTS.md -> 读本文件的"搬运契约" -> 查看勾选状态与 `RESULT.md` 的提交一览
-> `git log --oneline` 确认最后完成的步骤 -> 从执行顺序里的第一个未勾选项继续; 开工前先跑 A0.1
的四闸门复验. 行号一律以函数名为准.

**步骤编号是稳定标签, 不是执行顺序**(账本表按编号引用). 实际顺序见"阶段 A"开头的顺序行; 顺序
由契约 1 的调用图约束决定, 与编号先后不一致处一律以顺序行为准.

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
表达式末语句形状, 加减法 `invalid operands`). `test/diagnostic.sh` 的逐字节锁定是这一条的量尺:
原有 44 例只覆盖 parse 的 33 处与 sema 判定表 E, **夹在降级里的检查一例都没有**, 所以 A0.1 先补
f 系列 9 例(共 53 例)再开始搬 - 语料里只有合法 C, 丢一条检查不会被任何其它闸门看见.

**判据 3 - 对象物化按语义归属**
C 语言规定存在的对象(字符串字面量的匿名全局, 复合字面量的无名字对象, 参数 / `__va_area__` /
`__alloca_size__` / 大结构体返回缓冲参数等 ABI 对象)归 sema; 只为实现服务的槽(调用者的返回缓冲,
VLA 尺寸变量, 读写回环与 elvis 的临时量)归 codegen, 计入帧布局. 本判据同时是顺序约束, 见契约 4.

**判据 4 - 结论记在节点上, 不复制下游逻辑**
一个改写若结论只是"另一个已有子树的引用"或"一个数值"(泛型选择, sizeof/alignof, 两个类型
builtin, 字符串物化后的对象), 就把结论写进节点字段, 求值器与 codegen 各自读, 不再让 `add_type`
的整形替两边代劳. 落地形式见契约 2: codegen 可以**调用**库层的类型级工具, 但不得**重新实现**任何
类型决策.

## 搬运契约(本线的实施规则, 每步都适用)

**契约 1 - 调用方向单向, 顺序由调用图决定**
codegen 是库的消费者, 可以调用 sema 导出的任何函数; sema 绝不能调用 codegen. 因此: **一个函数只能在
它的全部 sema 侧调用者都已搬走或不再调用它之后搬走**; 在那之前, 已搬到 codegen 的代码调用仍在 sema
的被调用者(临时导出, 被调用者搬走的那一步撤掉, A9.2 核对为 0). 这条把 A3-A9 从"重写"降为
"文本搬运 + 导出声明", 本线最大的风险(`add_type` 一分为二)主要靠它压住.

实测的调用图约束(决定了下面的执行顺序):
- `new_add`/`new_sub` 被 `add_type` 的 ND_ADD/ND_SUB, ND_SUBSCRIPT, `compound_op`, `to_assign`
  的原子分支, `new_inc_dec` 调用 -> 下标(A3.1)与复合赋值(A5.1)必须先于算术(A4.1)搬.
- `compute_vla_size`/`vla_size_expr` 被 `add_type` 的 ND_SIZEOF(VLA 情形)与 ND_DECL 调用 ->
  结论类的消费侧(A9.1)必须先于 VLA(A7.1)搬, 否则 sema 会在 codegen 还没跑的时候去读一个由
  codegen 写的 `Type::vla_size`.
- `lvar_init_comma` 只被 ND_DECL 与 ND_COMPOUND_LITERAL 调用 -> 声明展开与初始化器消费侧必须
  同一步搬(A8.1), 复合字面量随后(A8.3).

**契约 2 - 库层导出的类型级工具**
codegen 建节点一律经这些导出函数, 拿到的是**已定型**的节点, 与今天逐字节相同(含 sema 插的隐式
cast 与其 `.loc`):
- 永久导出: `add_type`(A9.2 后是纯标注遍), `new_arith`, `usual_arith_conv`, `get_common_type`,
  `new_cast`, `new_long`, `new_ulong`, `new_var_node`, `new_vla_ptr`, `new_alloca`.
- 临时导出(被调用者搬进 codegen 的那一步同时删掉声明, 最迟 A9.2 核对为 0): `new_add`, `new_sub`,
  `scale_rhs`(以上 A4.1 结束), `vla_size_expr`, `compute_vla_size`(以上 A7.1 结束).
- 陷阱: `combine` 的非加减分支必须走 `add_type` 而不是 `new_arith` - 移位运算不做常规算术转换
  (`add_type` 的 ND_SHL/ND_SHR 只写 `ty = lhs->ty`), 走 `new_arith` 会给 `x <<= y` 的操作数插一个
  今天不存在的 cast.
- 陷阱: `usual_arith_conv` 对 `ptr + n*4` 也会给两侧插 cast(`get_common_type` 一见 `ty1->base`
  就返回 `pointer_to(ty1->base)`), 这个 cast 到指针的节点今天确实在树里, codegen 手搓缩放和就会
  少掉它. 这是"必须经 `new_arith`"而不是"可以自己拼"的原因.

**契约 3 - 整形遍的不变量**
- (a) **后序**: 先整形子树再整形本节点(与 `add_type` 同). 外层降级依赖内层已降级 - A5.1 的
  `to_assign` member 分支读 `node->lhs->lhs`, 前提是 A3.1 已把 arrow 补成 DEREF.
- (b) **相位序**: 表达式/声明整形 -> 控制流标注(与 sema 的 `add_type` -> `analyze` 同). 原子 op=
  造出的 do-while, ND_DECL 展开出的语句, 都要在标注相位之前成形才拿得到标签.
- (c) **每函数上下文**: `current_fn`, 槽工厂(追加到 `fn->locals`), 标签计数器, 循环/switch 标签栈.
  全部槽必须在 `assign_lvar_offsets` 之前建完, 所以 `codegen()` 入口是
  "整形遍 -> assign_lvar_offsets -> emit_data -> emit_text".
- (d) **不重入**: codegen 自建的子树直接建成**已降级**形态, 只对全新节点调 `add_type` 定型;
  绝不对含旧节点的子树重跑整形遍. `add_type` 的 `if (node->ty) return` 能挡住二次**定型**, 挡不住
  二次**降级** - 指针会被缩放两次.
- (e) **访问一次**: 记录留在链里之后(A8.2), 嵌套 ND_FUNCDEF 的体既挂在宿主函数体内又在 `prog`
  上, 整形遍只能从 `prog` 进一次, 宿主体里遇到该记录要跳过其 body.
- (f) **初始化器不在自然路径上**: `ResolvedInit` 经 `node->init_resolved` 挂着的表达式不是
  lhs/rhs/body 的孩子, 整形遍与标注遍都不会自然到达, 两侧都要显式走(见契约 5).

**契约 4 - 物化顺序 = `.data` 块序**
匿名全局(字符串字面量, 文件域复合字面量, 块域 static)是**创建即入链**的: `new_gvar` 把它**前插**
到 `globals` 链首(`scan_globals` 保序重建), 所以 `emit_data` 发出的 `.data` 块序是创建序的**逆序**.
归一化**不折叠块序**. sema 的标注遍按函数**定义**序走, codegen 的整形遍按 `prog` 走, 而 `prog` 是
Obj 的**首次声明**序 - 先声明后定义的函数会让两者不同. 推论: **任何"创建匿名全局"或"写
`init_data`"的动作一旦搬到 codegen, `.data` 块序就可能变, 且 ndiff 会看见.** 因此 A9.1 的字符串
物化, A8.3 的复合字面量对象, A8.1 的块域 static 数据镜像与 `serialize_gvar`, 都必须留在 sema 的
同一遍历位置(函数内部的相对顺序两侧一致, 有风险的是跨函数顺序). 局部隐藏对象的顺序只影响栈偏移,
ndiff 第 3/4 类免疫, 可自由搬迁.

**契约 5 - 初始化器表达式的定型时机**
局部初始化器表达式(`ResolvedInit.expr`)今天不由标注遍直接到达, 而是由 `create_lvar_init` 建出
ASSIGN 之后那次 `add_type` 顺带定型(并顺带物化其中的字符串字面量, 例如 `char *p = c ? "a" : "b";`).
A8.1 把它搬走后, sema 的标注遍必须在**同一位置**(ND_DECL / ND_COMPOUND_LITERAL 的 case 里)显式对
每个 `init->expr` 调 `add_type`, 否则定型与物化一起后移到 codegen -> 违反契约 4. 这是 A8.1 里
唯一"新写的 sema 代码", 不是搬家.

## 现状锚点(开工前实测)

- sema 的整形分三处: `add_type`(case 分派: ADD/SUB 缩放, ASSIGN.op, INCDEC, GT/GE, SUBSCRIPT,
  MEMBER 补 DEREF, COND elvis, FUNCALL, STRING, SIZEOF/ALIGNOF, TYPES_COMPATIBLE/REG_CLASS,
  GENERIC, DEREF-of-func, DECL, COMPOUND_LITERAL), `type_chain`(链上记录摘除 + for-init 的
  ND_BLOCK 包装), `analyze`(标签分配, ND_WHILE→ND_FOR, break/continue→goto, case 链, label 名).
- **求值器搭了整形的便车**: `eval2`/`is_const_expr`/`eval_double` 开头就调 `add_type`, 于是求值
  switch 见到的都是整形后的形状(GT→LT, SUBSCRIPT→DEREF, STRING→VAR, SIZEOF→NUM, GENERIC→选中项,
  `arr+2`→`ADD(VAR, MUL(2,4))`); preprocess 的 `#if` 经 `const_expr` 走同一条路. 整形一移走, 求值器
  必须自己认忠实形态 - 这是本线第一处"新逻辑", 不是搬家. `write_gvar_data`(全局初始化序列化)经
  `eval2`/`eval_rval` 受同一影响.
- **求值器今天看不见缩放**: `eval2` 的 ND_ADD/ND_SUB 直接 `eval2(lhs,label) + eval(rhs)`, 因为
  缩放已经由 `new_add` 变成 `MUL` 子树. 缩放搬走后, eval 必须自己按元素大小放大, 并且要认
  `num + ptr`(规范化也搬走); ptr-ptr 必须以 `label == NULL` 求两侧, 才能复现今天的
  "not a compile-time constant"(今天那棵树是 `DIV(SUB(..),size)`, `eval2(ND_DIV)` 走的是
  `eval(lhs)`). VLA 元素在常量表达式里不参与(今天也不是常量).
- `resolve_member` 今天在**最内层 link** 上补 DEREF 并把 `arrow_tok` 置 NULL. 匿名成员展平后
  `p->x` 是 `MEMBER(MEMBER(DEREF(VAR p)))`, DEREF 属于最内层. 所以"不补 DEREF"的等价做法是:
  展平照旧, `arrow_tok` 留在需要解引用的那一层 link 上不清, 由 codegen 补 DEREF 并清标记.
- `to_assign` 的原子分支**预分配** `loop->brk_label`/`cont_label`(经 `new_unique_name`),
  `analyze_node` 的 `if (!node->brk_label)` 守卫就是为它留的. A2.1 把标签搬进 codegen 后这两处
  预分配必须同时删掉, 否则 sema 与 codegen 两个计数器会产出同名标签(`.L..5` 撞车 -> as 报重复标签).
- **`.L..%d` 是一个共用名字空间**: `new_unique_name` 的注释写明"字符串字面量全局, 块域 static,
  控制流标签"共用这一个计数器. 标签搬进 codegen 后必须解决撞名, 而且**不能换前缀** -
  `snapshot-normalize.awk` 的第 2 类只折叠 `\.L[A-Za-z0-9_.$]+` 且末段为纯数字的名字, 换成
  `.L.cg.5` 这类前缀后它仍会被重编号, 但重编号结果(`.L.cg.1`)与基线的(`.L..2`)不同, 于是一次
  纯重编号会让 ndiff 非空. 所以 codegen 必须继续发 `.L..%d`.
- `type_chain` 对 ND_FUNCDEF 调 `analyze_function`, 即"在宿主体的这个位置标注嵌套函数体";
  A8.2 只去掉"出链"动作, 这个标注调用必须留在原位(否则嵌套函数体里的字符串物化会换位置, 契约 4).
- **顺序敏感面**: 见契约 4.
- codegen 今天已自算 `pass_by_stack`(`push_args`); sema 写入而 codegen 读取的字段: `ret_buffer`,
  `brk_label`/`cont_label`, `unique_label`/`label`, `case_next`/`default_case`, `func_ty`,
  `Type::vla_size`.
- `codegen()` 入口顺序是 `assign_lvar_offsets → emit_data → emit_text`; 块域 static 初始化器里的
  `&&label` 经 `Relocation.label`(一个 `char **`, 指向 `node->unique_label`)在 emit 时才解引用,
  所以标签名只要在 `emit_data` 之前定下即可 - 整形遍在最前面, 满足.
- codegen 只拿到 `Obj *prog`, **拿不到顶层记录链**(main.c 是 `codegen(sema(parse(tok)), out)`).
  所以 A8.2 的"出链"只发生在函数体内的语句链上; 顶层记录本来就只在 sema 的输出里.

## 闸门口径

- **[搬迁] 步骤**(整形逻辑换位置, 发射形态不变): 硬闸门 = `make docker-test`(含自举, 内含
  `test/diagnostic.sh` 的 55 例逐字节) + `make docker-snapshot-ndiff` 为空 +
  `make docker-test-thirdparty THIRDPARTY=tinycc`; 同提交内 `make docker-snapshot` 重置 raw 基线
  (`-diff` 只作可读参考).
- **[就地] 步骤**(发射形态变化, 只在阶段 B): 硬闸门 = 上面三项 + 该步新增的形状断言;
  `docker-snapshot-ndiff` 允许非空, 但差异必须逐条归入预期模式并记入 `RESULT.md`.
- **tinycc 进每步闸门**(本线相对上一线的口径变化): 实测约 15 s, 而 RESULT-faithful 的 R2.5 记录
  证明 `docker-test` 的 41 文件语料对"tag-only 定义 + 不完整类型期写下的指针"这类形状是盲的, 只有
  tcc 的源码抓到过真误编译. 本线搬的是 ~800 行降级逻辑, 不值得为省 15 s 复用那个盲区.
  命令用 docker 版: 本机是 macOS/arm64, `make test-thirdparty` 会直接报错退出.
- **非空转验收**: A1.1/A1.2 这类"先埋好, 当前不可达"的步骤, 提交前必须用一次性探针证明新代码真的
  会被走到(临时把对应降级关掉, 重编, 确认输出不变且新 case 命中; 探针不入提交). 每个 eval 的忠实
  case 必须在其对应降级搬走的那一步**之前**有用例覆盖; 没有就在该步补 `test/*.c` 或 `#if` 用例, 不留
  "搬完才发现 eval 那条路没人走过".
- **诊断锁定只对"单一错误路径"的诊断有意义**(A0.1 实测): 一条诊断若有两个都会失败的兄弟子树,
  报哪一个取决于 C 不规定的操作数求值顺序, 而宿主 clang 先求 lhs, chibicc 自举出的 stage2 先求
  rhs(codegen 的整数二元运算是 `gen_expr(rhs); push; gen_expr(lhs); pop`) - 同一份期望在
  `make test` 与 `make test-stage2` 里必然有一轮失败. 加用例前先问"这条诊断只有一个可能的锚点吗";
  多路径的形状改用正向快照锁(见 `RESULT.md` 的结构性限制一节).
- 一步一提交, 返工 = revert 单个提交; 每步汇报固定四项(改了什么 / 为什么 / 闸门结果 / 偏差).
- 红线互换: codegen 的"零改动红线"本线作废(本线的目标就是改 codegen); 新红线 = **库层
  (parse.c / sema.c / type.c / preprocess.c / tokenize.c)不得再出现后端整形**(指针缩放, 读写回环,
  临时槽, 语句重排, 标签与唯一名分配), 且不得调用 codegen(契约 1).
- 本线不做(仍属库化阶段 5): 错误回调化, context 对象, 公共/内部头拆分, arena reset.

## 阶段 A: 边界搬迁(整形归 codegen, 发射形态不变)

**执行顺序**: A0.1 -> A1.2 -> A1.1 -> A2.1 -> A3.1 -> A5.1 -> A4.1 -> A6.1 -> A9.1 -> A7.1 ->
A8.1 -> A8.2 -> A8.3 -> A9.2 -> A10.1 -> A10.2.

- [x] **A0.1 账本与基线**: `RESULT.md` 的"整形清单表"补上"验收口径"列与漏项(指针算术的结论,
  `resolve_member` 清 arrow_tok, 嵌套函数体访问一次, 原子 do-while 的预分配标签, 初始化器表达式
  的定型时机); 诊断锁定补 f 系列 9 例把"夹在降级里的检查"锁住(44 -> 53, 见判据 2); 复验四闸门
  (docker-test / 诊断 53 例 / ndiff 空 / tinycc)并留档为本线基线; 顺带记录一个基线缺陷
  (`1 - p` 段错误, 上游 07f9010 起即如此, 本线不修).
- [x] **A1.2 结论字段与导出面**(原 A1.2 前移, 因为求值器要读结论): Node 增设结论槽并让 `add_type`
  **写**结论 - 本步**仍然照旧替换节点**(消费方到 A9.1 才改读字段), 所以零行为变化; 字段先长出来,
  A9.1 就只是"删掉替换 + 消费方改读". 实测需要的字段只有一个: `_Generic` 的选中项(节点引用);
  sizeof/alignof 与两个类型 builtin 的结论都是数值, 复用 `val` + `ty`(与 ND_NUM 同槽, A9.1 的消费方
  可以统一读), 不新增字段. 同一步把契约 2 的永久导出面在 `chibicc.h` 声明出来(去 static + 声明,
  无行为变化). 指针算术**不设**结论字段: 哪一侧是指针, 元素类型是什么, 都可以从 sema 已写下的 `ty`
  读出来, 加字段反而多一处要同步的真值(若实施时发现两侧各写一遍判断容易漂, 再回来加
  `Type *ptr_base`, 记为偏差).
- [x] **A1.1 求值器认忠实形态**(原 A1.1 后移): `eval2`/`eval_rval`/`eval_double`/`is_const_expr` 补
  忠实 case - ND_GT/ND_GE(就地判), ND_SUBSCRIPT(按下标取址), ND_MEMBER 的 arrow(指针值 + 偏移,
  `eval_rval` 与 `eval2` 两侧都要), ND_STRING(读物化后的 `node->var`), ND_SIZEOF/ND_ALIGNOF
  (定长读结论, VLA 情形不是常量), ND_TYPES_COMPATIBLE/ND_REG_CLASS/ND_GENERIC(读结论),
  ND_INCDEC 与带 op 的 ND_ASSIGN(非常量, 保持 "not a compile-time constant" 文案; 值形下标不设
  case - 降级把 DEREF 写在同一节点上, 末尾 error 的文案与锚点天然相同). `#if` 路径随 `const_expr`
  一并覆盖. **执行修正(见 RESULT.md 偏差)**: ND_ADD/ND_SUB 的元素大小缩放与 num+ptr 认识
  **移入 A4.1** - 这两个 case 的槽位为降级/忠实两个时代共享, 降级形态的 rhs 已含缩放乘积,
  缩放支在 A4.1 之前无条件存在会二次缩放(initializer.s 实测 `.quad g11+8` 变 `g11+64`);
  ptr-ptr 除法支今日形状是 DIV, 休眠安全, 留在本步. 缩放路径的语料覆盖已由 initializer.c 的
  `.quad` 值锁住, A4.1 搬运时快照闸门即时核对; 正向快照锁的用例仍归 A10.2.
  验收: 常规口径 + 非空转探针(8 组, 见 RESULT.md) + 逐 case 覆盖登记, 没覆盖的在 A10.2 或对应搬运步补.
  **已识别的覆盖空洞(A0.1/A1.2 期间实测)**: `is_const_expr` 只有一个真调用者 - `resolve_type`
  用它判数组维度是定长还是 VLA. 今天 `int n=5; int x[n]; int y[sizeof(x)];` 之所以正确(y 是 VLA),
  是因为 `add_type` 先把 sizeof 折成 `COMMA(算尺寸, 读尺寸变量)`, `is_const_expr` 看到的是 COMMA ->
  VAR -> false. A9.1 停止折叠后, `is_const_expr(ND_SIZEOF)` 必须自己按"操作数类型是不是 VLA"返回
  false, 否则一个 VLA 会被静默当成定长数组. 而 `test/vla.c` 里**没有**"sizeof(VLA) 作维度"的用例
  (只有 `sizeof(char[2][n])` 作表达式), 所以这条要在 A1.1 就补进 test/vla.c, 不能等 A9.1.
  (已补: `ASSERT(80, ...)`, 期望值 clang 验证.)
- [x] **A2.1 标签与控制流归 codegen**: codegen 新增整形遍骨架(契约 3 的 (b)(c)), 标签计数器与
  "循环/switch 标签栈", `analyze` 的整段搬入(标签分配, ND_WHILE→ND_FOR, break/continue→ND_GOTO,
  case 链, ND_LABEL 与 `&&label` 的名字); **同时删掉 `to_assign` 原子分支的两处预分配**(否则标签
  撞名; 原子环无标签到达, 整形遍的守卫随之删除 - 恒真分支). 名字空间按"现状锚点"的约束处理:
  格式仍是 `.L..%d`, codegen 在 `codegen()` 开头向库取一次计数器的下一个值作为基址(库侧只多一个
  只读 getter, 标签**分配**归 codegen, 红线不破); 若用户更愿意让名字工厂搬到中性文件由两侧共用
  一个计数器, 也可行, 记为偏差. sema 只保留 stray 四检查与 goto/label 配对检查(纯检查, 不写名字,
  配对改成比名字字符串; 检查下降只跟踪循环/switch 嵌套深度, 文案与锚点不变).
  **执行补充(见 RESULT.md 偏差)**: `&&label` 在初始化器里的节点只有 sema 的收集链可达
  (`add_type` 的 ND_LABEL_VAL case), 契约 3(f) 的"两侧都要显式走"落地为 **Obj.label_gotos 交接
  字段** - sema 的检查遍把每函数的 goto/label-value 链存到 fn 上, codegen 的整形遍用它做解析
  (拿到的是收集好的引用链, 自己的下降只收集 ND_LABEL). `codegen()` 入口改为
  "整形遍 → assign_lvar_offsets → emit_data → emit_text".
  验收: ndiff 空 - 依据是归一化第 2 类按**文件内首现顺序**重编号, 与本步无关的只有名字取值:
  数据段标签之间的相对顺序不变(物化没动), 文本段标签之间的相对顺序不变(同一遍下降), 数据段整体
  仍在文本段之前, `&&label` 的 `.quad` 仍紧跟它所属 static 的块标号 - 所以首现序列恒等. 若 ndiff
  非空, 说明某个标签的出现顺序真的变了, 必须归因, 不许直接重置基线.
  (实测: ndiff 空; raw diff 2008 行**全部**为 `.L..` 编号变化, 0 行非标签 - 同提交重置基线并复验;
  .s 无重复标号; sema 的 new_unique_name 只剩 new_anon_gvar 一个调用者.)
- [x] **A3.1 成员与下标**: ND_SUBSCRIPT 的 `*(x+y)` 降级(codegen 经导出的 `new_add` 建, 契约 2),
  ND_MEMBER 的 arrow DEREF 补插搬 codegen; sema 侧 `resolve_member` 改为**不清 `arrow_tok`**, 把它
  留在需要解引用的那一层 link 上(展平与绑定不变), codegen 补完 DEREF 后清标记; sema 保留
  `invalid pointer dereference`/`dereferencing a void pointer`/`not a struct nor a union`/
  `no such member`/`cannot take address of bitfield` 检查; `*foo`(函数指针消解, 6.5.3.2p4)按判据 1
  留 sema 定型, 不再替换节点. 预期偏差: 保留 ND_DEREF 会让 `gen_expr` 多打一条 `.loc`, raw diff
  非空 / ndiff 空(第 1 类折叠), 提前记账.
  (实测: ndiff 空(混合基线), raw 恰 4 行 `.loc` 增行(`(*add2)` 1 + `(***add2)` 3); to_assign 的
  member 分支在中间态下需把带 arrow_tok 的操作数包回 DEREF, `x[s]`/`f[0]` 两个崩溃形状转为
  `invalid operands` - 均为计划未列偏差, 见 RESULT.md; `new_add` 依契约 2 临时导出, A4.1 撤.)
- [x] **A5.1 复合赋值与自增自减**(先于 A4.1, 契约 1): `to_assign`/`compound_op`/`new_inc_dec` 搬
  codegen, 其中对 `new_add`/`new_sub` 的调用改成调导出符号; 普通与位域成员情形落成"地址一次求值 +
  读改写", 原子 op= 的 do-while + CAS 语句构造用 codegen 自己的槽与标签(标签由 A2.1 的标注相位给,
  本步不再预分配); sema 保留"赋值给数组"等检查.
  (实测: ndiff 空; raw 1248 行全部为栈偏移类(临时量改由 codegen 的槽工厂链首前插), 零指令/标签/
  `.loc` 变化, 同提交重置基线; combine 随两个调用者一并搬走; A3.1 的 member 分支 DEREF 回插适配
  在新时序下死代码删除 - 后序整形已把 arrow 解析完, to_assign 读到的就是 `MEMBER(DEREF(p))`;
  原子环的标签经 `shape_node(loop)` 就地分配(改写在遍内发生, 遍到不了新建循环), 相对顺序不变;
  槽工厂为 codegen 自建 4 行, 不导出 sema 的 new_var(push_scope 是语义层状态), 见 RESULT.md 偏差;
  eval 侧零改动, e1/e4 错误锚点逐字节复验.)
- [x] **A4.1 算术与比较**: `new_add`/`new_sub`/`scale_rhs`/`combine` 与 GT/GE 交换搬 codegen;
  `new_arith`/`usual_arith_conv`/`get_common_type`/`new_cast` **留 sema 并导出**(判据 4 + 契约 2);
  sema 的 ND_ADD/ND_SUB case 改为: 跑 `invalid operands` 检查, 定型(含 `usual_arith_conv` 的隐式
  cast), 不缩放不换序. 到这一步 `new_add`/`new_sub`/`scale_rhs` 的临时导出声明随搬删除.
  **同一步把求值侧的指针缩放补进 eval2 的 ND_ADD/ND_SUB case**(自 A1.1 移入: 降级离开后这里成为
  唯一缩放 - 指针侧经 conv 之下找, `num+ptr` 认左手, VLA 元素尺寸报非常量; 语料的 `.quad` 值与
  A10.2 的正向快照锁核对), ptr-ptr 除法支已在 A1.1 就位.
  **执行补充(见 RESULT.md 偏差)**: (1) sema 的定型不再插 conv cast(重整与二次 cast 风险, 由
  codegen 侧 new_arith 补上, 与降级时代逐字节一致); (2) eval2 的 ADD/SUB 缩放直接读操作数自身
  `ty`(忠实树无可穿透的 conv cast), `num+ptr` 的指针侧经 eval2 带 label 求值(保 `2 + gp` 的
  "invalid initializer" 锚点), `ty_beyond_convs` 随 A1.1 的用途消失而删除; (3) **两个计划未列的
  重入缺陷被快照抓到并修复**: elvis 降级留下的陈旧 cond/then/els 别名(同一对象两处引用, 整形遍
  沿陈旧字段二次下潜)在降级处断开; 原子 retry 环的 `shape_node(loop)` 下潜改为直接分配
  brk/cont 标签(环形无控制流可整形, 下潜会重跑表达式 case). 修复后 raw diff **全空** - 本步不
  新建任何局部槽, 栈偏移与标签分配顺序逐字节一致, 基线无需重置.
  注意: `new_sub` 的 VLA 分支今天缺 `lhs->ty->base &&` 守卫, `1 - p` 会段错误(上游 07f9010 起
  即如此, 见 `RESULT.md` 的基线缺陷记录). 搬运步**原样搬**, 不顺手补守卫 - 那是行为变化, 要修
  另起一个提交并由用户拍板. (2026-09-29 已由用户拍板修复: 守卫已就位, 搬运的是修复后的版本.)
- [x] **A6.1 函数调用**: `lower_funcall` 的检查(不是函数, 实参个数, float 提升)与实参转换(隐式
  cast, 判据 1)留 sema, `func_ty` 标注留 sema; 只有调用者返回缓冲(`ret_buffer`)的槽创建搬 codegen
  (用契约 3(c) 的槽工厂; 偏移由归一化第 3 类吃掉).
  (实测: 语料 raw diff **全空**(本步不引入语料可见的偏移变化 - 整形遍的创建顺序与标注遍一致);
  sret 探针(多结构体返回混声明)偏移整体移动(diff 66 行全部为栈偏移操作数), docker 内实测语义
  正确("sret ok") - 该探针归 A10.2 的"大结构体返回缓冲"用例; shape_node 的 ND_FUNCALL case 经
  shape_children 后建槽, 与 gen_expr 读 ret_buffer 的时机(emit 时)一致.)
- [x] **A9.1 结论类的消费侧**(先于 A7.1, 契约 1): ND_STRING 物化后只记 `node->var` 而不改 kind
  (物化位置与顺序不变, 契约 4), codegen 的 `gen_addr`/`gen_expr` 增 ND_STRING case(同 tok, 同指令,
  预期 raw 也逐字节相同); ND_SIZEOF/ND_ALIGNOF/ND_TYPES_COMPATIBLE/ND_REG_CLASS/ND_GENERIC 不再被
  替换, 消费方读结论字段 - 其中 **sizeof(VLA) 仍是 codegen 的整形项**(它要建 `COMMA(算尺寸, 读尺寸
  变量)` 这棵运行期表达式, 本步经临时导出的 `vla_size_expr` 建, A7.1 再把该函数搬过来);
  ND_COND 的 elvis 降级搬 codegen, **阶段 A 保持字节等价**: 仍建临时槽, 仍落成 `tmp = a, tmp ? tmp : b`
  (无槽就地发射是发射形态变化, 归 B1.1, 不在阶段 A 混入 [就地] 口径).
  **执行补充(见 RESULT.md 偏差)**: (1) `vla_size_expr` **未导出也未保留** - 实测它在 codegen 侧
  不可用: compute_vla_size 里的 `new_lvar` 是 sema 的槽工厂, codegen 调用会把尺寸变量挂进 sema 的
  陈旧链(偏移 0, 栈帧损坏); 改为 codegen 自建 `shape_compute_vla_size`/`shape_vla_size_expr`
  (镜像文本, 用自己的槽工厂), sema 的 vla_size_expr 随折叠取消成为死码删除 - 契约 2 的临时导出
  列表提前清空, A7.1 只剩搬 compute_vla_size 与 sema 侧收尾; (2) elvis 的 codegen 降级必须
  **先清 `node->ty`** 再 `add_type`(sema 结论已在场, 不清则新树完全未定型, gen_expr 崩在 store(NULL));
  (3) 移除 eager fold 后, 维度里 `sizeof(x)` 不再内联重算尺寸(基线在 resolve 折叠时重算过一遍),
  vla.s 的 ndiff 残留恰为这段冗余计算删除 - 语义经 docker 实测与 gcc 逐点一致(含 n 改写角落:
  `sizeof(x)=20`(声明时定值)/`sizeof(int[n])=28`(类型名即时求值), 基线对前者给 28 是错的);
  按 [就地] 口径归因后同提交重置基线; (4) generic 的消费方委托多一条 `.loc`(归一化折叠).
  另发现并修正对比脚本缺陷: 双端"静默失败"曾被计为一致(缺 rc 与产物存在性检查), 本步电池已改严.
- [x] **A7.1 VLA**: `compute_vla_size`/`vla_size_expr` 与 `Type::vla_size` 的写入搬 codegen(字段
  注释改标"codegen 侧缓存"), VLA 指针的缩放随 `scale_rhs` 已在 codegen; sema 保留维度求值与
  "VLA 不得初始化"检查. 前置检查: 此时 sema 里不得再有任何读 `ty->vla_size` 的代码(逐个 grep
  确认: `scale_rhs` 已走, `vla_size_expr` 已走, eval 的 sizeof(VLA) 不是常量所以不读).
  **执行补充(计划未列, 见 RESULT.md 偏差)**: 声明侧不能只搬函数 - sema 的 ND_DECL **保留**
  持 VLA 的声明记录(物体自身或指针基类型), 链(尺寸计算 + alloca 赋值 + 可选的初始化器链)整个
  由 codegen 的 shape_node(ND_DECL) 展开; 因指针到 VLA 可带初始化器, `lvar_init_comma` 依
  契约 2 临时导出(A8.1 撤). 维度表达式先显式整形(类型是它们唯一引用; 以 `ty->vla_size` 已设
  区分 typedef 共享类型的第二次声明). 实测: 语料差异**全部为栈偏移**(4 文件, 0 .loc 0 指令),
  ndiff 空, 同提交重置 raw 基线; 指向 VLA 的指针(带/不带初始化器)、typedef 共享 VLA 类型、
  sizeof(VLA)、维度里 sizeof 五组探针与 A9.1 基线逐字节一致, docker 运行时断言全过.
- [x] **A8.1 初始化器消费侧 + ND_DECL 展开**(本线最大一步, 三件事必须同提交, 契约 1): 
  (i) `ResolvedInit`/`InitDesg` 移入 `chibicc.h` 并命名归位(`InitTree`/`InitPath`), 
  `create_lvar_init`/`lvar_init_comma`/`init_desg_expr` 搬 codegen(`init_desg_expr` 直接建已降级
  形态, 不再产 ND_SUBSCRIPT, 契约 3(d)); 
  (ii) sema 的标注遍在 ND_DECL / ND_COMPOUND_LITERAL 的 case 里**显式定型每个 `init->expr`**
  (契约 5, 本步唯一的新 sema 代码), ND_DECL 只留检查(VLA 不得初始化, 不完整类型, void)与块域
  static 的数据镜像(`gvar_init_data` 位置不变), 不再改写 kind, 不再设 `decl_remove`; 
  (iii) codegen 把 ND_DECL 展开成 0..n 条语句(VLA 的 alloca 赋值, MEMZERO + 赋值回环, 无初始化器的
  定长对象不产语句)并把它从链上摘掉, for-init 的 ND_BLOCK 包装随之搬到 codegen(它本来就是
  "展开成多于一句"的产物). 语句表达式的值语义按忠实形状重述: 末语句是记录(声明)即无值, 报
  "statement expression returning void is not supported"(与锁定用例 e11 同锚点, 文案不变).
  文件域序列化(`write_gvar_data`/`gvar_init_data`/`serialize_gvar`)留 sema.
  **执行修正(见 RESULT.md 偏差)**: (1) 语句表达式的值语义**无需改动** - 旧捕获本就发生在降级之前,
      忠实树让"末语句是记录"直接可比, sema 的判定与报错位置/文案逐字节不变(e11 闸门锁定); (2) 初始化器
      表达式用**独立 walker**(`annotate_init_exprs`/`shape_init_exprs`)显式走, 而非对建出的链整体
      下潜 - 后者会把自建的 DEREF/ADD 再喂给表达式 case(实测 `int x[3]={1,2,3}` 报 invalid operands);
      顺带消除 vla.s 里共享表达式二次整形产生的 no-op cast; (3) 复合字面量的**块域**展开随之搬入
      codegen(它调用 lvar_init_comma), 文件域仍留 sema 序列化 + ND_VAR 改写 - A8.3 的清单据此收窄.
- [x] **A8.2 其余记录出链**: `type_chain` 对 ND_TYPEDEF/ND_ENUM_CONST/ND_GVAR_DECL/ND_FUNCDEF 的
  摘除搬 codegen - 记录保留在 sema 输出里(库消费者可见, 是"直连"的应有之义); sema 只去掉"出链"
  动作, `serialize_gvar` 与"在此位置标注嵌套函数体"的调用**留在原位**(契约 4); codegen 侧按
  契约 3(e) 保证嵌套函数体只从 `prog` 进一次. 到这一步 sema 的 `decl_remove` 静态变量应当消失.
  (实测: 全量 A/B 逐字节一致(含 vla.s - A8.1 的 .loc 差异在基线重置后自然消失), raw diff **全空**;
  嵌套函数(带 typedef/enum/extern 的体内记录)与 for-init 的 enum+声明链探针逐字节一致;
  `type_chain` 降为纯遍历(`Node *` 签名), 摘除/展开全在 codegen 的链编辑 walker; 嵌套体从
  `prog` 单次进入, 宿主链上只摘记录不下潜.)
- [x] **A8.3 复合字面量**: 无名字对象的创建留 sema 的 resolve 遍(顺序不变, `.data` 块序保真),
  `gvar_init_data` 留 sema 的标注遍, "初始化链 + 对象引用"的整形归 codegen(复用 A8.1 已搬的
  `lvar_init_comma`).
  (实测: 三项内容已在 A8.1 一并落地(A7.1 的临时导出把块域展开提前牵走), 本步为**验证提交**,
  无代码改动: 语料 complit.c/initializer.c 覆盖设计符、括号后成员/下标、函数定义前中后的文件域块序;
  新增探针覆盖块域(成员访问、函数实参、下标、`&(T){...}`、循环内新鲜对象)与文件域(数组、指针初始化、
  指定初始化、`mid()` 定义前后)两组 + 六目录全探针, 与基线逐字节一致; docker 运行时断言全过
  ("complit-a83 ok"); 四闸门全绿(raw 全空). 两个预存在限制记录在案(基线同报错): 复合字面量直接跟
  `.`/`[...]`(无外层括号)在 parse 层报 "expected ','"; 块域 static 以复合字面量初始化报
  "not a compile-time constant".)
- [x] **A9.2 add_type 收尾核对**: `add_type` 至此应为纯标注遍 - 填 `ty`, 插隐式 cast, 做检查,
  写结论, 物化(位置不变), 绑定成员; 逐项核对账本表, 确认没有任何 case 改写树形状. 同一步撤掉
  契约 2 的临时导出面, 并按新红线 grep 佐证库层(缩放 / 读写回环 / 槽创建 / 语句重排 / 标签分配 /
  调用 codegen 均为 0 处).
  (实测: 34 个 case 逐项核对(表见 RESULT.md), 全部纯标注 - **审计发现并修掉最后一处形状改写**:
  文件域复合字面量原被改写为 ND_VAR, 现保留 kind, 由 eval 侧新增的 ND_COMPOUND_LITERAL case 直接
  读 `node->var`(A1.1 的字符串模式); 契约 2 临时导出面已为空(A8.1 起); 红线 grep 全 0 - 缩放/
  读写回环/标签分配(仅剩 new_anon_gvar 一个调用者)/语句重排/建槽(6 处全为参数/ABI/声明/复合字面量
  对象)/调用 codegen 均为清; 四闸门全绿, raw 全空.)
- [x] **A10.1 归属清扫与文档**: `chibicc.h` 逐字段标注归属(codegen 写入的字段: `ret_buffer`,
  `brk_label`/`cont_label`, `unique_label`/`label`, `case_next`/`default_case`, `pass_by_stack`,
  `Type::vla_size`, 结论槽), sema.c/codegen.c 文件头注释重写(codegen 头要写清整形遍的六条不变量),
  库层"无整形"的 grep 佐证留档, AGENTS.md 的目标架构/现状段/硬性规则按终态更新(含 static 状态清单
  去掉 `decl_remove`).
  (实测: chibicc.h 的 8 处归属标注(brk/cont 标签、pass_by_stack/ret_buffer、label/unique_label/
  goto_next、case 链、is_elvis、ty_op、generic_sel、Type::vla_size 已在 A7.1 标注); sema.c 头部
  的"标注+降级"改为"纯标注遍(无形状改写)"并列出物化清单; codegen.c 新增文件头(整形遍定位 + 契约 3
  六条不变量 + 字段归属指引); AGENTS.md 的 static 清单(10 个, 去掉 brk_label/cont_label/
  current_switch/decl_remove 等已迁出项)、目标架构第 4 条、现状段与代码地图按终态重写; 四闸门全绿,
  raw 全空(纯注释/文档).)
- [x] **A10.2 针对性回归**: 补测试覆盖忠实形态的发射面, 以及本次分析新识别的薄弱面 -
  `a[i] += j++`(下标 + 复合赋值 + 后缀自增), `p ?: q`, 大结构体返回值缓冲, VLA 尺寸复用与
  `sizeof(VLA)`, `_Generic` 结果, 位域 op=, 原子 op=, `&&label` 的块域 static 初始化器,
  语句表达式末语句为记录, **全局初始化器里的 `int *p = arr + 2;` / `int *q = &s.x + 1;` /
  `long d = a - a;`**(eval 缩放路径; 前两例实测发 `.quad a+8` / `.quad s+4`, 第三例必须报
  "not a compile-time constant"; 注意 `char *s = "abc" + 1;` 今天在 parse 就报 `expected ','`,
  不是可用形状), **先声明后定义的函数各放一个字符串字面量**(契约 4 的块序探针, 实测形状见
  `RESULT.md`: `__func__`/`__FUNCTION__` 让每个函数在 `.data` 里至少占两块, 块序是 resolve 遍
  定义序的逆序), **嵌套函数体里带循环与 `&&label`**(契约 3(e)),
  **共享同一个 VLA 类型的两个声明**(`typedef int T[n]; T a; T b;`, `Type::vla_size` 只算一次),
  **初始化器里含字符串字面量的三元表达式**(契约 5 的物化时机探针).
  (实测: 八文件补测(arith/struct/vla/generic/bitfield/atomic/initializer/function), 断言值全部经
  docker 运行时验证; 全局初始化器探针(`gsel+2`/`&gpair.b+1`)与 docker 语义核对; **嵌套函数项触出
  两个预存在缺陷并当步修复**(见 RESULT.md A10.2: resolve/analyze 的 `locals` 与 `sema_fn` 未跨嵌套
  保存 - 宿主帧辅助对象偏移 0 且内层返回类型污染外层, A5.1 基线起即坏, 语料从未运行过嵌套函数;
  修复后 m4/m5/nl/function.c 全过, 外层变量捕获(静态链)仍为原有未支持限制). 快照基准随语料变化
  重置(本地 A/B 证明编译器维度中性: 36/41 逐字节 + function.c 恰为修复本身 + 4 个 `.file` 伪影);
  四闸门与 tinycc 全绿. 语句表达式末语句为记录项: 既有 e11 诊断锁定已覆盖. ptr-ptr 负例仍不可
  逐字节锁(结构性限制, 见 RESULT.md).)

## 阶段 B: 就地化(让"直连"名副其实, 可裁)

阶段 A 之后 codegen 仍先把 sema 的树整回旧形状再发射; 阶段 B 把"只需换个发射方式"的项从整形遍
搬进发射点, 让整形遍只剩"需要语句重排或新槽"的少数项(原子 op=, 返回缓冲, VLA, elvis, ND_DECL).
A 完成即已达成"取消 sema 降级"; B 由用户决定是否做.

- [ ] **B1.1 发射点直读(表达式层)**: ND_SUBSCRIPT, ND_MEMBER(arrow), ND_GT/ND_GE, ND_STRING,
  ND_SIZEOF/ND_ALIGNOF, ND_GENERIC, elvis(改成条件值留在 rax, 不建槽)在 `gen_addr`/`gen_expr`
  就地处理, 对应整形 case 删除; 每项一个提交([就地] 口径).
- [ ] **B1.2 发射点直读(语句层)**: ND_WHILE, ND_BREAK/ND_CONTINUE(标签栈), ND_DECL(初始化链
  就地摊成语句)由 `gen_stmt` 直接处理, 整形遍对应部分删除.
- [ ] **B1.3 形状断言**: 对 .s 加模式断言(下标不再出现中间 DEREF 形, elvis 不再出现槽, while
  不再出现 FOR 形等), 与行为闸门共同作为阶段 B 的验收.

## 开放决策(实施时由用户拍板)

- **阶段 B 是否做**: A 是"取消 sema 降级"的充分条件; B 让它名副其实, 但引入 [就地] 口径与形状
  断言成本.
- **结论字段的形态**: 泛型选中项与 sizeof 结论是各占一个 Node 字段, 还是复用 `ty_op`/`lhs` 等既有
  槽(需注释区分).
- **指针算术要不要结论字段**: 本计划默认不加(从 `ty` 读), 代价是 eval 与 codegen 各写一遍"哪一侧
  是指针"的判断; 若嫌漂, 加 `Type *ptr_base` 即可, 属判据 4 的正常用法.
- **`InitTree` 的公共性**: 初始化器解析结果进 `chibicc.h` 即成库 API 面(命名与不透明程度待定);
  若不暴露, 只能让 codegen 反向依赖 sema 的遍历函数, 与库化方向冲突.
- **标签分配归属**: 本计划把标签名分配放 codegen(它同时负责 break/continue 的形状); 若希望 sema
  继续持有名字, 只把形状补上, 也可行(代价是 sema 仍需持有计数器的交织顺序).
- **`.L..%d` 名字工厂的归属**: codegen 取基址(库多一个只读 getter) vs 工厂搬到中性文件两侧共用
  一个计数器. 前者更符合"标签分配归 codegen"的红线, 后者更简单且天然不撞名; 两者都保持名字格式,
  所以 ndiff 口径一致.
- **库 API 面**: 本线让函数体内的声明记录留在树里; 顶层记录链是否也作为 sema 的返回值暴露(现在
  是 `Obj *sema(Node *)`, 而 codegen 只拿 `Obj *prog`), 属库化阶段 5 的 API 设计.

## 风险与对策

1. **`add_type` 一分为二是大爆炸点**: 对策 = 按契约 1 的调用图逐批搬运, 每批"标注侧减法 + 整形侧
   加法"同一提交、独立可绿; 每批都用 ndiff 校验"发射形态未变"; 搬过去的代码尽量保持文本原样(只把
   对 sema 内部函数的调用换成导出符号), 让 review 能按 diff 逐行对; 出现无法归因的差异时以
   `git worktree add /tmp/cbase <上一个绿提交>` 逐字节对照两个二进制的 .s(同路径, 见
   RESULT-faithful 的 A/B 规程).
2. **诊断漂移**: 每次提交跑 `test/diagnostic.sh`(含在 docker-test 里); 夹在降级里的检查(A5.1 的
   `not an lvalue`, A4.1 的 `invalid operands`, A6.1 的实参个数/不是函数, A7.1 的 VLA 不得初始化,
   A8.1 的不完整类型与语句表达式形状)先在新位置显式落地, 再删降级代码.
3. **`.data` 块序**: 契约 4; 物化与 `init_data` 写入一律留标注遍, A10.2 有专门探针.
4. **帧布局与 ABI**: 参数表顺序(`fn->params`, pass-by-stack 参数的 ABI 偏移)不得动; 返回缓冲与
   VLA 尺寸变量属 `locals` 侧新增, 只影响栈偏移(ndiff 第 3/4 类免疫). 槽一律在整形遍里建, 建完才
   跑 `assign_lvar_offsets`.
5. **原子 op= 的语句构造**: 今天依赖 sema 的 `new_lvar`/`new_unique_name`/ND_DO 标签; 搬进
   codegen 时用 codegen 自己的槽与标签分配(A2.1 已删掉预分配), 并核对 CAS 循环的跳出语义与
   `.L..N` 名字.
6. **标签撞名与名字空间**: `.L..%d` 今天由 sema 的一个计数器供三类对象共用(见"现状锚点"). A2.1
   之后必须只有一个来源: codegen 以库的计数器下一个值为基址, 或两侧共用一个工厂. 每步提交前 grep
   `new_unique_name` 在 sema 里的残留调用点, 确认剩下的只用于"语言规定存在的对象"(匿名全局,
   块域 static, 复合字面量), 不用于标签. 廉价自检: 对 .s 抓重复标号
   (`grep -o '^[.]L[^:]*:' x.s | sort | uniq -d` 为空).
7. **求值器的静默错值**: eval 走忠实形态是本线唯一的新逻辑, 而它的错值不一定让测试失败(例如少乘
   一个元素大小, 只在 `arr + 2` 这类初始化器上可见). 对策 = A1.1 的逐 case 覆盖登记 + A10.2 的
   全局初始化器探针 + 每步的 tinycc(tcc 自己的源码里满是这类初始化器).

## 终态验收(本线完成时逐项实测填写; 阶段 A 完成于 2026-10-08, 提交 4aa1615)

- **库层**: [完成] `add_type` 34 case 逐项审计无形状改写(A9.2 节表); sema.c 的缩放/读写回环/
  标签分配/语句重排/调用 codegen 经逐项 grep 均为 0 处, 建槽仅剩语言与 ABI 对象(6 处 new_lvar,
  A9.2 节); 求值器与全局序列化在忠实形态上工作(A1.1 + 各步 case); `decl_remove` 已消失(A8.1);
  后加的两处上下文修复(A10.2 嵌套函数)与文件域复合字面量读 var(A9.2)已入档.
- **codegen**: [完成] 整形遍含全部搬迁项(阶段 A 后的形态), 契约 3 六条不变量书写于文件头注释;
  帧布局与 ABI 与红线一致(参数表/返回缓冲/VLA 尺寸变量各步 ndiff/raw 记录在案).
  阶段 B(整形项就地化)未做 - 计划内属用户开放决策, 其验收(少数整形项 + 形状断言)不适用.
- **闸门**: [完成] 每步 docker-test(含自举与诊断 55 例逐字节) + tinycc + 快照口径记录于
  RESULT.md 各步; A8.1/A10.2 两次因语料变化按先例重置快照基准(编译器维度由本地 A/B 证明中性).
- **对比锚点(实测)**: parse.c 2148 / sema.c 2991 / chibicc.h 821 / codegen.c 2556
  (起始 2148 / 3145 / 704 / 1595; sema 净 -154, codegen 净 +961, chibicc.h +117
  - 全部为搬运与注释的账目, 见 RESULT.md 各步行数).
