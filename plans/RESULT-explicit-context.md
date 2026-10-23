# RESULT-explicit-context.md - 显式 ChibiccContext 传参线执行记录

> 计划: `plans/PLAN-explicit-context.md`
> 分支: `explicit-context`
> 开工提交: `38b6e1e` (plan) 之后

## E0 - 基线与开工准备

### E0.1 基线复验 (2026-10-23)

- `make docker-test`: 绿 (含自举 + stage2 + self-test).
- `make check-library` / `test/self-test.sh`: 在 docker-test 内全绿.
- `make docker-snapshot-diff`: **首次非空**, 仅 `string.s`. 原因: 基线快照
  (`.cache/snapshot/string.s`, 2026-10-23 00:56) 早于 `5bc0827` (01:09) 与
  `0138b5a` (08:05); 后者给 `test/string.c` 增加 6 行 (字符串字面量起始的
  初始化器), 因此新快照多出对应字符串对象的 `.data`. 判定为快照过期, 不是
  行为漂移. 按 E0.1 执行 `make docker-snapshot` 重置, 之后
  `make docker-snapshot-diff` 为空 (41 个文件)。
- 锚点 (与计划 3.x 一致): `cst.c` 2393, `hashmap.c` 145, `lib.c` 473,
  `parse.c` 2654, `preprocess.c` 1520, `provenance.c` 182, `sema.c` 3162,
  `strings.c` 82, `tokenize.c` 1050, `type.c` 161, `unicode.c` 200,
  `chibicc.h` 395, `codegen.c` 2820, `main.c` 1435; 41 个 `test/*.c`.
- 隐式上下文计数: `chibicc_ctx` 出现在 `tokenize.c` 29, `parse.c` 21,
  `sema.c` 15, `preprocess.c` 13, `lib.c` 1 (定义 + 访问器).

### E0.4 审计脚本

- 新增 `test/context-audit.sh` 与 `make check-context` /
  `make check-context-pratt`; 支持 `CHIBICC_LIB`, 独立于 `check-library`.
- E0 版本默认报告模式 (退出 0); `--strict` 已实现但尚未接入 Makefile 默认
  参数 (E10.6 切换并接入 `test-all` 与 Docker 闸门).
- TLS 检查只匹配声明形行, 避免把编译器关键字表里的
  `"_Thread_local"` / `"__thread"` 字符串算成命中.
