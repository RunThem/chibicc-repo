// 内部总头文件 (PLAN L3.1). 公共类型与声明位于 libcp/include/libcp/ 下;
// 实现包含的是本头文件. 公共 include 之下的一切都是库内部内容:
// context 布局, per-TU 分配, 辅助容器, 以及 facade 封装的阶段函数.
#define LIBCP_INTERNAL 1

#define _POSIX_C_SOURCE 200809L
#include "libcp/libcp.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <libgen.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// 库定义的每个类型都带有 Libcp 前缀 (2026-11-01); 实现直接使用这些名字,
// 不使用短别名.

#define MAX(x, y) ((x) < (y) ? (y) : (x))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#ifndef __GNUC__
#  define __attribute__(x)
#endif

// strings.c: 字符串, 数组与格式化工具

typedef struct {
  LibcpNode** data;
  int capacity;
  int len;
} LibcpNodeArray;

void libcp_strarray_push(LibcpContext* ctx, LibcpStringArray* arr, char* s);
void libcp_nodearray_push(LibcpContext* ctx, LibcpNodeArray* arr, LibcpNode* node);
char* libcp_format(LibcpContext* ctx, char* fmt, ...) __attribute__((format(printf, 2, 3)));

// tokenize.c: 词法分析, 诊断与 token 辅助

noreturn void libcp_error(LibcpContext* ctx, char* fmt, ...) __attribute__((format(printf, 2, 3)));
noreturn void libcp_error_at(LibcpContext* ctx, char* loc, char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
noreturn void libcp_error_tok(LibcpContext* ctx, LibcpToken* tok, char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
void libcp_warn_tok(LibcpContext* ctx, LibcpToken* tok, char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
// 公共 libcp_diagnostic_report 与上面的内部入口共用的报告核心: 只交付
// 诊断, 不 longjmp, 不改 context 状态.
void libcp_diagnostic_vreport(LibcpContext* ctx,
                              LibcpToken* tok,
                              LibcpDiagnosticSeverity severity,
                              const char* fmt,
                              va_list ap);
bool libcp_equal(LibcpToken* tok, char* op);
LibcpToken* libcp_skip(LibcpContext* ctx, LibcpToken* tok, char* op);
bool libcp_consume(LibcpToken** rest, LibcpToken* tok, char* str);
void libcp_convert_pp_tokens(LibcpContext* ctx, LibcpToken* tok);
void libcp_tokenize_string_literal(LibcpContext* ctx, LibcpToken* tok, LibcpType* basety);
LibcpToken* libcp_tokenize(LibcpContext* ctx, LibcpFile* file);
LibcpToken* libcp_tokenize_file_impl(LibcpContext* ctx, char* filename);

// source.c: 源坐标模块. LibcpFile 构造, 扫描缓冲构造, raw / 扫描偏移 /
// 物理行换算, file 级 synthetic 判定与 input file 表登记都由它独占.
// 公共的 input file 枚举与 libcp_token_is_synthetic 也在这里实现.

typedef struct {
  LibcpFile* file;
  long offset;
} LibcpSourcePos;

LibcpFile* libcp_new_file(LibcpContext* ctx, char* name, int file_no, char* contents);
LibcpFile* libcp_new_file_len(LibcpContext* ctx, char* name, int file_no, char* contents, int len);
LibcpFile* libcp_new_file_from_raw(LibcpContext* ctx, char* name, char* raw, int len);
void libcp_source_register_input_file(LibcpContext* ctx, LibcpFile* file);
bool libcp_is_line_break(char* p);
int libcp_line_break_len(char* p);
int libcp_file_raw_offset(const LibcpFile* file, const char* p);
int libcp_file_raw_end_of(const LibcpFile* file, int scan_off);
int libcp_file_physical_line(const LibcpFile* file, int raw_offset);
int libcp_token_raw_end(const LibcpToken* tok);
int libcp_token_physical_line(const LibcpToken* tok);
LibcpSourcePos libcp_token_source_pos(const LibcpToken* tok);
bool libcp_file_is_synthetic(const LibcpFile* file);

#define unreachable() libcp_error(ctx, "internal error at %s:%d", __FILE__, __LINE__)

// CST 通道入口 (libcp_cst_parse / _reconstruct / _dump / _verify),
// 流水线入口 (libcp_preprocess / libcp_parse / libcp_sema), 展开通道
// (libcp_expansion_dump / _verify) 以及 provenance 访问器直接使用其公共
// 名字, 并在公共头文件中声明 (PLAN-pratt-default S2: facade 包装已合并
// 进实现).
//

char* libcp_search_include_paths(LibcpContext* ctx, char* filename);
void libcp_init_macros(LibcpContext* ctx);
void libcp_define_macro(LibcpContext* ctx, char* name, char* buf);
void libcp_undef_macro(LibcpContext* ctx, char* name);

// parse.c: 解析器状态

// 忠实树节点的构造函数属于公共后端支持面 (<libcp/backend.h>):
// libcp_node_new / _binary / _unary / _num. 这里不重复声明.

// 标识符拼写, sema 在声明解析器声明记录所指称的对象时使用.
char* libcp_get_ident(LibcpContext* ctx, LibcpToken* tok);

void libcp_parse_init(LibcpContext* ctx);
void libcp_sema_store_label_gotos(LibcpContext* ctx, LibcpObj* fn, LibcpNodeArray gotos);
LibcpNodeArray* libcp_sema_label_gotos(LibcpContext* ctx, LibcpObj* fn);
void libcp_sema_state_clear(LibcpContext* ctx);

// sema.c: 常量求值与类型层工具

// 解析并求值常量表达式. 供预处理器处理 `#if` 时使用; 不属于下面的
// parse/sema 流水线.
int64_t libcp_const_expr(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok);

// 解析器的条件表达式入口 (parse.c), libcp_const_expr 用它读取要
// 求值的表达式.
LibcpNode* libcp_conditional(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok);

// 只有 sema 才会产生的形态的构造函数与类型工具属于公共后端支持面
// (<libcp/backend.h>): libcp_node_annotate_type, _arith, _cast, _long,
// _var, _vla_ptr, _alloca. 这里只声明库内部共用, 不对外承诺的辅助.
void libcp_usual_arith_conv(LibcpContext* ctx, LibcpNode** lhs, LibcpNode** rhs);
LibcpType* libcp_get_common_type(LibcpContext* ctx, LibcpType* ty1, LibcpType* ty2);

// 匿名名字计数器接下来会给出的值, 但不消耗它 (Phase 4 删除).
int libcp_unique_name_next(LibcpContext* ctx);

// type.c: 内建类型与类型构造

extern LibcpType* libcp_ty_void;
extern LibcpType* libcp_ty_bool;

extern LibcpType* libcp_ty_char;
extern LibcpType* libcp_ty_short;
extern LibcpType* libcp_ty_int;
extern LibcpType* libcp_ty_long;

extern LibcpType* libcp_ty_uchar;
extern LibcpType* libcp_ty_ushort;
extern LibcpType* libcp_ty_uint;
extern LibcpType* libcp_ty_ulong;

extern LibcpType* libcp_ty_float;
extern LibcpType* libcp_ty_double;
extern LibcpType* libcp_ty_ldouble;

bool libcp_is_integer(LibcpType* ty);
bool libcp_is_flonum(LibcpType* ty);
bool libcp_is_numeric(LibcpType* ty);
bool libcp_is_compatible(LibcpType* t1, LibcpType* t2);
LibcpType* libcp_copy_type(LibcpContext* ctx, LibcpType* ty);
LibcpType* libcp_func_type(LibcpContext* ctx, LibcpType* return_ty);
LibcpType* libcp_array_of(LibcpContext* ctx, LibcpType* base, int size);
LibcpType* libcp_vla_of(LibcpContext* ctx, LibcpType* base, LibcpNode* expr);
LibcpType* libcp_array_of_dim(LibcpContext* ctx, LibcpType* base, LibcpNode* dim);
LibcpType* libcp_typeof_placeholder(LibcpContext* ctx);
LibcpType* libcp_enum_type(LibcpContext* ctx);
LibcpType* libcp_struct_type(LibcpContext* ctx);

// strings.c: 对齐工具

int libcp_align_to(int n, int align);

// unicode.c: UTF-8 与显示宽度

int libcp_encode_utf8(char* buf, uint32_t c);
uint32_t libcp_decode_utf8(LibcpContext* ctx, char** new_pos, char* p);
bool libcp_is_ident1(uint32_t c);
bool libcp_is_ident2(uint32_t c);
int libcp_display_width(LibcpContext* ctx, char* p, int len);

// hashmap.c: 哈希表

typedef struct {
  char* key;
  int keylen;
  void* val;
} LibcpHashEntry;

typedef struct {
  LibcpHashEntry* buckets;
  int capacity;
  int used;      // 存活条目加上 tombstones
  int ndeleted;  // 已删除但仍计入 used 的条目
} LibcpHashMap;

void* libcp_hashmap_get(LibcpContext* ctx, LibcpHashMap* map, char* key);
void* libcp_hashmap_get2(LibcpContext* ctx, LibcpHashMap* map, char* key, int keylen);
void libcp_hashmap_put(LibcpContext* ctx, LibcpHashMap* map, char* key, void* val);
void libcp_hashmap_put2(LibcpContext* ctx, LibcpHashMap* map, char* key, int keylen, void* val);
void libcp_hashmap_delete(LibcpContext* ctx, LibcpHashMap* map, char* key);
void libcp_hashmap_delete2(LibcpContext* ctx, LibcpHashMap* map, char* key, int keylen);

// 共享辅助函数 (strings.c / tokenize.c)
//

bool libcp_file_exists(char* path);
void libcp_print_escaped(FILE* out, char* p, int len);
char* libcp_token_kind_name(LibcpContext* ctx, LibcpTokenKind kind);

// context / per-TU 分配 / 诊断 (PLAN L2)
//

typedef struct libcp_jmp_frame LibcpJmpFrame;
typedef struct libcp_tu_alloc LibcpTuAlloc;

// 错误边界: 每个公共入口压入一个, error_* 会 longjmp 到最内层 frame.
// 驱动器在 codegen 周围也会压入一个.
struct libcp_jmp_frame {
  LibcpJmpFrame* prev;
  jmp_buf buf;
};

// 公共入口的边界序言/尾声. 出错时 ENTER_RET 从所在函数返回 `fail`;
// ENTER_VOID 返回 void. frame 属于当前 context; 不保存或恢复任何其他
// 状态. lib.c 的 facade 入口以及直接使用公共名的阶段函数会使用它们
#define ENTER_RET(ctx, fail)                                                                       \
  LibcpJmpFrame _jf;                                                                               \
  _jf.prev       = (ctx)->jmp_top;                                                                 \
  (ctx)->jmp_top = &_jf;                                                                           \
  if (setjmp(_jf.buf)) {                                                                           \
    (ctx)->jmp_top = _jf.prev;                                                                     \
    return fail;                                                                                   \
  }

#define ENTER_VOID(ctx)                                                                            \
  LibcpJmpFrame _jf;                                                                               \
  _jf.prev       = (ctx)->jmp_top;                                                                 \
  (ctx)->jmp_top = &_jf;                                                                           \
  if (setjmp(_jf.buf)) {                                                                           \
    (ctx)->jmp_top = _jf.prev;                                                                     \
    return;                                                                                        \
  }

#define LEAVE(ctx)                                                                                 \
  do {                                                                                             \
    (ctx)->jmp_top = _jf.prev;                                                                     \
  } while (0)

// 每个 per-TU 对象是一次独立的 LIBCP_* 分配, 前面带一个只含链指针的
// 头部. reset / free 沿链逐块归还; 解析路径不做任何释放. 这不是
// 池化: 库不做块切分, 嵌入方提供的分配器 (包括 arena) 仍是唯一的
// 分配策略层.
struct libcp_tu_alloc {
  LibcpTuAlloc* next;
  LibcpTuAlloc* prev;
};

// sema 的检查 pass 收集的 goto / 标签值引用, 按其所属函数索引.
// 这份交接数据存放在 context 中, 而不是公共的 LibcpObj 上
// (PLAN L3.1b).
typedef struct libcp_label_goto_list LibcpLabelGotoList;
struct libcp_label_goto_list {
  LibcpLabelGotoList* next;
  LibcpObj* fn;
  LibcpNodeArray refs;
};

typedef struct libcp_cond_incl LibcpCondIncl;
typedef struct libcp_parse_scope LibcpParseScope;
typedef struct libcp_sema_scope LibcpSemaScope;

struct LibcpContext {
  // 本翻译单元所有分配组成的登记链. 链只保存 free 需要的地址.
  LibcpTuAlloc* tu_allocs;

  // 配置. 跨 reset 保留, 由 LIBCP_* 拥有, 由 libcp_context_free 释放.
  char** include_paths;
  int include_paths_len;
  int include_paths_cap;
  // 按顺序记录的命令行宏操作: "D:name=body" 或 "U:name".
  char** macro_ops;
  int macro_ops_len;
  int macro_ops_cap;
  // 消费者为 CST oracle 预置的 typedef 名 (PLAN-l5 W2): 这些名字来自
  // 被包含的头文件, 预处理前的 typedef 扫描看不到它们. 与其他配置一样
  // 跨 reset 保留.
  char** cst_seeds;
  int cst_seeds_len;
  int cst_seeds_cap;
  bool predefined_macros;
  LibcpDiagnosticFn diag_fn;
  void* diag_user;
  LibcpJmpFrame* jmp_top;
  bool failed;

  // per-TU 状态 (PLAN L2.2-L2.5). 它们以前是文件作用域的 static 变量.
  char* base_file;
  LibcpFile* current_file;
  LibcpFile** input_files;
  int input_files_len;
  int input_files_cap;
  int next_file_no;

  // 第一次读取 "-" 时的文本, 使第二次 tokenize stdin 时看到相同的输入,
  // 而不是粘住的 EOF 带来的空流 (C12).
  char* stdin_text;
  size_t stdin_len;
  bool at_bol;
  bool has_space;

  LibcpHashMap macros;
  LibcpPredefinedMacro* predefined;
  int predefined_len;
  LibcpCondIncl* cond_incl;
  LibcpHashMap pragma_once;
  LibcpHashMap search_cache;
  LibcpHashMap include_guards;
  LibcpHashMap keyword_map;
  LibcpHashMap typename_map;
  int counter;
  int include_next_idx;
  int include_count;
  int parse_depth;

  LibcpParseScope* scope;
  LibcpSemaScope* sema_scope;

  LibcpObj* locals;
  LibcpObj* globals;
  LibcpObj* sema_fn;
  LibcpObj* builtin_alloca;
  LibcpNodeArray gotos;
  LibcpNodeArray labels;
  LibcpLabelGotoList* label_gotos;
  int loop_depth;
  int switch_depth;
  int unique_name_id;
  bool resolving_body;
};

void* libcp_tu_alloc(LibcpContext* ctx, size_t size);
void* libcp_tu_calloc(LibcpContext* ctx, size_t count, size_t size);
char* libcp_tu_strdup(LibcpContext* ctx, char* s);
char* libcp_tu_strndup(LibcpContext* ctx, char* s, size_t n);
void* libcp_tu_realloc(LibcpContext* ctx, void* old, size_t old_size, size_t new_size);
