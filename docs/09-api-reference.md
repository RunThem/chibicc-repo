# 09 API 参考与用法

本章是公共 API 的逐项说明. 函数原型以 `libchibicc/include/chibicc/` 下的头文件为准. 本章只列 36 个公共函数, 不列内部函数.

## 9.1 头文件, 链接与命名

只需要 include 一个头:

```c
#include <chibicc/chibicc.h>
```

它是伞头, 包含其余 8 个公共头:

```text
chibicc.h       伞头, CHIBICC_VERSION, chibicc_version
context.h       context, 诊断回调, 配置, 预定义宏
diagnostic.h    ChibiccDiagnostic, severity
token.h         ChibiccToken, ChibiccFile, 第 1 层入口
preprocess.h    chibicc_preprocess
provenance.h    宏展开访问器, dump / verify
cst.h           第 2 层
ast.h           第 3/4 层的数据类型与 chibicc_parse
sema.h          chibicc_sema
```

链接:

```sh
cc -std=c11 -I/path/to/include app.c -L/path/to/lib -lchibicc
```

或直接给静态库路径:

```sh
cc -std=c11 -Ilibchibicc/include app.c libchibicc.a
```

命名规则:

- 函数一律 `chibicc_` 前缀.
- 类型一律 `Chibicc` 前缀.
- 结构体 tag 是 `struct chibicc_*`.
- 枚举常量保留层前缀: `TK_` (token), `ND_` (node), `TY_` (type), `CST_` (CST), `INIT_` (initializer).
- 宏是 `CHIBICC_*`.

`CHIBICC_API` 在 GCC/Clang 下展开为 `__attribute__((visibility("default")))`, 用于共享库导出. 消费者不需要关心它.

## 9.2 函数总表

| 组 | 函数 |
|---|---|
| context | `chibicc_context_new`, `_free`, `_reset`, `_set_diagnostic`, `_failed` |
| 配置 | `chibicc_context_add_include_path`, `_define`, `_undef`, `_set_predefined_macros`, `_predefined_count`, `_predefined_at` |
| 第 1 层 | `chibicc_tokenize_file`, `chibicc_tokenize_buffer`, `chibicc_tokenize_buffer_len`, `chibicc_reconstruct` |
| 预处理 | `chibicc_preprocess` |
| 第 2 层 | `chibicc_cst_parse`, `chibicc_cst_reconstruct`, `chibicc_cst_dump`, `chibicc_cst_verify` |
| 第 3 层 | `chibicc_parse` |
| 第 4 层 | `chibicc_sema` |
| provenance | `chibicc_token_provenance`, `chibicc_macro_prov_next`, `_expansion`, `_arg_index` |
| provenance | `chibicc_macro_expansion_name`, `_end`, `_macro`, `_def`, `_arg`, `_arg_count`, `_is_builtin` |
| provenance | `chibicc_expansion_dump`, `chibicc_expansion_verify` |
| 版本 | `chibicc_version`, `CHIBICC_VERSION` |

## 9.3 生命周期与配置

### 9.3.1 chibicc_context_new

```c
ChibiccContext *chibicc_context_new(void);
```

创建一个 context. 一个 context 服务一个翻译单元.

行为:

- 初始化 arena 和全部 per-TU 状态.
- 安装 47 个预定义宏.
- 不添加任何 include 搜索路径.
- 不注册诊断回调, 默认为"按驱动器格式打印到 stderr".

返回值: 正常路径上总是非 NULL. 内存分配失败没有单独的处理路径, 不构成"返回 NULL 的错误模型".

### 9.3.2 chibicc_context_free

```c
void chibicc_context_free(ChibiccContext *ctx);
```

销毁 context:

- 释放 arena 中的全部对象.
- 释放 include 路径和 `-D` / `-U` 配置数组.
- 清除 active context 指针.

`ctx` 为 NULL 时安全返回. 调用后所有指针失效.

### 9.3.3 chibicc_context_reset

```c
void chibicc_context_reset(ChibiccContext *ctx);
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

### 9.3.4 chibicc_context_set_diagnostic

```c
void chibicc_context_set_diagnostic(ChibiccContext *ctx,
                                    ChibiccDiagnosticFn fn, void *user);
```

注册诊断回调.

```c
typedef void (*ChibiccDiagnosticFn)(void *user,
                                    const ChibiccDiagnostic *);
```

- 注册后, 库不再向 stderr 打印.
- `user` 原样传给回调.
- `fn` 为 NULL 时恢复默认打印行为.
- 回调里的字符串只保证到下一次 reset.

### 9.3.5 chibicc_context_failed

```c
bool chibicc_context_failed(ChibiccContext *ctx);
```

返回上一次操作是否出错.

- 错误经 longjmp 回到公共入口时置位.
- 警告不置位.
- `reset` 清除.

用途: 区分"返回 NULL 是错误"和"返回 NULL 是空翻译单元".

### 9.3.6 chibicc_context_add_include_path

```c
void chibicc_context_add_include_path(ChibiccContext *ctx, char *path);
```

追加一个 include 搜索目录.

- 内部复制字符串, 调用者可以释放 `path`.
- 顺序就是搜索顺序.
- 没有默认系统路径. 需要 `/usr/include` 就自己加.
- 没有返回值. 内存分配失败时该路径可能不被记录.

库本身不读环境变量, 不加编译器的默认路径.

### 9.3.7 chibicc_context_define

```c
void chibicc_context_define(ChibiccContext *ctx, char *name, char *buf);
```

定义一个宏.

- `name` 是宏名.
- `buf` 是替换文本, 不是值. `"1"` 表示 `#define name 1`.
- 立即生效.
- 记录在配置里, reset 后重新应用.
- 顺序很重要, 后定义的覆盖先定义的.

### 9.3.8 chibicc_context_undef

```c
void chibicc_context_undef(ChibiccContext *ctx, char *name);
```

取消一个宏.

- 立即生效.
- 记录在配置里, reset 后重新应用.
- 对未定义的宏是空操作.

### 9.3.9 chibicc_context_set_predefined_macros

```c
void chibicc_context_set_predefined_macros(ChibiccContext *ctx, bool enabled);
```

打开或关闭预定义宏集.

- 默认打开.
- 设置在**下一次 reset** 生效.
- 关闭后, 预定义宏不安装, 但 `-D` / `-U` 仍然应用.

### 9.3.10 chibicc_context_predefined_count / _at

```c
int chibicc_context_predefined_count(ChibiccContext *ctx);
ChibiccPredefinedMacro chibicc_context_predefined_at(ChibiccContext *ctx,
                                                     int index);
```

枚举当前 TU 安装的预定义宏.

```c
typedef struct chibicc_predefined_macro {
  const char *name;
  const char *value;
} ChibiccPredefinedMacro;
```

- `value` 是替换文本. 内建宏和动态宏为 NULL.
- 字符串由 context 拥有, reset 后失效.
- 越界返回 `{NULL, NULL}`.
- 关闭预定义宏后 count 为 0.

## 9.4 第 1 层 API

### 9.4.1 chibicc_tokenize_file

```c
ChibiccToken *chibicc_tokenize_file(ChibiccContext *ctx, char *path);
```

读取文件并 tokenize.

- `path` 是文件系统路径.
- `-` 表示 stdin. 内容只读一次, 同一 context 内第二次 tokenize `-` 会拿到缓存.
- 返回以 `TK_EOF` 结尾的链.
- 失败返回 NULL 并报告诊断. 失败原因: 打不开, 读失败, 文件过大, 含 NUL, 非法 token.
- 第一次调用会设置 `__BASE_FILE__` 的文件名.

### 9.4.2 chibicc_tokenize_buffer

```c
ChibiccToken *chibicc_tokenize_buffer(ChibiccContext *ctx, char *name,
                                      char *contents);
```

tokenize 一个 NUL 结尾的内存字符串.

- `name` 是显示文件名, 用于诊断.
- 输入被复制进 arena.
- 不能用包含 NUL 的二进制块; 用 `_len` 版本.
- 不做扫描缓冲改写: 不跳 BOM, 不删续行, 不改写 universal char. 见第 11.5.3 节.

### 9.4.3 chibicc_tokenize_buffer_len

```c
ChibiccToken *chibicc_tokenize_buffer_len(ChibiccContext *ctx, char *name,
                                          char *contents, int len);
```

tokenize 恰好 `len` 字节.

- 缓冲区可以包含 NUL. 出现 NUL 会报 `NUL byte in source file`, 返回 NULL.
- `len` 为负报 `invalid buffer length`.
- 用于从文件映射, 网络缓冲等已知长度的输入.
- 与 `tokenize_buffer` 一样, 不做扫描缓冲改写: BOM, 续行, universal char 都按原字节处理.

### 9.4.4 chibicc_reconstruct

```c
void chibicc_reconstruct(ChibiccContext *ctx, ChibiccToken *tok, FILE *out);
```

按序写出每个 token 的 trivia 和拼写.

- 对 `tokenize_file` / `tokenize_buffer_len` 的直接输出, 结果等于输入文件的原始字节.
- 对预处理后的流, 结果不是原文件. 宏体替代了调用点, 指令行消失.
- `out` 由调用者打开和关闭.
- 不需要先预处理.

## 9.5 预处理 API

### 9.5.1 chibicc_preprocess

```c
ChibiccToken *chibicc_preprocess(ChibiccContext *ctx, ChibiccToken *tok);
```

执行预处理.

- 输入是一条 token 链.
- 输出是展开后的链, 以 `TK_EOF` 结尾.
- **输入链被原地改写**. 调用后不要再用原指针, 除非只做重建等只读操作并且确认未被破坏.
- 失败返回 NULL 并报告诊断.
- 展开产物带 provenance 链.

## 9.6 第 2 层 API

### 9.6.1 chibicc_cst_parse

```c
ChibiccCstNode *chibicc_cst_parse(ChibiccContext *ctx, ChibiccToken *tok);
```

从**预处理之前**的 token 流建 lossless CST.

- 解析是全函数, 不报语法诊断, 不退出.
- 返回 `CST_TRANSLATION_UNIT` 根.
- 叶子按序覆盖输入 token, 包括 EOF.
- 输入链在调用中不被改写.

### 9.6.2 chibicc_cst_reconstruct

```c
void chibicc_cst_reconstruct(ChibiccContext *ctx, ChibiccCstNode *root,
                             FILE *out);
```

按叶子顺序写出 trivia 和拼写.

对直接 tokenize 输出, 结果等于原文件字节.

### 9.6.3 chibicc_cst_dump

```c
void chibicc_cst_dump(ChibiccContext *ctx, ChibiccCstNode *root, FILE *out);
```

打印树形 dump, 每层缩进两格. 叶子打印种类和拼写.

### 9.6.4 chibicc_cst_verify

```c
bool chibicc_cst_verify(ChibiccContext *ctx, ChibiccCstNode *root,
                        ChibiccToken *tok);
```

校验树的叶子序列与 token 流:

- 数量一致.
- 每个叶子指向同一个 token 对象.
- 顺序一致.

返回 true 表示通过. 失败返回 false, 不报告诊断.

## 9.7 第 3 层 API

### 9.7.1 chibicc_parse

```c
ChibiccNode *chibicc_parse(ChibiccContext *ctx, ChibiccToken *tok);
```

从预处理后的 token 流建忠实语法 AST.

- 输入必须是 `chibicc_preprocess` 的输出.
- 返回顶层声明记录链.
- 空翻译单元返回 NULL, `failed` 为 false.
- 语法错误返回 NULL, `failed` 为 true.
- 表达式不做类型计算, 名字未绑定, 常量未求值. 数值字面量例外, 带词法类型.
- 深度上限 512.

## 9.8 第 4 层 API

### 9.8.1 chibicc_sema

```c
ChibiccObj *chibicc_sema(ChibiccContext *ctx, ChibiccNode *toplevel);
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
ChibiccMacroProv *chibicc_token_provenance(ChibiccToken *tok);
ChibiccMacroProv *chibicc_macro_prov_next(ChibiccMacroProv *p);
ChibiccMacroExpansion *chibicc_macro_prov_expansion(ChibiccMacroProv *p);
int chibicc_macro_prov_arg_index(ChibiccMacroProv *p);
```

- `chibicc_token_provenance`: 返回 token 的链头, 源 token 返回 NULL.
- `chibicc_macro_prov_next`: 下一层, 即更外层的调用上下文.
- `chibicc_macro_prov_expansion`: 该层对应的展开记录.
- `chibicc_macro_prov_arg_index`: `-1` 表示宏体, `>= 0` 表示实参序号, 0 起.

这些访问器不分配, 不需要错误边界. 传入 NULL 会崩溃, 调用者保证非 NULL.

### 9.9.2 展开记录访问器

```c
ChibiccToken *chibicc_macro_expansion_name(ChibiccMacroExpansion *e);
ChibiccToken *chibicc_macro_expansion_end(ChibiccMacroExpansion *e);
char *chibicc_macro_expansion_macro(ChibiccMacroExpansion *e);
ChibiccToken *chibicc_macro_expansion_def(ChibiccMacroExpansion *e);
ChibiccToken *chibicc_macro_expansion_arg(ChibiccMacroExpansion *e, int i);
int chibicc_macro_expansion_arg_count(ChibiccMacroExpansion *e);
bool chibicc_macro_expansion_is_builtin(ChibiccMacroExpansion *e);
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
void chibicc_expansion_dump(ChibiccContext *ctx, ChibiccToken *tok, FILE *out);
bool chibicc_expansion_verify(ChibiccContext *ctx, ChibiccToken *tok);
```

- `dump`: 每个非 EOF token 一行.
- `verify`: 校验链的深度, 唯一性, 记录完整性, 名字一致, 实参序号. 失败时向 stderr 打印一条消息并返回 false.

## 9.10 版本 API

```c
#define CHIBICC_VERSION "0.1.0"
const char *chibicc_version(void);
```

`CHIBICC_VERSION` 与 Makefile 的 `VERSION` 同步, 由 `test/install.sh` 校验.

只承诺源码兼容. 公共结构体字段变化就是版本变化.

## 9.11 返回值与错误约定

| 入口 | 成功 | 失败 |
|---|---|---|
| `chibicc_tokenize_file` | token 链 | NULL + `failed` |
| `chibicc_tokenize_buffer` | token 链 | NULL + `failed` |
| `chibicc_tokenize_buffer_len` | token 链 | NULL + `failed` |
| `chibicc_preprocess` | token 链 | NULL + `failed` |
| `chibicc_cst_parse` | 树 | NULL + `failed` |
| `chibicc_cst_verify` | true | false (或 false + `failed`) |
| `chibicc_parse` | 树 | NULL + `failed`; 空 TU 为 NULL 且未失败 |
| `chibicc_sema` | `Obj` 链 | NULL + `failed`; 空 TU 为 NULL 且未失败 |
| `chibicc_expansion_verify` | true | false |
| `chibicc_reconstruct` / `_cst_reconstruct` / `_cst_dump` / `_expansion_dump` | 返回 | 提前返回 |
| `chibicc_context_*` 配置 | 返回 | 无返回; 分配失败时配置可能未生效 |

错误处理模板:

```c
ChibiccNode *ast = chibicc_parse(ctx, tok);
if (chibicc_context_failed(ctx)) {
  /* 错误已经通过回调报告 */
  chibicc_context_reset(ctx);
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

下面的程序已经用 `cc -std=c11 -Ilibchibicc/include example.c libchibicc.a` 编译并运行. 它依次使用第 1 层, 第 2 层, 编译路径和 provenance.

```c
#include <chibicc/chibicc.h>
#include <stdio.h>
#include <string.h>

static void on_diag(void *user, const ChibiccDiagnostic *d) {
  (void)user;
  fprintf(stderr, "%s:%d:%d: %s\n",
          d->filename ? d->filename : "<input>",
          d->line, d->column, d->message);
}

static int count(ChibiccObj *o) {
  int n = 0;
  for (; o; o = o->next)
    n++;
  return n;
}

static int pipeline(ChibiccContext *ctx, char *src) {
  ChibiccToken *tok = chibicc_tokenize_buffer(ctx, "<example>", src);
  if (!tok)
    return 1;

  tok = chibicc_preprocess(ctx, tok);
  if (!tok)
    return 1;

  ChibiccNode *ast = chibicc_parse(ctx, tok);
  if (!ast && chibicc_context_failed(ctx))
    return 1;

  ChibiccObj *prog = chibicc_sema(ctx, ast);
  if (!prog && chibicc_context_failed(ctx))
    return 1;

  for (ChibiccObj *o = prog; o; o = o->next) {
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
  ChibiccContext *ctx = chibicc_context_new();
  chibicc_context_set_diagnostic(ctx, on_diag, NULL);

  char *src =
      "struct point { int x; int y; };\n"
      "#define N 3\n"
      "int g = N;\n"
      "int add(int a, int b) { return a + b; }\n";

  /* Layer 1: reconstruct the exact bytes. */
  ChibiccToken *raw = chibicc_tokenize_buffer(ctx, "<example>", src);
  FILE *f = tmpfile();
  chibicc_reconstruct(ctx, raw, f);
  fflush(f);
  rewind(f);
  char buf[256];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  printf("reconstruct equal: %d\n", strcmp(buf, src) == 0);

  /* Layer 2: build the CST from a second tokenize. */
  ChibiccToken *raw2 = chibicc_tokenize_buffer(ctx, "<example>", src);
  ChibiccCstNode *cst = chibicc_cst_parse(ctx, raw2);
  printf("cst root=%d children=%d verify=%d\n",
         (int)cst->kind, cst->child_count,
         (int)chibicc_cst_verify(ctx, cst, raw2));

  /* Compile path. */
  if (pipeline(ctx, src))
    return 1;

  /* Provenance of the N expansion. */
  ChibiccToken *t = chibicc_tokenize_buffer(ctx, "<example>", src);
  t = chibicc_preprocess(ctx, t);
  for (; t && t->kind != TK_EOF; t = t->next) {
    ChibiccMacroProv *p = chibicc_token_provenance(t);
    if (!p)
      continue;
    ChibiccMacroExpansion *e = chibicc_macro_prov_expansion(p);
    printf("expanded %.*s via %s (arg %d)\n",
           t->len, t->loc,
           chibicc_macro_expansion_macro(e),
           chibicc_macro_prov_arg_index(p));
  }

  chibicc_context_free(ctx);
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
ChibiccToken *raw = chibicc_tokenize_file(ctx, path);
ChibiccCstNode *cst = chibicc_cst_parse(ctx, raw);
/* 遍历 cst, 注释在叶子 token 的 trivia 里 */
```

不要先预处理. 需要同时编译时, 对同一个文件 tokenize 两次.

### 9.14.2 只做词法

```c
ChibiccToken *tok = chibicc_tokenize_file(ctx, path);
for (ChibiccToken *t = tok; t && t->kind != TK_EOF; t = t->next) {
  /* 读 t->raw_loc / t->raw_len / t->trivia */
}
```

原始流里关键字是 `TK_IDENT`. 要分类就自己比较拼写, 或调用预处理.

### 9.14.3 提取函数签名

用第 4 层:

```c
ChibiccObj *prog = chibicc_sema(ctx, ast);
for (ChibiccObj *o = prog; o; o = o->next) {
  if (!o->is_function || !o->is_definition)
    continue;
  printf("%s(", o->name);
  for (ChibiccObj *p = o->params; p; p = p->next)
    printf("%s%s", p == o->params ? "" : ", ", p->name);
  printf(")\n");
}
```

用第 3 层也可以, 但参数名在类型记录的 per-parameter 副本上, 且没有 `Obj` 方便.

### 9.14.4 追踪宏展开

```c
ChibiccToken *t = chibicc_preprocess(ctx, tok);
for (; t && t->kind != TK_EOF; t = t->next) {
  for (ChibiccMacroProv *p = chibicc_token_provenance(t); p;
       p = chibicc_macro_prov_next(p)) {
    ChibiccMacroExpansion *e = chibicc_macro_prov_expansion(p);
    printf("%.*s <- %s\n", t->len, t->loc,
           chibicc_macro_expansion_macro(e));
  }
}
```

### 9.14.5 批量处理多个 TU

```c
ChibiccContext *ctx = chibicc_context_new();
/* 配置一次 */
for (int i = 0; i < n; i++) {
  process(ctx, files[i]);
  chibicc_context_reset(ctx);
}
chibicc_context_free(ctx);
```

配置在 reset 后保留. 每个文件之间状态隔离.

### 9.14.6 出错后复用

```c
if (chibicc_context_failed(ctx)) {
  chibicc_context_reset(ctx);
  /* 重新开始 */
}
```

不要在失败后用部分树. 库不保证一致.

### 9.14.7 在构建系统里使用

```sh
# pkg-config
cc app.c $(pkg-config --cflags --libs libchibicc)

# 手工
cc -std=c11 -I/usr/local/include app.c -L/usr/local/lib -lchibicc
```

安装布局见第 10 章.

### 9.14.8 从 C++ 调用

公共头是 C11 头, 没有 `extern "C"` 包装. C++ 消费者需要自己包:

```cpp
extern "C" {
#include <chibicc/chibicc.h>
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
