#include "test.h"

// A pointer written while its tag is still incomplete, plus a definition
// of that tag with no declarator of its own: the subscript through the
// pointer is then the first thing that needs the size.
struct BareHolder {
  struct Bare *p;
};

struct Bare {
  int a, b, c, d, e, f;
};

static int bare_read(struct BareHolder *h, int i) {
  return h->p[i].a;
}

static struct Bare bare_objs[2];

// PLAN A10.2: a ten-int return exercises the caller-owned buffer.
struct RetBuf { int a[10]; };
struct RetBuf ret_buf(int n) { struct RetBuf b; b.a[0] = n; b.a[9] = n * 2; return b; }

int main() {
  ASSERT(1, ({ struct {int a; int b;} x; x.a=1; x.b=2; x.a; }));
  ASSERT(2, ({ struct {int a; int b;} x; x.a=1; x.b=2; x.b; }));
  ASSERT(1, ({ struct {char a; int b; char c;} x; x.a=1; x.b=2; x.c=3; x.a; }));
  ASSERT(2, ({ struct {char a; int b; char c;} x; x.b=1; x.b=2; x.c=3; x.b; }));
  ASSERT(3, ({ struct {char a; int b; char c;} x; x.a=1; x.b=2; x.c=3; x.c; }));

  ASSERT(0, ({ struct {char a; char b;} x[3]; char *p=x; p[0]=0; x[0].a; }));
  ASSERT(1, ({ struct {char a; char b;} x[3]; char *p=x; p[1]=1; x[0].b; }));
  ASSERT(2, ({ struct {char a; char b;} x[3]; char *p=x; p[2]=2; x[1].a; }));
  ASSERT(3, ({ struct {char a; char b;} x[3]; char *p=x; p[3]=3; x[1].b; }));

  ASSERT(6, ({ struct {char a[3]; char b[5];} x; char *p=&x; x.a[0]=6; p[0]; }));
  ASSERT(7, ({ struct {char a[3]; char b[5];} x; char *p=&x; x.b[0]=7; p[3]; }));

  ASSERT(6, ({ struct { struct { char b; } a; } x; x.a.b=6; x.a.b; }));

  // A member of an anonymous struct or union belongs to the enclosing
  // namespace, so the access chain runs through the anonymous link.
  ASSERT(3, ({ struct { struct {int a;}; int b; } x; x.a=3; x.b=4; x.a; }));
  ASSERT(4, ({ struct { struct {int a;}; int b; } x; x.a=3; x.b=4; x.b; }));
  ASSERT(5, ({ struct { struct {int a;}; int b; } x, *p=&x; p->a=5; p->b=6; x.a; }));
  ASSERT(6, ({ struct { struct {int a;}; int b; } x, *p=&x; p->a=5; p->b=6; x.b; }));
  ASSERT(7, ({ struct { struct { struct {int a;}; }; } x; x.a=7; x.a; }));
  ASSERT(8, ({ struct { union { int a; struct {int b;}; }; int c; } x; x.b=8; x.b; }));
  ASSERT(9, ({ typedef struct { struct {int a;}; } T; T x, y; x.a=9; y=x; y.a; }));
  ASSERT(10, ({ struct { struct {int a;} s; } x; x.s.a=10; x.s.a; }));
  ASSERT(11, ({ struct { struct {int a;}; int b; } x; x.a=11; (&x.a)[0]; }));
  ASSERT(3, ({ struct { struct {int a;}; int b; } x, *p=&x; p->a=1; p->a+=2; x.a; }));
  ASSERT(3, ({ struct {int a;} x, *p=&x; p->a=1; p->a+=2; x.a; }));
  ASSERT(4, ({ struct {int a;} x, *p=&x; p->a=3; p->a++; x.a; }));
  ASSERT(13, ({ struct { struct {int a;}; int b; } x; x.a=12; x.a++; x.a; }));
  ASSERT(2, ({ struct { struct { unsigned a : 2; }; int b; } x; x.a=1; x.a+=1; x.a; }));
  ASSERT(4, ({ struct { struct {int a;}; int b; } x; x.a=13; sizeof(x.a); }));
  ASSERT(29, ({ struct { struct {int a;}; int b; } x={14,15}; x.a+x.b; }));
  ASSERT(16, ({ struct { struct {int a;}; int b; } x; int *p=&x.a; *p=16; x.a; }));

  ASSERT(4, ({ struct {int a;} x; sizeof(x); }));
  ASSERT(8, ({ struct {int a; int b;} x; sizeof(x); }));
  ASSERT(8, ({ struct {int a, b;} x; sizeof(x); }));
  ASSERT(12, ({ struct {int a[3];} x; sizeof(x); }));
  ASSERT(16, ({ struct {int a;} x[4]; sizeof(x); }));
  ASSERT(24, ({ struct {int a[3];} x[2]; sizeof(x); }));
  ASSERT(24, ({ struct S {int a; char b;} x[3]; sizeof(x); }));
  ASSERT(16, ({ struct {struct {char c;} i; int m;} y[2]; sizeof(y); }));
  ASSERT(2, ({ struct {char a; char b;} x; sizeof(x); }));
  ASSERT(0, ({ struct {} x; sizeof(x); }));
  ASSERT(8, ({ struct {char a; int b;} x; sizeof(x); }));
  ASSERT(8, ({ struct {int a; char b;} x; sizeof(x); }));

  ASSERT(8, ({ struct t {int a; int b;} x; struct t y; sizeof(y); }));
  ASSERT(8, ({ struct t {int a; int b;}; struct t y; sizeof(y); }));
  ASSERT(2, ({ struct t {char a[2];}; { struct t {char a[4];}; } struct t y; sizeof(y); }));
  ASSERT(3, ({ struct t {int x;}; int t=1; struct t y; y.x=2; t+y.x; }));

  ASSERT(3, ({ struct t {char a;} x; struct t *y = &x; x.a=3; y->a; }));
  ASSERT(3, ({ struct t {char a;} x; struct t *y = &x; y->a=3; x.a; }));

  ASSERT(3, ({ struct {int a,b;} x,y; x.a=3; y=x; y.a; }));
  ASSERT(7, ({ struct t {int a,b;}; struct t x; x.a=7; struct t y; struct t *z=&y; *z=x; y.a; }));
  ASSERT(7, ({ struct t {int a,b;}; struct t x; x.a=7; struct t y, *p=&x, *q=&y; *q=*p; y.a; }));
  ASSERT(5, ({ struct t {char a, b;} x, y; x.a=5; y=x; y.a; }));

  ASSERT(3, ({ struct {int a,b;} x,y; x.a=3; y=x; y.a; }));
  ASSERT(7, ({ struct t {int a,b;}; struct t x; x.a=7; struct t y; struct t *z=&y; *z=x; y.a; }));
  ASSERT(7, ({ struct t {int a,b;}; struct t x; x.a=7; struct t y, *p=&x, *q=&y; *q=*p; y.a; }));
  ASSERT(5, ({ struct t {char a, b;} x, y; x.a=5; y=x; y.a; }));

  ASSERT(8, ({ struct t {int a; int b;} x; struct t y; sizeof(y); }));
  ASSERT(8, ({ struct t {int a; int b;}; struct t y; sizeof(y); }));

  ASSERT(16, ({ struct {char a; long b;} x; sizeof(x); }));
  ASSERT(4, ({ struct {char a; short b;} x; sizeof(x); }));

  ASSERT(8, ({ struct foo *bar; sizeof(bar); }));
  ASSERT(4, ({ struct T *foo; struct T {int x;}; sizeof(struct T); }));
  ASSERT(1, ({ struct T { struct T *next; int x; } a; struct T b; b.x=1; a.next=&b; a.next->x; }));
  ASSERT(4, ({ typedef struct T T; struct T { int x; }; sizeof(T); }));

  ASSERT(24, sizeof(struct Bare));
  bare_objs[1].a = 7;
  struct BareHolder h = { bare_objs };
  ASSERT(7, bare_read(&h, 1));
  ASSERT(2, ({ struct {int a;} x={1}, y={2}; (x=y).a; }));
  ASSERT(1, ({ struct {int a;} x={1}, y={2}; (1?x:y).a; }));
  ASSERT(2, ({ struct {int a;} x={1}, y={2}; (0?x:y).a; }));

  // Tag lookup walks the enclosing scopes, and a tag defined in an
  // inner block shadows the outer one only there.
  ASSERT(1, ({ struct S {int a;}; long r; { struct S {char b;}; struct S y; r = sizeof(y); } r; }));
  ASSERT(8, ({ struct S2 {int a;}; long r; { struct S2 *p; r = sizeof(p); } r; }));

  // A tag defined in a block is laid out from the record's position in
  // that block's chain, so a variable of the type declared right after
  // it sees the completed layout.
  ASSERT(9, ({ struct Tag { int x; }; struct Tag t; t.x=9; t.x; }));

  // PLAN A10.2: the caller-owned return buffer - a slot the shaping
  // pass creates - across initializer, assignment and chained calls.
  ASSERT(3, ({ struct RetBuf b = ret_buf(3); b.a[0]; }));
  ASSERT(6, ({ struct RetBuf b = ret_buf(3); b.a[9]; }));
  ASSERT(8, ({ struct RetBuf b; b = ret_buf(4); b.a[9]; }));
  ASSERT(15, (ret_buf(5).a[9] + ret_buf(3).a[0] + 2));

  printf("OK\n");
  return 0;
}
