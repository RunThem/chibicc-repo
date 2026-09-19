#include "test.h"

int main() {
  ASSERT(3, ({ typeof(int) x=3; x; }));
  ASSERT(3, ({ typeof(1) x=3; x; }));
  ASSERT(4, ({ int x; typeof(x) y; sizeof(y); }));
  ASSERT(8, ({ int x; typeof(&x) y; sizeof(y); }));
  ASSERT(4, ({ typeof("foo") x; sizeof(x); }));
  ASSERT(12, sizeof(typeof(struct { int a,b,c; })));

  // A `typeof(expr)` records the operand; its type is annotated by sema.
  ASSERT(4, ({ typeof(1.0f) x; sizeof(x); }));
  ASSERT(8, ({ typeof(&"foo"[0]) p; sizeof(p); }));
  ASSERT(12, ({ typeof(int[3]) *p; sizeof(*p); }));
  ASSERT(20, ({ int a[5]; typeof(a) *p; sizeof(*p); }));
  ASSERT(12, ({ struct Q { int a[3]; } q; typeof(q.a) *p; sizeof(*p); }));
  ASSERT(4, ({ typeof((typeof(int))0) z; sizeof(z); }));
  ASSERT(8, ({ int m = 0; typeof(&m) p = &m; *p = 8; m; }));

  printf("OK\n");
  return 0;
}
