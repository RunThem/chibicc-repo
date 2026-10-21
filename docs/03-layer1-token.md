# 03 第 1 层: token 流与 trivia

## 3.1 作用与位置

第 1 层是库的地基. 它把源文件的字节切成 token, 并且**不丢掉任何字节**:

- 空白, 换行, 注释, 反斜杠续行都以 trivia 的形式挂在后一个 token 上.
- 每个 token 记录它在原始文件中的拼写切片.
- token 流按序可逐字节重建输入文件.

这一层不认关键字, 不做宏展开, 不做名字绑定. 它只回答"文件里有哪些 token, 它们在哪儿, 它们前面是什么".

第 1 层是 CST 和预处理共同的输入. 所有其他层都间接依赖它.

## 3.2 设计思路

### 3.2.1 为什么保留 trivia

去掉空白和注释的 token 流对编译器足够, 对格式化器不够. 格式化器必须知道:

- 两个 token 之间有没有空格, 有几个.
- 有没有空行.
- 注释写在哪里, 是行注释还是块注释.
- 代码在物理行上怎么排列.

如果词法器丢弃这些信息, 格式化器只能重新猜. 重新猜就会改变用户的代码风格, 甚至改变语义, 例如 `a - -b` 和 `a--b`.

因此 trivia 是一等公民, 不是"词法器的中间状态". 它保存在 token 上, 坐标是原始文件的坐标.

### 3.2.2 为什么需要两套坐标

词法器不能直接在原始字节上扫描. 有三个原因:

- CRLF 和孤立 CR 要归一成 `\n`, 否则行注释和字符串处理要到处判断.
- 反斜杠续行要在扫描前删除, 否则 token 会被物理换行切断.
- universal character name, 例如 `\u00e9`, 要改写成 UTF-8 字节, 否则标识符比较和关键字识别会出错.

这些改写都会改变字节偏移. 如果 token 只记录扫描缓冲的偏移, 重建原始文件时就会错位.

库的解决办法是双缓冲加映射:

- `File.contents` 是扫描缓冲, 经过上述改写, 末尾保证有换行.
- `File.raw` 是原始文本, 一个字节都不改.
- `File.map[i]` 给出扫描偏移 `i` 对应的 raw 偏移.
- `File.lines[i]` 给出 raw 偏移 `i` 所在的行号.
- token 的 `loc` / `len` 是扫描坐标, `raw_loc` / `raw_len` 是 raw 坐标, `trivia` / `trivia_len` 也是 raw 坐标.

两张映射表由 `make_scan_buffer` 一遍扫描同时产出. 这是 trivia 线的核心设计, 取代了早期"就地改写原文件再补洞"的方案.

### 3.2.3 为什么 trivia 挂在后一个 token 上

前置 trivia 的归属是明确无歧义的: 两个 token 之间的字节属于后一个 token. 这样重建算法非常简单:

```text
for each token:
    output token.trivia[0 .. trivia_len]
    output token.raw_loc[0 .. raw_len]
```

文件末尾的 trivia 属于 `TK_EOF`. 文件输入时, 第一个 token 的前置 trivia 包含 BOM, 如果文件有 BOM.

内存输入不同: `chibicc_tokenize_buffer*` 不经过 `make_scan_buffer`, 不做 BOM 跳过. BOM 会留在第一个 token 的拼写里, 例如 `\xEF\xBB\xBFint` 被读成一个标识符. 从内存传入带 BOM 的文本时, 调用者要自己剥离 BOM.

`make_scan_buffer` 为了扫描方便可能在扫描缓冲末尾补一个换行, 这个换行不属于 raw 文本, 因此不出现在任何 token 的 raw 切片里. 所以重建结果等于磁盘上的文件, 不是等于扫描缓冲.

## 3.3 数据模型

### 3.3.1 ChibiccToken

```c
typedef enum {
  TK_IDENT,   /* 标识符 */
  TK_PUNCT,   /* 标点 */
  TK_KEYWORD, /* 关键字 */
  TK_STR,     /* 字符串字面量 */
  TK_NUM,     /* 数值字面量 */
  TK_PP_NUM,  /* 预处理数字 */
  TK_EOF,     /* 文件结束 */
} ChibiccTokenKind;

struct chibicc_token {
  ChibiccTokenKind kind;
  ChibiccToken *next;
  int64_t val;        /* TK_NUM 的整数值 */
  long double fval;   /* TK_NUM 的浮点值 */
  char *loc;          /* 拼写在扫描缓冲中的起点 */
  int len;            /* 拼写在扫描缓冲中的长度 */

  char *trivia;       /* 前置 trivia 在 raw 文本中的起点 */
  int trivia_len;
  char *raw_loc;      /* 拼写在 raw 文本中的起点 */
  int raw_len;

  ChibiccType *ty;    /* TK_NUM / TK_STR 的类型 */
  char *str;          /* 字符串字面量的解码内容, 以 NUL 结尾 */

  ChibiccFile *file;
  char *filename;     /* 显示文件名, 受 #line 影响 */
  int line_no;
  int line_delta;
  bool at_bol;        /* 是否位于逻辑行首 */
  bool has_space;     /* 前面是否有空白 */

  ChibiccHideset *hideset;  /* 宏展开用 */
  ChibiccToken *origin;     /* 宏展开的最外层调用点 token */
  ChibiccMacroProv *prov;   /* 展开来源链, 源 token 为 NULL */
};
```

字段的使用规则:

- `loc` / `len` 指向扫描缓冲, 用于词法分析和快速拼写比较. 它们**不保证**是 NUL 结尾的字符串, 不要直接 `strcmp`.
- `raw_loc` / `raw_len` 指向 raw 文本, 用于重建和精确位置.
- `val` 和 `fval` 只在 `TK_NUM` 上有意义.
- `str` 只在 `TK_STR` 上有意义, 是解码后的内容, 例如 `"a\n"` 的 `str` 是两个字节 `a` 和换行, 加结尾 NUL. 原始拼写仍在 `loc` 中.
- `filename` 是显示名, `#line` 可以改它. `file->name` 是物理路径.
- `line_no` 在原始 token 流里是物理行号; 在预处理后的流里叠了 `#line` 偏移.
- `at_bol` / `has_space` 是词法状态, 预处理用它们判断指令是否在行首.
- `hideset` / `origin` / `prov` 在预处理时填写, 第 1 层消费者通常不读.

### 3.3.2 ChibiccFile

```c
struct chibicc_file {
  char *name;          /* 物理文件名 */
  int file_no;         /* 文件编号, codegen 的 .file 指令用 */
  int include_depth;   /* include 深度 */

  char *contents;      /* 扫描缓冲 */
  char *raw;           /* 原始文本 */
  int *map;            /* 扫描偏移 -> raw 偏移 */
  int *lines;          /* raw 偏移 -> 行号 */

  char *display_name;  /* #line 显示名 */
  int line_delta;      /* #line 行偏移 */
};
```

`map` 和 `lines` 的长度是字节数加 1, 最后一项覆盖文件末尾.

对 `chibicc_tokenize_buffer` 的输入, 没有磁盘文件, 也没有改写: `raw == contents`, `map` 是恒等映射, `lines` 直接数换行. 这类 token 在 provenance dump 里显示为 `@synth`, 见第 4 章.

### 3.3.3 关键字的时机

**原始 token 流里的关键字是 `TK_IDENT`**. `int`, `return`, `while` 都先按标识符读入. 关键字分类发生在 `convert_pp_tokens`, 由 `chibicc_preprocess` 调用.

这意味着:

- 只用第 1 层或第 2 层时, 判断一个 token 是不是关键字要比较拼写, 不能看 `kind == TK_KEYWORD`.
- `TK_PP_NUM` 在预处理时才转成 `TK_NUM`.
- 数值常量的类型 (`ty`) 也是在预处理阶段补上的.

这是上游的行为, 库没有改. 对 CST 来说这不影响正确性, 因为 CST 按拼写识别关键字.

## 3.4 实现流程

### 3.4.1 读文件

`tokenize_file(path)`:

1. 调用 `read_file` 读入整个文件. 读 stdin 时只读一次, 内容缓存在 context 里, 第二次 tokenize `-` 会拿到同一份文本.
2. 检查文件大小不超过 `INT32_MAX`, 因为词法器用 `int` 存偏移.
3. 检查 NUL 字节. 源文件里出现 NUL 是诊断, 不是静默截断.
4. 分配 `ChibiccFile`, 编号并登记到 context 的输入文件表.

`tokenize_buffer(name, contents)` 走另一条路: 把传入的字节复制进 arena, 直接构造一个没有改写的 `ChibiccFile`. 内存中的输入不经过 `make_scan_buffer`.

### 3.4.2 构造扫描缓冲

`make_scan_buffer(raw, len, &map, &lines)` 一遍扫描完成五件事:

1. 跳过开头的 BOM.
2. 把 `\r\n` 和孤立的 `\r` 归一成 `\n`.
3. 删除反斜杠加换行的续行.
4. 把 `\uXXXX` / `\UXXXXXXXX` 改写成 UTF-8 字节.
5. 保证扫描缓冲以 `\n` 结尾.

同时填写 `map` 和 `lines`:

- `map[scan_offset] = raw_offset`.
- `lines[raw_offset] = 行号`.

对多字节改写, `map` 把同一个 raw 偏移映射给多个扫描偏移; 反向的"扫描位置对应的 raw 结束位置"由 `raw_end_of` 计算, 它认识换行和 universal character name 的长度.

### 3.4.3 切 token

`tokenize(file)` 是一个循环:

1. 跳过行注释, 块注释, 换行和空白, 只推进指针, 不产生 token. 跳过的字节留给下一个 token 当 trivia.
2. 按首字符分派:
   - 数字或 `.` 加数字: `TK_PP_NUM`
   - `"` / `u8"` / `u"` / `U"` / `L"`: 字符串字面量, 读入时解码转义
   - `'` / `u'` / `U'` / `L'`: 字符字面量, 作为整数常量
   - 标识符: `TK_IDENT`, 支持 UTF-8 标识符
   - 标点: `TK_PUNCT`, 最长匹配
3. 每个 token 由 `new_token` 填写:
   - `loc` / `len`: 扫描坐标
   - `trivia` / `trivia_len`: 本 token 之前的跳过字节, raw 坐标
   - `raw_loc` / `raw_len`: 拼写, raw 坐标
   - `file`, `filename`, `at_bol`, `has_space`
4. 循环结束时追加 `TK_EOF`, 它的前置 trivia 是文件末尾的全部字节.
5. `add_line_numbers` 给每个 token 填物理行号.

`new_token` 里有一个细节: `raw_len` 不是简单的 `len`. 扫描缓冲中的最后一个字节可能对应 raw 文本中的多个字节, 例如一个 universal character name. `raw_end_of` 负责算对结束位置.

### 3.4.4 重建

`chibicc_reconstruct(ctx, tok, out)` 就是前面那段循环:

```c
for (; tok; tok = tok->next) {
  fwrite(tok->trivia, 1, tok->trivia_len, out);
  fwrite(tok->raw_loc, 1, tok->raw_len, out);
}
```

在 `tokenize_file` / `tokenize_buffer_len` 的直接输出上, 输出等于输入文件的原始字节.

## 3.5 阶段产物

第 1 层的产物是一条 `ChibiccToken` 链:

- 以 `TK_EOF` 结尾, 不会为 NULL.
- 每个 token 带 raw 坐标的 trivia 和拼写.
- 每个 token 带物理行号.
- 字符串字面量带解码后的 `str`.
- 数值字面量在预处理前是 `TK_PP_NUM`, 还没有 `val` / `ty`.

产物之外还有两个可观察能力:

- `chibicc_reconstruct` 重建原始字节.
- 驱动器 `-dump-tokens` 打印 token 与 trivia, 用于调试.

## 3.6 产物可以做什么

- **源码重建**: 从 token 流还原文件, 验证工具没有丢字节.
- **词法级搜索**: 找到所有标识符, 字符串, 数字, 注释边界.
- **格式化器的底层数据**: 决定两个 token 之间保留几个空格, 注释放在哪.
- **行号映射**: 从 token 的 raw 坐标算物理行, 从 `line_no` 算显示行.
- **自定义预处理**: 消费者可以只做词法, 自己实现宏展开或条件编译, 不走 `chibicc_preprocess`.
- **输入检查**: NUL 字节, 文件过大, 非法 token 都会报告.

不能做:

- 不能直接判断关键字, 除非先预处理或自己比较拼写.
- 不能知道宏展开后的内容.
- 不能知道 token 的语法角色.

## 3.7 API 用法

### 3.7.1 从文件 tokenize

```c
ChibiccContext *ctx = chibicc_context_new();
ChibiccToken *tok = chibicc_tokenize_file(ctx, "input.c");
if (!tok) {
  /* 错误已经通过回调报告, ctx 已失败 */
  chibicc_context_free(ctx);
  return 1;
}
```

### 3.7.2 从内存 tokenize

```c
char *src = "int x = 1; /* comment */\n";
ChibiccToken *tok = chibicc_tokenize_buffer(ctx, "<memory>", src);
```

`chibicc_tokenize_buffer` 用 `strlen` 求长度, 不能包含 NUL. 已知长度时用:

```c
ChibiccToken *tok = chibicc_tokenize_buffer_len(ctx, "<memory>", buf, len);
```

`len` 是精确字节数. 缓冲区里有 NUL 会得到一条 `NUL byte in source file` 诊断, 入口返回 NULL.

### 3.7.3 遍历 token 并打印 trivia

```c
for (ChibiccToken *t = tok; t && t->kind != TK_EOF; t = t->next) {
  printf("line %d: trivia=%d bytes, token=%.*s\n",
         t->line_no, t->trivia_len, t->raw_len, t->raw_loc);
}
```

### 3.7.4 重建

```c
FILE *out = fopen("rebuilt.c", "wb");
chibicc_reconstruct(ctx, tok, out);
fclose(out);
```

重建不需要预处理, 也不要求 token 来自文件. 对内存 buffer 同样有效.

### 3.7.5 只读调试通道

```sh
./chibicc -dump-tokens input.c
./chibicc -reconstruct input.c > rebuilt.c
cmp input.c rebuilt.c
```

`-dump-tokens` 的格式是每个 token 一行, 带 T (trivia) 和 R (raw spelling) 信息. 具体形状由 `test/trivia.sh` 锁定.

## 3.8 边界与已知限制

- **trivia 契约只覆盖直接输出**. 宏展开, 字符串化, 粘贴, 内建宏产生的 token 是合成 token. 它们的 `trivia` / `raw_loc` 指向派生来源或合成缓冲. 对预处理后的流调用 `chibicc_reconstruct` 不会还原原文件, 而是输出展开流在 raw 坐标上的拼接. 第 4 章有例子.
- **关键字后置分类**. 原始流里 `TK_IDENT` 包含关键字. 只用第 1/2 层时要按拼写判断.
- **注释没有分类**. trivia 是原始字节. 库不告诉你"这是文档注释"或"这是行尾注释". 需要的话自己解析 trivia.
- **`loc` 不是 C 字符串**. 它是扫描缓冲里的切片, 可能没有 NUL. 用 `len` 限界, 或复制出来.
- **`#line` 只影响行号, 不影响 `file->name`**. 诊断显示的文件名仍是物理路径, 行号可能已被 `#line` 偏移. 见第 11 章.
- **内存中的输入被复制**. `tokenize_buffer` 会把字节复制进 arena, 调用者的缓冲区可以立即释放. 代价是同一份源码在内存里有两份.
- **内存输入不经过扫描缓冲改写**. `chibicc_tokenize_buffer*` 按字节扫描: 不跳过 BOM, 不删除反斜杠续行, 不把 `\uXXXX` 改写成 UTF-8, 不做换行归一 (CRLF 仍能扫描, 孤立 CR 的行号会错), 不补末尾换行. 反斜杠续行会变成一个 `\` token, `\u00e9` 会变成 `\` 加 `u00e9`. 真实源码优先用 `chibicc_tokenize_file`; 内存输入应当是已经处于"扫描缓冲形态"的文本.
- **文件大小上限**. 超过 `INT32_MAX` 字节的文件被拒绝, 不是分块处理.
- **没有字符集转换**. 文件按字节读入, 标识符支持 UTF-8. 其他编码需要调用者先转成 UTF-8.
