# PLAN-c2c.md - C→C 打印器线 (第二个消费者, 第 3 层)

> 状态: 执行中 (2026-10-25 用户拍板开工)
> 执行记录见 plans/RESULT-c2c.md
> 基线: main @ 43a718f
> 范围: 新增 tools/c2c.c (C→C 打印器) + test/c2c.sh + Makefile 布线 + 库内一处追加 (LibcpType.tag_tok) + plans/PLAN-c2c.md / RESULT-c2c.md + AGENTS.md 收官更新
> 回退: 每步单提交, 任何一步可 revert 回前一步; 库改动仅一个追加字段, 无行为耦合
> 拍板 (2026-10-25, AskUserQuestion): (1) tag 库内记录 (LibcpType 加 tag_tok, 不做工具侧合成名); (2) 自举语料 (main.c + codegen.c + libcp/src/*.c) 作为 -S 等价门的必过项; (3) 注释丢弃 (自造空白, 注释保真归 fmt)。

## 1. 目标与边界

### 1.1 背景
L5 完成后用户选定 C→C 打印器为下一条线 (AGENTS.md 路线图 6): 第二个真实消费者, 验证第 3 层 (忠实语法 AST) 作为库产品的可消费性; 三个 L5 遗留项 (trivia 分段迭代器 / range 语义 / 残余 UNPARSED 收编) 与 per-node user data 决策随本线凭证据评估 (plans/RESULT-l5.md 第 9 节)。

### 1.2 决策的关闭与时机
- 本线消费第 3 层: tokenize → preprocess → parse → 打印, **不调用 libcp_sema** (与 AGENTS.md "C→C codegen 消费 3" 一致)。sizeof 不折叠, _Generic 全关联保留, 隐式 cast 不存在, while/op=/自增自减等写法区分全部在。第 4 层打印 (sema 后, 常量已折叠) 是另一个产品, 不在本线。
- 宏保真沿用 2026-10-18 已决: 消费展开后的流, 输出是单一展开后翻译单元 (宏与 include 不复原), provenance 标注输出 (--annotate) 不在本线。
- 三遗留项评估在 CC7 落账, 不提前擅动公共面。

### 1.3 边界 (允许改的文件)
- 新增: tools/c2c.c, test/c2c.sh, plans/PLAN-c2c.md, plans/RESULT-c2c.md。
- 修改: libcp/include/libcp/ast.h (加一个 LibcpToken *tag_tok 字段, 源码兼容追加, D7 函数面不变), libcp/src/parse.c (struct/enum 说明符 set tag_tok, 约 4 行), Makefile (c2c / stage2/c2c / test 布线, 照抄 fmt 模式), AGENTS.md (收官), test/fmt.sh 不动。
- 驱动器与库其余零改动。

### 1.4 非目标
- 不做注释保留与 trivia 消费 (拍板 3); 不做行保持 (输出为规范化重排); 不做 #line 重发射 (line.c 因此成为 -S 门例外, 记录理由)。
- 不修 const/volatile 丢失 (chibicc 把它们当 no-op, IR 本来就没有, 记为已知缺口); asm 的 inline/volatile 限定符若 IR 未记录则打印裸 asm (chibicc 发射无差别, 记缺口); 类型无关的 GNU 属性丢弃, packed/aligned 从 IR 重合成到规范位置。
- 不做改写: 打印器只读树 + 自建 side table, 不改任何节点。

## 2. 判据 (完成定义)
1. **golden**: test/c2c.sh 的片段样本逐字节锁定打印输出, 覆盖全部 69 个 NodeKind 与全部 18 个 TypeKind 及棘手 declarator (函数指针/数组/嵌套括号)。
2. **幂等**: 对全部样本与语料, c2c(c2c(f)) == c2c(f) 逐字节成立。
3. **归一化 AST 等价**: 对 41 个 test/*.c (+ 自含头文件), -dump-ast(原文) 与 -dump-ast(打印产物) 剥离 `@span` 锚点后逐字节相等 (证明打印产物再解析得到同一棵第 3 层树)。
4. **-S 逐字节等价** (docker, 权威): 41 个 test/*.c 与自举语料 (main.c, codegen.c, libcp/src/*.c) 按 fmt.sh 模式 (原地覆盖 + 三时间宏钉死) 汇编逐字节相等; line.c 例外并记录。
5. **库零行为变化**: 既有全部门 (docker-test 含自举与 stage2, 快照空 diff, ast.sh/diagnostic.sh/shape.sh, check-library, check-context) 全绿。
6. **决策落账**: RESULT-c2c.md 记录三遗留项证据与 user data 决策输入, AGENTS.md 同步。

## 3. 现状锚点 (2026-10-25 实测, main @ 43a718f)
- 公共面: ast.h 551 行 (69 NodeKind, 18 TypeKind, 忠实初始化器记录三形态), token.h 公开 loc/len (拼写, 宏产物也正确), context.h 公开 add_include_path / define / undef / set_diagnostic / failed - **工具零新增 API 可落地**。sema.h 仅 15 行。
- -dump-ast: main.c:678-1102, 逐 kind 遍历 (parse 后 sema 前早退), 是打印器遍历结构的参考实现。
- 消费者骨架: tools/fmt.c 240 行 (argv/诊断/ctx 生命周期/emitter 状态全在遍历者手里); Makefile:67-69 (fmt 目标), 86-87 (test 拉起), 127-129 (stage2), 173 (clean)。
- 第 3 层关键形态 (探针实测): ND_GENERIC 控制表达式在 cond, 关联链在 args (parse.c:1497-1523); ND_ENUM_CONST 带名字 token/可选值 lhs/ty_op, 链上或 spec_decls (parse.c:693-746); **enum Type 是裸记录 (无 tag 无常量表), struct Type 有 members + layout_pending, tag 都只在 parser 作用域 (parse.c:1380-1427), 前向声明不产生链节点**; ND_TYPEDEF 类型在 node->ty (parse.c:1976-1999); 数组维度/typeof/位宽/对齐以未求值表达式挂在 Type (dim_len / typeof_expr / align_expr) 或 Member.width_expr; attr 携带存储类与 _Alignas。
- 本机 (macOS) 无法预处理含系统头的语料: test/expand.sh:394-403 的 -E 探测分流模式照抄。
- 仅 test/line.c 依赖 __LINE__/__FILE__ (#line 重设行号, 打印产物必然错位)。

## 4. 设计决策
- **D1 消费层**: 第 3 层, 见 1.2。
- **D2 tag 库内记录** (拍板 1): LibcpType 加 `LibcpToken *tag_tok`; parse.c 的 struct_or_union_specifier 前向分支/定义分支与 enum_specifier 各 set 一处; 重定义路径 `*ty2 = *ty` 值拷贝天然保留。使用点经 find_tag 拿到的共享记录自带 tag。dump 不打印它, 既有锁定零感知。
- **D3 输出契约** (拍板 3): 单一展开后 TU, 注释丢弃, 空白全部自造, 2 空格缩进; 一声明一行, 语句一行一个, 子语句非块不加花括号 (加块会产生 ND_BLOCK, 破坏判据 3)。
- **D4 拼写保真**: 一切字面量与标识符从 node/token 的 loc/len 取原拼写 (0x 形式, 字符常量, 后缀, 字符串编码前缀, 宏产物拼写); 布局外不改任何字节。
- **D5 typedef 名恢复** (工具侧): 预遍历收集 ND_TYPEDEF 的 (Type* → 名字), 仅对非基本类型单例的记录建表 (ty_int 等单例会毒化全文件); 使用点查表优先, 查不到走结构化展开。per-type side table 是 user data 决策的直接证据。
- **D6 枚举重组**: 链上/spec_decls 里连续且共享 ty_op 的 ND_ENUM_CONST 重组回 `enum <tag> { A = expr, ... };` (值表达式未求值, 原样打印); 使用点打 `enum <tag>`。依赖 D2。
- **D7 不完整类型**: size == -1 且 members 为空的记录, 首次使用前发射 `struct <tag>;` 前向声明 (依赖 D2 的 tag); 已完成的记录在使用点打 `struct <tag>` (定义已按链序先行)。
- **D8 括号化**: 优先级与结合性驱动的最小括号 (表达式一侧按二元/前缀/后缀/转型/条件的绑定力, 声明一侧按指针-函数-数组推导链规则); 括号不产生节点, 多余括号对判据 3 无害, 以最小为准。
- **D9 验证哲学**: golden 锁形态, 幂等锁布局确定性, 归一化 AST 等价锁语义 (纯前端, 本机可跑自含部分), -S 逐字节锁发射 (docker 权威), 自举语料锁真实代码 (拍板 2)。

## 5. 步骤 (CC0-CC7)
每步: 改动 → 本机闸门 → docker 闸门 → 提交。

- **CC0 计划落盘**: 写 plans/PLAN-c2c.md (本文); 基线锚点复核。提交 "Add the C-to-C printer plan"。
- **CC1 库内 tag 记录**: D2 的 ast.h + parse.c 改动。闸门: 本机 ast.sh / diagnostic.sh / trivia / cst / expand 全过 (dump 零变化), docker-test 全绿。提交 "Record struct and enum tags on the type record"。
- **CC2 骨架 + 类型打印**: tools/c2c.c 拷 fmt.c 骨架 (argv 加 -I 透传 add_include_path, -D/-U; stdout 输出); Emitter + 指针键 side table (D5/D6/D7); declspec 重建 (存储类 from attr, 全 TypeKind, 成员表/位域/_Alignas/typeof/VLA 维度) + declarator 重建 (D8 声明侧, 参数表含无名原型)。闸门: 本机新 golden 声明样本全过。提交。
- **CC3 表达式打印**: 全表达式 kind, D8 表达式侧; _Generic/CAS/EXCH/builtin 重合成调用形态; 语句表达式; 复合字面量。闸门: 本机表达式 golden (对齐 ast.sh 的二元/结合性清单)。提交。
- **CC4 语句与顶层**: 全语句 kind; 忠实初始化器记录 (designator/区间/string/表达式/花括号省略原样); 声明记录链与枚举重组; 标签/case 区间/计算 goto/asm。闸门: 本机 golden 覆盖全部 69 NodeKind。提交。
- **CC5 round-trip 基建**: test/c2c.sh 三段 (golden / 幂等 / 归一化 AST 等价, -E 探测分流照抄 expand.sh); Makefile: c2c 目标 (不进 all), make test 拉起, stage2/c2c + test-stage2, clean。归一化 = sed 剥 dump 的 `@span` 段 (格式按 ast_span 实测写, 引号内值保留)。闸门: 本机全过 (自含部分), docker 41+7 语料全过。提交。
- **CC6 -S 门 + 自举门**: c2c.sh 的 -S 段 (fmt.sh 模式: mktemp 备份 + 原地覆盖 + 三时间宏钉死; line.c 例外记录); 自举语料 main.c / codegen.c / libcp/src/*.c (include 旗标照抄 Makefile 的 stage2 规则)。失败定位: 打印器 bug 修本线, 库侧缺口记 finding 并在本线内处置。闸门: docker -S 全等。提交。
- **CC7 收官**: RESULT-c2c.md (实测数, 坑, 偏离; 三遗留项证据: trivia 迭代器 - 第二个不消费 trivia 的消费者; range - 打印器不需要 range, 维持现状; UNPARSED - c2c 不消费 CST, 无新证据维持推迟; user data - 改写型消费者仍是只读遍历 + side table, 负结果+1, 建议提请关闭); AGENTS.md 路线图/代码地图/决策记录更新; docker-test + check-library + check-context 全绿收官提交。

## 闸门
| 闸门 | 命令 | 通过条件 | 步骤 |
|---|---|---|---|
| 本机特性 | test/c2c.sh (自含部分) | golden/幂等/归一化 AST 全过 | CC5 起 |
| docker 特性 | 容器内 test/c2c.sh | 41+7 语料三项 + -S 逐字节 (line.c 例外) | CC5/CC6 |
| 自举 -S | 容器内自举语料 -S diff | 逐字节相等 | CC6 |
| 既有门 | make docker-test / check-library / check-context | 全绿, 快照空 diff | 每步 |

## 风险与对策
1. 归一化 dump 等价被"合法但树不同"的输出骗过 (如强加花括号产生 ND_BLOCK) - 布局规则约定不加块, golden 肉眼锁关键形态, -S 门兜底。
2. 优先级表错误 - 表达式 golden 对齐 ast.sh 清单 (全二元运算符×结合性), -S 门兜底。
3. 自举语料撞库侧缺口 (属性位置/asm 限定符未记录) - chibicc 发射只吃 packed/aligned, 预期 -S 不变; 若差, 按 finding 处置并记录。
4. 时间宏/文件路径污染 -S - fmt.sh 同款三宏钉死 + 原地覆盖; line.c 例外。
5. in-place 覆盖破坏语料 - mktemp 备份 + trap 还原 (fmt.sh 同款)。
6. stage2 一致性 - 同一份 c2c.c 链两套库各跑一遍 (fmt 模式)。

## 回退
c2c 为纯新增; 库改动仅 tag_tok 追加且无既有代码依赖。任一步 git revert 单提交即回到前一状态; CC1 单独 revert 会使 c2c.c 退回合成 tag 方案 (不可用), 故 CC1 与 CC2-CC7 不混在同一次 revert 里。
