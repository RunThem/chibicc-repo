#include "libcp.h"

// The built-in types are shared by every context and are read-only: a
// declarator never writes its name into a type record (parse.c keeps
// the name in a per-declaration carrier), so two contexts parsing in
// different threads never write the same record.
LibcpType *libcp_ty_void = &(LibcpType){TY_VOID, 1, 1};
LibcpType *libcp_ty_bool = &(LibcpType){TY_BOOL, 1, 1};

LibcpType *libcp_ty_char = &(LibcpType){TY_CHAR, 1, 1};
LibcpType *libcp_ty_short = &(LibcpType){TY_SHORT, 2, 2};
LibcpType *libcp_ty_int = &(LibcpType){TY_INT, 4, 4};
LibcpType *libcp_ty_long = &(LibcpType){TY_LONG, 8, 8};

LibcpType *libcp_ty_uchar = &(LibcpType){TY_CHAR, 1, 1, true};
LibcpType *libcp_ty_ushort = &(LibcpType){TY_SHORT, 2, 2, true};
LibcpType *libcp_ty_uint = &(LibcpType){TY_INT, 4, 4, true};
LibcpType *libcp_ty_ulong = &(LibcpType){TY_LONG, 8, 8, true};

LibcpType *libcp_ty_float = &(LibcpType){TY_FLOAT, 4, 4};
LibcpType *libcp_ty_double = &(LibcpType){TY_DOUBLE, 8, 8};
LibcpType *libcp_ty_ldouble = &(LibcpType){TY_LDOUBLE, 16, 16};

static LibcpType *new_type(LibcpContext *ctx, LibcpTypeKind kind, int size, int align) {
  LibcpType *ty = libcp_arena_calloc(ctx, 1, sizeof(LibcpType));
  ty->kind = kind;
  ty->size = size;
  ty->align = align;
  return ty;
}

bool libcp_is_integer(LibcpType *ty) {
  LibcpTypeKind k = ty->kind;
  return k == TY_BOOL || k == TY_CHAR || k == TY_SHORT ||
         k == TY_INT  || k == TY_LONG || k == TY_ENUM;
}

bool libcp_is_flonum(LibcpType *ty) {
  return ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE ||
         ty->kind == TY_LDOUBLE;
}

bool libcp_is_numeric(LibcpType *ty) {
  return libcp_is_integer(ty) || libcp_is_flonum(ty);
}

bool libcp_is_compatible(LibcpType *t1, LibcpType *t2) {
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
  case TY_LONG:
    return t1->is_unsigned == t2->is_unsigned;
  case TY_FLOAT:
  case TY_DOUBLE:
  case TY_LDOUBLE:
    return true;
  case TY_PTR:
    return libcp_is_compatible(t1->base, t2->base);
  case TY_FUNC: {
    if (!libcp_is_compatible(t1->return_ty, t2->return_ty))
      return false;
    if (t1->is_variadic != t2->is_variadic)
      return false;

    LibcpType *p1 = t1->params;
    LibcpType *p2 = t2->params;
    for (; p1 && p2; p1 = p1->next, p2 = p2->next)
      if (!libcp_is_compatible(p1, p2))
        return false;
    return p1 == NULL && p2 == NULL;
  }
  case TY_ARRAY:
    if (!libcp_is_compatible(t1->base, t2->base))
      return false;
    return t1->array_len < 0 && t2->array_len < 0 &&
           t1->array_len == t2->array_len;
  }
  return false;
}

LibcpType *libcp_copy_type(LibcpContext *ctx, LibcpType *ty) {
  LibcpType *ret = libcp_arena_calloc(ctx, 1, sizeof(LibcpType));
  *ret = *ty;
  ret->origin = ty;
  return ret;
}

LibcpType *libcp_pointer_to(LibcpContext *ctx, LibcpType *base) {
  LibcpType *ty = new_type(ctx, TY_PTR, 8, 8);
  ty->base = base;
  ty->is_unsigned = true;
  return ty;
}

LibcpType *libcp_func_type(LibcpContext *ctx, LibcpType *return_ty) {
  // The C spec disallows sizeof(<function type>), but
  // GCC allows that and the expression is evaluated to 1.
  LibcpType *ty = new_type(ctx, TY_FUNC, 1, 1);
  ty->return_ty = return_ty;
  return ty;
}

LibcpType *libcp_array_of(LibcpContext *ctx, LibcpType *base, int len) {
  // A negative length asks for an incomplete array, and a negative base
  // size means the element type has no size yet - a dimension the parser
  // left for sema to decide is one way to get there. The product of the
  // two would look like a complete type, so an array built on an element
  // it cannot measure stays unmeasurable.
  int size = base->size * len;
  if (size >= 0 && (base->size < 0 || len < 0))
    size = -1;

  LibcpType *ty = new_type(ctx, TY_ARRAY, size, base->align);
  ty->base = base;
  ty->array_len = len;
  return ty;
}

LibcpType *libcp_vla_of(LibcpContext *ctx, LibcpType *base, LibcpNode *len) {
  LibcpType *ty = new_type(ctx, TY_VLA, 8, 8);
  ty->base = base;
  ty->vla_len = len;
  return ty;
}

// An array whose dimension the parser could not decide: `dim` is the
// length expression, which sema evaluates into a length - or, when it is
// not a constant expression, turns the type into a VLA.
LibcpType *libcp_array_of_dim(LibcpContext *ctx, LibcpType *base, LibcpNode *dim) {
  LibcpType *ty = libcp_array_of(ctx, base, -1);
  ty->dim_len = dim;
  return ty;
}

// The type of a `typeof(expr)`, until sema annotates the operand and
// fills this record in. Its size is negative like an incomplete type's,
// so that a record nobody completed is rejected rather than measured.
LibcpType *libcp_typeof_placeholder(LibcpContext *ctx) {
  return new_type(ctx, TY_TYPEOF, -1, 1);
}

LibcpType *libcp_enum_type(LibcpContext *ctx) {
  return new_type(ctx, TY_ENUM, 4, 4);
}

LibcpType *libcp_struct_type(LibcpContext *ctx) {
  return new_type(ctx, TY_STRUCT, 0, 1);
}
