# CodeReview-Result - 全项目缺陷与框架审计

审计基线: `603df24` (2026-10-19)
审计范围: 全部实现源文件与公共头, 共 15893 行 - `sema.c` 3043, `codegen.c` 2776, `cst.c` 2255, `parse.c` 2154, `preprocess.c` 1366, `tokenize.c` 1005, `main.c` 975, `lib.c` 404, `unicode.c` 189, `provenance.c` 182, `hashmap.c` 165, `type.c` 157, `strings.c` 86, 内部 umbrella `chibicc.h` 382, 以及 `libchibicc/include/chibicc/` 的 9 个公共头.
不在范围: `test/`(按用户要求排除, 仅在判断"闸门是否覆盖"时引用其存在性), `include/*.h`(自举头文件), `Makefile`.

## 方法与边界

本文全部结论来自静态阅读, **没有构建, 没有运行任何测试或复现用例**(本工作区为 macOS ARM64, 完整闸门需 docker). 因此每条结论标注确定性:

- `[实锤]` - 从代码可直接推出, 不依赖运行时行为.
- `[需复现]` - 推导链完整, 但建议补一个最小用例确认.
- `[中置信]` - 需要专门验证才能定性.
- `[latent]` - 当前无调用点触发, 但契约无防护, 属埋雷.

行号以基线 commit 为准.

## 分级总览

| 级别 | 数量 | 条目 |
| --- | --- | --- |
| S (内存安全 / 崩溃 / UB) | 3 | B1, B2, B3 |
| A (框架契约与公开承诺冲突) | 8 | F1-F8 |
| B (C 保真与诊断缺口) | 6 | B4-B10 |
| C (次要 / 性能 / 风格) | 12 | 见"次要项" |

---

# 第一部分: S 级缺陷

## B1. u8 字符串字面量拼接: 死分支导致 arena 越界写与内容静默丢失 `[需复现]`

**根因**: `getStringKind`(preprocess.c:1218-1229)用 `strcmp(tok->loc, "u8")` 判断 UTF-8 前缀. 但 TK_STR 的 `loc` 指向 `u8"abc"` 的 `u`, 其后紧跟 `8` 与开引号, 所以 `strcmp` 在第 3 字节(`'"'` vs `'\0'`)必然非 0 - **`STR_UTF8` 是死分支**, 所有 u8 前缀落进 `switch (tok->loc[0])` 的 `case 'u'` 被当成 UTF-16.

**后果一: `u8"a" u"b"` → arena 越界写 2 字节**

- `u8"a"` 经 `read_string_literal` 得 `array_of(ty_char, 2)`: base size 1, array_len 2, ty->size 2.
- `u"b"` 经 `read_utf16_string_literal` 得 `array_of(ty_ushort, 2)`: base size 2, array_len 2, ty->size 4.
- 第一遍(preprocess.c:1237-1268): `kind = STR_UTF16`(错), `basety = ty_char`; 对 `u"b"` 求得 `k = STR_UTF16`(错), 于是 `kind == k` **不报错**; `basety->size == 1` 故**不触发重扫**.
- 第二遍(preprocess.c:1271-1298): `len = 2 + (2 - 1) = 3`, `buf = chibicc_arena_calloc(1, 3)`; 第一次 `memcpy(buf+0, str, 2)` 后 `i = 1`; 第二次 `memcpy(buf+1, str, 4)` 写 `buf[1..4]` - **3 字节缓冲写入 4 字节, 越界 2 字节**.

正确行为应是 C11 6.4.5p5 的约束违反诊断(`u8` 只能与无前缀或 `u8` 拼接), 即 "unsupported non-standard concatenation of string literals". 死分支把一条必需诊断变成了内存安全 bug.

**后果二: `u"a" u8"b"` → 内容静默丢失**

- 第一遍: `kind = STR_UTF16`, `basety = ty_ushort`(size 2); `u8"b"` 的 `k` 同样被误判为 `STR_UTF16` → `kind == k` **不报错**; 但 `basety->size > 1` 触发重扫, 对 base size 为 1 的 `u8"b"` 调 `tokenize_string_literal`(tokenize.c:581-602)→ `read_utf16_string_literal(tok->loc, tok->loc, ...)`, 即 `quote` 指向 `u`, `string_literal_end(quote + 1)` 从 `8` 开始找 `"`, **立即命中开引号** → `len = 0`, 字符 `b` 被丢弃.
- 结果是一个语义错误的字符串字面量, 无任何诊断.

**连带影响**: 该越界发生在 arena 块内部(见 F4), 现有 ASan 闸门看不见.

**修复方向**: `getStringKind` 改为按前缀实际长度判断(`tok->len >= 3 && tok->loc[0]=='u' && tok->loc[1]=='8'`), 并在 test 语料补 `u8`+`u`/`U`/`L` 的四组组合.

## B2. `ND_EXCH` 诊断锚点空指针解引用 `[实锤]`

sema.c:2185-2189:

```c
case ND_EXCH:
  if (node->lhs->ty->kind != TY_PTR)
    error_tok(node->cas_addr->tok, "pointer expected");
```

`__builtin_atomic_exchange` 在 parse.c:1971-1979 只设置 `lhs` / `rhs`, **从不设置 `cas_addr`**(那是 `ND_CAS` 的字段). 节点由 `chibicc_arena_calloc` 清零分配, 故 `cas_addr` 恒为 NULL → `NULL->tok` 解引用 → **段错误而非诊断**.

触发极简: `int a, b; __builtin_atomic_exchange(a, b);`

这直接破坏库化线 L2.6 建立的错误模型("一切错误经诊断回调 + longjmp 返回失败值", context.h:34-35 也承诺 "An error leaves the context inconsistent"). 修复: 锚点改为 `node->lhs->tok`(与 `ND_CAS` 分支的写法对齐).

## B3. 位域宽度无校验 → 编译期 UB 移位与非法汇编 `[实锤]`

`eval_bitfield_widths`(sema.c:672-679)直接 `mem->bit_width = eval(mem->width_expr)`, 不检查:

- 宽度是否超过承载类型的位宽(`int x : 100`)
- 宽度是否为负(`int x : -1`)
- 承载类型是否为整型(`float x : 3`, 指针位域)

下游三处直接消费该值:

- sema.c:1535 `(1L << mem->bit_width) - 1` - 全局初始化器序列化路径, `bit_width >= 64` 或为负时是 **UB 移位**, 且发生在编译期.
- codegen.c:968 `shl $(64 - bit_width - bit_offset)` 与 codegen.c:1002 `and $((1L << bit_width) - 1)` - 同样 UB, 且移位计数为负时产出汇编器拒收的指令, 失败点漂移到 `as`.
- sema.c:697-723 `layout_struct` 的 `bits`/`bit_offset` 计算在超宽位域下溢出, 布局结论错误.

C11 6.7.2.1p4 是约束违反, 要求诊断. 现有"零 UB"闸门(L3.4c 的 ASan+UBSan 重编库对象)未覆盖, 因为语料中没有病态位域. 修复: 在 `eval_bitfield_widths` 里加范围与整型检查并 `error_tok` 到 `mem->width_expr->tok` 或 `mem->tok`.

---

# 第二部分: A 级框架问题

## F1. 全局类型单例是共享可变状态, 与公开的线程承诺冲突 `[实锤]`

type.c:3-18 把 `ty_void` .. `ty_ldouble` 定义为文件级非 const 单例:

```c
Type *ty_int = &(Type){TY_INT, 4, 4};
```

而 parse.c:586-610 的 `declarator` 在直通路径上**直接写单例**:

```c
ty = type_suffix(rest, tok, ty);
ty->name = name;
ty->name_pos = name_pos;
```

对 `int x;` 而言 `declspec` 返回的就是 `ty_int` 指针本身(无 `*` / `[]` / `()` 时 `pointers` 与 `type_suffix` 原样返回), 所以 `ty_int->name` 被写入. parse.c:743-745 的注释自认这是窗口语义("The name goes on the shared type only until the next declarator overwrites it"), 调用方(`declaration`:746, `struct_members`:1574, `function_def`:2053, `global_variable`:2094)立即读走.

问题在于 context.h:3-4 公开承诺:

> Different contexts may live in different threads; one context must not be used concurrently from two threads.

两个 context 在不同线程并发 parse 时, 对同一单例 `name` / `name_pos` 字段的写是 data race(C11 5.1.2.4p25 UB). 单线程下也有公共面问题: `ChibiccType.name` 是公开字段(ast.h:219-220), 消费者任意时刻读到的都是"上一个 declarator 的残留", `chibicc_context_reset` 后更是指向已释放 arena 的悬垂指针.

这是 AGENTS.md "不新增静态全局状态 / 语义状态全部收进 `ChibiccContext`" 这条红线**唯一未覆盖到的全局可变状态** - L2.2-L2.5 收敛了 19 个文件域 static, 但 type.c 的单例不在其中.

**修复方向**(择一):

1. `declarator` 改为通过出参或局部载体返回 name/name_pos, 不写共享 Type;
2. 单例 const 化 + 把 name 通道独立成 parse 侧的 per-declarator 记录;
3. 单例改为 per-context 构造(context_new 时在 arena 里建一套), 代价是 `is_compatible` 的 `t1 == t2` 快速路径需保留(仍成立, 因为同 ctx 内同一).

## F2. `error_at` 的定位前提是隐式全局 `current_file` `[latent]`

tokenize.c:141-151:

```c
void error_at(char *loc, char *fmt, ...) {
  int line_no = ctx->current_file->lines[raw_off(ctx->current_file, loc)];
```

而 `raw_off`(tokenize.c:10-12)是 `file->map[p - file->contents]`. 这个表达式只在 `loc` 属于 `current_file->contents` 时才有意义.

`current_file` 是共享可变状态, 且被嵌套 tokenize **永久改写而不恢复**:

| 调用点 | 是否恢复 current_file |
| --- | --- |
| `tokenize_string_literal` (tokenize.c:587-597) | 是(save/restore) |
| `paste` (preprocess.c:567) | 否 |
| `new_str_token` (preprocess.c:280) | 否 |
| `new_num_token` (preprocess.c:300) | 否 |
| `define_macro` (preprocess.c:1093) | 否 |
| `include_file` → `tokenize_file` (preprocess.c:907) | 否 |

这个不对称本身就是信号: 作者在一处意识到需要恢复, 其余五处没有.

一旦 `error_at` 的 `loc` 不属于 `current_file`, `p - file->contents` 是两个独立 malloc 块的字节差(符号与量级均任意), `map[]` 是 `len + 2` 长的数组 → **越界读**, 行号随机, 极端情况崩溃.

**当前不可达**: 库内的错误路径几乎全部走 `error_tok`(用 `tok->file`, 正确); `error_at` 的调用点集中在 tokenize 自己的扫描循环内(`current_file` 此刻正确)与 `unicode.c:57,62`(经 `decode_utf8`, 也只在扫描期被调). 所以这是埋雷而非现行 bug. 但库化后消费者无法得知这个前提, 且任何新增的跨文件 `error_at` 都会静默错位.

**修复方向**: 让 `error_at` 接受所属 `File *`(或直接废弃, 统一走 `error_tok`); 或在上表五处补 save/restore.

## F3. codegen 的状态是进程生命周期, 不是 context 生命周期 `[实锤]`

codegen.c:43-49 与 86-89:

```c
static FILE *output_file;
static int depth;
static Obj *current_fn;
static CgNode *cg_nodes[CG_BUCKETS];
static CgType *cg_types[CG_BUCKETS];
static CgObj  *cg_objs[CG_BUCKETS];
```

注释自认(codegen.c:54-56): "Entries are created lazily and **live as long as the process**, like every other allocation of this consumer."

驱动器之所以没炸, 是因为 `run_cc1` → `run_subprocess`(main.c:458-474, 435-456)对每个 `.c` 文件 fork+exec, 用**进程隔离**实现了"一个进程一个 TU". 换句话说, codegen 的可重入性不是被解决了, 而是被绕过了.

若同进程内二次 codegen(库消费者复用 context, 或未来把 codegen 暴露出去):

- `chibicc_context_reset` 释放 arena 后, 新 TU 的分配从同样大小的 64KB 块开始, glibc 的 tcache/fastbin 高概率复用刚释放的地址 → **Node 地址逐一相同** → `cg(node)`(codegen.c:99-110)按指针 hash 命中**上一个 TU 的残留条目**;
- 命中 `is_lowered == true` 的条目会让新 TU 的忠实节点**跳过降级**(如指针算术不缩放), 命中 `brk_label` / `ret_buffer` 会串标签 - 都是**静默错误汇编**, 无诊断;
- 表永不清理 → 跨 TU 内存累积;
- 表无锁 → 与"不同 context 可在不同线程"的模型直接冲突.

`codegen` 不在 D7 公共面(公共头只导出 9 个函数), 所以库消费者当前不可达. 但 codegen.o 是 `libchibicc.a` / `.so` 的一部分, 这三个表是"状态收进 context"这条线上的**结构性例外**.

**修复方向**: 把三张表与 `output_file` / `depth` / `current_fn` 挂到 codegen 自己的 per-run 对象上(由调用者持有), 或在 `codegen()` 入口清表.

## F4. 验证闸门有结构性盲区: bump-arena 让 ASan 看不见块内越界 `[实锤]`

lib.c:51-78 的 `arena_alloc` 在 64KB `malloc` 块内做 bump 分配:

```c
ArenaBlock *nb = calloc(1, sizeof(ArenaBlock));
nb->data = malloc(cap);
...
void *p = b->data + start;
b->used = start + size;
```

ASan 的 redzone 只存在于 `malloc` 边界, 因此**同一个 arena 块内部的越界写对 ASan 完全不可见**. 而 L3.4c 建立的 ASan+UBSan 闸门(AGENTS.md "测试约定"末段, 以及 lib.c:88-89 与 tokenize.c:187-189 的注释都提到它抓到过问题)正是为抓越界而设的.

B1 的 2 字节越界、以及任何 `memcpy` 长度算错一类的 bug, 只要不跨过 64KB 块边界就检不出来. 这是闸门覆盖面与缺陷类别之间的错配, 不是闸门实现的问题.

**修复方向**: 为 sanitizer 构建加一个 debug arena 变体 - 每次分配单独 `malloc`(带 ASan redzone), 由 `test/library.sh` 的最后一轮启用. 代价是慢, 但那轮本来就是重编库对象跑闸门.

## F5. 输入契约的两个洞: NUL 字节与资源上限 `[实锤]`

**NUL**: `read_file`(tokenize.c:777-819)按二进制读入并保留 NUL, 但 `make_scan_buffer`(tokenize.c:874-970)用 `int n = strlen(text);`(tokenize.c:885)确定长度 → **含 NUL 的源文件在第一个 NUL 处被静默截断**, 无诊断, token 流提前 EOF. 同样, `chibicc_arena_strdup`(lib.c:95-102)使 `chibicc_tokenize_buffer(ctx, name, contents)`(token.h:98)**在 API 层面就无法表达含 NUL 的输入** - 没有长度参数.

对编译器这是病态输入; 对目标消费者(formatter, linter, 静态分析)处理任意用户文件是现实场景, 且失败模式是"静默少分析了半个文件".

**资源上限**: 全项目无任何递归深度或规模限制:

- `subst` → `preprocess2`(preprocess.c:693)对嵌套宏实参递归;
- `skip_cond_incl2`(preprocess.c:221-234)对嵌套 `#if` 递归;
- parse 的 `expr` 右递归(parse.c:1158-1166, `1,1,1,...` 十万个逗号即栈溢出)、`declarator` 嵌套(parse.c:589-596, `((((x))))`)、`postfix` / `primary` / `init_record` 递归;
- `cst_parse_group`(cst.c:77-104)与 cst 的表达式层递归;
- `resolve_type` / `resolve_type_exprs` / `has_flonum` / `write_gvar_data` / `mark_live` 递归.

include 侧: 无深度上限, 无文件数上限; `pragma_once`(preprocess.c:1066)与 include guard(:903-913)都按 **path 字符串**去重, 因此 `#include "a.h"` 与 `#include "./a.h"` 视为不同文件 → 无 guard 的循环 include 会**死循环**(每轮重新 `read_file`, 不是栈溢出而是持续 I/O).

**修复方向**: `tokenize_buffer` 增加长度参数(或新增 `_len` 变体); NUL 至少给一条诊断; 加一个 context 级的深度/规模上限配置, 超限走诊断而非崩溃. 这一项建议在 L5 开工前定稿 - formatter 是第一个真正吃任意用户文件的消费者.

## F6. 预定义宏集硬编码, 不可配置也不可枚举; 且与类型系统自相矛盾 `[实锤]`

`init_macros`(preprocess.c:1159-1212)写死 38 个 Linux/x86-64 宏(`__linux__`, `linux`, `unix`, `__ELF__`, `__x86_64`, `__gnu_linux__`, `__SIZEOF_*`, `__STDC_VERSION__` ...)加 5 个 builtin handler. 公共面(context.h)只提供 `add_include_path` / `define` / `undef`:

- **没有 target 配置**: 消费者无法说"我不是 Linux/x86-64";
- **没有枚举 API**: 想要干净环境只能逐个 `undef`, 而清单只存在于 `init_macros` 的源码里;
- **跨 TU 一致性**: `reset` 会重放 `macro_ops`(lib.c:246-247), 所以用户的 `-D`/`-U` 保活, 但预定义集不可见也不可关.

其中一处是硬矛盾: preprocess.c:1168 定义 `__SIZEOF_LONG_DOUBLE__` 为 `"8"`, 而 type.c:18 的 `ty_ldouble` 是 `{TY_LDOUBLE, 16, 16}`, codegen 也按 80 位 x87 发射(codegen.c:897-907). 于是 `sizeof(long double) == 16` 而 `__SIZEOF_LONG_DOUBLE__ == 8` - 消费者两边读会得到不一致答案, 第三方代码里 `#if __SIZEOF_LONG_DOUBLE__ == 8` 的分支会走错.

**修复方向**: 把预定义集提成可配置的表(至少可整体关闭 + 可枚举), 并让 `__SIZEOF_*` 由 `Type` 单例派生而不是手写常量.

## F7. 公共面卫生与 "context 不透明" 的立场不一致 `[实锤]`

- `ChibiccFile`(token.h:30-52)全字段公开可写, 而 `contents` / `raw` / `map` / `lines` 之间有强不变量(`map` 是 scan 偏移到 raw 偏移的映射, `lines` 是 raw 偏移到行号, 两者长度均为 `len + 2`), 这些不变量**只能用注释描述**(token.h:34-43). 外部写坏任一字段, `raw_off` / `add_line_numbers` / `error_at` 立即越界.
- `ChibiccToken`(token.h:56-95)与 `ChibiccNode`(ast.h:380-534)同样全字段可写. 对 IR 类型这或许是有意立场, 但与 `ChibiccContext` 的不透明化(context.h:26)标准不一致.
- `ChibiccNode.init_resolved`(ast.h:497-501)是公共 struct 里的 sema 私有 `void *`, 注释写 "Owned by sema.c" - 一个不透明载荷挂在公开可写的 struct 上, 消费者既读不懂也可能改坏.
- `ChibiccDiagnostic`(diagnostic.h:11-17)的 `filename` / `message` 是 arena 指针(tokenize.c:58-66 `vformat`), **回调返回后随时失效**(`reset` 释放). diagnostic.h 没有写这条生命周期约束, 回调里保存指针的消费者会在 reset 后拿到悬垂指针.
- `ChibiccCstNode`(cst.h:73-79)暴露 `child_cap`, 而 `cst_add_child`(cst.c:48-56)扩容后不清零 → `[child_count, child_cap)` 区间是未定义内容(arena 复用残渣). 公共契约里不该有未定义区.
- 诊断 severity 只有 0/1(diagnostic.h:15), 没有 note / 类别 / 诊断 ID, linter 集成需要更多结构.

## F8. OOM 与整数溢出无防护 `[实锤]`

- `arena_alloc`(lib.c:66-67): `calloc` 与 `malloc` 返回值均不检查 → OOM 时 NULL 解引用.
- `chibicc_arena_calloc`(lib.c:84-93): `size_t n = count * size;` 是裸乘法, **溢出后分配小内存并 `memset` 大范围** → 堆溢出. 这是公共 API, 参数来自调用者.
- `chibicc_arena_realloc`(lib.c:112-119): `old_size` 由调用者提供且无校验, 传错即读越界.
- `config_push`(lib.c:121-130): `realloc` 失败时 `*arr = n`(NULL)**丢掉旧指针** - 既泄漏又在下次 push 时 NULL 解引用.
- `chibicc_context_new`(lib.c:235): `calloc` 失败返回 NULL 后 `init_macros()` 会在 `chibicc_active_ctx = NULL` 下运行.
- `align_to`(strings.c:41-43): `(n + align - 1)` 是 int 运算, 大 n 溢出 UB.

上游风格如此(编译器可以假设内存充足), 但库面对第三方调用者时这些是攻击面. 至少 `arena_calloc` 的乘法溢出与 `config_push` 的 realloc 失败应当处理.

---

# 第三部分: B 级(C 保真与诊断缺口)

## B4. 移位与位反缺整数提升 - 第 4 层类型结论错误 `[实锤]`

sema.c:1995-1999:

```c
case ND_BITNOT:
case ND_SHL:
case ND_SHR:
  node->ty = node->lhs->ty;
  return;
```

C11 6.5.7p3(移位)与 6.5.3.3p3(一元 `~`)都要求**提升后**的类型: `char c; c << 2` 的结果类型是 `int`, 不是 `char`. 同一个 switch 里的 `ND_NEG`(sema.c:1938-1942)却做了 `get_common_type(ty_int, node->lhs->ty)` - **文件内部就不一致**.

**参考消费者碰巧正确**, 我追了三条发射路径确认:

1. 二元段的寄存器选择(codegen.c:1364-1374)只看 `node->lhs->ty` 是否 `TY_LONG` 或有 `base`, 否则一律 `%eax` → 移位实际在 32 位寄存器里做, 值正确;
2. 赋值插入的 `new_cast(shl_node /*char*/, ty_int)` 查 `cast_table[I8][I32]`(codegen.c:546 第 0 行第 2 列)= **NULL** → 不发射任何指令, 不会窄化;
3. `cmp_zero`(codegen.c:449-452)对 `is_integer(ty) && ty->size <= 4` 一律用 `cmp $0, %eax` → `if (c << 2)` 判断正确.

但**第 4 层交付的类型结论是错的**, 任何信任 `node->ty` 的消费者都会出错: C→C 打印器会印出 `(char)(c << 2)` 或按 char 做值域推理; linter 的溢出/提升检查会给出错误结论; 静态分析器的抽象解释会传播错误类型.

这一条同时暴露一个框架问题: **codegen 的正确性依赖 sema 的近似与发射侧的宽度不敏感互相抵消**. 分层设计(AGENTS.md 目标架构第 4 层"纯标注, 保留源码写法")的目的本就是消除这种耦合 - 第 4 层的结论应当可以独立于第 5 层的实现细节被信任.

**修复方向**: 三个 case 改为 `node->ty = get_common_type(ty_int, node->lhs->ty)`(与 `ND_NEG` 一致), 并按 AGENTS.md 的闸门要求跑汇编快照 diff 确认发射不变(预期不变, 因为上述三条路径都不读窄类型).

## B5. 匿名 union 的指定初始化不被识别 - 两条路径不一致 `[实锤]`

- `find_init_member`(sema.c:1066-1072, 初始化器路径)只下钻 `mem->ty->kind == TY_STRUCT && !mem->name`;
- `get_struct_member`(sema.c:1649-1657, 成员访问路径)是 `(TY_STRUCT || TY_UNION) && !mem->name`.

于是匿名 union 成员在初始化器路径落到 `if (!mem->name) continue;`(sema.c:1076-1077)被当无名成员跳过, 最终 `error_tok(name, "struct has no such member")`(sema.c:1084).

```c
struct S { union { int a; float b; }; };
struct S s = { .a = 1 };   // 被拒: "struct has no such member"
s.a = 1;                   // 正常
```

C11 6.7.2.1p13 下这是合法代码.  brace elision 形式(`struct S s = { 1 };`)因为走 `resolve_struct_cont` 按下标进 children 而不查名字, 反而正常 - 所以同一个类型, 带名字与不带名字的初始化行为不一致.

**修复方向**: `find_init_member` 的第一个条件补 `|| mem->ty->kind == TY_UNION`, 与 `get_struct_member` 对齐.

## B6. ctype 家族接收 `char` 实参 → 标准 UB `[实锤]`

- tokenize.c:650 `isspace(*p)`
- tokenize.c:657 `isdigit(*p)`
- tokenize.c:265 `ispunct(*p)`(在 `read_punct` 内)

C11 7.4p1 要求实参是 `EOF` 或**可表示为 `unsigned char`** 的值; `char` 在 x86-64/ARM64 Linux 与 macOS 上是有符号的, 高位字节(>= 0x80)传入即 UB.

这条路径是可达的: `decode_utf8`(unicode.c:37-68)只拒绝首字节形态为 `0b10xxxxxx` 的序列, 而一个孤立的续字节出现在标识符/标点位置时, `read_ident` 返回 0, 随后落到 `read_punct` 的 `ispunct(*p)` 与循环顶部的 `isspace(*p)`. glibc 与 macOS libc 的查表实现对 -128..255 宽容, 所以从未崩溃 - 但 **ASan/UBSan 不检查 ctype 实参**, 因此 AGENTS.md 宣称的"零 UB"口径在这里有洞, 且移植到严格 libc 时会出问题.

顺带: `decode_utf8` 不拒绝过长编码(`0xC0 0x80` 解出 U+0000), 也不拒绝代理区与 `> 0x10FFFF`.

**修复方向**: 三处改为 `isspace((unsigned char)*p)` 等.

## B7. `init_macros` 使用非线程安全的 `localtime` `[实锤]`

preprocess.c:1208-1211:

```c
time_t now = time(NULL);
struct tm *tm = localtime(&now);
define_macro("__DATE__", format_date(tm));
define_macro("__TIME__", format_time(tm));
```

`localtime` 返回进程级静态缓冲. `init_macros` 在 `chibicc_context_new`(lib.c:237)与 `chibicc_context_reset`(lib.c:245)里都会被调用 → **两个线程各自 new/reset 两个 context 即构成 data race**, 直接违反 context.h:3-4 的线程承诺(与 F1 是同一类问题的两个实例).

同文件的 `timestamp_macro`(preprocess.c:1134)用了 `ctime_r`, 说明作者知道 `_r` 版本的存在 - 这是遗漏而非取舍.

**修复方向**: 改 `localtime_r`.

## B8. builtin handler 的"单 token"隐式契约 `[实锤]`

preprocess.c:726-734:

```c
if (m->handler) {
  MacroExpansion *e = new_expansion(tok, NULL, m, NULL, 0, true);
  Token *body = m->handler(tok);
  for (Token *t = body; t->kind != TK_EOF; t = t->next)
    t->prov = new_prov(e, -1, tok->prov);
  body->next = tok->next;      // 改的是链头的 next
  *rest = body;
  return true;
}
```

循环走到 EOF 之后, `body->next = tok->next` 覆盖的是**链头**的 `next`. 只有当 handler 恰好产出"1 个 token + EOF"时才正确; 若某个 handler 产出 2 个以上 token, 第 2 个及之后会被静默丢弃.

现有 5 个 handler(`file_macro`, `line_macro`, `counter_macro`, `timestamp_macro`, `base_file_macro`)都满足单 token, 所以当前正确. 但契约既无断言也无注释, 而 `__has_include` / `__has_attribute` 一类将来的内建宏天然是多 token 的.

**修复方向**: 改为走到链尾再拼接(`for (t = body; t->next->kind != TK_EOF; t = t->next); t->next = tok->next;`), 或加断言.

## B9. struct/union tag 同作用域重定义被静默接受 `[实锤]`

parse.c:1666-1676:

```c
if (tag) {
  Type *ty2 = find_current_tag(tag);
  if (ty2) {
    *ty2 = *ty;      // 整体覆盖
    ret = ty2;
  } else {
    push_tag_scope(tag, ty);
  }
}
```

```c
struct S { int a; };
struct S { float b; };   // C11 6.7.2.3p3 约束违反, 这里静默接受并覆盖
```

`*ty2 = *ty` 保留对象 identity(已持有 `ty2` 指针的节点会看到新成员表), 但**没有任何诊断**. 对 linter / 静态分析消费者是漏检项. 注意 `enum_specifier`(parse.c:662-670)对未知 tag 是报错的, 两者对 tag 的严格程度也不一致.

## B10. `__attribute__` 只在 struct/union 声明处被识别 `[实锤]`

`attribute_list`(parse.c:1596-1629)只由 `struct_union_decl`(parse.c:1634, 1662)调用, 且只支持 `packed` 与 `aligned`, 其余一律 `error_tok(tok, "unknown attribute")`.

`__attribute__` 在关键字表里(tokenize.c:281)会被转成 TK_KEYWORD, 但不在 `is_typename` 的表里(parse.c:893-899), 所以:

```c
int x __attribute__((unused));            // 报 "expected ','"
__attribute__((noreturn)) void f(void);   // 报 "expected an expression" 一类
```

真实 C 代码里变量级(`unused`, `aligned`, `cleanup`)与函数级(`noreturn`, `format`, `visibility`)attribute 很常见. 这是 C 方言覆盖面的缺口, 会直接影响 thirdparty 编译成功率与 linter 的可用性.

---

# 第四部分: 次要项

| 编号 | 位置 | 问题 | 确定性 |
| --- | --- | --- | --- |
| C1 | preprocess.c:679-687 | `__VA_OPT__` 分支把实参 token 直接 splice 进输出链并覆写其 `prov`, 而这些 token 同时被 `MacroExpansion.args` 引用(经 `chibicc_macro_expansion_arg` 暴露)→ provenance 的"记录不可变"(preprocess.c:140-141 注释)有例外 | 中置信 |
| C2 | sema.c:450-457 | enum 常量值用 `int` 存, `enum E { X = 0x100000000 }` 静默截断 | 实锤 |
| C3 | tokenize.c:468 | `strtoul` 溢出饱和到 `ULONG_MAX` 且不查 `errno`, 无诊断 | 实锤 |
| C4 | sema.c:1204-1206 | 数组初始化器超额元素静默丢弃(注释自认), gcc 会警告 excess elements | 实锤 |
| C5 | sema.c:2158-2164 | GNU statement expression 返回 void 被拒("not supported"), 而 gcc 接受 | 实锤 |
| C6 | sema.c:397-421 | `scan_globals` 是 O(n^2)(globals 双重循环 + `strcmp`), 大 TU 下退化 | 实锤 |
| C7 | codegen.c:2586-2594 | `emit_data` 逐字节发 `.byte`, 大数组初始化的汇编体积膨胀约 16 倍 | 实锤 |
| C8 | hashmap.c:76-106 | 大量 delete 后 tombstone 不触发 rehash(`used` 不减), 探测退化为 O(capacity) | 实锤 |
| C9 | strings.c:11,29 / cst.c:52 / tokenize.c:995 | `arena_realloc` 旧块不回收 → 数组几何增长的峰值内存约 2x 终态; 且 `arena_realloc` 不清零, 依赖调用方手动补(strings.c 做了, cst.c 未做但只用 `child_count` 以内) | 实锤 |
| C10 | main.c:892 | `(opt_c \|\| opt_S \| opt_E)` - 第二个是位或笔误, 在 bool 上碰巧等价 | 实锤 |
| C11 | main.c:444-456 | `wait` 的 `status` 未按 `WIFEXITED`/`WEXITSTATUS` 解码; `while (wait(&status) > 0)` 会回收任意子进程(若库被嵌入到有自己的子进程宿主中会互抢) | 实锤 |
| C12 | tokenize.c:777,806 / lib.c:279 | `read_file` 的 `*len` 是 `int` → >2GB 文件截断; `"-"`(stdin)同进程第二次 tokenize 得到空流(`feof` 粘滞); `tokenize_buffer` 无长度参数(见 F5) | 实锤 |

补充: `sema_init`(sema.c:170-181)与 `sema()`(sema.c:3017 的 `globals = NULL`)分两处清状态, `label_gotos` 只在 `clear_state`(lib.c:225)清 → 同一 context 不 reset 而重复调 `chibicc_sema` 会累积旧的 `LabelGotoList` 条目(键是旧 arena 的 `Obj *`, 不 reset 时仍有效, 故无害但会增长). 状态初始化的职责分散本身是易错点.

---

# 第五部分: 建议处理顺序

**第一批(小改动, 高收益)**

1. **B1** - `getStringKind` 的 u8 判断. 一个死分支同时是内存安全 bug 和 C11 保真 bug. 顺带补 test 语料的四组前缀组合.
2. **B2** - `ND_EXCH` 锚点改 `node->lhs->tok`. 一行.
3. **B3** - 位域宽度校验. 消除编译期 UB 移位.

**第二批(与公开承诺直接冲突)**

4. **F1 + B7** - 类型单例的可变字段与 `localtime`. context.h 已经对外承诺了线程模型, 这两条是"文档说了但实现不成立", 属于必须收敛的契约债. F1 的修法需要拍板(出参 vs per-context 单例), 建议进 PLAN.

**第三批(L5 开工前必须定稿)**

5. **F5** - NUL 契约与资源上限. formatter 是第一个真正吃任意用户文件的消费者, 这两个洞会直接变成用户可见的静默错误分析结果.
6. **F4** - sanitizer 构建加 debug arena, 否则 B1 这一类块内越界永远检不出来.

**第四批(埋雷显式化, 成本低于将来查)**

7. **F2** - `error_at` 加断言或改签名; **F3** - codegen 三张表挂到 per-run 对象或入口清表.

**第五批(C 保真面, 按 L5 消费者的实际需要排)**

8. **B4**(移位提升), **B5**(匿名 union), **B9**(tag 重定义), **B10**(attribute 覆盖面), **B6**(ctype), **B8**(handler 契约), **F6**(预定义宏集), **F7**(公共面卫生), **F8**(OOM 防护).

---

# 附: 审计中确认成立的既有契约

为避免只报问题, 以下几条红线经逐项核对**确实成立**:

- **库内无新增 `exit()` 点**: 全项目 `exit(` 只出现在 tokenize.c:76(`raise_error` 在无 jmp frame 时的 fallback, 即驱动器 pre-cc1 路径)与 main.c:46,359,448,455,881(驱动器自身). 符合 AGENTS.md 的硬性规则.
- **诊断边界完整**: lib.c 的 `ENTER_RET`/`ENTER_VOID`/`LEAVE`(19-47)对每个公共入口都建帧并恢复 `jmp_top` 与 `chibicc_active_ctx`; `setjmp` 后只读不改局部变量, 无 longjmp 相关的 UB.
- **`arena_realloc` 的调用点全部自洽**: 只有 4 处(strings.c:11,29 / tokenize.c:995 / cst.c:52), 前三处立即覆写或手动清 NULL, 第四处只用 `child_count` 以内的槽.
- **`equal()` 的长度前置检查**(tokenize.c:186-190)确实消除了 `memcmp` 越界读, 与注释所述一致.
- **`add_type` 是纯标注遍**: 逐 case 核对(sema.c:1855-2267)未见为发射服务的形状改写, 与 A9.2 的审计结论一致; `lower_funcall` 重挂实参链属于"隐式转换是结论"的范畴, 且 `is_implicit` 标记可辨.
- **`preprocess` 消费输入链的契约已在公共头文档化**(preprocess.h:5-7), 消费者可预知.
- **`chibicc_context_reset` 的配置保活边界**(include paths / macro ops / 诊断回调保活, per-TU 状态清空)与 context.h:31-35 的描述一致; 唯一的例外是 `base_file` 不保活, 但 `chibicc_tokenize_file`(lib.c:267-268)会用首个路径隐式补上.
