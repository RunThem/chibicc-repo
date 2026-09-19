# RESULT.md - 忠实层收尾执行记录

本文件记录 PLAN.md(忠实层收尾线)各步骤的实际执行结果, 供审核与后续会话接续参考. 前一线(语法语义拆分)的计划与执行记录见归档 `PLAN-split.md` / `RESULT-split.md` - 其终态(02e5c13)是本线的量化对比基线.

## 闸门口径(本线生效)

- 常规步骤: `make docker-test`(含自举) + `make docker-snapshot-diff` 为空 + 诊断锁定测试.
- **[重组]** 步骤: 行为三闸门(docker-test / tinycc / 诊断测试) + 归一化 diff(`make docker-snapshot-ndiff`)为空, 同提交内 `make docker-snapshot` 重置 raw 基线, 提交说明声明"含预期字节变化".

## 基线记录(R0.3 完成后填写)

- docker-test: 2026-09-19, HEAD = 79e6c76(R0.2), 退出码 0 - 测试集 + driver.sh + 自举重跑全过; 诊断锁定 stage1/stage2 各 "43 cases byte-exact".
- tinycc: 退出码 0(test/thirdparty/make shim 跳过 Rosetta 下不稳定的 106_pthread/112_backtrace/113_btdll 三个用例, 其余全过).
- raw / 归一化 diff: `docker-snapshot-diff` 空 + `docker-snapshot-ndiff` 空; raw 基线沿用拆分线终态后的快照, 尚未重置(首个 [重组] 提交内重置).
- 诊断锁定测试: docker-test 内 stage1 + stage2 两遍全绿; 宿主机本地(macOS)另跑 43/43.

## 提交一览

| 提交 | 步骤 | 内容 |
|---|---|---|
| 16a6719 | R0.1 | 诊断锁定测试(test/diagnostic.sh, 43 用例) |
| 79e6c76 | R0.2 | 归一化快照 diff 闸门(snapshot-normalize.awk + docker-snapshot-ndiff) |
| 8a69232 | R0.3 | 基线复验留档(四闸门全绿) |
| (本提交) | R1.1 | 匿名名计数器与名字物化三函数迁 sema.c |

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

### R0.3 基线复验 (本提交)

- 改了什么: 无代码改动 - 在 HEAD = 79e6c76 上跑齐四项闸门, 结果写入上方"基线记录", 作为本线后续所有步骤的对照基线.
- 为什么改: 本线开工基线必须留档; 自此归一化 diff 闸门(docker-snapshot-ndiff)正式启用, raw 基线保持拆分线终态后的快照, 待首个 [重组] 提交内重置.
- 测试结果: 四项全绿 - docker-test 退出码 0(测试集 + driver.sh + 自举重跑, 诊断锁定 stage1/stage2 各 43/43); tinycc 退出码 0; raw diff 空; 归一化 diff 空. 另: 宿主机本地(macOS)构建后 diagnostic.sh 43/43.
- 偏差说明: 无 - 编译器源码零改动, 工作区仅剩与本线无关的未跟踪文件 .zcodeignore.

### R1.1 匿名名计数器归 sema (本提交)

- 改了什么: `new_unique_name`(含计数器 static)与 `new_anon_gvar`/`new_string_literal` 两个调用方从 parse.c 整体迁入 sema.c, 紧邻 new_gvar 等变量构造器; chibicc.h 的声明区注释同步(计数器所有权说明). parse.c 对这三个函数只剩跨文件调用(复合字面量的 new_anon_gvar, R2.8 收编).
- 为什么改: R1.1 的核心是计数器所有权 - 匿名名(字符串字面量全局, static 局部, 控制流标签)的唯一发号器归 sema, 为 R1.2 把标签分配触发点移入遍历扫清所有权问题; 拆分线 1.10 的"计数器交织"顾虑自此从所有权层面解除.
- 测试结果: 四项全绿 - docker-test 退出码 0(诊断锁定 stage1/stage2 各 43/43), tinycc 退出码 0, raw diff 为空, 归一化 diff 为空. raw 为空符合预期: 本步只搬定义, parse 侧分配触发点的调用顺序逐点不变, 计数器交织原样保留.
- 偏差说明: 步骤分工与 PLAN 文本有出入 - PLAN 把"brk/cont 标签分配移 sema"与"ND_LABEL/ND_LABEL_VAL unique_label 机制移入"列在本步, 实际这两项与绑定/stray 检查在构造现场标注架构下不可分离(parse 侧 break 绑定要求标签先于体解析而存在, 分配触发点无法先于绑定单独迁移), 故全部并入 R1.2 的前序语句下降一次完成; 本步只承担计数器所有权搬迁, 无行为变化. 归类为实现形态调整.
