// Layer 1: the token stream (PLAN L3.1b). Tokens keep their trivia in raw
// file coordinates, so the stream rebuilds the input file byte for byte.
//
// 第 1 层: token 流 (PLAN L3.1b). token 在原始文件坐标中保留 trivia,
// 因此该流可以逐字节重建输入文件.

#ifndef LIBCP_TOKEN_H
#define LIBCP_TOKEN_H

#include <libcp/context.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct libcp_type LibcpType;

// Private to preprocess.c / tokenize.c; a token only carries pointers.
// preprocess.c / tokenize.c 私有; token 只携带指针.
typedef struct libcp_hideset LibcpHideset;
typedef struct libcp_macro_prov LibcpMacroProv;

// LibcpToken
// LibcpToken 种类
typedef enum {
  TK_IDENT,    // Identifiers
               // 标识符
  TK_PUNCT,    // Punctuators
               // 标点
  TK_KEYWORD,  // Keywords
               // 关键字
  TK_STR,      // String literals
               // 字符串字面量
  TK_NUM,      // Numeric literals
               // 数值字面量
  TK_PP_NUM,   // Preprocessing numbers
               // 预处理数字
  TK_EOF,      // End-of-file markers
               // 文件结束标记
} LibcpTokenKind;

typedef struct libcp_file {
  char* name;
  int file_no;

  // 0 for a top-level file or a buffer; one more than the including
  // file for a file opened by #include.
  //
  // 顶层文件或缓冲区为 0; 由 #include 打开的文件比包含它的文件大 1.
  int include_depth;

  // `raw` is the text as read. `contents` is the buffer the lexer scans:
  // a derivative of `raw` with line breaks canonicalized to '\n', a
  // backslash before a line break removed together with it, universal
  // character names rewritten, a leading BOM skipped and a final newline
  // guaranteed (the rewrites of the T1-era pre-passes, kept so that every
  // consumer sees the text it always saw). `map` translates a scan offset
  // to an offset into `raw`, `lines` gives the line number of a raw
  // offset; both have one entry per byte plus a final one. For synthetic
  // buffers (paste, string tokens) the two texts coincide: `map` is the
  // identity and `lines` counts the newlines.
  //
  // `raw` 是按原样读入的文本. `contents` 是词法分析器扫描的缓冲区:
  // 它是 `raw` 的派生文本, 其中换行被规范化为 '\n', 换行前的反斜杠
  // 连同换行一起删除, universal character name 被改写, 开头的 BOM 被
  // 跳过, 并保证末尾有换行 (这些是 T1 时期预处理的改写, 保留下来是为了
  // 让每个消费者看到的文本与以前一致). `map` 把扫描偏移转换为 `raw`
  // 中的偏移, `lines` 给出 `raw` 偏移对应的行号; 两者都是每字节一个
  // 条目, 末尾再加一个. 对于合成缓冲区 (拼接, 字符串 token), 两份文本
  // 相同: `map` 是恒等映射, `lines` 统计换行数.
  char* contents;
  char* raw;
  int* map;
  int* lines;

  // For #line directive
  // 用于 #line 指令
  char* display_name;
  int line_delta;
} LibcpFile;

// LibcpToken type
typedef struct libcp_token LibcpToken;
struct libcp_token {
  LibcpTokenKind kind;  // LibcpToken kind
                        // LibcpToken 种类
  LibcpToken* next;     // Next token
                        // 下一个 token
  int64_t val;          // If kind is TK_NUM, its value
                        // 若 kind 为 TK_NUM, 则为其值
  long double fval;     // If kind is TK_NUM, its value
                        // 若 kind 为 TK_NUM, 则为其值
  char* loc;            // LibcpToken location (in the file's scan buffer)
                        // token 位置 (位于文件的扫描缓冲区)
  int len;              // LibcpToken length
                        // token 长度

  // The bytes between the previous token and this one that the lexer
  // skipped instead of tokenizing (whitespace, newlines, comments,
  // backslash-continuations) and the token's own spelling, both in raw
  // file coordinates: the trivia of the tokens plus their spellings cover
  // the raw text byte for byte, so the stream rebuilds the file (the
  // leading trivia of the first token includes the BOM, if any; the
  // final newline read_file synthesizes is not part of the file and does
  // not appear here). Tokens created outside tokenize()'s skip loop
  // (paste, tokenize_string_literal) carry the coordinates of the token
  // they are derived from.
  //
  // 这两个字段存放前一个 token 与本 token 之间被词法分析器跳过,
  // 未做 tokenize 的字节 (空白, 换行, 注释, 反斜杠续行) 以及 token
  // 自身的拼写, 都使用原始文件坐标: 各 token 的 trivia 加上它们的拼写
  // 逐字节覆盖原始文本, 因此该流可以重建文件 (第一个 token 的前导
  // trivia 包含 BOM, 如果有; read_file 合成的末尾换行不属于文件,
  // 不会出现在这里). 在 tokenize() 的跳过循环之外创建的 token
  // (拼接, tokenize_string_literal) 携带其来源 token 的坐标.
  char* trivia;    // Start of the skipped bytes before this token
                   // 本 token 之前被跳过字节的起点
  int trivia_len;  // Number of skipped bytes before this token
                   // 本 token 之前被跳过的字节数
  char* raw_loc;   // Start of the token's spelling in the raw text
                   // token 拼写在原始文本中的起点
  int raw_len;     // Length of the token's spelling in the raw text
                   // token 拼写在原始文本中的长度

  LibcpType* ty;  // Used if TK_NUM or TK_STR
                  // 若为 TK_NUM 或 TK_STR 时使用
  char* str;      // String literal contents including terminating '\0'
                  // 字符串字面量内容, 含结尾 '\0'

  LibcpFile* file;        // Source location
                          // 源码位置
  char* filename;         // Filename
                          // 文件名
  int line_no;            // Line number
                          // 行号
  int line_delta;         // Line number
                          // 行号
  bool at_bol;            // True if this token is at beginning of line
                          // 若本 token 位于行首则为 true
  bool has_space;         // True if this token follows a space character
                          // 若本 token 前有空格则为 true
  LibcpHideset* hideset;  // For macro expansion
                          // 用于宏展开
  LibcpToken* origin;     // If this is expanded from a macro, the original token
                          // 若由宏展开而来, 则为原始 token
  LibcpMacroProv* prov;   // How this token came to be: the chain of macro
                          // expansions that produced it, innermost layer
                          // first, or NULL for a token read from the source
                          // (preprocess.c owns the chain; the accessors below
                          // are the public face)
  // 本 token 的来源: 产生它的宏展开链, 最内层在前;
  // 从源码读入的 token 为 NULL (链由 preprocess.c 拥有;
  // 下面的访问器是公共接口)
};

LIBCP_API LibcpToken* libcp_tokenize_file(LibcpContext* ctx, char* path);
LIBCP_API LibcpToken* libcp_tokenize_buffer(LibcpContext* ctx, char* name, char* contents);

// Like libcp_tokenize_buffer, but reads exactly `len` bytes. An
// embedded NUL is a diagnostic rather than a silent truncation.
//
// 与 libcp_tokenize_buffer 类似, 但精确读取 `len` 个字节. 内嵌的 NUL
// 会产生诊断, 而不是被静默截断.
LIBCP_API LibcpToken*
    libcp_tokenize_buffer_len(LibcpContext* ctx, char* name, char* contents, int len);

// Print the bytes the token stream was tokenized from: the trivia in
// front of every token plus the token spellings, in raw coordinates (the
// trailing trivia rides on EOF). On tokenize()'s direct output the two
// cover the file's raw text byte for byte, so this rebuilds the input
// text - with no exceptions: the rewrites that used to be pre-passes
// live in the scan buffer and the map now, not in the coordinates.
//
// 打印 token 流进行 tokenize 时所依据的字节: 每个 token 前面的 trivia
// 加上 token 拼写, 使用原始坐标 (末尾的 trivia 挂在 EOF 上). 对于
// tokenize() 的直接输出, 两者逐字节覆盖文件的原始文本, 因此这里可以
// 重建输入文本 - 没有例外: 过去作为预处理步骤的改写现在位于扫描缓冲区
// 与 map 中, 而不在坐标里.
LIBCP_API void libcp_reconstruct(LibcpContext* ctx, LibcpToken* tok, FILE* out);

#endif
