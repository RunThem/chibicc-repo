// Layer 3: the faithful syntax tree (plans/PLAN-faithful.md). The parser keeps
// every written form distinct and makes no semantic decision; layer 4
// (sema.h) binds names, computes types and records its conclusions on
// these nodes.
//
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

// A growable string array. The library uses it internally; it is part of
// the public type set because LibcpObj.refs exposes one.
//
// 可增长的字符串数组. 库内部使用它; 它属于公共类型集合, 因为
// LibcpObj.refs 暴露了其中一个.
typedef struct libcp_string_array {
  char** data;
  int capacity;
  int len;
} LibcpStringArray;

// Variable or function
// 变量或函数
struct libcp_obj {
  LibcpObj* next;
  char* name;       // Variable name
                    // 变量名
  LibcpType* ty;    // LibcpType
                    // 类型
  LibcpToken* tok;  // representative token
                    // 代表 token
  bool is_local;    // local or global/function
                    // 局部, 或全局/函数
  int align;        // alignment
                    // 对齐

  // Global variable or function
  // 全局变量或函数
  bool is_function;
  bool is_definition;
  bool is_static;

  // Global variable
  // 全局变量
  bool is_tentative;
  bool is_tls;
  char* init_data;
  LibcpRelocation* rel;

  // Function
  // 函数
  bool is_inline;
  LibcpObj* params;
  LibcpNode* body;
  LibcpObj* locals;
  LibcpObj* va_area;
  LibcpObj* alloca_bottom;

  // Static inline function
  // static inline 函数
  bool is_live;
  bool is_root;
  LibcpStringArray refs;
};

// Global variable can be initialized either by a constant expression
// or a pointer to another global variable. This struct represents the
// latter.
//
// 全局变量既可以用常量表达式初始化, 也可以用指向另一个全局变量的
// 指针初始化. 本结构体表示后一种情况.
struct libcp_relocation {
  LibcpRelocation* next;
  int offset;

  // The target of an address constant. A named symbol (a global, a
  // function, a string literal's anonymous global, a file-scope
  // compound literal's object) is known at serialization time and
  // lives in `label`. A [GNU] `&&label` reference has no name yet:
  // codegen allocates the `.L` name during shaping, so the node is
  // kept here and resolved at emission.
  //
  // 地址常量的目标. 具名符号 (全局变量, 函数, 字符串字面量的匿名全局
  // 对象, 文件作用域复合字面量的对象) 在序列化时已知, 存放在 `label` 中.
  // [GNU] `&&label` 引用此时还没有名字: codegen 在整形阶段分配 `.L` 名,
  // 因此节点保存在这里, 并在发射时解析.
  char* label;
  LibcpNode* label_node;

  long addend;
};

// Declaration attributes collected by declspec: storage class and
// _Alignas. The parser fills this in and copies it onto the declaration
// record nodes; sema consumes it when it declares the object.
//
// declspec 收集到的声明属性: 存储类与 _Alignas. 解析器填充它, 并复制到
// 声明记录节点上; sema 在声明对象时使用它.
typedef struct libcp_var_attr {
  bool is_typedef;
  bool is_static;
  bool is_extern;
  bool is_inline;
  bool is_tls;
  int align;
  // A `_Alignas(<expr>)` argument, of which `align_ty` is the
  // `_Alignas(<type>)` form. Whichever comes last is the one that
  // counts; sema reads the type form's alignment off a completed type.
  //
  // `_Alignas(<expr>)` 的参数, 其中 `align_ty` 对应 `_Alignas(<type>)`
  // 形式. 以最后出现的那个为准; sema 从完整类型上读取类型形式对齐值.
  LibcpNode* align_expr;
  LibcpType* align_ty;
} LibcpVarAttr;

// Faithful initializer record (parse.c): the syntactic shape of an
// initializer as written - a braced item sequence, a string literal or
// a single expression, with designators recorded unevaluated. sema
// resolves designators, applies brace elision and lowers the record
// when it types the carrying node (ND_DECL, compound literal) or when
// a global variable definition is parsed.
//
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
  int kind;              // INIT_LIST, INIT_EXPR or INIT_STR
                         // INIT_LIST, INIT_EXPR 或 INIT_STR
  LibcpToken* tok;       // first token of the initializer source
                         // 初始化器源码的首 token
  LibcpInitItem* items;  // INIT_LIST: braced element sequence
                         // INIT_LIST: 花括号元素序列
  LibcpNode* expr;       // INIT_EXPR: scalar initialization expression
                         // INIT_EXPR: 标量初始化表达式
  LibcpToken* str_tok;   // INIT_STR: string literal token
                         // INIT_STR: 字符串字面量 token

  // Only for the top-level record of an ND_DECL: the `=` that
  // introduced it, which is where sema anchors a diagnostic that
  // rejects the declaration as a whole.
  //
  // 仅用于 ND_DECL 的顶层记录: 引入它的 `=`, sema 在这里锚定
  // 拒绝整个声明的诊断.
  LibcpToken* eq_tok;
};

// One element of a braced initializer list.
// 花括号初始化列表中的一个元素.
struct libcp_init_item {
  LibcpInitItem* next;
  LibcpToken* comma_tok;   // the `,` before this element (NULL for the first)
                           // 本元素前的 `,` (第一个为 NULL)
  LibcpInitDesig* desigs;  // designator chain as written (may be NULL)
                           // 按书写形式记录的 designator 链 (可为 NULL)
  LibcpInitializer* init;  // the element value
                           // 元素值
};

// One designator as written: `[expr]`, `[begin ... end]` or `.name`.
// Index expressions are recorded unevaluated; sema evaluates them.
//
// 按书写形式记录的一个 designator: `[expr]`, `[begin ... end]` 或
// `.name`. 索引表达式以未求值形式记录; sema 负责求值.
struct libcp_init_desig {
  LibcpInitDesig* next;
  LibcpToken* tok;          // `[` or `.`
                            // `[` 或 `.`
  LibcpToken* after_begin;  // token after the begin expression (`...` or `]`)
                            // begin 表达式之后的 token (`...` 或 `]`)
  LibcpToken* rbracket;     // the `]` token
                            // 右方括号 token
  LibcpNode* begin;         // array designator index
                            // 数组 designator 索引
  LibcpNode* end;           // range end (NULL unless `[begin ... end]`)
                            // 范围终点 (非 `[begin ... end]` 时为 NULL)
  LibcpToken* name;         // member designator name
                            // 成员 designator 名
};

// The resolved initializer tree (PLAN A8.1): what a faithful
// initializer record becomes once designators are evaluated, member
// names are bound, brace elision is applied and flexible arrays are
// sized. sema's resolve pass builds it; the consumer's declaration
// expansion walks it to build the assignment chain, and sema's
// file-scope serializer walks it into .data bytes. Since initializers
// can be nested (e.g. `int x[2][2] = {{1, 2}, {3, 4}}`), it is a tree.
//
// 解析后的初始化器树 (PLAN A8.1): 忠实的初始化器记录在 designator 求值,
// 成员名绑定, 花括号省略应用, 柔性数组定长之后变成的形态. 由 sema 的
// resolve pass 构建; 消费者的声明展开遍历它来构建赋值链, sema 的
// 文件作用域序列化器遍历它生成 .data 字节. 初始化器可以嵌套
// (例如 `int x[2][2] = {{1, 2}, {3, 4}}`), 因此它是一棵树.
struct libcp_init_tree {
  LibcpType* ty;
  bool is_flexible;

  // If it's not an aggregate type and has an initializer,
  // `expr` has an initialization expression.
  //
  // 若不是聚合类型且有初始化器, `expr` 存放初始化表达式.
  LibcpNode* expr;

  // If it's an initializer for an aggregate type (e.g. array or struct),
  // `children` has initializers for its children.
  //
  // 若是聚合类型 (例如数组或结构体) 的初始化器, `children` 存放其
  // 子对象的初始化器.
  LibcpInitTree** children;

  // Only one member can be initialized for a union.
  // `mem` is used to clarify which member is initialized.
  //
  // 联合体只能初始化一个成员. `mem` 用于说明初始化的是哪个成员.
  LibcpMember* mem;
};

// Designator chain describing the position of an element within a
// local variable initializer (e.g. `x[1].y[2]`), walked by the
// consumer's expansion to build the target expression of each
// assignment.
//
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
  TY_VLA,     // variable-length array
              // 变长数组
  TY_TYPEOF,  // [GNU] `typeof(expr)`, whose operand type sema has not
              // annotated yet
              // [GNU] `typeof(expr)`, sema 尚未标注其操作数类型
  TY_STRUCT,
  TY_UNION,
} LibcpTypeKind;

struct libcp_type {
  LibcpTypeKind kind;
  int size;           // sizeof() value
                      // sizeof() 值
  int align;          // alignment
                      // 对齐
  bool is_unsigned;   // unsigned or signed
                      // 无符号或有符号
  bool is_atomic;     // true if _Atomic
                      // 若为 _Atomic 则为 true
  LibcpType* origin;  // for type compatibility check
                      // 用于类型兼容性检查

  // Pointer-to or array-of type. We intentionally use the same member
  // to represent pointer/array duality in C.
  //
  // In many contexts in which a pointer is expected, we examine this
  // member instead of "kind" member to determine whether a type is a
  // pointer or not. That means in many contexts "array of T" is
  // naturally handled as if it were "pointer to T", as required by
  // the C spec.
  //
  // 指向类型或数组类型. 我们有意用同一个成员表示 C 中指针/数组的二重性.
  //
  // 在许多期望指针的上下文中, 我们检查这个成员而不是 "kind" 成员,
  // 以判断一个类型是否为指针. 这意味着在许多上下文中, "T 的数组"
  // 会自然地按 "指向 T 的指针" 处理, 符合 C 规范的要求.
  LibcpType* base;

  // Declaration. A declarator name is never written into a shared type
  // record (F1); these fields are filled only on the per-parameter copy
  // inside a function type, where sema reads them.
  //
  // 声明信息. declarator 名字从不写入共享的类型记录 (F1); 这些字段只在
  // 函数类型内部逐参数复制的那份类型上填充, sema 在那里读取它们.
  LibcpToken* name;
  LibcpToken* name_pos;

  // The tag identifier of a struct/union/enum specifier, recorded by
  // the parser at the forward-declaration and definition sites and
  // shared by every use site that resolves to the record; an anonymous
  // specifier carries NULL. The tag spelling otherwise lives only in
  // the parser's scope, so this is a consumer's way to recover it
  // (PLAN-c2c D2).
  //
  // struct/union/enum specifier 的标签标识符, 由解析器在向前声明与定义处
  // 记录, 并被每个解析到该记录的使用点共享; 匿名 specifier 携带 NULL.
  // 标签拼写否则只存在于解析器的作用域中, 因此这是消费者恢复它的途径
  // (PLAN-c2c D2).
  LibcpToken* tag_tok;

  // Array
  // 数组
  int array_len;

  // Records the parser's declarator layer leaves for sema to complete
  // (see resolve_type): an array dimension expression that has not been
  // evaluated, so that neither the length nor the fixed-or-variable
  // question is answered in the parser; a `typeof(expr)` operand whose
  // type has not been annotated; and the argument of an `aligned`
  // attribute. Each is cleared once consumed.
  //
  // 记录解析器 declarator 层留给 sema 完成的内容 (见 resolve_type):
  // 尚未求值的数组维度表达式, 因此长度以及定长/变长问题都不在解析器中
  // 回答; 尚未标注类型的 `typeof(expr)` 操作数; 以及 `aligned` 属性的
  // 参数. 每一项在被消费后清空.
  LibcpNode* dim_len;
  LibcpNode* typeof_expr;
  LibcpNode* align_expr;

  // Variable-length array. The runtime size slot codegen creates for
  // it (A7.1) is kept in codegen's private per-type table (PLAN L1).
  //
  // 变长数组. codegen 为它创建的运行时大小槽 (A7.1) 保存在 codegen
  // 私有的 per-type 表中 (PLAN L1).
  LibcpNode* vla_len;  // # of elements
                       // 元素个数

  // Struct
  // 结构体
  LibcpMember* members;
  bool is_flexible;
  bool is_packed;
  // The member list is complete but sema has not placed the members
  // yet; resolve_type lays the type out on first sight (an incomplete
  // forward-declared type never carries this).
  //
  // 成员链表已完整, 但 sema 尚未放置成员; resolve_type 在第一次看到时
  // 布局该类型 (不完整的向前声明类型不会携带此标志).
  bool layout_pending;

  // Function type
  // 函数类型
  LibcpType* return_ty;
  LibcpType* params;
  bool is_variadic;
  // Enum-constant declaration records a parameter's declspec produced
  // (e.g. `int f(enum E { A } x)`); sema's resolve pass registers them
  // when it completes this function type.
  //
  // 参数的 declspec 产生的枚举常量声明记录 (例如 `int f(enum E { A } x)`);
  // sema 的 resolve pass 在完成该函数类型时注册它们.
  LibcpNode* spec_decls;
  LibcpType* next;
};

// Struct member
// 结构体成员
struct libcp_member {
  LibcpMember* next;
  LibcpType* ty;
  LibcpToken* tok;  // for error message
                    // 用于错误信息
  LibcpToken* name;
  int idx;
  int align;
  int offset;

  // A `_Alignas` on the member's declspec: the `_Alignas(<type>)` form
  // is recorded in `align_ty` and the `_Alignas(<expr>)` form in
  // `align_expr`; sema settles the value when the aggregate is laid
  // out. Zero means no `_Alignas`, and the type's own alignment counts.
  //
  // 成员 declspec 上的 `_Alignas`: `_Alignas(<type>)` 形式记录在
  // `align_ty` 中, `_Alignas(<expr>)` 形式记录在 `align_expr` 中;
  // sema 在布局聚合体时确定该值. 零表示没有 `_Alignas`, 此时按类型
  // 自身的对齐.
  LibcpType* align_ty;
  LibcpNode* align_expr;

  // Bitfield
  // 位域
  bool is_bitfield;
  int bit_offset;
  int bit_width;
  // The recorded width expression of a bitfield, unevaluated; sema
  // evaluates it into bit_width when the aggregate is laid out.
  //
  // 位域记录的宽度表达式, 未求值; sema 在布局聚合体时求值并写入
  // bit_width.
  LibcpNode* width_expr;
};

// AST node
// AST 节点
typedef enum {
  ND_NULL_EXPR,         // Do nothing
                        // 什么都不做
  ND_ADD,               // +
  ND_SUB,               // -
  ND_MUL,               // *
  ND_DIV,               // /
  ND_NEG,               // unary -
                        // 一元 -
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
  ND_INCDEC,            // "++" and "--"
                        // "++" 与 "--"
  ND_SUBSCRIPT,         // x[y]
  ND_COND,              // ?:
  ND_COMMA,             // ,
  ND_MEMBER,            // . (struct member access)
                        // . (结构体成员访问)
  ND_ADDR,              // unary &
                        // 一元 &
  ND_DEREF,             // unary *
                        // 一元 *
  ND_NOT,               // !
  ND_BITNOT,            // ~
  ND_LOGAND,            // &&
  ND_LOGOR,             // ||
  ND_RETURN,            // "return"
  ND_IF,                // "if"
  ND_FOR,               // "for" (also the lowered shape of "while")
                        // "for" (也是 "while" 降级后的形态)
  ND_WHILE,             // "while"; sema lowers it to ND_FOR
                        // "while"; sema 将其降级为 ND_FOR
  ND_DO,                // "do"
  ND_SWITCH,            // "switch"
  ND_CASE,              // "case"
  ND_BLOCK,             // { ... }
  ND_GOTO,              // "goto"
  ND_BREAK,             // "break"; sema lowers it to a jump to the break label
                        // "break"; sema 将其降级为跳到 break 标签
  ND_CONTINUE,          // "continue"; sema lowers it to a jump to the continue label
                        // "continue"; sema 将其降级为跳到 continue 标签
  ND_GOTO_EXPR,         // "goto" labels-as-values
                        // "goto" 标签值
  ND_LABEL,             // Labeled statement
                        // 带标签语句
  ND_LABEL_VAL,         // [GNU] Labels-as-values
                        // [GNU] 标签值
  ND_FUNCALL,           // Function call
                        // 函数调用
  ND_EXPR_STMT,         // Expression statement
                        // 表达式语句
  ND_STMT_EXPR,         // Statement expression
                        // 语句表达式
  ND_IDENT,             // An unresolved name; sema binds it to a variable,
                        // a function or an enum constant
                        // 未解析的名字; sema 将其绑定到变量, 函数或枚举常量
  ND_VAR,               // Variable
                        // 变量
  ND_VLA_PTR,           // VLA designator
                        // 变长数组指示符
  ND_NUM,               // Integer
                        // 整数
  ND_STRING,            // String literal; sema lowers it to an anonymous global
                        // 字符串字面量; sema 将其降级为匿名全局对象
  ND_SIZEOF,            // "sizeof"; sema folds it to its value
                        // "sizeof"; sema 将其折叠为值
  ND_ALIGNOF,           // "_Alignof"; sema folds it to its value
                        // "_Alignof"; sema 将其折叠为值
  ND_GENERIC,           // "_Generic"; sema selects an association and becomes it
                        // "_Generic"; sema 选择一个关联并成为它
  ND_GENERIC_ASSOC,     // One association of an ND_GENERIC: the type name in
                        // ty_op (NULL for "default:"), the result expression in
                        // lhs. A carrier only; it never reaches codegen.
                        // ND_GENERIC 的一个关联: 类型名在 ty_op 中 ("default:"
                        // 为 NULL), 结果表达式在 lhs 中. 仅作载体; 从不进入 codegen.
  ND_TYPES_COMPATIBLE,  // "__builtin_types_compatible_p"; sema folds it to 0 or 1
                        // "__builtin_types_compatible_p"; sema 将其折叠为 0 或 1
  ND_REG_CLASS,         // "__builtin_reg_class"; sema folds it to its class
                        // "__builtin_reg_class"; sema 将其折叠为对应的类别
  ND_CAST,              // LibcpType cast. The parser leaves an explicit cast untyped
                        // with its target type in ty_op; sema resolves and types
                        // the node. Casts sema inserts are fully typed at birth
                        // and marked is_implicit.
                        // 类型转换. 解析器让显式 cast 保持未定型, 目标类型放在
                        // ty_op 中; sema 解析并标注该节点. sema 插入的 cast 在
                        // 创建时即完成定型, 并标记 is_implicit.
  ND_MEMZERO,           // Zero-clear a stack variable
                        // 将栈变量清零
  ND_DECL,              // Declaration of a local variable: the name token on its
                        // type record, the attributes in `attr` and the faithful
                        // initializer record. sema declares the variable (pass 1)
                        // and lowers the node to statements (pass 2).
                        // 局部变量声明: 名字 token 在其类型记录上, 属性在 `attr`
                        // 中, 忠实的初始化器记录也在其中. sema 声明该变量
                        // (第 1 遍), 并把节点降级为语句 (第 2 遍).
  ND_GVAR_DECL,         // Declaration record of a global variable (file scope, or
                        // an `extern`/tentative declaration inside a block). sema
                        // declares the variable, serializes its initializer and
                        // removes the record from any statement chain.
                        // 全局变量的声明记录 (文件作用域, 或块内的 `extern`/
                        // tentative 声明). sema 声明该变量, 序列化其初始化器,
                        // 并从任何语句链中移除该记录.
  ND_FUNCDEF,           // Declaration record of a function; `body` is non-NULL for
                        // a definition. sema declares the function, runs both
                        // passes over the body and removes the record from any
                        // statement chain (a block-scope prototype or a [GNU]
                        // nested definition).
                        // 函数的声明记录; 定义时 `body` 非 NULL. sema 声明该函数,
  // 对其函数体运行两遍处理, 并从任何语句链中移除该记录
  // (块作用域原型或 [GNU] 嵌套定义).
  ND_TYPEDEF,           // Typedef declaration record; sema resolves its type and
                        // removes it from any statement chain.
                        // typedef 声明记录; sema 解析其类型, 并从任何语句链中
                        // 移除它.
  ND_ENUM_CONST,        // Enum constant declaration (member name + optional value
                        // expression). sema evaluates and registers the name, and
                        // removes the record from any statement chain.
                        // 枚举常量声明 (成员名 + 可选值表达式). sema 求值并注册
                        // 该名字, 并从任何语句链中移除该记录.
  ND_COMPOUND_LITERAL,  // "(type){...}": the type in ty_op, the faithful
                        // initializer record in decl_init. sema materializes the
                        // hidden variable and lowers the node.
                        // "(type){...}": 类型在 ty_op 中, 忠实的初始化器记录在
                        // decl_init 中. sema 物化隐藏变量并降级该节点.
  ND_ASM,               // "asm"
  ND_CAS,               // Atomic compare-and-swap
                        // 原子比较并交换
  ND_EXCH,              // Atomic exchange
                        // 原子交换
} LibcpNodeKind;

// AST node type
// AST 节点类型
struct libcp_node {
  LibcpNodeKind kind;  // LibcpNode kind
                       // LibcpNode 种类
  LibcpNode* next;     // Next node
                       // 下一个节点
  LibcpType* ty;       // LibcpType, e.g. int or pointer to int
                       // 类型, 例如 int 或指向 int 的指针
  LibcpToken* tok;     // Representative token
                       // 代表 token

  LibcpNode* lhs;  // Left-hand side
                   // 左侧
  LibcpNode* rhs;  // Right-hand side
                   // 右侧

  // Compound assignment operator for ND_ASSIGN ("+=", "-=", ...).
  // 0 means a plain "=".
  //
  // ND_ASSIGN 的复合赋值运算符 ("+=", "-=", ...).
  // 0 表示普通的 "=".
  LibcpNodeKind op;

  // Increment/decrement for ND_INCDEC: postfix (`i++`) or prefix
  // (`++i`), with an addend of +1 or -1. sema lowers the node to a
  // compound assignment.
  //
  // ND_INCDEC 的自增/自减: 后缀 (`i++`) 或前缀 (`++i`), 增量为 +1
  // 或 -1. sema 将该节点降级为复合赋值.
  bool is_post;
  int addend;

  // "if" or "for" statement
  // "if" 或 "for" 语句
  LibcpNode* cond;
  LibcpNode* then;
  LibcpNode* els;
  LibcpNode* init;
  LibcpNode* inc;

  // Block or statement expression
  // 块或语句表达式
  LibcpNode* body;

  // ND_BLOCK: true for a compound-statement block, which is a lexical
  // scope; the empty statement and the block sema folds a `for`
  // init-declaration chain into are ND_BLOCK nodes without one. sema's
  // resolve pass derives its scope stack from this flag.
  //
  // ND_BLOCK: 复合语句块为 true, 它是一个词法作用域; 空语句以及 sema
  // 折叠 `for` 初始化声明链时形成的块是不带此标志的 ND_BLOCK 节点.
  // sema 的 resolve pass 根据该标志推导作用域栈.
  bool is_scope_block;

  // Struct member access. The parser leaves it unbound: an unresolved
  // ND_MEMBER carries the member name in `tok`, and sema's add_type
  // looks the name up. `arrow_tok` is the `->` token of a pointer
  // access and NULL for a plain `.`; it is the anchor of the checks on
  // the operand, which is why the token itself is kept. sema binds the
  // member and leaves the marker on the innermost link of a flattened
  // chain (clearing it on the dissolved outer node); the evaluator
  // reads it in constant expressions, and codegen's gen_addr and
  // to_assign read it as the dereference the `->` stands for (PLAN
  // B1.1b - the marker stays on the node, nothing rewrites it away).
  //
  // 结构体成员访问. 解析器不绑定它: 未解析的 ND_MEMBER 在 `tok` 中
  // 携带成员名, sema 的 add_type 负责查找该名字. `arrow_tok` 是指针
  // 访问的 `->` token, 普通 `.` 为 NULL; 它是操作数检查的锚点, 因此
  // 保留该 token. sema 绑定成员, 并把标记留在扁平化链的最内层连接上
  // (在消解的外层节点上清除); 求值器在常量表达式中读取它, codegen
  // 的 gen_addr 与 to_assign 把它读作 `->` 所代表的解引用
  // (PLAN B1.1b - 标记留在节点上, 不会被改写掉).
  LibcpMember* member;
  LibcpToken* arrow_tok;

  // Function call. func_ty and the conversions on the arguments are
  // sema's (the annotation). The call's codegen-side state - the
  // pass-by-stack marks and the caller-owned return buffer - lives in
  // codegen's private per-node table (PLAN L1).
  //
  // 函数调用. func_ty 以及实参上的转换属于 sema (标注). 调用在 codegen
  // 侧的状态 - 按栈传递标记与调用者拥有的返回缓冲区 - 保存在 codegen
  // 私有的 per-node 表中 (PLAN L1).
  LibcpType* func_ty;
  LibcpNode* args;

  // Goto or labeled statement, or labels-as-values. `label` is the
  // spelled name the parser recorded and the goto/label pairing check
  // compares; the `.L..` names codegen allocates for goto, label and
  // case nodes live in its private per-node table (PLAN L1).
  //
  // goto 或带标签语句, 或标签值. `label` 是解析器记录的拼写名字,
  // goto/label 配对检查会比较它; codegen 为 goto, label 与 case 节点
  // 分配的 `.L..` 名字保存在其私有的 per-node 表中 (PLAN L1).
  char* label;

  // [GNU] `a ?: b` conditional. sema types it; gen_expr emits it from
  // its own fields without a slot - the test value stays in the
  // register (PLAN B1.1e) - and the evaluator and is_const_expr treat
  // it as a runtime value through this flag.
  //
  // [GNU] `a ?: b` 条件表达式. sema 为其定型; gen_expr 直接用自己的
  // 字段发射, 不使用临时槽 - 测试值留在寄存器中 (PLAN B1.1e) - 求值器
  // 与 is_const_expr 通过该标志把它当作运行时值.
  bool is_elvis;

  // ND_SIZEOF/ND_ALIGNOF: the operand type for the `sizeof(type)`
  // form. The `sizeof expr` form carries its unevaluated operand in
  // `lhs` instead. sema records the size or alignment in `val`; a
  // VLA operand is not a constant and codegen's shaping pass builds
  // the runtime size expression for it (A9.1/A7.1). ND_CAST (from the
  // parser), ND_GENERIC_ASSOC, ND_TYPES_COMPATIBLE, ND_REG_CLASS,
  // ND_DECL, ND_GVAR_DECL, ND_FUNCDEF and ND_COMPOUND_LITERAL carry
  // their declared/target/operand type here as well.
  //
  // ND_SIZEOF/ND_ALIGNOF: `sizeof(type)` 形式的操作数类型.
  // `sizeof expr` 形式则把未求值的操作数放在 `lhs` 中. sema 把大小或
  // 对齐值记录到 `val`; VLA 操作数不是常量, codegen 的整形 pass 会为
  // 它构建运行时大小表达式 (A9.1/A7.1). ND_CAST (来自解析器),
  // ND_GENERIC_ASSOC, ND_TYPES_COMPATIBLE, ND_REG_CLASS, ND_DECL,
  // ND_GVAR_DECL, ND_FUNCDEF 与 ND_COMPOUND_LITERAL 也在这里存放
  // 它们声明的/目标的/操作数的类型.
  LibcpType* ty_op;

  // ND_TYPES_COMPATIBLE: the second type operand, the first being
  // ty_op. The type predicates take two types and no subexpression.
  //
  // ND_TYPES_COMPATIBLE: 第二个类型操作数, 第一个是 ty_op. 类型谓词
  // 接受两个类型, 不接受子表达式.
  LibcpType* ty_op2;

  // ND_GENERIC: the result expression of the association the controlling
  // expression selects - the conclusion of the selection, recorded on the
  // node so a consumer can read it instead of the node having to *become*
  // it. Written by sema's select_generic; the evaluator, gen_expr and
  // gen_addr read it (A9.1). The node keeps its kind.
  //
  // ND_GENERIC: 控制表达式所选中的关联的结果表达式 - 选择的结论,
  // 记录在节点上, 消费者可以直接读取, 而不必让节点"变成"它.
  // 由 sema 的 select_generic 写入; 求值器, gen_expr 与 gen_addr
  // 读取它 (A9.1). 节点保持自己的种类.
  LibcpNode* generic_sel;

  // ND_CAST: true for a cast sema inserted to carry out an implicit
  // conversion, false for one the source wrote. The parser never sets
  // it, new_cast always does, and the rewrite sites that copy a node's
  // shape along with it - so a source-to-source tool can tell the two
  // kinds of cast apart. codegen does not read the flag.
  //
  // ND_CAST: sema 为执行隐式转换而插入的 cast 为 true, 源码书写的 cast
  // 为 false. 解析器从不设置它, new_cast 总是设置, 复制节点形状的重写点
  // 也会一并复制 - 因此源码到源码的工具可以区分这两类 cast.
  // codegen 不读取该标志.
  bool is_implicit;

  // Declaration record nodes (ND_DECL, ND_GVAR_DECL, ND_FUNCDEF): the
  // storage-class and alignment attributes as the parser read them.
  //
  // 声明记录节点 (ND_DECL, ND_GVAR_DECL, ND_FUNCDEF): 解析器读到的
  // 存储类与对齐属性.
  LibcpVarAttr attr;

  // Declaration record nodes (ND_DECL, ND_GVAR_DECL, ND_FUNCDEF): the
  // declared identifier. The declarator also parks it on the type's
  // `name`, but types are shared (basic-type singletons, tag and
  // typedef types) and that slot is overwritten by the next
  // declarator, long before sema reads the record.
  //
  // 声明记录节点 (ND_DECL, ND_GVAR_DECL, ND_FUNCDEF): 被声明的标识符.
  // declarator 也会把它暂存在类型的 `name` 上, 但类型是共享的
  // (基本类型单例, 标签类型与 typedef 类型), 该槽位会在下一个
  // declarator 处被覆盖, 远早于 sema 读取记录.
  LibcpToken* name_tok;

  // Enum-constant declaration records that became visible while an
  // expression-context type was parsed (e.g. `sizeof(enum E { A })`).
  // sema's resolve pass evaluates and registers them at this node's
  // position, since the record has no statement-chain slot of its own.
  //
  // 在解析表达式上下文中的类型时变得可见的枚举常量声明记录
  // (例如 `sizeof(enum E { A })`). 由于该记录没有自己的语句链槽位,
  // sema 的 resolve pass 会在本节点的位置求值并注册它们.
  LibcpNode* spec_decls;

  // ND_DECL: the faithful initializer record, or NULL if the
  // declarator has no initializer. sema resolves and lowers it to the
  // MEMZERO + assignment comma chain.
  //
  // ND_DECL: 忠实的初始化器记录, 若 declarator 没有初始化器则为 NULL.
  // sema 解析它并降级为 MEMZERO + 赋值逗号链.
  LibcpInitializer* decl_init;

  // ND_DECL, ND_GVAR_DECL and ND_COMPOUND_LITERAL: an opaque payload
  // private to sema.c. The resolve pass parks the initializer record
  // resolved against the declared type here for the annotation pass; it
  // may be NULL outside a sema run, and a consumer must neither read nor
  // write it. The public tree does not interpret it.
  //
  // ND_DECL, ND_GVAR_DECL 与 ND_COMPOUND_LITERAL: sema.c 私有的不透明
  // 载荷. resolve pass 把按声明类型解析后的初始化器记录暂存在这里,
  // 供标注 pass 使用; 在 sema 运行之外它可能为 NULL, 消费者不得读写它.
  // 公共树不解释它.
  void* init_resolved;

  // Case
  // case
  // case 分支
  long begin;
  long end;
  bool is_default;  // `default:` rather than `case <expr>:`
                    // `default:` 而不是 `case <expr>:`

  // ND_CASE: the operands of `case <expr>:` and of the [GNU] range form
  // `case <expr> ... <expr>:`, recorded unevaluated; sema's add_type
  // fills in `begin`/`end` from them. `colon_tok` is the `:` that ends
  // the label, where the range check is anchored.
  //
  // ND_CASE: `case <expr>:` 以及 [GNU] 范围形式
  // `case <expr> ... <expr>:` 的操作数, 以未求值形式记录; sema 的
  // add_type 用它们填充 `begin`/`end`. `colon_tok` 是结束标签的 `:`,
  // 范围检查锚定在那里.
  LibcpNode* begin_expr;
  LibcpNode* end_expr;
  LibcpToken* colon_tok;

  // "asm" string literal
  // "asm" 字符串字面量
  char* asm_str;

  // Atomic compare-and-swap
  // 原子比较并交换
  LibcpNode* cas_addr;
  LibcpNode* cas_old;
  LibcpNode* cas_new;

  // Atomic op= operators
  // 原子 op= 运算符
  LibcpObj* atomic_addr;
  LibcpNode* atomic_expr;

  // Variable
  // 变量
  LibcpObj* var;

  // Numeric literal
  // 数值字面量
  int64_t val;
  long double fval;
};

// The parser builds the faithful syntax tree and returns the top-level
// declaration record chain; it makes no semantic decision beyond the
// typedef/tag classification oracle the C grammar requires.
//
// 解析器构建忠实的语法树, 返回顶层声明记录链; 除了 C 语法所需的
// typedef/tag 分类 oracle 之外, 它不做任何语义决策.
LIBCP_API LibcpNode* libcp_parse(LibcpContext* ctx, LibcpToken* tok);

// The anchor-token extremes of a subtree (PLAN-l5 W5): the smallest and
// largest representative tokens by (file, offset). *start and *end are
// NULL when the subtree carries no tokens. `next` chains are siblings
// and are not followed; initializer trees are not descended.
//
// 子树锚点 token 的两端 (PLAN-l5 W5): 按 (文件, 偏移) 比较的最小与
// 最大代表 token. 子树不携带 token 时 *start 与 *end 为 NULL.
// `next` 链是兄弟节点, 不会跟进; 初始化器树不会被深入.
LIBCP_API void libcp_node_source_range(const LibcpNode* node, LibcpToken** start, LibcpToken** end);

#endif
