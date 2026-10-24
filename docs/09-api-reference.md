# 09 API 参考与用法

本章是公共 API 的逐项说明. 函数原型以 `libcp/include/libcp/` 下的头文件为准. 本章只列 36 个公共函数与公共宏, 不列内部函数.

## 9.1 头文件, 链接与命名

只需要 include 一个头:

```c
#include <libcp/libcp.h>
```

它是伞头, 包含其余 9 个公共头:

```text
libcp.h       伞头, LIBCP_VERSION, libcp_version
allocator.h     可替换的分配器宏
context.h       context, 诊断回调, 配置, 预定义宏
diagnostic.h    LibcpDiagnostic, severity
token.h         LibcpToken, LibcpFile, 第 1 层入口
preprocess.h    libcp_preprocess
provenance.h    宏展开访问器, dump / verify
cst.h           第 2 层
ast.h           第 3/4 层的数据类型与 libcp_parse
sema.h          libcp_sema
```

链接:

```sh
cc -std=c11 -I/path/to/include app.c -L/path/to/lib -lcp
```

或直接给静态库路径:

```sh
cc -std=c11 -Ilibcp/include app.c libcp.a
```

命名规则:

- 函数一律 `libcp_` 前缀.
- 类型一律 `Libcp` 前缀.
- 结构体 tag 是 `struct libcp_*`.
- 枚举常量保留层前缀: `TK_` (token), `ND_` (node), `TY_` (type), `CST_` (CST), `INIT_` (initializer).
- 宏是 `LIBCP_*`.

`LIBCP_API` 在 GCC/Clang 下展开为 `__attribute__((visibility("default")))`, 用于共享库导出. 消费者不需要关心它.

`.a` 里还有内部符号(未在任何公共头声明的 `libcp_` 前缀名字, 2026-10-25 起与公共 API 同族). 它们对该归档可见, 但不属于 API: 内部函数以 `LibcpContext *ctx` 为首参这一形态也不承诺源码兼容, 换版本时就地改变; 契约只有公共头与本章的 36 个函数.

## 9.2 函数总表

| 组 | 函数 |
|---|---|
| context | `libcp_context_new`, `_free`, `_reset`, `_set_diagnostic`, `_failed` |
| 配置 | `libcp_context_add_include_path`, `_define`, `_undef`, `_set_predefined_macros`, `_predefined_count`, `_predefined_at` |
| 第 1 层 | `libcp_tokenize_file`, `libcp_tokenize_buffer`, `libcp_tokenize_buffer_len`, `libcp_reconstruct` |
| 预处理 | `libcp_preprocess` |
| 第 2 层 | `libcp_cst_parse`, `libcp_cst_seed_typedef`, `libcp_cst_reconstruct`, `libcp_cst_dump`, `libcp_cst_verify` |
| 第 3 层 | `libcp_parse`, `libcp_node_source_range` |
| 第 4 层 | `libcp_sema` |
| provenance | `libcp_token_provenance`, `libcp_macro_prov_next`, `_expansion`, `_arg_index` |
| provenance | `libcp_macro_expansion_name`, `_end`, `_macro`, `_def`, `_arg`, `_arg_count`, `_is_builtin` |
| provenance | `libcp_expansion_dump`, `libcp_expansion_verify` |
| 版本 | `libcp_version`, `LIBCP_VERSION` |

## 9.3 生命周期与配置

### 9.3.1 libcp_context_new

```c
LibcpContext *libcp_context_new(void);
```

创建一个 context. 一个 context 服务一个翻译单元.

行为:

- 初始化 arena 和全部 per-TU 状态.
- 安装 47 个预定义宏.
- 不添加任何 include 搜索路径.
- 不注册诊断回调, 默认为"按驱动器格式打印到 stderr".

返回值: 正常路径上总是非 NULL. 内存分配失败没有单独的处理路径, 不构成"返回 NULL 的错误模型".

### 9.3.2 libcp_context_free

```c
void libcp_context_free(LibcpContext *ctx);
```

销毁 context:

- 释放 arena 中的全部对象.
- 释放 include 路径和 `-D` / `-U` 配置数组.
- 清除 active context 指针.

`ctx` 为 NULL 时安全返回. 调用后所有指针失效.

### 9.3.3 libcp_context_reset

```c
void libcp_context_reset(LibcpContext *ctx);
```

重置 per-TU 状态, 保留配置:

- 释放整个 arena.
- 清空文件表, 宏表, 条件编译栈, 作用域, 语义状态.
- 清除 `failed`.
- 重新安装预定义宏.
- 按记录顺序重新应用 `-D` / `-U`.
- 保留 include 路径, 诊断回调, 预定义宏开关.

调用后:

- 所有由该 context 返回的指针失效.
- 可以开始下一个 TU.
- 上一个 TU 的错误状态被清除.

### 9.3.4 libcp_context_set_diagnostic

```c
void libcp_context_set_diagnostic(LibcpContext *ctx,
                                    LibcpDiagnosticFn fn, void *user);
```

注册诊断回调.

```c
typedef void (*LibcpDiagnosticFn)(void *user,
                                    const LibcpDiagnostic *);
```

- 注册后, 库不再向 stderr 打印.
- `user` 原样传给回调.
- `fn` 为 NULL 时恢复默认打印行为.
- 回调里的字符串只保证到下一次 reset.
- `LibcpDiagnostic` 自 L5 起带 `tok`(锚点 token, token 锚定的诊断非 NULL)与
  `prov`(该 token 的宏展开链, 源 token 为 NULL): 回调可以沿链走到最外层
  调用点. 默认打印行为忽略这两个字段, 输出逐字节不变.

### 9.3.5 libcp_context_failed

```c
bool libcp_context_failed(LibcpContext *ctx);
```

返回上一次操作是否出错.

- 错误经 longjmp 回到公共入口时置位.
- 警告不置位.
- `reset` 清除.

用途: 区分"返回 NULL 是错误"和"返回 NULL 是空翻译单元".

### 9.3.6 libcp_context_add_include_path

```c
void libcp_context_add_include_path(LibcpContext *ctx, char *path);
```

追加一个 include 搜索目录.

- 内部复制字符串, 调用者可以释放 `path`.
- 顺序就是搜索顺序.
- 没有默认系统路径. 需要 `/usr/include` 就自己加.
- 没有返回值. 内存分配失败时该路径可能不被记录.

库本身不读环境变量, 不加编译器的默认路径.

### 9.3.7 libcp_context_define

```c
void libcp_context_define(LibcpContext *ctx, char *name, char *buf);
```

定义一个宏.

- `name` 是宏名.
- `buf` 是替换文本, 不是值. `"1"` 表示 `#define name 1`.
- 立即生效.
- 记录在配置里, reset 后重新应用.
- 顺序很重要, 后定义的覆盖先定义的.

### 9.3.8 libcp_context_undef

```c
void libcp_context_undef(LibcpContext *ctx, char *name);
```

取消一个宏.

- 立即生效.
- 记录在配置里, reset 后重新应用.
- 对未定义的宏是空操作.

### 9.3.9 libcp_context_set_predefined_macros

```c
void libcp_context_set_predefined_macros(LibcpContext *ctx, bool enabled);
```

打开或关闭预定义宏集.

- 默认打开.
- 设置在**下一次 reset** 生效.
- 关闭后, 预定义宏不安装, 但 `-D` / `-U` 仍然应用.

### 9.3.10 libcp_context_predefined_count / _at

```c
int libcp_context_predefined_count(LibcpContext *ctx);
LibcpPredefinedMacro libcp_context_predefined_at(LibcpContext *ctx,
                                                     int index);
```

枚举当前 TU 安装的预定义宏.

```c
typedef struct libcp_predefined_macro {
  const char *name;
  const char *value;
} LibcpPredefinedMacro;
```

- `value` 是替换文本. 内建宏和动态宏为 NULL.
- 字符串由 context 拥有, reset 后失效.
- 越界返回 `{NULL, NULL}`.
- 关闭预定义宏后 count 为 0.

### 9.3.11 分配器宏

`allocator.h` 定义库的分配面, 选择方式是一个构建期开关:

```c
#ifdef LIBCP_DEFAULT_ALLOCATOR
/* 四个宏展开为 libc 的 malloc / calloc / realloc / free */
#elif defined(LIBCP_INTERNAL)
/* 库构建时: 四个宏必须已全部定义, 否则 #error */
#endif
```

- `LIBCP_DEFAULT_ALLOCATOR` 定义时使用 C 库分配器, 四个宏全部展开为 libc 同名函数, 不做逐个宏的回退.
- 未定义时, 嵌入方必须自己提供全部四个宏, 通常通过 `-include` shim 头. 校验只在构建 libcp 时进行(`LIBCP_INTERNAL`), 缺少任一个宏会在包含处 `#error`; 只 include 公共头的消费者不需要选择分配器.
- 默认构建在 Makefile 里定义 `LIBCP_DEFAULT_ALLOCATOR`(`ALLOCATOR_CFLAGS`); 自定义构建覆盖 `ALLOCATOR_CFLAGS`, 只给四个宏, 见 10.11.5 的 mimalloc 例子.
- 库拥有的每个块 - context 对象, 配置数组与字符串, arena 块及其头部 - 都从这四个宏取得, 并通过 `LIBCP_FREE` 归还.
- 它们是编译期配置: 在编译 libcp 时定义, 不是在消费者里定义. 链接预编译库时重新定义不会改变已经编译好的库.
- 替换实现必须满足 libc 的语义: 返回满足任意类型对齐的内存(x86-64 上 `Token` / `Node` 内嵌 `long double`, 需要 16 字节), `LIBCP_CALLOC` 清零, `LIBCP_REALLOC` 失败时返回 NULL 且保留原块, `LIBCP_FREE` 接受 NULL.
- 宏不产生符号, D7 与 `.so` 导出不变.
- 例外: 读文件时 `open_memstream` 的缓冲区由 libc 的 `malloc` 分配并用 `free` 释放, 不属于库的自有内存.
- `test/allocator.sh` 在不定义 `LIBCP_DEFAULT_ALLOCATOR` 的前提下用计数分配器重编整个库, 锁定"替换确实被调用", "缺宏会被诊断"与 `libcp_context_free` 后活块归零.

## 9.4 第 1 层 API

### 9.4.1 libcp_tokenize_file

```c
LibcpToken *libcp_tokenize_file(LibcpContext *ctx, char *path);
```

读取文件并 tokenize.

- `path` 是文件系统路径.
- `-` 表示 stdin. 内容只读一次, 同一 context 内第二次 tokenize `-` 会拿到缓存.
- 返回以 `TK_EOF` 结尾的链.
- 失败返回 NULL 并报告诊断. 失败原因: 打不开, 读失败, 文件过大, 含 NUL, 非法 token.
- 第一次调用会设置 `__BASE_FILE__` 的文件名.

### 9.4.2 libcp_tokenize_buffer

```c
LibcpToken *libcp_tokenize_buffer(LibcpContext *ctx, char *name,
                                      char *contents);
```

tokenize 一个 NUL 结尾的内存字符串.

- `name` 是显示文件名, 用于诊断.
- 输入被复制进 arena.
- 不能用包含 NUL 的二进制块; 用 `_len` 版本.
- 不做扫描缓冲改写: 不跳 BOM, 不删续行, 不改写 universal char. 见第 11.5.3 节.

### 9.4.3 libcp_tokenize_buffer_len

```c
LibcpToken *libcp_tokenize_buffer_len(LibcpContext *ctx, char *name,
                                          char *contents, int len);
```

tokenize 恰好 `len` 字节.

- 缓冲区可以包含 NUL. 出现 NUL 会报 `NUL byte in source file`, 返回 NULL.
- `len` 为负报 `invalid buffer length`.
- 用于从文件映射, 网络缓冲等已知长度的输入.
- 与 `tokenize_buffer` 一样, 不做扫描缓冲改写: BOM, 续行, universal char 都按原字节处理.

### 9.4.4 libcp_reconstruct

```c
void libcp_reconstruct(LibcpContext *ctx, LibcpToken *tok, FILE *out);
```

按序写出每个 token 的 trivia 和拼写.

- 对 `tokenize_file` / `tokenize_buffer_len` 的直接输出, 结果等于输入文件的原始字节.
- 对预处理后的流, 结果不是原文件. 宏体替代了调用点, 指令行消失.
- `out` 由调用者打开和关闭.
- 不需要先预处理.

## 9.5 预处理 API

### 9.5.1 libcp_preprocess

```c
LibcpToken *libcp_preprocess(LibcpContext *ctx, LibcpToken *tok);
```

执行预处理.

- 输入是一条 token 链.
- 输出是展开后的链, 以 `TK_EOF` 结尾.
- **输入链被原地改写**. 调用后不要再用原指针, 除非只做重建等只读操作并且确认未被破坏.
- 失败返回 NULL 并报告诊断.
- 展开产物带 provenance 链.

## 9.6 第 2 层 API

### 9.6.1 libcp_cst_parse

```c
LibcpCstNode *libcp_cst_parse(LibcpContext *ctx, LibcpToken *tok);
```

从**预处理之前**的 token 流建 lossless CST.

- 解析是全函数, 不报语法诊断, 不退出.
- 返回 `CST_TRANSLATION_UNIT` 根.
- 叶子按序覆盖输入 token, 包括 EOF.
- 输入链在调用中不被改写.

### 9.6.2 libcp_cst_seed_typedef

```c
void libcp_cst_seed_typedef(LibcpContext *ctx, char *name);
```

把一个由**被 include 的头文件**提供的 typedef 名(如 `va_list`, `pthread_t`)注入
CST 的声明 oracle. CST 建在预处理之前, 看不到 include 里的 `typedef`, 这类
名字不 seed 时相关声明回落 `CST_UNPARSED`. 配置语义: 跨 `reset` 保留,
`libcp_context_free` 释放. 库不猜 - 消费者知道自己目标的头, 由它供给.

### 9.6.3 libcp_cst_reconstruct

```c
void libcp_cst_reconstruct(LibcpContext *ctx, LibcpCstNode *root,
                             FILE *out);
```

按叶子顺序写出 trivia 和拼写.

对直接 tokenize 输出, 结果等于原文件字节.

### 9.6.4 libcp_cst_dump

```c
void libcp_cst_dump(LibcpContext *ctx, LibcpCstNode *root, FILE *out);
```

打印树形 dump, 每层缩进两格. 叶子打印种类和拼写.

### 9.6.5 libcp_cst_verify

```c
bool libcp_cst_verify(LibcpContext *ctx, LibcpCstNode *root,
                        LibcpToken *tok);
```

校验树的叶子序列与 token 流:

- 数量一致.
- 每个叶子指向同一个 token 对象.
- 顺序一致.

返回 true 表示通过. 失败返回 false, 不报告诊断.

## 9.7 第 3 层 API

### 9.7.1 libcp_parse

```c
LibcpNode *libcp_parse(LibcpContext *ctx, LibcpToken *tok);
```

从预处理后的 token 流建忠实语法 AST.

- 输入必须是 `libcp_preprocess` 的输出.
- 返回顶层声明记录链.
- 空翻译单元返回 NULL, `failed` 为 false.
- 语法错误返回 NULL, `failed` 为 true.
- 表达式不做类型计算, 名字未绑定, 常量未求值. 数值字面量例外, 带词法类型.
- 深度上限 512.

### 9.7.2 libcp_node_source_range

```c
void libcp_node_source_range(const LibcpNode *node,
                             LibcpToken **start, LibcpToken **end);
```

子树锚点 token 的极值(按 所属文件 + 偏移 比较): 把一个 AST 子树映射回源码
span 的最小接口. 注意事项:

- `next` 链是兄弟, 不被跟随; 初始化器树不被下降.
- 极值取自各节点的代表 token: 例如 `ND_FUNCDEF` 的代表 token 是函数体的
  `{`, 所以整个函数定义的 range 从它的名字 token 开始, 不从声明说明符
  开始 - 需要更早边界时要自己看 `spec_decls`.
- 宏展开产物 token 的坐标在合成缓冲里, 跨文件比较按文件指针先序, 稳定
  但无语义.

## 9.8 第 4 层 API

### 9.8.1 libcp_sema

```c
LibcpObj *libcp_sema(LibcpContext *ctx, LibcpNode *toplevel);
```

对第 3 层的树做语义分析.

- 原地标注.
- 返回全局对象链, 包含函数, 全局变量, 匿名全局, 以及 `extern` 声明.
- 空输入返回 NULL, `failed` 为 false.
- 语义错误返回 NULL, `failed` 为 true.
- `ND_IDENT` 和 `ND_STRING` 会被改写.
- 声明记录保留在语句链上.

消费者通常过滤 `is_definition` 来拿真正的定义.

## 9.9 provenance API

### 9.9.1 链访问器

```c
LibcpMacroProv *libcp_token_provenance(LibcpToken *tok);
LibcpMacroProv *libcp_macro_prov_next(LibcpMacroProv *p);
LibcpMacroExpansion *libcp_macro_prov_expansion(LibcpMacroProv *p);
int libcp_macro_prov_arg_index(LibcpMacroProv *p);
```

- `libcp_token_provenance`: 返回 token 的链头, 源 token 返回 NULL.
- `libcp_macro_prov_next`: 下一层, 即更外层的调用上下文.
- `libcp_macro_prov_expansion`: 该层对应的展开记录.
- `libcp_macro_prov_arg_index`: `-1` 表示宏体, `>= 0` 表示实参序号, 0 起.

这些访问器不分配, 不需要错误边界. 传入 NULL 会崩溃, 调用者保证非 NULL.

### 9.9.2 展开记录访问器

```c
LibcpToken *libcp_macro_expansion_name(LibcpMacroExpansion *e);
LibcpToken *libcp_macro_expansion_end(LibcpMacroExpansion *e);
char *libcp_macro_expansion_macro(LibcpMacroExpansion *e);
LibcpToken *libcp_macro_expansion_def(LibcpMacroExpansion *e);
LibcpToken *libcp_macro_expansion_arg(LibcpMacroExpansion *e, int i);
int libcp_macro_expansion_arg_count(LibcpMacroExpansion *e);
bool libcp_macro_expansion_is_builtin(LibcpMacroExpansion *e);
```

- `name`: 调用点的宏名 token.
- `end`: 函数宏的右括号; 对象宏返回宏名 token.
- `macro`: 宏的拼写, NUL 结尾.
- `def`: `#define` 的宏名 token; 内建宏和 `-D` 定义返回 NULL.
- `arg`: 第 i 个实参的 token 链, 以 `TK_EOF` 结尾; 越界返回 NULL.
- `arg_count`: 实参个数.
- `is_builtin`: 是否由 handler 产生.

### 9.9.3 通道

```c
void libcp_expansion_dump(LibcpContext *ctx, LibcpToken *tok, FILE *out);
bool libcp_expansion_verify(LibcpContext *ctx, LibcpToken *tok);
```

- `dump`: 每个非 EOF token 一行.
- `verify`: 校验链的深度, 唯一性, 记录完整性, 名字一致, 实参序号. 失败时向 stderr 打印一条消息并返回 false.

## 9.10 版本 API

```c
#define LIBCP_VERSION "0.1.0"
const char *libcp_version(void);
```

`LIBCP_VERSION` 与 Makefile 的 `VERSION` 同步, 由 `test/install.sh` 校验.

只承诺源码兼容. 公共结构体字段变化就是版本变化.

## 9.11 返回值与错误约定

| 入口 | 成功 | 失败 |
|---|---|---|
| `libcp_tokenize_file` | token 链 | NULL + `failed` |
| `libcp_tokenize_buffer` | token 链 | NULL + `failed` |
| `libcp_tokenize_buffer_len` | token 链 | NULL + `failed` |
| `libcp_preprocess` | token 链 | NULL + `failed` |
| `libcp_cst_parse` | 树 | NULL + `failed` |
| `libcp_cst_verify` | true | false (或 false + `failed`) |
| `libcp_parse` | 树 | NULL + `failed`; 空 TU 为 NULL 且未失败 |
| `libcp_sema` | `Obj` 链 | NULL + `failed`; 空 TU 为 NULL 且未失败 |
| `libcp_expansion_verify` | true | false |
| `libcp_reconstruct` / `_cst_reconstruct` / `_cst_dump` / `_expansion_dump` | 返回 | 提前返回 |
| `libcp_context_*` 配置 | 返回 | 无返回; 分配失败时配置可能未生效 |

错误处理模板:

```c
LibcpNode *ast = libcp_parse(ctx, tok);
if (libcp_context_failed(ctx)) {
  /* 错误已经通过回调报告 */
  libcp_context_reset(ctx);
  return -1;
}
if (!ast) {
  /* 空翻译单元 */
}
```

## 9.12 所有权与失效规则

- 库返回的所有指针由 context 拥有, 不要 `free`.
- 输入参数:
  - `tokenize_buffer*`: 复制输入字节, 调用者可以立即释放.
  - `add_include_path`: 复制路径.
  - `define` / `undef`: 复制名字和替换文本.
  - `FILE *out`: 调用者拥有, 库不关闭.
- 诊断回调里的 `filename` / `message`: context 拥有, 到下一次 reset.
- `reset` 后一切失效.
- `free` 后一切失效.

## 9.13 完整示例

下面的程序已经用 `cc -std=c11 -Ilibcp/include example.c libcp.a` 编译并运行. 它依次使用第 1 层, 第 2 层, 编译路径和 provenance.

```c
#include <libcp/libcp.h>
#include <stdio.h>
#include <string.h>

static void on_diag(void *user, const LibcpDiagnostic *d) {
  (void)user;
  fprintf(stderr, "%s:%d:%d: %s\n",
          d->filename ? d->filename : "<input>",
          d->line, d->column, d->message);
}

static int count(LibcpObj *o) {
  int n = 0;
  for (; o; o = o->next)
    n++;
  return n;
}

static int pipeline(LibcpContext *ctx, char *src) {
  LibcpToken *tok = libcp_tokenize_buffer(ctx, "<example>", src);
  if (!tok)
    return 1;

  tok = libcp_preprocess(ctx, tok);
  if (!tok)
    return 1;

  LibcpNode *ast = libcp_parse(ctx, tok);
  if (!ast && libcp_context_failed(ctx))
    return 1;

  LibcpObj *prog = libcp_sema(ctx, ast);
  if (!prog && libcp_context_failed(ctx))
    return 1;

  for (LibcpObj *o = prog; o; o = o->next) {
    if (!o->is_definition)
      continue;
    if (o->is_function)
      printf("function %s: %d parameter(s)\n", o->name, count(o->params));
    else
      printf("global %s: size=%d, align=%d\n",
             o->name, o->ty->size, o->ty->align);
  }
  return 0;
}

int main(void) {
  LibcpContext *ctx = libcp_context_new();
  libcp_context_set_diagnostic(ctx, on_diag, NULL);

  char *src =
      "struct point { int x; int y; };\n"
      "#define N 3\n"
      "int g = N;\n"
      "int add(int a, int b) { return a + b; }\n";

  /* Layer 1: reconstruct the exact bytes. */
  LibcpToken *raw = libcp_tokenize_buffer(ctx, "<example>", src);
  FILE *f = tmpfile();
  libcp_reconstruct(ctx, raw, f);
  fflush(f);
  rewind(f);
  char buf[256];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  printf("reconstruct equal: %d\n", strcmp(buf, src) == 0);

  /* Layer 2: build the CST from a second tokenize. */
  LibcpToken *raw2 = libcp_tokenize_buffer(ctx, "<example>", src);
  LibcpCstNode *cst = libcp_cst_parse(ctx, raw2);
  printf("cst root=%d children=%d verify=%d\n",
         (int)cst->kind, cst->child_count,
         (int)libcp_cst_verify(ctx, cst, raw2));

  /* Compile path. */
  if (pipeline(ctx, src))
    return 1;

  /* Provenance of the N expansion. */
  LibcpToken *t = libcp_tokenize_buffer(ctx, "<example>", src);
  t = libcp_preprocess(ctx, t);
  for (; t && t->kind != TK_EOF; t = t->next) {
    LibcpMacroProv *p = libcp_token_provenance(t);
    if (!p)
      continue;
    LibcpMacroExpansion *e = libcp_macro_prov_expansion(p);
    printf("expanded %.*s via %s (arg %d)\n",
           t->len, t->loc,
           libcp_macro_expansion_macro(e),
           libcp_macro_prov_arg_index(p));
  }

  libcp_context_free(ctx);
  return 0;
}
```

实际输出:

```text
reconstruct equal: 1
cst root=0 children=5 verify=1
global .L..1: size=4, align=1
global .L..0: size=4, align=1
function add: 2 parameter(s)
global g: size=4, align=4
expanded 3 via N (arg -1)
```

输出解释:

- `reconstruct equal: 1` 表示第 1 层逐字节还原成功.
- `cst root=0` 是 `CST_TRANSLATION_UNIT`. `children=5` 是 struct 声明, `#define`, 全局变量, 函数定义和 EOF.
- `.L..0` / `.L..1` 是 `add` 函数的 `__func__` / `__FUNCTION__` 匿名字符串全局, 大小 4 = `add` 加 NUL.
- `g` 的大小 4, 对齐 4.
- `expanded 3 via N (arg -1)` 表示数字 `3` 是 `N` 宏体的产物, 不是实参.

## 9.14 常见配方

### 9.14.1 保留注释地读源码

```c
LibcpToken *raw = libcp_tokenize_file(ctx, path);
LibcpCstNode *cst = libcp_cst_parse(ctx, raw);
/* 遍历 cst, 注释在叶子 token 的 trivia 里 */
```

不要先预处理. 需要同时编译时, 对同一个文件 tokenize 两次.

### 9.14.2 只做词法

```c
LibcpToken *tok = libcp_tokenize_file(ctx, path);
for (LibcpToken *t = tok; t && t->kind != TK_EOF; t = t->next) {
  /* 读 t->raw_loc / t->raw_len / t->trivia */
}
```

原始流里关键字是 `TK_IDENT`. 要分类就自己比较拼写, 或调用预处理.

### 9.14.3 提取函数签名

用第 4 层:

```c
LibcpObj *prog = libcp_sema(ctx, ast);
for (LibcpObj *o = prog; o; o = o->next) {
  if (!o->is_function || !o->is_definition)
    continue;
  printf("%s(", o->name);
  for (LibcpObj *p = o->params; p; p = p->next)
    printf("%s%s", p == o->params ? "" : ", ", p->name);
  printf(")\n");
}
```

用第 3 层也可以, 但参数名在类型记录的 per-parameter 副本上, 且没有 `Obj` 方便.

### 9.14.4 追踪宏展开

```c
LibcpToken *t = libcp_preprocess(ctx, tok);
for (; t && t->kind != TK_EOF; t = t->next) {
  for (LibcpMacroProv *p = libcp_token_provenance(t); p;
       p = libcp_macro_prov_next(p)) {
    LibcpMacroExpansion *e = libcp_macro_prov_expansion(p);
    printf("%.*s <- %s\n", t->len, t->loc,
           libcp_macro_expansion_macro(e));
  }
}
```

### 9.14.5 批量处理多个 TU

```c
LibcpContext *ctx = libcp_context_new();
/* 配置一次 */
for (int i = 0; i < n; i++) {
  process(ctx, files[i]);
  libcp_context_reset(ctx);
}
libcp_context_free(ctx);
```

配置在 reset 后保留. 每个文件之间状态隔离.

### 9.14.6 出错后复用

```c
if (libcp_context_failed(ctx)) {
  libcp_context_reset(ctx);
  /* 重新开始 */
}
```

不要在失败后用部分树. 库不保证一致.

### 9.14.7 在构建系统里使用

```sh
# pkg-config
cc app.c $(pkg-config --cflags --libs libcp)

# 手工
cc -std=c11 -I/usr/local/include app.c -L/usr/local/lib -lcp
```

安装布局见第 10 章.

### 9.14.8 从 C++ 调用

公共头是 C11 头, 没有 `extern "C"` 包装. C++ 消费者需要自己包:

```cpp
extern "C" {
#include <libcp/libcp.h>
}
```

结构体是普通 C 结构, 可以直接读字段.

## 9.15 API 的边界

- 没有深拷贝树的 API. 第 4 层原地改写.
- 没有源码范围 API. 只有代表 token.
- 没有 formatter API. 第 2 层是数据, 排版要自己写.
- 没有诊断展开点字段. provenance 访问器可以自己映射.
- 没有虚拟文件系统. include 只走磁盘.
- 没有增量 API. 每次从头构建.
- 没有 ABI 承诺.
