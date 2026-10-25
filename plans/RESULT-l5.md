# RESULT-l5.md - L5 产品化验证

> 状态: 已完成 (2026-10-25)
> 计划: `plans/PLAN-l5.md`
> 提交: `cb35df4` (计划 + 关闭展开流 CST) / `733dc3d` (W1) / `68362da`
>        (W2) / `3491438` (W3) / `a8061ee` (W4) / `25ced38` (W5) /
>        本提交 (W6 记录)
> 基线: `main` @ `c530431`

## 1. W1 - CST 覆盖 (733dc3d)

- `({ ... })` 语句表达式: `cst_parse_primary` 的 `(` 分支在 `(` 后跟 `{`
  时复用 `cst_parse_compound`, 新 kind `CST_STMT_EXPR`.
- 类型名实参: 新 `cst_parse_arg` 先按 assign 解析, 失败则回退为
  `cst_parse_type_name_core`(从 `cst_parse_type_name` 抽出的无括号内核);
  `_Generic` 的关联臂 `(type-name | "default") ":" assign` 建
  `CST_GENERIC_ASSOC`; `_Generic` 本身是关键字, 在 primary 给出调用形的
  特例.
- 实测: 48 语料文件 UNPARSED **770 -> 45**(arith.c 61 -> 0,
  initializer.c 114 -> 0 等; 判据 "< 60" 达成).
- 残余 45 的成因(golden 锁定; 2026-10-25 收尾后逐形复测修正过一次分类):
  macro.c 19(原始流上的宏形状如 `M1 5`, 本就不是合法 C 表达式, 属宏保真
  边界, 合法保留), attribute.c 14(`offsetof(struct __attribute__((packed))
  {...}, a)` 属性内嵌 struct), varargs.c 4 / atomic.c 3 / tls.c 1(include
  提供的 typedef: va_list 与 pthread_t, W2 的 seed 口径; atomic.c 首版
  误记为 `_Atomic` 声明形, 复测修正), string.c 3(初始化器位置的字符串
  后缀: `int x = "abc"[0];` 失败而同形表达式语句 `x = "abc"[0];` 与
  `int x = y[0];` 均成功 - 疑为初始化器路径对 TK_STR 的快路径未走完整
  postfix), typedef.c 1(文件级 typedef 名同时作变量名的歧义角落:
  `MyInt = MyInt + 1;` 被 decl_start 判为声明后失败且无表达式回退).
- cst.sh 新增 4 个 dump 样本(65 个, 既有 61 个逐字节不变); 通道与
  docker-test / 快照全绿.

## 2. W2 - libcp_cst_seed_typedef (68362da)

- 公共 API: `libcp_cst_seed_typedef(ctx, name)`(D7 +1, 定义在 lib.c 的
  配置族旁); `LibcpContext` 增 `cst_seeds` 配置数组(跨 reset 保留,
  free 时释放); `libcp_cst_parse` 先注入 seed 再跑本 TU 的 typedef 扫描.
- `test/library.sh` 新检查: 未 seed 时 `va_list ap;` 恰 1 个 UNPARSED,
  seed 后成结构, reset 后 seed 仍在.

## 3. W3 - 覆盖 golden (3491438)

- `test/cst-stats.sh <chibicc> [update]`: 统计每语料文件 dump 输出的
  UNPARSED 行, golden(`test/cst-stats.golden`)逐行比对; `make test` 与
  `test-stage2` 各跑一遍(两套编译器计数必须一致).
- 坑: `grep -c` 零命中时退出码 1 会杀死 `set -e` 脚本, 需 `|| true` 保住
  计数输出.

## 4. W4 - formatter MVP (a8061ee)

- `tools/fmt.c`(240 行, 只 include `<libcp/libcp.h>`): 行保持的空白规范化
  - 每个 trivia 换行原样保留(行数与 `__LINE__` 语义不动); 注释与
    反斜杠-换行续行字节原样; 缩进按树重算(BLOCK / 语句表达式 / 定界组
    深一级, 收尾定界回开者级别, 2 空格每级); 其余 token 间空白折叠为
    一个(原本没有就没有); 拼写永不改; `-s name` 透传 seed.
- `test/fmt.sh`: 48 文件幂等 + 行数不变 + 注释数量不变(本机跑这三项);
  容器内追加 41 个 test/*.c 的 **-S 逐字节等价**(时间宏固定).
  `make test` / `test-stage2` 各跑一遍(stage2 版 fmt 链
  `stage2/libcp.a`, 自举库也要过).
- 调试中修掉的三个坑(测试设计教训, 均已写进脚本注释): `-S` 首行
  `.file` 嵌入源路径 -> 原地格式化(容器 /work 是一次性副本); 时间宏值
  必须带字面引号(经变量展开会丢); `test/pragma-once.c` 以路径自包含,
  拷贝到别处会因路径身份变化重定义 main.
- 风格变化(合法空白级, 幂等): 块内指令行获得缩进; 宏续行的下一行回到
  列 0; BOM 被当空白折叠掉.

## 5. W5 - source mapping 与诊断展开点 (25ced38)

- `libcp/src/range.c`: `libcp_node_source_range`(D7 +1) - 子树锚点 token
  按(文件指针, 偏移)取极值; `next` 不跟随, 初始化器树不下降; 纯读,
  无 ctx.
- `LibcpDiagnostic` 追加 `tok`(token 锚定诊断非 NULL)与 `prov`(该
  token 的宏展开链头): `verror_at` 增 tok 参数, `error_tok` / `warn_tok`
  传入, `error_at` / `error` 传 NULL. **默认输出逐字节不变**
  (diagnostic.sh 68 例全绿).
- 头文件环: diagnostic.h 不能 include token.h(token.h -> context.h ->
  diagnostic.h), 用 `struct libcp_token` 前向声明.
- library.sh 新检查: `int f(void){return 1+2;}` 的 `+` 子树 range ==
  start "1" / end "2"; 宏内错误的回调拿到 prov 链, 沿链到最外层, 展开
  名 == "M"; 普通错误 prov == NULL.
- 符号面: 130 个外部符号, 全部 `libcp_` 前缀(128 + 两个新 D7).

## 6. API 手感发现清单(本线的核心产出)

1. **CST 遍历面够用**: kind + children + child_count + tok 四件套支撑了
   自顶向下发射的全过程, 没有出现需要父指针的场景.
2. **trivia 的结构化解读是公共面缺口**: fmt 手写了 trivia 扫描(块注释
   边界 / 行注释 / 续行 / BOM / CR 家族). 每个空白敏感的消费者都要重写
   这段. 候选公共面: 一个 trivia 分段迭代器(按 注释/续行/换行/空白 分段).
   待拍板, 不在本线擅加.
3. **seed API 手感好**: 工具知道自己目标的头, `-s va_list` 直接透传;
   配置语义(reset 保留)对工具的多文件循环正合适.
4. **代表 token 的选择影响 range 语义**: `ND_FUNCDEF` 的 tok 是函数体的
   `{`, 整个函数的 range 从名字 token 开始而非声明说明符. 已写进
   docs/09; 想要"更早"的边界要自己看 spec_decls.
5. **诊断展开点一次到位**: tok + prov 两个字段就能从宏体内错误走到最外
   层调用点; provenance 链的 prepend 方向(最内层在前)在"走到最外层"的
   用法下是 循环到尾, 无歧义.
6. **per-node user data 扩展点 - 负结果**: 只读遍历型消费者(formatter)
   不需要在节点上挂任何数据(状态在遍历者手里, 树只读). 该决策继续推迟
   到改写型消费者(C→C 打印器)落地时再拍, 与计划 1.2 一致.
7. **公共枚举追加无破坏**: `CST_STMT_EXPR` / `CST_GENERIC_ASSOC` 尾部
   追加, 全部闸门(dump/golden/public-api/.so)无感.
8. **小刺**: provenance 访问器参数非 const, 消费者拿 `const LibcpMacroProv *`
   要转型(D7 签名锁定, 记录在案).

## 7. 决策记录

- **展开流 CST: 关闭(2026-10-25)** - 维持现状(CST 建在预处理前, 展开流
  用第 3 层与 provenance), 理由即 AGENTS.md 未决项里的既有分析; 记录于
  W0 提交与 AGENTS.md 已决事项.
- **per-node user data: 继续推迟** - formatter 的负结果(见 6.6)支持
  "用到再拍"; AST 侧证据等 C→C 打印器.

## 8. 终态闸门 (W6)

- 本机: make / check-context / check-library / 前端通道(ast 85, trivia,
  cst 65 样本, expand, diagnostic 68) / cst-stats golden / fmt.sh /
  library.sh(含 ASan 轮)全绿.
- docker: docker-test(自举 + stage2, fmt 双套)全绿; 快照
  `snapshot diff: empty`; 容器 check-library(D7 38 个齐, `.so` 恰好
  D7); tinycc thirdparty 全绿.

## 9. 后续(2026-10-25 用户拍板: 全部挂到 C→C 打印器线上评估)

用户决定: 暂不修复下列任何一项, 先落地 C→C 打印器(第二个消费者, 第 3/4
层), 在实现它的过程中带真实需求重新评估每一项的看法. 记录在案的分析:

**残余 UNPARSED 的收编判定**(修正后分类, 见 1 节): 45 个中只有 18 个
属于"值得收编"的真文法缺口 - attribute.c 14(真实代码里 packed struct +
offsetof 常见, 价值最高, 改动在 decl_specs 的 struct/属性 raw-group 组
合), string.c 3(初始化器快路径), typedef.c 1(语句层回退或向前看); 预算
半天级. 8 个 include-typedef 已由 seed API 解决, golden 不含 seed 是刻
意口径; 19 个 macro 形状**明确不收编** - 收编只有两条路, 都违反已决
策略(CST 预处理前 / 不引入宏调用节点), 它们是宏保真决策的正确代价.

**trivia 分段迭代器**(6.2 的展开): 候选形态是 trivia 分段枚举
(SPACE / NEWLINE / COMMENT / CONTINUATION / BOM)加迭代器, 实现约 60 行;
拍板口径: 孤立 CR 算不算 NEWLINE, BOM 独立段还是 SPACE, 续行段边界,
迭代器形态与命名. 验收方式: fmt.c 删掉手写扫描换用它(第二次手感验证).
推迟理由: 单一消费者证据不足; C→C 打印器若打印时自造空白, 则它也不
需要, 证据会更完整.

**range 语义**(6.4 的展开): 保持现状的理由 - 改 FUNCDEF 锚点是行为
变化(诊断插入符被 diagnostic.sh 逐字节锁定); "锚点极值"契约零字段成本;
docs/09 已写明消费者组合义务(spec_decls). 若 C→C 打印器被"range 不含
声明说明符"实际绊住, 再议便捷函数 libcp_decl_range 或 first/last token
记录(后者是构造点级大改, 需独立拍板).

**per-node user data**: formatter 的负结果(6.6)加上 C→C 打印器的证据
共同构成该决策的完整输入 - C→C 是改写型消费者, 正是最可能需要挂节点
数据的形态.
