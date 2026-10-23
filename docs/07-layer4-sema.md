# 07 第 4 层: sema 标注树

## 7.1 作用与位置

第 4 层是语义分析. 它在第 3 层的树上做五件事:

- 名字解析: 把 `ND_IDENT` 绑定到对象或枚举常量.
- 类型标注: 给每个表达式节点算出类型.
- 常量求值: 算数组维度, 枚举值, case 值, 位域宽度, 全局初始化器, `sizeof` 结论.
- 聚合布局: 算结构体/联合的成员偏移, 对齐, 位域位置.
- 结论记录: 把 `sizeof`, `_Generic` 选择, 内建类型谓词的结果写回节点.

它**不**做的事:

- 不改写源码写法. 下标还是下标, 复合赋值还是复合赋值.
- 不做指针缩放.
- 不展开自增自减的读写回环.
- 不分配控制流标签.
- 不建临时槽.
- 不把声明记录变成语句.

这些是第 5 层的整形遍.

输入: `libcp_parse` 的输出.
输出: 原地标注的同一棵树, 加上一条 `LibcpObj` 全局对象链.

## 7.2 设计思路

### 7.2.1 标注而不是重建

很多编译器在语义分析阶段会把树改写成更低层的形状. 本库不做. 理由是第 4 层要服务两类消费者:

- 语义工具: 需要类型, 名字, 常量, 布局.
- 源到源工具: 需要源码写法.

如果第 4 层把 `x += 1` 改写成读改写, 源到源工具就失去了用户写的形式. 如果它把 `p + 1` 缩放进指针算术, 源到源工具就得反推.

所以第 4 层只填字段:

- `node->ty`: 类型.
- `node->var`: 绑定结果.
- `node->val`: 常量结论.
- `node->generic_sel`: `_Generic` 选择的结果表达式.
- `Type.size` / `align` / `members` / `offset` / `bit_offset`: 布局结论.
- `Obj.init_data` / `rel`: 全局初始化数据与重定位.

源码写法保留.

### 7.2.2 两遍走

每个函数体走两遍:

**第一遍: resolve**

- 从树结构推导作用域. `ND_BLOCK` 的 `is_scope_block` 决定是否开作用域.
- 声明解析器留下的记录所命名的对象: 局部变量, 全局变量, 函数, typedef, 枚举常量.
- 绑定每个 `ND_IDENT`: 变量或函数变成 `ND_VAR`, 枚举常量变成 `ND_NUM`.
- 求值并注册枚举常量.
- 补全类型记录: 数组维度, `typeof` 操作数, 对齐, 位域宽度, case 值.
- 布局聚合类型.
- 解析初始化器记录, 建 `LibcpInitTree`.
- 物化语言规定存在的对象: 字符串字面量的匿名全局, 复合字面量的隐藏对象, 块域 static 的数据镜像.

**第二遍: 标注**

- `add_type` / `type_chain` 给每个节点定型.
- 插入标准要求的隐式转换, 全部经 `new_cast`, 带 `is_implicit = true`.
- 跑需要类型的检查: 左值性, 实参个数, 位域取地址, VLA 初始化, 不完整类型等.
- 记录编译期结论.
- 标注初始化器表达式. 声明记录的初始化器不会被普通遍历到达, 所以单独走 `init_resolved`.

两遍分开的原因是: 第一遍建立名字和类型, 第二遍才能定型. 例如 `a + b` 的类型取决于 `a` 和 `b` 的声明, 而声明可能写在后面.

### 7.2.3 结论记录在节点上

第 4 层不把 `sizeof` 折成数字节点, 而是保留 `ND_SIZEOF`, 把值写在 `val`:

```c
case ND_SIZEOF:
  ...
  node->val = ty->size;
  return;
```

`ND_GENERIC` 同理: 节点保持 `ND_GENERIC`, 选择结果写在 `generic_sel`. `ND_TYPES_COMPATIBLE` 和 `ND_REG_CLASS` 保持种类, 结论写在 `val`.

这样做的好处:

- 消费者能看出"这里是 `sizeof`", 不是"这里碰巧是个数字".
- 源到源工具可以原样输出.
- 第 5 层需要发射时读结论, 不需要重新计算.

### 7.2.4 绑定与物化

**绑定**是改写节点种类:

- `ND_IDENT` 指向变量或函数: 变成 `ND_VAR`, 填 `var` 和 `ty`.
- `ND_IDENT` 指向枚举常量: 变成 `ND_NUM`, 填 `val` 和 `ty`.
- `ND_STRING`: 变成 `ND_VAR`, 指向一个匿名全局, 该全局的 `init_data` 是字符串内容.

这是第 4 层仅有的节点种类改写.

**物化**是建出语言规定存在的对象:

- 字符串字面量: 一个匿名全局, 名字形如 `.L..N`.
- 复合字面量: 文件作用域下是匿名全局, 函数内是隐藏局部.
- 块域 static: 一个匿名全局, 数据镜像由初始化器序列化产生.
- `__func__` / `__FUNCTION__`: 每个函数两个匿名字符串全局.
- 变参函数: `__va_area__` 局部, 136 字节.
- 每个函数: `__alloca_size__` 局部.
- 大结构体返回值: 一个隐藏首参数, 名字为空.

这些不是后端便利, 是语言或 ABI 要求存在的东西. 库把它们建出来, 消费者才能正确理解函数签名和数据布局.

### 7.2.5 常量求值

常量求值器包括:

- `eval`: 整数常量表达式.
- `eval2`: 带标签引用处理的版本.
- `eval_rval`: 地址常量.
- `eval_double`: 浮点常量.
- `is_const_expr`: 判断是不是常量表达式.
- `const_expr`: 解析并求值, 供预处理器 `#if` 使用.

用途:

- 数组维度, 判断定长还是 VLA.
- 枚举值.
- `case` 值.
- 位域宽度.
- `_Alignas` 的值.
- 全局初始化器.
- 预处理 `#if`.

常量求值支持标签地址 `&&label`, 这是 GNU 扩展. 这类地址常量在全局初始化器里以 `LibcpRelocation` 记录, 标签名由第 5 层分配.

### 7.2.6 与第 5 层的分界

第 4 层和第 5 层的分界不是"谁更懂类型", 而是"谁需要改形状":

| 项 | 第 4 层 | 第 5 层 |
|---|---|---|
| 类型 | 算 | 读 |
| 名字绑定 | 做 | 读 |
| 常量结论 | 记录 | 读 |
| 布局 | 算 | 读 |
| 指针算术缩放 | 不做 | 发射时做 |
| 复合赋值 | 保留 `ND_ASSIGN.op` | 整形遍展开 |
| 自增自减 | 保留 `ND_INCDEC` | 整形遍展开 |
| 下标 | 保留 `ND_SUBSCRIPT` | 发射时按地址算 |
| `while` | 保留 `ND_WHILE` | 发射时当循环 |
| 标签 | 检查配对 | 分配名字 |
| 临时槽 | 不建 | 建 |
| 声明记录 | 保留 | 按需展开或摘除 |
| 初始化器链 | 只解析成树 | 展开成语句或数据 |

这条分界有逐项审计, 见 `plans/RESULT-codegen-direct.md` 的 A9.2 一节.

## 7.3 数据模型

### 7.3.1 LibcpObj

`LibcpObj` 表示一个变量或函数:

```c
struct libcp_obj {
  LibcpObj *next;
  char *name;
  LibcpType *ty;
  LibcpToken *tok;
  bool is_local;
  int align;

  /* 全局变量或函数 */
  bool is_function;
  bool is_definition;
  bool is_static;

  /* 全局变量 */
  bool is_tentative;
  bool is_tls;
  char *init_data;
  LibcpRelocation *rel;

  /* 函数 */
  bool is_inline;
  LibcpObj *params;
  LibcpNode *body;
  LibcpObj *locals;
  LibcpObj *va_area;
  LibcpObj *alloca_bottom;

  /* static inline 活跃性 */
  bool is_live;
  bool is_root;
  LibcpStringArray refs;
};
```

字段含义:

- `is_local`: 局部变量或参数.
- `is_function`: 函数.
- `is_definition`: 有定义的函数, 或有初始化/暂定定义的全局.
- `is_static`, `is_tls`, `is_inline`: 存储类.
- `is_tentative`: 暂定定义, 例如 `int x;`.
- `init_data`: 全局或 static 对象的初始化字节.
- `rel`: 地址常量重定位链.
- `params`: 函数参数链.
- `body`: 函数体的标注树.
- `locals`: 函数的全部局部对象, 包括参数, 隐藏对象, 变参区.
- `va_area`, `alloca_bottom`: ABI 对象.
- `is_live` / `is_root` / `refs`: static inline 函数的活跃性分析.

`libcp_sema` 返回的是 `globals` 链. 它的特点:

- 包含全局变量, 函数声明, 函数定义, 匿名全局.
- 包含 `extern` 声明, 用 `is_definition` 区分.
- 包含 `__func__` / `__FUNCTION__` 的匿名字符串对象.
- **不包含** `alloca` 内建声明. 它单独登记在 context 里, 不在 `globals` 链上.
- 链是创建顺序的逆序, 因为 `new_gvar` 前插. 消费者不应该依赖顺序.
- 重复的暂定定义在 `finalize_globals` 里合并, 每个名字只留一个.

消费者通常这样过滤:

```c
for (LibcpObj *o = prog; o; o = o->next) {
  if (!o->is_definition)
    continue;
  /* 真正的定义 */
}
```

### 7.3.2 LibcpType

第 4 层完成后, 类型记录包含:

- `kind`, `size`, `align`, `is_unsigned`, `is_atomic`.
- `base`: 指针或数组的元素类型.
- `array_len`: 数组长度.
- `members`: 结构体/联合成员链.
- `is_flexible`, `is_packed`.
- `return_ty`, `params`, `is_variadic`: 函数类型.
- `origin`: 兼容性检查用的原始类型.

成员 `LibcpMember` 包含:

- `ty`, `name`, `idx`.
- `offset`, `align`.
- `is_bitfield`, `bit_offset`, `bit_width`.
- `align_ty`, `align_expr`: `_Alignas`.

类型记录上的 `name` / `name_pos` 只在函数参数的 per-parameter 副本上填写. 共享类型单例不写名字. 要拿声明的名字, 用 `Obj.name` 或 `Node.name_tok`.

### 7.3.3 初始化树

`LibcpInitTree` 是解析后的初始化器:

```c
struct libcp_init_tree {
  LibcpType *ty;
  bool is_flexible;
  LibcpNode *expr;              /* 标量初始化表达式 */
  LibcpInitTree **children;     /* 聚合类型的子初始化器 */
  LibcpMember *mem;             /* 联合的活跃成员 */
};
```

它由第一遍的 `resolve_initializer` 建出, 处理:

- 指示符求值.
- 成员名绑定.
- 大括号省略.
- 灵活数组成员定尺寸.

指针放在 `Node.init_resolved`. 这个字段是 sema 私有载荷, 消费者不得读写. 需要初始化数据时, 用 `Obj.init_data` (全局) 或自己从 `Node.decl_init` 重新解析 (局部).

`LibcpInitPath` 是局部初始化器展开时的指示符路径, 供消费者建目标表达式:

```c
struct libcp_init_path {
  LibcpInitPath *next;
  int idx;
  LibcpMember *member;
  LibcpObj *var;
};
```

### 7.3.4 标注树

第 4 层完成后:

- 每个表达式节点有 `ty`.
- `ND_IDENT` 已变成 `ND_VAR` 或 `ND_NUM`.
- `ND_STRING` 已变成 `ND_VAR`.
- 隐式 cast 已插入, 带 `is_implicit`.
- 声明记录仍在语句链上.
- 初始化器树在 `init_resolved`.
- 结论字段已填.

树的形状与第 3 层相同, 除了上述节点种类改写和 cast 插入.

## 7.4 实现流程

### 7.4.1 入口

```c
Obj *sema(Node *toplevel) {
  sema_init();
  declare_builtin_functions();

  for (Node *n = toplevel; n; n = n->next) {
    switch (n->kind) {
    case ND_TYPEDEF:
    case ND_ENUM_CONST:
      resolve_node(n);
      break;
    case ND_GVAR_DECL:
      resolve_node(n);
      serialize_gvar(n);
      break;
    case ND_FUNCDEF:
      resolve_function(n);
      if (n->body)
        analyze_function(n->var);
      break;
    default:
      unreachable();
    }
  }

  finalize_globals();
  return globals;
}
```

顺序很重要:

- 按源码顺序处理顶层记录, 所以后出现的引用能看到前面的声明.
- 函数先 resolve 签名, 再分析函数体, 所以后面的记录能看到函数名.
- 全局变量的初始化器在 resolve 时解析, 在 `serialize_gvar` 时写字节.

### 7.4.2 名字解析

作用域由树结构推导, 不由解析器的栈提供:

- `ND_BLOCK` 且 `is_scope_block`: 开作用域.
- `ND_STMT_EXPR`: 开作用域.
- `ND_FOR`: 开作用域, init 声明和循环体共享.
- 函数体: 开函数作用域, 参数先登记.

`push_scope` / `find_var` 是 sema 自己的哈希表. 它同时存变量和枚举常量.

`bind_ident` 处理 `ND_IDENT`:

```c
static void bind_ident(Node *node) {
  Token *tok = node->tok;
  VarScope *sc = find_var(tok);
  Obj *var = sc ? sc->var : NULL;

  if (var) {
    node->kind = ND_VAR;
    node->var = var;
    node->ty = var->ty;
    return;
  }

  if (sc && sc->enum_ty) {
    node->kind = ND_NUM;
    node->val = sc->enum_val;
    node->ty = ty_int;
    return;
  }

  if (equal(tok->next, "("))
    error_tok(tok, "implicit declaration of a function");
  error_tok(tok, "undefined variable");
}
```

### 7.4.3 声明处理

`ND_DECL` (局部变量):

1. 补全 `ty_op`.
2. 检查 `void` 变量.
3. 建 `Obj`: 普通局部用 `new_lvar`; static 用 `new_anon_gvar`.
4. 把名字登记进当前作用域.
5. 解析初始化器记录, 建 `init_resolved`, 可能补全数组类型.
6. 第二遍标注初始化器表达式.

`ND_GVAR_DECL` (全局变量):

1. 补全 `ty_op`.
2. 建 `Obj`, 设置 `is_definition`, `is_static`, `is_tls`, `is_tentative`, `align`.
3. 解析初始化器.
4. `serialize_gvar` 写 `init_data` 和 `rel`.

`ND_FUNCDEF`:

1. `resolve_function` 处理签名, 参数, 返回类型.
2. 声明函数对象, 处理重声明.
3. 建参数局部, 隐藏返回缓冲, `__va_area__`, `__alloca_size__`, `__func__`, `__FUNCTION__`.
4. 有函数体时 `analyze_function`.

`ND_TYPEDEF`: 补全类型, 登记名字.

`ND_ENUM_CONST`: 求值, 登记名字. 枚举常量在表达式位置出现时, 记录在 `spec_decls` 里, 由 `resolve_enum_records` 处理.

### 7.4.4 类型补全与布局

`resolve_type` 按需补全:

- 数组: 求 `dim_len`, 判断定长还是 VLA, 设置 `array_len` 和 `size`.
- `typeof`: 标注 `typeof_expr`, 取类型.
- 对齐: 求 `align_expr` 或取 `align_ty`.
- 结构体/联合: 如果 `layout_pending`, 调 `layout_struct` / `layout_union`.

`layout_struct` 逐成员:

- 处理 `_Alignas`.
- 处理位域, 包括跨存储单元的规则.
- 计算 `offset`, `align`, `size`.
- 处理灵活数组成员.

`layout_union` 取最大成员, 处理位域和匿名成员.

### 7.4.5 标注与检查

`add_type` 是一个大 switch, 按节点种类定型. 它:

- 递归标注子节点.
- 计算节点类型.
- 插入隐式 cast.
- 跑检查.
- 记录结论.

`add_type` 的每个分支都在 A9.2 里逐项审计过, 确认全部是纯标注, 没有形状改写. 例外是 `ND_IDENT` 和 `ND_STRING` 的绑定改写, 以及显式 cast 的定型.

检查包括:

- 赋值目标的左值性.
- 函数实参个数与类型.
- 位域取地址.
- VLA 不得初始化.
- 不完整类型.
- `break` / `continue` / `case` / `default` 的位置.
- `goto` / label 配对.
- 返回值类型转换.
- `_Generic` 的控制表达式类型.

诊断文案与插入符锚点由 `test/diagnostic.sh` 逐字节锁定, 68 例.

### 7.4.6 全局数据序列化

`serialize_gvar` 把全局初始化器写成字节:

- 标量: 按类型宽度写.
- 数组/结构体: 递归写.
- 字符串: 直接指向字符串内容.
- 地址常量: 写 0, 并追加一条 `LibcpRelocation`.

重定位记录:

```c
struct libcp_relocation {
  LibcpRelocation *next;
  int offset;
  char *label;              /* 已命名的符号 */
  LibcpNode *label_node;  /* &&label, 名字由 codegen 分配 */
  long addend;
};
```

### 7.4.7 收尾

`finalize_globals`:

- 从 `is_root` 函数出发, 标记可达的 static inline 函数.
- 合并重复的暂定定义, 每个名字只留一个.

之后返回 `globals`.

## 7.5 阶段产物

产物分两部分:

1. **原地标注的树**. 每个表达式有类型, 名字已绑定, 结论已记录.
2. **`LibcpObj` 全局链**. 每个全局变量和函数一项, 带类型, 初始化数据, 重定位, 局部对象链, 函数体.

此外:

- 每个聚合类型的成员布局已算好.
- 每个初始化器已解析成 `LibcpInitTree`, 指针在 `init_resolved`.
- 每个 static inline 函数的活跃性已标记.
- goto / label 引用已收集在 context 里, 交给 codegen 配对.

## 7.6 产物可以做什么

- **语义级 linter**: 检查未使用变量, 危险的隐式转换, 返回类型不一致, 等. 需要自己遍历 `Obj.locals` 和标注树.
- **符号表与调用图**: `globals` 链给出全局符号; `ND_FUNCALL` 节点给出调用. `Obj.refs` 记录 static inline 引用.
- **数据布局分析**: `Type.size`, `Member.offset`, `bit_offset` 可以直接用于 ABI 检查, 序列化, 调试器.
- **常量折叠展示**: `ND_SIZEOF.val`, `ND_GENERIC.generic_sel`, 枚举值.
- **类型感知的源到源**: 保留写法, 同时知道每个表达式的类型, 可以做类型驱动的重写.
- **初始化数据检查**: `Obj.init_data` 是最终字节, 可以比较, 校验.
- **自己的后端**: 第 4 层的树和 `Obj` 链足够支撑一个代码生成器. 仓库里的 `codegen.c` 就是例子.

不能做:

- 不能恢复第 3 层的原始 `ND_IDENT` / `ND_STRING` 形态. 它们已经被改写.
- 不能读 `init_resolved` 的内部结构. 它是私有载荷.
- 不能做跨翻译单元分析. 只有当前 TU 的 `globals`.
- 不能拿到 const / volatile / restrict 限定符. 它们被解析后丢弃, 见第 11 章.

## 7.7 API 用法

### 7.7.1 运行 sema

```c
LibcpNode *ast = libcp_parse(ctx, tok);
LibcpObj *prog = libcp_sema(ctx, ast);
if (!prog && libcp_context_failed(ctx)) {
  /* 语义错误 */
}
```

注意: `ast` 被原地标注. `prog` 为 NULL 且 `failed` 为 false 的情况是空翻译单元.

### 7.7.2 遍历全局对象

```c
for (LibcpObj *o = prog; o; o = o->next) {
  if (!o->is_definition)
    continue;

  if (o->is_function) {
    printf("function %s: %d params, %d locals\n",
           o->name, count_params(o->params), count_objs(o->locals));
  } else {
    printf("global %s: size=%d, static=%d, tls=%d\n",
           o->name, o->ty->size, o->is_static, o->is_tls);
  }
}
```

### 7.7.3 读类型布局

```c
static void print_layout(LibcpType *ty) {
  printf("size=%d align=%d\n", ty->size, ty->align);
  for (LibcpMember *m = ty->members; m; m = m->next) {
    printf("  %.*s offset=%d", m->name->len, m->name->loc, m->offset);
    if (m->is_bitfield)
      printf(" bit=%d..%d", m->bit_offset, m->bit_offset + m->bit_width);
    putchar('\n');
  }
}
```

### 7.7.4 读常量结论

```c
if (node->kind == ND_SIZEOF)
  printf("sizeof = %lld\n", (long long)node->val);

if (node->kind == ND_GENERIC && node->generic_sel)
  printf("generic selected expression kind = %d\n",
         (int)node->generic_sel->kind);
```

### 7.7.5 读初始化数据

```c
if (!o->is_function && o->is_definition && o->init_data) {
  for (int i = 0; i < o->ty->size; i++)
    printf("%02x ", (unsigned char)o->init_data[i]);
  putchar('\n');
}
```

`init_data` 的长度由 `o->ty->size` 决定. 重定位用 `o->rel` 链.

### 7.7.6 先保留第 3 层形态

如果既要第 3 层的忠实形态, 又要第 4 层的类型, 有两个办法:

1. 在 sema 之前调用 `-dump-ast` 或自己的 dump, 留一份文本.
2. 对同一个文件重新 tokenize / preprocess / parse, 走第二条管线, 再 sema.

库不提供深拷贝树的 API. 第 4 层的改写是原地的.

## 7.8 边界与已知限制

- **原地改写**. `ND_IDENT` 变成 `ND_VAR` / `ND_NUM`, `ND_STRING` 变成 `ND_VAR`. 原始形态在 sema 后不再存在.
- **`init_resolved` 是私有字段**. 消费者不能读它的结构. 需要初始化数据时用 `Obj.init_data` 或自己解析 `decl_init`.
- **限定符丢失**. `const`, `volatile`, `restrict` 在解析时被消费, 不进入类型记录. `_Atomic` 有 `is_atomic`. 依赖限定符的工具需要自己从 token 或 CST 读.
- **没有源码范围**. 节点只有代表 token.
- **没有跨 TU 信息**. 只有当前 TU.
- **没有错误恢复**. 第一个语义错误就返回.
- **诊断位置有限**. 宏内错误可能指向宏定义而不是调用点. `#line` 改行号但不改文件名.
- **`globals` 链包含声明和匿名对象**. 不过滤 `is_definition` 会看到 `extern` 声明和 `__func__` 字符串.
- **`globals` 顺序是创建顺序的逆序**. 不要依赖.
- **`alloca` 不在 `globals` 里**. 它单独登记, 只用于 VLA 降级.
- **布局规则不是平台无关的**. `long double`, 位域, 对齐按 x86-64 System V 实现. 做跨平台分析要注意.
- **常量求值有平台假设**. 整数宽度, `long double` 格式, 指针大小按 LP64.
- **静态 inline 活跃性**. `is_live` 只对 static inline 函数有意义, 其他函数不保证被标记.
