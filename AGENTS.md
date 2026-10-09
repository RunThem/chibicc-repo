# AGENTS.md - chibicc(C 前端库 fork)

本仓库是 Rui Ueyama 的 chibicc(小型 C11 编译器, 约 9k 行, 扁平目录布局)的 fork. 项目目标已从"教学用编译器"重定位为:

**把 C 前端(tokenize / preprocess / parse / sema)做成一个可复用的库**, 供编译器之外的其他软件使用 - formatter(格式化), C 转 C 的源到源代码生成, linter, 静态分析等. chibicc 本体退化为"驱动器 + x86-64 codegen", 作为这个库的参考消费者. 上游 README(含 "Design principles" 一节)描述的是上游的教学定位; 与本文件冲突时, 以本文件为准. 除 `test/` 与 `include/` 外目前没有子目录, 库化后的目录形态在实施时定.

## 全局约定

语言: 本文件及所有输出统一使用中文, 代码标识符与专有名词保留英文原文.
标点: 所有输出(写入文件的注释 / 文档, 以及回显给用户的回复), 无论中文还是英文, 一律使用英文标点(, . : ; ( ) - _ /), 不使用中文标点(如 , 。 ： ； （ ） 、).

## 目标架构与库边界

流水线按"中间表示"划成 5 层; 每层是一个干净的阶段边界, 也是库对外暴露的一种能力:

1. **Token 流(trivia 保留)** - tokenize 的改造目标: 空白/注释/换行作为 trivia 挂在 token 上, 不丢弃; 拼写/位置/编码等文本级信息完整.
2. **CST(lossless 具体语法树)** - 建立在预处理**之前**的原始文本上, 预处理指令(`#define`/`#if` 等)本身是树的节点; 每个语法 token 与标点都是显式节点, trivia 挂靠; 保证逐字节还原源码. 服务 formatter 与精确重构.
3. **语法层 AST(忠实, 无类型)** - 从 CST 下降: 消解标点与括号, 但保留每一种源码写法的区分(`a[i]` vs `*(a+i)`, `while` vs `for`, `op=` 复合赋值, `>` vs 交换操作数, 隐式 cast 有标记); 不做降级, 不查名字, 不算类型. 服务 C→C 代码生成等语法级工具. 宏调用在本层的保留策略是开放设计点, 见"开放决策".
4. **sema 产物(纯标注, 保留源码写法)** - 名字解析(绑定到 Obj), 类型检查与标注, 常量求值, 结构体布局, 以及编译期必需的语义结论(泛型选择, sizeof 结论, 隐式 cast 带标记); 语言规定存在的对象(字符串字面量的匿名全局, 复合字面量的无名字对象, ABI 对象)在这一层物化. 不带任何"为某个后端发射服务的整形"(指针算术缩放, 复合赋值/自增自减的读写回环, 关系运算符换序, 临时槽, 语句重排) - 这些属于消费者. 该边界已达成(A9.2 审计), 见"路线图".
5. **汇编(codegen)** - x86-64 System V / GAS / ELF, 不属于库. codegen 直接消费第 4 层的标注树, 自己决定怎么把源码写法落成 x86-64(含它需要的临时槽与语句重排).

消费方式: 编译器(驱动器 + codegen)走 1→(展开)→3→4→5 的编译路径; formatter 只消费 1→2; C→C codegen 消费 3; 语义级工具消费 4(标注树对源到源与语义工具都是可直接使用的形态).

## 面向库的硬性规则(存量代码按路线图收敛, 新代码立即生效)

- 不新增 `exit()` 调用: 现有 `error_tok` 直接 exit(1), 作为库最终要改成可注册的错误回调或错误返回; 新代码不得引入新的 exit 点.
- 不新增静态全局状态: 语义状态已全部收到 sema.c(10 个文件域 static: `locals` / `globals` / `sema_fn` / `builtin_alloca` / 作用域表 `scope` / goto 配对检查的 `gotos` / `labels` / 检查用的 `loop_depth` / `switch_depth` / `resolving_body`; 外加 `new_unique_name` 的 `unique_name_id` 计数器. 控制流标签与控制流整形的 static 已随 A2.1 移入 codegen, `decl_remove` 随 A8.1 消失), 与库的可重入性冲突; parse.c 只剩 `scope`(typedef/tag 分类 oracle)与 `is_typename` 的关键字表缓存(建一次后只读). 新代码把状态放进显式的 context 结构.
- API 纪律: 公共头(未来从 `chibicc.h` 拆出)与内部头分离; 中间表示边界处不泄漏编译器内部假设(Linux 路径, 单次进程生命周期, 直接 exit 等).
- 后端整形只允许出现在 codegen.c: 指针算术缩放, 读写回环, 临时槽与隐藏变量, 语句重排, 标签与唯一名分配, 都不是库层的事. 库层(sema 及以下)只做标注与语义结论(含物化语言规定存在的对象). 该红线经 A9.2 的逐项 grep 证实已达成(缩放/回环/标签/重排/临时槽/调用 codegen 在库层均为 0 处; 库层的建槽只剩语言与 ABI 对象), 佐证留档于 `RESULT.md` 的 A9.2 一节.
- 检查与诊断留在库层: 夹在降级代码里的检查(左值性, 实参个数, 位域取地址, VLA 不得初始化, 不完整类型等)在降级搬走时必须在库层显式保留; 文案与插入符锚点以 `test/diagnostic.sh` 的逐字节锁定为准.
- 库名与对外头文件名属于用户决策; 定名前, 文档与代码注释统一用"前端库"指称, 不擅自更名.

## 路线图

语法语义拆分线(阶段 3 + 4)已细化为 22 步可勾选执行清单并**全部完成**(终态: parse.c 2531 行纯语法 + sema.c 1552 行, codegen.c 零改动; 计划与执行记录已归档为 `PLAN-split.md` / `RESULT-split.md`). 其后的忠实层收尾线(阀门口径放宽)同样**已全部完成** - 解除字节冻结造成的忠实层偏差后, parse.c 的表达式层达到"无类型标注, 无名字绑定, 无常量求值"的第 3 层形态(终态: parse.c 2148 行 + sema.c 3145 行 + chibicc.h 704 行; codegen.c 相对 5f53ed0 零改动; 计划与执行记录已归档为 `PLAN-faithful.md` / `RESULT-faithful.md`).

**现行线: codegen 直连 sema 产物(取消 sema 降级)** - **阶段 A, 阶段 B 与 B2 均已全部完成**(A0.1-A10.2, B0-B1.3 与 B2.0-B2.2, 计划 `PLAN.md`, 执行记录 `RESULT.md`): codegen 直接消费第 4 层的标注树, sema 不再做任何"为发射服务的整形", `add_type` 经 A9.2 逐 case 审计为纯标注遍; 第 4 层的边界按"语义结论归库层, 发射便利与临时槽归消费者"重划(判据见 `PLAN.md`), 上面第 1-5 条即终态. 阶段 B 与 B2 把"只需换发射方式"的整形项(下标, 箭头成员, `>`/`>=`, elvis, `while`, break/continue, 声明展开, 加减法的指针缩放)从整形遍搬进了发射点, 且除 elvis 的临时槽移除与 B2 的嵌套 VLA 维度修正外指令序列逐字节不变; 整形遍的残留 = 需要槽/语句/标签的项(op= 与自增自减, VLA 尺寸, 返回缓冲, 复合字面量, case 链与标签分配, 记录摘除). 其后:CST/trivia(阶段 1-2)与库化(阶段 5)的先后待用户拍板.

每个阶段完成时三道闸门必须全绿: `make docker-test`(含自举), 汇编等价性 diff(阶段 0 建立), 该阶段新增的针对性测试. 阶段内行为不允许变化, 变化只发生在阶段边界并单独提交.

0. **等价性基线**: 在 docker 里对全部 `test/*.c` 与自举产物跑 `./chibicc -S`, 存汇编快照; 之后每阶段结束逐字节 diff, 防行为回归.
1. **trivia 保留**: tokenize.c 改造, 注释/空白/换行挂到 token; 配 token 层测试.
2. **CST**: 建立在原始文本(含预处理指令)上的 lossless 树; 验收标准是逐字节 round-trip(打印结果 == 输入文件).
3. **忠实语法 AST**(已实现): 新增 21 个 NodeKind(上游 48 + 21 = 69): 忠实写法类 `ND_SUBSCRIPT`, `ND_GT`/`ND_GE`, `ND_INCDEC`, `ND_WHILE`, `ND_BREAK`/`ND_CONTINUE`, `ND_SIZEOF`/`ND_ALIGNOF`, `ND_STRING`; 未决引用类 `ND_IDENT`(parser 不绑定的名字); 声明记录类 `ND_DECL`, `ND_GVAR_DECL`, `ND_FUNCDEF`, `ND_TYPEDEF`, `ND_ENUM_CONST`, `ND_COMPOUND_LITERAL`; sema 侧载体类 `ND_GENERIC`/`ND_GENERIC_ASSOC`, `ND_TYPES_COMPATIBLE`, `ND_REG_CLASS`. 上游 48 个 kind 中 5 个加忠实性字段(`ND_ASSIGN.op`; `ND_CAST` 的 `ty_op` 与 `is_implicit`; `ND_MEMBER` 的 `arrow_tok`; `ND_COND` 的 `is_elvis`; `ND_FOR` 的标签槽); `ND_MEMZERO` 退出 parse 的树(sema 降级时重建). parse.c 原约 55 处不忠实构造点(复合赋值/自增自减重写, 指针算术缩放, 初始化器拍平, sizeof 折叠, 字符串字面量转匿名全局, 关系运算符交换操作数等)已全部消除, 降级逻辑在 sema.
4. **sema 独立 pass**(已实现, 现行线继续收边): parse.c 去语义化 - 名字解析/类型检查/常量求值/结构体布局/降级全部搬出到新的 sema.c; 解析器只保留 typedef 名字分类 oracle(C 文法要求的最小语义反馈). `eval/eval2/eval_double/is_const_expr`, `add_type`, `struct_decl` 布局, `write_gvar_data` 等随之迁移. 现行线把其中的"降级"再移一层: sema 只留标注, 整形归 codegen(见上).
5. **库边界固化**: 错误处理回调化, static 全局收敛为 context 对象, 公共头/内部头拆分, Makefile 新增库构建目标.
6. **参考消费者(可选)**: 最小 formatter(消费 CST)与 C→C 打印器(消费语法层 AST)作为示例, 验证库 API 的好用性.

## 开放决策(实施时由用户拍板, 不擅自定)

- 库名与公共头命名.
- 语法层 AST 对宏调用的保真策略: 保留宏调用为节点(源到源工具需要)还是消费展开后的流.
- 长驻进程场景下 arena 的整体 reset 时机与 API 形态.

## 构建与测试

- `make` - 构建 `chibicc` 二进制(纯 C11, 无外部依赖; CFLAGS 定义在 Makefile 中).
- `make test` - 用 chibicc 自身编译所有 `test/*.c`, 用 `cc -pthread` 链接 `test/common` 后逐个运行, 最后依次运行 `test/driver.sh ./chibicc`(命令行选项检查)、`test/diagnostic.sh ./chibicc`(诊断锁定 55 例逐字节)与 `test/shape.sh ./chibicc`(发射形状回归锁 33 段).
- `make test-stage2` - 自举检查: 用 chibicc 编译它自己, 再重跑全部测试.
- `make test-all` - 以上两项合计. `make clean` 用于清理.
- 单独运行某个特性测试: `make test/sizeof.exe && ./test/sizeof.exe`(模式规则会自动处理).
- 没有 Linux 后端时只验证前端: `./chibicc -E file.c`(仅预处理)或 `./chibicc -S file.c`(输出汇编文本).
- 库构建目标(`.a`/`.so` 与目标名)在路线图第 5 阶段加入 Makefile.

## 平台陷阱与 Docker 测试环境(本工作区为 macOS ARM64)

chibicc 生成的是 x86-64 System V / GAS / ELF 汇编, 且 `main.c` 硬编码了 Linux 路径(`/usr/include/x86_64-linux-gnu` 等)与 gcc/`ld` 调用. 编译器本体在本机**可以构建**, 但它生成的代码无法在本机汇编或运行, 直接跑 `make test` 会在汇编阶段失败. 只做前端检查可用 `./chibicc -E file.c`(仅预处理)或 `./chibicc -S file.c`(输出汇编文本).

完整测试通过 Docker 运行(OrbStack, amd64 容器走 Rosetta, 本机已具备):

- `make docker-image` - 构建依赖镜像 `chibicc:amd64`(ubuntu:22.04, amd64, 一次性约 90 s).
- `make docker-test` - 自研测试集 + 自举, 分钟级. 仓库以只读挂载, 源码拷贝到容器可写层后构建运行, 本机构建产物不受影响.
- `make docker-test-thirdparty` - 依次跑 test/thirdparty 五个脚本(tinycc libpng git sqlite cpython, 由小到大); `THIRDPARTY=git` 可单跑一个. tinycc 实测约 15 s; git/sqlite/cpython 的测试套件需要数小时, 首次运行需要网络 clone. clone 缓存保存在仓库的 `thirdparty/` 目录(已被 .gitignore 忽略, 跨运行复用; 若某个 clone 损坏, 直接删掉对应子目录重新拉).
- Rosetta 下 tcc 的 106_pthread/112_backtrace/113_btdll 三个用例(信号/TLS/线程类)会不稳定 SIGSEGV, 与 chibicc 无关; `test/thirdparty/make` 是一个 PATH shim(文件名必须是 make), 会在 make test 前移除这三个用例的源文件使其被跳过, 其余 110+ 用例照常. 真正的 x86-64 Linux 上无需此 shim.
- 为什么一切都要在容器可写层(/work)跑: 实测 tcc 的 `-run`/bcheck 运行时只认 overlayfs - 在 virtiofs 挂载路径或 docker volume(ext4)上会确定性 SIGSEGV(tinycc 112_backtrace 用例, 失败点还会漂移). 因此挂载目录只提供源码与 clone 缓存, 构建与测试全部发生在 /work, 这同时避免 darwin/linux 产物互相污染.
- 镜像内已把 `git@github.com:` 全局重写为 HTTPS(thirdparty 脚本用的是 SSH 地址, 容器里没有 SSH key); chibicc 运行时只调用 `as` 和 `ld` 并按 Ubuntu 路径找 crt/libgcc, 镜像里的 gcc 工具链满足.

## 现状与代码地图

现状: 语法语义拆分线、忠实层收尾线与 codegen 直连线(阶段 A + 阶段 B + B2)均已完成 - 第 3 层(忠实语法 AST)与第 4 层(sema 产物)在 parse.c / sema.c 之间分开; sema.c 持有名字解析(作用域表)、类型检查、常量求值、结构体布局与结论记录, **不做任何降级**(A9.2 审计: add_type 34 个 case 全部纯标注, 无形状改写); codegen 直接消费标注树 - 需要后端整形的项分两处落地: 只需换发射方式的(`x[y]`, `p->x`, `>`/`>=`, elvis, `while`, break/continue, 声明初始化链, 加减法的指针缩放)在**发射点就地处理**, 需要槽/语句/标签的(op=, 自增自减, VLA 尺寸, 返回缓冲, 复合字面量, case 链与标签分配)留在整形遍(契约 3 的六条不变量, 见该文件头注释); parse.c 只建忠实语法形状(表达式无类型, 名字不绑定, 常量不求值), 保留文法分类 oracle(typedef 名 / tag)与判定表 A 的 15 处文法可判诊断. 改动前先了解现状:

- `chibicc.h` - 所有共享类型(`Token`, `Obj`, `Node`, `Type`, `Member`, `VarAttr`)与跨文件声明; 未来在此拆分公共头与内部头.
- `tokenize.c` - 词法; 当前丢弃注释与空白(阶段 1 的改造对象).
- `preprocess.c` - 宏展开与预处理指令, 输入输出都是 token 列表.
- `parse.c` - 递归下降解析器(2148 行), 只做语法分析与忠实建树: 声明产出记录节点(ND_DECL / ND_GVAR_DECL / ND_FUNCDEF / ND_TYPEDEF / ND_ENUM_CONST)交 sema 消费, 待补全的类型记录(数组维度 / typeof 操作数 / 对齐 / 位宽)挂在类型或节点上留给 sema. 唯一的语义反馈是文法必需的 typedef/tag 分类 oracle(本文件唯一的文件域 static). 诊断只剩 RESULT-split.md 判定表 A 的 15 处文法可判项(B/C/D 已清空, 见 RESULT.md 的 R4.2); parse 不在构造现场调用任何降级(旧的时序原则已废止).
- `sema.c` - 语义分析(2968 行), 每个函数体两趟(见文件头注释): resolve 遍历重建作用域并绑名、声明对象、补全类型(维度 / typeof / 对齐 / 位宽 / case 值)与布局; 标注遍历(`add_type`/`type_chain`)定型、插隐式 cast、跑检查、写结论(物化字符串/复合字面量/块域 static 数据镜像), 不改写树形状. 控制流四检查与 goto/label 配对检查随后跑(纯检查, 不分配名字); 标签与唯一名分配在 codegen. 常量求值(`eval`/`eval2`/`eval_double`/`is_const_expr`/`const_expr`)与全局初始化器序列化(`write_gvar_data`)也在这一侧, 预处理器 `#if` 经 `const_expr` 调用. 全部语义 static 状态在此.
- `type.c` - 类型构造器与类型谓词(`is_compatible`/`is_integer` 等).
- `codegen.c` - 整形遍(残留) + 发射点就地降级 + AST 翻译成 x86-64 汇编文本, 无优化 pass. 整形遍在 `codegen()` 入口先于一切赋值与发射运行(契约 3 的六条不变量见文件头注释), 只剩需要槽/语句/标签的项: 控制流标签分配(case 链与 goto/label 配对), **VLA 声明**的展开(运行时尺寸槽必须先于帧布局)与其余记录的**残留项到达**(`shape_init_exprs` 只走不再改), 复合赋值与自增自减, 调用返回缓冲, 复合字面量; 发射点就地处理的是下标(`gen_addr`/`gen_expr` 经 `new_add` 现建现发), 箭头成员(`gen_addr` 按 `arrow_tok`), `>`/`>=`(二元尾部换序), elvis(`gen_expr(ND_COND)` 无槽), `while` 与 break/continue(`gen_stmt` + 发射侧环境标签栈), 声明初始化链(`gen_stmt(ND_DECL)` 就地摊), 加减法的缩放与 `num+ptr` 规范化(`gen_expr` 顶部把未标记的 `ND_ADD`/`ND_SUB` 替换成 `new_add`/`new_sub` 的产物 - 三个降级工厂把自己产出的节点打上 `is_lowered` 标记, 发射点只降级没标记的, 即忠实节点). 该线(阶段 A + B + B2)的每一步与验证见 `PLAN.md` / `RESULT.md`.
- `main.c` - 驱动器; `hashmap.c`(字符串驻留哈希表), `unicode.c`(UTF 编码表), `strings.c`(字符串辅助)为基础设施.

本节的代码地图描述"codegen 直连 sema 产物"线开工前的现状(sema 侧仍含降级); 该线终态时重写.

## 设计原则

- 可读性优先于巧妙. 路线图内的阶段化重构是本项目明确授权的工作; 除此之外不做顺手的"改进", 不重组无关代码, 不用宏/高阶函数合并解析器里有意的重复.
- 内存: 一律 `calloc` 的 arena 式分配, 从不逐对象 `free`; 库语境下生命周期按"一次编译会话一个 arena"设计(整体 reset 的 API 见开放决策). 简单但慢的算法是可接受的设计.
- codegen 中没有优化 pass - 这是故意的, 保留.
- `include/` 存放 chibicc 自带的自举头文件(`stddef.h`, `stdarg.h`, `stdbool.h`, `float.h`, `stdalign.h`, `stdatomic.h`, `stdnoreturn.h`), 通过 `-Iinclude` 使用, 保持不变.

## 测试约定

- 每个特性对应一个 `test/<feature>.c`; 测试使用 `test/test.h` 中的 `ASSERT(expected, expr)`(打印表达式与结果, 失败即退出), 并链接 `test/common` 以获得 `assert()` 辅助函数与共享符号. 新测试加到对应的特性文件里; 仅在必要时才扩展 `test/test.h`/`test/common`.
- 库化新增的测试形态: CST 用逐字节 round-trip 测试; 忠实 AST 用打印/结构断言; 阶段改造期间用汇编快照 diff 防行为回归.
- `test/diagnostic.sh` 逐字节锁定诊断文案与插入符锚点(55 例); `test/shape.sh` 锁定发射形状(33 段: 每个片段用 `$chibicc -S` 汇编后断言助记符序列 `want` / 整行正则 `wantline` / 不得出现的形 `absent`). 两者都由 `make test` 与 `make test-stage2` 调用, 即自举出的编译器也要过一遍; 改 codegen 的发射形时必须同步更新 shape.sh 的对应片段.
- `test/thirdparty/*.sh` 用 chibicc 构建真实项目(git, sqlite, libpng, cpython, tinycc) - 很慢, 仅限 Linux, 不属于 `make test`.
