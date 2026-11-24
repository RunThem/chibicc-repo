// 第 3 层: 忠实的语法树. 解析器保留每种书写形式
// 的差异, 不做语义决策; 第 4 层 (sema.h) 在其上绑定名字, 计算类型,
// 并记录语义结论.

#ifndef LIBCP_AST_H
#define LIBCP_AST_H

#include <libcp/token.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct libcp_type LibcpType;
typedef struct libcp_node LibcpNode;
typedef struct libcp_obj LibcpObj;
typedef struct libcp_member LibcpMember;
typedef struct libcp_relocation LibcpRelocation;
typedef struct libcp_initializer LibcpInitializer;
typedef struct libcp_init_item LibcpInitItem;
typedef struct libcp_init_desig LibcpInitDesig;
typedef struct libcp_init_tree LibcpInitTree;
typedef struct libcp_init_path LibcpInitPath;

// 可增长的字符串数组. 库内部使用它; 它属于公共类型集合, 因为
// LibcpObj.refs 暴露了其中一个.
typedef struct libcp_string_array {
  char** data;
  int capacity;
  int len;
} LibcpStringArray;

// 变量或函数
struct libcp_obj {
  LibcpObj* next;
  char* name;       // 变量名
  LibcpType* ty;    // 类型
  LibcpToken* tok;  // 代表 token
  bool is_local;    // 局部, 或全局/函数
  int align;        // 对齐

  // 全局变量或函数
  bool is_function;
  bool is_definition;
  bool is_static;

  // 全局变量
  bool is_tentative;
  bool is_tls;
  char* init_data;
  LibcpRelocation* rel;

  // 函数
  bool is_inline;
  LibcpObj* params;
  LibcpNode* body;
  LibcpObj* locals;
  LibcpObj* va_area;
  LibcpObj* alloca_bottom;

  // static inline 函数
  bool is_live;
  bool is_root;
  LibcpStringArray refs;
};

// 全局变量既可以用常量表达式初始化, 也可以用指向另一个全局变量的
// 指针初始化. 本结构体表示后一种情况.
struct libcp_relocation {
  LibcpRelocation* next;
  int offset;

  // 地址常量的目标. 具名符号 (全局变量, 函数, 字符串字面量的匿名全局
  // 对象, 文件作用域复合字面量的对象) 在序列化时已知, 存放在 `label` 中.
  // [GNU] `&&label` 引用此时还没有名字: codegen 在整形阶段分配 `.L` 名,
  // 因此节点保存在这里, 并在发射时解析.
  char* label;
  LibcpNode* label_node;

  long addend;
};

// declspec 收集到的声明属性: 存储类与 _Alignas. 解析器填充它, 并复制到
// 声明记录节点上; sema 在声明对象时使用它.
typedef struct libcp_var_attr {
  bool is_typedef;
  bool is_static;
  bool is_extern;
  bool is_inline;
  bool is_tls;
  int align;
  // `_Alignas(<expr>)` 的参数, 其中 `align_ty` 对应 `_Alignas(<type>)`
  // 形式. 以最后出现的那个为准; sema 从完整类型上读取类型形式对齐值.
  LibcpNode* align_expr;
  LibcpType* align_ty;
} LibcpVarAttr;

// 忠实的初始化器记录 (parse.c): 初始化器按书写形式保存的语法形态 -
// 花括号元素序列, 字符串字面量或单个表达式, designator 以未求值形式
// 记录. sema 在给承载节点 (ND_DECL, 复合字面量) 定型时, 或在解析
// 全局变量定义时, 解析 designator, 应用花括号省略, 并降级该记录.
enum {
  INIT_LIST,
  INIT_EXPR,
  INIT_STR
};

struct libcp_initializer {
  int kind;              // INIT_LIST, INIT_EXPR 或 INIT_STR
  LibcpToken* tok;       // 初始化器源码的首 token
  LibcpInitItem* items;  // INIT_LIST: 花括号元素序列
  LibcpNode* expr;       // INIT_EXPR: 标量初始化表达式
  LibcpToken* str_tok;   // INIT_STR: 字符串字面量 token

  // 仅用于 ND_DECL 的顶层记录: 引入它的 `=`, sema 在这里锚定
  // 拒绝整个声明的诊断.
  LibcpToken* eq_tok;
};

// 花括号初始化列表中的一个元素.
struct libcp_init_item {
  LibcpInitItem* next;
  LibcpToken* comma_tok;   // 本元素前的 `,` (第一个为 NULL)
  LibcpInitDesig* desigs;  // 按书写形式记录的 designator 链 (可为 NULL)
  LibcpInitializer* init;  // 元素值
};

// 按书写形式记录的一个 designator: `[expr]`, `[begin ... end]` 或
// `.name`. 索引表达式以未求值形式记录; sema 负责求值.
struct libcp_init_desig {
  LibcpInitDesig* next;
  LibcpToken* tok;          // `[` 或 `.`
  LibcpToken* after_begin;  // begin 表达式之后的 token (`...` 或 `]`)
  LibcpToken* rbracket;     // 右方括号 token
  LibcpNode* begin;         // 数组 designator 索引
  LibcpNode* end;           // 范围终点 (非 `[begin ... end]` 时为 NULL)
  LibcpToken* name;         // 成员 designator 名
};

// 解析后的初始化器树 (PLAN A8.1): 忠实的初始化器记录在 designator 求值,
// 成员名绑定, 花括号省略应用, 柔性数组定长之后变成的形态. 由 sema 的
// resolve pass 构建; 消费者的声明展开遍历它来构建赋值链, sema 的
// 文件作用域序列化器遍历它生成 .data 字节. 初始化器可以嵌套
// (例如 `int x[2][2] = {{1, 2}, {3, 4}}`), 因此它是一棵树.
struct libcp_init_tree {
  LibcpType* ty;
  bool is_flexible;

  // 若不是聚合类型且有初始化器, `expr` 存放初始化表达式.
  LibcpNode* expr;

  // 若是聚合类型 (例如数组或结构体) 的初始化器, `children` 存放其
  // 子对象的初始化器.
  LibcpInitTree** children;

  // 联合体只能初始化一个成员. `mem` 用于说明初始化的是哪个成员.
  LibcpMember* mem;
};

// 描述局部变量初始化器中元素位置的 designator 链
// (例如 `x[1].y[2]`), 消费者展开时遍历它来构建每次赋值的目标表达式.
struct libcp_init_path {
  LibcpInitPath* next;
  int idx;
  LibcpMember* member;
  LibcpObj* var;
};

typedef enum {
  TY_VOID,
  TY_BOOL,
  TY_CHAR,
  TY_SHORT,
  TY_INT,
  TY_LONG,
  TY_FLOAT,
  TY_DOUBLE,
  TY_LDOUBLE,
  TY_ENUM,
  TY_PTR,
  TY_FUNC,
  TY_ARRAY,
  TY_VLA,     // 变长数组
  TY_TYPEOF,  // [GNU] `typeof(expr)`, sema 尚未标注其操作数类型
  TY_STRUCT,
  TY_UNION,
} LibcpTypeKind;

struct libcp_type {
  LibcpTypeKind kind;
  int size;           // sizeof() 值
  int align;          // 对齐
  bool is_unsigned;   // 无符号或有符号
  bool is_atomic;     // 若为 _Atomic 则为 true
  LibcpType* origin;  // 用于类型兼容性检查

  // 指针的目标类型或数组的元素类型. 有意用同一个成员表示 C 中
  // 指针/数组的二重性.
  //
  // 在许多期望指针的上下文中, 检查这个成员而不是 "kind" 成员, 以判断
  // 一个类型是否为指针. 这意味着在许多上下文中, "T 的数组" 会自然地按
  // "指向 T 的指针" 处理, 符合 C 规范的要求.
  LibcpType* base;

  // 声明信息. declarator 名字从不写入共享的类型记录 (F1); 这些字段只在
  // 函数类型内部逐参数复制的那份类型上填充, sema 在那里读取它们.
  LibcpToken* name;
  LibcpToken* name_pos;

  // struct/union/enum specifier 的标签标识符, 由解析器在向前声明与定义处
  // 记录, 并被每个解析到该记录的使用点共享; 匿名 specifier 携带 NULL.
  // 标签拼写否则只存在于解析器的作用域中, 因此这是消费者恢复它的途径
  // (PLAN-c2c D2).
  LibcpToken* tag_tok;

  // 数组
  int array_len;

  // 记录解析器 declarator 层留给 sema 完成的内容 (见 resolve_type):
  // 尚未求值的数组维度表达式, 因此长度以及定长/变长问题都不在解析器中
  // 回答; 尚未标注类型的 `typeof(expr)` 操作数; 以及 `aligned` 属性的
  // 参数. 每一项在被消费后清空.
  LibcpNode* dim_len;
  LibcpNode* typeof_expr;
  LibcpNode* align_expr;

  // 变长数组. codegen 为它创建的运行时大小槽 (A7.1) 保存在 codegen
  // 私有的 per-type 表中 (PLAN L1).
  LibcpNode* vla_len;  // 元素个数

  // 结构体
  LibcpMember* members;
  bool is_flexible;
  bool is_packed;
  // 成员链表已完整, 但 sema 尚未放置成员; resolve_type 在第一次看到时
  // 布局该类型 (不完整的向前声明类型不会携带此标志).
  bool layout_pending;

  // 函数类型
  LibcpType* return_ty;
  LibcpType* params;
  bool is_variadic;
  // 参数的 declspec 产生的枚举常量声明记录 (例如 `int f(enum E { A } x)`);
  // sema 的 resolve pass 在完成该函数类型时注册它们.
  LibcpNode* spec_decls;
  LibcpType* next;
};

// 结构体成员
struct libcp_member {
  LibcpMember* next;
  LibcpType* ty;
  LibcpToken* tok;  // 用于错误信息
  LibcpToken* name;
  int idx;
  int align;
  int offset;

  // 成员 declspec 上的 `_Alignas`: `_Alignas(<type>)` 形式记录在
  // `align_ty` 中, `_Alignas(<expr>)` 形式记录在 `align_expr` 中;
  // sema 在布局聚合体时确定该值. 零表示没有 `_Alignas`, 此时按类型
  // 自身的对齐.
  LibcpType* align_ty;
  LibcpNode* align_expr;

  // 位域
  bool is_bitfield;
  int bit_offset;
  int bit_width;
  // 位域记录的宽度表达式, 未求值; sema 在布局聚合体时求值并写入
  LibcpNode* width_expr;
};

// AST 节点
typedef enum {
  ND_NULL_EXPR,         // 什么都不做
  ND_ADD,               // +
  ND_SUB,               // -
  ND_MUL,               // *
  ND_DIV,               // /
  ND_NEG,               // 一元 -
  ND_MOD,               // %
  ND_BITAND,            // &
  ND_BITOR,             // |
  ND_BITXOR,            // ^
  ND_SHL,               // <<
  ND_SHR,               // >>
  ND_EQ,                // ==
  ND_NE,                // !=
  ND_LT,                // <
  ND_LE,                // <=
  ND_GT,                // >
  ND_GE,                // >=
  ND_ASSIGN,            // =
  ND_INCDEC,            // "++" 与 "--"; 复合赋值由消费者的整形 pass 重写
  ND_SUBSCRIPT,         // x[y]
  ND_COND,              // ?:
  ND_COMMA,             // ,
  ND_MEMBER,            // . (结构体成员访问)
  ND_ADDR,              // 一元 &
  ND_DEREF,             // 一元 *
  ND_NOT,               // !
  ND_BITNOT,            // ~
  ND_LOGAND,            // &&
  ND_LOGOR,             // ||
  ND_RETURN,            // "return"
  ND_IF,                // "if"
  ND_FOR,               // "for"
  ND_WHILE,             // "while"
  ND_DO,                // "do"
  ND_SWITCH,            // "switch"
  ND_CASE,              // "case"
  ND_BLOCK,             // { ... }
  ND_GOTO,              // "goto"
  ND_BREAK,             // "break"; 发射时解析到最内层循环或 switch 的 break 标签
  ND_CONTINUE,          // "continue"; 发射时解析到最内层循环的 continue 标签
  ND_GOTO_EXPR,         // "goto" 标签值
  ND_LABEL,             // 带标签语句
  ND_LABEL_VAL,         // [GNU] 标签值
  ND_FUNCALL,           // 函数调用
  ND_EXPR_STMT,         // 表达式语句
  ND_STMT_EXPR,         // 语句表达式
  ND_IDENT,             // 未解析的名字; sema 将其绑定到变量, 函数或枚举常量
  ND_VAR,               // 变量
  ND_VLA_PTR,           // 变长数组指示符
  ND_NUM,               // 整数
  ND_STRING,            // 字符串字面量; sema 将其降级为匿名全局对象
  ND_SIZEOF,            // "sizeof"; sema 将其折叠为值
  ND_ALIGNOF,           // "_Alignof"; sema 将其折叠为值
  ND_GENERIC,           // "_Generic"; sema 选择一个关联, 结果表达式记录在 generic_sel 中
  ND_GENERIC_ASSOC,     // ND_GENERIC 的一个关联: 类型名在 ty_op 中 ("default:"
                        // 为 NULL), 结果表达式在 lhs 中. 仅作载体; 从不进入 codegen.
  ND_TYPES_COMPATIBLE,  // "__builtin_types_compatible_p"; sema 将其折叠为 0 或 1
  ND_REG_CLASS,         // "__builtin_reg_class"; sema 将其折叠为对应的类别
  ND_CAST,              // 类型转换. 解析器让显式 cast 保持未定型, 目标类型放在
                        // ty_op 中; sema 解析并标注该节点. sema 插入的 cast 在
                        // 创建时即完成定型, 并标记 is_implicit.
  ND_MEMZERO,           // 将栈变量清零
  ND_DECL,              // 局部变量声明: 名字 token 在 `name_tok` 中, 类型在
                        // `ty_op` 中, 属性在 `attr` 中, 忠实的初始化器记录在
                        // `decl_init` 中. sema 在 resolve pass 声明该变量, 在
                        // 标注 pass 标注初始化器表达式; 消费者的发射把它展开为
                        // 0..n 条语句.
  ND_GVAR_DECL,         // 全局变量的声明记录 (文件作用域, 或块内的 `extern`/
                        // tentative 声明). sema 声明该变量并序列化其初始化器;
                        // 记录留在语句链中, 消费者的整形 pass 在发射时移除它.
  ND_FUNCDEF,           // 函数的声明记录; 定义时 `body` 非 NULL. sema 声明该函数,
                        // 对其函数体运行两遍处理; 记录留在语句链中, 消费者的
                        // 整形 pass 在发射时移除它 (块作用域原型或 [GNU] 嵌套定义).
  ND_TYPEDEF,           // typedef 声明记录; sema 解析其类型; 记录留在语句链中,
                        // 消费者的整形 pass 在发射时移除它.
  ND_ENUM_CONST,        // 枚举常量声明 (成员名 + 可选值表达式). sema 求值并注册
                        // 该名字; 记录留在语句链中, 消费者的整形 pass 在发射时
                        // 移除它.
  ND_COMPOUND_LITERAL,  // "(type){...}": 类型在 ty_op 中, 忠实的初始化器记录在
                        // decl_init 中. sema 物化隐藏变量, 文件作用域的数据由
                        // sema 序列化; 块作用域的展开由消费者的整形 pass 完成.
  ND_ASM,               // "asm"
  ND_CAS,               // 原子比较并交换
  ND_EXCH,              // 原子交换
} LibcpNodeKind;

// LibcpNode 的字段族. 每个 kind 只使用一个族, 公共头字段除外.
// 通用 walker 用族决定下降哪些子节点; 它也是字段族 union 的分组依据.
typedef enum {
  LIBCP_NF_NONE = 0,  // 只使用公共头
  LIBCP_NF_CTRL,      // 控制流, 调用与 _Generic
  LIBCP_NF_DECL,      // 复合语句与声明记录
  LIBCP_NF_VALUE,     // 字面量, 类型查询与 cast
  LIBCP_NF_CASE,      // case 标签
  LIBCP_NF_ACCESS,    // 成员访问
  LIBCP_NF_JUMP,      // goto 与标签
  LIBCP_NF_ASSIGN,    // 复合赋值运算符
  LIBCP_NF_INCDEC,    // 自增/自减
  LIBCP_NF_ASM,       // asm
  LIBCP_NF_CAS,       // 原子 CAS
} LibcpNodeFamily;

// 返回 kind 所属的字段族. 未列出的 kind 返回 LIBCP_NF_NONE.
LIBCP_API LibcpNodeFamily libcp_node_family(LibcpNodeKind kind);

// 控制流, 函数调用与 _Generic 的载荷. 条件, 分支, 循环头与实参共享
// 一组槽位; _Generic 用 cond 保存控制表达式, args 保存关联链.
typedef struct {
  LibcpNode* cond;         // if/while/do/for/switch 以及 ?: 的条件
  LibcpNode* then;         // 真分支或循环体
  LibcpNode* els;          // else 分支, 没有则为 NULL
  LibcpNode* init;         // for 的初始化子句
  LibcpNode* inc;          // for 的递增子句
  LibcpNode* args;         // 调用实参或 _Generic 的关联链
  LibcpType* func_ty;      // ND_FUNCALL 已解析的函数类型
  LibcpNode* generic_sel;  // ND_GENERIC 选中的关联结果
  bool is_elvis;           // [GNU] a ?: b
} LibcpNodeCtrl;

// 复合语句与声明记录的载荷. 函数定义同时携带函数体与声明属性, 因此
// body 与声明字段同族; 块与语句表达式只使用 body 与 is_scope_block.
typedef struct {
  LibcpNode* body;              // 复合语句块, 语句表达式或函数体
  bool is_scope_block;          // ND_BLOCK 是否引入词法作用域
  LibcpVarAttr attr;            // 存储类与 _Alignas
  LibcpToken* name_tok;         // 被声明的标识符
  LibcpInitializer* decl_init;  // 忠实的初始化器记录
  void* init_resolved;          // sema 私有: 解析后的初始化器树
  LibcpObj* var;                // sema 绑定的变量/函数对象
} LibcpNodeDecl;

// 字面量, 类型查询与 cast 的载荷.
typedef struct {
  int64_t val;        // 整数字面量或折叠后的 sizeof/_Alignof 值
  long double fval;   // 浮点字面量
  LibcpType* ty_op2;  // ND_TYPES_COMPATIBLE 的第二个类型操作数
  bool is_implicit;   // ND_CAST 是否由 sema 插入
} LibcpNodeValue;

// case 标签的载荷.
typedef struct {
  long begin;             // case 值, 或范围起点
  long end;               // 范围终点, 普通 case 与 begin 相同
  bool is_default;        // default: 而不是 case <expr>:
  LibcpNode* begin_expr;  // 未求值的起点表达式
  LibcpNode* end_expr;    // 未求值的范围终点
  LibcpToken* colon_tok;  // 结束标签的 ':'
} LibcpNodeCase;

// 结构体成员访问.
typedef struct {
  LibcpMember* member;
  LibcpToken* arrow_tok;  // `->`, 普通 `.` 为 NULL
} LibcpNodeAccess;

// goto 与标签.
typedef struct {
  char* label;
} LibcpNodeJump;

// 复合赋值的运算符, 0 表示普通的 "=".
typedef struct {
  LibcpNodeKind op;
} LibcpNodeAssign;

// 自增/自减.
typedef struct {
  bool is_post;  // 后缀 i++ 为 true, 前缀 ++i 为 false
  int addend;    // +1 或 -1
} LibcpNodeIncDec;

// asm 字符串字面量.
typedef struct {
  char* asm_str;
} LibcpNodeAsm;

// 原子比较并交换.
typedef struct {
  LibcpNode* cas_addr;
  LibcpNode* cas_old;
  LibcpNode* cas_new;
} LibcpNodeCas;

// AST 节点类型. 字段按节点族放进匿名 union: 每个 kind 只使用一个族,
// 访问形式为 node->ctrl.cond / node->decl.attr. 公共头字段被所有族共享.
struct libcp_node {
  LibcpNodeKind kind;  // LibcpNode 种类
  LibcpNode* next;     // 下一个节点
  LibcpType* ty;       // 类型, 例如 int 或指向 int 的指针
  LibcpToken* tok;     // 代表 token

  LibcpNode* lhs;  // 左侧
  LibcpNode* rhs;  // 右侧

  // ND_SIZEOF/ND_ALIGNOF 的目标类型, ND_CAST 的目标类型, 以及声明记录
  // 声明的类型. 详见上方各族的注释.
  LibcpType* ty_op;

  // 在解析表达式上下文中的类型时变得可见的枚举常量声明记录
  // (例如 `sizeof(enum E { A })`). sema 的 resolve pass 会在本节点的
  // 位置求值并注册它们.
  LibcpNode* spec_decls;

  union {
    LibcpNodeCtrl ctrl;
    LibcpNodeDecl decl;
    LibcpNodeValue value;
    LibcpNodeCase case_;
    LibcpNodeAccess access;
    LibcpNodeJump jump;
    LibcpNodeAssign assign;
    LibcpNodeIncDec incdec;
    LibcpNodeAsm asm_;
    LibcpNodeCas cas;
  };
};

// 解析器构建忠实的语法树, 返回顶层声明记录链; 除了 C 语法所需的
// typedef/tag 分类 oracle 之外, 它不做任何语义决策.
LIBCP_API LibcpNode* libcp_parse(LibcpContext* ctx, LibcpToken* tok);

// 子树锚点 token 的两端 (PLAN-l5 W5): 按 (文件, 偏移) 比较的最小与
// 最大代表 token. 子树不携带 token 时 *start 与 *end 为 NULL.
// `next` 链是兄弟节点, 不会跟进; 初始化器树不会被深入.
LIBCP_API void libcp_node_source_range(const LibcpNode* node, LibcpToken** start, LibcpToken** end);

#endif
