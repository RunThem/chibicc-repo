// 第 3 层: 忠实的语法树 (plans/PLAN-faithful.md). 解析器保留每种书写形式
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

// AST 节点类型
struct libcp_node {
  LibcpNodeKind kind;  // LibcpNode 种类
  LibcpNode* next;     // 下一个节点
  LibcpType* ty;       // 类型, 例如 int 或指向 int 的指针
  LibcpToken* tok;     // 代表 token

  LibcpNode* lhs;  // 左侧
  LibcpNode* rhs;  // 右侧

  // ND_ASSIGN 的复合赋值运算符 ("+=", "-=", ...).
  // 0 表示普通的 "=".
  LibcpNodeKind op;

  // ND_INCDEC 的自增/自减: 后缀 (`i++`) 或前缀 (`++i`), 增量为 +1
  // 或 -1. 复合赋值重写由消费者的整形 pass 完成.
  bool is_post;
  int addend;

  // "if" 或 "for" 语句
  LibcpNode* cond;
  LibcpNode* then;
  LibcpNode* els;
  LibcpNode* init;
  LibcpNode* inc;

  // 块或语句表达式
  LibcpNode* body;

  // ND_BLOCK: 复合语句块为 true, 它是一个词法作用域; 空语句以及消费者
  // 整形 `for` 初始化声明链时形成的块是不带此标志的 ND_BLOCK 节点.
  // sema 的 resolve pass 根据该标志推导作用域栈.
  bool is_scope_block;

  // 结构体成员访问. 解析器不绑定它: 未解析的 ND_MEMBER 在 `tok` 中
  // 携带成员名, sema 的 add_type 负责查找该名字. `arrow_tok` 是指针
  // 访问的 `->` token, 普通 `.` 为 NULL; 它是操作数检查的锚点, 因此
  // 保留该 token. sema 绑定成员, 并把标记留在扁平化链的最内层连接上
  // (在消解的外层节点上清除); 求值器在常量表达式中读取它, codegen
  // 的 gen_addr 与 to_assign 把它读作 `->` 所代表的解引用
  // (PLAN B1.1b - 标记留在节点上, 不会被改写掉).
  LibcpMember* member;
  LibcpToken* arrow_tok;

  // 函数调用. func_ty 以及实参上的转换属于 sema (标注). 调用在 codegen
  // 侧的状态 - 按栈传递标记与调用者拥有的返回缓冲区 - 保存在 codegen
  // 私有的 per-node 表中 (PLAN L1).
  LibcpType* func_ty;
  LibcpNode* args;

  // goto 或带标签语句, 或标签值. `label` 是解析器记录的拼写名字,
  // goto/label 配对检查会比较它; codegen 为 goto, label 与 case 节点
  // 分配的 `.L..` 名字保存在其私有的 per-node 表中 (PLAN L1).
  char* label;

  // [GNU] `a ?: b` 条件表达式. sema 为其定型; gen_expr 直接用自己的
  // 字段发射, 不使用临时槽 - 测试值留在寄存器中 (PLAN B1.1e) - 求值器
  // 与 is_const_expr 通过该标志把它当作运行时值.
  bool is_elvis;

  // ND_SIZEOF/ND_ALIGNOF: `sizeof(type)` 形式的操作数类型.
  // `sizeof expr` 形式则把未求值的操作数放在 `lhs` 中. sema 把大小或
  // 对齐值记录到 `val`; VLA 操作数不是常量, codegen 的整形 pass 会为
  // 它构建运行时大小表达式 (A9.1/A7.1). ND_CAST (来自解析器),
  // ND_GENERIC_ASSOC, ND_TYPES_COMPATIBLE, ND_REG_CLASS, ND_DECL,
  // ND_GVAR_DECL, ND_FUNCDEF 与 ND_COMPOUND_LITERAL 也在这里存放
  // 它们声明的/目标的/操作数的类型.
  LibcpType* ty_op;

  // ND_TYPES_COMPATIBLE: 第二个类型操作数, 第一个是 ty_op. 类型谓词
  // 接受两个类型, 不接受子表达式.
  LibcpType* ty_op2;

  // ND_GENERIC: 控制表达式所选中的关联的结果表达式 - 选择的结论,
  // 记录在节点上, 消费者可以直接读取, 而不必让节点"变成"它.
  // 由 sema 的 select_generic 写入; 求值器, gen_expr 与 gen_addr
  // 读取它 (A9.1). 节点保持自己的种类.
  LibcpNode* generic_sel;

  // ND_CAST: sema 为执行隐式转换而插入的 cast 为 true, 源码书写的 cast
  // 为 false. 解析器从不设置它, new_cast 总是设置, 复制节点形状的重写点
  // 也会一并复制 - 因此源码到源码的工具可以区分这两类 cast.
  // codegen 不读取该标志.
  bool is_implicit;

  // 声明记录节点 (ND_DECL, ND_GVAR_DECL, ND_FUNCDEF): 解析器读到的
  // 存储类与对齐属性.
  LibcpVarAttr attr;

  // 声明记录节点 (ND_DECL, ND_GVAR_DECL, ND_FUNCDEF): 被声明的标识符.
  // declarator 也会把它暂存在类型的 `name` 上, 但类型是共享的
  // (基本类型单例, 标签类型与 typedef 类型), 该槽位会在下一个
  // declarator 处被覆盖, 远早于 sema 读取记录.
  LibcpToken* name_tok;

  // 在解析表达式上下文中的类型时变得可见的枚举常量声明记录
  // (例如 `sizeof(enum E { A })`). 由于该记录没有自己的语句链槽位,
  // sema 的 resolve pass 会在本节点的位置求值并注册它们.
  LibcpNode* spec_decls;

  // ND_DECL: 忠实的初始化器记录, 若 declarator 没有初始化器则为 NULL.
  // sema 把它解析成 `init_resolved`; 消费者在发射时展开为 MEMZERO +
  // 赋值逗号链.
  LibcpInitializer* decl_init;

  // ND_DECL, ND_GVAR_DECL 与 ND_COMPOUND_LITERAL: sema.c 私有的不透明
  // 载荷. resolve pass 把按声明类型解析后的初始化器记录暂存在这里,
  // 供标注 pass 使用; 在 sema 运行之外它可能为 NULL, 消费者不得读写它.
  // 公共树不解释它.
  void* init_resolved;

  // case 分支
  long begin;
  long end;
  bool is_default;  // `default:` 而不是 `case <expr>:`

  // ND_CASE: `case <expr>:` 以及 [GNU] 范围形式
  // `case <expr> ... <expr>:` 的操作数, 以未求值形式记录; sema 的
  // add_type 用它们填充 `begin`/`end`. `colon_tok` 是结束标签的 `:`,
  // 范围检查锚定在那里.
  LibcpNode* begin_expr;
  LibcpNode* end_expr;
  LibcpToken* colon_tok;

  // "asm" 字符串字面量
  char* asm_str;

  // 原子比较并交换
  LibcpNode* cas_addr;
  LibcpNode* cas_old;
  LibcpNode* cas_new;

  // 原子 op= 运算符
  LibcpObj* atomic_addr;
  LibcpNode* atomic_expr;

  // 变量
  LibcpObj* var;

  // 数值字面量
  int64_t val;
  long double fval;
};

// 解析器构建忠实的语法树, 返回顶层声明记录链; 除了 C 语法所需的
// typedef/tag 分类 oracle 之外, 它不做任何语义决策.
LIBCP_API LibcpNode* libcp_parse(LibcpContext* ctx, LibcpToken* tok);

// 子树锚点 token 的两端 (PLAN-l5 W5): 按 (文件, 偏移) 比较的最小与
// 最大代表 token. 子树不携带 token 时 *start 与 *end 为 NULL.
// `next` 链是兄弟节点, 不会跟进; 初始化器树不会被深入.
LIBCP_API void libcp_node_source_range(const LibcpNode* node, LibcpToken** start, LibcpToken** end);

#endif
