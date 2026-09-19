#include "test.h"

int main() {
  ASSERT(1, __builtin_types_compatible_p(int, int));
  ASSERT(1, __builtin_types_compatible_p(double, double));
  ASSERT(0, __builtin_types_compatible_p(int, long));
  ASSERT(0, __builtin_types_compatible_p(long, float));
  ASSERT(1, __builtin_types_compatible_p(int *, int *));
  ASSERT(0, __builtin_types_compatible_p(short *, int *));
  ASSERT(0, __builtin_types_compatible_p(int **, int *));
  ASSERT(1, __builtin_types_compatible_p(const int, int));
  ASSERT(0, __builtin_types_compatible_p(unsigned, int));
  ASSERT(1, __builtin_types_compatible_p(signed, int));
  ASSERT(0, __builtin_types_compatible_p(struct {int a;}, struct {int a;}));

  ASSERT(1, __builtin_types_compatible_p(int (*)(void), int (*)(void)));
  ASSERT(1, __builtin_types_compatible_p(void (*)(int), void (*)(int)));
  ASSERT(1, __builtin_types_compatible_p(void (*)(int, double), void (*)(int, double)));
  ASSERT(1, __builtin_types_compatible_p(int (*)(float, double), int (*)(float, double)));
  ASSERT(0, __builtin_types_compatible_p(int (*)(float, double), int));
  ASSERT(0, __builtin_types_compatible_p(int (*)(float, double), int (*)(float)));
  ASSERT(0, __builtin_types_compatible_p(int (*)(float, double), int (*)(float, double, int)));
  ASSERT(1, __builtin_types_compatible_p(double (*)(...), double (*)(...)));
  ASSERT(0, __builtin_types_compatible_p(double (*)(...), double (*)(void)));

  ASSERT(1, ({ typedef struct {int a;} T; __builtin_types_compatible_p(T, T); }));
  ASSERT(1, ({ typedef struct {int a;} T; __builtin_types_compatible_p(T, const T); }));

  ASSERT(1, ({ struct {int a; int b;} x; __builtin_types_compatible_p(typeof(x.a), typeof(x.b)); }));

  // __builtin_reg_class folds to the register class of its type operand:
  // integer or pointer, floating-point, or anything else.
  ASSERT(0, __builtin_reg_class(int));
  ASSERT(0, __builtin_reg_class(unsigned char));
  ASSERT(0, __builtin_reg_class(int *));
  ASSERT(1, __builtin_reg_class(float));
  ASSERT(1, __builtin_reg_class(double));
  ASSERT(1, __builtin_reg_class(long double));
  ASSERT(2, __builtin_reg_class(struct {int a;}));
  ASSERT(2, __builtin_reg_class(void));

  // Both folds are constant expressions.
  ASSERT(3, ({ int a[__builtin_types_compatible_p(int, int) + 2]; sizeof(a) / sizeof(a[0]); }));
  ASSERT(1, __builtin_reg_class(int) + __builtin_reg_class(double));

  printf("OK\n");
  return 0;
}
