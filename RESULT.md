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
| (本提交) | R1.2 | analyze 前序语句下降入 sema: 标签分配 + break/continue/case 绑定 + stray 四检查 |

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

### R1.2 绑定与 stray 检查移 sema (本提交)

- 改了什么: sema.c 新增导出的 `analyze(Node *body)` - 函数体的一遍控制流下降, 由三个 static 组成(analyze_node / analyze_chain / analyze_children), 在 parse.c 的 `function()` 末尾替代原 `resolve_goto_labels()` 调用. 它承担: 循环与 switch 的 brk_label/cont_label 分配, ND_BREAK/ND_CONTINUE 填 unique_label 并降级为 ND_GOTO, ND_CASE 分配 label 并挂入所属 switch, ND_GOTO/ND_LABEL 的清单收集与 unique_label 分配, stray break/continue/case/default 四处检查, 以及 ND_WHILE -> ND_FOR 降级. `resolve_labels` 由导出改为 static 并改读 sema 侧的 gotos/labels. parse.c 删除 current_switch/brk_label/cont_label/gotos/labels 五个 static 与 resolve_goto_labels 壳, switch/for/while/do/case/default/break/continue/goto/label/`&&label` 十处构造点回到只建忠实形状(2516 -> 2427 行, error_tok 33 -> 29, 判定表 B 清空). chibicc.h: ND_CASE 新增 `bool is_default`(sema 挂链时需要区分 `default:` 与 `case <expr>:`, 今天这个信息只存在于 parse 的语法位置), 导出 `analyze` 取代 `resolve_labels`. add_type 删除 ND_WHILE/ND_BREAK/ND_CONTINUE 三个 case, ND_LABEL_VAL case 改为把节点收进 gotos. sema.c 1570 -> 1699 行. codegen.c 零改动.
- 为什么改: break/continue 的目标标签是一次名字绑定, case 归属哪个 switch 也是 - 按层 3 的定义(无类型标注, 无名字绑定)它们不属于语法层. 解析现场维护"当前循环/switch"要求 parse 持有五个可变 static 上下文, 与库的可重入性冲突; 换成函数体解析完后的一遍下降, 上下文变成该遍的局部状态, parse 侧的控制流 static 归零. 判定表 B 的四处 stray 检查随之迁走, 因为它们问的正是"有没有外层目标"这个绑定问题.
- 测试结果: 四闸门全绿. (1) 归一化 diff(`docker-snapshot-ndiff`)为空 - 形状面等价; (2) `docker-test` 退出码 0: 40 个测试可执行文件 stage1 + stage2 自举各一遍全过, driver.sh 126 项 passed, 诊断锁定 stage1/stage2 各 "43 cases byte-exact"; (3) `docker-test-thirdparty THIRDPARTY=tinycc` 退出码 0; (4) raw diff 非空(4 文件 / 414 hunk / 1932 行), 逐行核对**全部**为 `.L..N` 标签编号变化(1932/1932 行含 `.L..`, 无一例外), 符合 [重组] 步骤的预期字节变化口径. 典型形态: constexpr.s 的 "OK\n" 字符串全局由 `.L..47` 变 `.L..45` - 两个控制流标签原先在解析途中取号(排在字符串之前), 现在在函数末尾的 analyze 里取号(排在字符串之后). 本提交内已 `make docker-snapshot` 重置 raw 基线, 重置后复跑 `docker-snapshot-diff` 为空, 确认基线可复现且无不确定性. 另在宿主机本地核对: 四处 stray 诊断文案与插入符位置逐字节不变(stray break / stray continue / stray case / stray default 均锚关键字 token), `use of undeclared label` 不变, `&&lbl` 静态跳转表(test/control.c 的 `static void *p[]={&&v41,&&v42,&&v43}`)三项仍解析到正确的 `.L..3/.L..4/.L..5`.
- 偏差说明: 五项.
  1. **PLAN 文本的 analyze 并不存在** - PLAN 写"analyze 的语句下降是前序的", 但当前代码里没有 analyze, 本步创建它. 同时 R1.1 已记录标签分配触发点并入本步, 故 R1.2 的实际范围 = 分配 + 绑定 + 降级 + stray 检查一次落地.
  2. **case 挂链是后序的, 不是前序** - 原 parse 现场在 `node->lhs = stmt(...)` **之后**才把 case 挂进 case_next, 于是埋在体内层 case 先入链; analyze 若按 PLAN 的前序挂链, codegen 的比较链顺序会翻转, 归一化 diff 实测抓到 control.s 两处 4 行(`cmp $0/je` 与 `cmp $1/je` 互换). 改为"label 分配前序 + 挂链后序"后归一化 diff 为空. 这只在埋藏 case(外层 case 的语句里再出现 case)时有区别; 无重叠的 case 常量下两种顺序语义相同, 但本线要求形状等价, 故取后者.
  3. **ND_LABEL_VAL 的收集留在 add_type, 没有进 analyze** - `&&lbl` 可以出现在语句树之外: 块域 static 数组初始化器里的跳转表只有常量求值器会走到, analyze 的语句下降看不到它, 漏收会导致 `use of undeclared label` 误报. 因此 analyze 的 gotos 清单由两处供给(analyze 收 ND_GOTO, add_type 收 ND_LABEL_VAL); add_type 的 `node->ty` 早退守卫保证每个节点恰好收一次. 代价是 `&&lbl` 与同函数内 `goto` 的入链相对顺序可能与原 parse 顺序不同, 只在同一函数有多处未声明标签时影响首个报错, 归入第 4 项.
  4. **stray 检查时机后移到函数末尾, 多错误输入的首个报错可能改变**(经用户确认接受). 单错误输入完全不变(43 个锁定用例均为单错误, 全绿). 实测两类位移: `int main(){ break; int x=1; return x->y; }` 原报 "stray break"(锚 break), 现报 "invalid pointer dereference"(锚 `->`); `int main(){ int x = ({ break; }); }` 原报 "stray break", 现报 "statement expression returning void is not supported"(锚 `(`) - 后者是 add_type 的语句表达式检查在解析现场先跑所致. 反向不位移: 另一处错误在 stray 之前时两版一致, 且 "use of undeclared label" 由 resolve_labels 在下降之后触发, 两版都排在全部 stray 之后.
  5. **嵌套函数定义中的外层 goto 现在能编译通过**(经用户确认接受). `int main(){ goto out; int inner(void){ return 42; } out: return 1; }` 原报 "use of undeclared label" - parse 的 resolve_goto_labels 在内层函数解析完时就跑, 拿外层已积累的 goto 去匹配内层的 label 清单, 匹配不上即报错, 随后清空清单. analyze 按函数体调用, 内层只收内层的 goto, 外层的留到外层自己的下降里解析. 这是原实现的缺陷被顺带修掉, 不是新引入的行为; 归类为诊断消失.
