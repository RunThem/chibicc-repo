# RESULT.md - 忠实层收尾执行记录

本文件记录 PLAN.md(忠实层收尾线)各步骤的实际执行结果, 供审核与后续会话接续参考. 前一线(语法语义拆分)的计划与执行记录见归档 `PLAN-split.md` / `RESULT-split.md` - 其终态(02e5c13)是本线的量化对比基线.

## 闸门口径(本线生效)

- 常规步骤: `make docker-test`(含自举) + `make docker-snapshot-diff` 为空 + 诊断锁定测试.
- **[重组]** 步骤: 行为三闸门(docker-test / tinycc / 诊断测试) + 归一化 diff(`make docker-snapshot-ndiff`)为空, 同提交内 `make docker-snapshot` 重置 raw 基线, 提交说明声明"含预期字节变化".

## 基线记录(R0.3 完成后填写)

- docker-test: 2026-09-19, HEAD = 79e6c76(R0.2), 退出码 0 - 测试集 + driver.sh + 自举重跑全过; 诊断锁定 stage1/stage2 各 "43 cases byte-exact".
- tinycc: 退出码 0(test/thirdparty/make shim 跳过 Rosetta 下不稳定的 106_pthread/112_backtrace/113_btdll 三个用例, 其余全过).
- raw / 归一化 diff: `docker-snapshot-diff` 空 + `docker-snapshot-ndiff` 空; raw 基线当时沿用拆分线终态后的快照, 现已在 R1.2(首个 [重组] 提交)内重置 - 见该步详情.
- 诊断锁定测试: docker-test 内 stage1 + stage2 两遍全绿; 宿主机本地(macOS)另跑 43/43.

## 提交一览

| 提交 | 步骤 | 内容 |
|---|---|---|
| 16a6719 | R0.1 | 诊断锁定测试(test/diagnostic.sh, 43 用例) |
| 79e6c76 | R0.2 | 归一化快照 diff 闸门(snapshot-normalize.awk + docker-snapshot-ndiff) |
| 8a69232 | R0.3 | 基线复验留档(四闸门全绿) |
| 9d66036 | R1.1 | 匿名名计数器与名字物化三函数迁 sema.c |
| 956a662 | R1.2 前置 | test/control.c 补控制流形状覆盖(纯测试) |
| 4bf8aec | R1.2 | analyze 前序语句下降入 sema: 标签分配 + break/continue/case 绑定 + stray 四检查 |
| 162cd2a | R2.1 | 成员访问两段式: ND_MEMBER 留未绑定名字, 成员链/四类检查/DEREF 补插归 sema |
| d281858 | R2.2 | `_Generic` 改发忠实节点(ND_GENERIC + assoc), 两个类型 builtin 折叠移 sema |
| 332f31e | R2.3 | 常量求值出 parse 第一批: 数组维度/typeof/aligned 与 _Alignas/case 值改记表达式, resolve_type 回填 |
| 47e0040 | R2.4 | 初始化器忠实化: 忠实 brace 记录入树, 定位/越界/brace elision/灵活长度全部移 sema |
| 5f07540 | R2.5 | 位域宽度记表达式, 布局动作转 sema 私有(反悔 4.2a), 收尾触发落在定义完成点 |
| 516c4a0 | R2.6 | 拆全部构造现场调用, sema 改 resolve + 标注/降级两趟独立遍历, parse.c 达层 3 形态 |
| 59b6d2a | R2.7 | 作用域表一分为二: parse 私有 typedef/tag 影子栈 + sema 私有变量/枚举栈(4.2b 共享表解体) |
| (本提交) | R2.8 | `in_file_scope` oracle 删除, 复合字面量的存储类改由 resolve 上下文标志给出 |

## 各步详情

### R0.2 归一化 diff 目标 (79e6c76)

- 改了什么: 新增根目录 `snapshot-normalize.awk` - 汇编快照归一化器, 三条规则: (1) 折叠 `.loc`/`.file` 行; (2) 末段为 ".纯数字" 的 `.L` 标签按文件内首现顺序重编号; (3) 负 rbp 局部偏移按每函数首现顺序映射为 `-N(%rbp)` 序号, 函数边界 = 列 0 的非点号标签. 新增 Makefile 目标 `docker-snapshot-ndiff` - 与 `docker-snapshot-diff` 同源生成新快照, 对基线与新快照施加同一归一化后 `diff -ru`, 文件集不一致单独报 FILE-SET MISMATCH. raw 闸门与 raw 基线 `.cache/snapshot` 不动. 另入库 `snapshot-normalize-test.sh` - 归一化器性质自测脚本(需先 `make docker-snapshot` 生成基线, 手动运行).
- 为什么改: [重组] 步骤(R1 起)允许汇编字节变化, 形状闸门改用归一化 diff - 三规则分别吃掉 R3 语句链整形的调试行号重排, R1.1/R2.6 的标签计数器交织与分配后置, R2.6/R2.8 的隐藏变量创建时机变化引起的 lvar 偏移重排; 同时结构性变化(指令增删, 标签前缀, 符号名, 数据内容)必须仍然可见.
- 测试结果: 本地性质自测 7 项全绿(snapshot-normalize-test.sh): 41 文件幂等; 同前缀标签双射互换(`.L..0` <-> `.L..1`)被吃; 偏移双射互换(`-112(%rbp)` <-> `-224(%rbp)`)被吃; 跨前缀互换(`.L..N` <-> `.L.end.M`)仍被抓; 删除 mov/jump 指令仍被抓; `.loc`/`.file` 折叠(单文件 462 行). 对当前 HEAD: `docker-snapshot-diff` 空 + `docker-snapshot-ndiff` 空 - R0.2 验收达标(四闸门之二, 其余两项 R0.3 补齐).
- 偏差说明: PLAN 原文 "`.L..N` 标签按首现顺序重编号" 实现为 "末段为 '.纯数字' 的 `.L` 标签" - 覆盖全部六族计数器标签(`.L..N` 与 `.L.begin/end/else/true/false.N`), 同时保证 `.L.return.<函数名>` 永不重编号(函数名是语义; 即使函数名以数字结尾, 尾段也不是 '.纯数字'). 标签映射按完整 token 建键, 跨前缀互换(前缀变化 = 结构变化)仍被抓, 这是故意的. 负偏移渲染为 `-N(%rbp)`, 映射逐值唯一无歧义, 不做对齐假设(实测存在 -1/-10 等非对齐值). awk 只用 POSIX 特性, 归一化在宿主机侧执行, 两侧同一实现.

### R0.1 诊断锁定测试 (16a6719)

- 改了什么: 新增 test/diagnostic.sh - 43 个用例, 每个用例 = 触发一处诊断的独立源码片段 + 期望 stderr 原文, 在临时目录以 `<用例名>.c` 编译后与期望逐字节 diff; 随 make test 与 make test-stage2 对 chibicc 与自举编译器各跑一遍(Makefile 两处接线). 编译器源码零改动.
- 为什么改: R2 各步要把大量检查从 parse 移入 sema, 移动纪律是 "文案 + 锚点" 保真 - 本测试是该纪律的回归闸门; 同时把拆分线 4.2c 的两处诊断时机偏差拍板为规范(锁进用例): "too many arguments" 锚调用右括号(ND_FUNCALL.tok), 块域 `void x = <初始化器>;` 锚初始化器之后的 token, 块域 static 路径(declare_static_local)仍锚 `=`.
- 测试结果: 本地(macOS) 43/43 通过; `make docker-test` 退出码 0, diagnostic lock 在 stage1/stage2 各报 "43 cases byte-exact"; `make docker-snapshot-diff` 为空.
- 偏差说明: 覆盖 = 判定表 A 15 处 + B 4 处 + C 11 处 + D 3 处(parse.c 33 处 error_tok 全部) + 判定表 E 9/10 处. 唯一未覆盖: sema.c 的 "redeclared as a different kind of symbol" - find_func 只返回 `is_function` 为真的对象, 该守卫恒假, 是拆分线遗留的死检查, 任何输入不可达(内置 alloca 未注册进作用域表, `void alloca() {}` 亦不触发), 已在测试头注释记录. 另两点现状一并锁定: 文件域 `void x;` 被接受(无检查, 无用例), 块域 `int x; int x;` 被接受(重定义检查只对函数定义生效). 触发写法的非显然点, 供 R2 维持用例时参考: `.5` 被词法为浮点字面量(字段指示符用 `.+` 触发); `typedef static int x;` 合法(typedef 组合检查只拦 typedef 携带两个以上其他存储类, 用 `typedef static extern` 触发); `unsigned unsigned` 合法(符号位是 `|=`, 位掩码用 `char float` 触发); "expected string literal" 属 asm 语句而非初始化器(`asm(1)`); `int ()` 走全局变量路径(函数名省略用 `int ()()` 触发); `goto` 缺标签是 get_ident 的可达路径.

### R0.3 基线复验 (8a69232)

- 改了什么: 无代码改动 - 在 HEAD = 79e6c76 上跑齐四项闸门, 结果写入上方"基线记录", 作为本线后续所有步骤的对照基线.
- 为什么改: 本线开工基线必须留档; 自此归一化 diff 闸门(docker-snapshot-ndiff)正式启用, raw 基线保持拆分线终态后的快照, 待首个 [重组] 提交内重置.
- 测试结果: 四项全绿 - docker-test 退出码 0(测试集 + driver.sh + 自举重跑, 诊断锁定 stage1/stage2 各 43/43); tinycc 退出码 0; raw diff 空; 归一化 diff 空. 另: 宿主机本地(macOS)构建后 diagnostic.sh 43/43.
- 偏差说明: 无 - 编译器源码零改动, 工作区仅剩与本线无关的未跟踪文件 .zcodeignore.

### R1.1 匿名名计数器归 sema (9d66036)

- 改了什么: `new_unique_name`(含计数器 static)与 `new_anon_gvar`/`new_string_literal` 两个调用方从 parse.c 整体迁入 sema.c, 紧邻 new_gvar 等变量构造器; chibicc.h 的声明区注释同步(计数器所有权说明). parse.c 对这三个函数只剩跨文件调用(复合字面量的 new_anon_gvar, R2.8 收编).
- 为什么改: R1.1 的核心是计数器所有权 - 匿名名(字符串字面量全局, static 局部, 控制流标签)的唯一发号器归 sema, 为 R1.2 把标签分配触发点移入遍历扫清所有权问题; 拆分线 1.10 的"计数器交织"顾虑自此从所有权层面解除.
- 测试结果: 四项全绿 - docker-test 退出码 0(诊断锁定 stage1/stage2 各 43/43), tinycc 退出码 0, raw diff 为空, 归一化 diff 为空. raw 为空符合预期: 本步只搬定义, parse 侧分配触发点的调用顺序逐点不变, 计数器交织原样保留.
- 偏差说明: 步骤分工与 PLAN 文本有出入 - PLAN 把"brk/cont 标签分配移 sema"与"ND_LABEL/ND_LABEL_VAL unique_label 机制移入"列在本步, 实际这两项与绑定/stray 检查在构造现场标注架构下不可分离(parse 侧 break 绑定要求标签先于体解析而存在, 分配触发点无法先于绑定单独迁移), 故全部并入 R1.2 的前序语句下降一次完成; 本步只承担计数器所有权搬迁, 无行为变化. 归类为实现形态调整.

### R1.2 前置: test/control.c 形状覆盖 (956a662)

- 改了什么: test/control.c 新增 10 条 ASSERT(+18 行), 覆盖 R1.2 要重新绑定而此前无覆盖的控制流形状: switch 体内埋藏的 case 标签(循环内, Duff's device 形状, 普通块内三例), 嵌套 switch 的 case 与 break 不得串台(三例), break/continue 跨语句表达式抵达外层循环(两例), goto 跨语句表达式跳到外面的标签(一例). 编译器源码零改动.
- 为什么改: R1.2 把绑定从"解析现场维护上下文"换成"整棵树下降一遍", 两条实现路径只在非平凡形状上分叉 - 埋藏 case, 嵌套 switch, 跨语句表达式的跳转. 先把这些形状钉进测试, 重组才有行为面判据. 期望值全部先用宿主机 clang 算出再写入, 不拿 chibicc 自证.
- 测试结果: `make docker-test` 退出码 0, 新断言在 stage1/stage2 两遍均通过, 诊断锁定 43/43 两遍; 纯测试提交, 行为中立.
- 偏差说明: 原拟的一条用例被删 - "goto 跳入语句表达式内部的标签" 被 clang 拒绝(jump enters a statement expression), 不写入真实编译器同样拒绝的形状. 另: 本次提交未同步重置 raw 基线(control.s 因新增断言变长), 重置在 R1.2 步内完成 - `.cache/` 已被 .gitignore 忽略, 不涉及提交内容.

### R1.2 绑定与 stray 检查移 sema (4bf8aec)

- 改了什么: sema.c 新增导出的 `analyze(Node *body)` - 函数体的一遍控制流下降, 由三个 static 组成(analyze_node / analyze_chain / analyze_children), 在 parse.c 的 `function()` 末尾替代原 `resolve_goto_labels()` 调用. 它承担: 循环与 switch 的 brk_label/cont_label 分配, ND_BREAK/ND_CONTINUE 填 unique_label 并降级为 ND_GOTO, ND_CASE 分配 label 并挂入所属 switch, ND_GOTO/ND_LABEL 的清单收集与 unique_label 分配, stray break/continue/case/default 四处检查, 以及 ND_WHILE -> ND_FOR 降级. `resolve_labels` 由导出改为 static 并改读 sema 侧的 gotos/labels. parse.c 删除 current_switch/brk_label/cont_label/gotos/labels 五个 static 与 resolve_goto_labels 壳, switch/for/while/do/case/default/break/continue/goto/label/`&&label` 十处构造点回到只建忠实形状(2516 -> 2427 行, error_tok 33 -> 29, 判定表 B 清空). chibicc.h: ND_CASE 新增 `bool is_default`(sema 挂链时需要区分 `default:` 与 `case <expr>:`, 今天这个信息只存在于 parse 的语法位置), 导出 `analyze` 取代 `resolve_labels`. add_type 删除 ND_WHILE/ND_BREAK/ND_CONTINUE 三个 case, ND_LABEL_VAL case 改为把节点收进 gotos. sema.c 1570 -> 1699 行. codegen.c 零改动.
- 为什么改: break/continue 的目标标签是一次名字绑定, case 归属哪个 switch 也是 - 按层 3 的定义(无类型标注, 无名字绑定)它们不属于语法层. 解析现场维护"当前循环/switch"要求 parse 持有五个可变 static 上下文, 与库的可重入性冲突; 换成函数体解析完后的一遍下降, 上下文变成该遍的局部状态, parse 侧的控制流 static 归零. 判定表 B 的四处 stray 检查随之迁走, 因为它们问的正是"有没有外层目标"这个绑定问题.
- 测试结果: 四闸门全绿. (1) 归一化 diff(`docker-snapshot-ndiff`)为空 - 形状面等价; (2) `docker-test` 退出码 0: 40 个测试可执行文件 stage1 + stage2 自举各一遍全过, driver.sh 126 项 passed, 诊断锁定 stage1/stage2 各 "43 cases byte-exact"; (3) `docker-test-thirdparty THIRDPARTY=tinycc` 退出码 0; (4) raw diff 非空(4 文件 / 414 hunk / 1932 行), 逐行核对**全部**为 `.L..N` 标签编号变化(1932/1932 行含 `.L..`, 无一例外), 符合 [重组] 步骤的预期字节变化口径. 典型形态: constexpr.s 的 "OK\n" 字符串全局由 `.L..47` 变 `.L..45` - 两个控制流标签原先在解析途中取号(排在字符串之前), 现在在函数末尾的 analyze 里取号(排在字符串之后). 本提交内已 `make docker-snapshot` 重置 raw 基线, 重置后复跑 `docker-snapshot-diff` 为空, 确认基线可复现且无不确定性. 另在宿主机本地核对: 四处 stray 诊断文案与插入符位置逐字节不变(stray break / stray continue / stray case / stray default 均锚关键字 token), `use of undeclared label` 不变, `&&lbl` 静态跳转表(test/control.c 的 `static void *p[]={&&v41,&&v42,&&v43}`)三项仍解析到正确的 `.L..3/.L..4/.L..5`.
- 偏差说明: 五项.
  1. **PLAN 文本的 analyze 并不存在** - PLAN 写"analyze 的语句下降是前序的", 但当前代码里没有 analyze, 本步创建它. 同时 R1.1 已记录标签分配触发点并入本步, 故 R1.2 的实际范围 = 分配 + 绑定 + 降级 + stray 检查一次落地.
  2. **case 挂链是后序的, 不是前序** - 原 parse 现场在 `node->lhs = stmt(...)` **之后**才把 case 挂进 case_next, 于是埋在体内层 case 先入链; analyze 若按 PLAN 的前序挂链, codegen 的比较链顺序会翻转, 归一化 diff 实测抓到 control.s 两处 4 行(`cmp $0/je` 与 `cmp $1/je` 互换). 改为"label 分配前序 + 挂链后序"后归一化 diff 为空. 这只在埋藏 case(外层 case 的语句里再出现 case)时有区别; 无重叠的 case 常量下两种顺序语义相同, 但本线要求形状等价, 故取后者.
  3. **ND_LABEL_VAL 的收集留在 add_type, 没有进 analyze** - `&&lbl` 可以出现在语句树之外: 块域 static 数组初始化器里的跳转表只有常量求值器会走到, analyze 的语句下降看不到它, 漏收会导致 `use of undeclared label` 误报. 因此 analyze 的 gotos 清单由两处供给(analyze 收 ND_GOTO, add_type 收 ND_LABEL_VAL); add_type 的 `node->ty` 早退守卫保证每个节点恰好收一次. 代价是 `&&lbl` 与同函数内 `goto` 的入链相对顺序可能与原 parse 顺序不同, 只在同一函数有多处未声明标签时影响首个报错, 归入第 4 项.
  4. **stray 检查时机后移到函数末尾, 多错误输入的首个报错可能改变**(经用户确认接受). 单错误输入完全不变(43 个锁定用例均为单错误, 全绿). 实测两类位移: `int main(){ break; int x=1; return x->y; }` 原报 "stray break"(锚 break), 现报 "invalid pointer dereference"(锚 `->`); `int main(){ int x = ({ break; }); }` 原报 "stray break", 现报 "statement expression returning void is not supported"(锚 `(`) - 后者是 add_type 的语句表达式检查在解析现场先跑所致. 反向不位移: 另一处错误在 stray 之前时两版一致, 且 "use of undeclared label" 由 resolve_labels 在下降之后触发, 两版都排在全部 stray 之后.
  5. **嵌套函数定义中的外层 goto 现在能编译通过**(经用户确认接受). `int main(){ goto out; int inner(void){ return 42; } out: return 1; }` 原报 "use of undeclared label" - parse 的 resolve_goto_labels 在内层函数解析完时就跑, 拿外层已积累的 goto 去匹配内层的 label 清单, 匹配不上即报错, 随后清空清单. analyze 按函数体调用, 内层只收内层的 goto, 外层的留到外层自己的下降里解析. 这是原实现的缺陷被顺带修掉, 不是新引入的行为; 归类为诊断消失.

### R2.1 成员访问两段式 (162cd2a)

- 改了什么: parse.c 的 `struct_ref` 从 30 行降到 4 行 - 只发一个未绑定的 ND_MEMBER(`lhs`=被访问表达式, `tok`=成员名 token, `arrow_tok`=`->` token 或 NULL), 不再 `add_type(操作数)`, 不再查成员表, 不再做四类检查; `get_struct_member` 整体迁到 sema.c 并导出(parse 的 `struct_designator` 仍在用它 - 初始化器指示符归 R2.4). sema.c 新增 `resolve_member(Node*)`, 由 add_type 的 ND_MEMBER case 在 `node->member==NULL` 时调用: 四类检查(invalid pointer dereference / dereferencing a void pointer / not a struct nor a union(箭头锚) / not a struct nor a union(点号锚操作数 token) / no such member(锚成员名 token)), 匿名成员展平(路径上每个匿名成员自成一个 ND_MEMBER 挂在操作数与具名成员之间), 以及 `->` 的 DEREF 补插(锚成员名 token, 归最内层链接). 结果节点字段原样搬回 `node`(父指针指向它). chibicc.h: `bool is_arrow` 改为 `Token *arrow_tok`(一个字段同时承载"是不是箭头"与该箭头的锚点, 二者本不可分), 新增 `get_struct_member` 声明. test/struct.c 补 17 条匿名成员展平断言(`.`/`->`/三层匿名/匿名 union/结构赋值/取址/`+=`/`++`/位域/`sizeof`/带初值声明/指针间接改值), 期望值先用宿主机 clang 算出. parse.c 2427 -> 2378 行, error_tok 29 -> 24(判定表 C 的 struct_ref 5 处清空, 仅剩初始化器 6 处待 R2.4); sema.c 1699 -> 1770 行; codegen.c 零改动.
- 为什么改: 成员名指向哪个 Member 是一次名字查找, `x->y` 的类型合法性与 `(*x).y` 的补插也是语义 - 按层 3 的定义不属于语法层. 拆成两段后 parse 只记录"谁.谁", sema 在标注遍里查表, 判定表 C 里"建树必需"的成员访问一项就此消失(该项在拆分线的理由是"成员链含匿名成员必须现解析", 现在它和绑定/降级一起在遍历现场完成).
- 测试结果: **四项全绿且 raw 逐字节为空**(本步无需重置 raw 基线, 口径上等同常规步骤). 判据做法: 因为本提交同时给 test/struct.c 加了断言, 直接用旧基线比会把"测试源变了"和"编译器变了"混在一起 - 于是先用 **HEAD 的编译器 + 工作区的新测试源**在容器里生成 41 个文件的快照作为对照基线, 再用新编译器生成快照跑 `docker-snapshot-diff`(raw)与 `docker-snapshot-ndiff`(归一化), **两者均为空**. 另在宿主机做独立交叉验证: HEAD 构建的二进制与工作区构建的二进制对 39 个 `test/*.c`(atomic/tls 需 Linux 系统头, 本机跳过)与匿名成员样例 `/tmp/anon2.c` 输出的 .s 逐字节相同. `docker-test` 退出码 0(40 个测试可执行文件 stage1 + stage2 自举各一遍, 含新增 17 条断言; driver.sh passed; 诊断锁定 stage1/stage2 各 "43 cases byte-exact"), `docker-test-thirdparty THIRDPARTY=tinycc` 退出码 0. 最后用新编译器 `make docker-snapshot` 重置 raw 基线并复跑 `docker-snapshot-diff` 为空, 确认基线可复现.
- 环境注记: 本会话 `make docker-image` 无法联网(docker build 拉 `ubuntu:22.04` 清单时 auth.docker.io 连接被重置), 但本地已有 `chibicc:amd64` 镜像. 用 `make -o docker-image <target>` 把该前置目标标记为不重做, 其余目标照常, 四个 docker 闸门全部照跑. 网络恢复后无需任何改动.
- 偏差说明: 三项, 前两项同源于"被丢弃的表达式不再受成员检查".
  1. **未选中的 `_Generic` 分支**: `struct S{int a;}; int main(){ return _Generic(1, int: 0, double: ((struct S*)0)->b); }` 原报 "no such member"(泛型选择边解析边在 `ctrl` 上定类型, 落选分支的节点被丢弃, 从未标注), 现在静默通过. 真实编译器(clang/gcc)会对落选分支做类型检查, 所以这是向"更 lax"偏离 - R2.2 把 `_Generic` 改成忠实节点后所有 association 表达式都进遍历, 该检查自然恢复, 偏差只存在于 R2.1~R2.2 之间.
  2. **多余初始化元素**: `int x[1] = {0, ((struct S*)0)->b};` 的第二元素被 `skip_excess_element` 跳过(chibicc 对"initializer 元素过多"本就无诊断), 其成员检查现在不再触发. 与 1. 同源: 检查跟着标注走, 不标注就不检查. 归入 R1.2 偏差 4 同一类(诊断时机/诊断存在性随遍历现场移动), 单错误合法输入完全不变(43 个锁定用例全绿).
  3. 实现形态调整(非行为): `is_arrow` 换成 `arrow_tok` - 计划文本写"ND_MEMBER 只带成员名 token 与 is_arrow(字段已有)", 但四类检查里三类必须锚在 `->` token 上(纪律 4 要求锚点原样传递), 布尔字段不够用, 故直接存 token; "是否箭头"由它是否为 NULL 给出.

### R2.2 泛型选择与 builtin 折叠忠实式 (d281858)

- 改了什么: (1) `_Generic` 不再在解析现场做选择. parse.c 的 `generic_selection` 只建树 - `ND_GENERIC`(tok=`(`, 即原诊断锚点) 的 `cond` 存 controlling 表达式, `args` 存一条 `ND_GENERIC_ASSOC` 链, 每个 assoc 的 `ty_op` 存 type-name(`default:` 项为 NULL, 这就是它的标识), `lhs` 存结果表达式; 原 `add_type(ctrl)` 与 `is_compatible` 循环全部删掉. 选择移入 sema: add_type 顶部(前序递归**之前**)判到 ND_GENERIC 就调新函数 `select_generic(node)` - 标注 cond, 按 func/array 退化后逐个 assoc 比 `is_compatible`, 保持旧循环的胜出规则(具名匹配覆盖 default, 多个匹配后者胜), 无匹配则 `error_tok(node->tok, "controlling expression type not compatible with any generic association type")`, 落选分支各自 `add_type` 一遍(只要检查, 不进树), 最后 `*node = *sel` 并保回 `node->next`. (2) `__builtin_types_compatible_p`/`__builtin_reg_class` 由"解析现算 is_compatible/is_integer 折成 ND_NUM"改为发 `ND_TYPES_COMPATIBLE`(ty_op/ty_op2)/`ND_REG_CLASS`(ty_op) 忠实节点, sema 的 add_type 折叠成 ND_NUM 后再 `add_type(node)` 补类型. chibicc.h: 新增 4 个 NodeKind(ND_GENERIC/ND_GENERIC_ASSOC/ND_TYPES_COMPATIBLE/ND_REG_CLASS)与 `Type *ty_op2`. test/generic.c +11 断言(default 兜底与覆盖次序, 用作数组维度/枚举值/case 标签/sizeof 操作数, `&&label` 经 default 选中后仍能跳对, 嵌套 _Generic, 数组退化), test/builtin.c +11 断言(reg_class 六类取值 + 折果参与常量运算与数组维度). parse.c 2378 -> 2373 行, error_tok 24 -> 23(判定表 D 第 3 处清空); sema.c 1770 -> 1849 行; codegen.c 零改动.
- 为什么改: 选中哪个分支取决于 controlling 表达式的**类型**, 是类型判定不是语法选择 - 拆分线判定表 D 给它的理由是"is_compatible 的结果决定选中哪个分支, 是语法选择而非事后诊断", 本线把"分支"本身变成树里的忠实记录后, 这个理由消失. 两个 builtin 折叠是拆分线 1.9 明确留下的返工点("暂留 parse"), 与 `__builtin_reg_class` 一起在此清掉. 落选分支自此也受类型检查(与 clang/gcc 一致).
- 测试结果: 四项全绿, 且 **raw 逐字节为空**(无需重置基线). 基线做法同 R2.1: 用 HEAD(R2.1) 的编译器 + 本提交的新测试源生成 41 文件快照作为对照基线, 新编译器下 `docker-snapshot-diff` 与 `docker-snapshot-ndiff` 均为空 - 即"选择逻辑搬家"没有改变任何一个文件的产物字节. 另在宿主机交叉验证: R2.1 与 R2.2 二进制对 39 个 `test/*.c` + 四个手写压力文件(匿名成员/泛型全表达式位置/常量上下文/`&&label` 与 reg_class)输出逐字节相同. `docker-test` 退出码 0(新增 22 条断言 stage1+stage2 两遍全过, driver.sh passed, 诊断锁定 stage1/stage2 各 43/43), `docker-test-thirdparty THIRDPARTY=tinycc` 退出码 0.
- 偏差说明: 三项.
  1. **落选分支现在受完整类型检查**(行为增强, 与 clang 对齐): `struct S{int a;}; int main(){ return _Generic(1, int: 0, double: ((struct S*)0)->b); }` 原静默通过(拆分线起 `_Generic` 的落选节点从未标注), 现报 "no such member" 并退出 1 - 宿主机 clang 同样报错, 文案锚点与 R2.1 时选中分支的检查完全一致. 这同时关闭 R2.1 偏差 1.
  2. **实现形态调整(非行为)**: 计划文本写"新增 NodeKind 或复用载体", 实施为新增 4 个 kind + 一个 `ty_op2` 字段; `default:` 用"assoc 无 type-name"表示, 未新增标志字段. 另外选择动作必须放在 add_type 的**入口**(前序递归之前), 放在 switch 里会让选中分支先作为 `args` 的子孙被独立标注, 于是 `&&label` 把自己的**原节点**登记进 gotos 清单, 而树上留下的是改写后的节点, 二者的 `unique_label` 脱钩 - 实测 `static void *gt[] = { _Generic(1, default: &&l2) };` 的跳转表项退化为 `.quad (null)+0`(R2.1 基线为 `.quad .L..6+0`). 改在入口做选择并只对落选分支做检查, 选中分支由改写后的节点自己走标注, 登记身份与树一致, 该用例逐字节复原. 落选分支的 `&&label` 仍会登记进 gotos, 因此未选中的 `&&未定义标签` 现在会报 "use of undeclared label"(clang 亦如此), 属 1. 的同一类增强.
  3. **测试期望值的一处自我纠正**: `__builtin_reg_class(int) + __builtin_reg_class(double)` 先按 2 写入断言, docker-test 报 "2 expected but got 1" - 折叠规则(整型/指针=0, 浮点=1)下 0+1=1 才是对的, 已改正. 记录在此说明该断言的取值来源是规则本身, `reg_class` 非标准 builtin, 宿主机 clang 无法旁证.

### R2.3 常量求值出 parse(第一批) (332f31e)

- 改了什么: 四类"解析现场算常量"的决策点改为**只记表达式**, 求值/判定统一进 sema 的新函数 `resolve_type(Type*)`.
  1. **数组维度与 VLA 分类**: `array_dimensions` 改发 `array_of_dim(ty, expr)`(type.c 新增: TY_ARRAY, array_len=-1, `Type.dim_len` 挂长度表达式); chibicc.h 的 `array_dimension_type` 声明与 sema 侧定义**删除**. resolve_type 递归走 declarator 建出的类型链(base / return_ty / params), 内层先于外层, 判 `base->kind==TY_VLA || !is_const_expr(dim)` 则就地改成 TY_VLA(size/align=8), 否则 `array_len=(int)eval(dim)` 重算 size 与 align - 与旧 `array_dimension_type` 同一判据同一次序. **触发点**: `declarator()` 与 `abstract_declarator()` 的非括号返回(类型完整之后、写 name 之前), `struct_members()` 对 basety 的一次(匿名成员靠 kind 识别, 该路径后面没有 declarator 出口), `layout_struct/layout_union` 入口, `new_var()` 读 `ty->align` 之前. 全部幂等(记录取出即清空).
  2. **typeof(expr)**: 新 TypeKind `TY_TYPEOF` 与 `Type.typeof_expr`; `typeof_specifier` 改为 `typeof_placeholder()` + 记表达式, **删掉该处 add_type**(计划文本所说"parse 侧对应的 add_type 消费点消失"之一). resolve_type 里 `add_type(操作数)` 后把操作数类型**回填进这条记录**(`*ty = *expr->ty`, 并保住 name/name_pos), 因为 declarator 已经把指针/数组类型包在这块记录之上.
  3. **`_Alignas` 与 `aligned`**: `VarAttr.align_expr` 记 `_Alignas(<expr>)`(类型形式仍由 parse 读 `typename()->align`, 两种形式后写者生效, 与旧一致), 新 `attr_align(VarAttr*)` 在三个消费点(块域声明/文件域声明/结构成员)取值并**把值缓存回 attr->align**(一条声明声明多个 declarator 要重复问, 不缓存则第二个拿不到). `__attribute__((aligned(<expr>)))` 改记 `Type.align_expr`, 由 resolve_type 在 layout 之前回填.
  4. **case 的 begin/end 与空区间**: ND_CASE 新增 `begin_expr`/`end_expr`/`colon_tok`; parse 的 case 分支只做 `conditional()`; add_type 新增 `case ND_CASE:` 求值回填 begin/end(仍按 int 截断, 与旧 `int begin = const_expr(...)` 一致), `end<begin` 报 "empty case range specified" 锚 `:`.
  5. **VLA 带初值**: parse 的 `ty->kind==TY_VLA` 分支不再报错也不再提前 continue(有 `=` 时落到通用路径, 初值照常解析, `Initializer.eq_tok` 记下 `=`), 报错移到 add_type 的 ND_DECL VLA 降级分支, 锚点即 eq_tok.
  6. `array_of` 补一条规则: 元素尺寸未知(负)而长度也为负时结果尺寸保持 -1, 不再"负负得正" - 待决维度会让这种组合真实出现.
  7. 测试: test/alignof.c +3(`_Alignas(1<<4)` 多 declarator、`aligned(8*8)`、成员 `_Alignas(4<<3)` 的 struct 对齐), test/control.c +6(case 值为枚举/[GNU]区间/`sizeof`/负数), test/typeof.c +7(typeof 操作数为浮点/取址/数组类型名/结构成员/嵌套 typeof/指针类型流变), 期望值先用宿主机 clang 算出.
  规模: parse.c 2373 -> 2398 行(记录表达式与说明注释, 但求值调用清零), error_tok 23 -> **21**(判定表 D 的 1/2 两处随 case 值与 VLA 初值迁走, **D 表就此全清**), parse.c 剩余 `const_expr` 5 处(初始化设计符 4 处归 R2.4, 位域宽度 1 处归 R2.5); sema.c 1849 -> 1958; type.c 132 -> 157; chibicc.h 688 -> 722; codegen.c 零改动.
- 为什么改: 维度是不是常量, `aligned` 的参数是多少, case 标签代表什么值, `typeof(expr)` 的操作数是什么类型 - 全是常量求值与类型标注, 按层 3 的定义不属于语法层. 拆分线把这几处留在 parse 的理由都是"parse 必须现在就拿到结果才能继续建树"; 本步用"待补全的类型/属性记录"把它们解开: 声明符层照常建它能建的形状, 只把值挖空交给 sema. 至此 parse.c 的常量求值只剩初始化器与位域两处, 也正是 R2.4/R2.5 的对象.
- 测试结果: **四闸门全绿, raw 逐字节为空**. 做法同 R2.1/R2.2: 用 HEAD(R2.2) 编译器 + 本提交的新测试源生成 41 文件对照基线, 新编译器下 `docker-snapshot-diff`(raw) 与 `docker-snapshot-ndiff` **均为空** - 即数组维度/typeof/对齐/case 值的求权搬家没有改变任何产物字节. `docker-test` 退出码 0(含新增 16 条断言, stage1 + stage2 自举各一遍, 诊断锁定 stage1/stage2 各 "43 cases byte-exact"; d01_vla_initialized 与 d02_empty_case_range 两个已锁用例随迁移后文案与锚点逐字节不变), `docker-test-thirdparty THIRDPARTY=tinycc` 退出码 0(该轮在测试源定稿前跑, 编译器二进制与定稿同一份). 另在宿主机交叉验证: R2.2 与 R2.3 二进制对 39 个 `test/*.c`(atomic/tls 需 Linux 系统头本机跳过)与手写的 R2.3 压力文件(多维 VLA/`(*pv)[n]`/`typeof(VLA)`/`_Alignas(expr)`/`aligned(expr)`/枚举与区间 case 标签)输出逐字节相同; 单独核对 `case zzz:`(undefined variable)、`case n:`(not a compile-time constant)、`case 2 ... 1:`(empty case range)三处诊断文案与锚点不变.
- 偏差说明: 四项.
  1. **`typeof(vla)` 不再与源变量共用类型对象**: 旧实现里 typeof 的操作数类型就是那个 VLA 的 Type, 两个声明共用一条 `vla_size` 槽位; 现在回填出的类型是独立记录, 每个声明各有自己的 `vla_size` 局部. 实测 `int f(int n){ int vla[n]; typeof(vla) c; sizeof(vla); sizeof(c); }`: 旧版两处都读 `-16(%rbp)`, 新版读 `-16` 与 `-32` - **值相同**(同一条 `vla_len` 乘同一基尺寸), 仅栈槽数量随声明个数增长. 归为产物形状变化, 测试集无此写法故 raw/归一化 diff 均空. 计划文本要求 typeof 改记表达式并由 sema 回填, 记录本身即独立类型, 此为该设计的直接后果.
  2. **诊断时机后移(多错误输入首个报错可能变化)**, 单错误输入完全不变(43 用例全绿). case 标签的值现在在语句整体标注时求值, 即晚于其**语句体**的现场标注: `switch(n){ case qq: int m; undeclared; }` 这类同时含两处错误的输入, 首个报错可能从体侧移到标签侧(实测 `case zzz: return 1;` 单错仍先报标签的 undefined variable, 次序不变). `_Alignas(expr)` 的求值从 declspec 内移到声明符完成处, 若表达式本身报错(如 `case _Alignas` 里引用未声明名)锚点与文案不变, 只晚于同一 declspec 后续关键字. 与 R1.2 偏差 4 同类, 经该步一并确认.
  3. **实现形态调整(非行为)**: 计划文本写"由 sema 求值/判定后回填", 未指明触发时机; 按本线纪律 6(R2.1-R2.5 只做两段式, R2.6 才改遍历), 求值动作放在 resolve_type 里, 而 resolve_type 的**调用点**仍在解析现场(declarator 出口, 结构成员, layout 入口, new_var). 其中 `attr_align(VarAttr*)` 由 parse 直接调用并取回一个 int - 这是本步唯一"parse 仍向 sema 要一个值"的点, 因为 `_Alignas` 的值在一条声明里被多个 declarator 消费, 挂到 ND_DECL 上需要再引一个节点字段而收益为零; R2.6 的 resolve 遍历将连同 declarator 出口的 resolve_type 调用一起拆除. 另: 匿名成员那条路径没有 declarator 出口, 故 `struct_members` 里对 basety 多一次 resolve_type 调用.
  4. **未纳入本步的两项按计划归属确认无误**: 位域宽度 `mem->bit_width = const_expr(...)` 因布局时序留在 parse(归 R2.5); 初始化设计符的 4 处 `const_expr`(数组下标与区间)留在 parse(归 R2.4). `typeof` 的类型形式(`typeof(int)`)本就由 parse 走 typename, 不在求值迁移范围内.

### R2.4 初始化器忠实化 (47e0040)

- 改了什么: 初始化器解析与定位彻底两段式化 - parse 只建**忠实 brace 记录**, 全部语义(定位, 越界检查, brace elision, 灵活数组长度, 字符串展开, 结构复制判定)移入 sema 的 resolver.
  1. **chibicc.h**: `Initializer` 重定义为忠实记录(kind = INIT_LIST/INIT_EXPR/INIT_STR; LIST 携带 `InitItem` 元素序列, 每元素带 `comma_tok` + `InitDesig` designator 链 + 值记录; `InitDesig` 记 `[`/`.` token, 未求值的 begin/end 表达式节点, `...` 后的 after_begin 锚点与 `]` 锚点, 成员名 token). 旧 typed 树结构与 `InitDesg` 从 chibicc.h 撤出.
  2. **parse.c**: 删除 new_initializer/string_initializer/array_designator/struct_designator/designation/count_array_init_elements/array_initializer1/2/struct_initializer1/2/union_initializer/initializer2/skip_excess_element/copy_struct_type/is_end 约 420 行; 新增 `designators`(designator 链的纯语法解析, "expected a field designator" 保留 = 判定表 A)与 `init_record`(brace-list | 字符串 token | assign 表达式三形状), `initializer()` 变为无类型参数的记录解析入口. declaration/复合字面量两个调用点只挂记录; 块域 static 与文件域仍调 sema 的 `gvar_initializer`.
  3. **sema.c**: 新增私有 `ResolvedInit` typed 树(字段同旧 Initializer)与 resolver 全家 - `resolve_initializer`(入口, 含灵活结构成员补全 + copy_struct_type 迁入), `resolve_value/resolve_desig/resolve_item/resolve_array1/resolve_array_cont/resolve_struct1/resolve_struct_cont/resolve_struct_first/resolve_union/resolve_string/materialize_str/count_flex/count_flex_list/new_resolved_init/eval_array_desig/find_init_member`. 三类越界与成员诊断("array designator index exceeds array bounds" x2 / "range is empty" / "struct has no such member" / "array index in non-array initializer" / "field name not in struct or union initializer")随迁, 锚点经记录 token 原样传递(begin 越界锚 begin 表达式后 token, end 越界与空区间锚 `]`, 成员锚名字 token) - **判定表 C 的初始化器 6 处清空, C 表全清**. 触发点(纪律 6 两段式): ND_DECL 与 ND_COMPOUND_LITERAL 的 add_type case, 以及 gvar_initializer; ND_DECL 降级顺序改为 resolve -> void 检查 -> incomplete 检查 -> lvar_init_comma(灵活数组/灵活成员的类型补全发生在 resolve). create_lvar_init/write_gvar_data 等既有降级改吃 ResolvedInit, 逻辑零改动.
  4. **测试**: test/initializer.c 末尾新增 16 条断言(designated 成员后跟位置兄弟, 深层匿名链 `.deep`, 联合首成员字符串展开, typedef 灵活数组, 区间 designator 带 brace 值, 块域灵活成员), 期望值经宿主机 clang 验证; 全部为旧编译器可编译形状, 对照基线可由 R2.3 二进制生成.
  规模: parse.c 2398 -> 2069 行, error_tok 21 -> **15**(恰为判定表 A 全集, 终态目标提前达成); sema.c 1958 -> 2496; chibicc.h 722 -> 729; parse.c 的 eval 族调用只剩位域 1 处 const_expr(R2.5 对象), 初始化器常量求值清零(纪律 7 口径); codegen.c 零改动.
- 为什么改: 初始化器的 designator 求值/成员定位/越界判定是拆分线判定表 C 最大的一块(11 处中占 6 处), 也是 parse 侧常量求值的最后两处之一; "解析器必须先拿到 Member*/索引值才能建树"的旧理由被"忠实记录 + sema 铺开"解开 - 元素定位不再需要现算值, 记录本身就是形状. 同时消除 parse 对结构 size 的初始化器消费(灵活成员补全迁 sema), 为 R2.5 的布局后置铺路. R2.6 的 resolve 遍历只需改触发时机, 无需再动初始化器逻辑.
- 测试结果: **四闸门全绿, raw 逐字节为空**. 对照基线做法同 R2.1-R2.3: 用 R2.3(332f31e)编译器 + 本提交的新测试源在容器内生成 41 文件快照, 新编译器下 `docker-snapshot-diff`(raw)与 `docker-snapshot-ndiff` **均为空** - 定位/求值搬家没有改变任何产物字节. `docker-test` 退出码 0(含新增 16 条断言 stage1+stage2 两遍全过, 诊断锁定 stage1/stage2 各 "43 cases byte-exact", c01-c06 六个初始化器用例文案锚点逐字节不变). `docker-test-thirdparty THIRDPARTY=tinycc` 退出码 0. 宿主机交叉验证: R2.3 与 R2.4 二进制对 39 个 test/*.c(atomic/tls 需 Linux 系统头跳过)与两个手写压力文件(约 90 个初始化器形状: 多维/灵活/designated/区间/字符串/匿名成员/位域/复合字面量/重定位/typedef 灵活数组)输出逐字节相同; 18 个错误路径样例 stderr 逐字节对比(17 同 1 异, 异者见偏差 2). 另在容器内编译运行"仅新版接受"形状 12 条断言(见偏差 1), 运行值与 clang 语义一致.
- 偏差说明: 五项.
  1. **旧版两处初始化器 bug 顺带修复(行为更标准, 与 clang 一致)**: 旧 designation -> struct_initializer2 续填的首轮不跳逗号, 导致 (a) 匿名成员 designation 后跟兄弟元素(`struct {int x; struct {int p,q;};} g = {1,.p=2,3}`)与 (b) 数组内 designated 结构成员后跟任何兄弟元素(`struct S y[2] = {[0].a=1, [0].c=2}`)旧版报 "expected an expression", 新版按标准接受; (c) 同因, 顶层 `int x[] = 1;` 旧版 count pass 找不到 `}` 报 "expected ,", 新版接受为长度 1(clang 同). 三者均无测试覆盖, 运行值经容器实测与 clang 一致.
  2. **excess element 的多元素 brace 由报错改为静默丢弃**: `int x[2] = {1,2,{3,4}}` 旧版 skip_excess_element 在 brace 内逗号处报 "expected '}'", 新版整项丢弃(与旧版对普通 excess 的静默丢弃口径一致, clang 警告后丢弃). 同类: 数组 brace 列表内 `.name` 项/结构 brace 列表内 `[n]` 项旧版按子节点类型报 "expected an expression"/"expected ,", 新版统一按 designator 语义处理 - 目标非数组/非结构时报对应语义文案(锚点同 token), 目标是结构/数组时**接受**(如 `struct S x[2] = {.a=1}` 视为 x[0].a=1; clang 报错, 属更 lax 方向).
  3. **range designator 的值节点共享**: `[1...2] = <expr>` 旧版对区间每槽位重新解析(独立节点), 新版各槽位共享同一记录表达式节点. 纯表达式产物逐字节不变(41 文件快照含 `[2 ... 10]='a'` 等区间用例, raw diff 空); 含副作用值(elvis 临时变量/字符串匿名全局)时临时对象数量比旧版少(旧版区间每槽位各建一份), 值语义不变. 同类: struct-copy 判定路径旧版"解析-丢弃-再解析"会双份物化临时对象, 新版单份.
  4. **字符串物化与 copy 判定时点后移**: 初始化器内字符串记录的匿名全局物化与结构复制判定的 add_type 从解析现场移到 resolve(降级)时点. 单 declarator 声明内相对顺序不变(降级紧随该语句解析), 仅"同一声明内前一 declarator 的字符串初始化器 + 后一 declarator 的表达式初始化器含字符串字面量"(如 `char *p = "a", *q = f("b");`)会使匿名名编号相对旧版互换; 测试语料无此写法(raw diff 空). 与 R2.3 偏差 2 同类的时点位移.
  5. **多错误输入的首错优先级微移**: VLA 带初始化器且初始化器本身非法时(如 `int x[n] = {[0]=1}`), 旧版先报解析侧语法错("expected an expression"), 新版先报 "variable-sized object may not be initialized"(VLA 检查在 resolve 之前); `void x = <非法初始化器>` 同理由 resolve 侧先报. 单错误输入不变(d01 与 flex/void 锁定用例逐字节同, 43/43 全绿).

### R2.5 位域宽度记录与布局后置 (5f07540)

- 改了什么: 布局动作彻底离开 parse(`layout_struct/layout_union` 的直接调用消失, 转 sema 私有), 位域宽度改记表达式 - 拆分线 4.2a 的"解析现场直接铺布局"就此反悔.
  1. **宽度记录**: `Member` 加 `width_expr`(未求值的宽度表达式); parse 的 `mem->bit_width = const_expr(...)` 改为 `conditional()` 记录. sema 的 `eval_bitfield_widths` 在 layout 开头求值回填(结构/联合体共用), **纪律 7 就此达成: parse.c 的 eval/eval2/eval_double/const_expr/is_const_expr 调用计数 = 0**.
  2. **布局后置**: `Type` 加 `layout_pending`(成员表列完、尚未铺开时置位; 前向声明不带); `struct_decl/union_decl` 删除对 `layout_struct/layout_union` 的直接调用, 两函数转 sema 私有, 由 `resolve_type` 的 TY_STRUCT/TY_UNION case 驱动. 按纪律 6, 触发点仍是解析现场的 `resolve_type` 调用(parse.c 侧共 5 处): declarator 与 abstract_declarator 出口, struct_members 匿名成员, **struct_decl 与 union_decl 的收尾调用**; 另有 sema 侧 new_var 在登记对象时收尾. 最后两处是必需的: `struct T {...};` 这种不带 declarator 的裸定义(如 tcc 的 `struct sym_version`)在其余四处触发点上都排不到, 若不在此收尾, 类型会永远停在 struct_type() 的占位 size 0, 指针下标的缩放常量折成 0 并造成堆破坏 - 详见测试结果. 收尾调用必须在 `ty->kind` 落定之后: kind 还是 TY_STRUCT 时铺联合会把"取最大成员尺寸"铺成"成员尺寸累加". 嵌套布局顺序由定义收尾点保证(内层在自身右括号处先铺, 先于外层).
  3. **连带保留 - 数组尺寸回填**: pending 结构上建的具体数组(array_of 当时 base size 是占位 0)在 resolve_type 的 TY_ARRAY case 里按已铺好的 base 重算 size/align(`dim_len` 那条径本就在回填分支算). 定义收尾点生效后, 常规声明符路径不再产生这种数组; 仍会命中的是 `typeof(struct T{...})` 数组 - 数组包在尚未填充的 typeof 记录上, 尺寸只能在填充后重算. VLA/指针分支不动.
  4. **连带修复 - 上游既有崩溃**: 匿名零宽位域成员(`int : 0;`, name==NULL 且类型非聚合)会让按名查成员的 `mem->name->len` NULL 解引用 - `struct {int c:4; int:0; int d:2;} b; b.d` 与 `= {.d=1}` 两类输入在**新旧二进制上都静默崩**(cc1 子进程段错, 驱动 exit 1 无输出; 该模式自上游 f814033 起存在). 本线代码 `get_struct_member`(R2.1) 与 `find_init_member`(R2.4) 各加 `!mem->name` 跳过守卫, 崩溃输入转为正常编译(与 clang 一致).
  5. **测试**: test/bitfield.c +4(宽度为 `1+3`/`sizeof(int)*8-8`/枚举/带括号字面量的 sizeof), test/struct.c +4(内联定义结构的数组与嵌套结构数组 sizeof, 以及裸定义聚合经不完整指针下标访问的尺寸缩放 - 见"中途缺陷"), 期望值宿主机 clang 验证, 全部为 R2.4 编译器可编译形状.
  规模: parse.c 2069 -> 2080(记录注释与两处收尾调用净增), sema.c 2496 -> 2548, chibicc.h 729 -> 731; error_tok parse.c 维持 15(判定表 A); codegen.c 零改动.
- 为什么改: 位域宽度是常量求值、offset/size/align 是类型系统产物, 都不是语法形状 - 层 3 定义下 parse 一律不该碰. R2.4 清掉初始化器对结构 size 的消费后, 布局时点后移的最后一个障碍(拆分线 4.2a 列的"声明符 size<0 判定/初始化器/数组维度当场要用")只剩数组维度, 由本步的 resolve 回填接住. 布局动作本身已在 sema 内, R2.6 的 resolve 遍历把触发点从现场调用换成树上遍及时, 布局逻辑零改动.
- 测试结果: **四闸门全绿, raw 逐字节为空**. 对照基线 = R2.4(47e0040)编译器 + 本提交新测试源生成的 41 文件快照, `docker-snapshot-diff` 与 `docker-snapshot-ndiff` 均空; `docker-test` 退出码 0(新增断言两遍全过, 诊断锁定 stage1/stage2 各 43/43); `docker-test-thirdparty THIRDPARTY=tinycc` 退出码 0. 宿主机交叉验证: R2.4 vs R2.5 二进制对全部 test/*.c + 三个压力文件(R2.4 的两个 + R2.5 专项: 表达式宽度/零宽匿名位域/内联嵌套结构数组/前向声明后定义/typedef 数组/packed/aligned(16)+位域) + 两个 tcc 缺陷最小复现文件逐字节相同(压力文件含旧版崩溃输入, 见偏差 1); 13 个宽度/访问错误路径 stderr 逐字节对比(11 同, 2 项为崩溃转接受的偏差 1 类); 布局值与手算及容器内 clang 语义核对(sizeof(struct B)=8, Aligned=16, packed=5, 域值回读 1/2/3/0/-1/7/5/1). 决定性同路径对照: 同一容器同一目录内先后用两个编译器构建并测试 tcc, 两轮退出码均 0, 且 18 个 .o 与链接出的 tcc 二进制逐字节相同(obdiffs=0).
- 中途缺陷与捕获路径(留档, 说明第三道闸门不可省): 首版把布局只挂在 declarator/new_var 那几个"首见 pending"的触发点上, **不带 declarator 的裸定义**因此永远不铺开. tcc 的 `struct sym_version`(tccelf.c:29)正是这种形状 - 它的指针字段在 tcc.h 里于该 tag 尚不完整时写下, 之后只被下标消费; 于是下标缩放常量折成 0, tcc 一编译 examples/ex1.c 就 `free(): double free detected in tcache 2`(hello-exe/dlltest/abitest 连带失败, 两轮确定性复现). 定位链: 同路径 A/B 显示唯一差异目标是 tccelf.o(其余 .o 全同) → 同路径 `-S` diff 给出 5 处 `mov $24` 变 `mov $0`(tccelf.c:119/120/575/576/577) → 宿主机 20 行最小复现锁定 → 收尾触发改到定义完成点(必须在 `ty->kind` 落定之后, 否则联合按结构铺开, 尺寸从"最大成员"变成"成员累加"). **自研语料/docker-test/raw diff/压力文件全部没抓到**, 因为语料里每个聚合定义都带 declarator; 已在 test/struct.c 补专项回归断言(经验证非空转: 撤掉收尾触发点后该下标折 0, 修复后折 24).
- 偏差说明: 一项行为偏差, 另两项为形态与口径说明.
  1. **崩溃输入转正常编译**(条目 4 的 NULL 解引用): `int : 0;` 后接成员访问(`b.d`)或初始化器成员指定(`{.d=1}`)旧版段错静默退出, 新版正确布局并读取(4 位域零宽对齐语义经值核对). 属修复, 非行为回归(崩溃输入不可能有合法依赖).
  2. **实施形态调整(非行为)**: 计划文本写"布局调用点从解析现场移入 sema 遍历"; 按纪律 6(R2.1-R2.5 只做两段式, R2.6 才改遍历), 本步迁移的是**布局动作** - `layout_struct/layout_union` 的直接调用消失, 改为 `resolve_type` 的 TY_STRUCT/TY_UNION case(内含位宽求值), 而触发调用留在 struct_decl/union_decl 等解析现场. R2.6 的 resolve 遍历拆掉这些现场调用时, 布局与位宽逻辑零改动. 拆分线 4.2a 的"调用点仍在解析现场且直接铺布局"就此反悔.
  3. 拆分线 4.2a 记的两项时机偏差(常量诊断时机后移、布局时点移到首用点)在收尾点定到定义完成处之后**不再存在**: 位宽非常量诊断与布局计算时机逐字节回到 R2.4 口径(裸定义/后用/指针三类探针全同).

### R2.6 拆构造现场标注 + resolve 前序遍历 (516c4a0)

- 改了什么: 本步是 [重组] 步 - R2.1-R2.5 把语义动作从"解析现场算"改成"解析现场记录 + 现场调用 sema 求值", 触发点仍散落在 parse 的各构造点; R2.6 把这些**现场调用全部拆除**, sema 成为对整棵树的两趟独立遍历, parse.c 自此达到层 3 形态(无类型标注, 无名字绑定, 无降级).
  1. **parse.c 去语义化(终态)**: `declarator` 不再 `copy_type`(共享类型单例的 `name`/`name_pos` 直接写在 declspec 造出的那块记录上); `declaration()`/`global_variable()` 在读到名字后**立即**用 `add_declared_name(name)` 把名字推进作用域影子表(供文法分类 oracle 识别 `t t=1; t;` 里末尾的 `t` 是表达式), 并把该 token 存进 `decl->name_tok`(在解析初始化器**之前**捕获, 因为共享单例的 `name` 会被下一个 declarator 覆写); `function_def()` 进作用域 + 推参数名后解析函数体(镜像基线 begin_function), 同样存 `name_tok`; `enum_specifier()` 每读一个枚举常量名就 `add_declared_name`. parse.c 里 `add_type`/`resolve_type`/`gvar_initializer`/`attr_align`/`declare_function`/`new_lvar`/`new_gvar`/`push_scope` 的调用计数**全部归零**(eval 族自 R2.5 已为 0).
  2. **sema.c 两趟遍历**: 新增驱动 `Obj *sema(Node *toplevel)` - 按源码顺序走 parse 产出的顶层声明记录链(ND_TYPEDEF/ND_ENUM_CONST/ND_GVAR_DECL/ND_FUNCDEF), 函数记录先解析(让后续记录看见其名), 其 body 紧随其后标注与下降. 每函数内: **pass1 = resolve 前序遍历**(从树上重建作用域 - ND_BLOCK 的 is_scope_block / ND_STMT_EXPR / ND_FOR; 绑定 ND_IDENT 到 Obj; 求值枚举; 解析 pass 暂存的初始化器 `init_resolved`; 补全聚合布局与类型; 物化 ND_STRING 为匿名全局, 保持源码顺序), **pass2 = add_type/type_chain/analyze 标注与降级**(pass2 新建的临时变量经 `splice_locals` 前插进 locals 链). `analyze_function` 用 get_locals/set_locals 隔离每函数的 locals 累加.
  3. **pass1 暂存与回填**: 初始化器的 `resolve_initializer` 从 pass2 移进 pass1 的声明记录处(数组按初始化器定长、灵活成员必须在后续引用之前补全), 结果存进 `Node.init_resolved`(不透明 `void*`, ResolvedInit 是 sema 私有类型); pass2 的 ND_DECL/ND_COMPOUND_LITERAL/serialize_gvar 取回该结果并清空两个字段. ND_STRING 的物化也在 pass1(否则匿名全局的数据段顺序会随 pass2 降级时机漂移).
  4. **pass2 建节点必须自带类型**(没有第二趟补标注): ND_SIZEOF/ND_ALIGNOF 折叠出的 `folded` 节点在拷回字段前 `add_type(folded)`; ND_DECL 的 VLA 降级与初始化器降级两处 `decl_splice`(经 type_chain 链入, 背着标注下降)各自 `add_type(decl_splice)`.
  5. **chibicc.h**: `Node` 新增 `Token *name_tok`(共享类型单例的 name 会被后续 declarator 覆写, 故名字 token 必须在 parse 现场即捕获)与 `void *init_resolved`(sema 私有载荷, 对 parse 不透明); `add_declared_name(Token*)` 导出. main.c 一行: `Obj *prog = sema(parse(tok));`.
  6. **snapshot-normalize.awk 扩两类**(吸收 pass2 临时变量前插造成的栈帧重排): 类 4 = 每函数首个 `sub $N, %rsp` 归一为 `sub $FRAME, %rsp`(FRAME 交替保证幂等); 类 5 = memzero 的 `addq $-N, -M(%rbp)` 负立即数按每函数首现顺序映射为序号. 类 1-3(.loc 折叠 / .L 标签重编号 / 负 rbp 偏移序列化)不变.
  7. **测试**: test/typedef.c +2(`MyInt MyInt` 形参自增、块内同名 typedef 变量 - 验证 oracle 影子表), test/enum.c +4(文件域裸 `enum{...};` 后接函数/枚举记录、`enum{C1}; enum{C2=C1+1};` 跨记录引用、typedef 枚举 sizeof、块内枚举 + 后接 extern 声明 - 验证 resolve_enum_records 的记录链 kind 守卫), 期望值宿主机 clang 验证.
  规模: parse.c 2080 -> **2066**(error_tok 维持 15 = 判定表 A 全集; eval 族 0; 上述语义调用 0), sema.c 2548 -> **3153**, chibicc.h 731 -> **715**(Node 字段净增但撤出若干 parse 侧声明), main.c 1 行; **codegen.c 与 type.c 零改动**(git diff --stat 不列).
- 为什么改: 层 3(忠实语法 AST)的定义是"不做降级, 不查名字, 不算类型"; R2.1-R2.5 已把每一类语义动作改成"记录 + sema 求值", 但求值的**触发**仍由 parse 在构造现场调用 - parse 依旧主导语义时序, 不是干净的阶段边界. R2.6 把触发权收归 sema 自己的树遍历: parse 只产出忠实记录链, sema 独立走两趟. 这是路线图阶段 4("sema 独立 pass")的收口, 也是 PLAN 本线"解除字节冻结造成的忠实层偏差"的终点 - 此后 parse.c 的表达式层真正"无类型标注, 无名字绑定".
- 测试结果: 本步为 [重组], 闸门 = 行为三闸门 + **归一化** diff 为空 + 同提交重置 raw 基线.
  - `make docker-test`(含 stage1 + stage2 自举 + driver.sh + 诊断锁定)**退出码 0**, 末行 "diagnostic lock: 43 cases byte-exact"(新增 typedef/enum 断言两遍全过).
  - `make docker-test-thirdparty THIRDPARTY=tinycc` **退出码 0**: tests2 全套(含 109_float_struct_calling/110_average/111_conversion 与 "Auto Bound-Test2 OK")通过, tcc 自身编译(2363 ms)成功; 该闸门正是抓到偏差 2 的文件域裸 enum 挂死的地方.
  - 诊断锁定: 43/43 单错误用例 stderr 逐字节不变(e08 的锚点随 name_tok 迁移仍逐字节同).
  - 归一化 diff: 对照基线做法同 R2.1-R2.5 - 用 HEAD(R2.5, 5f07540)编译器 + **本提交的新测试源**在容器内生成 41 文件快照(否则新增的 fparam/file_enum_fn 等断言会被误判为差异), 新编译器下 `make docker-snapshot-ndiff` **退出码 0, "snapshot ndiff: empty"**(扩展归一化器类 1-5 吸收全部预期字节变化). 宿主机 A/B 旁证(带 SNAPSHOT_DATEFLAGS 吃掉 __TIME__/__DATE__ 运行伪影): 参考编译器(/tmp/cbase26, 自 5f07540 纯净重建)vs 新编译器对同一份新测试源, 41/41 全部 normalized MATCH.
  - raw 基线重置: `make docker-snapshot` 重写 .cache/snapshot 为本步产物(退出码 0), `make docker-snapshot-diff` 复跑 **退出码 0, "snapshot diff: empty"**(raw 相对 R2.5 的预期字节变化 = 栈帧大小立即数 / memzero 负立即数 / .L 标签重编号 / .loc 行号 / 匿名全局数据顺序, 均由归一化器类 1-5 吸收, 故 ndiff 空而 raw 非空 - 这正是 [重组] 步的预期口径).
  - 新测试断言: typedef.c 的 6/2 与 enum.c 的 4/1/4/9 经宿主机 clang 验证, 且参考 vs 新编译器归一化 match.
- 偏差说明: 八项.
  1. **R2.8/R3.3/R2.10 的实质被本步吸收**(计划口径调整, 非行为): 计划把"记录链化与删除中间节点"(R3.3)、"scope_decls 删除"(R2.8)、"隐藏变量在 resolve 中创建"(R2.10)列为后续步; 实施 R2.6 的两趟遍历时, 这些是 pass1/pass2 结构的自然组成 - 记录链由 parse 产出、sema 按链遍历即隐含"链化", 作用域表从树上重建即不再需要 scope_decls, ND_STRING/临时变量在 pass1/pass2 创建即"隐藏变量在 resolve 中创建". 后续对应步若只剩空壳则在执行时标注"已由 R2.6 吸收".
  2. **resolve_enum_records 无限循环缺陷 + 捕获路径**(留档, 同 R2.5 教训): pass1 求值枚举记录链的外层循环初版未守卫 `recs->kind == ND_ENUM_CONST` - glibc 的 ctype.h/unistd.h 有文件域裸 `enum {...};`, 其后紧跟 ND_FUNCDEF/ND_GVAR_DECL(ty_op 非 NULL), 循环永不前进 → atomic.c/tls.c/stage2 在 docker-test 里**确定性挂死**. 定位链: debug 容器二分 + 宿主机 `sample <pid>` 抓挂死栈(lldb 在部分用例上自身挂死). 修复: `while (recs && recs->kind == ND_ENUM_CONST)`. **41 文件语料/docker-snapshot/宿主机 A/B 全没抓到**(语料无文件域裸 enum 后接非枚举记录), 是 docker-test 的自举 + 系统头路径抓到的 - 与 R2.5 的 tcc 闸门同一教训: 第三道闸门(真实项目/自举)不可省. 已在 test/enum.c 补该形状的回归断言.
  3. **共享类型单例的 name 覆写类 + 现场即捕获**(实现约束): declspec 造出的 `ty_int` 等是共享单例, `Type.name` 是瞬态(被下一个 declarator 覆写); 任何延迟到 sema 才读名字的路径都必须用 parse 现场捕获的 `Node.name_tok`, 不能读 `ty_op->name`. 初版未捕获导致 21 文件报 "undefined variable"(gvar 记录 `int g` 推到 sema 时 name 已是后写的 `z`). 曾在 declarator 里 `copy_type` 试图修, 但聚合类型一旦复制就破坏布局语义(is_flexible 由成员转换 `last->ty=array_of(base,0)` 派生一次, 复制后再布局丢失它)→ initializer.c 段错; 最终方案 = name_tok 现场捕获, declarator 不复制.
  4. **add_declared_name 增强 oracle**(行为不变, 文法分类必需): typedef 名不再被"已声明变量"遮蔽后, `t t=1; t;` 的末尾 `t;` 会被误判(报 "statement expression returning void"); `add_declared_name` 推进影子 VarScope(var/type_def 均 NULL)让 oracle 仍把已声明名识别为表达式. function_def 进作用域 + 推参数名镜像基线 begin_function. 纯文法分类反馈, 不绑定名字、不算类型.
  5. **pass1 类型补全 / 字符串物化 / 构造即标注的纪律**(实现约束): 没有第二趟补标注, 故 (a) 初始化器定长/灵活成员补全必须在 pass1(否则 pass1 绑定 ND_IDENT 时 var->ty 尚不完整, `int x[]={1,2,3,4}` 的 sizeof 折成 -4); (b) ND_STRING 物化在 pass1(否则匿名全局数据段顺序随 pass2 漂移, control/typeof 出现 762/106 行 diff); (c) pass2 新建节点(folded/decl_splice)必须自带 add_type.
  6. **locals 顺序变化 → 归一化器类 4/5 + raw 基线重置**(预期字节变化): pass2 临时变量前插(splice_locals)vs 基线交织创建, 造成对齐填充与栈帧大小的字节变化(5 文件 4 行); 设计决策 = 不为字节冻结而扭曲两趟结构, 改由归一化器吸收(类 4 帧大小 / 类 5 memzero 负立即数), 并在本提交重置 raw 基线. ndiff 空证明结构等价.
  7. **诊断时机后移**(多错误输入首错优先级可能微移, 单错误不变): 名字解析/类型检查从构造现场移到 pass1/pass2 遍历, 单错误输入 43/43 逐字节同(e08 锚点改用 name_tok 仍同); 多错误输入的首错优先级可能随遍历顺序变化, 与 R1.2 偏差 4 / R2.3 偏差 2 / R2.4 偏差 5 同类.
  8. **复合赋值 / 前缀自增自降的整结构复制改写**(潜在缺陷规避): pass2 降级 `op=` 与前缀 `++/--` 时对整个 Node 做结构复制再改写, 规避了"语句表达式 body 在改写中丢失"的潜在缺陷(若只改字段, ND_STMT_EXPR 的 body 链会脱钩); 纯表达式产物逐字节不变(归一化 match).

### R2.7 typedef/tag 影子作用域 (59b6d2a)

- 改了什么: 作用域表一分为二, 拆分线 4.2b 的"共享表"解体.
  1. **parse.c 自持一个只装文法分类信息的私有作用域栈**: 新增 `ParseScope`(`names` + `tags` 两张 HashMap, C 的两个块作用域)与 `NameEntry`(只有 `Type *type_def` 一个字段). `find_typedef`/`add_typedef`/`add_declared_name`/`find_tag`/`find_current_tag`/`push_tag_scope`/`enter_scope`/`leave_scope` 八个函数连同 `scope` static 全部由 sema.c 迁入 parse.c 并收为 static, chibicc.h 对应的八行声明删除. 变量名遮蔽 typedef 的机制不变: 每个 declarator 声明的名字(变量/函数/形参/枚举常量)仍推一条 `type_def == NULL` 的空条目(R2.6 偏差 4 的 oracle 增强).
  2. **sema.c 的表只剩名字解析产物**: `Scope` 去掉 `tags`, `VarScope` 去掉 `type_def`, `enter_scope`/`leave_scope`/`find_var`/`push_scope` 收为 static. 它的栈完全由 resolve 遍历驱动(ND_BLOCK 的 is_scope_block / ND_STMT_EXPR / ND_FOR / resolve_function 的形参层), 与 parse 的栈不再有共享的可变状态.
  3. **测试**: test/typedef.c +3(for-init declarator 遮蔽 typedef, 块内变量遮蔽 typedef 且块外 typedef 恢复可用, 块内 typedef), test/struct.c +2(内层 tag 遮蔽外层 tag, 外层 tag 在内层经 find_tag 上溯可见), 期望值宿主机 clang 验证.
  规模: parse.c 2066 -> **2171**(作用域栈机器 +105 行), sema.c 3153 -> **3095**, chibicc.h 715 -> **697**; parse.c 的 error_tok 维持 15(判定表 A 全集); codegen.c 与 type.c 零改动.
- 为什么改: 共享表要求两个阶段对同一个 static 栈轮流 push/pop, 且 parse 登记的 typedef 名与 sema 登记的对象/枚举常量混在同一张 HashMap 里 - 库语境下这是"两个阶段共用可变全局状态", 与可重入性直接冲突, 也让"谁是作用域的权威"取决于当前处在哪个阶段. 拆开后 parse 的表只回答 C 文法要求的两个问题(这个名字是不是 typedef 名, 这个 tag 指向什么), 即阶段 3 明确永久保留的词法 hack oracle; sema 的表只承载名字解析结果, 由它自己的遍历结构驱动. 两侧的生命周期自此互不重叠, 也不再需要"parse 先 push, sema 后 push 到同一个栈上"这种隐式契约.
- 测试结果: **四闸门全绿, raw 逐字节为空**(无需重置基线). 对照基线做法同 R2.1-R2.5: 用 HEAD(R2.6, 516c4a0)编译器 + 本提交的新测试源在容器内生成 41 文件快照, 新编译器下 `docker-snapshot-diff`(raw)与 `docker-snapshot-ndiff`(归一化)**均为空** - 表的拆分没有改变任何产物字节. `docker-test` 退出码 0(新增 5 条断言 stage1 + stage2 自举各一遍全过, driver.sh passed, 诊断锁定 stage1/stage2 各 "43 cases byte-exact"), `docker-test-thirdparty THIRDPARTY=tinycc` 退出码 0(tcc 自身编译 3149 ms + tests2 全套). 宿主机 A/B 旁证: 参考编译器(/tmp/cbase27, 自 516c4a0 构建)与新编译器对同一份新测试源的 39 个 `test/*.c`(atomic/tls 需 Linux 系统头, 本机跳过)输出 `.s` 与 stderr **39/39 逐字节相同**(带 SNAPSHOT_DATEFLAGS 吃掉时钟伪影). 另用 `nm` 核对层边界: `parse.o` 的未定义符号集与 `sema.o` 的导出符号集**交集为空** - sema.o 现在只导出 `sema` 与 `const_expr`(后者供 preprocess.c 的 `#if`), parse.o 的跨文件依赖只剩 type.c 的类型构造器与 tokenize.c/hashmap.c 的基础设施.
- 偏差说明: 三项, 均为实现形态, 无行为偏差.
  1. **一张 `names` 表同时装 typedef 名与遮蔽条目**: 计划文本写"仅 typedef/tag 的作用域栈", 实施为 typedef 名与被声明名的空条目同表 - 后者既不是 typedef 也不是 tag, 但它是 oracle 正确工作的前提(`t t=1; t;` 里末尾的 `t` 必须不再被当作类型名), 单独建一张表只会让 find_typedef 多一次查表. 表中不存在任何 `Obj *` 或枚举值, "只装文法分类信息"的实质不变.
  2. **登记侧函数一并收编**: 计划只列了六个查询/驱动函数, `add_typedef`/`add_declared_name` 是它们的写入侧, 同样只被 parse 调用, 故一并转为 parse 私有. 此后 chibicc.h 的 parse/sema 接口只剩 `parse`/`sema`/`const_expr`/`conditional`/`get_ident` 与六个节点构造器.
  3. **"sema 的作用域由 resolve 遍历自管"在 R2.6 已成立**: enter_scope/leave_scope 的 sema 侧调用点自 R2.6 起全在 resolve_node/resolve_function 内, 本步改变的是这些调用落到哪张表上, 不是调用结构. 另: `in_file_scope()` 读的 `scope->next == NULL` 自此是 sema 自己栈的深度(而非共享栈), 语义不变; 该 oracle 的拆除是 R2.8 的内容.

### R2.8 隐藏变量创建归 sema (本提交)

- 改了什么: `in_file_scope()` 删除 - 复合字面量的存储类判定不再从"作用域栈有多深"倒推, 改由 resolve 遍历自己维护的显式上下文标志 `resolving_body` 给出: `resolve_function` 在解析函数体前 save/置位, 出来后恢复(嵌套函数定义从体内进入, 故必须 save 而非清零), `resolve_node` 的 ND_COMPOUND_LITERAL case 据此选 `new_lvar`(体内, 自动存储期)或 `new_anon_gvar`(文件域, 静态存储期). test/complit.c +5 断言: 文件域复合字面量分别位于一个函数定义**之前**与**之后**(`int *before = (int[]){1,2}; int mid(void){...} int *after = (int[]){3,4};`), 加上 mid 体内的一处块域复合字面量, 期望值宿主机 clang 验证. 规模: sema.c 3095 -> **3102**, parse.c/chibicc.h/codegen.c/type.c **零改动**.
- 为什么改: `scope->next == NULL` 是拆分线 4.2b 留给 parse 的三个 oracle 之一, 它成立的前提是"作用域栈由 parse 驱动, 深度即词法位置" - R2.7 把栈拆开后这个前提在字面上已经失效(读的是 sema 自己的栈), 只是恰好还给出正确答案. 复合字面量的存储期是 C 语义(块内自动, 文件域静态), 应当由 sema 的遍历上下文直接陈述, 而不是从栈深度反推. 至此 4.2b 的三个 oracle 只剩 typedef/tag 分类一项, 而那一项是阶段 3 明确永久保留的.
- 测试结果: **四闸门全绿, raw 逐字节为空**(无需重置基线). 对照基线做法同前: 用 HEAD(R2.7, 59b6d2a)编译器 + 本提交的新测试源在容器内生成 41 文件快照, 新编译器下 `docker-snapshot-diff`(raw)与 `docker-snapshot-ndiff`(归一化)**均为空**. `docker-test` 退出码 0(新增 5 条断言 stage1 + stage2 自举各一遍全过, 诊断锁定 stage1/stage2 各 "43 cases byte-exact"), `docker-test-thirdparty THIRDPARTY=tinycc` 退出码 0. 宿主机 A/B: 参考编译器(/tmp/cbase27, 自 59b6d2a 构建)与新编译器对同一份新测试源的 39 个 `test/*.c` 输出 `.s` 与 stderr **39/39 逐字节相同**; 另对 4269 行的指针算术压力文件逐字节相同.
  **新断言的非空转验证**(按 gate-blindspots 的纪律, 撤掉修复看观测值是否翻转): 把 `resolve_function` 末尾的 `resolving_body = save_body;` 删掉重编, `after` 的初始化数据由 `.quad .L..7+0` 退化为 **`.quad +0`**(该复合字面量被当作块域处理, 建成一个挂在 mid 的 locals 链上的 lvar, 于是文件域的重定位指向一个没有名字的局部对象), 匿名全局编号整体少一个; `before`(在任何函数定义之前)不受影响. 即缺了恢复动作时新增的 `after[0]`/`after[1]` 两条断言会读到空指针 - 该形状(文件域复合字面量出现在函数定义**之后**)是 41 文件语料与 test/complit.c 原有内容都没有的, 与 R2.5/R2.6 两次由"新触发点排不到"引起的缺陷同类.
- 偏差说明: 三项.
  1. **R2.6 已吸收本步主体**(同 R2.6 偏差 1 的预告): 计划要求的"复合字面量的域判定与 new_lvar/new_anon_gvar 移 ND_COMPOUND_LITERAL case"与"隐藏变量创建归 sema", 在 R2.6 落地两趟遍历时已经完成 - 复合字面量的隐藏变量在 `resolve_node` 的 ND_COMPOUND_LITERAL case 创建, 字符串字面量匿名全局在 ND_STRING case(pass1), `__func__`/`__FUNCTION__`/形参/va_area/alloca_bottom 在 begin_function, 全部临时变量与 ret_buffer 在 pass2 的各降级点. 本步实测核对: `parse.o` 不引用 `new_lvar`/`new_gvar`/`new_anon_gvar`/`new_string_literal` 中任何一个(nm 口径, 见 R2.7 测试结果), 拆分线 2.3 记的"变量创建仍在 parse"偏差就此**完全清算**.
  2. **块域 static 的匿名全局创建留在 pass1, 不移 pass2**(计划文本未达成, 结构性原因): 计划写"改由 ND_DECL 降级路径完成". 实测该移动不可行 - pass1 的 `bind_ident` 要为同一块内后续引用绑定名字, `int f(void){ static int x = 1; return x; }` 里 `return x;` 的 x 在 pass1 就得有 Obj, 否则报 "undefined variable". 若改为"pass1 建一个不在任何链上的占位 Obj, pass2 再补匿名名并挂进 globals", 则 globals 的插入序从 resolve 序变成标注序(数据段顺序随之变), 且占位 Obj 必须避开 locals 链以免被 assign_lvar_offsets 分配栈偏移 - 更多机械, 零可观测收益. 现状即"创建 + 名字登记在 pass1, 数据段序列化(`gvar_init_data`)在 pass2 的降级路径", 拆分线 3.4a 记的偏差实质是"创建发生在 parse", 这一点已由 R2.6 清算; "创建发生在降级趟"的字面要求判定为不采纳.
  3. **实现形态**: `resolving_body` 是 resolve 遍历的上下文标志而非参数 - 与 analyze 的 `brk_label`/`cont_label`/`current_switch`(R1.2)同一处理方式, 上下文成为该遍的私有状态而不是 static 全局的隐式契约. 库化阶段(路线图 5)的 context 对象化会把它与那几个一并收进去, 本线按纪律 5 不做 context 化.
