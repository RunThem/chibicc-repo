# RESULT.md - codegen 直连 sema 产物执行记录

本文件记录 `PLAN.md`(codegen 直连 sema 产物线, 取消 sema 降级)各步骤的实际执行结果, 供审核与
后续会话接续参考. 前两条线已归档: 语法语义拆分见 `PLAN-split.md` / `RESULT-split.md`, 忠实层
收尾见 `PLAN-faithful.md` / `RESULT-faithful.md` - 后者记录的终态(20abd43)是本线的量化对比基线.

## 闸门口径(本线生效)

- **[搬迁] 步骤**(整形逻辑换位置, 发射形态不变): `make docker-test`(含自举) +
  `test/diagnostic.sh`(53 例逐字节) + `make docker-snapshot-ndiff` 为空 +
  `make docker-test-thirdparty THIRDPARTY=tinycc`; 同提交内 `make docker-snapshot` 重置 raw 基线.
- **[就地] 步骤**(发射形态变化, 只在阶段 B): `make docker-test` + 诊断锁定 + tinycc +
  该步新增的形状断言; ndiff 允许非空, 差异须逐条归入预期模式并记在本文件的偏差记录里.
- 相对上一线的两处口径变化: tinycc 从"[就地] 才跑"提升为**每步都跑**(15 s, 而 R2.5 是
  docker-test 漏掉真误编译的书面记录); 命令用 docker 版(本机 macOS/arm64, `make test-thirdparty`
  会直接报错退出).

## 基线记录(A0.1, HEAD = c4e7f77 的树 + 本提交)

- 起始状态: 忠实层终态 20abd43 的代码未动 - parse.c 2148 行, sema.c 3145 行, chibicc.h 704 行,
  codegen.c 1595 行(相对 5f53ed0 零 diff). 本线到目前为止只改了文档与本测试文件.
- docker-test(含 test-all 与自举): **rc=0**, 全部 test/*.c 通过, `diagnostic lock: 53 cases
  byte-exact`(两轮: make test 与 make test-stage2 各一遍). 第一轮尝试**失败过一次**, 原因见下面的
  "诊断锁定的一个结构性限制", 改掉 f06 之后两轮全绿.
- 诊断锁定: 44 例(原有) + 9 例(A0.1 新增 f 系列) = **53 例逐字节通过**; 新增前后原有 44 例的
  期望文本一字未改(`git diff` 里 diagnostic.sh 的删除行只有 7 行文件头注释).
- raw / 归一化 diff: `make docker-snapshot-ndiff` -> **snapshot ndiff: empty**(基线是上一线留下的
  `.cache/snapshot`, 41 个文件); 编译器未改, raw 亦无变化, 本步不重置基线.
- tinycc(`make docker-test-thirdparty THIRDPARTY=tinycc`): **rc=0**, tests 全过(shim 跳过的
  106_pthread/112_backtrace/113_btdll 三例按既有约定不计).

## 诊断锁定的一个结构性限制(A0.1 实测, 影响 A1.1 的验收方式)

f06 最初锁的是 ptr-ptr 的常量表达式:

```c
int a[2]; long d = a - a;      /* 期望: "not a compile-time constant" */
```

`make test`(宿主 cc 编出的 chibicc)锚第一个 `a`, `make test-stage2`(chibicc 自举出的 stage2)
锚第二个 `a`, 差 4 列, 逐字节 diff 必然有一轮失败. 不是抖动, 是确定性的:

- 那条路径上两个操作数都会报错 - `eval2` 的 ND_SUB 写作 `eval2(lhs, label) - eval(rhs)`, 两侧都是
  数组名衰变来的指针, 都以 `label == NULL` 落到同一句 `error_tok`; 报哪一个取决于 C 不规定的
  操作数求值顺序.
- 宿主 clang 先求 lhs; 而 chibicc 自己的整数二元运算发射是
  `gen_expr(node->rhs); push(); gen_expr(node->lhs); pop("%rdi")` - **rhs 先**, 所以自举出的
  stage2 先求 rhs. 两个宿主的求值顺序天生相反.
- 同一份 sema.c 用 `-O0` 与 `-O2` 编出的两个宿主二进制都锚第一个 `a`, 所以这不是优化级别问题,
  是 clang 与 chibicc 的固定差异.

结论与处置:
- **逐字节锁定只对"单一错误路径"的诊断有意义.** 现有 44 例都满足, 所以这个限制到今天才暴露;
  本线搬的是降级代码, 而降级代码里的检查常常有两个都会失败的兄弟子树, 后面每加一例都要先问一句
  "这条诊断只有一个可能的锚点吗".
- f06 换成单路径的 `int a[2]; int x = a[0];`(常量表达式里的下标: 今天 add_type 把它降成 DEREF,
  `eval2` 没有 DEREF 的 case, 落到函数末尾的 error_tok; A1.1 补上忠实的 ND_SUBSCRIPT case 之后必须
  报同一文案同一锚点). 两个宿主二进制与 `-O0`/`-O2` 都一致.
- ptr-ptr 那条路径改用**正向锁**: `struct S { int x; } s; int *q = &s.x + 1;` 实测发 `.quad s+4`,
  `int a[2]; int *p = a + 1;` 发 `.quad a+4` - 这两个形状进 A10.2 的用例, 由汇编快照闸门看住,
  而快照闸门对两轮 stage 用的是同一个二进制, 不存在求值顺序问题.

## 契约 4 的实测证据(A0.1 探针, 供后续步骤对照)

`begin_function` 为每个函数无条件物化 `__func__` 与 `__FUNCTION__` 两个字符串字面量(匿名全局,
6.4.2.2p1), 所以 `.data` 的块序里每个函数至少占两块, 顺序由 **resolve 遍的定义序**决定. 探针:

```c
void f(void);                                                    /* 先声明 */
void g(void) { char *s = "in g"; (void)s; }                      /* 后定义 */
void f(void) { char *t = "in f"; (void)t; }
```

实测 `.data` 块序(归一化不折叠它): `.L..5`="in f", `.L..4`/`.L..3`="f", `.L..2`="in g",
`.L..1`/`.L..0`="g" - 即**创建序的逆序**, 而创建序是 g 先 f 后(定义序), 与 `prog` 的 f 先 g 后
(首次声明序)相反. 若字符串物化搬进 codegen 的整形遍(按 `prog` 走), 块序会变成 "in g", g, g,
"in f", f, f, ndiff 立刻非空. 这条探针进 A10.2.

## 基线缺陷记录(开工前发现, 本线不修)

- `void f(int *p) { 1 - p; }` **段错误**(宿主机上表现为 chibicc 退出 1 且 stderr 为空 - 驱动器
  以子进程跑 cc1, 子进程崩溃后父进程只报退出码). 原因: `new_sub` 的 VLA 分支写作
  `if (lhs->ty->base->kind == TY_VLA)`, 左操作数是整数时 `base` 为 NULL. 上游 chibicc 的
  07f9010("Add pointer arithmetic for VLA", 2020-09-03)引入时即缺 `lhs->ty->base &&` 守卫,
  不是本 fork 两条重构线造成的. 影响: 只有非法 C 会触发, 语料与第三方套件都不含此形状, 所以三道
  闸门一直看不见. 处置: **本线原样搬运, 不顺手补守卫**(补了就是行为变化, 会污染 A4.1 的
  "纯搬家" diff); 因此 `new_sub` 的 `invalid operands` 只有 ADD 侧被锁(f04), SUB 侧无锁,
  f 系列的注释里记了原因. 要不要单起一个提交修, 由用户拍板.

## 整形清单表(账本, A0.1 核对并补"验收口径"列)

判据: 1 = 语义结论 vs 发射便利; 2 = 检查留 sema; 3 = 对象物化归属; 4 = 结论记节点上.
验收口径里 `诊断 xNN` 指 `test/diagnostic.sh` 的用例名, `ndiff 空` 指该步的归一化快照闸门,
`A10.2` 指该步要新增的针对性用例.

| # | 项 | 现位置 | 归属 | 目标位置 | 步 | 验收口径 |
|---|---|---|---|---|---|---|
| 1 | `+`/`-` 指针缩放与 `num+ptr` 规范化 | `add_type` ND_ADD/ND_SUB, `new_add`/`new_sub`/`scale_rhs`/`new_arith` | 1 发射便利 | codegen 整形遍或发射点 | A4.1 | ndiff 空 + 诊断 f04 |
| 2 | 常规算术转换插入隐式 cast | `usual_arith_conv`(多处调用) | 1 语义结论 | sema 留(导出给 codegen 调用) | - | 不变 |
| 3 | 一元 `-` 的操作数提升 | `add_type` ND_NEG | 1 语义结论 | sema 留(只插 cast) | - | 不变 |
| 4 | `op=` 的读写回环 | `add_type` ND_ASSIGN(op), `to_assign`/`compound_op` | 1 发射便利 | codegen(原子特例进整形遍) | A5.1 | ndiff 空 + 诊断 f02 + A10.2 位域/原子 op= |
| 5 | 赋值 rhs 转换 + `not an lvalue` | `add_type` ND_ASSIGN | 1 结论 / 2 检查 | sema 留 | - | 诊断 f01 |
| 6 | `++`/`--` 降级(含后缀取值) | `add_type` ND_INCDEC, `new_inc_dec` | 1 发射便利 | codegen | A5.1 | ndiff 空 + 诊断 f03 + A10.2 `a[i] += j++` |
| 7 | `>`/`>=` 交换成 `<`/`<=` | `add_type` ND_GT/ND_GE | 1 发射便利 | codegen | A4.1 | ndiff 空 |
| 8 | 调用者检查(不是函数/实参个数/float 提升) | `lower_funcall` | 2 检查 | sema 留 | - | 诊断 e01/e02/e03 |
| 9 | 实参到形参类型的隐式 cast | `lower_funcall` | 1 语义结论 | sema 留 | - | 不变 |
| 10 | `func_ty` 标注 | `lower_funcall` | 1 标注 | sema 留 | - | 不变 |
| 11 | 调用者返回缓冲槽 `ret_buffer` | `lower_funcall` | 3 实现的槽 | codegen | A6.1 | ndiff 空 + A10.2 大结构体返回 |
| 12 | `pass_by_stack` | codegen `push_args` | - | 已是 codegen | - | 不变 |
| 13 | 一元/位/移位运算定型 | `add_type` | 1 标注 | sema 留 | - | 不变 |
| 14 | 字符串字面量物化(匿名全局) | `add_type` ND_STRING, `new_string_literal` | 3 语义对象 | sema 留(位置与顺序不变) | A9.1 | ndiff 空(块序) + A10.2 块序探针 |
| 15 | ND_STRING → ND_VAR 的形状改写 | `add_type` ND_STRING | 1 发射便利 | codegen(读 `node->var`) | A9.1 | ndiff 空, 预期 raw 亦空 |
| 16 | 名字绑定(变量/函数/枚举常量) | resolve 遍 `bind_ident` | 1 语义结论 | sema 留 | - | 不变 |
| 17 | `sizeof`/`_Alignof` 折叠 | `add_type` | 1 语义结论 | sema 记结论, 消费方读 | A1.2/A9.1 | ndiff 空 + A10.2 `sizeof(VLA)` |
| 18 | `__builtin_types_compatible_p` / `__builtin_reg_class` 折叠 | `add_type` | 1 语义结论 | 同上 | A1.2/A9.1 | ndiff 空 |
| 19 | `_Generic` 选中关联项 | `add_type` + `select_generic` | 1 语义结论 | 同上(选中项记字段) | A1.2/A9.1 | ndiff 空 + 诊断 d03 + A10.2 |
| 20 | elvis `a ?: b` 的临时量构造 | `add_type` ND_COND | 1 发射便利 | codegen(阶段 A 仍建槽, 保字节等价) | A9.1 | ndiff 空 + A10.2 `p ?: q` |
| 21 | `?:` 两臂转换与 void 定型 | `add_type` ND_COND | 1 标注 | sema 留 | - | 不变 |
| 22 | 成员绑定/匿名展平/三类校验 | `resolve_member`(pass 2 调用) | 1 结论 + 2 检查 | sema 留 | - | 诊断 c07-c11 |
| 23 | `->` 补插 DEREF | `resolve_member` | 1 发射便利 | codegen | A3.1 | ndiff 空 |
| 24 | 取地址的位域检查与定型 | `add_type` ND_ADDR | 2 检查 + 1 标注 | sema 留 | - | 诊断 e04 |
| 25 | `x[y]` → `*(x+y)` | `add_type` ND_SUBSCRIPT | 1 发射便利 | codegen | A3.1 | ndiff 空 + A10.2 `a[i] += j++` |
| 26 | `*foo` 函数消解(6.5.3.2p4) | `add_type` ND_DEREF | 1 语义结论 | sema 留(只定型, 不换节点) | A3.1 | ndiff 空(raw 多 `.loc`, 已提前记账) |
| 27 | 解引用检查与定型 | `add_type` ND_DEREF | 2 检查 + 1 标注 | sema 留 | - | 诊断 f08/f09 |
| 28 | 语句表达式的值语义 | `add_type` ND_STMT_EXPR | 2 检查 | sema 留(按忠实形状判: 末语句是记录即无值) | A8.1 | 诊断 e11 |
| 29 | `&&label` 登记(配对检查用) | `add_type` ND_LABEL_VAL | 2 检查 | sema 留(名字归 codegen) | A2.1 | 诊断 f05 + A10.2 `&&label` static 初始化器 |
| 30 | 原子 CAS/EXCH 定型与检查 | `add_type` ND_CAS/ND_EXCH | 1 标注 + 2 检查 | sema 留 | - | 不变 |
| 31 | 显式 cast 定型 | `add_type` ND_CAST(`ty_op`) | 1 标注 | sema 留 | - | 不变 |
| 32 | `return` 到返回类型的隐式 cast | `add_type` ND_RETURN | 1 语义结论 | sema 留 | - | 不变 |
| 33 | ND_DECL 展开成语句(VLA alloca, MEMZERO+赋值链) | `add_type` ND_DECL, `compute_vla_size` | 1 发射便利 | codegen | A8.1 | ndiff 空 + 诊断 d01/e05/e06/e08 |
| 34 | 块域 static 的数据镜像序列化 | `add_type` ND_DECL, `gvar_init_data` | 3 对象模型 | sema 留 | - | ndiff 空(块序) |
| 35 | 复合字面量: 无名字对象创建 | `add_type`(resolve 侧已建) | 3 语义对象 | sema 留 | A8.3 | ndiff 空(块序) + A10.2 块序探针 |
| 36 | 复合字面量: 初始化链 + 对象引用 | `add_type` ND_COMPOUND_LITERAL | 1 发射便利 | codegen | A8.3 | ndiff 空 |
| 37 | 记录节点出链(TYPEDEF/ENUM_CONST/GVAR_DECL/FUNCDEF) | `type_chain` | 1 发射便利 | codegen(记录在 sema 输出中保留) | A8.2 | ndiff 空 + A10.2 嵌套函数探针 |
| 38 | for-init 的 ND_BLOCK 包装 | `add_type` ND_FOR | 1 发射便利 | codegen | A8.1 | ndiff 空 |
| 39 | 循环/switch/case/label 的标签分配 | `analyze` | 1 发射便利 | codegen(标签栈 + 计数器) | A2.1 | ndiff 空 + .s 无重复标号 |
| 40 | ND_WHILE → ND_FOR | `analyze` | 1 发射便利 | codegen | A2.1 | ndiff 空 |
| 41 | break/continue → goto | `analyze` | 1 发射便利 | codegen | A2.1 | ndiff 空 + 诊断 b03/b04 |
| 42 | case 链(`case_next`/`default_case`) | `analyze` | 1 发射便利 | codegen | A2.1 | ndiff 空 + 诊断 b01/b02/d02 |
| 43 | stray case/default/break/continue 四检查 | `analyze` | 2 检查 | sema 留 | - | 诊断 b01-b04 |
| 44 | goto/label 配对检查 | `analyze` + `resolve_labels` | 2 检查 | sema 留(比名字字符串, 不写名字) | A2.1 | 诊断 f05 |
| 45 | 初始化器解析(designator 求值/brace elision/柔性数组) | `resolve_initializer` 一组 | 1 语义结论 | sema 留(`InitTree` 结构进共享头) | A8.1 | 诊断 c01-c06 |
| 46 | 局部初始化链 + MEMZERO | `create_lvar_init`/`lvar_init_comma`/`init_desg_expr` | 1 发射便利 | codegen | A8.1 | ndiff 空 |
| 47 | 全局初始化序列化 | `write_gvar_data`/`gvar_init_data` | 3 对象模型 | sema 留(经 `eval` 认忠实形态) | A1.1 | 诊断 f06/f07 + A10.2 全局初始化器探针 |
| 48 | 帧辅助对象(参数/`__va_area__`/`__alloca_size__`/大结构体返回缓冲参数)与 `__func__`/`__FUNCTION__` 字符串 | `begin_function` | 3 ABI 对象 + 3 语义对象 | sema 留(resolve 遍, 定义序决定 `.data` 块序) | - | ndiff 空(块序), 见上面的实测证据 |
| 49 | `Type::vla_size` 与 VLA 尺寸表达式 | `add_type`/`compute_vla_size`/`vla_size_expr` | 3 实现的槽 | codegen(字段改标 codegen 侧) | A7.1 | ndiff 空 + A10.2 VLA 尺寸复用探针 |
| 50 | 指针算术的元素大小缩放(**求值侧**) | `eval2` ND_ADD/ND_SUB - 今天见到的是 `new_add` 已产出的 `MUL` 子树, 所以 eval 从不缩放 | 1 语义结论 | sema 留: eval 自己按元素大小放大, 认 `num+ptr`, ptr-ptr 以 `label == NULL` 求两侧 | A1.1 | A10.2 的正向快照锁(`.quad s+4` / `.quad a+4`); ptr-ptr 的负例**不可逐字节锁**, 见"诊断锁定的一个结构性限制" |
| 51 | `resolve_member` 清 `arrow_tok` | `resolve_member` 末尾 | 1 发射便利的载体 | 不清: 留在最内层 link 上, 由 codegen 补 DEREF 后清 | A3.1 | ndiff 空 |
| 52 | 嵌套函数体的标注位置 | `type_chain` 的 ND_FUNCDEF 调 `analyze_function` | 3 顺序 | sema 留原位(只去掉出链); codegen 侧按契约 3(e) 只从 `prog` 进一次 | A8.2 | ndiff 空(块序) + A10.2 嵌套函数探针 |
| 53 | 原子 do-while 的预分配标签 | `to_assign` 里两处 `new_unique_name` | 1 发射便利 | 删除, 由 codegen 的标注相位分配(`analyze_node` 的 `if (!brk_label)` 守卫随之消失) | A2.1 | .s 无重复标号 + A10.2 原子 op= |
| 54 | 局部初始化器表达式的定型时机 | `create_lvar_init` 建出 ASSIGN 之后那次 `add_type`(ND_DECL / ND_COMPOUND_LITERAL 末尾) | 1 标注 + 3 顺序 | sema 的标注遍显式走 `init_resolved`, 在同一位置定型每个 `expr` | A8.1 | ndiff 空(块序) + A10.2 三元含字符串字面量探针 |
| 55 | `.L..%d` 名字计数器 | `new_unique_name`(匿名全局与标签共用一个计数器) | 1 基础设施 | 一个来源: codegen 取库的下一个值作基址, 或工厂搬到中性文件共用 | A2.1 | .s 无重复标号 + ndiff 空(前缀不能换) |

## 关键结论(A0.1 核对后)

- 判据 1 的落点: 清单里"发射便利"共 18 项(原 16 + 新增 51/53), 全部移 codegen; "语义结论/标注"
  共 19 项留 sema(原 17 + 新增 50/54).
- 判据 2 的落点: 11 项是检查, 全部留 sema. **A0.1 之前其中 5 项没有任何锁定用例**(赋值给数组的
  `not an lvalue` 三种形状, `invalid operands`, `use of undeclared label`, 求值器的
  `not a compile-time constant`, plain `*p` 的两条解引用检查) - 它们正好位于要搬走的代码里或
  紧邻它, 而语料只有合法 C, 丢了不会被发现. f 系列 9 例补上后, 44 -> 53.
- 归 codegen 的物化只有 4 类: 调用者返回缓冲(11), VLA 尺寸(49), 读写回环与 elvis 的临时量(4/20),
  原子 op= 的内部槽(4 的原子分支).
- 求值器与全局序列化(47/50)是"新逻辑"而非搬家: `eval2`/`is_const_expr` 开头调 `add_type`, 今天
  免费拿到整形后的形状; 整形移走后必须自己认忠实形态(A1.1). 其中 50 是本次核对新识别的, 原计划
  完全没提, 而 `int *p = arr + 2;`(实测 `.quad a+4`)与 `int *q = &s.x + 1;`(实测 `.quad s+4`)
  这类全局初始化器全走它.
- 顺序敏感: 全局匿名对象的创建顺序进 `.data` 块序(ndiff 不折叠), 所以 14/34/35/52/54 留在标注遍
  的同一遍历位置; 局部槽顺序只影响栈偏移(ndiff 第 3/4 类免疫), 可自由搬迁.
- 执行顺序与编号不一致, 由契约 1 的调用图决定: A0.1 -> A1.2 -> A1.1 -> A2.1 -> A3.1 -> A5.1 ->
  A4.1 -> A6.1 -> A9.1 -> A7.1 -> A8.1 -> A8.2 -> A8.3 -> A9.2 -> A10.1 -> A10.2.

## 提交一览

| 提交 | 步骤 | 内容 |
|---|---|---|
| 5413ceb | 线间归档 | 忠实层收尾线归档为 `PLAN-faithful.md`/`RESULT-faithful.md`, 新建本线与账本 |
| c4e7f77 | 计划优化 | 五条搬运契约, 按调用图重排执行顺序, 补四处计划漏项(求值侧缩放, 标签名字空间, arrow_tok 载体, 初始化器定型时机), tinycc 进每步闸门, elvis 的无槽发射挪到 B1.1 |
| (本提交) | A0.1 | 账本补"验收口径"列与 50-55 行, 诊断锁定 44 -> 53(f 系列, 把夹在降级里的检查锁住), 四闸门基线留档, 记录 `1 - p` 基线缺陷与"诊断锁定只对单路径诊断有意义"的结构性限制 |

## 给审核者的提示

- 审核重心: sema 的 `add_type`(每次提交都应少掉若干"改写树形状"的 case, 且剩下的 case 只填
  `ty`/写结论/做检查), codegen 新增的整形遍(是否忠实搬运、是否自带槽与标签分配、是否遵守契约 3
  的六条不变量), 以及 `test/diagnostic.sh` 的 53 例是否逐字节不变.
- 因 codegen 的"零改动红线"在本线作废, 对比口径从"`git diff 5f53ed0 -- codegen.c` 为空"改为
  "codegen 的改动全部可归入清单表的 codegen 侧项".
- 每步都应能回答两个问题: 这一步搬的代码在 sema 侧还有没有调用者(契约 1), 以及这一步有没有让
  某个"创建匿名全局"或"写 init_data"的动作换位置(契约 4).
