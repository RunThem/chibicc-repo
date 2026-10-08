# RESULT.md - codegen 直连 sema 产物执行记录

本文件记录 `PLAN.md`(codegen 直连 sema 产物线, 取消 sema 降级)各步骤的实际执行结果, 供审核与
后续会话接续参考. 前两条线已归档: 语法语义拆分见 `PLAN-split.md` / `RESULT-split.md`, 忠实层
收尾见 `PLAN-faithful.md` / `RESULT-faithful.md` - 后者记录的终态(20abd43)是本线的量化对比基线.

## 闸门口径(本线生效)

- **[搬迁] 步骤**(整形逻辑换位置, 发射形态不变): `make docker-test`(含自举) +
  `test/diagnostic.sh`(55 例逐字节) + `make docker-snapshot-ndiff` 为空 +
  `make docker-test-thirdparty THIRDPARTY=tinycc`; 同提交内 `make docker-snapshot` 重置 raw 基线.
- **[就地] 步骤**(发射形态变化, 只在阶段 B): `make docker-test` + 诊断锁定 + tinycc +
  该步新增的形状断言; ndiff 允许非空, 差异须逐条归入预期模式并记在本文件的偏差记录里.
- 相对上一线的两处口径变化: tinycc 从"[就地] 才跑"提升为**每步都跑**(15 s, 而 R2.5 是
  docker-test 漏掉真误编译的书面记录); 命令用 docker 版(本机 macOS/arm64, `make test-thirdparty`
  会直接报错退出).

## 基线记录(A0.1, HEAD = c4e7f77 的树 + 4891aa2)

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

## 基线缺陷记录(开工前发现; 2026-09-29 已修复)

- `void f(int *p) { 1 - p; }` 曾**段错误**(宿主机上表现为 chibicc 退出 1 且 stderr 为空 - 驱动器
  以子进程跑 cc1, 子进程崩溃后父进程只报退出码). 原因: `new_sub` 的 VLA 分支写作
  `if (lhs->ty->base->kind == TY_VLA)`, 左操作数是整数时 `base` 为 NULL. 上游 chibicc 的
  07f9010("Add pointer arithmetic for VLA", 2020-09-03)引入时即缺 `lhs->ty->base &&` 守卫,
  不是本 fork 两条重构线造成的. 影响: 只有非法 C 会触发, 语料与第三方套件都不含此形状, 所以三道
  闸门一直看不见. 处置分两段: 开工时定为**本线原样搬运, 不顺手补守卫**(补了就是行为变化, 会污染
  A4.1 的"纯搬家" diff), SUB 侧因此无锁(只有 ADD 侧的 f04); **2026-09-29 用户拍板, 单起提交修复** -
  守卫补上, `1 - p` 与经 compound_op 的 `i -= p`(同走该分支)由段错误转为 `invalid operands`
  (分别锚 `-` 与 `-=`), 诊断锁定随修复新增 f10/f11 两例(53 -> 55), 期望文本取自修复后的实际输出.
  A4.1 此后搬运的就是带守卫的版本, "纯搬家" diff 不受污染.

## 整形清单表(账本, A0.1 核对并补"验收口径"列)

判据: 1 = 语义结论 vs 发射便利; 2 = 检查留 sema; 3 = 对象物化归属; 4 = 结论记节点上.
验收口径里 `诊断 xNN` 指 `test/diagnostic.sh` 的用例名, `ndiff 空` 指该步的归一化快照闸门,
`A10.2` 指该步要新增的针对性用例.

| # | 项 | 现位置 | 归属 | 目标位置 | 步 | 验收口径 |
|---|---|---|---|---|---|---|
| 1 | `+`/`-` 指针缩放与 `num+ptr` 规范化 | `add_type` ND_ADD/ND_SUB, `new_add`/`new_sub`/`scale_rhs`/`new_arith` | 1 发射便利 | codegen 整形遍或发射点 | A4.1 | ndiff 空 + 诊断 f04/f10/f11 |
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
| 50 | 指针算术的元素大小缩放(**求值侧**) | `eval2` ND_ADD/ND_SUB - 今天见到的是 `new_add` 已产出的 `MUL` 子树, 所以 eval 从不缩放 | 1 语义结论 | sema 留: eval 自己按元素大小放大, 认 `num+ptr`, ptr-ptr 以 `label == NULL` 求两侧 | A4.1(自 A1.1 移入, 见 A1.1 偏差 1) | A10.2 的正向快照锁(`.quad s+4` / `.quad a+4`); ptr-ptr 的负例**不可逐字节锁**, 见"诊断锁定的一个结构性限制" |
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
| 4891aa2 | A0.1 | 账本补"验收口径"列与 50-55 行, 诊断锁定 44 -> 53(f 系列, 把夹在降级里的检查锁住), 四闸门基线留档, 记录 `1 - p` 基线缺陷与"诊断锁定只对单路径诊断有意义"的结构性限制 |
| 5b1fd98 | A1.2 | Node 增 `generic_sel` 结论槽并由 `select_generic` 写入; 契约 2 的永久导出面(10 个类型级符号)去 static 并声明; sizeof 与两个 builtin 的结论复用 `val`/`ty`, 不新增字段 |
| 0ede0ad | 线间收尾 | 计划锚点里的陷阱标注到代码现场(new_sub 守卫缺失/求值器搭便车/名字工厂共用计数器/原子预分配标签/死检查), diagnostic.sh 头注释"33 处"表述修正, 账本与步骤头占位符回填哈希 |
| 3228229 | 基线缺陷修复 | new_sub 的 VLA 分支补 `lhs->ty->base &&` 守卫, `1 - p` 与经 compound_op 的 `i -= p` 由段错误转为 `invalid operands`; 诊断锁定新增 f10/f11(53 -> 55) |
| cd44d0b | A1.1 | 求值器四函数补忠实 case(GT/GE, 下标取址, 成员 arrow, STRING, SIZEOF/ALIGNOF 结论, 两个类型 builtin, GENERIC, elvis 守卫, ptr-ptr 除法); ADD/SUB 缩放支移入 A4.1; test/vla.c 补 sizeof(VLA) 作维度回归; 8 组非空转探针 |
| 49d7cac | A2.1 | 标签与控制流整形归 codegen: 整形遍 + `.L..` 基址续号, sema 检查化(stray 四检查 + 名字串配对), Obj.label_gotos 交接, 原子环预分配删除 |
| e300eb9 | A3.1 | 成员与下标整形归 codegen: `x[y]`→`*(x+y)`(经临时导出的 new_add), arrow 的 DEREF 补插(arrow_tok 留最内层 link), `*foo` 只定型不换节点, eval2 补 DEREF 消解, to_assign member 分支适配忠实成员, new_add 依契约 2 临时导出 |
| 46a2b2a | A5.1 | 复合赋值与自增自减整形归 codegen: to_assign/compound_op/combine/new_inc_dec 整体搬入整形遍, add_type 的 ND_ASSIGN(op)/ND_INCDEC 改纯标注, codegen 自建槽工厂(不导出 sema 的 new_var), 原子 retry 环就地 shape 拿标签, new_sub 依契约 2 临时导出 |
| (本提交) | A4.1 | 算术与比较归 codegen: new_add/new_sub/scale_rhs 搬入整形遍(static), GT/GE 交换搬 shape_node, sema 的 ND_ADD/ND_SUB 改检查+定型(不插 cast), eval2 补 ADD/SUB 指针缩放(num+ptr 认源码序), 撤两处临时导出并删死码 ty_beyond_convs; 顺带修复两个整形遍重入缺陷(elvis 陈旧字段别名 / 原子环 shape 下潜), 二者均为计划未列, 快照抓出 |

## 各步详情

### A0.1 账本与基线 (4891aa2)

- 改了什么: 编译器源码零改动. (1) 本文件的账本表补"验收口径"列, 并补 50-55 六行原计划没有的项
  (求值侧的指针缩放, `resolve_member` 清 `arrow_tok`, 嵌套函数体的标注位置与访问一次, 原子
  do-while 的预分配标签, 局部初始化器表达式的定型时机, `.L..%d` 共用计数器); 48 行的描述补上
  `__func__`/`__FUNCTION__`. (2) `test/diagnostic.sh` 44 -> 53 例, 新增 f 系列 9 例锁住"夹在降级
  代码里的检查". (3) PLAN.md 补两处: 判据 2 说明为什么要先扩锁定, 闸门口径补"诊断锁定只对单一
  错误路径的诊断有意义".
- 为什么改: 本线要搬的 ~800 行降级里嵌着诊断, 而原有 44 例只覆盖 parse 的 33 处与 sema 判定表 E -
  夹在降级里的那些**一例都没有**. 语料只有合法 C, 丢一条检查不会被任何闸门看见(R3.1 的教训是同一
  类). 量尺必须在搬之前立起来.
- 测试结果: 四闸门全绿 - `docker-test` rc=0(两轮诊断各 53 例逐字节), `docker-snapshot-ndiff` 空
  (41 文件基线), `docker-test-thirdparty THIRDPARTY=tinycc` rc=0; 宿主机本地构建后 diagnostic.sh
  亦 53/53.
- 偏差说明: 三处, 都已写进上面的专节. (1) f06 第一次锁的是 ptr-ptr 的 `a - a`, stage2 与宿主构建
  报不同锚点, 必然有一轮失败 - 换成单路径的"常量表达式里的下标", ptr-ptr 改用正向快照锁.
  (2) 发现基线缺陷 `1 - p` 段错误(上游 07f9010 起), 记录不修. (3) 契约 4 由推理升级为实测:
  `__func__`/`__FUNCTION__` 让每个函数在 `.data` 里占两块, 块序确为 resolve 定义序的逆序.

### A1.2 结论字段与导出面 (5b1fd98)

- 改了什么: (1) `chibicc.h` 的 Node 增一个字段 `Node *generic_sel` - ND_GENERIC 的结论槽, 存
  controlling 表达式选中的那个关联项的结果表达式; `select_generic` 在改写节点之后写它.
  (2) `chibicc.h` 声明契约 2 的永久导出面 10 个符号, `sema.c` 对应去掉 `static`
  (`add_type`, `new_arith`, `usual_arith_conv`, `get_common_type`, `new_cast`, `new_long`,
  `new_ulong`, `new_var_node`, `new_vla_ptr`, `new_alloca`), 并删掉两条与头文件声明冲突的 static
  前向声明; sema.c 与 chibicc.h 的文件头/段落注释说明这个导出面是干什么的.
  (3) sizeof/alignof 与两个类型 builtin **不新增字段**: 它们的结论是数值, 今天的折叠已经把它落在
  `val` 与 `ty` 上(与 ND_NUM 同槽), A9.1 只需要停止改 `kind`, 消费方读法不变.
  行数: chibicc.h 704 -> 737, sema.c 3145 -> 3173, codegen.c 1595 与 parse.c 2148 零改动.
- 为什么改: 执行顺序把 A1.2 排在 A1.1 之前, 因为求值器认忠实形态时要读 `_Generic` 的选中项 -
  字段必须先长出来. 导出面则是契约 2 的落地: 让 A3.1-A9.1 的搬运保持"文本搬家 + 换调用符号",
  而不是在 codegen 里重述类型决策. 具体到字节: `usual_arith_conv` 对 `ptr + n*4` 也会给两侧插
  cast(`get_common_type` 一见 `ty1->base` 就返回 `pointer_to(ty1->base)`), codegen 若自己拼缩放和
  就会少掉那个 cast 节点与它的 `.loc`.
- 测试结果: 四项全绿, 且 **raw 逐字节为空**(比 ndiff 闸门更强, 无需重置基线) -
  `docker-snapshot-diff` 与 `docker-snapshot-ndiff` 都报 empty(41 文件); `docker-test` 跑完
  make test 与 make test-stage2 两轮, 末尾 `diagnostic lock: 53 cases byte-exact`;
  `docker-test-thirdparty THIRDPARTY=tinycc` 跑完 tcc 全套无 FAILED. 即"长出结论槽 + 导出符号"
  没有改变任何一个文件的产物字节, 符合本步的零行为变化设计.
- 偏差说明: 两处. (1) **计划文本自相矛盾, 已改**: 原 A1.2 写"`add_type` 写结论而**不**替换节点
  (本步消费方仍是整形替换, 所以零行为变化)" - 消费方还在读替换结果时停止替换就不是零变化.
  实施为: 本步**照旧替换**, 只把结论同时写进字段, 两条载体并存且一致; 停止替换是 A9.1 的事.
  PLAN.md 的 A1.2 条目已按此改正. (2) 导出面里 `usual_arith_conv`/`get_common_type`/`new_ulong`
  目前在 sema 之外没有调用者, 一并导出是为了让后续每步都是纯搬家; 若更倾向最小 API 面, A9.2 收口时
  可以把仍无外部调用者的收回 - 记为待决, 不影响本线闸门.
  非空转探针(不入提交): 临时在 `select_generic` 末尾打印 `generic_sel` 与 `sel`, 编译
  `test/generic.c`, 6 次选择全部 `generic_sel == sel`, 证明字段确实被写且指向选中的关联项;
  探针撤销后 `git diff` 只含 chibicc.h / sema.c / PLAN.md / RESULT.md.

### A1.1 求值器认忠实形态 (cd44d0b)

- 改了什么: 编译器源只动 sema.c 的求值器四函数与一个新辅助。 (1) `eval2` 补忠实 case:
  ND_GT/ND_GE 就地反比(带符号位取自交换后的左操作数, 与降级形态逐值一致), ND_STRING(读物化后
  `node->var`), ND_SIZEOF/ND_ALIGNOF(定长读 `val`, sizeof 的 VLA 操作数报非常量), ND_TYPES_COMPATIBLE
  /ND_REG_CLASS(读 `val`), ND_GENERIC(`eval2(generic_sel)`), ND_COND 的 is_elvis 守卫(落向函数末尾的
  非常量 error), ND_ADD/ND_SUB 的 ptr-ptr 除法支(两侧以 `label == NULL` 求, 复现降级 DIV 的提问
  方式)与 invalid-operands 镜像; 入口注释修正: **入口的 add_type 调用保留** - 它是标注来源(#if 的
  常量表达式只有这一处定型), 离开的是其中的改写, switch 自此要认忠实 kind。 (2) `eval_rval` 补
  ND_SUBSCRIPT 取址(基址 + 下标 x 元素尺寸, VLA 尺寸报非常量), ND_MEMBER 的 arrow(指针值 + 偏移),
  ND_STRING。 (3) `is_const_expr` 补 GT/GE 二元表项, SIZEOF/ALIGNOF(定长 true, sizeof 的 VLA false),
  两个 builtin true, GENERIC 递归 `generic_sel`, COND 的 is_elvis false。 (4) `eval_double` 补
  GENERIC 与 is_elvis 守卫。 (5) 新辅助 `ty_beyond_convs`: 穿过 conv 插入的 cast 链读操作数原类型 -
  忠实指针算术要知道哪一侧原本是指针。 (6) `test/vla.c` 补 sizeof(VLA) 作维度回归
  (`ASSERT(80, ...)`, 期望值 clang 验证)。 行数: sema.c 3173 -> 3352, 其余零改动。
- 为什么改: A2.1-A9.1 每搬走一个降级, 求值器就开始见到该 kind 的忠实形态, case 必须先就位 -
  这是本线唯一"新逻辑"的铺设。 每个 case 的激活步与覆盖:

  | case | 激活步 | 探针 | 语料覆盖 |
  |---|---|---|---|
  | GT/GE | A4.1 | 8 组之一(逐值) | 常量比较在语料中存在, 激活步快照核对 |
  | SUBSCRIPT 取址 | A3.1 | 8 组之一 | initializer.c 的 `&g11[..]` 形状(.quad 已被快照锁) |
  | MEMBER arrow | A3.1 | 8 组之一 | 常量上下文的 `->` 语料罕见, A3.1/A10.2 核对 |
  | STRING(两函数) | A9.1 | 8 组之一 | string/literal 等大量全局初始化器 |
  | SIZEOF/ALIGNOF | A9.1 | 定长支 8 组之一; VLA 维度支由 vla.c 新断言锁定(激活于 A9.1) | sizeof 全域 |
  | 两个 builtin / GENERIC | A9.1 | 8 组之一(含 double 泛型走 eval_double) | builtin.c/generic.c |
  | elvis 守卫(三函数) | A9.1 | 8 组之一(错误路径逐字节) | 无常量上下文 elvis(A10.2 的 p?:q 是运行期) |
  | ptr-ptr 除法 | A4.1 | 8 组之一(负例错误逐字节) | 无负例语料; 计划已知不可逐字节锁 |

  值形下标**不设** eval2 case: 降级把 DEREF 写在同一节点(锚点不变), 忠实形态落函数末尾 error,
  文案与锚点(下标节点自己的 tok)与 f06 锁定完全一致。
- 测试结果: 四闸门全绿。 docker-test rc=0(两轮 55 例逐字节, 含 vla.c 新断言); 快照用混合基线
  (HEAD 编译器 + 新 vla.c 生成对照): raw 与归一化 diff **均为空** - 求值器改动对语料逐字节中性;
  基线已重置为新编译器 + 新语料并复验; tinycc rc=0。 本地 A/B(HEAD 二进制 vs 新二进制对全部
  test/*.c 出 -S, atomic/tls 因系统头不可本机构建): 39 文件全部逐字节相同。 **非空转探针 8 组**:
  探针树上逐项关掉对应降级(GT/GE 交换, 下标改写, arrow 的 DEREF 补插与 arrow_tok 清理, 两处
  STRING 的 kind 改写, 定长 sizeof 的折叠改写, 两个 builtin 与 GENERIC 的改写, elvis 降级,
  以及单独一组 ADD/SUB 的 A4.1 预览), 对应输入在探针二进制与主二进制下输出**逐字节相同**
  (.s 与 stderr, 含错误路径) - 旧路径关闭后唯一能算出正确值的就是新 case, 相同即"命中"的证明。
- 偏差说明: 四项。
  1. **ADD/SUB 的缩放支与 num+ptr 认识移入 A4.1**(计划把它们列在本步)。 机制: 这两个 case 的
     槽位为两个时代共享 - 降级形态的 rhs 已是 `MUL(下标, 尺寸)` 乘积, 忠实缩放支无条件存在会对
     已缩放的树二次缩放; 首版实现即被闸门抓住(initializer.s 实测 `g11+8` -> `g11+64`, `g26+4` ->
     `g26+16`, 本地 A/B 定位), 且**无法用形状区分两种时代** - 降级产物 `MUL(2, 4)` 与源码写的
     `p + 2*4` 同形, 任何启发式都会静默算错。 处置: 缩放支随降级的离开(A4.1)落地, 那时它成为唯一
     缩放; ptr-ptr 支今日形状是 DIV(不同 kind), 休眠安全, 留在本步。 语料中常量上下文的指针算术
     (initializer.c)已把降级侧的 `.quad` 值锁进快照, A4.1 搬运时即时核对。 PLAN.md 的 A1.1/A4.1
     条目与账本行 50 已按此改。
  2. **elvis 守卫是计划未列的补充**: 计划只点名 INCDEC 与带 op 的 ASSIGN 为非常量, 但 elvis 的
     `then` 为 NULL - 忠实形态下不设守卫, eval2/eval_double 的 COND case 会 NULL 解引用,
     is_const_expr 会 NULL 递归。 按今日行为(降级后 tmp lvar 报 "not a compile-time constant",
     锚 elvis tok)保守处理为非常量, 探针证明错误路径逐字节一致。
  3. **新基线缺陷(记录不修, 与 `1 - p` 同族)**: `int g; int h; long d = &g - &h;` 段错误(空 stderr
     退出 1)。 原因: 降级形态 `DIV(SUB(ADDR, ADDR), 4)` 求值时 `eval2(SUB, NULL)` 经 ND_ADDR ->
     `eval_rval(VAR, NULL)`, 后者无 label 判空直接写 `*label`。 忠实 ptr-ptr 支走同一调用序列,
     行为逐字节保持(含此缺陷)。 修复属用户拍板项。 经下标写法的 `&a[1] - &a[0]` 干净报错
     ("not a compile-time constant"), 不触发。
  4. **is_const_expr(VLA) 支的探针只能覆盖定长支**: sizeof(VLA) 的完整形态要到 A9.1 才出现
     (VLA 折叠本步照旧), 其判定的真实闸门是 vla.c 新断言在 A9.1 激活时的锁定 - 若届时
     is_const_expr(ND_SIZEOF) 答错, 该断言会让编译直接失败。

### A2.1 标签与控制流归 codegen (49d7cac)

- 改了什么: codegen.c 首次承接整形。 (1) 新增整形遍一节(~150 行, 置于 assign_lvar_offsets 之前):
  `shape/shape_node/shape_chain/shape_children` 是原 `analyze` 家家的搬运版 - 标签分配
  (`new_label`: `.L..%d`, 基址取 `unique_name_next()` 后自增序号), 循环/switch 标签栈的压入/弹出,
  ND_WHILE→ND_FOR, break/continue→ND_GOTO, case 的前序挂链与 default 指针, ND_LABEL 的名字与
  收集; 检查全部不在(codegen 只对过了检查的树做功)。 (2) `resolve_label_refs`: 对每函数遍历
  **Obj.label_gotos**(sema 交来的 goto/label-value 引用链), 与本函数下降收集的标签按名字串配对,
  写 `unique_label` - 原 `resolve_labels` 的写入半。 (3) `codegen()` 入口变为
  "取基址 → shape → assign_lvar_offsets → emit_data → emit_text"。 (4) sema.c: analyze 家家
  (brk/cont/current_switch 三个 static + `resolve_labels` + `analyze_node/chain/children/analyze`)
  替换为**纯检查**版本 - `check_node/chain/children` 只跟踪循环/switch 嵌套深度跑 stray 四检查
  并收集 goto/label(文案与锚点逐字不变), `check_labels` 按名字串配对报 "use of undeclared label"
  不写名字; `analyze_function` 在检查后把 `gotos` 链存进 `fn->label_gotos` 并清空两链。
  (5) `to_assign` 原子分支的两处预分配删除(守卫随预分配恒真, 一并删除); 计数器提为文件域
  `unique_name_id` 并导出只读 `unique_name_next()`。 (6) chibicc.h: Obj 增 `label_gotos` 字段
  (sema 写 / codegen 读一次), 导出 `unique_name_next`。 规模: codegen.c 1595 -> 1766,
  sema.c 3352 -> 3332, chibicc.h 750 -> 756。
- 为什么改: 判据 1 的第 39-41 行 - 标签分配, while 降级, break/continue 改写全是"为发射服务的
  整形", 归 codegen; 判据 2 的第 43/44 行 - stray 与配对检查留 sema。 账本行 29/39/40/41/43/44/53/55
  在本步落位: 库层不再分配任何标签(sema 的 `new_unique_name` 只剩 `new_anon_gvar` 一个调用者,
  匿名全局一族), `.L..` 名字空间一个来源(codegen 从库计数器停点续号), 原子环的 do-while 无标签
  到达, 由整形遍统一分配。
- 测试结果: 四闸门全绿。 docker-test rc=0(两轮 55 例逐字节, b01-b04/f05 锁定检查侧, control.c 的
  `&&label` 块域 static 跳转表与 atomic.c 的 CAS 循环在运行期验证交接字段与守卫分配)。
  **归一化 diff 空**(权威形状闸门, 依据即计划验收段的首现序列恒等论证)。 raw diff 非空 - 2008 行
  **逐行核对全部为 `.L..` 编号变化, 0 行非标签**(本地 A/B 39 文件同口径: 1848 标签行 + 8 行
  diff 对齐伪影的 `.data` 同文对), 本提交内重置 raw 基线并复验为空。 tinycc rc=0。 自检:
  生成的 .s 无重复标号; 红线 grep - parse/preprocess/tokenize/type 零标签产物, sema 不调 codegen。
  本步代码**完全在役**(非先埋后用), 语料即非空转证明 - 标签值重编号即新分配器的输出, `&&label`
  的 .quad 解析即交接字段 + resolve_label_refs 的输出。
- 偏差说明: 两项, 均为机制补充而非范围变化。
  1. **Obj.label_gotos 交接字段是计划未写的机制**: 计划验收段假设"整形遍在最前面, 标签名定下即可"
     对 `&&label` 成立, 但初始化器里的 label-value 节点**只有 sema 的收集链可达**(`add_type` 的
     ND_LABEL_VAL case - 它们不在语句树里, codegen 的下降走不到, ResolvedInit 又是 sema 私有)。
     契约 3(f)"初始化器不在自然路径上, 两侧都要显式走"落地为: sema 收集链存到 fn, codegen 用链
     解析。 字段归属(sema 写/codegen 读一次)记入 A10.1 的逐字段账。
  2. **嵌套函数与标签编号的既有怪癖照旧**: 宿主函数里位于嵌套定义**之前**的 `&&label` 引用,
     今天会被嵌套函数的 `resolve_labels` 拿去与嵌套标签配对(而非宿主的); 新机制下同一节点进
     嵌套函数的 label_gotos 链, 由 codegen 对嵌套标签解析 - 行为逐点一致(含"配同名的嵌套标签
     则解析成功"这一怪情况)。 该形状无语料无锁定, 双方均为对非法 C 的处置, 不构成回归。

### A3.1 成员与下标 (e300eb9)

- 改了什么: 账本行 23/25/26/51 落位, sema 的 add_type 从此不再改写这三类表达式节点。
  (1) `add_type` ND_SUBSCRIPT 只定型: 节点保持自身, `ty = base`(点类型)。operand-pair 的接受集
  逐分支镜像 new_add - 两侧 numeric, 或恰一侧带 base(不检 num 侧是否 integer, `p[s]` 这类垃圾
  形状照旧接受, 保零行为变化); 两 base 报 `invalid operands`, 无 base 报 `invalid pointer
  dereference`, base 为 void 报 `dereferencing a void pointer` - 全部锚 `node->tok`, 与今天经
  new_add/DEREF 检查的锚点逐字节一致。`*(x+y)` 改写连同缩放搬 codegen 整形遍(经临时导出的
  `new_add`, 契约 2; 加法侧的 f04 与 `1 - p` 的 f10/f11 检查仍在 new_add/new_sub, A4.1 搬)。
  (2) `add_type` ND_DEREF 的 `*foo` 消解(6.5.3.2p4)不再 `*node = *node->lhs`: 节点保留, 定型
  `ty = lhs->ty`(函数指示符类型); codegen 不重建该形状 - gen_expr 的既有 ND_DEREF case 对
  TY_FUNC 的 load 是 no-op, `(*fp)(..)`(操作数为函数指针变量)今天树里本就有 DEREF, 无变化。
  (3) `resolve_member` 展平与绑定照旧, 不再补插 DEREF: arrow_tok 留在最内层 link 上(展平发生
  时清外层节点的自己的标记, 单层时节点 dissolved 后自己就是最内层); codegen 整形遍对带标记的
  MEMBER 补 DEREF(锚 node->tok, 与今天 resolve_member 用的 token 相同)后清标记。
  (4) `to_assign` 的 member 分支(仍是 sema 降级, A5.1 搬)适配忠实成员: 带 arrow_tok 时把操作数
  包回 DEREF 再取址 - 今天的分支读的 `node->lhs->lhs` 是降级形态的 DEREF; 匿名展平的内层 link
  自带标记由 codegen 解析, 分支对它无需改(struct.c:48 既有用例锁住)。
  (5) `eval2` 补 ND_DEREF case(操作数为函数指示符时读过去, 其余落函数末尾的非常量错误)-
  A1.1 覆盖表没有 DEREF 行, 因为消解过的树到不了求值器; 本步起忠实形态到得了, 用例为
  function.c 的全局 `int (*gfp)(int,int) = *add2;`(.quad add2, 快照锁)。
  (6) 契约 2 临时导出面新增 `new_add`(去 static + chibicc.h 声明, 注明 A4.1 撤)。
  测试: struct.c 补 1 层 arrow 的 `+=`/`++` 两例(既有 corpus 只有匿名展平的 `p->a += 2`),
  function.c 补 `(*add2)(2,3)` 直呼、`(*fn)(2,5)` 指针解呼、`gfp(2,5)` 三断言与 gfp 全局初始化器。
  行数: sema.c 3332 -> 3374, codegen.c 1766 -> 1794, chibicc.h 756 -> 766, parse.c 2148 零改动。
- 为什么改: 判据 1 的第 23/25/51 行(下标改写, arrow 补插, arrow_tok 清理全是发射便利)与第 26 行
  (`*foo` 的结论是定型, 换节点是发射便利)。"codegen 必须经 new_add 而不是自己拼"由契约 2 的
  陷阱保证: usual_arith_conv 对 ptr 算术两侧都插 cast, 手搓缩放和会丢节点与 .loc - 整形遍调用
  导出的 new_add, 产出与今天逐字节相同的子树。eval2/eval_rval 的忠实 case(下标取址, 成员
  arrow)自 A1.1 埋好, 本步按计划激活。
- 测试结果: 四闸门全绿。docker-test rc=0(两轮 55 例诊断逐字节, f06 的下标锚点/ f08/f09 的
  DEREF 检查/ f04/f10/f11 不受影响)。ndiff 空 - 因本步改了两个测试文件, 按 A1.1 先例先以
  **混合基线**(HEAD 编译器 + 新语料)复验, 归一化 diff 为空即编译器变化对语料发射中性; 随后
  raw diff 恰 **4 行全部为 `.loc` 增行**(function.c 的 `(*add2)` 1 行 + `(***add2)` 3 行, 每个
  保留的 DEREF 一行, struct.s 零差异), 即计划预告的偏差, 归一化第 1 类折叠吃掉; 同提交重置
  raw 基线(41 文件)并复验。tinycc rc=0(日志中 11 处 "failed as expected" 是 tcc 自身的预期
  失败用例, 非回归)。本地 A/B(worktree HEAD 二进制): 33 文件逐字节相同(attribute/offsetof/
  stdhdr/varargs 的 `.file` 行为 worktree 路径伪影), 错误路径 18 例 16 同(见偏差 1)。自检:
  41 快照无重复标号; sema 的 new_unique_name 只剩 new_anon_gvar; 红线 grep - parse/preprocess/
  tokenize/type 无整形, sema 不调 codegen。
- 偏差说明: 五项。
  1. **两个崩溃形状转为干净诊断(计划未列, `1 - p` 同族)**: `int x; struct{int a;} s; x[s];` 与
     `int f(void); f[0];` 今天在 new_add→scale_rhs 里对无 base 的类型读 `->kind` 段错误(退出 1
     无 stderr); 新检查将其报为 `invalid operands`(锚 `[`) - 即 new_add 自己 fall-through 的
     文案, 与 f10/f11 的 2026-09-29 拍板方向一致。这是 A3.1 语义上不可避免的: 新检查代码无处
     保留崩溃。若审核要求严格保留崩溃语义需拆出该分支, 请拍板。另注: `f[0]` 是合法 C(GCC
     接受, 函数指示符衰变后下标), chibicc 上游从未支持(一直崩溃), 新行为是报错而非支持。
  2. **to_assign member 分支的 DEREF 回插是计划未写的机制**: 契约 3(a) 预警过 "A5.1 的
     to_assign 读 node->lhs->lhs", 但中间态(本步落地而 A5.1 未动)下该分支读的是 resolve_member
     今天补的 DEREF - 不适配则 `p->x += 1` 会把 `&p` 当成员地址(指针变量的地址而非其值)。处置
     见"(4)"; A5.1 搬走该分支时回插随之离开库层, 不新增红线欠账。
  3. **eval_rval 的 arrow 分支不设守卫(A1.1 埋的形状实测确认是对的)**: `&p->x` 这类非量地址的
     错误路径经 `eval2(VAR p)` 报 "invalid initializer" 锚在指针操作数上, 与降级形态一致; 实施
     中途曾按 eval2 侧的守卫形状给 eval_rval 补守卫(锚移到成员名), 本地 A/B 抓到后回滚 -
     eval2 与 eval_rval 的守卫差异今天就存在, 不是本线造成的。
  4. **`.loc` 偏差兑现但比预告窄**: 计划预期 "保留 ND_DEREF 会让 gen_expr 多打一条 .loc" - 实测
     语料中该形状仅 function.c 的 `(*add2)`/`(***add2)`(4 行); `(*fp)`(函数指针变量)今天就不
     消解, 树里本有 DEREF, 零变化。
  5. **语料外角落(记录不修)**: `(*alloca)(n)` - 今天 callee 消解为 VAR 后命中 gen_expr 的
     builtin_alloca 特判; 新形态 callee 是 DEREF, 落通用调用路径。 语料(test/ 与 tcc 源码)无此
     形状, 闸门不可见; 若要保真可在整形遍消解该形状, 归 A10.2 或后续拍板。

### A5.1 复合赋值与自增自减 (46a2b2a)

- 改了什么: 账本行 4/6 落位, sema 的 add_type 从此不再改写这两类节点。
  (1) `to_assign`/`compound_op`/`combine`/`new_inc_dec` 四函数整体搬入 codegen 的整形遍区段
  (文本原样; 对 `new_add`/`new_sub` 的调用走契约 2 的临时导出, `new_sub` 本步去 static + 声明,
  A4.1 随其搬走撤除); `add_type` 的 ND_ASSIGN(op) 与 ND_INCDEC 两个 case 改为纯标注:
  `node->ty = node->lhs->ty` - 三种改写形态(普通 comma, 成员 comma, 原子语句表达式)的结论类型
  都是左操作数类型。 (2) codegen 的 shape_node 新增 ND_ASSIGN/ND_INCDEC 两 case, 文本镜像 sema
  原样: 先 shape_children(契约 3(a) 后序), 再改写, `*node = *result` 保留链上 next, 末尾
  add_type 定型新树(契约 3(d) 只对全新节点; 旧子树已在后序中整形过且全部带标记, 不重入)。
  (3) 原子 op= 的 do-while 在整形遍内构建, 遍不会自然到达它 - 构建完成后直接
  `shape_node(loop)`, 用与其它循环同一机制拿 brk/cont 标签(A2.1 已删预分配, 无撞名)。
  (4) 槽工厂: codegen 自建 `new_lvar`(calloc Obj + align + is_local + 前插
  `current_fn->locals`), 刻意**不**导出 sema 的 `new_var` - 它做 push_scope 与 resolve_type,
  是语义层状态操作, 导出即让 codegen 触碰作用域表; `shape()` 每函数先设 `current_fn`。
  (5) A3.1 的 to_assign member 分支适配(带 arrow_tok 时包回 DEREF)在新时序下成为死代码并删除:
  整形遍后序先对 lhs 补 DEREF 清标记, to_assign 读到的已是 `MEMBER(DEREF(p))`, 与今天改写产物
  逐字节相同(含 DEREF 的 token 锚点 - 两者都锚成员 token)。 (6) 契约 3(b) 的原子环 do-while、
  语句表达式、comma 均在标注相位之前成形, add_type 定型时经既有 case(含 plain assign 的
  `not an lvalue` 检查 - `arr += 1` 经新树的 DEREF 左值到达, 锚点不变)。
  测试: 本地探针 40+ 形状的 .s 逐字节相同(全部 op= / 前后缀 ++-- / 指针与数组 / 多层 arrow /
  匿名展平的点号与 arrow 两种 / 位域 op= 与 ++ / 下标操作数 / 原子 op= 与 ++-- / 语句表达式
  操作数 / 表达式语境 / `s.self->a` 链); 5 个错误路径 stderr 逐字节相同(文件域初始化器与
  static 局部初始化器中的复合赋值报 "not a compile-time constant" 且锚 `+=` token - 降级形态经
  comma→assign 到达的也是同一 token, e1/e4; 数组操作数 "not an lvalue" e2; `p += p`
  "invalid operands" e3; `++x = 2` e5)。 四闸门: docker-test rc=0(55 例诊断逐字节, 含
  f01-f03/f04/f10/f11); ndiff 空; raw diff 1248 行全部为栈偏移类(1246 行 `%rbp` 偏移 + 1 对
  `sub $N, %rsp` 帧大小), 零指令/标签/`.loc` 变化 - 即契约 4 预告的"局部槽只影响栈偏移"
  (临时量从 sema 标注时的 mid-chain 插入变为整形遍时的链首前插), 同提交重置基线并复验空;
  tinycc rc=0(与 HEAD worktree 的 A/B 对比: 10 处 "failed as expected" + 6 处 "succeeded"
  完全一致 - RESULT A3.1 所记"11 处"是当时的计数口径差, 两份日志实质一致, 差异仅 docker 计时
  噪声)。 自检: 搬走的四函数在 sema 零残留; sema 不调用 codegen; 探针 .s 无重复标号。
  行数: sema.c 3374 -> 3170, codegen.c 1794 -> 2031, chibicc.h 766 -> 768, parse.c 2148 零改动。
- 为什么改: 判据 1 的第 4 行(op= 读写回环)与第 6 行(++/-- 降级含后缀取值)全是发射便利。
  "必须经 new_add/new_sub 而非手拼"由契约 2 的两个陷阱保证(ptr 算术的隐式 cast 与缩放,
  手搓会丢节点与 .loc); combine 的非加减分支走 add_type(移位不做常规算术转换)随文本原样保留。
  eval 侧无需改动: eval2/is_const_expr/eval_double 对带 op 的 ND_ASSIGN 与 ND_INCDEC 本就无
  case, 落到末尾的统一错误/false, 与降级形态的到达路径相比文案与锚点相同(e1/e4 实测)。
- 偏差说明: 三项。
  1. **槽工厂的实现形态是计划未定的第一次落地**: 计划只说"用 codegen 自己的槽"(契约 3(c))。
     实现为 codegen 内 4 行工厂, 不导出 sema 的 new_var(理由见上)。 临时量在 `fn->locals` 里的
     位置相应改变, 只影响栈偏移, raw diff 的 1248 行全部为此类, ndiff 口径免疫。 A6.1 的
     ret_buffer 槽复用同一工厂。
  2. **原子 retry 环的标签走 shape_node(loop) 显式分配**: 契约 3(b) 相位序在"改写发生在整形遍
     内部"这一新形态下的落地 - 新建循环不在语句链上, 遍的下降到不了它。 与今天(HEAD)的到达
     路径相比标签的相对分配顺序不变(都在该语句位置、其 lhs/rhs 子树之后), 归一化第 2 类按
     首现顺序重编号后无差异(ndiff 空实测)。
  3. **语料外角落(记录不修)**: VLA 维度里的复合赋值(如 `int a[n += 2]`)在 parse 层即被拒
     (维度只收 conditional, 报 "expected ']'"), 该形状不可达 - 账本行 6 的 A10.2 用例
     `a[i] += j++` 不受影响; 亦无 eval 路径经此到达。

### A4.1 算术与比较 (本提交)

- 改了什么: 账本行 1(指针缩放与 num+ptr 规范化)与行 7(/>/>= 交换)落位,sema 的 add_type 不再改写
  这两类节点; 账本行 50(求值侧缩放)同步落位。
  (1) `new_add`/`new_sub`/`scale_rhs` 搬 codegen 的整形遍区段(static; 文本原样, 含 2026-09-29
  拍板的 VLA 守卫修复)。`new_arith`/`usual_arith_conv`/`get_common_type`/`new_cast` 留 sema 导出
  (契约 2 永久面), 整形遍的 new_add/new_sub/combine 经它们建树, 产出与降级时代逐字节相同(含两侧
  隐式 cast 与其 `.loc`)。chibicc.h 的两处临时导出声明随之撤除。
  (2) `add_type` 的 ND_ADD/ND_SUB 改为检查+定型: ADD 的 `ptr+ptr` 报 `invalid operands`(锚 `tok`,
  与原 new_add 相同), SUB 的 `ptr-ptr` 定型 ty_long(除法树在整形遍建), 其余定型
  `get_common_type`。**不插 cast**: 隐式 cast 由整形遍的 new_arith 统一补(此处若也插, 树里会有
  两套 cast; 且 new_arith 会做与降级完全一致的 `is_numeric`/base 判定, 语义结论不重写)。
  (3) `shape_node` 新增 ND_ADD/ND_SUB case: shape_children(契约 3(a))后经 new_add/new_sub 重建
  并原位换形; 新增 ND_GT/ND_GE case: 交换操作数成 LT/LE。A1.1 埋的 eval2 忠实 case 本步激活。
  (4) `eval2` 的 ND_ADD/ND_SUB 是本步唯一的新逻辑(eval 侧缩放): 指针侧按操作数**自身** `ty`
  判定(忠实树没有可穿透的 conv cast; 显式 cast 必须保有目标类型 - `(int*)0+2` 作 VLA 维度实测
  抓到过穿透读), 数字侧乘元素尺寸; `num + ptr` 按源码序接受、指针侧经 eval2 带 label 求值(保
  `2 + gp` 初始化器的 "invalid initializer" 锚点在指针操作数上); VLA 元素尺寸报非常量; ptr-ptr
  除法支沿用 A1.1(两侧以 label==NULL 求值)。`ty_beyond_convs`(A1.1 为缩放埋的穿透工具)随用途
  消失而删除。
  测试: corpus 41 文件 A/B(与 A5.1 二进制): 37 逐字节相同; attribute/offsetof/stdhdr/varargs 的
  `.file` 为 include 路径解析伪影(base 二进制在 /tmp,a 有差异, 非 .file 差异 0 行)。探针 17 组
  (op=/自增自减全形状、指针与数组、位域与原子、elvis+自增、`(int*)0+2` 作 VLA 维度、5 个错误
  路径)逐字节相同。
- 为什么改: 判据 1 的第 1/7 行(缩放与换序都是发射便利); eval 侧缩放是行 50 的既定归属(eval 是
  求值器, 常量折叠必须自己认忠实形态)。
- 偏差说明: 三项, 前两项是**计划未列的整形遍重入缺陷**, 均由 raw 快照抓到并当步修复:
  1. **elvis 降级留下陈旧字段别名**(sema 修复): `a ?: b` 降级把 ND_COND 节点原地改成 ND_COMMA
     时只设 kind/lhs/rhs, 原 COND 的 `cond`/`els` 字段仍指向 lhs/rhs 里同一批节点。整形遍的
     shape_children 会沿陈旧字段再走一遍已整形的子树 - A5.1 前无害(那时表达式 case 只有下标与
     成员), A4.1 的 ND_ADD case 令其二次缩放/二次 cast(`ASSERT(4, ({int i=3; ++i?:10;}))` 实测
     多 2 行 `.loc`)。修复: 降级处显式断开三字段(逗号形态只读 lhs/rhs, 别名断开无其它影响)。
     这是契约 3(d)"不得重入"对树侧的要求 - 树应无别名, 而非要求遍自带 visited 集。
  2. **原子 retry 环的 shape 下潜重入**(codegen 修复, A5.1 已引入): A5.1 为给 codegen 自建的
     do-while 拿标签调了 `shape_node(loop)`, 该下潜会进入环内 - 环体里的 combine ADD 属新树,
     二次 new_add(实测 `(*x)++`/`*x += 5`/`x--` 各多 2 行 `.loc`)。修复: 直接分配
     `brk_label`/`cont_label`(环内无循环/switch/break/label 可整形), 分配顺序与全树下潜一致。
  3. **`.file` 伪影照旧**: base 二进制与 new 二进制的 include 搜索路径不同(dirname(argv0)),
     四个含 chibicc 自带头的用例出现 `.file` 行差异, 非 .file 差异 0 行(A3.1 已记账同款)。
  修复 1/2 后 raw diff **全空**: 本步不新建局部槽, 栈偏移、标签分配顺序、`.loc` 序列全部逐字节
  一致, 基线无需重置(与 A5.1 的栈偏移类差异不同 - 那步移动了槽的创建时机, 本步没有)。
  行数: sema.c 3170 -> 3131, codegen.c 2031 -> 2134, chibicc.h 768 -> 766, parse.c 2148 零改动。

## 给审核者的提示

- 审核重心: sema 的 `add_type`(每次提交都应少掉若干"改写树形状"的 case, 且剩下的 case 只填
  `ty`/写结论/做检查), codegen 新增的整形遍(是否忠实搬运、是否自带槽与标签分配、是否遵守契约 3
  的六条不变量), 以及 `test/diagnostic.sh` 的 55 例是否逐字节不变.
- 因 codegen 的"零改动红线"在本线作废, 对比口径从"`git diff 5f53ed0 -- codegen.c` 为空"改为
  "codegen 的改动全部可归入清单表的 codegen 侧项".
- 每步都应能回答两个问题: 这一步搬的代码在 sema 侧还有没有调用者(契约 1), 以及这一步有没有让
  某个"创建匿名全局"或"写 init_data"的动作换位置(契约 4).
