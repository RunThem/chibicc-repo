#include "test.h"

int main() {
  ASSERT(3, ({ int x=3; *&x; }));
  ASSERT(3, ({ int x=3; int *y=&x; int **z=&y; **z; }));
  ASSERT(5, ({ int x=3; int y=5; *(&x+1); }));
  ASSERT(3, ({ int x=3; int y=5; *(&y-1); }));
  ASSERT(5, ({ int x=3; int y=5; *(&x-(-1)); }));
  ASSERT(5, ({ int x=3; int *y=&x; *y=5; x; }));
  ASSERT(7, ({ int x=3; int y=5; *(&x+1)=7; y; }));
  ASSERT(7, ({ int x=3; int y=5; *(&y-2+1)=7; x; }));
  ASSERT(5, ({ int x=3; (&x+2)-&x+3; }));
  ASSERT(8, ({ int x, y; x=3; y=5; x+y; }));
  ASSERT(8, ({ int x=3, y=5; x+y; }));

  ASSERT(3, ({ int x[2]; int *y=&x; *y=3; *x; }));

  ASSERT(3, ({ int x[3]; *x=3; *(x+1)=4; *(x+2)=5; *x; }));
  ASSERT(4, ({ int x[3]; *x=3; *(x+1)=4; *(x+2)=5; *(x+1); }));
  ASSERT(5, ({ int x[3]; *x=3; *(x+1)=4; *(x+2)=5; *(x+2); }));

  ASSERT(0, ({ int x[2][3]; int *y=x; *y=0; **x; }));
  ASSERT(1, ({ int x[2][3]; int *y=x; *(y+1)=1; *(*x+1); }));
  ASSERT(2, ({ int x[2][3]; int *y=x; *(y+2)=2; *(*x+2); }));
  ASSERT(3, ({ int x[2][3]; int *y=x; *(y+3)=3; **(x+1); }));
  ASSERT(4, ({ int x[2][3]; int *y=x; *(y+4)=4; *(*(x+1)+1); }));
  ASSERT(5, ({ int x[2][3]; int *y=x; *(y+5)=5; *(*(x+1)+2); }));

  ASSERT(3, ({ int x[3]; *x=3; x[1]=4; x[2]=5; *x; }));
  ASSERT(4, ({ int x[3]; *x=3; x[1]=4; x[2]=5; *(x+1); }));
  ASSERT(5, ({ int x[3]; *x=3; x[1]=4; x[2]=5; *(x+2); }));
  ASSERT(5, ({ int x[3]; *x=3; x[1]=4; x[2]=5; *(x+2); }));
  ASSERT(5, ({ int x[3]; *x=3; x[1]=4; 2[x]=5; *(x+2); }));

  ASSERT(0, ({ int x[2][3]; int *y=x; y[0]=0; x[0][0]; }));
  ASSERT(1, ({ int x[2][3]; int *y=x; y[1]=1; x[0][1]; }));
  ASSERT(2, ({ int x[2][3]; int *y=x; y[2]=2; x[0][2]; }));
  ASSERT(3, ({ int x[2][3]; int *y=x; y[3]=3; x[1][0]; }));
  ASSERT(4, ({ int x[2][3]; int *y=x; y[4]=4; x[1][1]; }));
  ASSERT(5, ({ int x[2][3]; int *y=x; y[5]=5; x[1][2]; }));

  // Pointer arithmetic and subscripting must lay out a bare aggregate
  // target: sema defers struct/union layout, while codegen scales by the
  // element size. Without that layout, codegen reads the placeholder
  // size 0 and every index collapses to the first element.
  //
  // 指针算术与下标必须布局裸的聚合目标: sema 推迟 struct/union 布局,
  // 而 codegen 按元素大小缩放. 不布局时读到占位大小 0, 所有下标都退化
  // 到第一个元素.
  ASSERT(16, ({ struct BarePtrIndex { int a; char pad[12]; } *p = (struct BarePtrIndex *)0x1000;
                (char *)&p[1] - (char *)p; }));
  ASSERT(32, ({ struct BarePtrAdd { int a; char pad[12]; } *p = (struct BarePtrAdd *)0x1000;
                (char *)(p + 2) - (char *)p; }));
  ASSERT(16, ({ struct BarePtrSub { int a; char pad[12]; } *p = (struct BarePtrSub *)0x1000;
                (char *)p - (char *)(p - 1); }));
  ASSERT(2, ({ struct BarePtrDiff { int a; char pad[12]; } *p = (struct BarePtrDiff *)0x1000;
               p - (p - 2); }));

  printf("OK\n");
  return 0;
}
