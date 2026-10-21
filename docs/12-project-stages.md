# 12 工程阶段史: 每个阶段解决了什么

## 12.1 为什么第三方开发者要读这一章

库现在的代码形态不是一次设计出来的. 它是若干次行为保持的重构叠加的结果. 理解这些阶段, 能回答很多"为什么这里要这样写"的问题:

- 为什么 `parse.c` 保留 typedef oracle, 而不是彻底不做语义.
- 为什么 `sema.c` 不做降级, 而 `codegen.c` 里有一个整形遍.
- 为什么 CST 单独一棵树, 不复用语法树.
- 为什么公共 `Node` 里没有 codegen 字段.
- 为什么 `#if` 的分支在 CST 里保留, 而在预处理后消失.

每一节讲一个阶段: 问题, 设计, 实现, 产物, 产物能干什么, 终态提交.

## 12.2 时间线

| 日期 | 阶段 | 终态或归档提交 |
|---|---|---|
| 2026-09 之前 | 上游 chibicc, 阶段 0 前 | - |
| 2026-09-19 | 语法语义拆分 (阶段 3+4) | `0f29d4a` 归档 |
| 2026-09-21 | 忠实层收尾 | `5413ceb` 归档 |
| 2026-10-12 | codegen 直连 (阶段 A/B/B2) | `e8c8608` 归档 |
| 2026-10-12 | trivia 保留 (阶段 1) | `443b4c2` 完成 |
| 2026-10-18 | CST (阶段 2) | `d67fa4c` 完成 |
| 2026-10-18 | 宏展开 provenance | `a75de30` 完成 |
| 2026-10-19 | 库边界固化 (阶段 5) | `3e799e2` 完成 |
| 2026-10-21 | Pratt 表达式解析 (附加线) | `c3d75fe` 归档 |
| 2026-10-21 | 源码与计划归档整理 | `dd3e631` 之后 |
| 未开始 | L5 产品化验证 (原阶段 6) | - |

日期是终态提交的日期, 不是开工日期. 每个阶段的详细执行记录在 `plans/` 下.

## 12.3 阶段 0: 等价性基线

### 问题

后面所有重构都要求"行为不变". 没有基线, "不变"无法证明.

### 设计

在 Linux amd64 容器里, 对全部 `test/*.c` 和自举产物运行 `./chibicc -S`, 保存汇编文本. 每个阶段结束时逐字节 diff. 任何指令序列变化都视为回归, 除非该阶段明确要改变它.

### 实现

- Dockerfile 构建 `chibicc:amd64`, 基于 ubuntu:22.04.
- 仓库只读挂载, 源码复制到容器可写层 `/work`.
- `snapshot-normalize.awk` 归一化时间宏和不稳定标签编号.
- `make docker-snapshot-diff` 输出 `snapshot diff: empty` 才算通过.

### 产物

- 汇编快照基线.
- 归一化脚本.
- Docker 测试环境.

### 产物可以干什么

- 证明重构没有改变代码生成.
- 在修改共享代码时提供逐字节回归信号.
- 让"行为保持"从口号变成可执行的闸门.

## 12.4 语法语义拆分 (阶段 3+4)

### 问题

上游 `parse.c` 同时做语法和语义:

- 解析时立刻建变量, 查名字.
- 解析时立刻算类型.
- 解析时立刻求常量.
- 解析时立刻做降级.

结果是一个函数几百行, 既读文法又读语义, 无法单独复用语法层.

### 设计

把 `parse.c` 拆成两遍:

- `parse.c` 只建语法树.
- 新的 `sema.c` 做名字解析, 类型检查, 常量求值, 布局.

拆分的关键约束是: 行为逐字节不变. 每个小步都跑汇编快照.

### 实现

- 22 步可勾选清单.
- 声明记录节点 (`ND_DECL`, `ND_GVAR_DECL`, `ND_FUNCDEF`, `ND_TYPEDEF`, `ND_ENUM_CONST`) 作为两遍之间的交接.
- `eval` / `eval2` / `eval_double` / `is_const_expr`, `add_type`, 结构体布局, `write_gvar_data` 迁到 `sema.c`.
- 控制流检查, goto/label 配对检查也迁走.
- codegen 零改动.

### 产物

- `parse.c` 2531 行纯语法.
- `sema.c` 1552 行语义.
- 语法层可以独立存在于编译路径之外.

### 产物可以干什么

- 语法级工具可以只调用 `chibicc_parse`.
- 语义工具可以只关心 `chibicc_sema`.
- 后续的忠实层和 codegen 直连有了落脚点.

终态: `plans/PLAN-split.md` / `plans/RESULT-split.md`.

## 12.5 忠实层收尾

### 问题

拆分只解决了"语义在哪个文件", 没有解决"语法树有多忠实". 拆分后的 `parse.c` 仍然保留大量为语义和代码生成服务的改写:

- 复合赋值展开.
- 自增自减展开.
- 指针算术缩放.
- 关系运算符交换操作数.
- `sizeof` 折叠.
- 字符串字面量转匿名全局.
- 初始化器拍平.
- `while` 降级成 `for`.
- `break` / `continue` 降级成 `goto`.

这些改写让语法层无法服务源到源工具.

### 设计

解除"字节冻结"对树形的限制, 把改写全部移到 `sema.c`:

- 新增 21 个 NodeKind, 表示忠实写法.
- 上游 48 个 kind 中 5 个加忠实字段.
- 解析器只做文法必需的 typedef/tag 分类.
- 声明以记录节点交给 sema.
- 初始化器保留为记录, 不拍平.
- `sizeof` 保留形式, 不折数字.
- 字符串保留 `ND_STRING`, 不物化.
- 关系运算符保留方向.

### 实现

- 约 55 处不忠实构造点逐一消除.
- `ND_SUBSCRIPT`, `ND_GT` / `ND_GE`, `ND_INCDEC`, `ND_WHILE`, `ND_BREAK` / `ND_CONTINUE`, `ND_SIZEOF` / `ND_ALIGNOF`, `ND_STRING` 等新节点.
- `ND_ASSIGN.op`, `ND_CAST.ty_op` / `is_implicit`, `ND_MEMBER.arrow_tok`, `ND_COND.is_elvis` 等字段.
- 降级逻辑在 `sema.c`.
- codegen 相对 `5f53ed0` 零改动.

### 产物

- `parse.c` 2148 行.
- `sema.c` 3145 行.
- 忠实语法树.

### 产物可以干什么

- C 转 C 工具可以直接消费.
- 语法级 lint 能看到源码写法.
- 第 4 层可以在忠实树上做标注, 不必反推.

终态: `plans/PLAN-faithful.md` / `plans/RESULT-faithful.md`.

## 12.6 codegen 直连 (阶段 A/B/B2)

### 问题

忠实层把降级搬进了 `sema.c`. 这解决了语法层的忠实性, 但让语义层变成了"标注 + 整形"的混合体:

- 指针算术缩放.
- 复合赋值展开.
- 自增自减.
- 下标降级.
- 箭头成员降级.
- `>` / `>=` 换序.
- elvis 临时槽.
- `while` 降级.
- break / continue 降级.
- 声明展开.
- VLA 尺寸.
- 返回缓冲.
- 复合字面量.
- case 链与标签.

第 4 层因此不再是"纯语义结论", 语义工具要面对后端形状.

### 设计

把"为发射服务的整形"从 `sema.c` 搬到 `codegen.c`:

- 只需换发射方式的, 在发射点就地处理.
- 需要槽, 语句或标签的, 留在 codegen 的整形遍.
- `sema.c` 只做标注和语义结论.

判据是"这项改写是不是语言要求的". 语言要求的 (字符串匿名全局, 复合字面量对象, ABI 参数区) 留在库层; 后端便利的 (缩放, 回环, 临时槽, 标签) 搬到 codegen.

### 实现

- 阶段 A: A0.1-A10.2, 逐项迁移, 每步汇编快照.
- 阶段 B: B0-B1.3, 把只需换发射方式的项移进发射点.
- 阶段 B2: B2.0-B2.2, 下标, 箭头, `>`/`>=`, elvis, `while`, break/continue, 声明链, 加减法缩放.
- A9.2 逐 case 审计 `add_type`, 确认全部纯标注.
- 两处有意的指令序列变化: elvis 临时槽移除, 嵌套 VLA 维度修正.

### 产物

- `sema.c` 纯标注.
- `codegen.c` 整形遍 + 发射点降级.
- 六条整形不变量.
- 第 4 层的边界定义.

### 产物可以干什么

- 语义工具拿到的是源码写法和语义结论, 没有后端形状.
- 新的后端可以自己决定怎么整形.
- 第 5 层成为一个可替换的参考实现.

终态: `plans/PLAN-codegen-direct.md` / `plans/RESULT-codegen-direct.md`.

## 12.7 trivia 保留 (阶段 1)

### 问题

上游词法器丢弃空白和注释. 格式化器和重构工具无法保留源码布局.

### 设计

- token 保留前置 trivia.
- token 记录 raw 坐标的拼写.
- 预处理前的 token 流可以逐字节重建文件.
- 不改变既有消费者的可观察行为.

### 实现

- 双缓冲: `File.raw` 原样文本, `File.contents` 扫描缓冲.
- `File.map` 扫描偏移到 raw 偏移, `File.lines` raw 偏移到行号.
- `make_scan_buffer` 一遍产出扫描缓冲和两张表.
- token 的 `trivia` / `trivia_len` / `raw_loc` / `raw_len` 都是 raw 坐标.
- 扫描缓冲保持 T1 前的文字效果: 换行归一变 `\n`, 续行删除, universal char 改写, BOM 跳过, 末尾补换行.
- 新增 `-dump-tokens` 和 `-reconstruct`.

### 产物

- 第 1 层.
- `test/trivia.sh`.
- 48 个语料文件与 15 个边界样本的逐字节重建.

### 产物可以干什么

- 格式化器保留注释和空白.
- 重构工具做最小 diff.
- CST 有了输入基础.

终态: 443b4c2, `plans/PLAN-trivia.md` / `plans/RESULT-trivia.md`.

## 12.8 CST (阶段 2)

### 问题

trivia 只保留了 token 之间的字节, 没有语法结构. 格式化器还需要知道语句, 声明, 表达式的边界. 同时, 预处理是不可逆的, 所以结构必须建在预处理之前.

### 设计

- 在 `tokenize_file` 的直接输出上建 lossless 具体语法树.
- 预处理指令是节点.
- `#if` 的所有分支保留.
- 每个语法 token 和标点是显式叶子.
- 叶子按序重建输入.
- 解析是全函数, 不报诊断.
- 文法覆盖不了的跨度成为 `CST_UNPARSED`.

### 实现

- `libchibicc/src/cst.c`, 55 种 `CstKind`.
- 括号分组, 声明, 语句, 表达式, 初始化器, 指示符.
- 指令节点: `#define`, `#undef`, `#include`, `#line`, `#pragma`, `#error`, 空指令, `#if` 分支.
- 文件内 typedef 预扫描作为 cast/声明 oracle.
- 三个只读通道: `-dump-cst`, `-reconstruct-cst`, `-verify-cst`.
- 305 项断言, 61 个 dump 样本, 48 个语料文件加 15 个边界样本 round-trip.

### 产物

- 第 2 层.
- `test/cst.sh`.

### 产物可以干什么

- 格式化器.
- 注释迁移.
- 预处理感知的源码索引.
- 局部重构.

终态: d67fa4c, `plans/PLAN-cst.md` / `plans/RESULT-cst.md`.

## 12.9 宏展开 provenance

### 问题

预处理后的 token 知道自己"是什么", 不知道"从哪来". 诊断, 重构, 代码浏览器都需要来源.

### 设计

- 每次宏展开建一条 `MacroExpansion` 记录.
- 每个产物 token 挂 `MacroProv` 链.
- 链最内层在前, 只 prepend, 共享尾不可变.
- 记录调用点, 右括号, 宏定义, `#define` 名, 实参, 嵌套链.
- 公共面只暴露 11 个访问器和两个只读通道.

### 实现

- `MacroExpansion` / `MacroProv` 在 `preprocess.c`.
- 写入点: 内建宏, 对象宏, 函数宏, `subst` 的宏体与实参分支, 字符串拼接重扫.
- `prov` 链在 arena 里, 与 token 同生命周期.
- `-dump-expansions` / `-verify-expansions`.
- 29 个 dump 样本, 3 个链形状样本, 17 个 verify 样本, 全量语料校验.

### 产物

- provenance 链与访问器.
- `test/expand.sh`.

### 产物可以干什么

- 把错误映射回宏调用点.
- 分析宏使用.
- 源码浏览器展示调用与展开.
- 为将来的诊断展开点字段提供数据.

终态: a75de30, `plans/PLAN-provenance.md` / `plans/RESULT-provenance.md`.

## 12.10 库边界固化 (阶段 5)

### 问题

前五个阶段的产物都在一个程序里, 但还不能被第三方链接:

- 状态是文件域 static.
- 错误直接 `exit(1)`.
- 头文件包含内部类型.
- 没有 context, 没有 reset.
- 没有安装面.

### 设计

用户 2026-10-19 拍板的四件事:

- 库名 `libchibicc`, 公共头 `<chibicc/...>`, 符号 `chibicc_` / `Chibicc` 前缀.
- 只承诺源码兼容, 不承诺 ABI.
- 一个 context 一个翻译单元.
- 公共 `Node` / `Obj` 只保留第 3/4 层字段.

### 实现

L0 到 L4, 每步一提交:

- L1: codegen 字段移出公共 `Node` / `Type` / `Obj`, 进 codegen 私有表.
- L2: `ChibiccContext`, arena, 诊断回调 + longjmp, 19 个文件域 static 收进 context.
- L3.0: CST children 进 arena, reset 活字节闸门.
- L3.1: 9 个公共头, context 不透明, 头文件卫生.
- L3.2: `Chibicc*` 命名, `chibicc_reconstruct` 进库, D7 白名单, `.a` 内部符号策略.
- L3.3: `.a`, Linux `.so`, stage2 库对象, install, pkg-config, 版本.
- L3.4: 错误模型, reset, 双 context, ASan/UBSan.
- L3.5 / L4: 全量闸门, 文档.

修复的缺陷:

- `equal()` 的越界读.
- arena 8 字节对齐导致 `long double` 的 UBSan 报告.
- CST children 的裸 `realloc` 泄漏.

### 产物

- `libchibicc.a`, Linux `libchibicc.so.0`.
- 9 个公共头, 36 个公共函数.
- `ChibiccContext` 三态.
- `test/library.sh`, `test/public-api.sh`, `test/install.sh`.
- `make check-library`.

### 产物可以干什么

- 第三方可以链接, 重置, 注册诊断.
- 长驻进程可以按 TU 复用 context.
- 安装后可以用 pkg-config.

终态: 3e799e2, `plans/PLAN.md` / `plans/RESULT.md`.

## 12.11 Pratt 表达式解析 (附加线)

### 问题

表达式层的级联函数每个优先级一个, 重复度高, 修改一个运算符要动多个函数.

### 设计

- 增加一个只读绑定力表 `InfixOp`.
- 用一个 `expr_bp` 循环处理前缀, cast, 后缀, 二元, 赋值, `?:`, 逗号.
- 通过 `CHIBICC_PRATT_PARSE_FEATURE` 编译期开关控制.
- 默认构建保持不变.
- 两个模式的 AST 和汇编必须逐字节相同.

### 实现

- 先加 `-dump-ast` 和 `test/ast.sh` 作为形状锁.
- `expr_bp` 用绑定力和结合性表.
- `make chibicc-pratt` 在 out-of-tree `pratt/` 构建.
- `make test-both`, `test-ab`, `test-stage2-pratt`.
- `test/pratt-ab.sh` 对 41 个语料文件和 tinycc 真实构建比较 AST 与汇编.
- 后续修正: 逗号左结合, `infix_bp` 只读表, AST 锁定更新.

### 产物

- 可选的 Pratt 解析构建.
- A/B 测试工具.
- 两套构建的闸门.

### 产物可以干什么

- 实验新的表达式解析策略, 不影响默认构建.
- 作为"同一语义, 两种实现"的对照.

终态: c3d75fe, `plans/PLAN-pratt-parse.md` / `plans/RESULT-pratt-parse.md`.

## 12.12 收尾整理

阶段 5 和 Pratt 之后, 仓库做了三次不改变行为的整理:

- `267fb6f`: 库源码搬到 `libchibicc/src/`, 根目录只留驱动器和 codegen.
- `8976851`: 计划与执行记录搬到 `plans/`.
- `dd3e631`: 库内自测搬到 `test/self/`, 不再编进 `libchibicc.a`.

这些整理让目录结构与"库 + 参考消费者"的定位一致.

## 12.13 L5 产品化验证 (未开工)

这是原阶段 6, 2026-10-19 改为必选, 但目前没有开始.

计划内容:

- L5.1: CST formatter MVP, 只消费第 1/2 层, 对 `UNPARSED` 原样输出, 幂等, 编译等价, 注释数量不变.
- L5.2: CST 结构覆盖率统计与 typedef 供给.
- L5.3: AST 源码范围或 CST 映射; 诊断展开点字段.
- L5.4: 库文档与示例.
- L5.5: L5 闸门.

未完成意味着:

- API 还没有被真实工具验证.
- formatter 的可行性没有被证明.
- 源码映射和诊断展开点还缺.

这是当前最大的产品化风险, 不是隐藏信息. 采用者应该把它计入决策.

## 12.14 从阶段史看现在的代码

| 你看到的 | 来自哪个阶段 | 为什么 |
|---|---|---|
| `parse.c` 里有 typedef/tag 栈 | 语法语义拆分 | C 文法需要这个 oracle |
| `parse.c` 保留声明记录 | 忠实层 | 语义层消费记录, 语法层不建对象 |
| `sema.c` 没有降级 | codegen 直连 | 第 4 层只做标注 |
| `codegen.c` 有整形遍 | codegen 直连 | 需要槽/语句/标签的改写归后端 |
| 公共 `Node` 没有 codegen 字段 | 库边界 L1 | 公共 IR 只留源码和语义字段 |
| CST 单独一棵树 | CST 线 | 预处理不可逆 |
| `#if` 分支全保留 | CST 线 | 格式化需要未选中分支 |
| token 有 trivia 和 raw 坐标 | trivia 线 | 逐字节重建 |
| provenance 链 | provenance 线 | 宏来源不可从 AST 恢复 |
| `ChibiccContext` 不透明 | 库边界 L3 | 公共面不泄漏内部布局 |
| 36 个公共函数 | 库边界 D7 | 唯一的公共 API 清单 |
| Pratt 代码在 `#if` 下 | Pratt 线 | 默认构建不变 |

## 12.15 读历史文档的注意事项

`plans/` 下的文件是执行记录, 不是当前 API 文档:

- 行号, 函数名, 字段名可能已经变了.
- 当时的"未完成"可能现在已经完成.
- 当时的"临时"可能已经删除.
- 计划里的判据是当时的口径, 不一定等于今天的公共契约.

需要当前行为时, 以 `docs/`, 公共头和测试为准. 历史文档用于理解"为什么", 不用于查"是什么".
