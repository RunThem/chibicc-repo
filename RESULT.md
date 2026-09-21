# RESULT.md - codegen 直连 sema 产物执行记录

本文件记录 `PLAN.md`(codegen 直连 sema 产物线, 取消 sema 降级)各步骤的实际执行结果, 供审核与
后续会话接续参考. 前两条线已归档: 语法语义拆分见 `PLAN-split.md` / `RESULT-split.md`, 忠实层
收尾见 `PLAN-faithful.md` / `RESULT-faithful.md` - 后者记录的终态(20abd43)是本线的量化对比基线.

## 闸门口径(本线生效)

- **[搬迁] 步骤**(整形逻辑换位置, 发射形态不变): `make docker-test`(含自举) +
  `test/diagnostic.sh`(44 例逐字节) + `make docker-snapshot-ndiff` 为空; 同提交内
  `make docker-snapshot` 重置 raw 基线.
- **[就地] 步骤**(发射形态变化): `make docker-test` + 诊断锁定 +
  `make test-thirdparty THIRDPARTY=tinycc` + 该步新增的形状断言; ndiff 允许非空, 差异须逐条归入
  预期模式并记在本文件的偏差记录里.

## 基线记录(A0.1 完成后填写)

- 起始状态: HEAD = 20abd43(忠实层收尾终态), parse.c 2148 行, sema.c 3145 行, chibicc.h 704 行,
  codegen.c 1595 行(相对 5f53ed0 零 diff).
- docker-test: (待填)
- 诊断锁定: (待填)
- raw / 归一化 diff: (待填)
- tinycc(`make test-thirdparty THIRDPARTY=tinycc`): (待填)

## 整形清单表(账本初版, A0.1 核对, 实施时逐项打勾)

判据: 1 = 语义结论 vs 发射便利; 2 = 检查留 sema; 3 = 对象物化归属; 4 = 结论记节点上.

| # | 项 | 现位置 | 归属 | 目标位置 | 步 |
|---|---|---|---|---|---|
| 1 | `+`/`-` 指针缩放与 `num+ptr` 规范化 | `add_type` ND_ADD/ND_SUB, `new_add`/`new_sub`/`scale_rhs`/`new_arith` | 1 发射便利 | codegen 整形遍或发射点 | A4.1 |
| 2 | 常规算术转换插入隐式 cast | `usual_arith_conv`(多处调用) | 1 语义结论 | sema 留 | - |
| 3 | 一元 `-` 的操作数提升 | `add_type` ND_NEG | 1 语义结论 | sema 留(只插 cast) | - |
| 4 | `op=` 的读写回环 | `add_type` ND_ASSIGN(op), `to_assign`/`compound_op` | 1 发射便利 | codegen(原子特例进整形遍) | A5.1 |
| 5 | 赋值 rhs 转换 + `not an lvalue` | `add_type` ND_ASSIGN | 1 结论 / 2 检查 | sema 留 | - |
| 6 | `++`/`--` 降级(含后缀取值) | `add_type` ND_INCDEC, `new_inc_dec` | 1 发射便利 | codegen | A5.1 |
| 7 | `>`/`>=` 交换成 `<`/`<=` | `add_type` ND_GT/ND_GE | 1 发射便利 | codegen | A4.1 |
| 8 | 调用者检查(不是函数/实参个数/float 提升) | `lower_funcall` | 2 检查 | sema 留 | - |
| 9 | 实参到形参类型的隐式 cast | `lower_funcall` | 1 语义结论 | sema 留 | - |
| 10 | `func_ty` 标注 | `lower_funcall` | 1 标注 | sema 留 | - |
| 11 | 调用者返回缓冲槽 `ret_buffer` | `lower_funcall` | 3 实现的槽 | codegen | A6.1 |
| 12 | `pass_by_stack` | codegen `push_args` | - | 已是 codegen | - |
| 13 | 一元/位/移位运算定型 | `add_type` | 1 标注 | sema 留 | - |
| 14 | 字符串字面量物化(匿名全局) | `add_type` ND_STRING, `new_string_literal` | 3 语义对象 | sema 留(位置与顺序不变) | A9.1 |
| 15 | ND_STRING → ND_VAR 的形状改写 | `add_type` ND_STRING | 1 发射便利 | codegen(读 `node->var`) | A9.1 |
| 16 | 名字绑定(变量/函数/枚举常量) | resolve 遍 `bind_ident` | 1 语义结论 | sema 留 | - |
| 17 | `sizeof`/`_Alignof` 折叠 | `add_type` | 1 语义结论 | sema 记结论, 消费方读 | A1.2/A9.1 |
| 18 | `__builtin_types_compatible_p` / `__builtin_reg_class` 折叠 | `add_type` | 1 语义结论 | 同上 | A1.2/A9.1 |
| 19 | `_Generic` 选中关联项 | `add_type` + `select_generic` | 1 语义结论 | 同上(选中项记字段) | A1.2/A9.1 |
| 20 | elvis `a ?: b` 的临时量构造 | `add_type` ND_COND | 1 发射便利 | codegen(优先无槽) | A9.1 |
| 21 | `?:` 两臂转换与 void 定型 | `add_type` ND_COND | 1 标注 | sema 留 | - |
| 22 | 成员绑定/匿名展平/三类校验 | `resolve_member`(pass 2 调用) | 1 结论 + 2 检查 | sema 留 | - |
| 23 | `->` 补插 DEREF | `resolve_member` | 1 发射便利 | codegen | A3.1 |
| 24 | 取地址的位域检查与定型 | `add_type` ND_ADDR | 2 检查 + 1 标注 | sema 留 | - |
| 25 | `x[y]` → `*(x+y)` | `add_type` ND_SUBSCRIPT | 1 发射便利 | codegen | A3.1 |
| 26 | `*foo` 函数消解(6.5.3.2p4) | `add_type` ND_DEREF | 1 语义结论 | sema 留(只定型, 不换节点) | A3.1 |
| 27 | 解引用检查与定型 | `add_type` ND_DEREF | 2 检查 + 1 标注 | sema 留 | - |
| 28 | 语句表达式的值语义 | `add_type` ND_STMT_EXPR | 2 检查 | sema 留(简化: 末语句是记录即无值) | A8.1 |
| 29 | `&&label` 登记(配对检查用) | `add_type` ND_LABEL_VAL | 2 检查 | sema 留(名字归 codegen) | A2.1 |
| 30 | 原子 CAS/EXCH 定型与检查 | `add_type` ND_CAS/ND_EXCH | 1 标注 + 2 检查 | sema 留 | - |
| 31 | 显式 cast 定型 | `add_type` ND_CAST(`ty_op`) | 1 标注 | sema 留 | - |
| 32 | `return` 到返回类型的隐式 cast | `add_type` ND_RETURN | 1 语义结论 | sema 留 | - |
| 33 | ND_DECL 展开成语句(VLA alloca, MEMZERO+赋值链) | `add_type` ND_DECL, `compute_vla_size` | 1 发射便利 | codegen | A8.1 |
| 34 | 块域 static 的数据镜像序列化 | `add_type` ND_DECL, `gvar_init_data` | 3 对象模型 | sema 留 | - |
| 35 | 复合字面量: 无名字对象创建 | `add_type`(resolve 侧已建) | 3 语义对象 | sema 留 | A8.3 |
| 36 | 复合字面量: 初始化链 + 对象引用 | `add_type` ND_COMPOUND_LITERAL | 1 发射便利 | codegen | A8.3 |
| 37 | 记录节点出链(TYPEDEF/ENUM_CONST/GVAR_DECL/FUNCDEF) | `type_chain` | 1 发射便利 | codegen(记录在 sema 输出中保留) | A8.1 |
| 38 | for-init 的 ND_BLOCK 包装 | `add_type` ND_FOR | 1 发射便利 | codegen | A8.1 |
| 39 | 循环/switch/case/label 的标签分配 | `analyze` | 1 发射便利 | codegen(标签栈 + 计数器) | A2.1 |
| 40 | ND_WHILE → ND_FOR | `analyze` | 1 发射便利 | codegen | A2.1 |
| 41 | break/continue → goto | `analyze` | 1 发射便利 | codegen | A2.1 |
| 42 | case 链(`case_next`/`default_case`) | `analyze` | 1 发射便利 | codegen | A2.1 |
| 43 | stray case/default/break/continue 四检查 | `analyze` | 2 检查 | sema 留 | - |
| 44 | goto/label 配对检查 | `analyze` + `resolve_labels` | 2 检查 | sema 留 | - |
| 45 | 初始化器解析(designator 求值/brace elision/柔性数组) | `resolve_initializer` 一组 | 1 语义结论 | sema 留(`InitTree` 结构进共享头) | A8.2 |
| 46 | 局部初始化链 + MEMZERO | `create_lvar_init`/`lvar_init_comma`/`init_desg_expr` | 1 发射便利 | codegen | A8.2 |
| 47 | 全局初始化序列化 | `write_gvar_data`/`gvar_init_data` | 3 对象模型 | sema 留(经 `eval` 认忠实形态) | A1.1 |
| 48 | 帧辅助对象(参数/`__va_area__`/`__alloca_size__`/大返回缓冲参数) | `begin_function` | 3 ABI 对象 | sema 留 | - |
| 49 | `Type::vla_size` 与 VLA 尺寸表达式 | `add_type`/`compute_vla_size`/`vla_size_expr` | 3 实现的槽 | codegen(字段改标 codegen 侧) | A7.1 |

## 关键结论(开工前分析, 供审核者速览)

- 判据 1 的落点: 清单里"发射便利"共 16 项, 全部移 codegen; "语义结论/标注"共 17 项留 sema.
- 判据 2 的落点: 清单里 9 项是检查, 全部留 sema; 其中 3 项(5/8/33 里的 VLA 不得初始化)今天夹在
  降级代码里, 搬运时必须先在新位置显式落地再删降级.
- 归 codegen 的物化只有 4 类: 调用者返回缓冲(11), VLA 尺寸(49), 读写回环与 elvis 的临时量(4/20),
  原子 op= 的内部槽(4 的原子分支).
- 求值器与全局序列化(47)是"新逻辑"而非搬家: `eval2`/`is_const_expr` 开头调 `add_type`, 今天免费
  拿到整形后的形状; 整形移走后必须自己认忠实形态(A1.1).
- 顺序敏感: 全局匿名对象的创建顺序进 `.data` 块序(ndiff 不折叠), 所以 14(字符串物化)与 35(复合
  字面量对象)留在标注遍的同一遍历位置; 局部槽顺序只影响栈偏移(ndiff 第 3/4 类免疫), 可自由搬迁.

## 提交一览

| 提交 | 步骤 | 内容 |
|---|---|---|
| (本提交) | 线间归档 | 忠实层收尾线归档为 `PLAN-faithful.md`/`RESULT-faithful.md`, 新建本线与账本 |

## 给审核者的提示

- 审核重心: sema 的 `add_type`(每次提交都应少掉若干"改写树形状"的 case, 且剩下的 case 只填
  `ty`/写结论/做检查), codegen 新增的整形遍(是否忠实搬运、是否自带槽与标签分配), 以及
  `test/diagnostic.sh` 的 44 例是否逐字节不变.
- 因 codegen 的"零改动红线"在本线作废, 对比口径从"`git diff 5f53ed0 -- codegen.c` 为空"改为
  "codegen 的改动全部可归入清单表的 codegen 侧项".
