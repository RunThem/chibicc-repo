# RESULT-c2c.md - C→C 打印器线

> 状态: 已完成 (2026-10-25)
> 计划: plans/PLAN-c2c.md
> 提交: 5edae93 (计划), 8407099 (CC1), 3a0e4e6 (CC2), 0e3569e (CC3), bfb3888 (CC4), 02d44c4 (CC5), 754b0cd (CC6)
> 基线: main @ 43a718f

## 1. 终态

`tools/c2c.c` 约 1900 行: tokenize -> preprocess -> parse (第 3 层, 不调 libcp_sema) -> 打印单一展开后的翻译单元。库侧唯一改动是 CC1 的 `LibcpType.tag_tok` (ast.h + parse.c 各一处赋值, 源码兼容追加, D7 函数面不变)。

闸门终态 (make test 与 make test-stage2 双跑, docker 为权威):
- **golden**: test/c2c.sh 的 35 个片段逐字节锁定, 覆盖 parse 树可达的全部 65 个 NodeKind (69 个中 4 个 sema 专属: ND_VAR / ND_VLA_PTR / ND_MEMZERO / ND_NULL_EXPR) 与全部 18 个 TypeKind; 脚本内含覆盖审计 (低于 65 即失败)。
- **归一化 AST 等价**: 46 个语料文件 (test/*.c + include/*.h, 排除 line.c 与 pragma-once.c) 的打印产物再解析得到的树, 与原文的树剥掉 @span 锚点与引号文本后逐字节相等; 且打印产物再打印逐字节不变。
- **-S 汇编逐字节等价**: 54 个文件 (41 个 test/*.c + main.c + codegen.c + libcp/src/*.c 共 12 个自举源, 约 15k 行) 的打印产物编译出的汇编, 剥离 `.file`/`.loc` 调试行信息后与原文逐字节相等。时间宏钉死且同时传给 chibicc 与 c2c。
- 既有全部门 (docker-test 含自举与 stage2, 快照空 diff, diagnostic/shape/ast/trivia/cst/expand, check-library, check-context) 全绿。

## 2. 与计划的偏离

1. **-S 门的口径从"逐字节"放宽为"剥离调试行信息后逐字节"** (计划判据 4 措辞收紧): 展开后的单文件没有 `#include` 行, `.file` 的头文件表与 `.loc` 的行号本质上不可能与原文一致 (fmt 能全字节等价是因为它保留 include 行)。指令序列、标签、数据与重定位仍逐字节; 这一层对语义等价的锁定不减弱。
2. **语料排除了 test/pragma-once.c** (与 line.c 同类的例外并已记录在脚本): 该文件 `#include` 指向自身, 树的形状取决于文件的路径身份, 脱离原路径后展开结果不同 (fmt.sh 的同款陷阱)。
3. **typedef 记录永不分组**: 计划 D3 的"同 declspec 记录连续声明合并"在 typedef 场景不满足多轮打印稳定性 - 一组多 declarator 的 typedef 在再解析时会产生新的派生记录排列 (c35 与 glibc 的 `__locale_t`/`locale_t` 都撞到), 打印形态在第二轮起发散。typedef 一律一条一句, 形态自第一轮起固定; 匿名 enum/struct 的多 declarator 声明组保留分组 (那是单一定义的正确拼写)。
4. **flush 机制比计划 D7 复杂得多** (D7 只预见了"不完整类型的前向声明"): 实际实现是"文件作用域 tag 定义的再锚定" - 锚点为首个触达记录的链位置 (node->origin 记录链位置而非指针), 受四个正交约束: 成员引用的 typedef 名必须已发射 (tname_emitted, 否则定义内部拼写会吞掉后续 typedef 语句引入的定义); 含未定义 enum 成员的宿主只能在 enum 的 consts run 处 flush (否则常量链位置错位, 归一化 AST 等价破); 同 tag 的多定义只 flush 最早注册的一个 (shadow 对 hoist 冲突); 函数体内首触达的记录不 flush (局部 tag 定义提升到文件作用域会污染后续局部定义的 tag 查找, struct.c 的自引用 `struct T { struct T *next; ... }` 撞到)。flush 前有可行性检查 (flush_feasible/type_feasible, 带自引用记忆化)。

## 3. 坑与修复 (实施中的主要发现)

1. **`_POSIX_C_SOURCE`**: glibc 在严格 `-std=c11` 下不声明 `open_memstream`/`strndup`, 隐式声明返回 int 截断成指针, Linux 构建段错误而 macOS/ASan 全绿。fmt.c 无 POSIX 调用所以 L5 没踩过。
2. **origin 根解析**: `libcp_copy_type` 对函数参数与限定符应用做浅拷贝并挂 `origin`, 副表按指针分桶时拷贝与定义记录是两个 key, 定义会打印两次 (glibc 的 `union sigval` 双定义)。`ty_info` 现在先解析到 origin 链根。
3. **`print_enum_spec_e` 不能复用 struct 的 `layout_pending` 早退**: enum 记录该字段恒为 false, emitted=false 的路径会打出裸 `enum` (随后 `typedef enum LibcpNodeKind;` 之类的畸形)。
4. **U/L 字符串前缀按元素符号性恢复**: L 元素是 signed int, U 元素是 unsigned int, 同为 4 字节 - 按 `base->is_unsigned` 区分, 否则 `U"\xffffffff"[0] >> 31` 的 shr 会变 sar。
5. **sizeof(expr) 的 cast/复合字面量操作数必须括起**: 粘连后 `sizeof (char)1` 读成 sizeof(type) 形态。
6. **链式指示符整条链一个 `=`** (`.b[2] = v`, 不是 `.b = ... = v`); **postfix 基座在 16 级** (否则 `(*p)--` 重解析为 `*p--`); **asm 不消费自己的分号**, 链上吸收其后的空语句合成一行; **顶层空 TU (纯头文件) 的 parse 返回 NULL 是成功不是失败**。
7. **-S 段的旗标三组** (test 语料 `-Iinclude -Itest`; 自举源 `-DLIBCP_DEFAULT_ALLOCATOR -Ilibcp/include -Ilibcp/src -Iinclude`; 时间宏必须同时传给 chibicc 与 c2c 且必须内联 - 经变量展开会丢字面引号, fmt.sh 的同款坑)。

## 4. L5 遗留项的评估落账

1. **trivia 分段迭代器**: c2c 是第二个消费者且**不消费 trivia** (拍板 3, 注释丢弃) - 两个消费者一个保留一个丢弃, 无公共需求形态出现。维持推迟 (RESULT-l5 第 9 节 b 的候选形态不变), 等真正需要解读 trivia 的第三个消费者。
2. **range 语义**: c2c 全程不使用 `libcp_node_source_range` (打印走树, 拼写从 token 取, 不回溯源文本)。维持现状。
3. **残余 UNPARSED 收编**: c2c 不消费 CST, 无新证据。维持推迟。
4. **per-node user data**: **本线给出决定性负证据** - C→C 打印器是计划预期的"改写型消费者", 实际形态仍是只读遍历 + 遍历者自建 side table (指针键哈希, 存 typedef 名/枚举记录/锚点等), 零节点改写, 零节点挂载。两个消费者均不需要。**建议提请关闭该开放决策**: 公共面不引入 per-node user data 扩展点。

## 5. API 手感发现 (第二个消费者的清单)

1. `LibcpType.tag_tok` 是第一个"为消费者加的字段" - 加得动 (源码兼容, 既有闸门零感知), 印证公共 IR 的可生长性。
2. 声明记录的拆解全部可行, 但三处形状迫使消费者重建语义: tag 只在 Type 记录上 (CC1 补的), 匿名 enum 的常量是链上记录而非 Type 字段, 函数参数是类型浅拷贝 (origin 链)。
3. `libcp_parse` 对空翻译单元返回 NULL - 库语义是"无声明"而非失败, 但对消费者是个隐性约定, 文档值得写明。
4. 诊断回调的 severity 区分有用: 属性警告 (如 cleanup ignored) 在消费场景应不阻断。
5. -dump-ast 只接受 `.c` 输入是消费面的一个小刺 (头文件语料要拷贝改名)。

## 6. 决策记录

拍板 (2026-10-25, AskUserQuestion): (1) tag 库内记录 - 实施为 tag_tok, 正确; (2) 自举语料作为 -S 门必过项 - 全部 54 文件通过, 撞出 origin/POSIX/作用域三处真 bug, 判断正确; (3) 注释丢弃 - 实施简单, 幂等性好锁。

后续: C→C 打印器作为第 3 层消费者收官; 第 4 层 (sema 后) 的打印与 provenance 标注输出 (--annotate) 是潜在后续, 未立项。
