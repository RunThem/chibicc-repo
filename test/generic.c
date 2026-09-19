#include "test.h"

int main() {
  ASSERT(1, _Generic(100.0, double: 1, int *: 2, int: 3, float: 4));
  ASSERT(2, _Generic((int *)0, double: 1, int *: 2, int: 3, float: 4));
  ASSERT(2, _Generic((int[3]){}, double: 1, int *: 2, int: 3, float: 4));
  ASSERT(3, _Generic(100, double: 1, int *: 2, int: 3, float: 4));
  ASSERT(4, _Generic(100f, double: 1, int *: 2, int: 3, float: 4));

  // A "default:" association is a fallback: a matching type wins over it
  // wherever the two appear, and the default is taken only when no type
  // has matched so far.
  ASSERT(1, _Generic(1, int: 1, double: 2, default: 3));
  ASSERT(6, _Generic(1, float: 4, default: 5, int: 6));
  ASSERT(3, _Generic(1.0f, int: 1, double: 2, default: 3));

  // The selection is made on the controlling expression's type, and the
  // expression itself is not evaluated, so a generic selection serves
  // wherever a constant expression is required.
  ASSERT(4, ({ int a[_Generic(1, int: 4, default: 5)]; sizeof(a) / sizeof(a[0]); }));
  ASSERT(7, ({ enum { E = _Generic(1, int: 7, default: 8) }; E; }));
  ASSERT(11, ({ int r=0; switch (2) { case _Generic(1, int: 2, default: 3): r=11; break; default: r=22; } r; }));
  ASSERT(1, _Generic(1, default: sizeof(int), int: 1));

  // The result of the selection is the association's expression, in
  // every shape - including a `&&label`, whose identity the selection
  // must hand over to the label resolution.
  ASSERT(7, ({ int r = 0; void *t = _Generic(1, default: &&lbl); goto *t; r = 9; lbl: r = 7; r; }));
  ASSERT(5, _Generic(1, int: _Generic(1.0, double: 5, default: 6), default: 7));
  ASSERT(2, _Generic((int[3]){0}, int *: 2, int: 3, default: 4));

  printf("OK\n");
  return 0;
}
