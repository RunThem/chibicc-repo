# PLAN-docs-calibration.md - 按当前代码校准 docs 执行计划(已归档)

> 归档说明(2026-11-22): 本文件是已完成的 docs 校准线的执行计划, 随该线收官归档; 现行计划见 `PLAN.md`, 现行记录见 `RESULT.md`. 文中的步骤勾选状态与闸门记录是该线终态, 不再随新线更新.

状态: 已批准 (2026-11-21).

## 1. 目标与边界

- 一句话目标: 让 `docs/` 的 13 份文档与当前 `main` 的公共头, 实现, 构建系统, 测试脚本和 CI 一致; 过期计数, 字段路径, 章节描述, 缺失的 bench 与 2026-11 阶段记录全部修正, 并由 `test/docs-links.sh`, 全量测试与逐字节快照闸门锁定.
- 允许改动: `docs/*.md`; `plans/PLAN.md`; `plans/RESULT.md`; 归档上一线为 `plans/PLAN-agents-dedup.md` 与 `plans/RESULT-agents-dedup.md`; `/tmp` 下的临时快照与检查脚本.
- 非目标: 不改任何 C 源码, 公共头, `Makefile`, 测试脚本或 CI; 不修发现的实现缺陷; 不新增功能文档; 不改章节编号, 只新增 `10.8.1` 与 `10.10.1` 两个子节; 不做文风重写; 不检查外部 URL; 不把 `AGENTS.md` 再改一遍.
- 允许的文档链接目标: `docs/01-overview.md` 1.5 / 1.9 / 1.10, `docs/02-architecture.md` 2.4 / 2.5 / 2.6 / 2.9 / 2.10, `docs/03-layer1-token.md` 3.4 / 3.6 / 3.8, `docs/04-preprocess.md` 4.4 / 4.8, `docs/05-layer2-cst.md` 5.4 / 5.9, `docs/06-layer3-parse.md` 6.3 / 6.7 / 6.8, `docs/07-layer4-sema.md` 7.2 / 7.3 / 7.4 / 7.5, `docs/08-codegen.md` 8.5 / 8.6 / 8.8, `docs/09-api-reference.md` 9.3 / 9.4 / 9.7 / 9.9 / 9.11 / 9.12 / 9.14, `docs/10-build-install.md` 10.2 / 10.3 / 10.6 / 10.7 / 10.8 / 10.9 / 10.10 / 10.11, `docs/11-limitations.md` 11.5 / 11.7 / 11.8 / 11.9 / 11.11 / 11.12 / 11.14, `docs/12-project-stages.md` 12.2 / 12.5 / 12.10 / 12.15 / 12.16.

## 2. 用户拍板 (2026-11-21)

- 已确认: 用户要求"根据代码来更新 docs"; 事实来源是当前公共头, 实现, 测试与构建系统, 不是旧计划或旧文档.
- 已确认: 当前 `docs/README.md` 的代码基线是 `9b06d35` (2026-11-07), 该提交的 `LIBCP_VERSION` 还是 `0.1.0`, 没有 `libcp_node_family`; 其后至少 6 条线与公共 API, 分配模型, 节点布局, 构建目标有关, 文档未全量回写.
- 未决项: 无. "本线只改 docs, 实现缺陷只记录不修"随本计划一起批准; 如果用户要修代码, 另立计划.
- 待批准: 本计划的 14 个步骤, 范围边界, 每步闸门与全线验收标准.

## 3. 现状锚点

- 开工 HEAD: `c57acbe` (`Record the AGENTS and docs dedup line`).
- 宿主: `Linux` / `x86_64`, 原生路径; `uname -s` 为 `Linux`, `uname -m` 为 `x86_64`.
- 实测基线:
  - `make test-all`: 退出 0; 末尾为 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
  - `bash test/docs-links.sh`: `docs-links: ok (48 references checked)`.
  - `make format-check`: 退出 0.
  - `make check-context`: 退出 0; accessors 0, TLS 0, `cg_ctx` 0, accessor symbols 0.
  - `git status`: 计划写作前工作树干净.
- 汇编基线: 上一线在同一 commit `c57acbe` 记录 41 份 `.s` 与基线 diff 为空; 本线不改代码, S1 会重新生成并每步复用.
- 审计范围: 4 个只读审计并行覆盖 `docs/README.md` 与 01-12; 对照 10 个公共头, `libcp/src/`, `codegen.c`, `main.c`, `Makefile`, `bench/`, `tools/`, `test/*.sh` 与 `.github/workflows/`.
- 审计结果索引:

| 文件 | 主要问题 |
|---|---|
| `docs/README.md` | 基线 `9b06d35` 过期; 阶段 5 函数数写成 39; `fmt-tool` 构建范围不完整; 五分钟示例少列两个匿名全局 |
| `docs/01-overview.md` | 仍说 `LibcpNode` 不做 union; 把链表概括成全库集合实现; 根目录"只留三个文件"不成立; CST check 总数 309 过期 |
| `docs/02-architecture.md` | 第 3 层契约的 `ty_op` / `ty` 归属错误; 多处旧扁平字段; 错误模型漏 `expansion_verify` 的 stderr; 目录地图缺 `bench/` 与根级文件; `plans/` 描述过期 |
| `docs/03-layer1-token.md` | 把字符常量算进 `TK_PP_NUM`; `File.*` 短名; 原始 `\uXXXX` 内存输入的 byte-exact 重建不成立 |
| `docs/04-preprocess.md` | 把三类宏的 provenance 挂接都归给 `subst`; `#include_next` 描述成 per-file 栈, 实际是 context 全局索引 |
| `docs/05-layer2-cst.md` | 无初始化器声明也写成 `CST_INIT_DECLARATOR`; `#include_next` 在 CST 中落到 `CST_PP_DIRECTIVE` 未说明 |
| `docs/06-layer3-parse.md` | 解析器产出数写成 66, 实际 65; 不产出列表漏 `ND_NULL_EXPR`; 多个示例仍是扁平字段; `ND_IDENT.var` 不存在 |
| `docs/07-layer4-sema.md` | 把成员偏移写成 `LibcpType` 字段; 入口示例用旧字段; 字符串序列化描述错位; `add_type` 例外归属错; `layout_union` 描述过强; 字段表不完整 |
| `docs/08-codegen.md` | 六条不变量与实现的标签前序分配不符; 漏语句链过滤与 VLA 声明展开; 指针缩放归属过窄; 标签, 全局发射, `is_live`, `-g` 描述过期; 多处旧字段 |
| `docs/09-api-reference.md` | `tokenize_buffer` 的 NUL 行为写错; stdin 缓存未限定 reset; 预定义宏生命周期自相矛盾; source range 锚点不全; `cst_verify` 与 `context_*` 返回值行过宽; 隐藏返回缓冲区未说明; C++ `char *` 未说明 |
| `docs/10-build-install.md` | 完全没有 bench; 测试目标顺序与依赖不全; `check-library` 前置构建与 `check-context` 缺失; Docker 只读挂载描述对 thirdparty 不成立; 嵌入源码漏 `-DLIBCP_DEFAULT_ALLOCATOR`; 无 CI 章节 |
| `docs/11-limitations.md` | NUL 限制对 `tokenize_buffer` 不成立; stdin 缓存未限定 reset; 缺 `context_define` 的 provenance 名缺陷; `seed_typedef` 缓解未写; 旧字段; "字符串驻留"不存在 |
| `docs/12-project-stages.md` | 时间线停在 2026-10-25, 漏 c2c / benchmark / per-TU 分配 / 字段族 / docs 去重; arena 仍写成现行; CST check 总数 309 过期; `plans/` 描述过期 |

- 估计: 约 60 处编辑; 不新增文件, 不删章节; 所有改动可由 `git diff --stat c57acbe..HEAD` 限定在 `docs/` 与 `plans/`.

## 4. 设计决定

### D1 - 只改 docs, 不修实现

- 选择: 本线只写文档; 实现与公共头注释的偏离记录在 `docs/11-limitations.md`, 并在 `plans/RESULT.md` 的"后续影响"列出待另线处理的条目.
- 理由: 用户要求的是更新 docs; 修代码会改变可观察行为, 需要独立计划, 测试与提交边界.
- 影响: 文档可以准确描述当前行为, 但不会把缺陷写成新契约; 代码修复项留待用户拍板.

### D2 - 以公共头与测试为契约, 以实测为实现事实

- 选择: 公共头声明的契约保持为契约; 实现偏离处写成"当前实现"并链接 `docs/11`; 纯计数与结构事实以源码枚举和脚本提取为准.
- 理由: `docs/README.md` 已经规定"文档与头文件冲突时以头文件为准"; 但读者也需要知道当前实现哪里不可靠.
- 影响: `docs/09` 的 API 语义仍是规范文本; `docs/11` 增加"已知缺陷 (2026-11-21 实测)"条目.

### D3 - 保持章节编号稳定

- 选择: 不重编号 01-12; 只在 `docs/10` 新增 `10.8.1 前端 benchmark` 与 `10.10.1 CI 闸门`; 其他修正都在原章节内完成.
- 理由: 大量文档与 `AGENTS.md` 用"文件加章节号"引用; 重编号会制造新的失效引用.
- 影响: `test/docs-links.sh` 的 48 个引用基线不应大幅变化; 新增子节不改变父节号.

### D4 - 字段族写法统一

- 选择: 文档中的节点载荷统一写成 `node->ctrl.*`, `node->decl.*`, `node->value.*`, `node->case_.*`, `node->access.*`, `node->jump.*`, `node->assign.*`, `node->incdec.*`, `node->asm_.*`, `node->cas.*`; 公共头字段只写 `kind`, `next`, `ty`, `tok`, `lhs`, `rhs`, `ty_op`, `spec_decls`.
- 理由: 0.2.0 把 46 个平铺字段拆成十个族, 旧字段名在公共头中已不存在; 文档示例必须能编译.
- 影响: `docs/06` 的字段族表成为其他章节的参照; `docs/07`, `docs/08`, `docs/09`, `docs/11` 的旧路径统一修正.

### D5 - 计数用脚本提取, 不在正文手工维护多个副本

- 选择: 39 个公共函数, 57 个 `CST_*`, 69 个 `ND_*`, 65 个解析器产出, 47 个预定义宏, 313 次 CST check 这些数字先在 `/tmp` 用脚本或现有测试输出确认, 再写入文档; 同一数字只保留一处主陈述, 其他章节链接或简述.
- 理由: 本次审计发现的多数高严重度问题就是同一数字在多处不同步.
- 影响: 后续 API 变化时, 至少有一个可复现的计数命令.

### D6 - 已知缺陷的记录格式

- 选择: `docs/11` 的每条已知缺陷写四段: 观察到的行为, 与哪个头文件或文档契约冲突, 规避方式, 建议修复方向; 不写"以后会修"这类不可验收的承诺.
- 理由: 读者需要能判断当前版本能不能用; 修复方向留给另线计划.
- 影响: `plans/RESULT.md` 的后续影响能直接转成新计划的范围.

### D7 - 每步一个文件或一组同主题文件

- 选择: S2-S13 按文档文件分组, `docs/03` 与 `docs/04` 合并为一步, 其余每份文档一个提交; S1 只处理归档与基线, S14 只做全线校对与记录.
- 理由: 每步可独立审阅, 失败时可只回退该文件; 同主题合并避免 03/04 两条 provenance 描述分叉.
- 影响: 14 个提交, 每个提交都能跑完整闸门.

## 5. 步骤

### S1 - 归档上一线并建立本线与快照基线

- 改什么: 从 `HEAD` 取出现行 `plans/PLAN.md` 与 `plans/RESULT.md`, 写入 `plans/PLAN-agents-dedup.md` 与 `plans/RESULT-agents-dedup.md`, 文件头各加两行归档说明; 新建本线 `plans/RESULT.md`, 写入开工 HEAD, 宿主, 基线闸门与审计索引; 在 `c57acbe` 的临时 worktree 生成 41 份 `/tmp/libcp-docs-base-s/*.s`.
- 为什么: 上一线已完成并被 `c57acbe` 记录; 固定槽位要留给本线; 后续每步需要同一份汇编基线.
- 目的与效果: 旧线完整保留; 新 PLAN 与 RESULT 指向本线; 快照基线可复用.
- 如何测试: `test -f` 两个归档文件; `head -3` 检查归档说明; `wc -l` 与 `git show HEAD:plans/PLAN.md` 对比正文行数; `make test-all`; 生成当前快照并 `diff -ru /tmp/libcp-docs-base-s /tmp/libcp-docs-cur-s`.
- 验收标准: 归档文件存在且带归档说明; 新 PLAN 与 RESULT 标题正确; 41 份快照 diff 为空; 本步提交只含 `plans/`.
- 提交边界: 本步一个提交.

### S2 - `docs/README.md` 基线与状态

- 改什么: 基线改为 `c57acbe` (2026-11-21); 阶段 5 函数数改写为"阶段 5 时 36 个, L5 后 38 个, 0.2.0 后 39 个"; `tools/fmt.c` 说明改为"host 二进制由 `make fmt-tool` 构建, 自举测试另建 `stage2/fmt`, 都不进 `all` / `install`"; 五分钟示例循环加 `if (o->is_function)`, 预期输出只保留 `global: square, function=1`.
- 为什么: 基线提交的版本是 `0.1.0`, 且没有 `libcp_node_family`; 39 个函数不是阶段 5 当时的数字; `stage2/fmt` 确实会构建该消费者; 示例实际会多打印两个匿名全局.
- 目的与效果: 读者看到的基线, 函数计数与可运行示例都与当前代码一致.
- 如何测试: `git cat-file -e c57acbe`; `grep -n "0.2.0\|c57acbe\|36 个\|38 个\|39 个"`; 把五分钟示例复制到 `/tmp/demo.c`, `make libcp.a`, `cc -std=c11 -Ilibcp/include /tmp/demo.c libcp.a -o /tmp/demo`, 运行并逐行比对.
- 验收标准: 基线行只出现 `c57acbe`; 36/38/39 的时间线清楚; 示例编译并输出一行 `global: square, function=1`; `bash test/docs-links.sh` 通过.
- 提交边界: 本步一个提交.

### S3 - `docs/01-overview.md` 设计原则与成熟度

- 改什么: 1.5.6 删除"链表当集合, 查找 O(n)"的全库概括与"一个 `LibcpNode` 容纳所有字段, 不做 union"; 改写为"token / 对象 / 声明链仍用链表; 名字与 tag 作用域, 宏表, `pragma once`, include guard 用哈希表; `LibcpNode` 是公共头加十个字段族的匿名 union, `sizeof` 144 字节"; 同时把"解析期间不释放"改为"不显式调用 `LIBCP_FREE`, `realloc` 可能搬移或回收旧块, 对象级释放在 reset / free 边界". 1.9 把"根目录只留三个文件"改为"根目录的库外 C 源文件只有 `main.c`, `codegen.c`, `driver.h`". 1.10 的 309 改为 313, 或保留分项并写"共 313 次 check".
- 为什么: 字段族拆分和哈希表迁移已经改变了这两条设计事实; 根目录还有构建, 文档, 测试文件; CST check 总数与 `test/cst.sh` 的 48+48+30+130+1+56 不符.
- 目的与效果: 概览不再把已废弃的实现选择当成现行原则; 数字与测试一致.
- 如何测试: `! grep -n "不做 union" docs/01-overview.md`; `grep -n "313"`; `grep -n "库外 C 源文件"`; `bash test/docs-links.sh`.
- 验收标准: "不做 union" 消失; 313 出现且分项能相加; 根目录表述只限定库外 C 源; `docs-links` 通过.
- 提交边界: 本步一个提交.

### S4 - `docs/02-architecture.md` 层间契约, 生命周期, 错误模型与目录

- 改什么:
  - 2.4.4: 明确 `ND_TYPEDEF` 把声明类型放在 `ty`, `ND_CAST` / `ND_DECL` / `ND_GVAR_DECL` / `ND_FUNCDEF` / `ND_ENUM_CONST` 放在 `ty_op`.
  - 2.4.5: `var`, `val`, `generic_sel`, `is_implicit`, `init_resolved` 改成 `decl.var`, `value.val`, `ctrl.generic_sel`, `value.is_implicit`, `decl.init_resolved`.
  - 2.5.3: 把"解析期间不做任何释放"改为"不显式 `LIBCP_FREE`; `LIBCP_REALLOC` 可能搬移或归还旧块; 逐块释放在 `reset` / `free`".
  - 2.6.1 / 2.6.2: `libcp_expansion_verify` 直接写 stderr, 不受诊断回调影响; 默认处理器分 `verror_at` 的源码行加插入符与 `libcp_error` 的无位置行两种.
  - 2.9 / 2.10: 删除或修正"本章后面的例子"; `AGENTS.md 的开放决策一节` 改为 `AGENTS.md` 的 `已定边界`; 目录地图补 `bench/`, `docs/`, 根级 `Makefile` / `README.md` / `AGENTS.md` / `Dockerfile` / `libcp.pc.in`; `plans/` 改为"现行 `PLAN.md` / `RESULT.md`, 归档 `PLAN-*.md` / `RESULT-*.md`, 模板 `*-TEMPLATE.md`"; `ast.h` 条目补 `LibcpNodeFamily` 与 `libcp_node_family`; `allocator.h` 条目补"构建期开关, `LIBCP_DEFAULT_ALLOCATOR` 或全部四个宏, 只在重编库时生效".
- 为什么: 这些描述与 0.2.0 公共头, `lib.c`, `provenance.c`, 构建树和当前计划目录不一致; 目录地图漏掉已跟踪的 `bench/` 会让读者以为 benchmark 不存在.
- 目的与效果: 架构章节可以作为修改库的入口, 不再把读者引向不存在的章节或旧字段.
- 如何测试: `grep -n "开放决策"` 无命中; `grep -n "bench/"` 命中目录地图; `grep -n "decl.var\|value.val\|ctrl.generic_sel"` 命中; `grep -n "LIBCP_DEFAULT_ALLOCATOR"` 命中; `bash test/docs-links.sh`.
- 验收标准: 2.4 的类型字段归属与 `parse.c` 一致; 2.5.3 不再声称零释放; 2.6 明确 verify 例外; 2.10 包含 `bench/`, `docs/`, 根级构建文件与三态 `plans/`; 旧字段名在该节清零; `docs-links` 通过.
- 提交边界: 本步一个提交.

### S5 - `docs/03-layer1-token.md` 与 `docs/04-preprocess.md`

- 改什么:
  - 03 3.3.3 / 3.5: 说明 `TK_PP_NUM` 只覆盖 pp-number 形式的数值字面量; 字符常量在词法阶段就是 `TK_NUM`, 并带 `val` / `ty`.
  - 03 3.2.2 / 3.3.2: `File.*` 改为 `LibcpFile.contents` / `.raw` / `.map` / `.lines`.
  - 03 3.4.4 / 3.6 / 3.8: byte-exact 重建只对 `tokenize_file` 与已经处于扫描缓冲形态的 memory 输入成立; 原始 `\uXXXX` 的 memory 输入会让 `raw_end_of` 算出过长的 `raw_len`, 导致 `trivia_len` 为负, 见 `docs/11` 11.5.8; 给出规避: 先用 UTF-8 编码或走 `tokenize_file`.
  - 04 4.4.3: 函数式宏的宏体与实参 provenance 在 `subst` 挂接; 对象宏与内建宏在 `expand_macro` 直接 `new_prov`.
  - 04 4.4.2 / 4.8: `#include_next` 从 context 全局 `include_next_idx` 继续扫描, 不是 GNU 的 per-file include 栈; 重复 `#include_next` 可能再次命中同一目录.
- 为什么: 三处都是会误导消费者写错代码的行为描述; 字符常量和 UCN 是 token 层的可直接观察事实; `subst` 与 `include_next` 是实现结构.
- 目的与效果: token 与预处理章节的契约与当前实现一致, 特殊输入有明确规避方式.
- 如何测试: `grep -n "TK_PP_NUM"` 上下文只描述数值字面量; `grep -n "LibcpFile"`; `grep -n "subst"` 与 `grep -n "expand_macro"` 的归属正确; `grep -n "include_next_idx"`; `bash test/docs-links.sh`.
- 验收标准: 03 不再把字符常量写成 pp-num; 03 的 byte-exact 声明有输入形态限定; 04 的 provenance 挂接分三类; 04 的 `#include_next` 不再是 per-file 栈描述; `docs-links` 通过.
- 提交边界: 本步一个提交.

### S6 - `docs/05-layer2-cst.md` 声明形状与指令节点

- 改什么: 5.4.3 把"声明拆成 `CST_DECL_SPECS`, 一个或多个 `CST_INIT_DECLARATOR`"改为"一个或多个 `CST_DECLARATOR`; 有初始化器时该声明符包在 `CST_INIT_DECLARATOR` 中"; 5.4.4 增加一句 `#include_next` 在 `cst_pp_kind` 中没有专门分支, 因此落到 `CST_PP_DIRECTIVE`, 不是 `CST_PP_INCLUDE`; 5.9 的 typedef 限制补一句可用 `libcp_cst_seed_typedef` 预置 include 提供的关键字.
- 为什么: `cst.c` 只在看到 `=` 时创建 `CST_INIT_DECLARATOR`; `#include_next` 的节点种类与实际 dump 不符; `seed_typedef` 已是公共 API, 是现有缓解手段.
- 目的与效果: CST 形状描述可以直接用来写遍历器; `#include_next` 的节点种类不再让消费者意外.
- 如何测试: 用 `./chibicc -dump-cst` 对 `int x;` 与 `int y = 1;` 检查 `DECL` 子树; `grep -n "CST_PP_DIRECTIVE"`; `grep -n "seed_typedef"`; `bash test/docs-links.sh`.
- 验收标准: 无初始化器声明不再写成 `CST_INIT_DECLARATOR`; `#include_next` 的落点明确; `seed_typedef` 缓解存在; 57 种节点计数不变; `docs-links` 通过.
- 提交边界: 本步一个提交.

### S7 - `docs/06-layer3-parse.md` 计数, 字段路径与示例

- 改什么:
  - 6.3.1: `LibcpNodeKind` 总数 69 保持不变; 解析器实际产出改为 65; 不产出列表改为 4 种, 增加 `ND_NULL_EXPR` (第 5 层整形时产生); 解释 46 个字面量构造种类加 19 个 `infix_ops[].kind` 的计数方法.
  - 6.2.1 / 6.2.3 / 6.4.4: `name_tok`, `attr`, `decl_init` 改为 `decl.name_tok`, `decl.attr`, `decl.decl_init`; `Member.width_expr` 改为 `LibcpMember.width_expr`.
  - 6.7.2: `n->name_tok` / `n->body` 改为 `n->decl.name_tok` / `n->decl.body`; 6.7.4: `n->decl_init` 改为 `n->decl.decl_init`.
  - 6.8: `ND_IDENT.var` 改为"`ND_IDENT` 不携带绑定字段; 第 4 层把变量/函数引用改写成 `ND_VAR`, 绑定在 `decl.var`; 枚举常量改写成 `ND_NUM`, 值在 `value.val`"; 诊断计数写明"`test/diagnostic.sh` 的 A 组 15 例"或当前 21 个调用点 / 17 条消息.
- 为什么: 字段族拆分后示例无法编译; 解析器产出计数与 `parse.c` 的构造点不符; `ND_IDENT` 从来没有 `var` 字段.
- 目的与效果: 第 3 层章节的示例可以编译; 计数可复现; 读者不会去访问不存在的字段.
- 如何测试: 把 6.7.2 与 6.7.4 的示例复制到 `/tmp`, 用 `cc -fsyntax-only -std=c11 -Ilibcp/include` 验证; 用 `grep` 统计 `parse.c` 的 `libcp_new_*` 字面量种类与 `infix_ops` 的 kind; `! grep -n "66 种"`; `bash test/docs-links.sh`.
- 验收标准: 69 与 65 的口径分开写清; `ND_NULL_EXPR` 在不产出列表; 06 中不再有裸 `name_tok` / `body` / `decl_init` / `ND_IDENT.var`; 示例通过语法检查; `docs-links` 通过.
- 提交边界: 本步一个提交.

### S8 - `docs/07-layer4-sema.md` 字段, 布局与序列化

- 改什么:
  - 7.2.1: `LibcpType.size` / `align` / `members` 保留; `offset` / `bit_offset` 改到 `LibcpMember.offset` / `LibcpMember.bit_offset`.
  - 7.3.2: 标题改为"关键字段"或补一行"其余字段": `LibcpType` 的 `name`, `name_pos`, `tag_tok`, `dim_len`, `typeof_expr`, `align_expr`, `vla_len`, `layout_pending`, `spec_decls`, `next`; `LibcpMember` 的 `next`, `tok`, `width_expr`.
  - 7.4.1: 示例 `n->body` / `n->var` 改为 `n->decl.body` / `n->decl.var`.
  - 7.4.4: `layout_union` 改为"取最大成员的大小与对齐; 位域按其底层类型大小参与; 匿名成员作为普通成员参与", 不再声称有专门分支.
  - 7.4.5: "例外是 `ND_IDENT` / `ND_STRING` 绑定改写"改为"绑定改写发生在 resolve pass; `libcp_add_type` 的 `ND_STRING` 分支只物化匿名全局并设置 `decl.var`, 不改 kind".
  - 7.4.6: 区分 `new_string_literal` 物化匿名全局对象与 `serialize_gvar` 写重定位; `char *p = "abc"` 写 `LibcpRelocation`, `char a[] = "abc"` 才逐元素写数组字节.
  - 7.5: `Obj.init_data` 改为 `LibcpObj.init_data`; `ND_SIZEOF.val` / `ND_GENERIC.generic_sel` 改为 `ND_SIZEOF` 的 `value.val` / `ND_GENERIC` 的 `ctrl.generic_sel`.
- 为什么: 这些字段归属与 0.2.0 `ast.h` 和 `sema.c` 的序列化路径不符; 读者按旧描述会访问不存在的成员或误判字符串初始化.
- 目的与效果: 第 4 层章节可以直接对照公共头; 字符串与布局行为的描述可以指导消费者.
- 如何测试: `grep -n "LibcpType.*offset\|LibcpType.*bit_offset"` 无命中; `grep -n "n->body\|n->var\|Obj.init_data"` 无命中; 用 `cc -fsyntax-only` 检查改过的示例; `bash test/docs-links.sh`.
- 验收标准: 成员偏移只出现在 `LibcpMember`; 字段表补全或明确为关键字段; 示例可编译; 字符串序列化区分两种路径; `layout_union` 不再声称专门处理位域/匿名成员; `docs-links` 通过.
- 提交边界: 本步一个提交.

### S9 - `docs/08-codegen.md` 整形遍与发射

- 改什么:
  - 8.5.1: 六条不变量按实现改写: (a) 表达式与声明改写后序; (b) 循环与 `switch` 在进入子树前分配标签, 是前序; (c) 每函数上下文; (d) 不重入; (e) 嵌套函数只访问一次; (f) 初始化器走 `shape_init_exprs`. 增加一句 `codegen.c` 文件头的 (a)/(b) 注释与实现不一致, 见 `docs/11` 或 RESULT 后续项, 不在本线改源码.
  - 8.5.2: 表内 `op != 0` 改 `assign.op`; `arrow_tok` 改 `access.arrow_tok`; elvis 改 `ctrl.is_elvis`; `case` 链改 `case_`; 后缀 `++` 的"减一"改为"用 `new_add(..., -addend)` 反向加减并 cast"; 增加两行"声明记录链过滤"与"VLA 声明展开"; `goto` 行补 `ND_LABEL_VAL`.
  - 8.5.3: 指针缩放改为"普通 `ND_ADD` / `ND_SUB` 与 `ND_SUBSCRIPT` 在发射点缩放; `op=`, `++`, `--` 在整形遍通过 `new_add` / `new_sub` 缩放".
  - 8.5.4: 标签名改为"整形遍标签 `.L..N`, 基数为 `libcp_unique_name_next(ctx)`, 之后由 codegen 局部 `label_seq` 递增; 发射器另有 `.L.begin` / `.L.else` / `.L.end` / `.L.false` / `.L.true` / `.L.return` 与数字局部标签".
  - 8.5.5: "私有指针键表"改为"4096 桶, 以指针为键的私有哈希表".
  - 8.6.1: 全局变量分三类: 初始化定义输出对齐, 标签与字节; 暂定定义走 `.comm`; 未初始化定义走 `.bss` / `.tbss` 的 `.zero`.
  - 8.6.2: `emit_text` 增加 `is_live == false` 的 static inline 过滤; `new_add` / `new_sub` 分开描述, 不再说两者做同样三件事.
  - 8.6.4: `-g` / `-O` / `-W` 是接受但忽略, 不生成调试信息; 仍发射 `.file` / `.loc`.
  - 8.8.1: 增加字段族与 `libcp_node_family` 的说明; 忠实节点清单补 `ND_LABEL_VAL`.
- 为什么: codegen 在 2026-11 有较大重构, 但文档仍按旧的"先全部表达式后全部标签"和"整形遍不缩放"描述; 第三方后端会按错顺序实现.
- 目的与效果: 第 5 层章节与实际 pass 顺序, 字段布局, 发射分支一致; 读者能据此写自己的后端.
- 如何测试: 对照 `codegen.c` 的 `shape_node` / `shape_chain` / `expand_decl` / `new_inc_dec` / `emit_text`; `grep -n "assign.op\|access.arrow_tok\|ctrl.is_elvis"` 命中; `grep -n "is_live"` 命中; `bash test/docs-links.sh`.
- 验收标准: 六条不变量与实现一致且明确说明前序标签; 8.5.2 覆盖链过滤与 VLA 展开; 指针缩放归属正确; 标签, 全局发射, `is_live`, `-g` 描述正确; 旧字段清零; `docs-links` 通过.
- 提交边界: 本步一个提交.

### S10 - `docs/09-api-reference.md` API 语义与示例

- 改什么:
  - 9.3.3: `reset` 清理列表补 stdin 缓存.
  - 9.3.4: 诊断回调说明加"`libcp_expansion_verify` 的违规报告直接写 stderr, 是例外".
  - 9.3.7 / 9.9.2: `libcp_context_define` 定义的对象宏, 当前 `libcp_macro_expansion_macro` 返回 `NAME=body`, 不是 `NAME`; 源码 `#define` 正常; 指向 `docs/11` 11.7.8.
  - 9.3.10 / 9.12: 预定义宏的静态 `name` 与固定 `value` 不随 reset 失效; 只有 per-TU 构造的 value 与 `predefined` 数组失效; 把"reset 后一切失效"改为带此例外.
  - 9.4.1: stdin 缓存限定为"同一 TU, 即 reset 之前"; reset 后再次读 stdin 会得到空流, 因为缓存已清.
  - 9.4.2: `libcp_tokenize_buffer` 用 `strlen` 取长度, 内嵌 NUL 会静默截断; 要检测 NUL 用 `libcp_tokenize_buffer_len`; 9.11 表同步.
  - 9.6.5 / 9.11: `libcp_cst_verify` 普通失败返回 false, 不置 failed; 只有内部错误边界才可能 false + failed.
  - 9.7.2: source range 还收集 `decl.name_tok`, `access.arrow_tok`, `case_.colon_tok` 与 `spec_decls`; 子树没有 token 时 `*start` / `*end` 为 NULL; `spec_decls` 按 `ast.h` 的措辞描述.
  - 9.11: `libcp_context_*` 行限定为 void setter; 查询函数有返回值.
  - 9.14.3: 参数遍历可能遇到隐藏返回缓冲区 (大 struct/union 返回值), 示例过滤空名参数或说明多一个参数.
  - 9.14.8: C++ 侧 `char *` 参数不能直接用字符串字面量, 需要可写缓冲区或显式 cast.
- 为什么: 这些是公共 API 的直接语义, 写错会让消费者做出错误的错误处理或生命周期判断; `tokenize_buffer` 的 NUL 与 `context_define` 的 provenance 名已经实测.
- 目的与效果: 09 章成为可依赖的 API 参考; 与头文件契约冲突的实现行为明确标为缺陷.
- 如何测试: 用 `test/library.sh` 覆盖的基本路径; 对 NUL 与 `context_define` 写一次性 `/tmp` 消费者验证当前行为; `grep -n "reset 后一切失效"` 只保留带例外的版本; `bash test/docs-links.sh`.
- 验收标准: 09 的每条 API 语义与头文件或实测一致; `tokenize_buffer` NUL 与 stdin 生命周期不再误导; `context_define` 缺陷有链接; `cst_verify` / `context_*` 返回值行正确; source range 锚点完整; 示例可编译; `docs-links` 通过.
- 提交边界: 本步一个提交.

### S11 - `docs/10-build-install.md` 构建, 测试, bench 与 CI

- 改什么:
  - 10.2: 编译选项块补 `BENCH_CFLAGS` 与 `bench/obj`, `bench/libcp_bench.a`, `bench/libcp-bench` 产物.
  - 10.3: 目标表补 `bench-tool` 与 `bench`; `fmt-tool` 行注明产出二进制 `fmt`; `format` 行补"版本低于 20 时失败"; `test` 行改为"完整测试套件, 不含 check-library / thirdparty / bench".
  - 10.4: 安装布局注明都位于 `$(DESTDIR)$(PREFIX)` 下.
  - 10.6.1: "全部外部符号"改为"全部已定义全局符号"; 10.6.2 的 `nm -D` 改为 `nm -D --defined-only`.
  - 10.7: 自举测试清单补 `test/self-test.sh`; 不跑列表补 `test/context-audit.sh` 与 `test/docs-links.sh`; 说明 `stage2/fmt` / `stage2/c2c` 由宿主 `cc` 链接 `stage2/libcp.a` 构建.
  - 10.8: 说明表按类别列出, 不表示执行顺序; `test/docs-links.sh` 是 `make test` 第一条; 补 `$(MAKE) fmt-tool` 与 `$(MAKE) c2c` 两个构建步骤; `test/fmt.sh` 的 `-S` 等价改为"有系统头时执行, 容器内为权威, 原生 x86-64 Linux 也执行"; 新增 10.8.1 前端 benchmark, 写 `make bench-tool`, `make bench`, `bench/run.sh`, `BENCH_REPEAT` 默认 5, 四组语料, reset / tokenize / preprocess / parse / sema / total 时间与 alloc/calls/peak, Linux-only, 独立 `BENCH_CFLAGS`, 不进入 `all` / `test` / `install`, CI 只跑 `bench-tool`.
  - 10.9: `check-library` 前置构建 `libcp.a`, Linux 下的 `libcp.so` 与 `libcp.pc`; 补 `check-context` 段落; `test/library.sh` 补共享库消费者与 64 次 CST+reset 的 32 KiB 活字节上界.
  - 10.10: Docker 只读挂载限定为 `docker-test` 与 `docker-snapshot*`; `docker-test-thirdparty` 读写挂载并回写 `thirdparty/` 克隆缓存; 补本机 `make test-thirdparty`; 新增 10.10.1 CI 闸门, 按 `test.yml`, `format.yml`, `docs.yml` 列出 job, 说明 CI thirdparty 只跑 tinycc/libpng/git/sqlite, cpython 只在手工 Docker 闸门.
  - 10.11.3: 嵌入源码补 `-DLIBCP_DEFAULT_ALLOCATOR` 或四个 `LIBCP_*` 宏, 说明 `libcp/src/libcp.h` 定义 `LIBCP_INTERNAL`.
  - 10.11.5: `test/allocator.sh` 只验证替换机制, 自定义分配器需自己的 shim 或链接参数.
- 为什么: bench 线完全没有进入 docs; 测试顺序与依赖已经变化; 嵌入方式按文档操作会直接 `#error`; Docker 挂载描述对 thirdparty 不成立; CI 是当前构建事实的一部分.
- 目的与效果: 10 章覆盖构建, 安装, 测试, bench, Docker 与 CI 的当前入口; 读者可以按文档复现.
- 如何测试: `grep -n "bench"` 在 10 章命中; `make bench-tool`; `make bench` 的 Linux 守卫可单独运行 `bash -n bench/run.sh`; 对照 `Makefile` 的目标与依赖; `nm -D --defined-only libcp.so | wc -l`; `bash test/docs-links.sh`.
- 验收标准: bench 目标与测量项完整; 10.3 表与 `Makefile` 一致; `check-library` / `check-context` 有前置与正文; Docker 挂载描述分目标; CI 章节存在; 嵌入命令可编译; `docs-links` 通过.
- 提交边界: 本步一个提交.

### S12 - `docs/11-limitations.md` 限制与已知缺陷

- 改什么:
  - 11.5.6: stdin 缓存限定为"同一次 reset 周期内"; reset 清空缓存.
  - 11.5.7: 拆成文件与 `_len` 会报 NUL, `libcp_tokenize_buffer` 用 `strlen` 会静默截断; 11.14 表加一行.
  - 11.5.8 (新增子节): 原始 `\uXXXX` 的 memory buffer 会破坏 `raw_len` / `trivia_len` 与 byte-exact 重建; 规避方式是先转 UTF-8 或走 `tokenize_file`; 建议修复 `raw_end_of` 或统一扫描缓冲.
  - 11.7.8 (新增子节): `libcp_context_define` 定义的对象宏, provenance 的 `macro` 名返回 `NAME=body`; 源码 `#define` 正常; 规避是不要依赖 `-D` 宏的 `macro` 名; 建议在 `add_macro` 复制 name.
  - 11.8.2: 补 `libcp_cst_seed_typedef` 作为 include 提供 typedef 的缓解.
  - 11.9.1: `ND_IDENT.var` 改为"`ND_IDENT` 无绑定字段; `ND_VAR` 的绑定在 `decl.var`".
  - 11.9.3: 标题 `init_resolved` 改为 `decl.init_resolved`.
  - 11.11.3: "参数不是 const"改为"字符串类入口的参数是 `char *`, 其他参数仍有 const 形式".
  - 11.11.5 (新增子节): `context.h` 关于预定义宏字符串会被 reset 失效的注释与实现不一致; 当前静态 name/value 不失效; 以 docs/09 9.3.10 与 9.12 为准.
  - 11.12: 删除"字符串驻留", 改为"字符串复制与存储".
  - 11.14: 增加 `tokenize_buffer` NUL 截断, `-D` provenance 名, reset 后 stdin 缓存失效, UCN raw 坐标错位四行.
- 为什么: 限制章节是消费者判断可用性的地方; 当前有三条实测行为与文档不符, 还有两条旧字段/不存在的机制.
- 目的与效果: 11 章准确列出当前版本的坑; 每条都有规避方式; 代码修复项可转入后续计划.
- 如何测试: 逐条对照 `lib.c`, `tokenize.c`, `preprocess.c`, `context.h`; 用 `/tmp` 消费者复现 NUL 截断, `-D` 宏名, reset 后 stdin; `grep -n "字符串驻留"` 无命中; `grep -n "decl.init_resolved"` 命中; `bash test/docs-links.sh`.
- 验收标准: 四条新缺陷有观察行为, 冲突契约, 规避与建议; 旧字段清零; `seed_typedef` 缓解存在; 11.14 表覆盖; `docs-links` 通过.
- 提交边界: 本步一个提交.

### S13 - `docs/12-project-stages.md` 时间线与阶段事实

- 改什么:
  - 12.2 时间线补: 2026-10-26 c2c 回归闸门 (`plans/RESULT-c2c.md`); 2026-11-01 类型名统一 `Libcp*` (`53785b9`); 2026-11-10 benchmark (`plans/RESULT-benchmark.md`, `7dc2126`); 2026-11-19 per-TU 分配替换 arena (`b70287e`, `plans/RESULT-alloc.md`); 2026-11-21 `LibcpNode` 字段族 (`505d0bf`, `plans/RESULT-node-families.md`); 2026-11-21 AGENTS/docs 去重 (`c57acbe`, `plans/RESULT-agents-dedup.md`).
  - 12.10: L2 与 L3.0 的 arena 描述标注"当时实现"; 增加"arena 已于 2026-11-19 被 per-TU 分配替换"; "arena 8 字节对齐"缺陷标注为历史, 当前用 `_Alignof(max_align_t)`.
  - 12.5: 309 改为 313, 61/65 dump 样本的口径与 `test/cst.sh` 一致.
  - 12.15: 增加"字段族 union / `libcp_node_family`"与"per-TU 分配替换 arena"两行; 保留 39 个函数行.
  - 12.16: `plans/` 描述改为"现行 `PLAN.md` / `RESULT.md`, 归档 `PLAN-*.md` / `RESULT-*.md`, 模板 `*-TEMPLATE.md`".
- 为什么: 阶段史是第三方判断当前代码为什么长这样的入口; 时间线停在 2026-10-25 会让读者以为 benchmark, per-TU 分配与字段族没有发生; arena 仍被写成现行设计.
- 目的与效果: 12 章覆盖到当前 HEAD, 历史与现状的时态清楚.
- 如何测试: `grep -n "2026-11"` 命中六条; `grep -n "arena"` 的每处都有"当时"或"已替换"限定; `grep -n "313"`; `grep -n "PLAN-TEMPLATE"`; `bash test/docs-links.sh`.
- 验收标准: 时间线含 2026-11 五条以上; arena 不再被写成现行; 309 清零; 12.15 含字段族与分配模型; `plans/` 三态描述正确; `docs-links` 通过.
- 提交边界: 本步一个提交.

### S14 - 全线一致性校对与 RESULT 收尾

- 改什么: 对全 `docs/` 做一次交叉引用与计数 sweep; 修正遗漏的旧字段, 旧数字, 旧章节名; 在 `docs/12` 时间线追加本线 (终态 `plans/RESULT.md`); 补齐 `plans/RESULT.md` 的提交表, 各步记录, 验收对照, 偏差, 后续影响与闸门实测; 记录三条代码缺陷与 `codegen.c` 文件头注释不同步作为后续项.
- 为什么: 单文件步骤通过后仍可能有跨文件不一致; 本线需要一份可审计的收尾记录.
- 目的与效果: 所有文档对同一事实只有一个口径; RESULT 可以直接作为下一线的输入.
- 如何测试: 运行第 6 节全线闸门; 用 `/tmp` 脚本检查 `39`, `57`, `69`, `65`, `47`, `313`, `68` 的主陈述; `! grep -rn "不做 union\|66 种\|309\|\bObj\.\|File\." docs`; `git diff --check`; `git diff --stat c57acbe..HEAD`.
- 验收标准: 全线闸门全绿; 旧字段与旧计数清零; `docs-links` 通过; `git diff` 只含 `docs/` 与 `plans/`; RESULT 完整且包含三条缺陷后续项.
- 提交边界: 本步一个提交.

## 6. 闸门

### 每步闸门

- `bash test/docs-links.sh`: 输出 `docs-links: ok (...)`.
- `make test-all`: 退出 0; 末尾必须出现 `c2c lock: 81 golden samples byte-exact and idempotent, 65 node kinds covered`.
- 汇编快照: 用 S1 的 `/tmp/libcp-docs-base-s` 与当前树重建的 `/tmp/libcp-docs-cur-s` 做 `diff -ru`, 必须为空; 当前树重建命令沿用上一线: 对 `test/*.c` 逐个 `./chibicc -S <file> -o /tmp/libcp-docs-cur-s/<name>.s`.
- `git diff --check`: 通过.
- 代码零改动: `git diff --stat c57acbe..HEAD -- ':!docs' ':!plans'` 为空.
- 针对性检查: 每步"如何测试"列出的 grep, 编译或运行时检查.

### 全线闸门

- `make test-all`.
- `make format-check`.
- `make check-context`.
- `bash test/docs-links.sh`.
- 41 份 `.s` 快照 diff 为空.
- `git diff --check`.
- `git diff --stat c57acbe..HEAD` 只含 `docs/` 与 `plans/`.
- 跨文档计数脚本通过: 39 个公共函数, 57 个 `CST_*`, 69 个 `ND_*`, 65 个解析器产出, 47 个预定义宏, 313 次 CST check, 68 例诊断.
- 旧口径 grep 清零: `不做 union`, `66 种`, `309`, 裸 `Obj.` / `File.`, `ND_IDENT.var`, `字符串驻留`.

## 7. 风险与回退

- 风险: 文档链接或章节引用被改坏. 缓解: 每步跑 `test/docs-links.sh`; S14 再人工检查"第 X 章"与 `docs/xx.md x.y` 引用.
- 风险: 把实现缺陷写成新契约. 缓解: D2 与 D6; 契约文本留在 09, 缺陷文本进 11, RESULT 列后续项.
- 风险: 计数改了正文但漏了另一处副本. 缓解: D5 的脚本 sweep; S14 的 grep 清单.
- 风险: 纯文档改动仍跑全量测试耗时. 缓解: 接受该成本; 快照基线只生成一次; 每步复用.
- 风险: 归档上一线时丢失正文. 缓解: 从 `git show HEAD:plans/PLAN.md` 与 `HEAD:plans/RESULT.md` 生成归档; S1 提交前逐行比较.
- 回退点: S1 的归档提交; 单文件步骤失败时只回退该步, 不影响其他文档.

## 8. 执行流程

1. 计划写入 `plans/PLAN.md` 并展示全文, 停下等待明确批准.
2. 批准后按 S1-S14 推进, 一步一提交; 每步先跑该步闸门, 通过后再提交.
3. 每步提交后立即更新 `plans/RESULT.md`, 记录实际改动, 命令, 结果与偏差.
4. 范围, 设计或验收标准变化时, 先更新 `plans/PLAN.md` 说明原因, 再次请求批准.
5. 收线时补齐 RESULT 的验收表, 偏差与后续影响.

## 9. 计划书自检

- 每步写全六项: 改什么, 为什么, 目的与效果, 如何测试, 验收标准, 提交边界.
- 每步能独立提交并通过该步闸门; S2-S13 每步只碰一份或一组同主题文档.
- 不把"优化", "整理", "完善"这类无法验收的词单独作为一步; 所有步骤都有可判定的 grep, 编译或 diff.
- 本计划不修改代码; 发现的实现缺陷有明确记录位置与后续项.
