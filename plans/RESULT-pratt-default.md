# RESULT-pratt-default.md - Pratt 单一路径与内部符号前缀

> 状态: 已完成 (2026-10-25)
> 计划: `plans/PLAN-pratt-default.md`
> 提交: `2d3510e` (计划) / `3d4a558` (阶段 1) / `ab6106f` (阶段 2) / 本提交 (记录)
> 基线: `main` @ `e8d6f3d`

## S1 - 删除旧级联, Pratt 唯一化 (`3d4a558`)

改动与计划一致:

- `libcp/src/parse.c` 2654 -> 2116 行: 8 个 `#if !LIBCP_PRATT_PARSE_FEATURE` 块
  (5 个前置声明守卫 + 3 段旧级联定义体, 21 个函数 / 约 509 行) 整体删除,
  Pratt 段的 `#if` / `#endif` 拆包装, 内容原样保留; 非 static 的
  `conditional` 保留 Pratt 版, `sema.c` 的 `const_expr` 调用不变.
- `libcp/src/libcp.h` 删宏定义; `Makefile` 删全部 pratt 目标 / 规则 / 变量
  / clean / BOOT / .PHONY / 4 个 docker 目标(共约 130 行);
  `git rm test/pratt-ab.sh`; `test/library.sh` / `test/self-test.sh` 删
  pratt 环境变量逻辑(self-test 归档从四套变两套); ast.sh /
  context-audit.sh / public-api.sh / self-test.c / ci.yml / .gitignore 的
  双构建说法清理; docs/01 / 06 / 08 / 10 / 12 现状同步(docs/12 新增
  §12.12 并把后续小节重编号, §12.11 历史小节原文保留); AGENTS.md 路线图 /
  已决事项 / 构建与测试 / 现状 / 设计原则 / 测试约定更新.
- parse.c 内两处 "in the Pratt build" 注释随单一路径改写.

闸门(全绿): 本机 `make`(0 warning) + `make check-context` +
ast(85 逐字节) / trivia / cst / expand / diagnostic(68 逐字节) 通道 +
`make check-library`; docker `docker-test`(自举 + stage2) +
`docker-snapshot-diff` 输出 `snapshot diff: empty` + 容器内
`check-library`(含 `.so` 恰好导出 D7 与 install / pkg-config) +
`docker-test-thirdparty THIRDPARTY=tinycc` 全绿.
判据 1: `LIBCP_PRATT_PARSE_FEATURE` 在活面零残留(仅 `plans/**` 归档与
docs/12 / docs/06 的历史沿革说明).

## S2 - 内部符号 libcp_ 统一前缀 + 合并薄包装 (`ab6106f`)

### 符号面终态

- `nm -g libcp.a`: 外部符号 128 个, **100% `libcp_` 前缀, 零裸名残留**
  (基线 148 = 106 裸名 + 42 已前缀; 删 20 个薄包装后 128 = 36 个 D7 +
  92 个内部).
- 106 个改名: 93 函数 + 13 个 `ty_*` 变量, 词边界受限替换
  (`(?<![A-Za-z0-9_])name(?=\s*\()`, 变量用完整词边界), 覆盖
  `libcp/src/*.c` + `libcp.h` + `codegen.c` + `main.c` +
  `test/self/self-test.c`.
- 21 个撞名对的处理(与计划的偏离见下):
  - **20 个合并**: 实现原地改用公共名(定义留在 preprocess.c / parse.c /
    sema.c / cst.c / provenance.c), `lib.c` 的包装删除, 内部 umbrella
    删掉重复声明(公共头已有, `LIBCP_API` 声明可见性随定义继承, 由容器
    `.so` exact-D7 闸门证实).
  - **1 个保留一层**: `tokenize_file` -> `libcp_tokenize_file_impl`,
    `lib.c` 的 `libcp_tokenize_file` 门面保留(base_file 默认值 + 打不开
    报错), 因为 `preprocess.c` 的 `#include` 路径(preprocess.c:290)需要
    原始"打不开返回 NULL, 由调用方带 `#include` 锚点报错"的行为, 不能走
    公共入口的 base_file 变更与报错.
- 9 个合并的管线 / 通道入口带错误边界: `ENTER_RET` / `ENTER_VOID` /
  `LEAVE` 宏从 `lib.c` 搬到内部 umbrella `libcp.h`, 边界随实现移动:
  `libcp_preprocess` / `libcp_parse` / `libcp_sema` / `libcp_cst_parse` /
  `libcp_cst_reconstruct` / `libcp_cst_dump` / `libcp_cst_verify` /
  `libcp_expansion_dump` / `libcp_expansion_verify`. 其中
  `cst_reconstruct` / `cst_verify` / `expansion_verify` 原实现无 ctx,
  合并时按公共签名补 `LibcpContext *ctx` 首参.
- 11 个 provenance 访问器纯改名(无边界), `provenance.c` 的调用点同步.

### 与计划的偏离

1. **S2.3 预计"有实际逻辑的包装 0 个" - 实际 10 个**: 管线与通道的包装
   不是纯一行转发, 它们携带错误边界(setjmp 帧)ENTER/LEAVE, `tokenize_file`
   的包装还有 base_file 与报错逻辑. 处理: 9 个把边界随实现搬走(真合并),
   `tokenize_file` 保留一层. 合并语义与拍板一致("实现直接用公共名, 删无用
   包装"), 边界行为逐字节不变.
2. **3 个函数内部签名变化**: `cst_reconstruct` / `cst_verify` /
   `expansion_verify` 补 ctx 首参(公共 API 本来就是这签名, 内部实现向
   公共形状对齐).
3. **改名脚本的 6 处误伤当场修复**: `__attribute__((format(...)))` 里的
   `format(` 被规则命中成 `libcp_format(`(libcp.h 5 处 + codegen.c 1 处),
   已还原; 注释里 23 处指称已改名函数的旧名(如 `add_type`, `new_arith`,
   `tokenize_file`)更新, 2 处文法注释把被半改的 `libcp_conditional` 还原
   为文法产生式名 `conditional`(cst.c 的 designator / case 行).
4. **public-api.sh 黑名单重写**: 原裸名黑名单在统一前缀后失效, 改为
   "精确名单(20 个内部名) + 家族模式(hashmap/arena/sema/ty 四族)" 锚定
   `libcp_` 前缀; 第一版正则的 `tokenize` 前缀会误伤 D7 的
   `libcp_tokenize_buffer`, 已改为精确匹配.

### 闸门(全绿)

- 本机: `make`(0 warning) + `make check-context` + ast(85 逐字节) /
  trivia / cst / expand / diagnostic(68 逐字节) 通道 + `test/self-test.sh`
  + `make check-library`(含新黑名单的 public-api.sh).
- docker(最终树): `docker-test` 全绿(自举: chibicc 编译改名后的库源码,
  stage2 归档同样全前缀) + `docker-snapshot-diff` 输出
  `snapshot diff: empty` + 容器内 `check-library` 全绿(`.so` 恰好导出
  D7 - 合并方案的关键闸门, 证明 `LIBCP_API` 声明的可见性被
  parse.c/sema.c/cst.c/preprocess.c/provenance.c 里的定义继承).
- 判据 3: `nm -g libcp.a` 128/128 前缀.

## 第三点 - AST 改写器盲区的最终状态

- **盲区 1(Pratt 分支不可见)与盲区 3(跨模式重名)**: 随 S1 的双模式删除
  **结构性消失** - 全库不再有 `LIBCP_PRATT_PARSE_FEATURE` 守卫,
  `expr` / `assign` / `conditional` / `unary` 各只有一份定义.
  `grep -rn LIBCP_PRATT_PARSE_FEATURE` 活面零命中即为验证.
- **盲区 2(`LIBCP_TEST_BUILTINS` 不可见)**: 保留现状. `preprocess.c` 的
  `multi_macro` 测试钩子仍只在 `test/library.sh` 的 ASan 轮编译, 由该轮
  继续覆盖(默认与 stage2 两套归档各跑一次); `test/context-audit.sh` 的
  文本 grep 对 `#ifdef` 块同样生效. 该宏只此一处使用, 无扩散.

## 后续

- L5 产品化验证线仍是唯一未开工的路线图项.
- 两个未决决策(per-node user data 扩展点, 展开流 CST)仍待用户拍板,
  与本线无关.
