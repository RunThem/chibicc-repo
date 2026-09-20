#include "test.h"

typedef int MyInt, MyInt2[4];
typedef int;

static int fparam(MyInt MyInt) {
  MyInt = MyInt + 1;
  return MyInt;
}

int main() {
  ASSERT(1, ({ typedef int t; t x=1; x; }));
  ASSERT(1, ({ typedef struct {int a;} t; t x; x.a=1; x.a; }));
  ASSERT(1, ({ typedef int t; t t=1; t; }));
  ASSERT(2, ({ typedef struct {int a;} t; { typedef int t; } t x; x.a=2; x.a; }));
  ASSERT(4, ({ typedef t; t x; sizeof(x); }));
  ASSERT(3, ({ MyInt x=3; x; }));
  ASSERT(16, ({ MyInt2 x; sizeof(x); }));
  ASSERT(6, fparam(5));
  ASSERT(2, ({ MyInt MyInt = 2; MyInt; }));

  // The typedef/tag oracle is the parser's own scope stack: a name a
  // declarator declares shadows a typedef of the same spelling until the
  // block ends, and a for-init declarator shadows one for the loop.
  ASSERT(3, ({ typedef int t; int s=0; for (int t=0; t<3; t++) s+=t; s; }));
  ASSERT(6, ({ typedef int U; long r; { int U = 2; r = U; } U x = 4; r + x; }));
  ASSERT(5, ({ long r; { typedef int V; V x = 5; r = x; } r; }));

  // A typedef record carries the type sema completes at the record's own
  // position: a VLA behind the name is still a VLA where it is used.
  ASSERT(7, ({ int n=7; typedef int V[n]; V v; v[0]=7; v[0]; }));
  ASSERT(5, ({ int n=3; typedef int W[n][n]; W w; w[2][1]=5; w[2][1]; }));

  printf("OK\n");
  return 0;
}
