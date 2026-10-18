# DUMP-AST-PLAN.md - 临时 faithful AST dump 通道(路线图第 3 层)

目标: 增加 `--dump-ast`(同时接受 `-dump-ast`), 在 `parse()` 之后, `sema()` 之前,
把忠实语法树打印成文本. 这是路线图第 3 层的只读观察面, 用于检查 parse 建出的树是否
保留源码写法, 以及每种 `NodeKind` 的实际字段.

这是一次性调试特性: 用户只需要观察实现效果, 之后整体移除. 因此设计优先级是:

1. 侵入面最小: 改动集中在 1 个新文件 + `main.c` 的 4 个小 hunk + `chibicc.h` 的 1 行声明.
2. 可干净移除: 没有长期兼容负担, 没有数据/API 迁移.
3. 不改编译路径语义: `parse` / `sema` / `codegen` / `type` / `tokenize` / `preprocess`
   零改动.

## 非目标

- 不做第 4 层(typed / annotated AST)的 dump, 不打印 sema 结论.
- 不导出公共库 API, 不参与路线图阶段 5 的头文件与 context 改造.
- 不做源码回写, 不做宏调用保真. 树建立在**预处理之后**的 token 流上, 宏已展开,
  看不到宏调用本身.
- 不更新 `AGENTS.md` / `PLAN.md` / `RESULT.md`, 不接入 `make test`, 不新增 Makefile 目标.
- 不承诺 dump 格式长期稳定. 测试只锁本次样本与本次 coverage 列表.

## 判据

- 只读: dump 只遍历 `Node` / `Type` / `Initializer`, 不写任何字段.
- 只走前端: 只调用 `tokenize_file` / `preprocess` / `parse`; 不调用 `sema` / `codegen`.
- 无新增 `exit`, 无新增文件域 static. dump 的状态全部在栈上.
- 编译路径行为不变: 未给出 `--dump-ast` 时, `cc1` 的调用序列与现在相同; 汇编快照
  diff 必须为空.
- 输出确定: 不含地址, 时间, 绝对路径; 同一输入同一输出.

## 改动面

| 文件 | 改动 | 移除时 |
|---|---|---|
| `ast_dump.c` | 新增, 约 450-550 行, 自包含 | 删除 |
| `chibicc.h` | 1 行原型 + 1 行注释 | 删除 |
| `main.c` | 4 个 hunk: option 变量 / parse_args / cc1 / driver 分支 | 回退 |
| `test/ast.sh` | 新增临时测试脚本, 手动运行 | 删除 |
| `DUMP-AST-PLAN.md` | 本文件 | 删除 |

Makefile **不需要改**: `SRCS=$(wildcard *.c)`, 新文件自动进入 `chibicc` 与
`stage2/chibicc` 的编译. 这也意味着自举会强制编译 `ast_dump.c`, 是一个免费的前端
语法兼容性检查.

### main.c 的四个 hunk

1. option 变量, 放在现有 dump 变量旁(main.c:22-25):

```c
static bool opt_dump_ast;
```

2. `parse_args` 的识别分支, 放在 `-dump-cst` 等分支旁(main.c:188 附近):

```c
if (!strcmp(argv[i], "--dump-ast") ||
    !strcmp(argv[i], "-dump-ast")) {
  opt_dump_ast = true;
  continue;
}
```

主拼写用 `--dump-ast`(用户指定), `-dump-ast` 作为与现有通道一致的别名. 该选项不
带参数, `take_arg` 不需要改.

3. `cc1` 的解析与早退, 替换 main.c:749 的单行调用:

```c
  Node *toplevel = parse(tok);

  if (opt_dump_ast) {
    dump_ast(toplevel, open_file(opt_o ? opt_o : "-"));
    return 0;
  }

  Obj *prog = sema(toplevel);
```

位置在 `-M` / `-E` 的早退之后, 因此:

- `-E` 优先, 打印预处理结果, 不 dump AST.
- `-M` 优先, 打印依赖后返回.
- `-MD` 先写依赖文件, 再 dump AST. `-MD` 与 `-o` 同给时依赖文件与 dump 争用同一
  路径, 不要组合使用.
- `--dump-ast` 优先于 `-S` / `-c` / 链接, 不写目标文件.

这是唯一需要重排的编译路径语句, 语义等价: 未开 dump 时, `sema` 收到的仍是
`parse(tok)` 的结果.

4. driver 的透传条件, main.c:956:

```c
  if (opt_E || opt_M || opt_dump_tokens || opt_reconstruct ||
      opt_dump_cst || opt_reconstruct_cst || opt_verify_cst ||
      opt_dump_ast) {
    run_cc1(argc, argv, input, NULL);
    continue;
  }
```

这样 driver 不会进入汇编/链接阶段. 多输入文件时每个输入各 dump 一次; 不给 `-o` 时
全部写 stdout, 给 `-o` 时后一个输入会覆盖前一个. 本特性按单文件使用.

### chibicc.h

放在 CST 声明之后(chibicc.h:223):

```c
// Temporary debug channel (ast_dump.c); see DUMP-AST-PLAN.md.
void dump_ast(Node *toplevel, FILE *out);
```

## ast_dump.c 结构

自包含, 只 `#include "chibicc.h"`. 不调用其他编译器内部函数(除了 token 字段读取),
这样移除时不会牵连任何其他文件.

```c
void dump_ast(Node *toplevel, FILE *out);          // 入口: 遍历顶层声明链

static void dump_node(Node *node, FILE *out, int depth);
static void dump_chain(Node *node, FILE *out, int depth);
static void dump_init(Initializer *init, FILE *out, int depth);
static void dump_init_item(InitItem *item, FILE *out, int depth);
static void dump_desig(InitDesig *desig, FILE *out, int depth);
static void dump_type(Type *ty, FILE *out, int depth);   // 写一行 type="..."
static const char *kind_name(NodeKind kind);
static void print_escaped(FILE *out, char *p, int len);
```

实现约束:

- 无内存分配, 无文件域状态; `dump_type` 用固定缓冲(例如 512 字节)拼装后写字段,
  超过深度上限(8)打印 `...`.
- `dump_node` 只打印当前节点和它的子节点; `next` 由 `dump_chain` 展开. 顶层链,
  语句链, 声明链, 参数链都走 `dump_chain`.
- 所有 `NodeKind` 在 `kind_name` 中都有名字. 兜底打印 `ND_<整数>`; 测试断言输出中
  不出现 `ND_<`.
- `ND_NULL_EXPR` / `ND_VAR` / `ND_VLA_PTR` / `ND_MEMZERO` 是 sema 才产生的 kind.
  第 3 层 dump 不会出现; 测试断言 `ND_VAR` 不出现, 以证明 dump 在 sema 之前.
- 不为了 dump 往 `parse.c` 加字段或 helper. 字段拿不到就少打印, 不扩公共结构.

## 输出格式

### 基本规则

- 缩进: 每层两个空格.
- 每个节点一行, 行首是 `ND_*` 种类名.
- 行内字段: `key=value`, 空格分隔. 字符串值一律用双引号并转义; 数字/枚举值不加引号.
- 子节点: 先打印一行 `<label>:`(比父节点深一层), 子节点再深一层. 同一 label 下
  的链表项是兄弟, 不重复 label.
- 空/NULL 子节点不打印 label 行.
- token 拼写作为 `tok="..."` 打印在行尾; 使用与 `cst.c` 相同的转义规则(复制一份
  `print_escaped`, 刻意不在这次改动里导出 cst 的 static 函数).
- 不打印行号, 列号, 文件名, 地址.

### 字段与子节点表

| 节点/组 | 行内字段 | 子节点 label |
|---|---|---|
| `ND_ADD` `ND_SUB` `ND_MUL` `ND_DIV` `ND_MOD` `ND_BITAND` `ND_BITOR` `ND_BITXOR` `ND_SHL` `ND_SHR` `ND_EQ` `ND_NE` `ND_LT` `ND_LE` `ND_GT` `ND_GE` `ND_LOGAND` `ND_LOGOR` `ND_COMMA` | `tok` | `lhs:`, `rhs:` |
| `ND_ASSIGN` | `op=ND_*`(仅复合赋值), `tok` | `lhs:`, `rhs:` |
| `ND_SUBSCRIPT` | `tok` | `base:`, `index:` |
| `ND_NEG` `ND_ADDR` `ND_DEREF` `ND_NOT` `ND_BITNOT` | `tok` | `expr:` |
| `ND_INCDEC` | `post=1`(后缀), `addend=1/-1`, `tok` | `expr:` |
| `ND_CAST` | `ty_op="..."`, `tok` | `expr:` |
| `ND_MEMBER` | `name="..."`, `arrow=1`(仅 `->`), `tok` | `base:` |
| `ND_FUNCALL` | `tok` | `callee:`, `args:` |
| `ND_COND` | `elvis=1`(仅 `a ?: b`), `tok` | `cond:`, `then:`, `else:` |
| `ND_SIZEOF` `ND_ALIGNOF` | `ty_op="..."`(类型形式), `tok` | `expr:`(表达式形式) |
| `ND_GENERIC` | `tok` | `controlling:`, `assocs:` |
| `ND_GENERIC_ASSOC` | `default=1` 或 `ty_op="..."` | `result:` |
| `ND_TYPES_COMPATIBLE` | `ty_op="..."`, `ty_op2="..."` | - |
| `ND_REG_CLASS` | `ty_op="..."` | - |
| `ND_COMPOUND_LITERAL` | `ty_op="..."` | `init:` |
| `ND_IDENT` | `name="..."`, `tok` | - |
| `ND_NUM` | `val=<整数>` 或 `fval=<浮点>`, `ty="..."`, `tok` | - |
| `ND_STRING` | `tok` | - |
| `ND_RETURN` | `tok` | `expr:` |
| `ND_IF` | `tok` | `cond:`, `then:`, `else:` |
| `ND_FOR` | `tok` | `init:`, `cond:`, `inc:`, `body:` |
| `ND_WHILE` | `tok` | `cond:`, `body:` |
| `ND_DO` | `tok` | `body:`, `cond:` |
| `ND_SWITCH` | `tok` | `cond:`, `body:` |
| `ND_CASE` | `default=1`(仅 default), `tok` | `begin:`, `end:`(仅 range), `body:` |
| `ND_BLOCK` | `scope=1`(仅 compound-stmt) | `body:` |
| `ND_GOTO` | `label="..."`, `tok` | - |
| `ND_GOTO_EXPR` | `tok` | `expr:` |
| `ND_BREAK` `ND_CONTINUE` | `tok` | - |
| `ND_LABEL` | `label="..."`, `tok` | `body:` |
| `ND_LABEL_VAL` | `label="..."`, `tok` | - |
| `ND_ASM` | `asm="..."`, `tok` | - |
| `ND_EXPR_STMT` | `tok` | `expr:` |
| `ND_STMT_EXPR` | `tok` | `body:` |
| `ND_DECL` `ND_GVAR_DECL` | `name="..."`, `ty_op="..."`, attr 标记, `tok` | `init:`, `spec_decls:` |
| `ND_FUNCDEF` | `name="..."`, `ty_op="..."`, attr 标记, `tok` | `body:`, `spec_decls:` |
| `ND_TYPEDEF` | `name="..."`, `ty="..."`, `tok` | - |
| `ND_ENUM_CONST` | `name="..."`, `ty_op="..."`, `tok` | `value:` |
| `ND_VAR` `ND_VLA_PTR`(sema 才有) | `name="..."`(有 var 时) | - |
| `ND_NULL_EXPR` `ND_MEMZERO`(sema 才有) | - | - |

attr 标记只打印 parser 读到的顶层信息: `typedef=1`, `static=1`, `extern=1`,
`inline=1`, `tls=1`, `align=<n>`. `_Alignas(<expr>)` 与 `_Alignas(<type>)` 的
表达式/类型形式分别用 `align_expr:` 与 `align_ty="..."` 打印.

### 类型表示

只打印类型脊柱, 不展开 struct/union 成员列表, 避免自引用类型无限递归. 深度上限 8,
超过打印 `...`.

| 类型 | 表示 |
|---|---|
| `TY_VOID` | `void` |
| `TY_BOOL` | `_Bool` |
| `TY_CHAR` / `TY_SHORT` / `TY_INT` / `TY_LONG` | `char` / `short` / `int` / `long`, `is_unsigned` 加 `unsigned ` 前缀 |
| `TY_FLOAT` / `TY_DOUBLE` / `TY_LDOUBLE` | `float` / `double` / `long double` |
| `TY_ENUM` | `enum` |
| `TY_STRUCT` / `TY_UNION` | `struct` / `union` |
| `TY_PTR` | `<base> *` |
| `TY_ARRAY` | `<base>[<len>]`. `dim_len` 是 `ND_NUM` 时打印其 `val`, 否则 `?`; `array_len < 0` 且无 `dim_len` 时打印 `[]` |
| `TY_VLA` | `<base>[*]` |
| `TY_TYPEOF` | `typeof(?)` |
| `TY_FUNC` | `<ret> (<params>)`. 无参数且 `!is_variadic` 打印 `(void)`; 无参数且 `is_variadic` 打印 `()`(该层无法区分 `()` 与 `(...)`); 有参数时逗号分隔, 参数有名字时打印 `<type> <name>`, 变参追加 `...` |

tag 名在 parse 的 tag scope 里, 不在 `Type` 上; 本通道不读 parse 的内部表, 所以
`struct` / `union` / `enum` 只打印关键字, 不打印 tag. 这是第 3 层数据模型的限制,
不是 dump 的遗漏.

`is_atomic` 为真时加 `_Atomic ` 前缀.

### 初始化器表示

```text
INIT_EXPR
  expr:
    <Node>
INIT_LIST
  item:
    desigs:
      INIT_DESIG kind="index"
        begin:
          <Node>
    value:
      <Initializer>
INIT_STR tok="\"abc\""
```

- `INIT_DESIG` 的 `kind` 为 `index` / `range` / `member`.
- `range` 有 `begin:` 与 `end:` 两个子节点; `member` 打印 `name="..."`; `index`
  只有 `begin:`.
- `INIT_LIST` 的每个元素先打印 `item:`, 其下按需打印 `desigs:` 与 `value:`.

### 示例

输入:

```c
int g = 1;
int main(void) {
  int x = 2;
  return x + g;
}
```

输出:

```text
ND_GVAR_DECL name="g" ty_op="int"
  init:
    INIT_EXPR
      expr:
        ND_NUM val=1 ty="int" tok="1"
ND_FUNCDEF name="main" ty_op="int (void)"
  body:
    ND_BLOCK scope=1
      body:
        ND_DECL name="x" ty_op="int"
          init:
            INIT_EXPR
              expr:
                ND_NUM val=2 ty="int" tok="2"
        ND_RETURN
          expr:
            ND_ADD
              lhs:
                ND_IDENT name="x" tok="x"
              rhs:
                ND_IDENT name="g" tok="g"
```

## 实现步骤

- [ ] **D1 - 骨架与通道**: 新增 `ast_dump.c`, 实现 `kind_name` / `dump_node` /
      `dump_chain` / `print_escaped`; 只打印顶层声明记录与 `ND_FUNCDEF` 的 body 骨架.
      接入 `main.c` 的 4 个 hunk 与 `chibicc.h` 的 1 行声明. `make` 通过,
      `./chibicc --dump-ast` 能打印最简单的 `int main(void) {}`.
- [ ] **D2 - 表达式与语句**: 补齐二元/一元/后缀/条件/赋值/自增自减/类型转换/
      函数调用/成员/下标, 以及 if/for/while/do/switch/case/block/goto/label/
      return/break/continue/asm/expr-stmt/stmt-expr. 用自包含样本逐类目视.
- [ ] **D3 - 声明与初始化器**: 补齐 ND_DECL / ND_GVAR_DECL / ND_FUNCDEF /
      ND_TYPEDEF / ND_ENUM_CONST / ND_COMPOUND_LITERAL 的字段与 `decl_init` 树;
      补齐 `spec_decls` 链; 补齐 `dump_type`.
- [ ] **D4 - 临时测试**: 写 `test/ast.sh`(手动运行, 不接入 `make test`), 包含
      一个 exact-diff 样本, 若干关键行断言, `ND_<` 缺席断言, `ND_VAR` 缺席断言,
      以及 65 个 parse 可产生 kind 的 coverage 列表.
- [ ] **D5 - 闸门与收口**: 跑本机测试与 docker 回归; 回填本文件的执行记录;
      单独提交.

## 测试计划

### 样本

`test/ast.sh` 用临时目录下的自包含 C 文件, 不依赖系统头文件, 因此可在 macOS 本机跑.
样本覆盖:

1. 全局变量, 数组, 指针, struct/union/enum, typedef, 函数原型与定义.
2. 局部声明, 标量/数组/字符串初始化器, designated initializer, 复合字面量.
3. 二元/一元/后缀(`a[i]` / `f(x)` / `p->m` / `i++`)/条件/逗号/赋值/复合赋值/
   自增自减/显式 cast. 二元样本用一条
   覆盖全部运算符的表达式, 例如 `a+b-c*d/e%f & g | h ^ i << j >> k == l != m
   < n <= o > p >= q && r || s`.
4. struct 成员 `.` 与 `->`, 函数调用与参数链.
5. if/else, for(含声明 init), while, do, switch/case/default/range, break/continue.
6. goto, label, labels-as-values(`&&L`), `goto *p`.
7. `sizeof(type)` / `sizeof expr` / `_Alignof(type)` / `_Alignof expr`.
8. `_Generic`(typed 与 default), `__builtin_types_compatible_p`,
   `__builtin_reg_class`.
9. `__builtin_compare_and_swap`, `__builtin_atomic_exchange`.
10. GNU statement expression, `asm("...")`.
11. VLA 形式 `int a[n];`(parse 阶段只记录维度表达式, 不求值).
12. `typeof(x)` 声明(若 parse 路径可稳定产生 `TY_TYPEOF`).

### 断言

- exact diff: 一个最小程序的完整 dump 与期望文本逐字节相等, 锁格式.
- 关键行: 每个类目至少一个 `grep -qx` / `grep -q` 断言.
- `ND_<` 不出现: `kind_name` 的兜底分支没有被触发.
- `ND_VAR` / `ND_NULL_EXPR` / `ND_VLA_PTR` / `ND_MEMZERO` 不出现: 证明在 sema 之前.
- coverage: 把所有 dump 拼起来, 逐一检查 65 个 parse 可产生的 kind 名至少出现一次.
  65 个 kind = 69 个 `NodeKind` 减去 4 个 sema-only
  (`ND_NULL_EXPR` / `ND_VAR` / `ND_VLA_PTR` / `ND_MEMZERO`).
- `-o` 输出: `./chibicc --dump-ast -o out.c in.c` 写文件, stdout 为空.
- 别名: `-dump-ast` 与 `--dump-ast` 输出一致.

### 运行方式

本机(前端可跑, 不需要 Linux 后端):

```sh
make
./test/ast.sh ./chibicc
```

Docker(回归 + 自举 + 全语料 smoke):

```sh
make docker-test
make docker-snapshot-diff
```

全语料 smoke 需要 Linux 头文件, 因此在容器里跑:

```sh
for f in test/*.c; do
  ./chibicc -Iinclude -Itest --dump-ast "$f" > /dev/null || exit 1
done
```

自举产物也跑一遍 AST 样本:

```sh
./test/ast.sh ./stage2/chibicc
```

自举本身已由 `make docker-test` 覆盖: `ast_dump.c` 会被 `./chibicc` 编译进
`stage2/chibicc`, 所以 `make test-stage2` 同时验证了新文件能被 chibicc 编译.

### coverage 列表(65 个 parse 可产生 kind)

表达式(45):

`ND_ADD` `ND_SUB` `ND_MUL` `ND_DIV` `ND_NEG` `ND_MOD` `ND_BITAND` `ND_BITOR`
`ND_BITXOR` `ND_SHL` `ND_SHR` `ND_EQ` `ND_NE` `ND_LT` `ND_LE` `ND_GT` `ND_GE`
`ND_ASSIGN` `ND_INCDEC` `ND_SUBSCRIPT` `ND_COND` `ND_COMMA` `ND_MEMBER` `ND_ADDR`
`ND_DEREF` `ND_NOT` `ND_BITNOT` `ND_LOGAND` `ND_LOGOR` `ND_FUNCALL`
`ND_STMT_EXPR` `ND_IDENT` `ND_NUM` `ND_STRING` `ND_SIZEOF` `ND_ALIGNOF`
`ND_GENERIC` `ND_GENERIC_ASSOC` `ND_TYPES_COMPATIBLE` `ND_REG_CLASS` `ND_CAST`
`ND_COMPOUND_LITERAL` `ND_CAS` `ND_EXCH` `ND_LABEL_VAL`

语句(15):

`ND_RETURN` `ND_IF` `ND_FOR` `ND_WHILE` `ND_DO` `ND_SWITCH` `ND_CASE` `ND_BLOCK`
`ND_GOTO` `ND_BREAK` `ND_CONTINUE` `ND_GOTO_EXPR` `ND_LABEL` `ND_EXPR_STMT` `ND_ASM`

声明(5):

`ND_DECL` `ND_GVAR_DECL` `ND_FUNCDEF` `ND_TYPEDEF` `ND_ENUM_CONST`

sema-only, 测试断言不出现(4):

`ND_NULL_EXPR` `ND_VAR` `ND_VLA_PTR` `ND_MEMZERO`

## 风险与对策

1. **漏掉某个 NodeKind**: `-Wno-switch` 不会报缺失 case. 对策 = `kind_name` 覆盖
   全部 69 个枚举值, 兜底输出 `ND_<n>`, 测试断言 `ND_<` 不出现; coverage 列表
   覆盖 65 个 parse 可产生的 kind.
2. **类型递归**: `struct S { struct S *next; }` 会让展开成员列表的实现无限递归.
   对策 = 只打印类型脊柱, 不展开 struct/union 成员; 深度上限 8.
3. **指针/数组维度误读**: parse 阶段的数组维度在 `dim_len`, `array_len` 通常为 -1,
   即使源码写的是 `[3]`. 对策 = 优先看 `dim_len`, `ND_NUM` 时打印其值, 否则 `?`;
   不要打印误导性的 `array_len`.
4. **转义规则漂移**: dump 的 token 文本必须与 `cst.c` 的可读性一致. 对策 = 复制
   `print_escaped` 的实现; 这是为可移除性刻意接受的重复.
5. **浮点格式**: `%Lg` 在不同 libc 下可能有末位差异. 对策 = 浮点样本只断言源拼写
   `tok`, 不断言 `fval` 的具体文本; 整数样本可以断 `val`.
6. **dump 过大**: 全语料 dump 可能几十 MB. 对策 = smoke 重定向到 `/dev/null`;
   不把 dump 写进测试日志.
7. **驱动优先级**: `-E` / `-M` 与 `--dump-ast` 同时给出时的行为必须在脚本里固定,
   避免以后误以为 dump 一定执行.
8. **删除后的 stale object**: 删掉 `ast_dump.c` 后, `ast_dump.o` 可能留在目录里.
   对策 = 移除后 `make clean` 再重建; `OBJS` 由 wildcard 计算, 不会把 stale
   object 链接进新二进制.

## 移除步骤

首选: 本特性作为单独一个 commit 提交, 移除时直接

```sh
git revert <dump-ast-commit>
```

即可. 该 commit 只包含:

- `ast_dump.c`(新增)
- `chibicc.h`(2 行)
- `main.c`(4 个 hunk)
- `test/ast.sh`(新增)
- `DUMP-AST-PLAN.md`(新增)

若没有按单 commit 提交, 手工移除:

1. 删除 `ast_dump.c` / `test/ast.sh` / `DUMP-AST-PLAN.md`.
2. 回退 `chibicc.h` 的 `dump_ast` 声明与注释.
3. 回退 `main.c` 的 4 个 hunk, 恢复
   `Obj *prog = sema(parse(tok));`.
4. `make clean && make`.
5. `rg -n 'dump_ast|dump-ast|DUMP-AST'` 确认无残留.
6. 跑 `make docker-test` 与 `make docker-snapshot-diff` 确认回到基线.

不需要处理: Makefile 条目(不存在), 文档引用(只有本文件), 序列化格式, 库 API,
测试数据.

## 终态验收

- `make` 通过, `./chibicc --dump-ast` 对 12 类样本输出可读树, `-o` 可重定向.
- `./test/ast.sh ./chibicc` 全绿, 65 个 parse 可产生 kind 全覆盖, `ND_<` 与
  sema-only kind 均不出现.
- `make docker-test` EXIT=0(含自举), `make docker-snapshot-diff` 为
  `snapshot diff: empty`, 诊断 55 例 / 形状 33 段 / trivia / CST 锁全部不变.
- Docker 内 41 个 `test/*.c` 语料文件 `--dump-ast` smoke 全部正常退出;
  `stage2/chibicc` 的
  AST 样本通过.
- `git diff --stat` 只包含本文件列出的 5 个文件.
- 按"移除步骤"演练后, `rg` 无残留, 重新构建与回归通过.

## 执行记录 (2026-10-18)

D1-D5 已执行完毕, 全部闸门通过.

改动面: `ast_dump.c` 新增 806 行; `chibicc.h` +3 行; `main.c` 4 个 hunk
(+19/-2); `test/ast.sh` 新增 390 行 (222 项断言). `parse.c` / `sema.c` /
`codegen.c` / `type.c` / `tokenize.c` / `preprocess.c` 零改动.

与计划的偏离 (均为输出格式细节, 判据与改动面不变):

1. 声明记录 (`ND_DECL` / `ND_GVAR_DECL` / `ND_FUNCDEF`) 不打印 `tok`.
   节点的 `tok` 是 declarator 的起点 (函数定义处是 `{`, 原型处是 `;`), 不是
   名字; 计划示例也省略了这三个节点的 `tok`. 名字由 `name=` 打印.
2. `tok="..."` 的转义在 `cst.c` 的规则上多转义双引号 (`\"`), 与计划示例
   `INIT_STR tok="\"abc\""` 一致; 其余 (空格 -> `\x20`, 控制字符 -> `\xHH`)
   相同.
3. `ND_CAS` / `ND_EXCH` 的字段计划表未列, 实现为 `addr:` / `old:` / `new:`
   与 `lhs:` / `rhs:`.
4. 多维数组按计划的 `<base>[<len>]` 递归表示, 因此 `int a[2][3]` 打印
   `int[3][2]` (内层类型在前); 这是类型脊柱而不是 C 的书写顺序.
5. `VarAttr.align` 在 parse 阶段从不赋值 (`_Alignas` 记录为 `align_expr` /
   `align_ty`), 所以 `align=` 实际不会出现; 保留代码路径以对应字段.

验证:

- 本机: `make` 通过; `./test/ast.sh ./chibicc` 222 项断言全绿, 65 个 parse
  可产生的 kind 全覆盖, `ND_<` 与四个 sema-only kind 均不出现.
- `make docker-test` EXIT=0 (含自举), 诊断 55 例 / 形状 33 段 / trivia / CST
  锁全部不变.
- `make docker-snapshot-diff`: `snapshot diff: empty`.
- 容器内 41 个 `test/*.c` 的 `--dump-ast` smoke 全部正常退出.
- stage1 与 stage2 (自举产物) 各跑 `test/ast.sh`, 各 222 项通过.

