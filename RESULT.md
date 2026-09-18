# RESULT.md - 忠实层收尾执行记录

本文件记录 PLAN.md(忠实层收尾线)各步骤的实际执行结果, 供审核与后续会话接续参考. 前一线(语法语义拆分)的计划与执行记录见归档 `PLAN-split.md` / `RESULT-split.md` - 其终态(02e5c13)是本线的量化对比基线.

## 闸门口径(本线生效)

- 常规步骤: `make docker-test`(含自举) + `make docker-snapshot-diff` 为空 + 诊断锁定测试.
- **[重组]** 步骤: 行为三闸门(docker-test / tinycc / 诊断测试) + 归一化 diff 为空, 同提交内 `make docker-snapshot` 重置 raw 基线, 提交说明声明"含预期字节变化".

## 基线记录(R0.3 完成后填写)

- docker-test: (待填)
- tinycc: (待填)
- raw / 归一化 diff: (待填)
- 诊断锁定测试: (待填)

## 提交一览

| 提交 | 步骤 | 内容 |
|---|---|---|
| (本提交) | R0.1 | 诊断锁定测试(test/diagnostic.sh, 43 用例) |

## 各步详情

### R0.1 诊断锁定测试 (本提交)

- 改了什么: 新增 test/diagnostic.sh - 43 个用例, 每个用例 = 触发一处诊断的独立源码片段 + 期望 stderr 原文, 在临时目录以 `<用例名>.c` 编译后与期望逐字节 diff; 随 make test 与 make test-stage2 对 chibicc 与自举编译器各跑一遍(Makefile 两处接线). 编译器源码零改动.
- 为什么改: R2 各步要把大量检查从 parse 移入 sema, 移动纪律是 "文案 + 锚点" 保真 - 本测试是该纪律的回归闸门; 同时把拆分线 4.2c 的两处诊断时机偏差拍板为规范(锁进用例): "too many arguments" 锚调用右括号(ND_FUNCALL.tok), 块域 `void x = <初始化器>;` 锚初始化器之后的 token, 块域 static 路径(declare_static_local)仍锚 `=`.
- 测试结果: 本地(macOS) 43/43 通过; `make docker-test` 退出码 0, diagnostic lock 在 stage1/stage2 各报 "43 cases byte-exact"; `make docker-snapshot-diff` 为空.
- 偏差说明: 覆盖 = 判定表 A 15 处 + B 4 处 + C 11 处 + D 3 处(parse.c 33 处 error_tok 全部) + 判定表 E 9/10 处. 唯一未覆盖: sema.c 的 "redeclared as a different kind of symbol" - find_func 只返回 `is_function` 为真的对象, 该守卫恒假, 是拆分线遗留的死检查, 任何输入不可达(内置 alloca 未注册进作用域表, `void alloca() {}` 亦不触发), 已在测试头注释记录. 另两点现状一并锁定: 文件域 `void x;` 被接受(无检查, 无用例), 块域 `int x; int x;` 被接受(重定义检查只对函数定义生效). 触发写法的非显然点, 供 R2 维持用例时参考: `.5` 被词法为浮点字面量(字段指示符用 `.+` 触发); `typedef static int x;` 合法(typedef 组合检查只拦 typedef 携带两个以上其他存储类, 用 `typedef static extern` 触发); `unsigned unsigned` 合法(符号位是 `|=`, 位掩码用 `char float` 触发); "expected string literal" 属 asm 语句而非初始化器(`asm(1)`); `int ()` 走全局变量路径(函数名省略用 `int ()()` 触发); `goto` 缺标签是 get_ident 的可达路径.
