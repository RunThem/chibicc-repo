# 09 API 参考与用法

本章是公共 API 的逐项说明. 函数原型以 `libcp/include/libcp/` 下的头文件为准. 本章只列 63 个公共函数与公共宏, 不列内部函数.

## 9.1 头文件, 链接与命名

只需要 include 一个头:

```c
#include <libcp/libcp.h>
```

它是伞头, 包含其余 10 个公共头:

```text
libcp.h       伞头, LIBCP_VERSION, libcp_version
allocator.h     可替换的分配器宏
context.h       context, 诊断回调, 配置, base file, 预定义宏
diagnostic.h    LibcpDiagnostic, severity
token.h         LibcpToken, LibcpFile, 第 1 层入口, input file 枚举与 synthetic 判定
preprocess.h    libcp_preprocess
provenance.h    宏展开访问器, dump / verify
cst.h           第 2 层
ast.h           第 3/4 层的数据类型与 libcp_parse
backend.h       后端支持面: 节点构造, LibcpBuiltinType, 类型标注, 标签引用, 唯一名与诊断
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
- 公开数据结构的 tag 是 `struct libcp_*`; 不透明的 `LibcpContext` 例外, tag 是 `struct LibcpContext`.
- 枚举常量保留层前缀: `TK_` (token), `ND_` (node), `TY_` (type), `CST_` (CST), `INIT_` (initializer), `BT_` (backend 的内建类型).
- 宏是 `LIBCP_*`.

`LIBCP_API` 在 GCC/Clang 下展开为 `__attribute__((visibility("default")))`, 用于共享库导出. 消费者不需要关心它.

`.a` 里还有内部符号(未在任何公共头声明的 `libcp_` 前缀名字, 2026-10-25 起与公共 API 同族). 它们对该归档可见, 但不属于 API: 内部函数以 `LibcpContext *ctx` 为首参这一形态也不承诺源码兼容, 换版本时就地改变; 契约只有公共头与本章的 63 个函数.

## 9.2 函数总表

| 组 | 函数 |
|---|---|
| context | `libcp_context_new`, `_free`, `_reset`, `_set_diagnostic`, `_failed` |
| 配置 | `libcp_context_add_include_path`, `libcp_context_set_base_file`, `_define`, `_undef`, `_set_predefined_macros`, `_predefined_count`, `_predefined_at` |
| include 搜索 | `libcp_context_add_include_path_with_origin`, `libcp_context_resolve_include`, `libcp_context_include_is_system` |
| 第 1 层 | `libcp_tokenize_file`, `libcp_tokenize_buffer`, `libcp_tokenize_buffer_len`, `libcp_reconstruct` |
| 源坐标 | `libcp_context_input_file_count`, `libcp_context_input_file_at`, `libcp_token_is_synthetic` |
| 预处理 | `libcp_preprocess` |
| 第 2 层 | `libcp_cst_parse`, `libcp_cst_seed_typedef`, `libcp_cst_reconstruct`, `libcp_cst_dump`, `libcp_cst_verify` |
| 第 3 层 | `libcp_parse`, `libcp_node_family`, `libcp_node_source_range` |
| 第 4 层 | `libcp_sema` |
| 后端支持面 | `libcp_node_new`, `_binary`, `_unary`, `_num`, `_long`, `_arith`, `_var`, `_vla_ptr`, `_alloca`, `_cast` |
| 后端支持面 | `libcp_type_builtin`, `libcp_node_annotate_type`, `libcp_type_pointer_to` |
| 后端支持面 | `libcp_label_ref_count`, `libcp_label_ref_at` |
| 后端支持面 | `libcp_context_take_unique_name` |
| 后端支持面 | `libcp_diagnostic_report` |
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

- 初始化全部 per-TU 状态和分配登记链.
- 安装 47 个预定义宏.
- 不添加任何 include 搜索路径.
- 不注册诊断回调, 默认为"按驱动器格式打印到 stderr".

返回值: 正常路径上总是非 NULL. 内存分配失败没有单独的处理路径, 不构成"返回 NULL 的错误模型".

### 9.3.2 libcp_context_free

```c
void libcp_context_free(LibcpContext *ctx);
```

销毁 context:

- 归还登记链上的全部 per-TU 分配.
- 释放 include 路径, `-D` / `-U` 配置数组与 CST typedef 种子.
- 释放 context 本身; 库没有全局或线程局部的 active context 指针需要清除.

`ctx` 为 NULL 时安全返回. 调用后所有指针失效.

### 9.3.3 libcp_context_reset

```c
void libcp_context_reset(LibcpContext *ctx);
```

重置 per-TU 状态, 保留配置:

- 归还全部 per-TU 分配.
- 清空文件表, 宏表, 条件编译栈, 作用域, 语义状态.
- 清空 stdin 缓存 (`stdin_text` / `stdin_len`).
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

- 注册后, 库不再向 stderr 打印. 例外是 `libcp_expansion_verify` 的违规报告: 它直接写 stderr, 不经过回调.
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
- 返回式入口 `libcp_diagnostic_report` 报错误时也置位 (见 9.15.5).
- 警告不置位.
- `reset` 清除.

用途: 区分"返回 NULL 是错误"和"返回 NULL 是空翻译单元".

### 9.3.6 libcp_context_add_include_path

```c
void libcp_context_add_include_path(LibcpContext *ctx, char *path);
```

追加一个 include 搜索目录, 等同于来源为 `LIBCP_INCLUDE_USER` 的版本.

- 内部复制字符串, 调用者可以释放 `path`.
- 没有默认系统路径. 需要 `/usr/include` 就自己加.
- 没有返回值. 内存分配失败时该路径可能不被记录.

路径表跨 reset 保留. 库本身不读环境变量, 不加编译器的默认路径.

### 9.3.7 libcp_context_add_include_path_with_origin

```c
void libcp_context_add_include_path_with_origin(LibcpContext *ctx,
                                                char *path,
                                                LibcpIncludeOrigin origin);
```

按来源追加一个 include 搜索目录. `LibcpIncludeOrigin` 有三个值:

- `LIBCP_INCLUDE_USER`: 用户目录 (`-I`).
- `LIBCP_INCLUDE_SYSTEM`: 内建或默认系统目录.
- `LIBCP_INCLUDE_AFTER`: `-idirafter` 风格目录.

搜索顺序固定为 `USER` -> `AFTER` -> `SYSTEM`, 同一来源内保持注册顺序. 命令行上 `-I` 与 `-idirafter` 的先后不影响结果. 这与 GCC 不同: GCC 把 `-idirafter` 排在系统目录之后, 本库排在系统目录之前.

字符串被复制, 路径表跨 reset 保留, 没有返回值. 内存分配失败时该路径可能不被记录.

### 9.3.8 libcp_context_resolve_include

```c
char *libcp_context_resolve_include(LibcpContext *ctx, char *filename);
```

解析一个 include 文件名, 供 `-include` 或树外驱动器复用.

- 先按原样查文件 (cwd 相对或绝对); 存在就返回该路径.
- 否则按 `USER` -> `AFTER` -> `SYSTEM` 遍历路径表.
- 绝对路径不会与路径表拼接; 表内也找不到时返回 NULL.
- 返回 per-TU 分配的路径; 有效期到下一次 `reset` 或 `free`.
- 命中路径表时与 `#include` 一样更新内部搜索状态.

### 9.3.9 libcp_context_include_is_system

```c
bool libcp_context_include_is_system(LibcpContext *ctx, char *path);
```

判断一个文件路径是否落在系统目录下.

- 对路径表做字符串前缀匹配, 不做 realpath 或 inode 归一.
- `SYSTEM` 与 `AFTER` 来源算系统目录, `USER` 与表外路径不算.
- 供 `-MMD` 一类"省略系统头"的过滤使用. `libcp` 不决定过滤策略, 只回答分类.

### 9.3.10 libcp_context_set_base_file

```c
void libcp_context_set_base_file(LibcpContext *ctx, char *path);
```

设置 `__BASE_FILE__` 的路径.

- 内部复制字符串, 调用者可以释放 `path`.
- per-TU 状态: reset 清空, 与 include 路径和宏操作不同 (后者跨 reset 保留).
- 未显式设置时, 第一个被词法分析的输入文件 (文件或内存输入) 充当它.

### 9.3.11 libcp_context_define

```c
void libcp_context_define(LibcpContext *ctx, char *name, char *buf);
```

定义一个宏.

- `name` 是宏名.
- `buf` 是替换文本, 不是值. `"1"` 表示 `#define name 1`.
- 立即生效.
- 记录在配置里, reset 后重新应用.
- 顺序很重要, 后定义的覆盖先定义的.
- 已知缺陷: 这样定义的对象宏, `libcp_macro_expansion_macro` 当前返回 `NAME=body`, 不是 `NAME`; 源码里的 `#define` 正常. 见 11.7.8, 不要依赖 `-D` 宏的 `macro` 名.

### 9.3.12 libcp_context_undef

```c
void libcp_context_undef(LibcpContext *ctx, char *name);
```

取消一个宏.

- 立即生效.
- 记录在配置里, reset 后重新应用.
- 对未定义的宏是空操作.

### 9.3.13 libcp_context_set_predefined_macros

```c
void libcp_context_set_predefined_macros(LibcpContext *ctx, bool enabled);
```

打开或关闭预定义宏集.

- 默认打开.
- 设置在**下一次 reset** 生效; setter 本身不改当前 TU 已安装的集合.
- 关闭后, 预定义宏不安装, 但 `-D` / `-U` 仍然应用.

### 9.3.14 libcp_context_predefined_count / _at

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
- `name` 与固定 `value` 可能指向静态表, 不随 reset 失效; 由 context 在 per-TU 分配中构造的条目在 reset 后失效. 返回的指针都不要 `free`.
- 越界返回 `{NULL, NULL}`.
- 关闭预定义宏并 reset 之后 count 为 0; 只调用 setter 而未 reset 时, 当前 TU 的集合仍在.

### 9.3.15 分配器宏

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
- 库拥有的每个块 - context 对象, 配置数组与字符串, 每个 per-TU 对象及其链头部 - 都从这四个宏取得, 并通过 `LIBCP_FREE` 归还.
- 它们是编译期配置: 在编译 libcp 时定义, 不是在消费者里定义. 链接预编译库时重新定义不会改变已经编译好的库.
- 替换实现必须满足 libc 的语义: 返回满足任意类型对齐的内存(x86-64 上 `LibcpToken` / `LibcpNode` 内嵌 `long double`, 需要 16 字节), `LIBCP_CALLOC` 清零, `LIBCP_REALLOC` 失败时返回 NULL 且保留原块, `LIBCP_FREE` 接受 NULL.
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
- `-` 表示 stdin. 内容只读一次, 同一 TU (即 reset 之前) 内第二次 tokenize `-` 会拿到缓存. reset 会清空缓存; reset 后再次 tokenize `-` 读到空流, 因为 stdin 的 EOF 标志是粘滞的.
- 返回以 `TK_EOF` 结尾的链.
- 失败返回 NULL 并报告诊断. 失败原因: 打不开, 文件过大, 含 NUL, 非法 token.
- 未调用 `libcp_context_set_base_file` 时, 第一次调用会设置 `__BASE_FILE__` 的文件名.

### 9.4.2 libcp_tokenize_buffer

```c
LibcpToken *libcp_tokenize_buffer(LibcpContext *ctx, char *name,
                                      char *contents);
```

tokenize 一个 NUL 结尾的内存字符串.

- `name` 是显示文件名, 用于诊断.
- 输入被复制进 per-TU 分配.
- 长度由 `strlen` 求得, 所以缓冲区里的内嵌 NUL 会被静默截断, 不会报错. 要检测内嵌 NUL, 用 `libcp_tokenize_buffer_len`.
- 不做扫描缓冲改写: 不跳 BOM, 不删续行, 不改写 universal char. 见第 11.5.3 节.

### 9.4.3 libcp_tokenize_buffer_len

```c
LibcpToken *libcp_tokenize_buffer_len(LibcpContext *ctx, char *name,
                                          char *contents, int len);
```

tokenize 恰好 `len` 字节.

- 按恰好 `len` 字节读取; `len` 范围内出现 NUL 会报 `NUL byte in source file`, 返回 NULL. `_len` 不是"允许 NUL"的版本, 只是已知长度.
- `len` 为负报 `invalid buffer length`.
- 用于从文件映射, 网络缓冲等已知长度的输入.
- 与 `tokenize_buffer` 一样, 不做扫描缓冲改写: BOM, 续行, universal char 都按原字节处理.

### 9.4.4 libcp_reconstruct

```c
void libcp_reconstruct(LibcpContext *ctx, LibcpToken *tok, FILE *out);
```

按序写出每个 token 的 trivia 和拼写.

- 对 `tokenize_file` 与已处于扫描缓冲形态的 `tokenize_buffer_len` 输出, 结果等于输入字节; 原始 `\uXXXX` 的内存输入例外, 见 3.8 与 11.5.8.
- 对预处理后的流, 结果不是原文件. 宏体替代了调用点, 指令行消失.
- `out` 由调用者打开和关闭.
- 不需要先预处理.

### 9.4.5 libcp_context_input_file_count / _at

```c
int libcp_context_input_file_count(LibcpContext *ctx);
LibcpFile *libcp_context_input_file_at(LibcpContext *ctx, int index);
```

枚举当前翻译单元已读入的文件.

- 顺序是首次读取顺序; 主文件通常是索引 0, 由 `#include` 打开的文件随后.
- 只登记顶层输入与 include 读入的真实文件; 内存输入与宏展开产生的合成缓冲区不登记.
- `count` 在 `ctx` 为 NULL 时为 0; `at` 在 `index` 越界或 `ctx` 为 NULL 时为 NULL.
- 返回的 `LibcpFile` 归 context 所有, 下一次 reset 失效; 字段是只读的公共数据.
- 表是 per-TU 状态: reset 清空.

### 9.4.6 libcp_token_is_synthetic

```c
bool libcp_token_is_synthetic(const LibcpToken *tok);
```

判断 token 是否来自合成文件.

- 这是 file 级判定: `tok->file->raw == tok->file->contents`. 内存输入 (`libcp_tokenize_buffer*`) 也满足它; 真实文件不满足.
- 宏展开产物的 `@synth` 标记是另一回事, 按 token 的 provenance 判断 (见 9.9).
- `tok` 为 NULL 时返回 false.

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

返回 true 表示通过. 普通校验失败返回 false, 不报告诊断, 也不置 `failed`; 只有内部错误边界 (例如上下文已处于错误状态) 才可能返回 false 且 `failed` 为 true.

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
- 极值取自各节点的代表 token, 并额外收集 `decl.name_tok`, `access.arrow_tok` 与 `case_.colon_tok`: 例如 `ND_FUNCDEF` 的代表 token 是函数体的 `{`, 但 `range.c` 还会收集 `name_tok`, 所以整个函数定义的 range 从它的名字 token 开始, 不从声明说明符开始. `spec_decls` 只保存 declspec 中产生的声明记录 (如 enum 常量), 不是说明符 token, 不能用来取得更早边界; 需要更早边界时请回到 token / CST.
- 子树完全没有 token 时, `*start` 与 `*end` 都为 NULL.
- 坐标换算由内部 source 模块的 `libcp_token_source_pos` 统一提供; 需要物理行时用 `libcp_token_physical_line`, 它取文件的 `lines` / `map` 表, 不依赖 `line_no - line_delta`.
- 宏展开产物 token 的坐标在合成缓冲里, 跨文件比较按文件指针先序, 稳定
  但无语义.

### 9.7.3 libcp_node_family

```c
typedef enum {
  LIBCP_NF_NONE = 0,
  LIBCP_NF_CTRL,
  LIBCP_NF_DECL,
  LIBCP_NF_VALUE,
  LIBCP_NF_CASE,
  LIBCP_NF_ACCESS,
  LIBCP_NF_JUMP,
  LIBCP_NF_ASSIGN,
  LIBCP_NF_INCDEC,
  LIBCP_NF_ASM,
  LIBCP_NF_CAS,
} LibcpNodeFamily;

LibcpNodeFamily libcp_node_family(LibcpNodeKind kind);
```

返回 kind 所属的字段族. `LibcpNode` 的公共头字段 (`kind`, `next`, `ty`, `tok`,
`lhs`, `rhs`, `ty_op`, `spec_decls`) 由所有 kind 共享; 其余字段按族放在匿名
union 中, 访问形式为 `node->ctrl.cond`, `node->decl.attr`. `LIBCP_NF_NONE` 表示
该 kind 只使用公共头. 每个 kind 只使用一个族; 读取非活动族是未定义行为.
族的字段与 kind 对应见 `docs/06-layer3-parse.md` 的 6.3.2.

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
- 源码表达式位置的 `ND_IDENT` 和 `ND_STRING` 会被改写; 物化字符串时创建的 `ND_STRING` 节点保留原 kind.
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
- `macro`: 宏的拼写, NUL 结尾. 源码 `#define` 返回宏名; `-D` / `libcp_context_define` 定义的对象宏当前返回 `NAME=body`, 见 11.7.8.
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
#define LIBCP_VERSION "0.2.0"
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
| `libcp_cst_verify` | true | false (普通失败不置 `failed`; 内部错误边界才可能 false + `failed`) |
| `libcp_parse` | 树 | NULL + `failed`; 空 TU 为 NULL 且未失败 |
| `libcp_sema` | `LibcpObj` 链 | NULL + `failed`; 空 TU 为 NULL 且未失败 |
| `libcp_expansion_verify` | true | false |
| `libcp_reconstruct` / `_cst_reconstruct` / `_cst_dump` / `_expansion_dump` | 返回 | 提前返回 |
| `libcp_context_*` void 配置函数 | 返回 | 无返回; 分配失败时配置可能未生效 |

`libcp_context_failed` / `_predefined_count` / `_predefined_at` 是查询函数, 正常返回值, 不设错误边界.

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

- 库返回的所有指针由 context 拥有, 不要 `free`; `libcp_version` 返回字符串字面量, 预定义宏表里的静态 `name` 与固定 `value` 也不属于 per-TU 分配, 不随 reset 失效.
- 输入参数:
  - `tokenize_buffer*`: 复制输入字节, 调用者可以立即释放.
  - `add_include_path`: 复制路径.
  - `set_base_file`: 复制路径.
  - `define` / `undef`: 复制名字和替换文本.
  - `FILE *out`: 调用者拥有, 库不关闭.
- 诊断回调里的 `filename` / `message`: context 拥有, 到下一次 reset.
- `reset` 后, 由 context 做 per-TU 分配的指针失效; `libcp_version` 的字符串字面量与预定义宏表的静态 `name` / 固定 `value` 例外.
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
  int first = 1;
  for (LibcpObj *p = o->params; p; p = p->next) {
    if (!*p->name)  /* 跳过隐藏返回缓冲区参数 */
      continue;
    printf("%s%s", first ? "" : ", ", p->name);
    first = 0;
  }
  printf(")\n");
}
```

大 struct/union 返回值会多一个名字为空的隐藏首参数; 上面的 `if (*p->name)` 把它过滤掉. 需要完整 ABI 签名时不要过滤, 但要知道第一个参数可能是返回缓冲区.

用第 3 层也可以, 但参数名在类型记录的 per-parameter 副本上, 且没有 `LibcpObj` 方便.

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

公共入口的参数类型是 `char *`, 不是 `const char *`. C++ 里不能直接把字符串字面量传进去; 用可写缓冲区, 或显式 `const_cast<char *>` (仅当库确实不修改该缓冲时).

## 9.15 后端支持面

`<libcp/backend.h>` 服务库外后端: 它提供构造第 4 层节点, 取内建类型, 标注子树, 构造指针类型, 读取标签引用, 取唯一名与报告诊断所需的最小入口. 它不是库自带的第 5 层, 也不拥有层间的数据契约; 构造, 标注与诊断规则仍由库实现.

### 9.15.1 内建类型

```c
typedef enum {
  BT_VOID, BT_BOOL, BT_CHAR, BT_UCHAR, BT_SHORT, BT_USHORT, BT_INT,
  BT_UINT, BT_LONG, BT_ULONG, BT_FLOAT, BT_DOUBLE, BT_LDOUBLE,
} LibcpBuiltinType;

LibcpType *libcp_type_builtin(LibcpBuiltinType kind);
```

- 13 种内建类型分别映射到库内共享的类型记录.
- 返回记录跨 context 共享且只读, 消费者不得修改. 该查询不触碰 per-TU 状态, 因此不带 context.
- 记录的 `kind`, `size`, `align` 与 `is_unsigned` 直接可读; 例如 `BT_ULONG` 是 `TY_LONG`, 大小与对齐都是 8, `is_unsigned` 为 true.

### 9.15.2 节点构造

```c
LibcpNode *libcp_node_new(LibcpContext *ctx, LibcpNodeKind kind, LibcpToken *tok);
LibcpNode *libcp_node_binary(LibcpContext *ctx, LibcpNodeKind kind,
                             LibcpNode *lhs, LibcpNode *rhs, LibcpToken *tok);
LibcpNode *libcp_node_unary(LibcpContext *ctx, LibcpNodeKind kind,
                            LibcpNode *expr, LibcpToken *tok);
LibcpNode *libcp_node_num(LibcpContext *ctx, int64_t val, LibcpToken *tok);
LibcpNode *libcp_node_long(LibcpContext *ctx, int64_t val, LibcpToken *tok);
LibcpNode *libcp_node_arith(LibcpContext *ctx, LibcpNodeKind kind,
                            LibcpNode *lhs, LibcpNode *rhs, LibcpToken *tok);
LibcpNode *libcp_node_var(LibcpContext *ctx, LibcpObj *var, LibcpToken *tok);
LibcpNode *libcp_node_vla_ptr(LibcpContext *ctx, LibcpObj *var, LibcpToken *tok);
LibcpNode *libcp_node_alloca(LibcpContext *ctx, LibcpNode *sz);
LibcpNode *libcp_node_cast(LibcpContext *ctx, LibcpNode *expr, LibcpType *ty);
```

- 节点分配自当前 context 的 per-TU 登记链, 归 context 所有; 不提供逐对象 free, reset / free 时一起归还.
- `libcp_node_new` 只填 kind 与 tok, 其余字段为零.
- `libcp_node_num` / `_long` / `_unary` / `_binary` 产生降级过程使用的忠实形态; `_long` 的节点创建时即带 `long` 类型.
- `libcp_node_arith` 先标注两个操作数, 再施加常规算术转换, 返回节点的 `ty` 就是结果类型.
- `libcp_node_var` / `_vla_ptr` 携带已绑定的 `LibcpObj`; `libcp_node_alloca` 构造内建 `alloca` 的调用节点, 因此要求当前 context 至少跑过一次 `libcp_sema` (内建声明在那里安装).
- `libcp_node_cast` 构造 sema 风格的隐式转换: `value.is_implicit` 为 true, `ty` 是目标类型的浅拷贝, 其 `origin` 指向传入的 `ty`.

### 9.15.3 类型标注与指针类型

```c
void libcp_node_annotate_type(LibcpContext *ctx, LibcpNode *node);
LibcpType *libcp_type_pointer_to(LibcpContext *ctx, LibcpType *base);
```

- `libcp_node_annotate_type` 用 sema 的规则标注一棵子树: 填 `node->ty`, 插入标准要求的隐式转换, 运行需要已定型树的检查. 幂等: 已带 `ty` 的节点直接返回.
- `libcp_type_pointer_to` 从基础类型构造指针类型: `kind` 为 `TY_PTR`, 8 字节大小与对齐, `is_unsigned` 为 true, `base` 指向传入类型.

### 9.15.4 标签引用与唯一名

```c
int libcp_label_ref_count(LibcpContext *ctx, LibcpObj *fn);
LibcpNode *libcp_label_ref_at(LibcpContext *ctx, LibcpObj *fn, int index);
int libcp_context_take_unique_name(LibcpContext *ctx);
```

- sema 为每个已定义的函数收集 `goto` 语句与 [GNU] 标签值 (`&&label`) 节点, 顺序是标注遍历的访问顺序. `libcp_label_ref_count` 返回个数, 没有记录 (非函数对象或没有引用) 时为 0; `libcp_label_ref_at` 取第 `index` 个, 越界或负数返回 NULL.
- 引用节点仍是树的一部分: `kind` 为 `ND_GOTO` 或 `ND_LABEL_VAL`, `jump.label` 是源码里的标签名; 未声明标签的检查已由 sema 完成, 后端只需把名字映射到自己的跳转目标.
- 数组归 context 所有, 活到 reset; 消费者不要释放或改写节点.
- `libcp_context_take_unique_name` 取一个新编号并消耗它, 同一个号不会再从同一个 context 发出. 它与库为字符串字面量全局对象, 块作用域 static, 复合字面量和控制流标签生成 `.L..N` 名字所用的计数器是同一个, 所以消费者按返回的号构造自己的名字不会与库生成的对象冲突. 计数器随 `libcp_context_reset` 回到起点.

### 9.15.5 返回式诊断

```c
void libcp_diagnostic_report(LibcpContext *ctx, LibcpToken *tok,
                             LibcpDiagnosticSeverity severity,
                             const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
```

- 与库内部的 error / warn 路径共用同一套格式化与回调交付: 注册过回调时回调收到 `LibcpDiagnostic`, 没注册时按默认格式打印到 stderr; `tok` 作为锚点 token 与 `prov` 来源.
- 它返回, 不 longjmp. 错误把 context 标记为失败 (`libcp_context_failed` 为 true), 警告不标记. 消费者拿到控制权后自行决定后续动作.
- `tok` 可以为 NULL: 此时诊断没有文件与行列, 回调的 `filename` 为 NULL, `line` 与 `column` 为 0, 默认处理器只打印消息一行.
- `fmt` 走 printf 格式检查 (GCC/Clang 的 `format` 属性).

## 9.16 API 的边界

- 没有深拷贝树的 API. 第 4 层原地改写.
- 第 3 层有 `libcp_node_source_range` 取子树锚点范围; CST 节点仍没有 `start` / `end`, sema 的 `LibcpObj` 链也没有等价范围 API.
- 没有公共 formatter API. `tools/fmt.c` 是仓库内的测试消费者, 不随 `make install` 发布; 第 2 层本身是数据.
- 诊断已有 `tok` / `prov` 字段; 库不替消费者把整条链归结为单个"调用点位置".
- 没有虚拟文件系统. include 只走磁盘.
- 没有增量 API. 每次从头构建.
- 没有 ABI 承诺.
