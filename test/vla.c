#include "test.h"

int main() {
  ASSERT(20, ({ int n=5; int x[n]; sizeof(x); }));
  ASSERT((5+1)*(8*2)*4, ({ int m=5, n=8; int x[m+1][n*2]; sizeof(x); }));

  ASSERT(8, ({ char n=10; int (*x)[n][n+2]; sizeof(x); }));
  ASSERT(480, ({ char n=10; int (*x)[n][n+2]; sizeof(*x); }));
  ASSERT(48, ({ char n=10; int (*x)[n][n+2]; sizeof(**x); }));
  ASSERT(4, ({ char n=10; int (*x)[n][n+2]; sizeof(***x); }));

  ASSERT(60, ({ char n=3; int x[5][n]; sizeof(x); }));
  ASSERT(12, ({ char n=3; int x[5][n]; sizeof(*x); }));

  ASSERT(60, ({ char n=3; int x[n][5]; sizeof(x); }));
  ASSERT(20, ({ char n=3; int x[n][5]; sizeof(*x); }));

  ASSERT(0, ({ int n=10; int x[n+1][n+6]; int *p=x; for (int i = 0; i<sizeof(x)/4; i++) p[i]=i; x[0][0]; }));
  ASSERT(5, ({ int n=10; int x[n+1][n+6]; int *p=x; for (int i = 0; i<sizeof(x)/4; i++) p[i]=i; x[0][5]; }));
  ASSERT(5*16+2, ({ int n=10; int x[n+1][n+6]; int *p=x; for (int i = 0; i<sizeof(x)/4; i++) p[i]=i; x[5][2]; }));

  ASSERT(10, ({ int n=5; sizeof(char[2][n]); }));

  // A declaration whose type holds no VLA and that has no initializer
  // emits no statement; one that needs a size emits a single statement
  // with the computation sequenced before the alloca assignment or the
  // initializer chain. A pointer to a VLA has the size computed at its
  // own declaration, which is what the later subscript scaling reads.
  ASSERT(7, ({ int n=3; int v[n]; int (*p)[n] = &v; (*p)[2] = 7; v[2]; }));
  ASSERT(10, ({ int n=4; int (*p)[n]; int v[n]; p=&v; (*p)[1]=5; (*p)[1]+v[1]; }));
  ASSERT(7, ({ int n=3; int v[n][n]; int (*p)[n][n] = &v; (*p)[1][2] = 7; v[1][2]; }));
  ASSERT(3, ({ int n=2; int v[n][n]; v[1][0]=3; v[1][0]; }));
  ASSERT(3, ({ int x; int y; x=1; y=2; x+y; }));
  ASSERT(3, ({ int s=0; for (int x; ; ) { s++; if (s==3) break; } s; }));

  printf("OK\n");
  return 0;
}
