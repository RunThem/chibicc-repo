# 08 第 5 层: x86-64 codegen 参考消费者

## 8.1 作用与位置

第 5 层是 `codegen.c`, 把第 4 层的标注树翻译成 x86-64 System V 汇编. 它**不属于库**:

- 不在 `libchibicc.a` / `libchibicc.so` 里.
- 不导出公共函数.
- 不承诺 API 稳定性.
- 只支持 x86-64 Linux (GAS 语法, ELF 目标).

它的价值有两个:

1. 它是 `chibicc` 二进制的后半段, 让仓库能自举.
2. 它是一个完整的"如何消费第 4 层"的参考实现.

如果你要写自己的后端, 第 5 层可以当范例读, 但不要依赖它的内部结构.

## 8.2 为什么代码生成不在库里

代码生成必然包含大量后端假设:

- 调用约定.
- 栈帧布局.
- 寄存器分配.
- 数据段与重定位格式.
- 标签命名.
- 指令选择.

这些假设对库的其他消费者没有意义. 更重要的是, 把它们放进库会反向污染第 4 层: 语义分析会开始"顺手"生成后端需要的形状, 例如指针缩放, 临时槽, 语句重排.

本库的分界是:

- 库层只做标注和语义结论.
- 后端需要的形状改写, 由后端自己做.

第 5 层的整形遍就是这个原则的体现: 它住在 `codegen.c`, 不在 `sema.c`.

## 8.3 输入与输出

输入:

- `ChibiccObj *prog`: `chibicc_sema` 返回的全局对象链.
- 每个函数的 `body`: 标注树.
- 每个全局变量的 `init_data` / `rel`: 已序列化的数据.

输出:

- GAS 语法的 x86-64 汇编文本, 写到 `FILE *out`.
- 包含 `.file`, `.data`, `.text`, `.globl`, 符号, 重定位.

入口:

```c
void codegen(Obj *prog, FILE *out);
```

`codegen` 不是公共 API. 它声明在驱动器内部头 `driver.h`, 只被 `main.c` 使用.

## 8.4 两阶段结构

`codegen` 分两步:

```c
void codegen(Obj *prog, FILE *out) {
  output_file = out;

  File **files = get_input_files();
  for (int i = 0; files[i]; i++)
    println("  .file %d \"%s\"", files[i]->file_no, files[i]->name);

  label_base = unique_name_next();
  shape(prog);

  assign_lvar_offsets(prog);
  emit_data(prog);
  emit_text(prog);
}
```

1. **整形遍**: `shape(prog)` 改写树, 分配标签, 建临时槽.
2. **发射**: `assign_lvar_offsets`, `emit_data`, `emit_text`.

顺序不能换: 发射器读的标签字段和槽必须已经就位.

## 8.5 整形遍

### 8.5.1 六条不变量

整形遍按六条不变量运行:

1. **后序**: 子节点先整形, 再改写父节点. 外层改写可以依赖内层已完成.
2. **阶段顺序**: 表达式和声明整形先于控制流标签分配, 这样整形中新建的循环也能拿到标签.
3. **函数上下文**: 改写创建的临时槽追加到当前函数的 locals, 并且都在 `assign_lvar_offsets` 之前存在.
4. **不重入**: 整形新建的子树已经是整形后的, 不再走一遍; 树里没有能把遍历引回去的别名.
5. **只访问一次**: 嵌套函数的函数体只从程序链访问一次, 不从宿主函数体进入.
6. **初始化器走显式路径**: 解析后的初始化器树由 `shape_init_exprs` 显式遍历, 不靠节点字段递归.

这六条写在 `codegen.c` 文件头, 是修改整形遍时的契约.

### 8.5.2 整形遍做什么

| 项 | 处理 |
|---|---|
| `while` / `do` / `for` | 分配 break 和 continue 标签 |
| `switch` | 分配 break 标签, 收集 case 链 |
| `case` | 分配唯一标签, 挂进 case 链 |
| `label` | 分配唯一标签, 收集到函数标签表 |
| `goto` | 留给 `resolve_label_refs`, 与 label 配对 |
| `for` 的 init 链 | 多于一条语句时包成 `ND_BLOCK`, 适配发射器的单语句 init 槽 |
| 复合赋值 `op != 0` | `to_assign` 改写成读改写, 处理成员和原子形式 |
| `ND_INCDEC` | 改写成复合赋值, 后缀形式再包一层减一 |
| 结构体/联合返回值调用 | 建 `ret_buffer` 临时槽 |
| VLA 的 `sizeof` | 改写成计算并读取尺寸变量的逗号表达式 |
| 复合字面量 | 改写成初始化逗号加隐藏对象引用 |
| `_Generic` | 只整形被选中的关联表达式 |
| `ND_COND` | 只递归, 不改写. elvis 由发射器无槽处理 |

### 8.5.3 整形遍不做什么

- 不缩放下标和指针算术. 这由发射器的 `new_add` / `new_sub` 在发射点做.
- 不交换 `>` / `>=`. 发射器处理.
- 不把 `while` 改成 `for`. 发射器按 `ND_WHILE` 发射.
- 不展开普通声明初始化链. 发射器在 `gen_stmt(ND_DECL)` 就地摊开.
- 不处理 `break` / `continue` 的跳转. 发射器用环境标签栈.
- 不把 elvis 建成临时槽.

### 8.5.4 标签命名

标签名形如 `.L..N`. 编号来自一个全局计数器:

- 第 4 层物化匿名对象时用这个计数器, 例如字符串全局.
- 第 5 层整形时从 `unique_name_next()` 接着用.

格式是 load-bearing 的: 快照归一化脚本会重编号后缀为 `.<数字>` 的标签. 改前缀会让所有快照 diff.

### 8.5.5 私有状态

第 5 层需要 per-node / per-type / per-obj 的状态:

- 标签字段.
- `is_lowered` 标记.
- `pass_by_stack`.
- `ret_buffer`.
- case 链.
- `Obj.offset`.
- `stack_size`.
- `Type.vla_size`.

这些字段不在公共 `ChibiccNode` / `ChibiccObj` / `ChibiccType` 里, 而是保存在 `codegen.c` 的私有指针键表:

- `CgNode`: 按 `Node *` 查.
- `ObjInfo`: 按 `Obj *` 查.
- 类型槽: 按 `Type *` 查.

这是库化线 L1 的成果: 公共 IR 只保留源码和语义字段. 消费者不会看到后端状态.

## 8.6 发射

### 8.6.1 数据

`emit_data` 遍历 `prog`:

- 跳过非定义和函数.
- 对每个全局变量输出 `.data` / `.bss` / `.tdata` / `.tbss`, 对齐, 标签, 初始化字节.
- 处理 `init_data` 和 `rel`.

### 8.6.2 文本

`emit_text` 遍历 `prog`:

- 跳过非函数和非定义.
- 对每个函数输出序言, 栈帧, 参数保存, 函数体, 尾声.
- 函数体由 `gen_stmt` / `gen_expr` 递归发射.

`gen_expr` 在入口把忠实的 `ND_ADD` / `ND_SUB` 替换成 `new_add` / `new_sub` 的产物. 这三个降级工厂:

- 做指针缩放.
- 把 `num + ptr` 规范化为 `ptr + num`.
- 把产物记在私有 `CgNode.is_lowered`.

发射器只降级没有标记的节点, 也就是第 3/4 层留下的忠实节点.

### 8.6.3 发射点就地处理

| 源码形态 | 发射处理 |
|---|---|
| `p[i]` | `gen_addr` 按指针加下标算地址, 缩放就地做 |
| `p->x` | `gen_addr` 按 `arrow_tok` 先解引用 |
| `a > b` / `a >= b` | 二元尾部换序, 用小于/小于等于指令 |
| `a ?: b` | 无临时槽, 测试值留在寄存器 |
| `while` | `gen_stmt` 按循环发射 |
| `break` / `continue` | 发射侧环境标签栈 |
| `int x = e;` | `gen_stmt(ND_DECL)` 就地摊成初始化链 |
| `p + 1` | `gen_expr` 顶部 `new_add` 缩放 |

### 8.6.4 目标平台假设

- x86-64 System V ABI.
- GAS 语法.
- ELF 目标.
- Linux 路径: 驱动器硬编码 `/usr/lib/x86_64-linux-gnu`, `/usr/include/x86_64-linux-gnu` 等.
- 调用 `as` 和 `ld`.
- 不生成调试信息.
- 不做优化.

在 macOS 上可以构建 `chibicc`, 也能输出汇编文本 (`-S`), 但生成的汇编不能在本机汇编或运行. 完整测试在 Linux 容器里跑.

## 8.7 阶段产物与用途

### 8.7.1 产物

- **汇编文本**: GAS 语法的 x86-64 汇编, 包含 `.file`, `.data`, `.text`, 符号与重定位.
- **整形后的内存树**: 标签, 临时槽, 展开后的复合赋值和自增自减. 它只存在于本次发射过程, 不写回文件, 也不是公共产物.
- **可执行文件**: 驱动器调用 `as` 和 `ld` 后才产生, 不是 `codegen` 的直接输出.

### 8.7.2 可以做什么

- **参考后端**: 证明第 4 层的树足够支撑一个真实代码生成器.
- **自举**: `make test-stage2` 用 chibicc 编译自己.
- **汇编检查**: `-S` 输出汇编文本, 用于人工检查或快照比较.
- **A/B 对照**: Pratt 线用 `-S` 比较两个解析构建的代码生成是否一致.

### 8.7.3 不能做什么

- 不能作为公共 API 被第三方链接.
- 不能生成非 x86-64 目标.
- 不能提供优化, 调试信息或稳定输出.
- 不能保证汇编形状在版本间不变.

## 8.8 对第三方开发者的含义

### 8.8.1 写自己的后端

如果你要用第 4 层做自己的代码生成器, 需要处理:

- `ChibiccObj` 链, 过滤 `is_definition`.
- 声明记录 `ND_DECL` / `ND_GVAR_DECL` / `ND_FUNCDEF` / `ND_TYPEDEF` / `ND_ENUM_CONST` 仍在语句链上.
- 忠实节点: `ND_SUBSCRIPT`, `ND_WHILE`, `ND_GT`, `ND_GE`, `ND_INCDEC`, 带 `op` 的 `ND_ASSIGN`, `ND_STRING` 已变成 `ND_VAR`, `ND_COMPOUND_LITERAL` 带隐藏对象.
- 需要自己决定: 指针缩放, 复合赋值展开, 自增自减, 控制流标签, 临时槽, 初始化器展开.
- 初始化数据: `Obj.init_data` / `rel`.
- 函数体: `Obj.body`.
- 局部对象: `Obj.locals`.

仓库的 `codegen.c` 是一个完整例子, 但它不是 API. 读它可以, 链接它不行.

### 8.8.2 不要依赖的字段

- `Node` / `Obj` / `Type` 上的后端字段已经移走. 不要试图在公共结构里找 `offset`, `stack_size`, `brk_label`, `unique_label`, `vla_size`.
- 不要依赖 `.L..N` 的编号.
- 不要依赖汇编的具体形状. 快照测试锁定的是仓库自己的行为, 不是库的承诺.

### 8.8.3 只想检查汇编

如果只需要看编译器生成的汇编文本:

```sh
./chibicc -S input.c -o input.s
```

`-S` 只做前端加代码生成, 不调用 `as` / `ld`, 在 macOS 上也能跑. 生成的汇编不能在 macOS 上汇编.

## 8.9 边界与已知限制

- **不是库的一部分**. 不导出, 不稳定, 不支持非 x86-64.
- **没有优化**. 生成的代码比 GCC 慢, 这是设计选择.
- **没有调试信息**.
- **只支持 x86-64 Linux**. 其他目标要自己写后端.
- **依赖驱动器路径**. 汇编器和链接器路径硬编码.
- **快照测试锁定内部形状**. 修改发射顺序会破坏快照, 这是有意的回归保护.
- **整形遍有六条不变量**. 修改时必须遵守, 否则树会进入不一致状态.
