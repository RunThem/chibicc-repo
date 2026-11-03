// 类型构造与基础查询, 是第 3 层与第 4 层共用的类型系统地基: 内建类型
// 单例, 整数 / 浮点判定, 类型兼容性比较, 以及指针, 函数, 数组, VLA 与
// typeof 占位等类型构造. 新类型一律从 context 的 arena 分配, 只有内建
// 类型是共享单例.
#include "libcp.h"

// The built-in types are shared by every context and are read-only: a
// declarator never writes its name into a type record (parse.c keeps
// the name in a per-declaration carrier), so two contexts parsing in
// different threads never write the same record.
//
// 内建类型由所有 context 共享且只读: 声明符从不把名字写进类型记录
// (parse.c 把名字留在每次声明的载体里), 因此在不同线程里解析的两个
// context 不会写同一条记录.
LibcpType* libcp_ty_void = &(LibcpType){TY_VOID, 1, 1};
LibcpType* libcp_ty_bool = &(LibcpType){TY_BOOL, 1, 1};

LibcpType* libcp_ty_char  = &(LibcpType){TY_CHAR, 1, 1};
LibcpType* libcp_ty_short = &(LibcpType){TY_SHORT, 2, 2};
LibcpType* libcp_ty_int   = &(LibcpType){TY_INT, 4, 4};
LibcpType* libcp_ty_long  = &(LibcpType){TY_LONG, 8, 8};

LibcpType* libcp_ty_uchar  = &(LibcpType){TY_CHAR, 1, 1, true};
LibcpType* libcp_ty_ushort = &(LibcpType){TY_SHORT, 2, 2, true};
LibcpType* libcp_ty_uint   = &(LibcpType){TY_INT, 4, 4, true};
LibcpType* libcp_ty_ulong  = &(LibcpType){TY_LONG, 8, 8, true};

LibcpType* libcp_ty_float   = &(LibcpType){TY_FLOAT, 4, 4};
LibcpType* libcp_ty_double  = &(LibcpType){TY_DOUBLE, 8, 8};
LibcpType* libcp_ty_ldouble = &(LibcpType){TY_LDOUBLE, 16, 16};

// 分配并清零一条类型记录, 填入 kind / size / align; 其余字段交给调用者.
static LibcpType* new_type(LibcpContext* ctx, LibcpTypeKind kind, int size, int align) {
  LibcpType* ty = libcp_arena_calloc(ctx, 1, sizeof(LibcpType));
  ty->kind      = kind;
  ty->size      = size;
  ty->align     = align;
  return ty;
}

// 整数类包含 enum; 有符号与否不影响归类.
bool libcp_is_integer(LibcpType* ty) {
  LibcpTypeKind k = ty->kind;
  return k == TY_BOOL || k == TY_CHAR || k == TY_SHORT || k == TY_INT || k == TY_LONG ||
         k == TY_ENUM;
}

bool libcp_is_flonum(LibcpType* ty) {
  return ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE || ty->kind == TY_LDOUBLE;
}

bool libcp_is_numeric(LibcpType* ty) {
  return libcp_is_integer(ty) || libcp_is_flonum(ty);
}

// 结构化比较两个类型. 先沿 origin 链还原: copy_type 产生的副本与其原始
// 类型等价. 指针与数组递归比较元素类型; 函数比较返回类型, 可变参数标志
// 与形参列表; 数组还要求两侧长度都为未知 (-1). 其余种类 (struct / union /
// void 等) 只能经由指针相等或 origin 链判定.
bool libcp_is_compatible(LibcpType* t1, LibcpType* t2) {
  if (t1 == t2)
    return true;

  if (t1->origin)
    return libcp_is_compatible(t1->origin, t2);

  if (t2->origin)
    return libcp_is_compatible(t1, t2->origin);

  if (t1->kind != t2->kind)
    return false;

  switch (t1->kind) {
    case TY_CHAR:
    case TY_SHORT:
    case TY_INT:
    case TY_LONG: return t1->is_unsigned == t2->is_unsigned;
    case TY_FLOAT:
    case TY_DOUBLE:
    case TY_LDOUBLE: return true;
    case TY_PTR: return libcp_is_compatible(t1->base, t2->base);
    case TY_FUNC: {
      if (!libcp_is_compatible(t1->return_ty, t2->return_ty))
        return false;
      if (t1->is_variadic != t2->is_variadic)
        return false;

      LibcpType* p1 = t1->params;
      LibcpType* p2 = t2->params;
      for (; p1 && p2; p1 = p1->next, p2 = p2->next)
        if (!libcp_is_compatible(p1, p2))
          return false;
      return p1 == NULL && p2 == NULL;
    }
    case TY_ARRAY:
      if (!libcp_is_compatible(t1->base, t2->base))
        return false;
      return t1->array_len < 0 && t2->array_len < 0 && t1->array_len == t2->array_len;
  }
  return false;
}

// 浅拷贝类型记录并记下 origin: 副本可以安全改写, 比较时仍等价于原类型.
LibcpType* libcp_copy_type(LibcpContext* ctx, LibcpType* ty) {
  LibcpType* ret = libcp_arena_calloc(ctx, 1, sizeof(LibcpType));
  *ret           = *ty;
  ret->origin    = ty;
  return ret;
}

// 指针一律 8 字节大小与对齐, 并标记为无符号, 以适配指针运算.
LibcpType* libcp_pointer_to(LibcpContext* ctx, LibcpType* base) {
  LibcpType* ty   = new_type(ctx, TY_PTR, 8, 8);
  ty->base        = base;
  ty->is_unsigned = true;
  return ty;
}

LibcpType* libcp_func_type(LibcpContext* ctx, LibcpType* return_ty) {
  // The C spec disallows sizeof(<function type>), but
  // GCC allows that and the expression is evaluated to 1.
  //
  // C 标准不允许 sizeof(<函数类型>), 但 GCC 允许, 且该表达式的值为 1,
  // 因此函数类型的大小与对齐都记作 1 而不是 0.
  LibcpType* ty = new_type(ctx, TY_FUNC, 1, 1);
  ty->return_ty = return_ty;
  return ty;
}

LibcpType* libcp_array_of(LibcpContext* ctx, LibcpType* base, int len) {
  // A negative length asks for an incomplete array, and a negative base
  // size means the element type has no size yet - a dimension the parser
  // left for sema to decide is one way to get there. The product of the
  // two would look like a complete type, so an array built on an element
  // it cannot measure stays unmeasurable.
  //
  // 负长度表示不完整数组; 元素类型的 size 为负则说明它还没有大小 -
  // 解析器把某个维度留给 sema 决定就会走到这类情况. 两者相乘可能看起来
  // 像一个完整类型, 因此建立在无法度量元素之上的数组保持不可度量.
  int size = base->size * len;
  if (size >= 0 && (base->size < 0 || len < 0))
    size = -1;

  LibcpType* ty = new_type(ctx, TY_ARRAY, size, base->align);
  ty->base      = base;
  ty->array_len = len;
  return ty;
}

// VLA: 记录元素类型与长度表达式; 布局在运行时按实际长度决定,
// 类型记录本身按 8 字节大小与对齐记账.
LibcpType* libcp_vla_of(LibcpContext* ctx, LibcpType* base, LibcpNode* len) {
  LibcpType* ty = new_type(ctx, TY_VLA, 8, 8);
  ty->base      = base;
  ty->vla_len   = len;
  return ty;
}

// An array whose dimension the parser could not decide: `dim` is the
// length expression, which sema evaluates into a length - or, when it is
// not a constant expression, turns the type into a VLA.
//
// 维度尚未由解析器决定的数组: `dim` 是长度表达式, sema 会把它求值成
// 长度; 当它不是常量表达式时, sema 把该类型改判为 VLA.
LibcpType* libcp_array_of_dim(LibcpContext* ctx, LibcpType* base, LibcpNode* dim) {
  LibcpType* ty = libcp_array_of(ctx, base, -1);
  ty->dim_len   = dim;
  return ty;
}

// The type of a `typeof(expr)`, until sema annotates the operand and
// fills this record in. Its size is negative like an incomplete type's,
// so that a record nobody completed is rejected rather than measured.
//
// `typeof(expr)` 的类型占位, 等待 sema 标注其操作数并补全这条记录.
// 它的大小为负, 和不完整类型一样, 这样没人补全的记录会被拒绝, 而不会
// 被当作可度量的类型.
LibcpType* libcp_typeof_placeholder(LibcpContext* ctx) {
  return new_type(ctx, TY_TYPEOF, -1, 1);
}

// enum 在布局上等同 int: 4 字节大小与对齐.
LibcpType* libcp_enum_type(LibcpContext* ctx) {
  return new_type(ctx, TY_ENUM, 4, 4);
}

// struct / union 的初始记录: 大小 0, 对齐 1; 成员由解析器填入, 大小与
// 对齐在 sema 布局该聚合时确定.
LibcpType* libcp_struct_type(LibcpContext* ctx) {
  return new_type(ctx, TY_STRUCT, 0, 1);
}
