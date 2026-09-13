# PLAN.md - 语法语义拆分执行计划

本文件细化 AGENTS.md 路线图中"语法语义拆分"线(对应路线图阶段 3 + 4), 是可勾选的执行清单. CST/trivia 线(路线图阶段 1-2)与库化阶段(阶段 5)暂缓, 与本线正交.

新会话恢复方法: 通读 AGENTS.md -> 查看本文件勾选状态 -> `git log --oneline` 确认最后完成的步骤 -> 从第一个未勾选项继续; 开工前先跑 `make docker-test` 确认基线为绿.

行号以拆分前基线(上游 commit 5f53ed0)为准, 落地后会漂移, 定位一律以函数名为准.

## 纪律

1. 每个编号步骤结束 `make docker-test` 必须全绿; P3 起加跑 `make test-stage2`(自举). 一步一提交; 返工 = revert 单个提交.
2. codegen.c 全程零改动: 每引入一个忠实节点, sema 将其降级回今天 parse 的输出形状(逐节点一致, 汇编不变).
3. 搬家优先: 语义函数先原样搬进 sema.c(parse.c 跨文件调用), 后续步骤再翻转调用方式; 标注(返工点)处是刻意保留的中间态.
4. 本线不做(属库化阶段): 错误回调化, context 对象, 公共/内部头拆分. static 全局仅在 P3.1 做持有权搬家, 不做 context 化.
5. sema 的实现形态: `analyze()` 单遍 = 递归处理子树 -> `add_type` 本节点 -> 在同一 switch 里对特定 kind 就地降级改写(先子树后本节点, 改写产物与今天 parse 的输出逐节点一致).

## 执行与汇报流程

- 开始执行后按编号连续推进, **不等待用户逐步确认**; 汇报完一步, 自动开启下一步.
- 每一步结束时向用户汇报, 内容固定包含四项:
  1. **改了什么**: 本步改动的文件/函数/新增节点, 用简短列表说明;
  2. **为什么改**: 该步在拆分目标中的位置(搬走了哪类语义, parse 侧少了什么职责), 以及刻意保留的中间态与返工点(若有);
  3. **测试结果**: `make docker-test` 是否通过(附耗时量级), P3 起加自举结果, 快照 diff 是否为空(0.1 完成后);
  4. **偏差说明**: 实现与计划不符或临时调整方案时, 明确指出并说明理由.
- 仅在无法自行解决的阻塞(测试修不绿且原因不明, 方案前提失效等)处停下向用户求助; 其余情况不打断.
- 每步完成即勾选本文件对应 checkbox 并提交, 保证任何时刻重开会话都能从勾选状态与 git log 无损接续.

## P0 基建

- [x] **0.1 汇编快照脚本**: 新增 make 目标, 在 docker 内构建 chibicc 后对全部 `test/*.c` 跑 `./chibicc -S` 存档 .s(遵循 docker-test 的 /work 可写层约束, 产物目录加入 .gitignore), 并提供重新生成 + diff 命令. 之后每步额外验证快照 diff 为空.

- [ ] **0.2 建 sema.c(纯搬家, 零行为变化)**: type.c 的 `get_common_type`(134-161), `usual_arith_conv`(170-174), `add_type`(176-307) 移入 sema.c; parse.c 的 `eval` 全家(1828-2025: eval/eval2/eval_rval/is_const_expr/const_expr/eval_double)移入 sema.c, parse.c 保留跨文件调用(`const_expr` 被数组维度/枚举/case/位域宽度/_Alignas/attribute 等处调用); 节点与变量构造器(`new_node/new_binary/new_unary/new_num/new_long/new_ulong/new_var_node/new_vla_ptr/new_cast/new_lvar/new_gvar/new_anon_gvar/new_string_literal/new_unique_name`)提升为跨文件, 声明加入 chibicc.h; static 清单(locals/globals/scope 等)仍留 parse.c.

## P1 表达式层忠实化(10 步)

- [ ] **1.1 ND_GT/ND_GE**: 新增两个 kind; `relational()`(2292-2321) 的 `>` `>=` 不再交换操作数; sema 在比较运算的 case 处降级(改写回 ND_LT/ND_LE + 交换).

- [ ] **1.2 elvis**: `conditional()`(2194-2204) 的 GNU `a ?: b` 分支发 `ND_COND{is_elvis}`; sema 展开 `tmp = a, tmp ? tmp : b`(只用纯 `=`, 不依赖 1.3).

- [ ] **1.3 复合赋值(拆两个提交)**: `Node` 加 `NodeKind op` 字段(0 表示纯 `=`); `assign()`(2145-2183) 的 10 个 `op=` 分支改发 `ND_ASSIGN{op}`(不再经 `new_add/new_sub`, 缩放交给 sema); 第一个提交切 `+= -=`(原走 new_add/new_sub, 改动最大), 第二个提交切其余 8 个; `to_assign`(2033-2140) 搬 sema, 三个分支(member/atomic/plain)原样保留, 由 `node->op` 驱动.

- [ ] **1.4 ND_INCDEC**: 新 kind + `is_post`/addend 字段; 前缀(`unary()` 2523-2528)与后缀(`postfix()` 2857-2867)改发; `new_inc_dec`(2790-2795) 搬 sema 作降级.

- [ ] **1.5 ND_SUBSCRIPT**: `postfix()`(2834-2841) 改发; `init_desg_expr`(1316-1329) 的数组分支同步; sema 降回 `DEREF(ADD)`(含指针缩放).

- [ ] **1.6 is_arrow**: `postfix()`(2849-2855) 的 `->` 不再偷插 `DEREF`, 改为 `ND_MEMBER.is_arrow`; sema 补插 DEREF.

- [ ] **1.7 裸指针算术**: `add()/sub()` 直接发 `ND_ADD/ND_SUB`; `new_add`(2350-2377)/`new_sub`(2380-2414) 搬 sema 作降级(缩放乘法/`1+p` 规范化/ptr-ptr 除法, 逐分支与今天一致); 前提: 1.3/1.4/1.5 已切断其余调用者; "invalid operands" 报错随之移入 sema, 位置锚定运算符 token.

- [ ] **1.8 ND_STRING**: `primary()`(3107-3111) 改发字符串节点; sema 建匿名全局并改写为 `ND_VAR`.

- [ ] **1.9 ND_SIZEOF/ND_ALIGNOF**: `primary()` 四个分支(3000-3034)改发(节点保留操作数类型/未求值操作数); `compute_vla_size`(813-832) 搬 sema; sema 折叠(定长 -> `new_ulong`, VLA -> vla_size 引用). `__builtin_types_compatible_p`/`__builtin_reg_class` 的折叠暂留 parse(返工点).

- [ ] **1.10 ND_WHILE/ND_BREAK/ND_CONTINUE**: `stmt()`(1669-1685, 1728-1744)改发; sema 降回 `ND_FOR`(带合成 brk/cont 标签)/`ND_GOTO`; "stray break/continue" 合法性检查暂留 parse(返工点).

## P2 声明与初始化(3 步)

- [ ] **2.1 ND_DECL**: 新 kind 挂 `Obj *var` + `Initializer *init`; `declaration()`(844-909) 每个声明符发一个 ND_DECL(不再拍平成赋值逗号链; 外层 ND_BLOCK 包装取消; for-init 同步); `create_lvar_init/init_desg_expr/lvar_initializer`(1316-1389) 搬 sema 作降级(MEMZERO + comma 生成与今天逐节点一致); `gvar_initializer/write_gvar_data`(1416-1494) 一并搬 sema. 返工点: `string_initializer` 的字符折叠与 static 局部变量的 gvar 创建暂留 parse(3.4 翻转).

- [ ] **2.2 VLA 忠实化**: `declaration()` 的 VLA 分支(873-888)改发 ND_DECL 形状, 树内噪音(`EXPR_STMT(NULL_EXPR)` 前缀, alloca 赋值语句)消失, 由 sema 生成; `array_dimensions`(636-652) 的 `const_expr` 判定暂留 parse(返工点).

- [ ] **2.3 ND_COMPOUND_LITERAL**: `postfix()`(2808-2824) 改发; sema 建隐藏 lvar(块内)或匿名全局(文件域).

## P3 名字解析出解析器(5 步)

- [ ] **3.1 清单持有权**: `locals/globals` 移 sema.c 持有(纯搬家; parse 经由已跨文件的 `new_lvar` 等间接使用).

- [ ] **3.2a ND_TYPEDEF 节点**: `parse_typedef`(3130-3144) 发声明形状节点(名字 + Type), 为 sema 重建作用域铺路; parse 自身仍同步登记 oracle.

- [ ] **3.2b 标识符翻转(最大单步)**: `primary()`(3082-3105) 的标识符发未解析名字节点(变量/枚举/函数引用共用, 语法期本就无法区分); sema 前置 resolve 遍历: 建作用域栈(块/for 作用域由树结构给出; typedef/tag 仍以 parse 的 oracle 为准, 经传递供 sema 查询), 绑定变量与枚举常量, 收集 static inline 的 refs; "undefined variable" 等错误触发时机后移, 锚定同一标识符 token, 文案不变. 已知中间态: `new_var` 仍会向 parse 的作用域表 push 条目, 与 sema 的解析作用域并存, P4.2 清理.

- [ ] **3.3 函数语义搬家**: `function()`(3199-3262) 只留语法形状; `create_param_lvars`, 隐藏 struct 返回缓冲, `va_area`, `alloca_bottom`, `__func__`/`__FUNCTION__` 移 sema.

- [ ] **3.4 清返工点**: static 局部 gvar 创建移 sema(ND_DECL 降级时建匿名全局); 枚举忠实化 - `enum_specifier`(751-794) 发声明节点(成员名 + 可选显式值), 值求值与注册移 sema; VLA 判定(`array_dimensions` 的 const_expr 调用)移 sema; `resolve_goto_labels`(3160-3174)/`mark_live`(3187-3197)/`scan_globals`(3303-3327) 归位 sema.

## P4 收尾(2 步)

- [ ] **4.1 检查归位盘点**: 逐条审视 parse.c 内残余语义检查(`funcall` 实参数量与类型 2878-2912, stray 系列, redefinition, incomplete type 等), 决定留 parse(语法可判)或移 sema; `test/driver.sh` 的精确文案断言逐条核对.

- [ ] **4.2 布局翻转与收官**: `struct_decl/union_decl`(2678-2735) 的布局计算改由 sema 驱动(parse 只建语法形状); 删除 parse 残留的类型依赖与 3.2b 遗留的双重作用域条目; `make docker-test` + `make test-stage2` + 快照 diff 全绿收官.

## 终态验收

- parse.c 3368 -> 约 2100 行(纯语法); sema.c 约 1300 行; chibicc.h 增约 60 行; **codegen.c 0 行 diff**.
- 全部测试 + 自举 + 汇编快照 diff 三道闸门全绿.
