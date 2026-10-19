#include "test.h"

// A designated initializer reaches through an anonymous union, in both
// the global and the block-scope paths.
struct AnonUnion {
  union { int a; float b; };
};
struct AnonUnion g_anon_union = { .a = 2 };

int main() {
  ASSERT(8, ({ union { int a; char b[6]; } x; sizeof(x); }));
  ASSERT(3, ({ union { int a; char b[4]; } x; x.a = 515; x.b[0]; }));
  ASSERT(2, ({ union { int a; char b[4]; } x; x.a = 515; x.b[1]; }));
  ASSERT(0, ({ union { int a; char b[4]; } x; x.a = 515; x.b[2]; }));
  ASSERT(0, ({ union { int a; char b[4]; } x; x.a = 515; x.b[3]; }));

  ASSERT(3, ({ union {int a,b;} x,y; x.a=3; y.a=5; y=x; y.a; }));
  ASSERT(3, ({ union {struct {int a,b;} c;} x,y; x.c.b=3; y.c.b=5; y=x; y.c.b; }));

  ASSERT(0xef, ({ union { struct { unsigned char a,b,c,d; }; long e; } x; x.e=0xdeadbeef; x.a; }));
  ASSERT(0xbe, ({ union { struct { unsigned char a,b,c,d; }; long e; } x; x.e=0xdeadbeef; x.b; }));
  ASSERT(0xad, ({ union { struct { unsigned char a,b,c,d; }; long e; } x; x.e=0xdeadbeef; x.c; }));
  ASSERT(0xde, ({ union { struct { unsigned char a,b,c,d; }; long e; } x; x.e=0xdeadbeef; x.d; }));

  ASSERT(3, ({struct { union { int a,b; }; union { int c,d; }; } x; x.a=3; x.b; }));
  ASSERT(5, ({struct { union { int a,b; }; union { int c,d; }; } x; x.d=5; x.c; }));

  ASSERT(2, g_anon_union.a);
  ASSERT(3, ({ struct AnonUnion x = { .a = 3 }; x.a; }));
  ASSERT(4, ({ struct { union { int a; }; } x = { .a = 4 }; x.a; }));
  ASSERT(5, ({ struct { struct { union { int a; }; }; } x = { .a = 5 }; x.a; }));
  ASSERT(6, ({ union { union { int a; float b; }; } x = { .a = 6 }; x.a; }));

  printf("OK\n");
  return 0;
}
