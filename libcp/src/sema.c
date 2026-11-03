// This file hosts the semantic analysis passes that operate on the
// faithful syntax tree built by parse.c.
//
// sema runs in two traversals per function body (and record by record
// at file scope):
//
//  - the resolve pass (resolve_node and friends) derives the scope
//    stack from the tree structure, declares the objects the parser's
//    declaration records name, binds every ND_IDENT, evaluates and
//    registers enum constants, settles the type records the parser
//    left to be completed (array dimensions, typeof operands,
//    alignments, bitfield widths, case values) and lays aggregates
//    out;
//
//  - the annotation pass (libcp_add_type and type_chain) types every node,
//    inserts the implicit conversions the standard requires, runs the
//    checks that need a typed tree, records the compile-time
//    conclusions on the nodes (sizeof values, generic selections, the
//    type builtins) and materializes the objects the language says
//    exist (a string literal's anonymous global, a compound literal's
//    hidden object, a block-scope static's data image). It rewrites
//    no tree shape: everything the older lines called lowering -
//    pointer scaling, compound assignment and inc/dec round trips,
//    the call, subscript, member, string, VLA, initializer and
//    declaration shaping - is codegen's shaping pass (plans/PLAN.md, the
//    codegen-direct line). The control-flow checks (stray
//    break/continue/case/default, undeclared labels) run as a
//    descent of their own; the label allocation and the control-flow
//    rewrites those checks used to ride along with are the shaping
//    pass's, and the goto and label-value references each function
//    collected for the pairing check are handed over through
//    LibcpObj.label_gotos.
//
// It also contains the constant expression evaluator (eval and
// friends), which both passes use, and which the preprocessor uses
// for `#if` through libcp_const_expr.
//
// A handful of its type-level tools are exported for consumers that
// build nodes of their own - libcp_add_type, libcp_new_arith, usual_arith_conv,
// libcp_get_common_type, libcp_new_cast and the node constructors (see libcp.h).
// A consumer obtains its conversions from here rather than restating
// them; what it contributes is the shape, not the semantics.
//
// 第 4 层 sema: 名字解析, 类型检查与标注, 常量求值, 布局与语义结论.
// 作用于 parse.c 构建的忠实语法树, 为 codegen 提供已标注的语义树.
//
// 本文件承载作用于 parse.c 构建的忠实语法树之上的语义分析 pass.
//
// sema 对每个函数体做两趟遍历 (文件作用域下则逐条声明记录处理):
//
//  - resolve pass (resolve_node 及其同族函数) 从树结构推导作用域栈,
//    声明解析器的声明记录所指称的对象, 绑定每个 ND_IDENT, 求值并注册
//    枚举常量, 完成解析器留给后续处理的类型记录 (数组维度, typeof
//    操作数, 对齐, 位域宽度, case 值), 并完成聚合类型的布局;
//
//  - 标注 pass (libcp_add_type 与 type_chain) 为每个节点定型, 插入标准
//    要求的隐式转换, 运行需要已定型树的检查, 把编译期结论记录到节点上
//    (sizeof 值, 泛型选择, 类型内建), 并物化语言规定存在的对象 (字符串
//    字面量的匿名全局对象, 复合字面量的隐藏对象, 块作用域 static 的
//    数据镜像). 它不改写任何树形: 旧代码称为降级的一切 - 指针缩放,
//    复合赋值与自增自减的往返, 调用, 下标, 成员, 字符串, VLA, 初始化器
//    与声明的整形 - 都是 codegen 的 shaping pass (plans/PLAN.md 的
//    codegen-direct 路线). 控制流检查 (游离的 break/continue/case/default,
//    未声明的标签) 作为独立的一趟遍历运行; 标签分配以及这些检查过去
//    附带的控制流改写属于 shaping pass, 每个函数为配对检查收集的
//    goto 与标签值引用通过 LibcpObj.label_gotos 移交.
//
// 它还包含常量表达式求值器 (eval 及其同族), 两个 pass 都会使用它,
// 预处理器也通过 libcp_const_expr 用它处理 `#if`.
//
// 少量类型层工具导出给自行构建节点的消费者 - libcp_add_type,
// libcp_new_arith, usual_arith_conv, libcp_get_common_type,
// libcp_new_cast 以及节点构造函数 (见 libcp.h). 消费者从这里获得
// 转换, 而不是重新实现它们; 它提供的是形状, 不是语义.

#include "libcp.h"

// `fn->locals` is the field the context's `locals` list is spliced
// into; these helpers keep the field access out of the macro below.
//
// `fn->locals` 是 context 的 `locals` 链表拼接进的那个字段;
// 这两个辅助函数把字段访问挡在下面的宏之外.
static LibcpObj* obj_locals(LibcpObj* fn) {
  return fn->locals;
}

static void set_obj_locals(LibcpObj* fn, LibcpObj* v) {
  fn->locals = v;
}

static int64_t eval(LibcpContext* ctx, LibcpNode* node);

// The target an address constant resolves to. `name` is a symbol the
// serializer can write down now; `node` is a [GNU] `&&label` whose
// `.L` name codegen allocates later.
//
// 地址常量解析得到的目标. `name` 是序列化器现在就能写下的符号;
// `node` 是 [GNU] 的 `&&label`, 它的 `.L` 名字由 codegen 稍后分配.
typedef struct {
  char* name;
  LibcpNode* node;
} LibcpLabelRef;

static int64_t eval2(LibcpContext* ctx, LibcpNode* node, LibcpLabelRef* label);
static int64_t eval_rval(LibcpContext* ctx, LibcpNode* node, LibcpLabelRef* label);
static double eval_double(LibcpContext* ctx, LibcpNode* node);
static bool is_const_expr(LibcpContext* ctx, LibcpNode* node);
static void layout_struct(LibcpContext* ctx, LibcpType* ty);
static void layout_union(LibcpContext* ctx, LibcpType* ty);
static void type_chain(LibcpContext* ctx, LibcpNode* head);
static void check_control_flow(LibcpContext* ctx, LibcpNode* body);
static void analyze_function(LibcpContext* ctx, LibcpObj* fn);
static void resolve_node(LibcpContext* ctx, LibcpNode* node);
static void resolve_chain(LibcpContext* ctx, LibcpNode* node);
static void resolve_function(LibcpContext* ctx, LibcpNode* node);
static void resolve_enum_records(LibcpContext* ctx, LibcpNode* recs);
static void resolve_type_exprs(LibcpContext* ctx, LibcpType* ty);
static void resolve_type(LibcpContext* ctx, LibcpType* ty);
static void serialize_gvar(LibcpContext* ctx, LibcpNode* node);
static int attr_align(LibcpContext* ctx, LibcpVarAttr* attr);
static LibcpObj* new_gvar(LibcpContext* ctx, char* name, LibcpType* ty);
static LibcpObj* new_lvar(LibcpContext* ctx, char* name, LibcpType* ty);
static char* new_unique_name(LibcpContext* ctx);
static LibcpMember* get_struct_member(LibcpType* ty, LibcpToken* tok);

// All local variable instances created for the function being analyzed
// are accumulated to this list.
//
// 为当前正在分析的函数创建的所有局部变量实例都累积到这个链表.

// Likewise, global variables are accumulated to this list.
//
// 类似地, 全局变量累积到这个链表.

// The function sema is currently working on, needed when a reference to
// a "static inline" function is recorded and when a return statement is
// converted to the return type. Like the parser's old current_fn, it
// persists after a [GNU] nested function definition is analyzed.
//
// sema 当前正在处理的函数, 在记录对 "static inline" 函数的引用以及把
// return 语句转换为返回类型时都需要它. 与解析器旧的 current_fn 一样,
// 它在 [GNU] 嵌套函数定义分析结束后仍然保留.

// The declaration of `alloca`, which the VLA lowering calls. It is a
// global object so that the call node can refer to it, but never a
// definition, so codegen emits nothing for it.
//
// `alloca` 的声明, VLA 降级会调用它. 它是一个全局对象, 以便调用节点
// 能引用它, 但永远不是定义, 因此 codegen 不会为它发射任何内容.

// Constructors for the node shapes only sema produces: a bound name
// (the parser leaves names unbound, as ND_IDENT), a VLA designator, and
// a numeric literal whose type is settled at birth rather than read off
// a token. Exported (see libcp.h): a consumer that builds a node needs
// it to arrive typed, and these are the shapes it cannot build itself.
//
// 只有 sema 才会产生的节点形态的构造函数: 已绑定名字 (解析器把名字
// 留作未绑定的 ND_IDENT), VLA 指示符, 以及类型在创建时即确定而不是
// 从 token 读出的数值字面量. 已导出 (见 libcp.h): 构建节点的消费者
// 需要节点一到达就已定型, 而这些是它自己无法构造的形态.
LibcpNode* libcp_new_var_node(LibcpContext* ctx, LibcpObj* var, LibcpToken* tok) {
  LibcpNode* node = libcp_new_node(ctx, ND_VAR, tok);
  node->var       = var;
  return node;
}

LibcpNode* libcp_new_vla_ptr(LibcpContext* ctx, LibcpObj* var, LibcpToken* tok) {
  LibcpNode* node = libcp_new_node(ctx, ND_VLA_PTR, tok);
  node->var       = var;
  return node;
}

LibcpNode* libcp_new_long(LibcpContext* ctx, int64_t val, LibcpToken* tok) {
  LibcpNode* node = libcp_new_node(ctx, ND_NUM, tok);
  node->val       = val;
  node->ty        = libcp_ty_long;
  return node;
}

LibcpNode* libcp_new_ulong(LibcpContext* ctx, long val, LibcpToken* tok) {
  LibcpNode* node = libcp_new_node(ctx, ND_NUM, tok);
  node->val       = val;
  node->ty        = libcp_ty_ulong;
  return node;
}

// The one place an implicit cast is built: every conversion sema
// inserts - the usual arithmetic conversions, an assignment, a return
// or an argument converted to its target type, the cast a lowered
// increment restores the operand's type with - comes through here, so
// marking the node is a single assignment. A cast the source wrote
// arrives as an ND_CAST node from the parser instead, which leaves the
// flag false, and libcp_add_type resolves rather than builds it.
//
// 隐式转换唯一的构建点: sema 插入的每个转换 - 常规算术转换, 赋值,
// return 或实参转换到目标类型, 降级后的自增恢复操作数类型所用的
// 转换 - 都经过这里, 因此标记节点只需一次赋值. 源码写出的 cast 由
// 解析器以 ND_CAST 节点到达, 其标记保持 false, 由 libcp_add_type
// 解析而不是构建.
LibcpNode* libcp_new_cast(LibcpContext* ctx, LibcpNode* expr, LibcpType* ty) {
  libcp_add_type(ctx, expr);

  LibcpNode* node   = libcp_arena_calloc(ctx, 1, sizeof(LibcpNode));
  node->kind        = ND_CAST;
  node->tok         = expr->tok;
  node->lhs         = expr;
  node->ty          = libcp_copy_type(ctx, ty);
  node->is_implicit = true;
  return node;
}

// An object or an enum constant the resolve pass has declared.
// resolve pass 已声明的对象或枚举常量.
typedef struct {
  LibcpObj* var;
  LibcpType* enum_ty;
  int enum_val;
} LibcpVarScope;

// Represents a block scope.
// 表示一个块作用域.
struct libcp_sema_scope {
  LibcpSemaScope* next;
  LibcpHashMap vars;
};

// Zeroes the sema-owned context fields. It allocates nothing: the
// context reset calls it after releasing the arena, and sema_init calls
// it before creating the root scope. Keeping the whole set in one place
// is what lets a repeated libcp_sema without a reset start from the
// same state as a reset one (the audit's label_gotos / unique_name_id
// omission).
//
// 清零 sema 拥有的 context 字段. 它不分配任何内存: context reset 在
// 释放 arena 之后调用它, sema_init 在创建根作用域之前调用它. 把整组
// 字段放在一处, 才能让未 reset 的重复 libcp_sema 与 reset 过的调用
// 从相同状态开始 (审计指出的 label_gotos / unique_name_id 遗漏).
void libcp_sema_state_clear(LibcpContext* ctx) {
  ctx->sema_scope     = NULL;
  ctx->locals         = NULL;
  ctx->globals        = NULL;
  ctx->sema_fn        = NULL;
  ctx->builtin_alloca = NULL;
  ctx->gotos          = (LibcpNodeArray){};
  ctx->labels         = (LibcpNodeArray){};
  ctx->label_gotos    = NULL;
  ctx->loop_depth     = 0;
  ctx->switch_depth   = 0;
  ctx->unique_name_id = 0;
  ctx->resolving_body = false;
}

// The resolve pass derives this scope stack from the tree structure, so
// it lives entirely within sema; the parser keeps a separate stack of
// its own for the typedef/tag classification the C grammar needs. The
// stack lives in LibcpContext (PLAN L2.5).
//
// A run starts with the fields cleared and a fresh root scope.
//
// resolve pass 从树结构推导这个作用域栈, 因此它完全属于 sema; 解析器
// 为 C 文法所需的 typedef/tag 分类保留自己独立的一个栈. 该栈存放在
// LibcpContext 中 (PLAN L2.5).
//
// 每次运行都从清零的字段和一个新的根作用域开始.
static void sema_init(LibcpContext* ctx) {
  libcp_sema_state_clear(ctx);
  ctx->sema_scope = libcp_arena_calloc(ctx, 1, sizeof(LibcpSemaScope));
}

#define scope          (ctx->sema_scope)
#define locals         (ctx->locals)
#define globals        (ctx->globals)
#define sema_fn        (ctx->sema_fn)
#define builtin_alloca (ctx->builtin_alloca)

static void enter_scope(LibcpContext* ctx) {
  // 压入一个新的块作用域.
  LibcpSemaScope* sc = libcp_arena_calloc(ctx, 1, sizeof(LibcpSemaScope));
  sc->next           = scope;
  scope              = sc;
}

static void leave_scope(LibcpContext* ctx) {
  // 弹出当前块作用域, 回到外层.
  scope = scope->next;
}

// Find a variable by name.
// 按名字查找变量.
static LibcpVarScope* find_var(LibcpContext* ctx, LibcpToken* tok) {
  for (LibcpSemaScope* sc = scope; sc; sc = sc->next) {
    LibcpVarScope* sc2 = libcp_hashmap_get2(ctx, &sc->vars, tok->loc, tok->len);
    if (sc2)
      return sc2;
  }
  return NULL;
}

static LibcpVarScope* push_scope(LibcpContext* ctx, char* name) {
  // 在当前作用域登记一个名字, 返回其绑定槽.
  LibcpVarScope* sc = libcp_arena_calloc(ctx, 1, sizeof(LibcpVarScope));
  libcp_hashmap_put(ctx, &scope->vars, name, sc);
  return sc;
}

// A block-scope declaration may not declare a void object. `tok` is the
// position of the declaration, anchored exactly as the parser used to.
//
// 块作用域声明不得声明 void 对象. `tok` 是声明所在的位置, 锚点与解析器
// 过去使用的完全一致.
static void check_declared_void(LibcpContext* ctx, LibcpToken* tok, LibcpType* ty) {
  if (ty->kind == TY_VOID)
    libcp_error_tok(ctx, tok, "variable declared void");
}

// Looks up a function in the file scope.
// 在文件作用域中查找函数.
static LibcpObj* find_func(LibcpContext* ctx, char* name) {
  LibcpSemaScope* sc = scope;
  while (sc->next)
    sc = sc->next;

  LibcpVarScope* sc2 = libcp_hashmap_get(ctx, &sc->vars, name);
  if (sc2 && sc2->var && sc2->var->is_function)
    return sc2->var;
  return NULL;
}

// Declares a function at file scope, or checks a redeclaration against
// the object declared before. `tok` is the token following the
// declarator and `is_definition` says whether a body follows.
//
// 在文件作用域声明函数, 或把重复声明与之前声明的对象做检查. `tok` 是
// 声明符之后的 token, `is_definition` 表示后面是否跟随函数体.
static LibcpObj* declare_function(LibcpContext* ctx,
                                  char* name,
                                  LibcpType* ty,
                                  LibcpVarAttr* attr,
                                  LibcpToken* tok,
                                  bool is_definition) {
  LibcpObj* fn = find_func(ctx, name);

  if (fn) {
    // Redeclaration. The first check is unreachable in the current
    // tree: find_func only ever returns an is_function object, so
    // fn->is_function is always true here (the diagnostic lock
    // leaves it uncovered for exactly that reason).
    //
    // 重复声明. 第一个检查在当前树中不可达: find_func 只会返回
    // is_function 为真的对象, 因此这里的 fn->is_function 恒为真
    // (诊断行为锁正是因此没有覆盖到它).
    if (!fn->is_function)
      libcp_error_tok(ctx, tok, "redeclared as a different kind of symbol");
    if (fn->is_definition && is_definition)
      libcp_error_tok(ctx, tok, "redefinition of %s", name);
    if (!fn->is_static && attr->is_static)
      libcp_error_tok(ctx, tok, "static declaration follows a non-static declaration");
    fn->is_definition = fn->is_definition || is_definition;
  } else {
    fn                = new_gvar(ctx, name, ty);
    fn->is_function   = true;
    fn->is_definition = is_definition;
    fn->is_static     = attr->is_static || (attr->is_inline && !attr->is_extern);
    fn->is_inline     = attr->is_inline;
  }

  fn->is_root = !(fn->is_static && fn->is_inline);
  return fn;
}

// The goto and label population of the function being checked: the
// goto and [GNU] label-value nodes collected while its body was
// annotated and checked (a label value can live outside the statement
// tree, in a block-scope static initializer, which only the
// annotation pass reaches - see libcp_add_type's ND_LABEL_VAL case).
//
// 正在检查的函数的 goto 与标签集合: 在其函数体被标注和检查期间收集的
// goto 与 [GNU] 标签值节点 (标签值可以位于语句树之外, 例如块作用域
// static 初始化器中, 只有标注 pass 能到达那里 - 见 libcp_add_type 的
// ND_LABEL_VAL 分支).
#define gotos  (ctx->gotos)
#define labels (ctx->labels)

// The pairing check: every goto and label value must name a label of
// the same function. Gotos may refer to a label that appears later,
// so this runs once the whole body has been checked. The matched name
// is written by codegen's shaping pass, which is what allocates it.
//
// 配对检查: 每个 goto 与标签值都必须指向同一函数内的标签. goto 可以
// 引用稍后才出现的标签, 因此这一步在整个函数体检查完毕后运行. 匹配的
// 名字由 codegen 的 shaping pass 写入, 名字分配也在那里.
static void check_labels(LibcpContext* ctx) {
  for (int i = 0; i < gotos.len; i++) {
    LibcpNode* x = gotos.data[i];
    bool found   = false;
    for (int j = 0; j < labels.len; j++)
      if (!strcmp(x->label, labels.data[j]->label)) {
        found = true;
        break;
      }
    if (!found)
      libcp_error_tok(ctx, x->tok->next, "use of undeclared label");
  }
}

// The control-flow checks that ride the descent: the stray
// break/continue/case/default diagnostics and the collection for the
// pairing check above. The work half of the old descent - the label
// allocation, the case chains, and the `while`/`break`/`continue`
// rewrites - is codegen's shaping pass now; this side only tracks the
// nesting the checks need (a break binds to the innermost loop or
// switch, a continue to the innermost loop, a case to the innermost
// switch - existence is all each check asks). It writes no names.
//
// 随遍历执行的控制流检查: 游离的 break/continue/case/default 诊断,
// 以及为上面的配对检查收集节点. 旧遍历中干活的那一半 - 标签分配,
// case 链, `while`/`break`/`continue` 改写 - 现在属于 codegen 的
// shaping pass; 这一侧只跟踪检查所需的嵌套层级 (break 绑定到最内层
// 循环或 switch, continue 绑定到最内层循环, case 绑定到最内层 switch -
// 每个检查只问存在性). 它不写入任何名字.
#define loop_depth   (ctx->loop_depth)
#define switch_depth (ctx->switch_depth)

static void check_node(LibcpContext* ctx, LibcpNode* node);

static void check_chain(LibcpContext* ctx, LibcpNode* node) {
  for (LibcpNode* n = node; n; n = n->next)
    check_node(ctx, n);
}

// Descends every subtree a node owns, statements and expressions alike:
// a break can sit in a statement expression, which is an expression
// node holding a statement chain.
//
// 下降节点拥有的每个子树, 语句与表达式一视同仁: break 可以出现在
// 语句表达式里, 而语句表达式是持有语句链的表达式节点.
static void check_children(LibcpContext* ctx, LibcpNode* node) {
  check_node(ctx, node->lhs);
  check_node(ctx, node->rhs);
  check_node(ctx, node->cond);
  check_node(ctx, node->then);
  check_node(ctx, node->els);
  check_node(ctx, node->init);
  check_node(ctx, node->inc);
  check_chain(ctx, node->body);
  check_chain(ctx, node->args);
  check_node(ctx, node->cas_addr);
  check_node(ctx, node->cas_old);
  check_node(ctx, node->cas_new);
}

// Pre-order, like the shaping pass it mirrors: the checks of a loop
// fire inside it, the stray check of a case inside its switch.
//
// 先序遍历, 与它镜像的 shaping pass 一致: 循环的检查在其内部触发,
// case 的游离检查在其 switch 内部触发.
static void check_node(LibcpContext* ctx, LibcpNode* node) {
  if (!node)
    return;

  switch (node->kind) {
    case ND_WHILE:
    case ND_DO:
    case ND_FOR:
      loop_depth++;
      check_children(ctx, node);
      loop_depth--;
      return;
    case ND_SWITCH:
      switch_depth++;
      check_children(ctx, node);
      switch_depth--;
      return;
    case ND_CASE:
      if (!switch_depth)
        libcp_error_tok(ctx, node->tok, node->is_default ? "stray default" : "stray case");
      check_node(ctx, node->lhs);
      return;
    case ND_BREAK:
      if (!loop_depth && !switch_depth)
        libcp_error_tok(ctx, node->tok, "stray break");
      return;
    case ND_CONTINUE:
      if (!loop_depth)
        libcp_error_tok(ctx, node->tok, "stray continue");
      return;
    case ND_GOTO: libcp_nodearray_push(ctx, &gotos, node); return;
    case ND_LABEL:
      libcp_nodearray_push(ctx, &labels, node);
      check_node(ctx, node->lhs);
      return;
    case ND_FUNCDEF:
      // A [GNU] nested definition: its own analysis ran when the record
      // was reached - checks, label pairing and all - and its body is
      // not this function's to descend. Walking it here would collect
      // the nested function's goto and label nodes into this function's
      // population, which its own resolution already handed to codegen.
      //
      // [GNU] 嵌套定义: 到达该记录时它自己的分析已经运行 - 包括检查与
      // 标签配对 - 它的函数体不归本函数下降. 在这里遍历会把嵌套函数的
      // goto 与标签节点收集进本函数的集合, 而它自己的 resolve 已经把
      // 这些交给了 codegen.
      return;
    default: check_children(ctx, node); return;
  }
}

static void check_control_flow(LibcpContext* ctx, LibcpNode* body) {
  check_chain(ctx, body);
  check_labels(ctx);
}

static void mark_live(LibcpContext* ctx, LibcpObj* var) {
  // 把可静态内联的函数从调用者出发递归标记为已使用; 只有一个函数
  // 引用过它, 它才需要被发射.
  if (!var->is_function || var->is_live)
    return;
  var->is_live = true;

  for (int i = 0; i < var->refs.len; i++) {
    LibcpObj* fn = find_func(ctx, var->refs.data[i]);
    if (fn)
      mark_live(ctx, fn);
  }
}

// Remove redundant tentative definitions. Two declarations of the same
// name without an initializer are one definition, so exactly one of them
// survives; a tentative definition that has a real definition to attach
// to is dropped. The lookups are hash maps - a scan of the whole list per
// tentative declaration made this quadratic (C6).
//
// 删除多余的暂定定义. 同一名字的两条无初始化器声明属于同一个定义,
// 因此只有其中一个保留; 已有真实定义可依附的暂定定义被丢弃. 查找
// 使用哈希表 - 过去每条暂定声明都扫描整个链表, 使复杂度变成平方
// (C6).
static void scan_globals(LibcpContext* ctx) {
  char real_tag, kept_tag;
  LibcpHashMap reals = {};
  LibcpHashMap kept  = {};

  for (LibcpObj* var = globals; var; var = var->next)
    if (var->is_definition && !var->is_tentative)
      libcp_hashmap_put(ctx, &reals, var->name, &real_tag);

  LibcpObj head;
  LibcpObj* cur = &head;

  for (LibcpObj* var = globals; var; var = var->next) {
    if (!var->is_tentative) {
      cur = cur->next = var;
      continue;
    }

    // A real definition of this name exists, or one tentative
    // definition of it already survived.
    //
    // 该名字已有真实定义, 或已有一个暂定定义被保留.
    if (libcp_hashmap_get(ctx, &reals, var->name) || libcp_hashmap_get(ctx, &kept, var->name))
      continue;

    libcp_hashmap_put(ctx, &kept, var->name, &kept_tag);
    cur = cur->next = var;
  }

  cur->next = NULL;
  globals   = head.next;
}

// Finishes the translated unit: marks the functions reachable from a
// root live and drops the redundant tentative definitions.
//
// 完成翻译单元: 从根出发把可达函数标记为存活, 并删除多余的暂定定义.
static void finalize_globals(LibcpContext* ctx) {
  for (LibcpObj* var = globals; var; var = var->next)
    if (var->is_root)
      mark_live(ctx, var);

  scan_globals(ctx);
}

// Evaluates and registers a run of enum-constant declaration records.
// The parser records each member with its name, its optional explicit
// value expression and the enum type; a member without an explicit
// value continues the running value of the list, exactly as the old
// on-the-spot evaluation did. Each record is consumed (ty_op cleared)
// so that a later walk skips it.
//
// 求值并注册一串枚举常量声明记录. 解析器记录每个成员的名字, 可选的
// 显式值表达式以及枚举类型; 没有显式值的成员延续列表当前的递增值,
// 与过去就地求值的做法完全一致. 每条记录都会被消费 (ty_op 清零),
// 以便后续遍历跳过它.
static void resolve_enum_records(LibcpContext* ctx, LibcpNode* recs) {
  // The walk stops at the first record that is not an enum constant:
  // on the top-level and block chains the run is followed by whatever
  // declaration record comes next, which carries its own type payload.
  //
  // 遍历在遇到第一条不是枚举常量的记录时停止: 在顶层链与块链上,
  // 这串记录之后是下一条声明记录, 那一条自带类型载荷.
  while (recs && recs->kind == ND_ENUM_CONST) {
    LibcpType* ety = recs->ty_op;
    if (!ety) {
      recs = recs->next;
      continue;
    }

    // C11 6.7.2.2 requires every enum constant to be representable as
    // int. The accumulator is int64_t so that an out-of-range value is
    // diagnosed instead of silently truncated - by the explicit
    // expression and by the implicit increment alike.
    //
    // C11 6.7.2.2 要求每个枚举常量都能用 int 表示. 累加器使用 int64_t,
    // 使得超出范围的值被诊断而不是静默截断 - 显式表达式与隐式递增
    // 一视同仁.
    int64_t val = 0;
    while (recs && recs->kind == ND_ENUM_CONST && recs->ty_op == ety) {
      if (recs->lhs) {
        resolve_node(ctx, recs->lhs);
        val = eval(ctx, recs->lhs);
      }
      if (val < INT32_MIN || val > INT32_MAX)
        libcp_error_tok(ctx,
                        recs->lhs ? recs->lhs->tok : recs->tok,
                        "enum constant value is out of range for int");
      recs->val   = val++;
      recs->ty_op = NULL;

      LibcpVarScope* sc = push_scope(ctx, libcp_get_ident(ctx, recs->tok));
      sc->enum_ty       = ety;
      sc->enum_val      = recs->val;
      recs              = recs->next;
    }
  }
}

// Completes the type records the parser's declarator layer could not
// decide. It walks the type a declarator built, so that an inner
// dimension or operand is settled before the outer one wrapped in it -
// the order the parser's own recursion gave. A record is cleared as it is
// consumed, so calling this again costs nothing.
//
// 完成解析器声明符层无法决定的类型记录. 它遍历声明符构建的类型, 使内层
// 维度或操作数先于包住它的外层确定 - 与解析器自身递归的顺序一致.
// 记录一旦被消费就清零, 因此再次调用没有代价.
static void resolve_type(LibcpContext* ctx, LibcpType* ty) {
  if (!ty)
    return;

  switch (ty->kind) {
    case TY_PTR:
      // A pointer's own layout is its own: the size and alignment do not
      // depend on the target, and C lets the target stay incomplete. An
      // aggregate target is deliberately not laid out from here. Doing so
      // would place it while the aggregate that owns this pointer may
      // still be mid-layout, and a target that holds that aggregate by
      // value would then size it from its not-yet-placed members; the
      // point of use resolves the target instead (resolve_member, a
      // sizeof/alignof operand, a dereference). An array or function
      // target still has to be settled: it carries an element size and a
      // pending dimension or parameter types of its own.
      //
      // 指针自身的布局独立: 大小与对齐不依赖目标类型, 而且 C 允许目标
      // 类型保持不完整. 有意不从这里布局聚合目标: 那样会在拥有该指针的
      // 聚合仍处于布局中途时放置目标, 而按值持有该聚合的目标会用它尚未
      // 放置的成员来定大小; 改由使用点解析目标 (resolve_member,
      // sizeof/alignof 操作数, 解引用). 数组或函数目标仍须确定: 它自带
      // 元素大小以及待定的维度或参数类型.
      if (ty->base && ty->base->kind != TY_STRUCT && ty->base->kind != TY_UNION)
        resolve_type(ctx, ty->base);
      break;
    case TY_VLA: resolve_type(ctx, ty->base); break;
    case TY_ARRAY:
      resolve_type(ctx, ty->base);
      // An array the parser built on a not-yet-laid-out aggregate was
      // sized with placeholder values; recompute now that the element
      // type is complete. (A pending dimension recomputes below.)
      //
      // 解析器在尚未布局的聚合之上构建的数组曾用占位值定大小;
      // 元素类型已完整, 现在重新计算. (待定维度在下面重算.)
      if (!ty->dim_len && ty->array_len >= 0) {
        ty->size  = (ty->base->size < 0) ? -1 : ty->base->size * ty->array_len;
        ty->align = ty->base->align;
      }
      break;
    case TY_STRUCT:
    case TY_UNION:
      // The parser records the member list; placing the members is
      // sema's job, done on first sight of the completed type. An
      // aggregate's own members are placed before an outer one consumes
      // their sizes, because the member types are resolved first.
      //
      // A definition still waiting for its members to be placed has size
      // 0; one that is only forward-declared has size -1 and nothing to
      // place. The flag is left set in that case so the completion of the
      // tag (a later definition) still gets its layout.
      //
      // 解析器记录成员链表; 放置成员是 sema 的工作, 在首次见到完整类型时
      // 完成. 聚合自身的成员先于外层消费它们的大小而放置, 因为成员类型
      // 先被解析.
      //
      // 仍在等待放置成员的完整定义大小为 0; 仅前向声明的类型大小为 -1,
      // 没有可放置的成员. 后一种情况下标志保持置位, 以便稍后的定义
      // 补全该 tag 时仍能获得布局.
      if (ty->layout_pending && ty->size >= 0) {
        ty->layout_pending = false;
        if (ty->kind == TY_STRUCT)
          layout_struct(ctx, ty);
        else
          layout_union(ctx, ty);
      }
      break;
    case TY_FUNC:
      // Enum constants a parameter's declspec defined ride on the
      // function type; they become visible when the type is completed.
      //
      // 参数声明说明符中定义的枚举常量附着在函数类型上; 它们在类型
      // 完成时变为可见.
      if (ty->spec_decls) {
        resolve_enum_records(ctx, ty->spec_decls);
        ty->spec_decls = NULL;
      }
      resolve_type(ctx, ty->return_ty);
      for (LibcpType* param = ty->params; param; param = param->next)
        resolve_type(ctx, param);
      break;
    default: break;
  }

  if (ty->align_expr) {
    ty->align      = (int)eval(ctx, ty->align_expr);
    ty->align_expr = NULL;
  }

  if (ty->typeof_expr) {
    LibcpNode* expr = ty->typeof_expr;
    ty->typeof_expr = NULL;

    // The operand's type fills this record in rather than replacing it:
    // the parser has already built the rest of the declarator's types on
    // top of it, and a per-parameter name already attached to this
    // record must survive the replacement (F1).
    //
    // 操作数的类型填充这条记录, 而不是替换它: 解析器已经在其上构建了
    // 声明符其余的类型, 而且已经附加到该记录上的参数名必须在替换后
    // 保留 (F1).
    libcp_add_type(ctx, expr);
    resolve_type(ctx, expr->ty);

    LibcpToken* name     = ty->name;
    LibcpToken* name_pos = ty->name_pos;
    *ty                  = *expr->ty;
    ty->name             = name;
    ty->name_pos         = name_pos;
  }

  if (ty->dim_len) {
    LibcpNode* dim = ty->dim_len;
    ty->dim_len    = NULL;

    // A dimension that is not a constant expression, like the base of a
    // VLA, gives a variable-length array.
    //
    // 不是常量表达式的维度 (与 VLA 的基础类型一样) 产生变长数组.
    if (ty->base->kind == TY_VLA || !is_const_expr(ctx, dim)) {
      ty->kind    = TY_VLA;
      ty->vla_len = dim;
      ty->size    = 8;
      ty->align   = 8;
    } else {
      ty->array_len = (int)eval(ctx, dim);
      ty->size      = ty->base->size * ty->array_len;
      ty->align     = ty->base->align;
    }
  }
}

// Walks a type's pending records and resolves the expression nodes
// hanging off them (array dimensions, typeof operands, alignments,
// bitfield widths) without evaluating anything: the names inside get
// bound and the declaration records inside (a statement expression can
// appear in a constant context) get declared, so that the evaluations
// resolve_type performs afterwards find a complete picture.
//
// 遍历类型的待定记录, 解析挂在它们上面的表达式节点 (数组维度, typeof
// 操作数, 对齐, 位域宽度) 但不求值: 其中的名字被绑定, 其中的声明记录
// (常量上下文中可以出现语句表达式) 被声明, 使 resolve_type 随后执行的
// 求值面对完整的信息.
static void resolve_type_exprs(LibcpContext* ctx, LibcpType* ty) {
  if (!ty)
    return;

  switch (ty->kind) {
    case TY_PTR:
      // A pointer's target is settled at its point of use, exactly as
      // resolve_type states. Descending into an aggregate target here
      // would resolve its member dimensions at the position of whichever
      // declaration mentioned the pointer first, before the constants
      // they name are in scope.
      //
      // 指针的目标在使用点确定, 与 resolve_type 的规则完全一致. 在这里
      // 下钻到聚合目标会让其成员的维度在最早提及该指针的那条声明的位置
      // 解析, 早于它们命名的常量进入作用域.
      if (ty->base && ty->base->kind != TY_STRUCT && ty->base->kind != TY_UNION)
        resolve_type_exprs(ctx, ty->base);
      break;
    case TY_VLA: resolve_type_exprs(ctx, ty->base); break;
    case TY_ARRAY:
      resolve_type_exprs(ctx, ty->base);
      resolve_node(ctx, ty->dim_len);
      break;
    case TY_FUNC:
      resolve_type_exprs(ctx, ty->return_ty);
      for (LibcpType* param = ty->params; param; param = param->next)
        resolve_type_exprs(ctx, param);
      break;
    case TY_STRUCT:
    case TY_UNION:
      // Layout resolves the member types itself; walking them here would
      // recurse forever on a self-referential member. The pending flag
      // bounds the recursion exactly as it does for the layout.
      //
      // 布局会自行解析成员类型; 在这里遍历它们会在自引用成员上无限递归.
      // 待定标志为递归设界, 与布局时的做法完全相同.
      if (ty->layout_pending) {
        ty->layout_pending = false;
        for (LibcpMember* mem = ty->members; mem; mem = mem->next) {
          resolve_type_exprs(ctx, mem->ty);
          resolve_node(ctx, mem->width_expr);
          resolve_node(ctx, mem->align_expr);
          resolve_type_exprs(ctx, mem->align_ty);
        }
        ty->layout_pending = true;
      }
      break;
    default: break;
  }

  resolve_node(ctx, ty->align_expr);
  resolve_node(ctx, ty->typeof_expr);
}

// The alignment a declaration's `_Alignas` asks for. The parser records
// both forms: the `_Alignas(type)` one as a type whose alignment is
// read once the type is complete, and the `_Alignas(expr)` one as a
// constant expression to evaluate. The value is cached, because one
// declaration declares several objects and each of them asks.
//
// 声明的 `_Alignas` 所要求的对齐. 解析器记录两种形式: `_Alignas(type)`
// 记录为类型, 在类型完整后读取其对齐; `_Alignas(expr)` 记录为待求值的
// 常量表达式. 值会被缓存, 因为一条声明会声明多个对象, 每个对象都会问.
static int attr_align(LibcpContext* ctx, LibcpVarAttr* attr) {
  if (!attr)
    return 0;
  if (attr->align_expr) {
    attr->align      = (int)eval(ctx, attr->align_expr);
    attr->align_expr = NULL;
    return attr->align;
  }
  if (attr->align_ty) {
    resolve_type(ctx, attr->align_ty);
    attr->align    = attr->align_ty->align;
    attr->align_ty = NULL;
    return attr->align;
  }
  return attr->align;
}

static int align_down(int n, int align) {
  // 把 n 向下取整到 align 的倍数 (位域偏移按字节对齐时使用).
  return libcp_align_to(n - align + 1, align);
}

// Completes the member records before they are placed: a member type
// may still carry parser stashes (a dimension, a typeof operand), and
// the `_Alignas` forms are settled here. A trailing incomplete-array
// member becomes the zero-sized flexible array member.
//
// 在放置成员之前补全成员记录: 成员类型可能仍带有解析器的暂存内容
// (维度, typeof 操作数), `_Alignas` 的各种形式也在这里确定. 末尾的
// 不完整数组成员变成零大小的柔性数组成员.
static void finalize_members(LibcpContext* ctx, LibcpType* ty) {
  for (LibcpMember* mem = ty->members; mem; mem = mem->next) {
    resolve_type_exprs(ctx, mem->ty);
    resolve_type(ctx, mem->ty);

    if (mem->align_ty) {
      resolve_type(ctx, mem->align_ty);
      mem->align    = mem->align_ty->align;
      mem->align_ty = NULL;
    }
    if (mem->align_expr) {
      mem->align      = (int)eval(ctx, mem->align_expr);
      mem->align_expr = NULL;
    }
    if (!mem->align)
      mem->align = mem->ty->align;
  }

  // If the last element is an array of incomplete type, it's
  // called a "flexible array member". It should behave as if
  // if were a zero-sized array.
  //
  // 若最后一个成员是不完整类型的数组, 它被称为 "柔性数组成员".
  // 它的行为应等同于零大小数组.
  LibcpMember* last = NULL;
  for (LibcpMember* mem = ty->members; mem; mem = mem->next)
    last = mem;
  if (last && last->ty->kind == TY_ARRAY && last->ty->array_len < 0) {
    last->ty        = libcp_array_of(ctx, last->ty->base, 0);
    ty->is_flexible = true;
  }
}

// Evaluate the recorded bitfield width expressions of a member list.
// The parser records the width unevaluated; layout is where the value
// is first needed.
//
// 求值成员链表中记录的位域宽度表达式. 解析器记录未求值的宽度;
// 布局是首次需要该值的地方.
static void eval_bitfield_widths(LibcpContext* ctx, LibcpType* ty) {
  for (LibcpMember* mem = ty->members; mem; mem = mem->next) {
    if (mem->width_expr) {
      // C11 6.7.2.1p4: the width is a nonnegative integer constant
      // expression that does not exceed the width of the member's type.
      // The mask builders below shift by this value, so an unchecked
      // width is both UB and a silent miscompile.
      //
      // C11 6.7.2.1p4: 宽度是不超过成员类型位宽的非负整数常量表达式.
      // 下面的掩码构建会按该值移位, 因此未检查的宽度既会引发 UB,
      // 也会造成静默的错误编译.
      if (!libcp_is_integer(mem->ty))
        libcp_error_tok(ctx, mem->tok, "bit-field has non-integer type");

      int64_t width = eval(ctx, mem->width_expr);
      if (width < 0)
        libcp_error_tok(ctx, mem->width_expr->tok, "negative width in bit-field");
      if (width > mem->ty->size * 8)
        libcp_error_tok(ctx, mem->width_expr->tok, "width of bit-field exceeds its type");

      mem->bit_width  = width;
      mem->width_expr = NULL;
    }
  }
}

// Assigns an offset to every member of a struct and computes its size
// and alignment. The parser builds the member list (the syntax shape)
// and marks it complete; resolve_type calls this on first sight, and
// nested aggregates are laid out inner-first by the resolve order.
//
// 为 struct 的每个成员分配偏移, 并计算其大小与对齐. 解析器构建成员链表
// (语法形状) 并标记完成; resolve_type 在首次见到时调用这里, 嵌套聚合
// 按 resolve 顺序由内向外完成布局.
static void layout_struct(LibcpContext* ctx, LibcpType* ty) {
  // An `aligned` attribute on the type is a constant expression the
  // parser recorded; it has to be settled before the offsets are.
  //
  // 类型上的 `aligned` 属性是解析器记录的常量表达式; 必须在计算偏移
  // 之前确定.
  resolve_type(ctx, ty);

  // Defensive: an incomplete type has no members to place.
  //
  // 防御性检查: 不完整类型没有可放置的成员.
  if (ty->size < 0)
    return;

  finalize_members(ctx, ty);
  eval_bitfield_widths(ctx, ty);

  int bits = 0;

  for (LibcpMember* mem = ty->members; mem; mem = mem->next) {
    if (mem->is_bitfield && mem->bit_width == 0) {
      // Zero-width anonymous bitfield has a special meaning.
      // It affects only alignment.
      //
      // 零宽匿名位域有特殊含义: 它只影响对齐.
      bits = libcp_align_to(bits, mem->ty->size * 8);
    } else if (mem->is_bitfield) {
      int sz = mem->ty->size;
      if (bits / (sz * 8) != (bits + mem->bit_width - 1) / (sz * 8))
        bits = libcp_align_to(bits, sz * 8);

      mem->offset     = align_down(bits / 8, sz);
      mem->bit_offset = bits % (sz * 8);
      bits += mem->bit_width;
    } else {
      if (!ty->is_packed)
        bits = libcp_align_to(bits, mem->align * 8);
      mem->offset = bits / 8;
      bits += mem->ty->size * 8;
    }

    if (!ty->is_packed && ty->align < mem->align)
      ty->align = mem->align;
  }

  ty->size = libcp_align_to(bits, ty->align * 8) / 8;
}

// Unions need no member offsets (they are all zero), only the union
// of the member sizes and the largest member alignment.
//
// union 不需要成员偏移 (它们全为零), 只需要成员大小的最大值与最大的
// 成员对齐.
static void layout_union(LibcpContext* ctx, LibcpType* ty) {
  resolve_type(ctx, ty);

  if (ty->size < 0)
    return;

  finalize_members(ctx, ty);
  eval_bitfield_widths(ctx, ty);

  for (LibcpMember* mem = ty->members; mem; mem = mem->next) {
    if (ty->align < mem->align)
      ty->align = mem->align;
    if (ty->size < mem->ty->size)
      ty->size = mem->ty->size;
  }
  ty->size = libcp_align_to(ty->size, ty->align);
}

// Binds an unresolved name. A variable or function reference becomes
// ND_VAR; an enum constant becomes ND_NUM.
//
// 绑定未解析的名字. 变量或函数引用变成 ND_VAR; 枚举常量变成 ND_NUM.
static void bind_ident(LibcpContext* ctx, LibcpNode* node) {
  LibcpToken* tok   = node->tok;
  LibcpVarScope* sc = find_var(ctx, tok);
  LibcpObj* var     = sc ? sc->var : NULL;

  if (var) {
    // For "static inline" functions, record the reference so that the
    // liveness analysis can tell which ones are actually needed.
    //
    // 对 "static inline" 函数记录引用, 使存活分析能分辨哪些真正被需要.
    if (var->is_function) {
      if (sema_fn)
        libcp_strarray_push(ctx, &sema_fn->refs, var->name);
      else
        var->is_root = true;
    }

    node->kind = ND_VAR;
    node->var  = var;
    node->ty   = var->ty;
    return;
  }

  if (sc && sc->enum_ty) {
    node->kind = ND_NUM;
    node->val  = sc->enum_val;
    node->ty   = libcp_ty_int;
    return;
  }

  if (libcp_equal(tok->next, "("))
    libcp_error_tok(ctx, tok, "implicit declaration of a function");
  libcp_error_tok(ctx, tok, "undefined variable");
}

static LibcpObj* new_var(LibcpContext* ctx, char* name, LibcpType* ty) {
  // 创建一个对象, 完成其类型, 并在当前作用域登记名字.
  LibcpObj* var = libcp_arena_calloc(ctx, 1, sizeof(LibcpObj));
  var->name     = name;
  var->ty       = ty;
  resolve_type(ctx, ty);
  var->align                 = ty->align;
  push_scope(ctx, name)->var = var;
  return var;
}

static LibcpObj* new_lvar(LibcpContext* ctx, char* name, LibcpType* ty) {
  // 创建局部变量, 并挂到当前函数的局部变量链表头部.
  LibcpObj* var = new_var(ctx, name, ty);
  var->is_local = true;
  var->next     = locals;
  locals        = var;
  return var;
}

// The type of a named object: a per-object copy carrying its name, made
// after resolve_type completed the type so the flexible and
// array-completion flags are final. Shared builtin, tag and typedef
// records are never written (F1). A VLA record is the identity codegen
// keys its runtime size on, and a typedef'd VLA is shared by the
// declarations that use it on purpose (A10.2), so it is returned as is;
// its name slot stays unset.
//
// 具名对象的类型: 携带其名字的逐对象副本, 在 resolve_type 完成类型之后
// 制作, 此时柔性数组与数组补全标志已最终确定. 共享的内建, tag 与
// typedef 记录永不被写入 (F1). VLA 记录是 codegen 用来索引运行时大小
// 的身份, typedef 的 VLA 被使用它的声明有意共享 (A10.2), 因此原样
// 返回; 其名字槽保持未设置.
static LibcpType* named_type(LibcpContext* ctx, LibcpType* ty, LibcpToken* name_tok) {
  if (ty->kind == TY_VLA)
    return ty;

  LibcpType* copy = libcp_copy_type(ctx, ty);
  copy->name      = name_tok;
  return copy;
}

static LibcpObj* new_gvar(LibcpContext* ctx, char* name, LibcpType* ty) {
  // 创建全局变量, 并挂到全局变量链表头部; 默认是 static 定义.
  LibcpObj* var      = new_var(ctx, name, ty);
  var->next          = globals;
  var->is_static     = true;
  var->is_definition = true;
  globals            = var;
  return var;
}

// A built-in declaration is registered in the file scope like any other
// object, but it is not linked into the globals list: codegen never
// emits it, and leaving it out keeps that list the plain result of the
// current TU instead of something libcp_sema() has to detach afterwards.
//
// 内建声明像其他对象一样登记在文件作用域, 但不接入 globals 链表:
// codegen 从不发射它, 把它排除在外可以让该链表保持为当前翻译单元的
// 纯粹结果, 而不需要 libcp_sema() 事后摘除.
static LibcpObj* new_builtin_gvar(LibcpContext* ctx, char* name, LibcpType* ty) {
  LibcpObj* var      = new_var(ctx, name, ty);
  var->is_static     = true;
  var->is_definition = true;
  return var;
}

// The anonymous names of hidden objects - string literal globals,
// static locals and the control-flow labels sema allocates - come
// from this one counter.
// The one name factory for the objects the compiler names on its
// own: string-literal globals, block-scope statics, compound
// literals and control-flow labels all draw from this counter, so
// their numbers interleave in creation order - and .data block
// order is creation order, which is why these allocations stay on
// the sema side even when labels move. The format is load-bearing:
// the snapshot normalizer renumbers labels whose suffix is exactly
// ".<digits>", so a prefix change shows up as a diff even when
// only the numbering moved. A2.1 moves label allocation to codegen
// against this same namespace (one counter), not a parallel one.
//
// 隐藏对象的匿名名字 - 字符串字面量全局对象, static 局部变量以及
// sema 分配的控制流标签 - 都来自这一个计数器.
// 编译器自行命名的对象只有一个名字工厂: 字符串字面量全局对象,
// 块作用域 static, 复合字面量与控件流标签都从这个计数器取值, 因此
// 它们的编号按创建顺序交错 - 而 .data 块顺序就是创建顺序, 这也是
// 即使标签分配搬走, 这些分配仍留在 sema 一侧的原因. 格式是关键:
// 快照规范化器只重新编号后缀恰好为 ".<digits>" 的标签, 因此前缀变化
// 即使编号没动也会表现为 diff. A2.1 把标签分配移到 codegen, 但使用
// 同一个命名空间 (同一个计数器), 而不是并行的另一个.
#define unique_name_id (ctx->unique_name_id)

static char* new_unique_name(LibcpContext* ctx) {
  return libcp_format(ctx, ".L..%d", unique_name_id++);
}

// Read-only peek at the counter: a consumer that allocates its own
// `.L..%d` labels (codegen's shaping pass) continues from here.
//
// 对计数器的只读窥视: 自行分配 `.L..%d` 标签的消费者 (codegen 的
// shaping pass) 从这里继续编号.
int libcp_unique_name_next(LibcpContext* ctx) {
  return unique_name_id;
}

static LibcpObj* new_anon_gvar(LibcpContext* ctx, LibcpType* ty) {
  // 为隐藏对象创建一个匿名全局对象.
  return new_gvar(ctx, new_unique_name(ctx), ty);
}

static LibcpObj* new_string_literal(LibcpContext* ctx, char* p, LibcpType* ty) {
  // 为字符串字面量创建一个匿名全局对象, 数据直接指向字面量内容.
  LibcpObj* var  = new_anon_gvar(ctx, ty);
  var->init_data = p;
  return var;
}

static void create_param_lvars(LibcpContext* ctx, LibcpType* param) {
  // 递归到参数链表末尾, 再依次创建局部变量, 保持参数顺序; 参数名缺失
  // 时报错.
  if (param) {
    create_param_lvars(ctx, param->next);
    if (!param->name)
      libcp_error_tok(ctx, param->name_pos, "parameter name omitted");
    new_lvar(ctx, libcp_get_ident(ctx, param->name), param);
  }
}

// Sets up the variables a function definition owns: the parameters
// (including the hidden buffer for a large struct/union return value),
// the __va_area__ and __alloca_size__ helpers, and the __func__ /
// __FUNCTION__ strings. The resolve pass calls this once it reaches the
// function body, so that everything is created in the same order as
// before.
//
// 建立函数定义拥有的变量: 参数 (包括大 struct/union 返回值所用的隐藏
// 缓冲区), __va_area__ 与 __alloca_size__ 辅助变量, 以及 __func__ /
// __FUNCTION__ 字符串. resolve pass 在到达函数体时调用一次, 使所有
// 内容的创建顺序与过去一致.
static void begin_function(LibcpContext* ctx, LibcpObj* fn, LibcpType* ty) {
  locals = NULL;
  create_param_lvars(ctx, ty->params);

  // A buffer for a struct/union return value is passed
  // as the hidden first parameter.
  //
  // 大 struct/union 返回值的缓冲区作为隐藏的第一个参数传入.
  LibcpType* rty = ty->return_ty;
  if ((rty->kind == TY_STRUCT || rty->kind == TY_UNION) && rty->size > 16)
    new_lvar(ctx, "", libcp_pointer_to(ctx, rty));

  fn->params = locals;

  if (ty->is_variadic)
    fn->va_area = new_lvar(ctx, "__va_area__", libcp_array_of(ctx, libcp_ty_char, 136));
  fn->alloca_bottom = new_lvar(ctx, "__alloca_size__", libcp_pointer_to(ctx, libcp_ty_char));

  // [https://www.sigbus.info/n1570#6.4.2.2p1] "__func__" is
  // automatically defined as a local variable containing
  // the current function name.
  //
  // [https://www.sigbus.info/n1570#6.4.2.2p1] "__func__" 被自动定义为
  // 包含当前函数名的局部变量.
  push_scope(ctx, "__func__")->var =
      new_string_literal(ctx, fn->name, libcp_array_of(ctx, libcp_ty_char, strlen(fn->name) + 1));

  // [GNU] __FUNCTION__ is yet another name of __func__.
  //
  // [GNU] __FUNCTION__ 是 __func__ 的另一个名字.
  push_scope(ctx, "__FUNCTION__")->var =
      new_string_literal(ctx, fn->name, libcp_array_of(ctx, libcp_ty_char, strlen(fn->name) + 1));
}

static void declare_builtin_functions(LibcpContext* ctx) {
  // 声明内建 alloca: 它是只有声明没有定义的对象, 供 VLA 降级调用.
  LibcpType* ty                 = libcp_func_type(ctx, libcp_pointer_to(ctx, libcp_ty_void));
  ty->params                    = libcp_copy_type(ctx, libcp_ty_int);
  builtin_alloca                = new_builtin_gvar(ctx, "alloca", ty);
  builtin_alloca->is_definition = false;
}

// Build the `alloca(<size>)` call node for a VLA declaration. The call
// arrives fully typed, so libcp_add_type skips it and codegen recognizes the
// builtin by name.
//
// 为 VLA 声明构建 `alloca(<size>)` 调用节点. 该调用到达时已完全定型,
// 因此 libcp_add_type 会跳过它, codegen 按名字识别这个内建.
LibcpNode* libcp_new_alloca(LibcpContext* ctx, LibcpNode* sz) {
  LibcpNode* node =
      libcp_new_unary(ctx, ND_FUNCALL, libcp_new_var_node(ctx, builtin_alloca, sz->tok), sz->tok);
  node->func_ty = builtin_alloca->ty;
  node->ty      = builtin_alloca->ty->return_ty;
  node->args    = sz;
  libcp_add_type(ctx, sz);
  return node;
}

// Builds a binary arithmetic node out of operands a lowering has
// already settled on: the usual arithmetic conversions are applied and
// the node comes back fully typed, so libcp_add_type has nothing left to do
// with it. This is what makes new_add/new_sub total - every `+`/`-` in
// the tree, whether the parser wrote it or sema rebuilt it from a
// compound assignment or a subscript, is scaled and converted exactly
// once, and no node needs a flag saying which of the two it had.
//
// Exported: this is how a consumer obtains a typed binary node, and with
// it sema's conversion decisions, without re-implementing either.
//
// 用降级已经定好的操作数构建二元算术节点: 施加常规算术转换后节点
// 返回时已完全定型, libcp_add_type 对它无事可做. 正是这一点让
// new_add/new_sub 完备 - 树中的每个 `+`/`-`, 无论来自解析器还是 sema
// 从复合赋值或下标重建, 都恰好被缩放和转换一次, 节点也不需要标志
// 说明自己属于哪一种.
//
// 已导出: 消费者由此获得已定型的二元节点, 以及 sema 的转换决策,
// 无需重新实现其中任何一项.
LibcpNode* libcp_new_arith(LibcpContext* ctx,
                           LibcpNodeKind kind,
                           LibcpNode* lhs,
                           LibcpNode* rhs,
                           LibcpToken* tok) {
  libcp_add_type(ctx, lhs);
  libcp_add_type(ctx, rhs);
  libcp_usual_arith_conv(ctx, &lhs, &rhs);

  LibcpNode* node = libcp_new_binary(ctx, kind, lhs, rhs, tok);
  node->ty        = lhs->ty;
  return node;
}

// Turns a faithful call node into the shape codegen expects: the callee
// must be a function or a pointer to one, each argument is converted to
// its parameter type (an argument past the parameter list is promoted
// instead, since it belongs to a variadic tail), and the result type is
// recorded - a struct or union one tells the consumer to allocate the
// buffer the call writes into. `tok` is the call's closing paren, where
// the argument-count diagnostics are anchored.
//
// 把忠实调用节点变成 codegen 期望的形状: 被调者必须是函数或指向函数的
// 指针, 每个实参转换到其参数类型 (超出参数列表的实参改为提升, 因为它
// 属于可变参数尾部), 并记录结果类型 - struct 或 union 结果类型告诉
// 消费者要分配调用写入的缓冲区. `tok` 是调用的右括号, 实参个数诊断
// 锚定在那里.
static void lower_funcall(LibcpContext* ctx, LibcpNode* node, LibcpToken* tok) {
  LibcpNode* fn = node->lhs;
  LibcpType* ty = fn->ty;

  if (ty->kind != TY_FUNC && (ty->kind != TY_PTR || ty->base->kind != TY_FUNC))
    libcp_error_tok(ctx, fn->tok, "not a function");

  if (ty->kind != TY_FUNC)
    ty = ty->base;

  node->func_ty = ty;

  LibcpType* param_ty = ty->params;
  LibcpNode head      = {};
  LibcpNode* cur      = &head;

  for (LibcpNode* arg = node->args; arg;) {
    LibcpNode* next = arg->next;
    libcp_add_type(ctx, arg);

    if (!param_ty && !ty->is_variadic)
      libcp_error_tok(ctx, tok, "too many arguments");

    if (param_ty) {
      if (param_ty->kind != TY_STRUCT && param_ty->kind != TY_UNION)
        arg = libcp_new_cast(ctx, arg, param_ty);
      param_ty = param_ty->next;
    } else if (arg->ty->kind == TY_FLOAT) {
      // If parameter type is omitted (e.g. in "..."), float
      // arguments are promoted to double.
      //
      // 若省略参数类型 (例如在 "..." 中), float 实参提升为 double.
      arg = libcp_new_cast(ctx, arg, libcp_ty_double);
    }

    arg->next = NULL;
    cur = cur->next = arg;
    arg             = next;
  }

  if (param_ty)
    libcp_error_tok(ctx, tok, "too few arguments");

  node->args = head.next;
  node->ty   = ty->return_ty;

  // A struct or union return value gets the caller-owned buffer, but
  // the slot itself is the consumer's (PLAN A6.1): codegen's shaping
  // pass creates it before the frame layout is assigned. What stays
  // here is the conclusion - the return type - which the pass reads.
  //
  // struct 或 union 返回值使用调用者拥有的缓冲区, 但槽位本身属于消费者
  // (PLAN A6.1): codegen 的 shaping pass 在分配栈帧布局之前创建它.
  // 留在这里的是结论 - 返回类型 - 供该 pass 读取.
}

static LibcpInitTree* new_resolved_init(LibcpContext* ctx, LibcpType* ty, bool is_flexible) {
  // 按目标类型构建初始化的槽位树: 数组与聚合递归展开, 柔性数组/柔性
  // 成员留下可扩展的叶子.
  LibcpInitTree* init = libcp_arena_calloc(ctx, 1, sizeof(LibcpInitTree));
  init->ty            = ty;

  if (ty->kind == TY_ARRAY) {
    if (is_flexible && ty->size < 0) {
      init->is_flexible = true;
      return init;
    }

    init->children = libcp_arena_calloc(ctx, ty->array_len, sizeof(LibcpInitTree*));
    for (int i = 0; i < ty->array_len; i++)
      init->children[i] = new_resolved_init(ctx, ty->base, false);
    return init;
  }

  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
    // Count the number of struct members.
    // 统计 struct 成员个数, 用于分配子槽位数组.
    int len = 0;
    for (LibcpMember* mem = ty->members; mem; mem = mem->next)
      len++;

    init->children = libcp_arena_calloc(ctx, len, sizeof(LibcpInitTree*));

    for (LibcpMember* mem = ty->members; mem; mem = mem->next) {
      if (is_flexible && ty->is_flexible && !mem->next) {
        LibcpInitTree* child     = libcp_arena_calloc(ctx, 1, sizeof(LibcpInitTree));
        child->ty                = mem->ty;
        child->is_flexible       = true;
        init->children[mem->idx] = child;
      } else {
        init->children[mem->idx] = new_resolved_init(ctx, mem->ty, false);
      }
    }
    return init;
  }

  return init;
}

static LibcpType* copy_struct_type(LibcpContext* ctx, LibcpType* ty) {
  // 深拷贝聚合类型及其成员链表, 供柔性数组成员补全后产生新类型.
  ty = libcp_copy_type(ctx, ty);

  LibcpMember head = {};
  LibcpMember* cur = &head;
  for (LibcpMember* mem = ty->members; mem; mem = mem->next) {
    LibcpMember* m = libcp_arena_calloc(ctx, 1, sizeof(LibcpMember));
    *m             = *mem;
    cur = cur->next = m;
  }

  ty->members = head.next;
  return ty;
}

// Evaluate a recorded designator index expression.
// 求值记录下来的指示符索引表达式.
static int eval_desig_index(LibcpContext* ctx, LibcpNode* expr) {
  return eval(ctx, expr);
}

// Evaluate an array designator and run its bounds checks against the
// array being designated. Anchors mirror the old parse-time checks:
// the begin bound reports at the token following the begin expression,
// the end bound and the empty range at `]`.
//
// 求值数组指示符, 并针对被指示的数组做边界检查. 锚点与旧的解析期检查
// 一致: 起始上界在起始表达式之后的 token 处报告, 结束上界与空区间在
// `]` 处报告.
static void
    eval_array_desig(LibcpContext* ctx, LibcpInitDesig* d, LibcpType* ty, int* begin, int* end) {
  *begin = eval_desig_index(ctx, d->begin);
  if (*begin >= ty->array_len)
    libcp_error_tok(ctx, d->after_begin, "array designator index exceeds array bounds");

  if (d->end) {
    *end = eval_desig_index(ctx, d->end);
    if (*end >= ty->array_len)
      libcp_error_tok(ctx, d->rbracket, "array designator index exceeds array bounds");
    if (*end < *begin)
      libcp_error_tok(ctx, d->rbracket, "array designator range [%d, %d] is empty", *begin, *end);
  } else {
    *end = *begin;
  }
}

// Find the member a `.name` designator refers to. If the name lives
// inside an anonymous struct member, that (nameless) member is
// returned and the caller re-applies the designator to its subtree.
//
// 查找 `.name` 指示符所指的成员. 若名字位于匿名 struct 成员内部,
// 则返回那个 (无名的) 成员, 调用者会把指示符重新施加到它的子树上.
static LibcpMember* find_init_member(LibcpContext* ctx, LibcpType* ty, LibcpToken* name) {
  for (LibcpMember* mem = ty->members; mem; mem = mem->next) {
    // Anonymous struct or union member
    // 匿名 struct 或 union 成员
    if ((mem->ty->kind == TY_STRUCT || mem->ty->kind == TY_UNION) && !mem->name) {
      if (get_struct_member(mem->ty, name))
        return mem;
      continue;
    }

    // A nameless member (e.g. an anonymous zero-width bitfield) takes
    // part in layout but can never be designated.
    //
    // 无名成员 (例如匿名零宽位域) 参与布局, 但永远不能被指示.
    if (!mem->name)
      continue;

    // Regular struct member
    // 普通 struct 成员
    if (mem->name->len == name->len && !strncmp(mem->name->loc, name->loc, name->len))
      return mem;
  }

  libcp_error_tok(ctx, name, "struct has no such member");
}

// Turn a recorded string literal into an expression node referencing
// its anonymous global, which is what a string initializer for a
// non-array target used to parse into.
//
// 把记录下来的字符串字面量变成引用其匿名全局对象的表达式节点, 这正是
// 过去非数组目标的字符串初始化器解析出的形态.
static LibcpNode* materialize_str(LibcpContext* ctx, LibcpInitializer* rec) {
  LibcpNode* node = libcp_new_node(ctx, ND_STRING, rec->str_tok);
  libcp_add_type(ctx, node);
  return node;
}

// Expand a string literal into the elements of a character array,
// one character (of the element width) per element.
//
// 把字符串字面量展开为字符数组的各个元素, 每个元素一个字符 (按元素
// 宽度读取).
static void resolve_string(LibcpContext* ctx, LibcpInitializer* rec, LibcpInitTree* init) {
  LibcpToken* tok = rec->str_tok;
  int len         = MIN(init->ty->array_len, tok->ty->array_len);

  switch (init->ty->base->size) {
    case 1: {
      char* str = tok->str;
      for (int i = 0; i < len; i++)
        init->children[i]->expr = libcp_new_num(ctx, str[i], tok);
      break;
    }
    case 2: {
      uint16_t* str = (uint16_t*)tok->str;
      for (int i = 0; i < len; i++)
        init->children[i]->expr = libcp_new_num(ctx, str[i], tok);
      break;
    }
    case 4: {
      uint32_t* str = (uint32_t*)tok->str;
      for (int i = 0; i < len; i++)
        init->children[i]->expr = libcp_new_num(ctx, str[i], tok);
      break;
    }
    default: unreachable();
  }
}

static void resolve_value(LibcpContext* ctx,
                          LibcpInitializer* rec,
                          LibcpInitTree* init,
                          LibcpInitItem** cursor);
static void resolve_desig(LibcpContext* ctx,
                          LibcpInitTree* init,
                          LibcpInitDesig* d,
                          LibcpInitializer* rec,
                          LibcpInitItem** cursor);
static void resolve_array1(LibcpContext* ctx, LibcpInitializer* rec, LibcpInitTree* init);
static void
    resolve_array_cont(LibcpContext* ctx, LibcpInitTree* init, int i, LibcpInitItem** cursor);
static void resolve_struct1(LibcpContext* ctx, LibcpInitializer* rec, LibcpInitTree* init);
static void resolve_struct_cont(LibcpContext* ctx,
                                LibcpInitTree* init,
                                LibcpMember* mem,
                                LibcpInitItem** cursor);
static void resolve_struct_first(LibcpContext* ctx,
                                 LibcpInitTree* init,
                                 LibcpMember* mem,
                                 LibcpInitializer* rec,
                                 LibcpInitItem** cursor);
static void resolve_union(LibcpContext* ctx,
                          LibcpInitializer* rec,
                          LibcpInitTree* init,
                          LibcpInitItem** cursor);
static int count_flex_list(LibcpContext* ctx, LibcpInitItem* items, LibcpType* ty);
static int
    count_flex(LibcpContext* ctx, LibcpInitializer* rec, LibcpInitItem** cursor, LibcpType* ty);

// Consume one braced-list item: apply its designator chain (if any)
// and resolve its value. *cursor advances past the item and any
// siblings the value consumes through brace elision.
//
// 消费一个花括号列表项: 施加其指示符链 (若有) 并解析其值. *cursor 越过
// 该项以及该值通过花括号省略消费掉的兄弟项.
static void resolve_item(LibcpContext* ctx, LibcpInitItem** cursor, LibcpInitTree* init) {
  LibcpInitItem* it = *cursor;
  *cursor           = it->next;
  resolve_desig(ctx, init, it->desigs, it->init, cursor);
}

// Apply a designator chain to a resolved node, then its value. This
// is the record-driven form of the old designation(): `[n]` moves
// within an array, `.name` within a struct or union, and after a
// struct member the following siblings continue with the next members.
//
// 对已解析的节点施加指示符链, 再处理其值. 这是旧 designation() 的记录
// 驱动形式: `[n]` 在数组内移动, `.name` 在 struct 或 union 内移动,
// 处理完一个 struct 成员后, 后续兄弟项接着下一个成员继续.
static void resolve_desig(LibcpContext* ctx,
                          LibcpInitTree* init,
                          LibcpInitDesig* d,
                          LibcpInitializer* rec,
                          LibcpInitItem** cursor) {
  if (!d) {
    resolve_value(ctx, rec, init, cursor);
    return;
  }

  if (!d->name) {
    if (init->ty->kind != TY_ARRAY)
      libcp_error_tok(ctx, d->tok, "array index in non-array initializer");

    int begin, end;
    eval_array_desig(ctx, d, init->ty, &begin, &end);

    for (int j = begin; j <= end; j++)
      resolve_desig(ctx, init->children[j], d->next, rec, cursor);
    resolve_array_cont(ctx, init, begin + 1, cursor);
    return;
  }

  if (init->ty->kind == TY_STRUCT) {
    LibcpMember* mem = find_init_member(ctx, init->ty, d->name);
    if (!mem->name) {
      // Anonymous struct member: re-apply the designator to its subtree.
      // 匿名 struct 成员: 把指示符重新施加到它的子树上.
      resolve_desig(ctx, init->children[mem->idx], d, rec, cursor);
      return;
    }
    resolve_desig(ctx, init->children[mem->idx], d->next, rec, cursor);
    init->expr = NULL;
    resolve_struct_cont(ctx, init, mem->next, cursor);
    return;
  }

  if (init->ty->kind == TY_UNION) {
    LibcpMember* mem = find_init_member(ctx, init->ty, d->name);
    init->mem        = mem;
    if (!mem->name) {
      resolve_desig(ctx, init->children[mem->idx], d, rec, cursor);
      return;
    }
    resolve_desig(ctx, init->children[mem->idx], d->next, rec, cursor);
    return;
  }

  libcp_error_tok(ctx, d->tok, "field name not in struct or union initializer");
}

// Fill array elements from sibling items (brace-elision continuation
// or post-designator continuation), stopping at a designated item or
// when the items run out.
//
// 用兄弟项填充数组元素 (花括号省略的延续, 或指示符之后的延续), 遇到
// 带指示符的项或项耗尽时停止.
static void
    resolve_array_cont(LibcpContext* ctx, LibcpInitTree* init, int i, LibcpInitItem** cursor) {
  for (; i < init->ty->array_len && *cursor && !(*cursor)->desigs; i++)
    resolve_item(ctx, cursor, init->children[i]);
}

// Resolve a braced array initializer, designated items included.
// Elements past the end of the array have no target; each one is
// diagnosed and dropped (C11 6.7.9 constrains the excess).
//
// 解析带花括号的数组初始化器, 包括带指示符的项. 超出数组末尾的元素
// 没有目标; 每一个都会被诊断并丢弃 (C11 6.7.9 限制多余项).
static void resolve_array1(LibcpContext* ctx, LibcpInitializer* rec, LibcpInitTree* init) {
  int i                 = 0;
  LibcpInitItem* cursor = rec->items;

  while (cursor) {
    LibcpInitItem* it = cursor;

    if (it->desigs && !it->desigs->name) {
      int begin, end;
      eval_array_desig(ctx, it->desigs, init->ty, &begin, &end);
      cursor = it->next;
      for (int j = begin; j <= end; j++)
        resolve_desig(ctx, init->children[j], it->desigs->next, it->init, &cursor);
      i = end + 1;
      continue;
    }

    if (i < init->ty->array_len) {
      resolve_item(ctx, &cursor, init->children[i]);
    } else {
      libcp_warn_tok(ctx, it->init->tok, "excess elements in array initializer");
      cursor = it->next;
    }
    i++;
  }
}

// Fill struct members from sibling items, stopping at a designated
// item or when the items run out.
//
// 用兄弟项填充 struct 成员, 遇到带指示符的项或项耗尽时停止.
static void resolve_struct_cont(LibcpContext* ctx,
                                LibcpInitTree* init,
                                LibcpMember* mem,
                                LibcpInitItem** cursor) {
  for (; mem && *cursor && !(*cursor)->desigs; mem = mem->next)
    resolve_item(ctx, cursor, init->children[mem->idx]);
}

// Brace-elision entry for structs: the expression a struct was
// "initialized" with becomes the first member's initializer and the
// siblings continue with the following members.
//
// struct 的花括号省略入口: 用来 "初始化" struct 的表达式成为第一个
// 成员的初始化器, 兄弟项接着后续成员继续.
static void resolve_struct_first(LibcpContext* ctx,
                                 LibcpInitTree* init,
                                 LibcpMember* mem,
                                 LibcpInitializer* rec,
                                 LibcpInitItem** cursor) {
  if (mem) {
    resolve_value(ctx, rec, init->children[mem->idx], cursor);
    mem = mem->next;
  }
  resolve_struct_cont(ctx, init, mem, cursor);
}

// Resolve a braced struct initializer.
// 解析带花括号的 struct 初始化器.
static void resolve_struct1(LibcpContext* ctx, LibcpInitializer* rec, LibcpInitTree* init) {
  LibcpMember* mem      = init->ty->members;
  LibcpInitItem* cursor = rec->items;

  while (cursor) {
    LibcpInitItem* it = cursor;

    if (it->desigs && it->desigs->name) {
      LibcpMember* m = find_init_member(ctx, init->ty, it->desigs->name);
      cursor         = it->next;
      if (m->name)
        resolve_desig(ctx, init->children[m->idx], it->desigs->next, it->init, &cursor);
      else
        resolve_desig(ctx, init->children[m->idx], it->desigs, it->init, &cursor);
      mem = m->next;
      continue;
    }

    if (mem) {
      resolve_item(ctx, &cursor, init->children[mem->idx]);
      mem = mem->next;
    } else {
      cursor = it->next;
    }
  }
}

// Resolve a union initializer. Unlike structs, union initializers
// take only one initializer, and that initializes the first union
// member by default. You can initialize another member using a
// designated initializer.
//
// 解析 union 初始化器. 与 struct 不同, union 初始化器只接受一个
// 初始化器, 默认初始化第一个 union 成员. 可以用指示符初始化另一个
// 成员.
static void resolve_union(LibcpContext* ctx,
                          LibcpInitializer* rec,
                          LibcpInitTree* init,
                          LibcpInitItem** cursor) {
  if (rec->kind == INIT_LIST) {
    LibcpInitItem* it = rec->items;

    if (it && it->desigs && it->desigs->name) {
      LibcpMember* mem    = find_init_member(ctx, init->ty, it->desigs->name);
      init->mem           = mem;
      LibcpInitItem* next = it->next;
      if (mem->name)
        resolve_desig(ctx, init->children[mem->idx], it->desigs->next, it->init, &next);
      else
        resolve_desig(ctx, init->children[mem->idx], it->desigs, it->init, &next);
      if (next)
        libcp_error_tok(ctx, next->comma_tok, "expected '}'");
      return;
    }

    init->mem = init->ty->members;
    if (!it) {
      // The old parser dispatched on the closing brace here; only an
      // array first member came out of `{}` without an error.
      //
      // 旧解析器在这里根据右花括号分派; 只有首成员为数组的情况下
      // `{}` 才不会报错.
      if (init->ty->members->ty->kind != TY_ARRAY)
        libcp_error_tok(ctx, rec->tok->next, "expected an expression");
      return;
    }

    LibcpInitItem* next = it->next;
    resolve_desig(ctx, init->children[0], it->desigs, it->init, &next);
    if (next)
      libcp_error_tok(ctx, next->desigs ? next->desigs->tok : next->init->tok, "expected '}'");
    return;
  }

  init->mem = init->ty->members;
  resolve_value(ctx, rec, init->children[0], cursor);
}

// Count the elements a flexible-array initializer will have so the
// array type can be completed before the tree is built. Items are
// resolved against a throwaway tree, exactly like the old parser's
// counting pass; designator bounds are not checked here (the build
// pass checks them against the completed length).
//
// 统计柔性数组初始化器将有多少个元素, 以便在构建树之前补全数组类型.
// 各项针对一棵一次性的树解析, 与旧解析器的计数 pass 完全一致;
// 这里不检查指示符边界 (构建 pass 会针对补全后的长度检查).
static int count_flex_list(LibcpContext* ctx, LibcpInitItem* items, LibcpType* ty) {
  LibcpInitTree* dummy = new_resolved_init(ctx, ty->base, true);
  int i = 0, max = 0;
  LibcpInitItem* cursor = items;

  while (cursor) {
    LibcpInitItem* it = cursor;
    if (it->desigs && !it->desigs->name) {
      i = eval_desig_index(ctx, it->desigs->begin);
      if (it->desigs->end)
        i = eval_desig_index(ctx, it->desigs->end);
      cursor = it->next;
      resolve_desig(ctx, dummy, it->desigs->next, it->init, &cursor);
    } else {
      resolve_item(ctx, &cursor, dummy);
    }
    i++;
    max = MAX(max, i);
  }
  return max;
}

// The brace-elision form of count_flex_list: the first value is the
// record itself and the siblings follow it.
//
// count_flex_list 的花括号省略形式: 第一个值就是记录本身, 兄弟项在其后.
static int
    count_flex(LibcpContext* ctx, LibcpInitializer* rec, LibcpInitItem** cursor, LibcpType* ty) {
  LibcpInitTree* dummy = new_resolved_init(ctx, ty->base, true);
  int i = 0, max = 0;

  resolve_value(ctx, rec, dummy, cursor);
  i++;
  max = MAX(max, i);

  while (*cursor) {
    LibcpInitItem* it = *cursor;
    if (it->desigs && !it->desigs->name) {
      i = eval_desig_index(ctx, it->desigs->begin);
      if (it->desigs->end)
        i = eval_desig_index(ctx, it->desigs->end);
      *cursor = it->next;
      resolve_desig(ctx, dummy, it->desigs->next, it->init, cursor);
    } else {
      resolve_item(ctx, cursor, dummy);
    }
    i++;
    max = MAX(max, i);
  }
  return max;
}

// Resolve one initializer record against its target slot.
// 针对目标槽位解析一条初始化器记录.
static void resolve_value(LibcpContext* ctx,
                          LibcpInitializer* rec,
                          LibcpInitTree* init,
                          LibcpInitItem** cursor) {
  LibcpType* ty = init->ty;

  if (ty->kind == TY_ARRAY && rec->kind == INIT_STR) {
    if (init->is_flexible)
      *init = *new_resolved_init(ctx,
                                 libcp_array_of(ctx, ty->base, rec->str_tok->ty->array_len),
                                 false);
    resolve_string(ctx, rec, init);
    return;
  }

  if (ty->kind == TY_ARRAY) {
    if (init->is_flexible) {
      // The counting pass walks the siblings without consuming them;
      // the build pass below resolves them for real.
      //
      // 计数 pass 遍历兄弟项但不消费它们; 下面的构建 pass 才真正解析.
      LibcpInitItem* save = *cursor;
      int len             = (rec->kind == INIT_LIST) ? count_flex_list(ctx, rec->items, ty) :
                                                       count_flex(ctx, rec, cursor, ty);
      *cursor             = save;
      *init               = *new_resolved_init(ctx, libcp_array_of(ctx, ty->base, len), false);
    }

    if (rec->kind == INIT_LIST) {
      resolve_array1(ctx, rec, init);
    } else {
      resolve_value(ctx, rec, init->children[0], cursor);
      resolve_array_cont(ctx, init, 1, cursor);
    }
    return;
  }

  if (ty->kind == TY_STRUCT) {
    if (rec->kind == INIT_LIST) {
      resolve_struct1(ctx, rec, init);
      return;
    }

    // A struct can be initialized with another struct. E.g.
    // `struct T x = y;` where y is a variable of type `struct T`.
    // Handle that case first.
    //
    // struct 可以用另一个 struct 初始化, 例如 `struct T x = y;`,
    // 其中 y 是 `struct T` 类型的变量. 先处理这种情况.
    LibcpNode* expr = (rec->kind == INIT_STR) ? materialize_str(ctx, rec) : rec->expr;
    libcp_add_type(ctx, expr);
    if (expr->ty->kind == TY_STRUCT) {
      init->expr = expr;
      return;
    }

    resolve_struct_first(ctx, init, ty->members, rec, cursor);
    return;
  }

  if (ty->kind == TY_UNION) {
    resolve_union(ctx, rec, init, cursor);
    return;
  }

  if (rec->kind == INIT_LIST) {
    // An initializer for a scalar variable can be surrounded by
    // braces. E.g. `int x = {3};`. Handle that case: exactly one
    // element, no designators.
    //
    // 标量变量的初始化器可以用花括号包围, 例如 `int x = {3};`.
    // 处理这种情况: 恰好一个元素, 不带指示符.
    LibcpInitItem* it = rec->items;
    if (!it)
      libcp_error_tok(ctx, rec->tok->next, "expected an expression");
    if (it->desigs)
      libcp_error_tok(ctx, it->desigs->tok, "expected an expression");
    LibcpInitItem* next = it->next;
    resolve_value(ctx, it->init, init, &next);
    if (next)
      libcp_error_tok(ctx, next->comma_tok, "expected '}'");
    return;
  }

  init->expr = (rec->kind == INIT_STR) ? materialize_str(ctx, rec) : rec->expr;
}

// Resolve a faithful initializer record against a declared type:
// evaluate designators, bind member names, apply brace elision, size
// flexible arrays and complete flexible struct/union members. *new_ty
// receives the completed type (it may differ from ty for flexible
// arrays and flexible members).
//
// 针对声明类型解析忠实初始化器记录: 求值指示符, 绑定成员名, 应用
// 花括号省略, 确定柔性数组大小, 补全柔性 struct/union 成员. *new_ty
// 接收补全后的类型 (对于柔性数组与柔性成员, 它可能与 ty 不同).
static LibcpInitTree* resolve_initializer(LibcpContext* ctx,
                                          LibcpInitializer* rec,
                                          LibcpType* ty,
                                          LibcpType** new_ty) {
  LibcpInitItem* cursor = NULL;
  LibcpInitTree* init   = new_resolved_init(ctx, ty, true);
  resolve_value(ctx, rec, init, &cursor);

  if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->is_flexible) {
    ty = copy_struct_type(ctx, ty);

    LibcpMember* mem = ty->members;
    while (mem->next)
      mem = mem->next;
    mem->ty = init->children[mem->idx]->ty;
    ty->size += mem->ty->size;

    *new_ty = ty;
    return init;
  }

  *new_ty = init->ty;
  return init;
}

// A variable definition with an initializer is a shorthand notation
// for a variable definition followed by assignments. This function
// generates assignment expressions for an initializer. For example,
// `int x[2][2] = {{6, 7}, {8, 9}}` is converted to the following
// expressions:
//
//   x[0][0] = 6;
//   x[0][1] = 7;
//   x[1][0] = 8;
//   x[1][1] = 9;
//
// The parser hands the faithful initializer record to the declaration
// node; sema resolves it and this lowering rebuilds the chain from the
// result.
//
// 带初始化器的变量定义是 "变量定义加若干赋值" 的简写形式. 本函数为
// 初始化器生成赋值表达式. 例如 `int x[2][2] = {{6, 7}, {8, 9}}`
// 被转换为以下表达式:
//
//   x[0][0] = 6;
//   x[0][1] = 7;
//   x[1][0] = 8;
//   x[1][1] = 9;
//
// 解析器把忠实初始化器记录交给声明节点; sema 解析它, 这次降级则从
// 结果重建表达式链.

static uint64_t read_buf(LibcpContext* ctx, char* buf, int sz) {
  // 从 .data 镜像按元素宽度读出一个无符号值 (位域读改写使用).
  if (sz == 1)
    return *buf;
  if (sz == 2)
    return *(uint16_t*)buf;
  if (sz == 4)
    return *(uint32_t*)buf;
  if (sz == 8)
    return *(uint64_t*)buf;
  unreachable();
}

static void write_buf(LibcpContext* ctx, char* buf, uint64_t val, int sz) {
  // 按元素宽度把一个无符号值写回 .data 镜像.
  if (sz == 1)
    *buf = val;
  else if (sz == 2)
    *(uint16_t*)buf = val;
  else if (sz == 4)
    *(uint32_t*)buf = val;
  else if (sz == 8)
    *(uint64_t*)buf = val;
  else
    unreachable();
}

static LibcpRelocation* write_gvar_data(LibcpContext* ctx,
                                        LibcpRelocation* cur,
                                        LibcpInitTree* init,
                                        LibcpType* ty,
                                        char* buf,
                                        int offset) {
  // 递归地把已解析的初始化器树写入 .data 缓冲区: 数组与聚合逐成员
  // 展开, 位域做读改写, 符号地址记入重定位链.
  if (ty->kind == TY_ARRAY) {
    int sz = ty->base->size;
    for (int i = 0; i < ty->array_len; i++)
      cur = write_gvar_data(ctx, cur, init->children[i], ty->base, buf, offset + sz * i);
    return cur;
  }

  if (ty->kind == TY_STRUCT) {
    for (LibcpMember* mem = ty->members; mem; mem = mem->next) {
      if (mem->is_bitfield) {
        LibcpNode* expr = init->children[mem->idx]->expr;
        if (!expr)
          break;

        char* loc         = buf + offset + mem->offset;
        uint64_t oldval   = read_buf(ctx, loc, mem->ty->size);
        uint64_t newval   = eval(ctx, expr);
        uint64_t mask     = mem->bit_width == 64 ? ~0ULL : (1ULL << mem->bit_width) - 1;
        uint64_t combined = oldval | ((newval & mask) << mem->bit_offset);
        write_buf(ctx, loc, combined, mem->ty->size);
      } else {
        cur =
            write_gvar_data(ctx, cur, init->children[mem->idx], mem->ty, buf, offset + mem->offset);
      }
    }
    return cur;
  }

  if (ty->kind == TY_UNION) {
    if (!init->mem)
      return cur;
    return write_gvar_data(ctx, cur, init->children[init->mem->idx], init->mem->ty, buf, offset);
  }

  if (!init->expr)
    return cur;

  if (ty->kind == TY_FLOAT) {
    *(float*)(buf + offset) = eval_double(ctx, init->expr);
    return cur;
  }

  if (ty->kind == TY_DOUBLE) {
    *(double*)(buf + offset) = eval_double(ctx, init->expr);
    return cur;
  }

  LibcpLabelRef label = {};
  uint64_t val        = eval2(ctx, init->expr, &label);

  if (!label.name && !label.node) {
    write_buf(ctx, buf + offset, val, ty->size);
    return cur;
  }

  LibcpRelocation* rel = libcp_arena_calloc(ctx, 1, sizeof(LibcpRelocation));
  rel->offset          = offset;
  rel->label           = label.name;
  rel->label_node      = label.node;
  rel->addend          = val;
  cur->next            = rel;
  return cur->next;
}

// Serialize a resolved initializer tree into the .data image of a
// global variable.
//
// 把已解析的初始化器树序列化为全局变量的 .data 数据镜像.
static void gvar_init_data(LibcpContext* ctx, LibcpObj* var, LibcpInitTree* init) {
  LibcpRelocation head = {};
  char* buf            = libcp_arena_calloc(ctx, 1, var->ty->size);
  write_gvar_data(ctx, &head, init, var->ty, buf, 0);
  var->init_data = buf;
  var->rel       = head.next;
}

// Resolve a global variable's faithful initializer record and
// serialize it into the variable's .data image. Initializers for
// global variables are evaluated at compile-time; it is a compile
// error if an initializer list contains a non-constant expression.
//
// 解析全局变量的忠实初始化器记录, 并序列化到该变量的 .data 镜像.
// 全局变量的初始化器在编译期求值; 若初始化器列表包含非常量表达式,
// 则是编译错误.
static void serialize_gvar(LibcpContext* ctx, LibcpNode* node) {
  if (!node->decl_init)
    return;

  gvar_init_data(ctx, node->var, node->init_resolved);
  node->decl_init     = NULL;
  node->init_resolved = NULL;
}

LibcpType* libcp_get_common_type(LibcpContext* ctx, LibcpType* ty1, LibcpType* ty2) {
  // 求两个类型的公共类型: 指针/函数按指针处理, 浮点按最高精度提升,
  // 窄于 int 的整数先提升到 int, 再按宽度与符号选择.
  if (ty1->base)
    return libcp_pointer_to(ctx, ty1->base);

  if (ty1->kind == TY_FUNC)
    return libcp_pointer_to(ctx, ty1);
  if (ty2->kind == TY_FUNC)
    return libcp_pointer_to(ctx, ty2);

  if (ty1->kind == TY_LDOUBLE || ty2->kind == TY_LDOUBLE)
    return libcp_ty_ldouble;
  if (ty1->kind == TY_DOUBLE || ty2->kind == TY_DOUBLE)
    return libcp_ty_double;
  if (ty1->kind == TY_FLOAT || ty2->kind == TY_FLOAT)
    return libcp_ty_float;

  if (ty1->size < 4)
    ty1 = libcp_ty_int;
  if (ty2->size < 4)
    ty2 = libcp_ty_int;

  if (ty1->size != ty2->size)
    return (ty1->size < ty2->size) ? ty2 : ty1;

  if (ty2->is_unsigned)
    return ty2;
  return ty1;
}

// For many binary operators, we implicitly promote operands so that
// both operands have the same type. Any integral type smaller than
// int is always promoted to int. If the type of one operand is larger
// than the other's (e.g. "long" vs. "int"), the smaller operand will
// be promoted to match with the other.
//
// This operation is called the "usual arithmetic conversion".
//
// 对许多二元运算符, 隐式提升操作数使两者类型相同. 任何窄于 int 的
// 整数类型总是提升为 int. 若一个操作数的类型更大 (例如 "long" 与
// "int"), 较小的操作数会被提升以与另一个匹配.
//
// 这一操作称为 "常规算术转换".
void libcp_usual_arith_conv(LibcpContext* ctx, LibcpNode** lhs, LibcpNode** rhs) {
  LibcpType* ty = libcp_get_common_type(ctx, (*lhs)->ty, (*rhs)->ty);
  *lhs          = libcp_new_cast(ctx, *lhs, ty);
  *rhs          = libcp_new_cast(ctx, *rhs, ty);
}

// Find a struct member by name, descending into anonymous members.
// 按名字查找 struct 成员, 并下降进匿名成员.
static LibcpMember* get_struct_member(LibcpType* ty, LibcpToken* tok) {
  for (LibcpMember* mem = ty->members; mem; mem = mem->next) {
    // Anonymous struct member
    // 匿名 struct 成员
    if ((mem->ty->kind == TY_STRUCT || mem->ty->kind == TY_UNION) && !mem->name) {
      if (get_struct_member(mem->ty, tok))
        return mem;
      continue;
    }

    // A nameless member (e.g. an anonymous zero-width bitfield) takes
    // part in layout but can never be named.
    //
    // 无名成员 (例如匿名零宽位域) 参与布局, 但永远不能被命名.
    if (!mem->name)
      continue;

    // Regular struct member
    // 普通 struct 成员
    if (mem->name->len == tok->len && !strncmp(mem->name->loc, tok->loc, tok->len))
      return mem;
  }
  return NULL;
}

// Binds a member access the parser left unbound: the checks on the
// operand, the lookup of the name, and the dereference implied by `->`.
// An anonymous member does not name a field of the type the access is
// written against, so each one on the path becomes an ND_MEMBER node of
// its own between the operand and the named member.
//
// `node` is rewritten in place because its parent points at it.
//
// 绑定解析器留下未绑定的成员访问: 对操作数的检查, 名字查找, 以及 `->`
// 蕴含的解引用. 匿名成员并不命名访问所写的类型上的字段, 因此路径上
// 的每个匿名成员都会在操作数与具名成员之间变成自己的 ND_MEMBER 节点.
//
// `node` 就地改写, 因为父节点指向它.
static void resolve_member(LibcpContext* ctx, LibcpNode* node) {
  LibcpNode* operand = node->lhs;
  LibcpType* ty      = operand->ty;
  LibcpToken* arrow  = node->arrow_tok;

  // `x->y` is `(*x).y`; these are the checks the dereference would
  // make, anchored at the arrow.
  //
  // `x->y` 就是 `(*x).y`; 这里是解引用会做的检查, 锚点落在箭头上.
  if (arrow) {
    if (ty->kind != TY_PTR || !ty->base)
      libcp_error_tok(ctx, arrow, "invalid pointer dereference");
    if (ty->base->kind == TY_VOID)
      libcp_error_tok(ctx, arrow, "dereferencing a void pointer");
    if (ty->base->kind != TY_STRUCT && ty->base->kind != TY_UNION)
      libcp_error_tok(ctx, arrow, "not a struct nor a union");
    ty = ty->base;
  } else if (ty->kind != TY_STRUCT && ty->kind != TY_UNION) {
    libcp_error_tok(ctx, operand->tok, "not a struct nor a union");
  }

  // The access needs the members placed. A bare `struct S {...};`
  // definition carries no declaration record, so nothing has laid the
  // type out yet when the first access arrives through a pointer.
  //
  // 访问要求成员已放置. 裸的 `struct S {...};` 定义不携带声明记录,
  // 因此第一个通过指针到达的访问到来时, 还没有任何东西布局过该类型.
  resolve_type(ctx, ty);

  bool flattened = false;
  LibcpNode* cur = operand;
  for (;;) {
    LibcpMember* mem = get_struct_member(ty, node->tok);
    if (!mem)
      libcp_error_tok(ctx, node->tok, "no such member");

    LibcpNode* link = libcp_new_unary(ctx, ND_MEMBER, cur, node->tok);
    link->member    = mem;
    if (arrow) {
      // The dereference belongs to the innermost link. The marker
      // stays on it: codegen's gen_addr reads it as the dereference
      // the `->` stands for (PLAN B1.1b - the shaping pass no longer
      // rewrites it away).
      //
      // 解引用属于最内层的链接. 标记留在它上面: codegen 的 gen_addr 把
      // 它读作 `->` 代表的解引用 (PLAN B1.1b - shaping pass 不再把它
      // 改写掉).
      link->arrow_tok = arrow;
      arrow           = NULL;
    }
    link->ty = mem->ty;
    cur      = link;

    if (mem->name)
      break;
    flattened = true;
    ty        = mem->ty;
  }

  node->lhs    = cur->lhs;
  node->member = cur->member;
  // The outermost link dissolved into this node. With flattening the
  // innermost link stays wrapped and carries the marker, so this
  // node's own goes away; a single link dissolved whole, leaving this
  // node as the innermost one.
  //
  // 最外层的链接融入了本节点. 发生展平时, 最内层链接保持包裹并携带
  // 标记, 因此本节点自己的标记被清除; 若只融解了一个链接, 本节点
  // 本身就是最内层.
  if (flattened)
    node->arrow_tok = NULL;
}

// Picks the association of an ND_GENERIC that the controlling expression
// selects, and makes the node that association's result expression. The
// comparison is written against the type the controlling expression decays
// to; a "default:" association (the one that carries no type name) is a
// fallback that a later match overrides, and among matching types the
// last one wins, exactly as the parser's loop used to decide.
//
// The result expression is deliberately left untyped: the caller types
// the node afresh. The other associations are typed for their checks
// only - they are expressions in the source, and a real compiler reports
// their errors too.
//
// 选出控制表达式所选择的 ND_GENERIC 关联, 并让该节点成为该关联的结果
// 表达式. 比较针对控制表达式退化后的类型书写; "default:" 关联 (不带
// 类型名的那一个) 是后备, 会被之后的匹配覆盖, 而多个匹配类型中最后一个
// 胜出, 与过去解析器的循环判断完全一致.
//
// 结果表达式有意保持未定型: 调用者会重新为该节点定型. 其余关联只为
// 检查而定型 - 它们也是源码中的表达式, 真正的编译器同样会报告它们的
// 错误.
static void select_generic(LibcpContext* ctx, LibcpNode* node) {
  libcp_add_type(ctx, node->cond);

  LibcpType* ty = node->cond->ty;
  if (ty->kind == TY_FUNC)
    ty = libcp_pointer_to(ctx, ty);
  else if (ty->kind == TY_ARRAY)
    ty = libcp_pointer_to(ctx, ty->base);

  LibcpNode* sel = NULL;
  for (LibcpNode* assoc = node->args; assoc; assoc = assoc->next) {
    if (!assoc->ty_op) {
      if (!sel)
        sel = assoc->lhs;
    } else {
      resolve_type(ctx, assoc->ty_op);
      if (libcp_is_compatible(ty, assoc->ty_op))
        sel = assoc->lhs;
    }
  }

  if (!sel)
    libcp_error_tok(ctx,
                    node->tok,
                    "controlling expression type not compatible with"
                    " any generic association type");

  for (LibcpNode* assoc = node->args; assoc; assoc = assoc->next)
    if (assoc->lhs != sel)
      libcp_add_type(ctx, assoc->lhs);

  // The conclusion of the selection: the result expression of the
  // association the controlling expression picked, recorded on the
  // node that asked the question - and the node keeps its kind, so
  // the consumer reads the field rather than a replaced tree (PLAN
  // A9.1). The selection itself is typed here for the same reason
  // the unselected ones are above.
  //
  // 选择的结论: 控制表达式所选关联的结果表达式, 记录在发问的节点上 -
  // 节点保持自己的 kind, 因此消费者读字段而不是读被替换的树
  // (PLAN A9.1). 选中的表达式在这里定型, 理由与上面未选中的那些相同.
  libcp_add_type(ctx, sel);
  node->generic_sel = sel;
  node->ty          = sel->ty;
}

// Walks a statement chain, typing each node. The declaration records
// stay in the chain (PLAN A8.2): a consumer that wants the source's
// structure sees it, and the consumer's expansion is what turns a
// record into statements or into nothing. What stays here is the work
// that has to happen at the record's position: the enum constants and
// typedefs were resolved by the resolve pass (nothing left to do),
// serialized global data keeps its position in the emission (contract
// 4), and a [GNU] nested function definition's body is analyzed at
// this position of the enclosing body, as the parser used to.
//
// 遍历语句链, 为每个节点定型. 声明记录留在链中 (PLAN A8.2): 想要
// 源码结构的消费者能看到它, 消费者的展开才把记录变成语句或什么都不
// 产生. 留在这里的是必须在记录所在位置完成的工作: 枚举常量与 typedef
// 已由 resolve pass 解析 (无需再做), 被序列化的全局数据保持在发射中
// 的位置 (契约 4), [GNU] 嵌套函数定义的函数体在外层函数体的这个位置
// 分析, 与解析器过去的做法一致.
static void type_chain(LibcpContext* ctx, LibcpNode* head) {
  for (LibcpNode* n = head; n; n = n->next) {
    switch (n->kind) {
      case ND_TYPEDEF:
      case ND_ENUM_CONST: continue;
      case ND_GVAR_DECL: serialize_gvar(ctx, n); continue;
      case ND_FUNCDEF:
        if (n->body)
          analyze_function(ctx, n->var);
        continue;
      default: break;
    }

    libcp_add_type(ctx, n);
  }
}

// Types every initializer expression a resolved record holds (PLAN
// A8.1, contract 5). The descent never reaches them - they live on
// the resolved tree, not on any subtree a parent owns - and the
// expansion that used to type them is the consumer's now, so the
// annotation is explicit here, in the same position the old
// expansion ran.
//
// 为已解析记录持有的每个初始化器表达式定型 (PLAN A8.1, 契约 5).
// 下降遍历永远不会到达它们 - 它们位于已解析树上, 而不是父节点拥有的
// 任何子树上 - 而过去为它们定型的展开现在属于消费者, 因此这里显式
// 标注, 位置与旧展开运行时相同.
static void annotate_init_exprs(LibcpContext* ctx, LibcpInitTree* init) {
  if (!init)
    return;

  if (init->expr) {
    libcp_add_type(ctx, init->expr);
    return;
  }

  if (!init->children)
    return;

  if (init->ty->kind == TY_ARRAY) {
    for (int i = 0; i < init->ty->array_len; i++)
      annotate_init_exprs(ctx, init->children[i]);
    return;
  }

  for (LibcpMember* mem = init->ty->members; mem; mem = mem->next)
    annotate_init_exprs(ctx, init->children[mem->idx]);
}

// The annotation pass: types a node and every subtree it owns, inserts
// the implicit conversions the standard requires, runs the checks that
// need a typed tree, materializes the objects the language says exist,
// and records the compile-time conclusions on the nodes.
//
// Exported, and re-entrant on purpose: a consumer that builds a node
// calls it to have the node typed by sema's rules rather than by a copy
// of them, and sema itself calls it that way from the lowerings. The
// `node->ty` test makes it idempotent, so a subtree is annotated exactly
// once - which is also why a consumer must not run its own shaping over a
// subtree twice: this guard stops a second *annotation*, not a second
// *lowering* (PLAN contract 3(d)).
//
// 标注 pass: 为节点及其拥有的每棵子树定型, 插入标准要求的隐式转换,
// 运行需要已定型树的检查, 物化语言规定存在的对象, 并把编译期结论
// 记录到节点上.
//
// 已导出, 且有意可重入: 构建节点的消费者调用它, 让节点按 sema 的规则
// 定型, 而不是按这些规则的副本; sema 自己在降级中也这样调用. `node->ty`
// 判断使它幂等, 因此每棵子树恰好被标注一次 - 这也是消费者不得对同一
// 子树运行两次自己的整形的原因: 这道防线只阻止第二次 *标注*, 不阻止
// 第二次 *降级* (PLAN 契约 3(d)).
void libcp_add_type(LibcpContext* ctx, LibcpNode* node) {
  if (!node || node->ty)
    return;

  // A generic selection is resolved before anything else: the
  // resolution types the controlling expression, the associations and
  // the selected result - and records the conclusion on this node,
  // which keeps its kind (PLAN A9.1). The order matters: the
  // associations are typed in the resolution, never by the generic
  // descent below, so that a subtree which registers itself by
  // identity while being typed - a `&&label` joining the gotos list -
  // registers exactly once.
  //
  // 泛型选择先于一切解析: 解析过程为控制表达式, 各关联与选中的结果
  // 定型 - 并把结论记录在本节点上, 节点保持原 kind (PLAN A9.1). 顺序
  // 很重要: 各关联在解析中定型, 绝不由下面的通用下降定型, 这样在定型
  // 时按身份自我登记的子树 - 例如加入 gotos 链表的 `&&label` - 恰好
  // 登记一次.
  if (node->kind == ND_GENERIC) {
    select_generic(ctx, node);
    return;
  }

  // A statement expression's value is its last statement, and only an
  // expression statement as written has one: a declaration record is
  // a record here - the consumer expands it later, and the expansion
  // produces no value (PLAN A8.1) - so the faithful shape decides.
  // Without such a last statement the construct has type void, which
  // gcc accepts (C5 audit); a void expression is usable wherever no
  // value is needed.
  //
  // 语句表达式的值来自它的最后一条语句, 而且只有写成表达式语句的
  // 那一条才有值: 声明记录在这里仍是记录 - 消费者稍后展开它, 展开
  // 不产生值 (PLAN A8.1) - 因此由忠实形态决定. 没有这样的末尾语句时,
  // 该构造的类型为 void, gcc 接受这一点 (C5 审计); void 表达式可以用
  // 在任何不需要值的地方.
  LibcpNode* stmt_expr_value = NULL;
  if (node->kind == ND_STMT_EXPR) {
    stmt_expr_value = node->body;
    while (stmt_expr_value && stmt_expr_value->next)
      stmt_expr_value = stmt_expr_value->next;
    if (stmt_expr_value && stmt_expr_value->kind != ND_EXPR_STMT)
      stmt_expr_value = NULL;
  }

  libcp_add_type(ctx, node->lhs);
  libcp_add_type(ctx, node->rhs);
  libcp_add_type(ctx, node->cond);
  libcp_add_type(ctx, node->then);
  libcp_add_type(ctx, node->els);
  type_chain(ctx, node->init);
  libcp_add_type(ctx, node->inc);
  type_chain(ctx, node->body);

  for (LibcpNode* n = node->args; n; n = n->next)
    libcp_add_type(ctx, n);

  switch (node->kind) {
    case ND_NUM: node->ty = libcp_ty_int; return;
    case ND_ADD:
    case ND_SUB: {
      // A raw `+`/`-` from the parser: the lowerings (the pointer
      // scaling, the `num + ptr` canonicalization and the ptr-ptr
      // element count) are the consumer's now (PLAN A4.1). What stays
      // here is the operand acceptance - the branches new_add/new_sub
      // check when they rebuild - and the result type. No conversion
      // casts: the rebuilding libcp_new_arith runs the same usual_arith_conv
      // it always did, and doubling it would stack cast nodes.
      //
      // 解析器给出的原始 `+`/`-`: 各项降级 (指针缩放, `num + ptr` 的
      // 规范化, ptr-ptr 的元素计数) 现在属于消费者 (PLAN A4.1). 留在这里
      // 的是操作数可接受性 - new_add/new_sub 重建时检查的分支 - 与结果
      // 类型. 不插入转换 cast: 重建的 libcp_new_arith 会执行它一贯执行的
      // usual_arith_conv, 重复执行会堆叠 cast 节点.
      LibcpType* lt = node->lhs->ty;
      LibcpType* rt = node->rhs->ty;
      if (node->kind == ND_ADD) {
        if (lt->base && rt->base)
          libcp_error_tok(ctx, node->tok, "invalid operands");
      } else {
        if (lt->base && rt->base) {
          // The difference itself is an element count, so it is typed
          // long; the division that scales it down is built on top.
          //
          // 差值本身就是元素计数, 因此类型为 long; 把它缩小的除法
          // 构建在其上.
          node->ty = libcp_ty_long;
          return;
        }
        if (!((libcp_is_numeric(lt) && libcp_is_numeric(rt)) ||
              (lt->base && (lt->base->kind == TY_VLA || libcp_is_integer(rt)))))
          libcp_error_tok(ctx, node->tok, "invalid operands");
      }
      node->ty = libcp_get_common_type(ctx, lt, rt);
      return;
    }
    case ND_MUL:
    case ND_DIV:
    case ND_MOD:
    case ND_BITAND:
    case ND_BITOR:
    case ND_BITXOR:
      libcp_usual_arith_conv(ctx, &node->lhs, &node->rhs);
      node->ty = node->lhs->ty;
      return;
    case ND_NEG: {
      LibcpType* ty = libcp_get_common_type(ctx, libcp_ty_int, node->lhs->ty);
      node->lhs     = libcp_new_cast(ctx, node->lhs, ty);
      node->ty      = ty;
      return;
    }
    case ND_ASSIGN:
      if (node->op) {
        // A compound assignment stays as the parser recorded it: the
        // read-modify-write rewrite is the consumer's shaping pass
        // (PLAN A5.1). Every form that rewrite produces takes the left
        // operand's type, so the annotation records it here; the checks
        // the rewrite used to run ride on the rewritten tree and stay
        // with it.
        //
        // 复合赋值保持解析器记录的样子: 读改写重写属于消费者的 shaping
        // pass (PLAN A5.1). 该重写产生的每种形态都取左操作数的类型,
        // 因此标注在这里记录它; 重写过去执行的检查随重写后的树一起
        // 留在那里.
        node->ty = node->lhs->ty;
        return;
      }
      if (node->lhs->ty->kind == TY_ARRAY)
        libcp_error_tok(ctx, node->lhs->tok, "not an lvalue");
      if (node->lhs->ty->kind != TY_STRUCT)
        node->rhs = libcp_new_cast(ctx, node->rhs, node->lhs->ty);
      node->ty = node->lhs->ty;
      return;
    case ND_INCDEC:
      // Stays as the parser recorded it; the compound-assignment rewrite
      // is the consumer's shaping pass (PLAN A5.1). Both the pre- and
      // post- forms carry the operand's type - the post-form's cast back
      // to it is built there.
      //
      // 保持解析器记录的样子; 复合赋值重写属于消费者的 shaping pass
      // (PLAN A5.1). 前置与后置形态都携带操作数的类型 - 后置形态转回
      // 该类型的 cast 在那里构建.
      node->ty = node->lhs->ty;
      return;
    case ND_GT:
    case ND_GE:
    case ND_LT:
    case ND_LE:
      // The faithful `>`/`>=` keeps its operands and its kind here;
      // swapping them into the `<`/`<=` shape gen_expr reads is the
      // consumer's (PLAN A4.1). The conversion does not care which way
      // the operands face - the common type is symmetric - so the cast
      // nodes land on the same operands either way.
      //
      // 忠实形态的 `>`/`>=` 在这里保留其操作数与 kind; 把它们交换成
      // gen_expr 读取的 `<`/`<=` 形态是消费者的事 (PLAN A4.1). 转换
      // 不关心操作数的朝向 - 公共类型是对称的 - 因此无论哪种方式,
      // cast 节点都落在相同的操作数上.
      libcp_usual_arith_conv(ctx, &node->lhs, &node->rhs);
      node->ty = libcp_ty_int;
      return;
    case ND_EQ:
    case ND_NE:
      libcp_usual_arith_conv(ctx, &node->lhs, &node->rhs);
      node->ty = libcp_ty_int;
      return;
    case ND_FUNCALL:
      // Lower the faithful call node: the callee check, the argument
      // conversions and the return buffer.
      //
      // 降级忠实调用节点: 被调者检查, 实参转换与返回缓冲区.
      lower_funcall(ctx, node, node->tok);
      return;
    case ND_NOT:
    case ND_LOGOR:
    case ND_LOGAND: node->ty = libcp_ty_int; return;
    case ND_BITNOT:
    case ND_SHL:
    case ND_SHR: {
      // C11 6.5.3.3p3 and 6.5.7p3: the integer promotions apply to these
      // operands, so a narrow left operand makes the result `int`. The
      // result type is a conclusion consumers read (`sizeof`, `_Generic`),
      // not just an instruction-selection hint.
      //
      // C11 6.5.3.3p3 与 6.5.7p3: 这些操作数适用整数提升, 因此较窄的
      // 左操作数使结果为 `int`. 结果类型是消费者会读取的结论 (`sizeof`,
      // `_Generic`), 而不仅仅是指令选择提示.
      LibcpType* ty = libcp_get_common_type(ctx, libcp_ty_int, node->lhs->ty);
      node->ty      = ty;
      return;
    }
    case ND_STRING: {
      // Materialize the literal's anonymous global, created here where
      // the literal is typed (PLAN contract 4). The node keeps its kind
      // and points at the object: the consumers - the evaluator and
      // codegen's address generation - read `node->var`.
      //
      // 物化字面量的匿名全局对象, 就在字面量定型的这里创建 (PLAN
      // 契约 4). 节点保持自己的 kind 并指向该对象: 消费者 - 求值器与
      // codegen 的地址生成 - 读取 `node->var`.
      LibcpObj* var = new_string_literal(ctx, node->tok->str, node->tok->ty);
      node->var     = var;
      node->ty      = var->ty;
      return;
    }
    case ND_IDENT:
      // The resolve pass binds every name while the scope it was
      // written in is reconstructed; this is the safety net for nodes
      // the evaluator reaches directly.
      //
      // resolve pass 在重建名字书写时所在的作用域时绑定每个名字; 这里是
      // 为求值器直接到达的节点准备的安全网.
      bind_ident(ctx, node);
      return;
    case ND_VAR:
    case ND_VLA_PTR: node->ty = node->var->ty; return;
    case ND_SIZEOF:
    case ND_ALIGNOF: {
      // The conclusion is a number: the size or the alignment of the
      // resolved type, recorded in the same `val` slot a literal uses.
      // A `sizeof expr` operand is typed but never evaluated, so its
      // nodes are not part of what the consumer emits. A sizeof of a
      // VLA is a runtime value, not a constant: the consumer's shaping
      // pass rebuilds that one as the comma expression that computes
      // and reads the size variable (PLAN A9.1; the builder itself is
      // still sema's until A7.1). The operand stays on the node for
      // that case - it is what identifies the type.
      //
      // 结论是一个数: 已解析类型的大小或对齐, 记录在字面量所用的同一个
      // `val` 槽中. `sizeof expr` 的操作数只定型不求值, 因此其节点不在
      // 消费者发射的内容中. VLA 的 sizeof 是运行时值, 不是常量: 消费者
      // 的 shaping pass 把这种情形重建为计算并读取大小变量的逗号表达式
      // (PLAN A9.1; 构建器本身在 A7.1 之前仍属于 sema). 该情形下操作数
      // 留在节点上 - 它正是类型的标识.
      LibcpType* ty = node->ty_op;
      if (!ty) {
        libcp_add_type(ctx, node->lhs);
        ty = node->lhs->ty;
      }
      // The operand's type is read for its size, so the layout has to be
      // complete; a struct reached through a dereference is not placed by
      // the pointer rules alone.
      //
      // 读取操作数类型是为了取大小, 因此布局必须完整; 通过解引用到达的
      // struct 单靠指针规则不会被放置.
      resolve_type(ctx, ty);

      node->ty = libcp_ty_ulong;
      if (node->kind == ND_SIZEOF && ty->kind == TY_VLA)
        return;

      node->val = node->kind == ND_SIZEOF ? ty->size : ty->align;
      return;
    }
    case ND_TYPES_COMPATIBLE:
      // `__builtin_types_compatible_p(T1, T2)`: a 0/1 conclusion in
      // `val`, typed like the literal it always folded to.
      //
      // `__builtin_types_compatible_p(T1, T2)`: 在 `val` 中给出 0/1
      // 结论, 类型与它一贯折叠成的字面量相同.
      resolve_type(ctx, node->ty_op);
      resolve_type(ctx, node->ty_op2);
      node->val    = libcp_is_compatible(node->ty_op, node->ty_op2);
      node->ty_op  = NULL;
      node->ty_op2 = NULL;
      node->ty     = libcp_ty_int;
      return;
    case ND_REG_CLASS: {
      // `__builtin_reg_class(T)`: integer or pointer, floating-point,
      // or anything else, as a 0/1/2 conclusion in `val`.
      //
      // `__builtin_reg_class(T)`: 整数或指针, 浮点, 或其他, 在 `val` 中
      // 给出 0/1/2 结论.
      resolve_type(ctx, node->ty_op);
      LibcpType* ty = node->ty_op;
      int64_t val   = 2;
      if (libcp_is_integer(ty) || ty->kind == TY_PTR)
        val = 0;
      else if (libcp_is_flonum(ty))
        val = 1;

      node->ty_op = NULL;
      node->val   = val;
      node->ty    = libcp_ty_int;
      return;
    }
    case ND_COND:
      if (node->is_elvis) {
        // The GNU `a ?: b` keeps its shape here; lowering it to
        // `tmp = a, tmp ? tmp : b` is the consumer's shaping pass
        // (PLAN A9.1), which also owns the temporary slot. What stays
        // is the conclusion: the type the lowered conditional would
        // have - the common type of the operand and the else arm, or
        // void when either is, exactly as the inner conditional of the
        // lowered form concludes it.
        //
        // GNU 的 `a ?: b` 在这里保持原形; 把它降级为
        // `tmp = a, tmp ? tmp : b` 是消费者的 shaping pass (PLAN A9.1),
        // 临时槽位也归它. 留在这里的是结论: 降级后条件表达式会有的
        // 类型 - 操作数与 else 分支的公共类型, 任一方为 void 时则为
        // void, 与降级形态的内层条件表达式得出的结论完全一致.
        LibcpType* ty = node->cond->ty;
        if (ty->kind == TY_VOID || node->els->ty->kind == TY_VOID)
          node->ty = libcp_ty_void;
        else
          node->ty = libcp_get_common_type(ctx, ty, node->els->ty);
        return;
      }
      if (node->then->ty->kind == TY_VOID || node->els->ty->kind == TY_VOID) {
        node->ty = libcp_ty_void;
      } else {
        libcp_usual_arith_conv(ctx, &node->then, &node->els);
        node->ty = node->then->ty;
      }
      return;
    case ND_COMMA: node->ty = node->rhs->ty; return;
    case ND_MEMBER:
      // Bind the member name the parser left unresolved. Nodes that sema
      // builds itself (compound assignment, initializer designators)
      // carry their member already.
      //
      // 绑定解析器留下未解析的成员名. sema 自己构建的节点 (复合赋值,
      // 初始化器指示符) 已经携带成员.
      if (!node->member)
        resolve_member(ctx, node);
      node->ty = node->member->ty;
      return;
    case ND_ADDR: {
      if (node->lhs->kind == ND_MEMBER && node->lhs->member->is_bitfield)
        libcp_error_tok(ctx, node->tok, "cannot take address of bitfield");

      LibcpType* ty = node->lhs->ty;
      if (ty->kind == TY_ARRAY)
        node->ty = libcp_pointer_to(ctx, ty->base);
      else
        node->ty = libcp_pointer_to(ctx, ty);
      return;
    }
    case ND_SUBSCRIPT: {
      // `x[y]` stays itself and takes the pointee type as its
      // conclusion. The operand pair is accepted exactly where new_add
      // accepted it when the lowering lived here - both sides numeric,
      // or one side carrying a base while the other carries none - and
      // the pointee then passes the checks the dereference made. The
      // `*(x+y)` rewrite with its scaling is codegen's (A3.1).
      //
      // `x[y]` 保持自身, 以指向类型为结论. 操作数对的接受条件与降级
      // 还在这里时 new_add 的条件完全一致 - 两侧都是数值, 或一侧带 base
      // 而另一侧不带 - 随后指向类型通过解引用所做的检查. 带缩放的
      // `*(x+y)` 重写属于 codegen (A3.1).
      LibcpType* lt = node->lhs->ty;
      LibcpType* rt = node->rhs->ty;

      if (!(libcp_is_numeric(lt) && libcp_is_numeric(rt)) && !lt->base && !rt->base)
        libcp_error_tok(ctx, node->tok, "invalid operands");
      if (lt->base && rt->base)
        libcp_error_tok(ctx, node->tok, "invalid operands");

      LibcpType* base = lt->base ? lt->base : rt->base;
      if (!base)
        libcp_error_tok(ctx, node->tok, "invalid pointer dereference");
      if (base->kind == TY_VOID)
        libcp_error_tok(ctx, node->tok, "dereferencing a void pointer");
      node->ty = base;
      return;
    }
    case ND_DEREF:
      if (node->lhs->ty->kind == TY_FUNC) {
        // [https://www.sigbus.info/n1570#6.5.3.2p4] Dereferencing a
        // function shouldn't do anything: `*foo` is just `foo`. The
        // parser cannot check this without typing the operand, so the
        // node survives until here; the conclusion is the function type
        // and the node stays itself - the evaluator reads through it in
        // constant expressions, and codegen loads nothing for it.
        //
        // [https://www.sigbus.info/n1570#6.5.3.2p4] 对函数解引用不应
        // 做任何事: `*foo` 就是 `foo`. 解析器在未定型操作数时无法检查
        // 这一点, 因此该节点存活到这里; 结论是函数类型, 节点保持自身 -
        // 常量表达式中求值器会穿过它读取, codegen 不会为它加载任何东西.
        node->ty = node->lhs->ty;
        return;
      }
      if (!node->lhs->ty->base)
        libcp_error_tok(ctx, node->tok, "invalid pointer dereference");
      if (node->lhs->ty->base->kind == TY_VOID)
        libcp_error_tok(ctx, node->tok, "dereferencing a void pointer");

      node->ty = node->lhs->ty->base;
      // A struct or union read through a pointer is used by value from
      // here on (`*p = x`, a struct argument), and that needs its members
      // placed; the pointer rules deliberately leave the target alone.
      //
      // 通过指针读取的 struct 或 union 从此按值使用 (`*p = x`, struct
      // 实参), 这要求其成员已放置; 指针规则有意不碰目标类型.
      resolve_type(ctx, node->ty);
      return;
    case ND_STMT_EXPR:
      if (stmt_expr_value) {
        node->ty = stmt_expr_value->lhs->ty;
        return;
      }
      // [GNU] A statement expression whose last statement is not an
      // expression statement has type void; gcc accepts it, and the
      // expression is valid wherever no value is needed.
      //
      // [GNU] 最后一条语句不是表达式语句的语句表达式类型为 void; gcc
      // 接受这一点, 该表达式可以用在任何不需要值的地方.
      node->ty = libcp_ty_void;
      return;
    case ND_LABEL_VAL:
      // [GNU] `&&lbl` is matched with the labels of the function by
      // analyze. It is collected here rather than there because such an
      // expression can live outside the statement tree - a block-scope
      // static initializer holds one, and only the evaluator reaches it.
      // The type annotation makes this run exactly once per node.
      //
      // [GNU] `&&lbl` 由 analyze 与函数的标签配对. 在这里而不是那里收集,
      // 是因为这种表达式可以位于语句树之外 - 块作用域 static 初始化器
      // 就可能持有它, 只有求值器能到达. 类型标注保证每个节点只运行一次.
      libcp_nodearray_push(ctx, &gotos, node);
      node->ty = libcp_pointer_to(ctx, libcp_ty_void);
      return;
    case ND_CAS:
      libcp_add_type(ctx, node->cas_addr);
      libcp_add_type(ctx, node->cas_old);
      libcp_add_type(ctx, node->cas_new);
      node->ty = libcp_ty_bool;

      if (node->cas_addr->ty->kind != TY_PTR)
        libcp_error_tok(ctx, node->cas_addr->tok, "pointer expected");
      if (node->cas_old->ty->kind != TY_PTR)
        libcp_error_tok(ctx, node->cas_old->tok, "pointer expected");
      return;
    case ND_EXCH:
      if (node->lhs->ty->kind != TY_PTR)
        libcp_error_tok(ctx, node->lhs->tok, "pointer expected");
      node->ty = node->lhs->ty->base;
      return;
    case ND_CAST:
      // An explicit cast from the parser: the target type is the record
      // in ty_op and the operand has been typed by the recursion above.
      // Casts sema builds itself arrive fully typed and never get here.
      //
      // 解析器给出的显式 cast: 目标类型是 ty_op 中的记录, 操作数已被上面
      // 的递归定型. sema 自己构建的 cast 到达时已完全定型, 不会进入这里.
      resolve_type(ctx, node->ty_op);
      node->ty = node->ty_op;
      return;
    case ND_RETURN:
      // The implicit conversion to the return type. Struct and union
      // returns are passed through as the parser left them.
      //
      // 到返回类型的隐式转换. struct 与 union 返回值按解析器留下的样子
      // 直接通过.
      if (node->lhs) {
        LibcpType* ty = sema_fn->ty->return_ty;
        if (ty->kind != TY_STRUCT && ty->kind != TY_UNION)
          node->lhs = libcp_new_cast(ctx, node->lhs, ty);
      }
      return;
    case ND_DECL: {
      // A declaration record keeps its shape (PLAN A8.1): the checks
      // run here, a block-scope static's initializer is serialized here
      // (the data image's position in the emission is contract 4), and
      // the initializer expressions are annotated here - explicitly,
      // because nothing else reaches them: the expansion that used to
      // type them is the consumer's now (contract 5). The consumer's
      // shaping pass expands what remains into 0..n statements and
      // removes the record from the chain when there are none.
      //
      // 声明记录保持其形状 (PLAN A8.1): 检查在这里运行, 块作用域 static
      // 初始化器在这里序列化 (数据镜像在发射中的位置是契约 4), 初始化器
      // 表达式在这里标注 - 而且是显式的, 因为没有其他东西能到达它们:
      // 过去为它们定型的展开现在属于消费者 (契约 5). 消费者的 shaping
      // pass 把剩余部分展开为 0..n 条语句, 没有语句时把记录从链中移除.
      LibcpObj* var   = node->var;
      LibcpToken* tok = node->tok;

      if (node->attr.is_static) {
        if (node->decl_init) {
          gvar_init_data(ctx, var, node->init_resolved);
          node->decl_init     = NULL;
          node->init_resolved = NULL;
        }
        return;
      }

      if (var->ty->kind == TY_VLA && node->decl_init)
        // A variable-length object may not be initialized. The parser left
        // the two standing side by side because whether a declaration is a
        // VLA one depends on the dimension being a constant expression;
        // the `=` is where that used to be reported.
        //
        // 变长对象不得初始化. 解析器把两者并排放置, 因为一条声明是否为
        // VLA 取决于维度是否为常量表达式; `=` 是过去报告该错误的位置.
        libcp_error_tok(ctx,
                        node->decl_init->eq_tok,
                        "variable-sized object may not be initialized");

      if (var->ty->kind != TY_VLA) {
        // A declared object must have a complete, non-void type.
        // 声明的对象必须具有完整的, 非 void 的类型.
        check_declared_void(ctx, tok, var->ty);
        if (var->ty->size < 0)
          libcp_error_tok(ctx, node->name_tok, "variable has incomplete type");
      }

      annotate_init_exprs(ctx, node->init_resolved);
      return;
    }
    case ND_COMPOUND_LITERAL: {
      // Materialize the compound literal. The resolve pass created the
      // hidden variable it owns - a hidden local in block scope - and
      // resolved its initializer record against the variable's type. At
      // file scope the variable is an anonymous global whose data is
      // serialized here and the node becomes a reference of it; in
      // block scope the consumer builds the initializer comma and the
      // reference (PLAN A8.1). The initializer expressions are
      // annotated here either way - nothing else reaches them.
      //
      // 物化复合字面量. resolve pass 创建了它拥有的隐藏变量 - 块作用域
      // 中是隐藏局部变量 - 并针对该变量的类型解析了初始化器记录. 在文件
      // 作用域, 该变量是匿名全局对象, 其数据在这里序列化, 节点变成对它的
      // 引用; 在块作用域, 消费者构建初始化逗号表达式与引用 (PLAN A8.1).
      // 无论哪种情形, 初始化器表达式都在这里标注 - 没有其他东西能到达
      // 它们.
      LibcpObj* var = node->var;

      annotate_init_exprs(ctx, node->init_resolved);

      if (!var->is_local) {
        gvar_init_data(ctx, var, node->init_resolved);
        node->decl_init     = NULL;
        node->init_resolved = NULL;
      }
      node->ty = var->ty;
      return;
    }
  }
}

static int64_t eval(LibcpContext* ctx, LibcpNode* node) {
  // 求值一个常量表达式, 不关心地址常量指向的标签.
  return eval2(ctx, node, NULL);
}

// Evaluate a given node as a constant expression.
//
// A constant expression is either just a number or ptr+n where ptr
// is a pointer to a global variable and n is a postiive/negative
// number. The latter form is accepted only as an initialization
// expression for a global variable.
//
// The libcp_add_type at the entry is what types the tree before the switch
// runs - for the preprocessor it is the only annotation the
// expression ever gets. It also still lowers part of the tree:
// SUBSCRIPT arrives as DEREF, STRING as a VAR, sizeof as a number,
// an elvis as the comma form, so the switch below sees those lowered
// shapes only. As the remaining lowerings leave the annotation pass
// (A6.1 onwards), the entry call stays - it still types the tree -
// but the switch starts seeing faithful kinds, which the cases below
// recognize alongside the lowered ones.
//
// 把给定节点作为常量表达式求值.
//
// 常量表达式要么只是一个数, 要么是 ptr+n, 其中 ptr 是指向全局变量的
// 指针, n 是正数或负数. 后一种形式只作为全局变量的初始化表达式接受.
//
// 入口处的 libcp_add_type 在 switch 运行前为树定型 - 对预处理器来说,
// 这是该表达式唯一会得到的标注. 它仍然会降级树的一部分: SUBSCRIPT
// 以 DEREF 到达, STRING 以 VAR 到达, sizeof 以数字到达, elvis 以逗号
// 形态到达, 因此下面的 switch 只会看到这些降级后的形态. 随着剩余降级
// 离开标注 pass (A6.1 起), 入口调用保留 - 它仍为树定型 - 但 switch
// 开始看到忠实 kind, 下面的分支会连同降级形态一起识别它们.
static int64_t eval2(LibcpContext* ctx, LibcpNode* node, LibcpLabelRef* label) {
  libcp_add_type(ctx, node);

  if (libcp_is_flonum(node->ty))
    return eval_double(ctx, node);

  switch (node->kind) {
    case ND_ADD: {
      // The additive lowering is codegen's now, so the faithful shape
      // reaches here unscaled and this case is the only scaling: the
      // pointer side is the operand whose own type has a base (the
      // operands carry their semantic types - there are no conversion
      // casts to pierce, and an explicit one must keep its target type),
      // the numeric side is scaled by the element size, and `num + ptr`
      // is accepted in the source order it was written. A VLA element
      // size is a runtime value, not a constant. `ptr + ptr` is rejected
      // by the annotation pass before the evaluator runs; the mirror
      // here is for the same guarantee.
      //
      // 加法降级现在属于 codegen, 因此忠实形态未缩放就到达这里, 而本
      // 分支是唯一的缩放点: 指针侧是其自身类型带 base 的操作数 (操作数
      // 携带语义类型 - 没有转换 cast 需要穿透, 显式 cast 必须保持目标
      // 类型), 数值侧按元素大小缩放, `num + ptr` 按其书写的源码顺序
      // 接受. VLA 元素大小是运行时值, 不是常量. `ptr + ptr` 会在求值器
      // 运行前被标注 pass 拒绝; 这里的镜像检查保证同一结论.
      LibcpType* lt = node->lhs->ty;
      LibcpType* rt = node->rhs->ty;
      if (lt->base && rt->base)
        libcp_error_tok(ctx, node->tok, "invalid operands");
      if (lt->base && !rt->base) {
        if (lt->base->kind == TY_VLA)
          libcp_error_tok(ctx, node->tok, "not a compile-time constant");
        return eval2(ctx, node->lhs, label) + eval(ctx, node->rhs) * lt->base->size;
      }
      if (!lt->base && rt->base) {
        if (rt->base->kind == TY_VLA)
          libcp_error_tok(ctx, node->tok, "not a compile-time constant");
        // The pointer side goes through eval2 with the caller's label
        // and unscaled, the numeric side carries the scale - the same
        // split the canonicalized lowered form had, with the pointer
        // side still evaluated first.
        //
        // 指针侧带着调用者的 label 经过 eval2 且不缩放, 数值侧承载缩放 -
        // 与规范化后降级形态相同的划分, 指针侧仍然先求值.
        return eval2(ctx, node->rhs, label) + eval2(ctx, node->lhs, label) * rt->base->size;
      }
      return eval2(ctx, node->lhs, label) + eval(ctx, node->rhs);
    }
    case ND_SUB: {
      // Same for `ptr - num`: the scaling is this case's now. A
      // `num - ptr` is invalid and rejected upstream. For `ptr - ptr`
      // the lowered form is `DIV(SUB(...), size)` - a different kind -
      // so this branch only runs on the faithful subtraction, and it
      // asks both sides as plain numbers (label == NULL), exactly the
      // question the lowered DIV asked: the difference of two addresses
      // is not a constant expression.
      //
      // `ptr - num` 同理: 缩放现在属于本分支. `num - ptr` 非法, 在上游
      // 被拒绝. 对 `ptr - ptr`, 降级形态是 `DIV(SUB(...), size)` - 另一种
      // kind - 因此本分支只处理忠实减法, 并且把两侧都当普通数字询问
      // (label == NULL), 与降级 DIV 所问的完全一致: 两个地址之差不是
      // 常量表达式.
      LibcpType* lt = node->lhs->ty;
      LibcpType* rt = node->rhs->ty;
      if (lt->base && rt->base) {
        int64_t lhs = eval2(ctx, node->lhs, NULL);
        int64_t rhs = eval2(ctx, node->rhs, NULL);
        return (lhs - rhs) / lt->base->size;
      }
      if (rt->base && !lt->base)
        libcp_error_tok(ctx, node->tok, "invalid operands");
      if (lt->base) {
        if (lt->base->kind == TY_VLA)
          libcp_error_tok(ctx, node->tok, "not a compile-time constant");
        return eval2(ctx, node->lhs, label) - eval(ctx, node->rhs) * lt->base->size;
      }
      return eval2(ctx, node->lhs, label) - eval(ctx, node->rhs);
    }
    case ND_MUL: return eval(ctx, node->lhs) * eval(ctx, node->rhs);
    case ND_DIV:
      if (node->ty->is_unsigned)
        return (uint64_t)eval(ctx, node->lhs) / eval(ctx, node->rhs);
      return eval(ctx, node->lhs) / eval(ctx, node->rhs);
    case ND_NEG: return -eval(ctx, node->lhs);
    case ND_MOD:
      if (node->ty->is_unsigned)
        return (uint64_t)eval(ctx, node->lhs) % eval(ctx, node->rhs);
      return eval(ctx, node->lhs) % eval(ctx, node->rhs);
    case ND_BITAND: return eval(ctx, node->lhs) & eval(ctx, node->rhs);
    case ND_BITOR: return eval(ctx, node->lhs) | eval(ctx, node->rhs);
    case ND_BITXOR: return eval(ctx, node->lhs) ^ eval(ctx, node->rhs);
    case ND_SHL: return eval(ctx, node->lhs) << eval(ctx, node->rhs);
    case ND_SHR:
      if (node->ty->is_unsigned && node->ty->size == 8)
        return (uint64_t)eval(ctx, node->lhs) >> eval(ctx, node->rhs);
      return eval(ctx, node->lhs) >> eval(ctx, node->rhs);
    case ND_EQ: return eval(ctx, node->lhs) == eval(ctx, node->rhs);
    case ND_NE: return eval(ctx, node->lhs) != eval(ctx, node->rhs);
    case ND_GT:
      // The faithful `>`: the lowering swapped the operands and turned
      // this into `<`, so the faithful comparison runs the other way
      // round, with the signedness taken from the operand the swap
      // would have moved to the left.
      //
      // 忠实形态的 `>`: 降级会交换操作数并把它变成 `<`, 因此忠实比较
      // 反方向进行, 符号性取自交换后会被移到左侧的那个操作数.
      if (node->rhs->ty->is_unsigned)
        return (uint64_t)eval(ctx, node->rhs) < eval(ctx, node->lhs);
      return eval(ctx, node->rhs) < eval(ctx, node->lhs);
    case ND_GE:
      if (node->rhs->ty->is_unsigned)
        return (uint64_t)eval(ctx, node->rhs) <= eval(ctx, node->lhs);
      return eval(ctx, node->rhs) <= eval(ctx, node->lhs);
    case ND_LT:
      if (node->lhs->ty->is_unsigned)
        return (uint64_t)eval(ctx, node->lhs) < eval(ctx, node->rhs);
      return eval(ctx, node->lhs) < eval(ctx, node->rhs);
    case ND_LE:
      if (node->lhs->ty->is_unsigned)
        return (uint64_t)eval(ctx, node->lhs) <= eval(ctx, node->rhs);
      return eval(ctx, node->lhs) <= eval(ctx, node->rhs);
    case ND_COND:
      // An elvis never becomes the comma form here: its value passes
      // through the temporary the lowering stores the operand in, so
      // it is a runtime value and not a constant.
      //
      // elvis 在这里永远不会变成逗号形态: 它的值经过降级存放操作数的
      // 临时变量, 因此是运行时值, 不是常量.
      if (node->is_elvis)
        break;
      return eval(ctx, node->cond) ? eval2(ctx, node->then, label) : eval2(ctx, node->els, label);
    case ND_COMMA: return eval2(ctx, node->rhs, label);
    case ND_NOT: return !eval(ctx, node->lhs);
    case ND_BITNOT: return ~eval(ctx, node->lhs);
    case ND_LOGAND: return eval(ctx, node->lhs) && eval(ctx, node->rhs);
    case ND_LOGOR: return eval(ctx, node->lhs) || eval(ctx, node->rhs);
    case ND_CAST: {
      int64_t val = eval2(ctx, node->lhs, label);
      if (libcp_is_integer(node->ty)) {
        switch (node->ty->size) {
          case 1: return node->ty->is_unsigned ? (uint8_t)val : (int8_t)val;
          case 2: return node->ty->is_unsigned ? (uint16_t)val : (int16_t)val;
          case 4: return node->ty->is_unsigned ? (uint32_t)val : (int32_t)val;
        }
      }
      return val;
    }
    case ND_DEREF:
      // `*foo` on a function designator is the function itself
      // (6.5.3.2p4): the annotation pass stopped dissolving the node at
      // A3.1, so the evaluator reads through it here, reaching the same
      // operand the dissolved form left in the tree. Any other
      // dereference is a runtime value and falls to the error below.
      //
      // 对函数指示符的 `*foo` 就是函数本身 (6.5.3.2p4): 标注 pass 从
      // A3.1 起不再溶解该节点, 因此求值器在这里穿过它读取, 到达与溶解
      // 形态留在树中相同的操作数. 其他任何解引用都是运行时值, 落入下面
      // 的错误.
      if (node->lhs->ty->kind == TY_FUNC)
        return eval2(ctx, node->lhs, label);
      break;
    case ND_ADDR: return eval_rval(ctx, node->lhs, label);
    case ND_LABEL_VAL: label->node = node; return 0;
    case ND_MEMBER:
      // `p->arr`, an array member of a pointer operand: the member's
      // address is the pointer value plus the offset, under the same
      // two guards the lowered member (DEREF inside) passed through.
      // A plain `.` member takes the address of its operand instead.
      //
      // `p->arr`, 指针操作数的数组成员: 成员地址是指针值加偏移, 并经过
      // 降级成员 (内含 DEREF) 所过的同样两道检查. 普通 `.` 成员改为取
      // 其操作数的地址.
      if (node->arrow_tok) {
        if (!label)
          libcp_error_tok(ctx, node->tok, "not a compile-time constant");
        if (node->ty->kind != TY_ARRAY)
          libcp_error_tok(ctx, node->tok, "invalid initializer");
        return eval2(ctx, node->lhs, label) + node->member->offset;
      }
      if (!label)
        libcp_error_tok(ctx, node->tok, "not a compile-time constant");
      if (node->ty->kind != TY_ARRAY)
        libcp_error_tok(ctx, node->tok, "invalid initializer");
      return eval_rval(ctx, node->lhs, label) + node->member->offset;
    case ND_VAR:
      if (!label)
        libcp_error_tok(ctx, node->tok, "not a compile-time constant");
      if (node->var->ty->kind != TY_ARRAY && node->var->ty->kind != TY_FUNC)
        libcp_error_tok(ctx, node->tok, "invalid initializer");
      label->name = node->var->name;
      return 0;
    case ND_STRING:
      // The literal's anonymous global, materialized where the literal
      // is annotated; the node keeps its kind and points at the
      // object, so the address is read off the object directly.
      //
      // 字面量的匿名全局对象, 在字面量被标注的地方物化; 节点保持自己的
      // kind 并指向该对象, 因此地址直接从对象读出.
      if (!label)
        libcp_error_tok(ctx, node->tok, "not a compile-time constant");
      label->name = node->var->name;
      return 0;
    case ND_COMPOUND_LITERAL:
      // A file-scope compound literal's hidden object, whose data the
      // annotation pass serialized; the consumer reads the object -
      // the node keeps its kind since A9.2, like a string.
      //
      // 文件作用域复合字面量的隐藏对象, 其数据已由标注 pass 序列化;
      // 消费者直接读取该对象 - 自 A9.2 起节点像字符串一样保持自己的
      // kind.
      if (node->var->is_local)
        libcp_error_tok(ctx, node->tok, "not a compile-time constant");
      label->name = node->var->name;
      return 0;
    case ND_SIZEOF:
    case ND_ALIGNOF: {
      // The conclusion the annotation pass records in `val` - the same
      // slot a literal uses. A sizeof of a VLA is a runtime value, so
      // that one is not a constant; an alignment always is.
      //
      // 标注 pass 记录在 `val` 中的结论 - 与字面量使用的同一个槽.
      // VLA 的 sizeof 是运行时值, 因此不是常量; 对齐则永远是常量.
      LibcpType* ty = node->ty_op ? node->ty_op : node->lhs->ty;
      if (node->kind == ND_SIZEOF && ty->kind == TY_VLA)
        libcp_error_tok(ctx, node->tok, "not a compile-time constant");
      return node->val;
    }
    case ND_TYPES_COMPATIBLE:
    case ND_REG_CLASS:
      // Both fold to a number, recorded in `val`.
      // 两者都折叠为一个数, 记录在 `val` 中.
      return node->val;
    case ND_GENERIC:
      // The conclusion of the selection: the result expression of the
      // association the controlling expression picked.
      //
      // 选择的结论: 控制表达式所选关联的结果表达式.
      return eval2(ctx, node->generic_sel, label);
    case ND_NUM: return node->val;
  }

  libcp_error_tok(ctx, node->tok, "not a compile-time constant");
}

static int64_t eval_rval(LibcpContext* ctx, LibcpNode* node, LibcpLabelRef* label) {
  // 求值一个左值表达式在常量上下文中的地址: 只接受全局对象, 解引用,
  // 字符串, 复合字面量与相对地址计算.
  switch (node->kind) {
    case ND_VAR:
      if (node->var->is_local)
        libcp_error_tok(ctx, node->tok, "not a compile-time constant");
      label->name = node->var->name;
      return 0;
    case ND_DEREF: return eval2(ctx, node->lhs, label);
    case ND_STRING:
      // The literal's anonymous global, as above.
      // 字面量的匿名全局对象, 同上.
      label->name = node->var->name;
      return 0;
    case ND_COMPOUND_LITERAL:
      // The file-scope literal's hidden global, as above: its address
      // is the object's, and a block-scope one is not a constant.
      //
      // 文件作用域字面量的隐藏全局对象, 同上: 其地址就是该对象的地址,
      // 而块作用域的不算常量.
      if (node->var->is_local)
        libcp_error_tok(ctx, node->tok, "not a compile-time constant");
      label->name = node->var->name;
      return 0;
    case ND_SUBSCRIPT: {
      // The address of `x[y]`: the base's address plus the index
      // scaled by the element size. A VLA element size is a runtime
      // value; an unscaled sum (both operands numbers) mirrors what
      // the lowered `*(x+y)` evaluated to for that shape.
      //
      // `x[y]` 的地址: 基址加上按元素大小缩放的索引. VLA 元素大小是
      // 运行时值; 不缩放的求和 (两侧都是数字) 镜像降级形态 `*(x+y)`
      // 在该情形下的求值结果.
      LibcpType* base = node->lhs->ty->base;
      if (base && base->kind == TY_VLA)
        libcp_error_tok(ctx, node->tok, "not a compile-time constant");
      if (!base)
        return eval2(ctx, node->lhs, label) + eval(ctx, node->rhs);
      return eval2(ctx, node->lhs, label) + eval(ctx, node->rhs) * base->size;
    }
    case ND_MEMBER:
      // `p->x`: the operand is the pointer itself, so the member's
      // address is the pointer value plus the offset; a `.` member
      // takes the address of its operand instead. No guards here, as
      // in the lowered form: the operand's own evaluation produces
      // whatever complaint is due.
      //
      // `p->x`: 操作数就是指针本身, 因此成员地址是指针值加偏移; `.`
      // 成员改为取其操作数的地址. 这里不加检查, 与降级形态一致:
      // 操作数自身的求值会产生应有的报错.
      if (node->arrow_tok)
        return eval2(ctx, node->lhs, label) + node->member->offset;
      return eval_rval(ctx, node->lhs, label) + node->member->offset;
  }

  libcp_error_tok(ctx, node->tok, "invalid initializer");
}

// Annotates first, like eval2, so it walks whatever shapes the
// annotation pass currently produces; see there. Its one real caller
// is resolve_type, classifying an array dimension as fixed or
// variable. Once the fold stops rewriting sizeof nodes (A9.1), the
// sizeof case below has to answer false for a VLA operand by its own
// kind, not because the fold hid it - otherwise a VLA silently
// becomes a fixed array.
//
// 像 eval2 一样先标注, 因此它遍历的是标注 pass 当前产生的形态; 见那里.
// 它唯一真正的调用者是 resolve_type, 用于把数组维度分类为固定或可变.
// 一旦折叠不再重写 sizeof 节点 (A9.1), 下面的 sizeof 分支就必须根据
// VLA 操作数自身的 kind 返回 false, 而不是因为折叠隐藏了它 - 否则
// VLA 会静默变成固定数组.
static bool is_const_expr(LibcpContext* ctx, LibcpNode* node) {
  libcp_add_type(ctx, node);

  switch (node->kind) {
    case ND_ADD:
    case ND_SUB:
    case ND_MUL:
    case ND_DIV:
    case ND_BITAND:
    case ND_BITOR:
    case ND_BITXOR:
    case ND_SHL:
    case ND_SHR:
    case ND_EQ:
    case ND_NE:
    case ND_LT:
    case ND_LE:
    case ND_GT:
    case ND_GE:
    case ND_LOGAND:
    case ND_LOGOR: return is_const_expr(ctx, node->lhs) && is_const_expr(ctx, node->rhs);
    case ND_COND:
      // An elvis is a runtime value: the lowering stores the operand
      // in a temporary first.
      //
      // elvis 是运行时值: 降级先把操作数存入临时变量.
      if (node->is_elvis)
        return false;
      if (!is_const_expr(ctx, node->cond))
        return false;
      return is_const_expr(ctx, eval(ctx, node->cond) ? node->then : node->els);
    case ND_COMMA: return is_const_expr(ctx, node->rhs);
    case ND_NEG:
    case ND_NOT:
    case ND_BITNOT:
    case ND_CAST: return is_const_expr(ctx, node->lhs);
    case ND_SIZEOF:
    case ND_ALIGNOF: {
      // The alignment of a VLA is a constant; its size is not.
      // VLA 的对齐是常量; 其大小不是.
      LibcpType* ty = node->ty_op ? node->ty_op : node->lhs->ty;
      return node->kind == ND_ALIGNOF || ty->kind != TY_VLA;
    }
    case ND_TYPES_COMPATIBLE:
    case ND_REG_CLASS: return true;
    case ND_GENERIC: return is_const_expr(ctx, node->generic_sel);
    case ND_NUM: return true;
  }

  return false;
}

int64_t libcp_const_expr(LibcpContext* ctx, LibcpToken** rest, LibcpToken* tok) {
  // 供预处理器 `#if` 使用: 解析一个条件表达式并求值.
  LibcpNode* node = libcp_conditional(ctx, rest, tok);
  return eval(ctx, node);
}

// Annotates first, like eval2 (see there): the shapes this switch
// sees are whatever the annotation pass currently produces, and the
// faithful cases below take over as the lowerings leave it.
//
// 像 eval2 一样先标注 (见那里): 这个 switch 看到的形态是标注 pass 当前
// 产生的形态, 随着降级离开标注 pass, 下面的忠实分支会接管.
static double eval_double(LibcpContext* ctx, LibcpNode* node) {
  libcp_add_type(ctx, node);

  if (libcp_is_integer(node->ty)) {
    if (node->ty->is_unsigned)
      return (unsigned long)eval(ctx, node);
    return eval(ctx, node);
  }

  switch (node->kind) {
    case ND_ADD: return eval_double(ctx, node->lhs) + eval_double(ctx, node->rhs);
    case ND_SUB: return eval_double(ctx, node->lhs) - eval_double(ctx, node->rhs);
    case ND_MUL: return eval_double(ctx, node->lhs) * eval_double(ctx, node->rhs);
    case ND_DIV: return eval_double(ctx, node->lhs) / eval_double(ctx, node->rhs);
    case ND_NEG: return -eval_double(ctx, node->lhs);
    case ND_COND:
      // An elvis is a runtime value; see eval2.
      // elvis 是运行时值; 见 eval2.
      if (node->is_elvis)
        break;
      return eval_double(ctx, node->cond) ? eval_double(ctx, node->then) :
                                            eval_double(ctx, node->els);
    case ND_COMMA: return eval_double(ctx, node->rhs);
    case ND_GENERIC:
      // The selected association's expression.
      // 选中关联的表达式.
      return eval_double(ctx, node->generic_sel);
    case ND_CAST:
      if (libcp_is_flonum(node->lhs->ty))
        return eval_double(ctx, node->lhs);
      return eval(ctx, node->lhs);
    case ND_NUM: return node->fval;
  }

  libcp_error_tok(ctx, node->tok, "not a compile-time constant");
}

//
// The resolve pass
//
// The scope stack is derived from the tree structure: a compound-stmt
// block pushes and pops a scope, a statement expression does the same
// for its body, and a `for` statement's init declarations share one
// scope with its condition, increment and body - exactly the scopes
// the parser used to drive while parsing.
//
// resolve pass
//
// 作用域栈从树结构推导: 复合语句块压入并弹出作用域, 语句表达式对其
// 函数体做同样的事, `for` 语句的 init 声明与其条件, 递增和函数体共享
// 一个作用域 - 与解析器过去在解析时驱动的作用域完全一致.

// True while the resolve pass is inside a function body. A compound
// literal owns a hidden local there and an anonymous global at file
// scope, and this is what says which: the storage class follows from
// where the resolve pass is, not from the depth of a scope stack.
//
// resolve pass 位于函数体内时为真. 复合字面量在函数体内拥有隐藏局部
// 变量, 在文件作用域拥有匿名全局对象, 这个标志决定是哪种: 存储类
// 跟随 resolve pass 所处的位置, 而不是作用域栈的深度.
#define resolving_body (ctx->resolving_body)

// Walks a faithful initializer record and resolves the expressions it
// carries (values and designator bounds); resolution and lowering of
// the record happen later, against the declared type.
//
// 遍历忠实初始化器记录, 解析它携带的表达式 (值与指示符边界);
// 该记录的解析与降级稍后针对声明类型进行.
static void resolve_init_record(LibcpContext* ctx, LibcpInitializer* rec) {
  if (!rec)
    return;

  if (rec->kind == INIT_EXPR)
    resolve_node(ctx, rec->expr);

  for (LibcpInitItem* it = rec->items; it; it = it->next) {
    for (LibcpInitDesig* d = it->desigs; d; d = d->next) {
      resolve_node(ctx, d->begin);
      resolve_node(ctx, d->end);
    }
    resolve_init_record(ctx, it->init);
  }
}

static void resolve_children(LibcpContext* ctx, LibcpNode* node) {
  // 递归解析节点拥有的全部子树, 覆盖语句与表达式两类字段.
  resolve_node(ctx, node->lhs);
  resolve_node(ctx, node->rhs);
  resolve_node(ctx, node->cond);
  resolve_node(ctx, node->then);
  resolve_node(ctx, node->els);
  resolve_chain(ctx, node->init);
  resolve_node(ctx, node->inc);
  resolve_chain(ctx, node->body);
  resolve_chain(ctx, node->args);
  resolve_node(ctx, node->cas_addr);
  resolve_node(ctx, node->cas_old);
  resolve_node(ctx, node->cas_new);
}

static void resolve_node(LibcpContext* ctx, LibcpNode* node) {
  if (!node)
    return;

  // Enum constants an expression-context type defined become visible
  // at this node's position.
  //
  // 表达式上下文中的类型所定义的枚举常量, 在这个节点的位置变为可见.
  if (node->spec_decls) {
    resolve_enum_records(ctx, node->spec_decls);
    node->spec_decls = NULL;
  }

  switch (node->kind) {
    case ND_TYPEDEF:
      resolve_type_exprs(ctx, node->ty);
      resolve_type(ctx, node->ty);
      return;
    case ND_ENUM_CONST:
      if (node->ty_op)
        resolve_enum_records(ctx, node);
      return;
    case ND_BLOCK:
      if (node->is_scope_block)
        enter_scope(ctx);
      resolve_chain(ctx, node->body);
      if (node->is_scope_block)
        leave_scope(ctx);
      return;
    case ND_STMT_EXPR:
      enter_scope(ctx);
      resolve_chain(ctx, node->body);
      leave_scope(ctx);
      return;
    case ND_FOR:
      // The init declarations share their scope with the rest of the
      // statement, and the records the init chain may start with are
      // registered inside it.
      //
      // init 声明与语句的其余部分共享作用域, init 链开头可能出现的记录
      // 也在该作用域内登记.
      enter_scope(ctx);
      resolve_chain(ctx, node->init);
      resolve_node(ctx, node->cond);
      resolve_node(ctx, node->inc);
      resolve_node(ctx, node->then);
      leave_scope(ctx);
      return;
    case ND_DECL: {
      resolve_type_exprs(ctx, node->ty_op);
      resolve_type(ctx, node->ty_op);

      if (node->attr.is_static) {
        // A block-scope static variable lives in the global data section
        // under an anonymous name, but its name is registered like any
        // other local. The void check is anchored where the old
        // declaration site reported it: at the `=` if there is an
        // initializer, at the declaration position otherwise.
        //
        // 块作用域 static 变量以匿名名字存放在全局数据段, 但它的名字像
        // 其他局部变量一样被登记. void 检查的锚点与旧声明点一致: 有
        // 初始化器时在 `=` 处, 否则在声明位置.
        LibcpToken* anchor = node->decl_init ? node->decl_init->eq_tok : node->tok;
        check_declared_void(ctx, anchor, node->ty_op);
        node->var = new_anon_gvar(ctx, node->ty_op);
        push_scope(ctx, libcp_get_ident(ctx, node->name_tok))->var = node->var;
      } else {
        node->var = new_lvar(ctx,
                             libcp_get_ident(ctx, node->name_tok),
                             named_type(ctx, node->ty_op, node->name_tok));
        int align = attr_align(ctx, &node->attr);
        if (align)
          node->var->align = align;
      }

      // The declared name is visible to its own initializer, as it was
      // when the parser created the variable before parsing the `=`.
      //
      // 被声明的名字对其自身初始化器可见, 与解析器在解析 `=` 之前创建
      // 变量时的行为一致.
      resolve_init_record(ctx, node->decl_init);

      // The initializer is resolved here, at the declaration's position:
      // resolution completes the declared type (an array sized by its
      // initializer, a flexible member), and later references in the
      // resolve order have to see the completed type.
      //
      // 初始化器在这里解析, 就在声明所在的位置: 解析会补全声明类型
      // (由初始化器确定大小的数组, 柔性成员), 而 resolve 顺序中稍后的
      // 引用必须看到补全后的类型.
      if (node->decl_init) {
        LibcpType* new_ty;
        node->init_resolved = resolve_initializer(ctx, node->decl_init, node->var->ty, &new_ty);
        node->var->ty       = new_ty;
      }
      return;
    }
    case ND_GVAR_DECL: {
      // 全局声明记录: 声明对象, 记录链接属性与对齐, 并解析初始化器.
      resolve_type_exprs(ctx, node->ty_op);
      resolve_type(ctx, node->ty_op);

      LibcpObj* var      = new_gvar(ctx,
                               libcp_get_ident(ctx, node->name_tok),
                               named_type(ctx, node->ty_op, node->name_tok));
      var->is_definition = !node->attr.is_extern;
      var->is_static     = node->attr.is_static;
      var->is_tls        = node->attr.is_tls;
      int align          = attr_align(ctx, &node->attr);
      if (align)
        var->align = align;
      if (!node->decl_init && !node->attr.is_extern && !node->attr.is_tls)
        var->is_tentative = true;
      node->var = var;

      resolve_init_record(ctx, node->decl_init);
      if (node->decl_init) {
        LibcpType* new_ty;
        node->init_resolved = resolve_initializer(ctx, node->decl_init, var->ty, &new_ty);
        var->ty             = new_ty;
      }
      return;
    }
    case ND_FUNCDEF: resolve_function(ctx, node); return;
    case ND_COMPOUND_LITERAL:
      // The hidden variable is created here, where the resolve context
      // says whether the literal lives in an anonymous global or on the
      // stack, and before the initializer record is walked.
      //
      // 隐藏变量在这里创建, 此时 resolve 上下文能判定字面量位于匿名
      // 全局对象还是栈上, 而且创建发生在遍历初始化器记录之前.
      resolve_type_exprs(ctx, node->ty_op);
      resolve_type(ctx, node->ty_op);
      node->var = resolving_body ? new_lvar(ctx, "", node->ty_op) : new_anon_gvar(ctx, node->ty_op);
      resolve_init_record(ctx, node->decl_init);
      if (node->decl_init) {
        LibcpType* new_ty;
        node->init_resolved = resolve_initializer(ctx, node->decl_init, node->var->ty, &new_ty);
        node->var->ty       = new_ty;
      }
      return;
    case ND_STRING: {
      // The literal becomes a reference to its anonymous global here, at
      // its source position, so the globals keep the declaration order
      // the single-pass parser produced.
      //
      // 字面量在这里, 就在其源码位置, 变成对其匿名全局对象的引用, 使
      // globals 链表保持单遍解析器产生的声明顺序.
      LibcpObj* var = new_string_literal(ctx, node->tok->str, node->tok->ty);
      node->kind    = ND_VAR;
      node->var     = var;
      node->ty      = var->ty;
      return;
    }
    case ND_CASE:
      // The value(s) a case label stands for: the parser recorded the
      // operands unevaluated, because whether an expression is a
      // constant is not a syntax question. They are truncated to `int`,
      // as they were when the parser wrote `begin`/`end` on its way past
      // them. `:` is where the range check is anchored.
      //
      // case 标签代表的值: 解析器记录未求值的操作数, 因为表达式是否
      // 为常量不是语法问题. 它们被截断为 `int`, 与解析器经过时写入
      // `begin`/`end` 的做法一致. 范围检查的锚点在 `:` 处.
      resolve_node(ctx, node->begin_expr);
      resolve_node(ctx, node->end_expr);
      if (node->begin_expr) {
        int begin = (int)eval(ctx, node->begin_expr);
        int end   = begin;

        if (node->end_expr) {
          end = (int)eval(ctx, node->end_expr);
          if (end < begin)
            libcp_error_tok(ctx, node->colon_tok, "empty case range specified");
        }

        node->begin      = begin;
        node->end        = end;
        node->begin_expr = NULL;
        node->end_expr   = NULL;
      }
      resolve_node(ctx, node->lhs);
      return;
    case ND_IDENT:
      // Bind the name (variable, function or enum constant), rewriting
      // the node into the shape codegen understands.
      //
      // 绑定名字 (变量, 函数或枚举常量), 把节点改写成 codegen 理解的
      // 形态.
      bind_ident(ctx, node);
      return;
    case ND_CAST:
      resolve_type_exprs(ctx, node->ty_op);
      resolve_type(ctx, node->ty_op);
      break;
    case ND_SIZEOF:
    case ND_ALIGNOF:
    case ND_REG_CLASS:
    case ND_GENERIC_ASSOC:
      if (node->ty_op) {
        resolve_type_exprs(ctx, node->ty_op);
        resolve_type(ctx, node->ty_op);
      }
      break;
    case ND_TYPES_COMPATIBLE:
      resolve_type_exprs(ctx, node->ty_op);
      resolve_type(ctx, node->ty_op);
      resolve_type_exprs(ctx, node->ty_op2);
      resolve_type(ctx, node->ty_op2);
      break;
    default: break;
  }

  resolve_children(ctx, node);
}

static void resolve_chain(LibcpContext* ctx, LibcpNode* node) {
  // 沿 next 链逐个解析节点.
  for (; node; node = node->next)
    resolve_node(ctx, node);
}

// Declares the function a record names (checking it against a previous
// declaration), then resolves its body: the parameters and helpers
// first, then the statements, with the scope stack the block structure
// gives. The variable list is captured before the annotation pass adds
// its temporaries; analyze_function splices those in afterwards.
//
// 声明记录所指的函数 (与先前的声明做检查), 然后解析其函数体: 先参数与
// 辅助变量, 再语句, 作用域栈由块结构给出. 变量链表在标注 pass 添加
// 临时变量之前捕获; analyze_function 随后把这些临时变量拼接进来.
static void resolve_function(LibcpContext* ctx, LibcpNode* node) {
  LibcpType* ty = node->ty_op;
  resolve_type_exprs(ctx, ty);
  resolve_type(ctx, ty);

  LibcpObj* fn = declare_function(ctx,
                                  libcp_get_ident(ctx, node->name_tok),
                                  named_type(ctx, ty, node->name_tok),
                                  &node->attr,
                                  node->tok,
                                  node->body != NULL);
  node->var    = fn;

  if (!node->body)
    return;

  fn->body = node->body;

  // A [GNU] nested function definition reaches this from inside the
  // enclosing body's resolution, and begin_function starts the local
  // list from scratch - so the enclosing function's context is saved
  // and restored around it, here and in the annotation pass
  // (analyze_function), which reads sema_fn for the return
  // conversions. The flag is saved for the same reach reason.
  //
  // [GNU] 嵌套函数定义会从外层函数体的 resolve 过程中到达这里, 而
  // begin_function 会从零开始局部变量链表 - 因此在外层函数上下文中
  // 保存并在之后恢复, 这里与标注 pass (analyze_function) 都要做,
  // 后者会读取 sema_fn 来完成返回转换. 标志同样因为这一到达路径而保存.
  LibcpObj* save_locals = locals;
  LibcpObj* save_fn     = sema_fn;
  bool save_body        = resolving_body;

  sema_fn        = fn;
  resolving_body = true;

  enter_scope(ctx);
  begin_function(ctx, fn, ty);
  resolve_node(ctx, node->body);
  set_obj_locals(fn, locals);
  leave_scope(ctx);

  resolving_body = save_body;
  sema_fn        = save_fn;
  locals         = save_locals;
}

// Runs the annotation + lowering pass and the control-flow descent over
// a resolved function body. The temporaries the lowering creates are
// collected on a fresh variable list and spliced in front of the ones
// the resolve pass captured, so that the function's frame is complete
// and no other function's list is polluted.
//
// 对已解析的函数体运行标注 + 降级 pass 以及控制流遍历. 降级创建的临时
// 变量收集在一个新的变量链表上, 并拼接到 resolve pass 捕获的变量链表
// 之前, 使函数的栈帧完整, 且不污染其他函数的链表.
static void splice_locals(LibcpContext* ctx, LibcpObj* fn) {
  LibcpObj* pass2 = locals;
  if (pass2) {
    LibcpObj* tail = pass2;
    while (tail->next)
      tail = tail->next;
    tail->next = obj_locals(fn);
    set_obj_locals(fn, pass2);
  }
}

// The goto and label-value references collected for one function are
// parked in the context (PLAN L3.1b): they are a sema -> codegen
// handoff, not a layer-3/4 field of LibcpObj. The parameter is not named
// `gotos` because sema.c aliases that name to the context slot.
//
// 为一个函数收集的 goto 与标签值引用存放在 context 中 (PLAN L3.1b):
// 它们是 sema 到 codegen 的交接数据, 不是 LibcpObj 的第 3/4 层字段.
// 参数不命名为 `gotos`, 因为 sema.c 把该名字别名到了 context 槽位.
void libcp_sema_store_label_gotos(LibcpContext* ctx, LibcpObj* fn, LibcpNodeArray refs) {
  LibcpLabelGotoList* l = libcp_arena_calloc(ctx, 1, sizeof(LibcpLabelGotoList));
  l->fn                 = fn;
  l->refs               = refs;
  l->next               = ctx->label_gotos;
  ctx->label_gotos      = l;
}

LibcpNodeArray* libcp_sema_label_gotos(LibcpContext* ctx, LibcpObj* fn) {
  // 取回某个函数交给 codegen 的 goto 与标签值引用; 没有则返回 NULL.
  for (LibcpLabelGotoList* l = ctx->label_gotos; l; l = l->next)
    if (l->fn == fn)
      return &l->refs;
  return NULL;
}

static void analyze_function(LibcpContext* ctx, LibcpObj* fn) {
  // The return conversions read the function being annotated, and a
  // nested definition's analysis runs from inside its host's, so the
  // context is saved and restored like the local list.
  //
  // 返回转换会读取正在标注的函数, 而嵌套定义的分析会在其宿主函数内部
  // 运行, 因此上下文像局部变量链表一样被保存并恢复.
  //
  // 对已解析的函数体做标注与控制流检查, 然后交接标签引用并拼接局部
  // 变量.
  LibcpObj* save_fn = sema_fn;
  sema_fn           = fn;
  LibcpObj* save    = locals;
  locals            = NULL;
  libcp_add_type(ctx, fn->body);
  check_control_flow(ctx, fn->body);

  // Hand the collected goto and label-value references to codegen's
  // shaping pass, which pairs them with the labels it allocates. The
  // label values that sit outside the statement tree (block-scope
  // static initializers) make this the only chain that reaches them
  // all.
  //
  // 把收集到的 goto 与标签值引用交给 codegen 的 shaping pass, 由它
  // 与分配的标签配对. 位于语句树之外的标签值 (块作用域 static
  // 初始化器) 使这条链成为唯一能到达全部引用的链.
  libcp_sema_store_label_gotos(ctx, fn, gotos);
  gotos = labels = (LibcpNodeArray){};

  splice_locals(ctx, fn);
  locals  = save;
  sema_fn = save_fn;
}

// Runs semantic analysis over the parser's top-level declaration-record
// chain, in source order, and returns the list of global objects for
// codegen. A function record is resolved first, so that later records
// see its name, and its body is annotated and descended right after -
// the order the parser used to interleave declaration and analysis.
//
// 按源码顺序对解析器的顶层声明记录链运行语义分析, 并返回给 codegen 的
// 全局对象链表. 函数记录先解析, 使后续记录能看到它的名字, 其函数体
// 紧接着标注并遍历 - 与解析器过去交错声明与分析时的顺序一致.
LibcpObj* libcp_sema(LibcpContext* ctx, LibcpNode* toplevel) {
  ENTER_RET(ctx, NULL);
  sema_init(ctx);
  declare_builtin_functions(ctx);

  for (LibcpNode* n = toplevel; n; n = n->next) {
    switch (n->kind) {
      case ND_TYPEDEF:
      case ND_ENUM_CONST: resolve_node(ctx, n); break;
      case ND_GVAR_DECL:
        resolve_node(ctx, n);
        serialize_gvar(ctx, n);
        break;
      case ND_FUNCDEF:
        resolve_function(ctx, n);
        if (n->body)
          analyze_function(ctx, n->var);
        break;
      default: unreachable();
    }
  }

  // Mark the reachable functions live and drop the redundant tentative
  // definitions.
  //
  // 把可达函数标记为存活, 并删除多余的暂定定义.
  finalize_globals(ctx);
  LEAVE(ctx);
  return globals;
}
