#!/bin/bash
# C→C 打印器测试 (plans/PLAN-c2c.md): 逐字节锁定 c2c 的打印输出。
#
# c2c 是库的第二个真实消费者: tokenize -> preprocess -> parse 之后把第 3 层
# 忠实语法树打印回单一展开后的翻译单元。输出策略: 注释丢弃, 空白全部自造
# (2 空格缩进, 一声明一行), 拼写从 token 原文取, tag 从 Type 记录的 tag_tok
# 取, 枚举定义由 ND_ENUM_CONST 记录重组, 未使用的独立 tag 定义消失 (首次
# 使用时内联打印), 同 declspec 记录的连续声明合并为多 declarator 语句。
#
# 覆盖 (随 CC2-CC5 逐步增长):
#   CC2: 全部 18 个 TypeKind 的声明形态; 指针/数组/函数指针/嵌套括号
#        declarator; 结构/联合/位域/弹性成员/匿名成员; 不完整类型前向声明;
#        枚举三种落位 (独立/吸收进声明/多 declarator); typedef 名恢复与
#        基本类型结构化展开; 存储类与 _Alignas; _Atomic 与 typeof 两形态;
#        原型形态 (无原型/(void)/变参/无名参数); packed/aligned 规范位;
#        维度表达式。
#   CC3: 全部二元运算符与优先级括号; 复合赋值与赋值链; 前后缀自增自减;
#        一元运算与拼写粘连守卫 (- -x); 嵌套 cast; sizeof/_Alignof 两形态;
#        ?: 与 elvis 与中段逗号; 调用/下标/成员/postfix 基座括号; 交换
#        下标 (3[a]); 字符串拼接重发射/转义/宽字符前缀; 字面量拼写;
#        _Generic 与类型谓词 builtin; 复合字面量; 原子 CAS/EXCH 重合成。
#   CC4: 全部语句形态 (if/else 链, while/do/for 含声明 init 与空段,
#        switch/case 区间/default 与 case 组缩进, 标签/计算 goto/
#        &&label, break/continue, asm 与其空语句合并, 块与空语句);
#        块内声明记录 (static/extern/typedef/enum/struct 局部化);
#        语句表达式多行; c34 覆盖样本 + 覆盖审计 (69 kind 中 4 个
#        sema 专属, parse 树可达的 65 个全部触达)。
#
# 用法: test/c2c.sh <chibicc> <c2c> (由 make test 与 make test-stage2 调用)

chibicc="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
c2c_bin="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
tmp=`mktemp -d /tmp/chibicc-c2c-XXXXXX`
trap 'rm -rf $tmp' INT TERM HUP EXIT

names=""
fails=0
checks=0

# snippet <name>: 后随 heredoc 为被打印的源码片段。
snippet() {
    names="$names $1"
    cat > "$tmp/$1.c"
}

# expect <name>: 后随 heredoc 为期望的 stdout 原文 (逐字节)。
expect() {
    cat > "$tmp/$1.expected"
}

snippet c01_basic_types <<'SNIP'
void f(void);
_Bool b;
char c;
short s;
int i;
long l;
float f2;
double d;
long double ld;
unsigned char uc;
unsigned short us;
unsigned int ui;
unsigned long ul;
SNIP

expect c01_basic_types <<'WANT'
void f(void);
_Bool b;
char c;
short s;
int i;
long l;
float f2;
double d;
long double ld;
unsigned char uc;
unsigned short us;
unsigned int ui;
unsigned long ul;
WANT

snippet c02_pointers_arrays <<'SNIP'
int *p;
int **pp;
char *s[2];
int a[3][4];
int *b[2][3];
double *d1, **d2;
SNIP

expect c02_pointers_arrays <<'WANT'
int *p, **pp;
char *s[2];
int a[3][4], *b[2][3];
double *d1, **d2;
WANT

snippet c03_funcptr <<'SNIP'
int (*fp)(void);
int (*table[4])(int);
int (*f(void))[3];
int *(*g[2])(void);
void (*h(int, char))(int);
SNIP

expect c03_funcptr <<'WANT'
int (*fp)(void), (*table[4])(int);
int (*f(void))[3];
int *(*g[2])(void);
void (*h(int, char))(int);
WANT

snippet c04_nested_declarators <<'SNIP'
int (*(*pp)(void))[3];
int (*(*x[2])(void))[4];
SNIP

expect c04_nested_declarators <<'WANT'
int (*(*pp)(void))[3], (*(*x[2])(void))[4];
WANT

snippet c05_structs <<'SNIP'
struct S { int a; int b; };
struct S s1;
struct S *s2;
union U { int i; float f; };
union U u;
struct N { struct { int x; }; union { int y; char z; }; };
struct N n;
SNIP

expect c05_structs <<'WANT'
struct S {
  int a;
  int b;
} s1, *s2;
union U {
  int i;
  float f;
} u;
struct N {
  struct {
    int x;
  };
  union {
    int y;
    char z;
  };
} n;
WANT

snippet c06_incomplete <<'SNIP'
struct A;
extern struct A *p;
struct A *q;
SNIP

expect c06_incomplete <<'WANT'
struct A;
extern struct A *p;
struct A *q;
WANT

snippet c07_enums <<'SNIP'
enum E { A, B = 2 };
enum E e1;
enum { C } x, *y;
enum F { D = A, E2 };
SNIP

expect c07_enums <<'WANT'
enum E {
  A,
  B = 2
} e1;
enum {
  C
} x, *y;
enum F {
  D = A,
  E2
};
WANT

snippet c08_typedefs <<'SNIP'
typedef int T;
typedef struct { int x; } P;
P p1;
P *p2;
typedef T Q;
SNIP

expect c08_typedefs <<'WANT'
typedef int T;
typedef struct {
  int x;
} P;
P p1, *p2;
typedef int Q;
WANT

snippet c09_storage <<'SNIP'
static int a;
extern int b;
static int c;
inline int f(void);
_Thread_local int t;
static _Thread_local int t2;
extern inline int g(void);
SNIP

expect c09_storage <<'WANT'
static int a;
extern int b;
static int c;
inline int f(void);
_Thread_local int t;
static _Thread_local int t2;
extern inline int g(void);
WANT

snippet c10_alignas_init <<'SNIP'
_Alignas(8) int g;
int h = 5;
char s[] = "hi";
int x = 'a';
SNIP

expect c10_alignas_init <<'WANT'
_Alignas(8) int g;
int h = 5;
char s[] = "hi";
int x = 'a';
WANT

snippet c11_atomic_typeof <<'SNIP'
int n;
_Atomic int a;
typeof(n) t;
typeof(int) u;
SNIP

expect c11_atomic_typeof <<'WANT'
int n;
_Atomic int a;
typeof(n) t;
int u;
WANT

snippet c12_params <<'SNIP'
int f();
int g(void);
int h(int, ...);
int k(int a, double b);
void (*cb)(int, char *);
int (*arr[3])(void);
SNIP

expect c12_params <<'WANT'
int f();
int g(void);
int h(int, ...);
int k(int a, double b);
void (*cb)(int, char *);
int (*arr[3])(void);
WANT

snippet c13_bitfields_flex <<'SNIP'
struct B { unsigned a : 3; unsigned b : 5; int : 0; int c; };
struct F { int n; char data[]; };
struct B bb;
struct F ff;
SNIP

expect c13_bitfields_flex <<'WANT'
struct B {
  unsigned int a : 3;
  unsigned int b : 5;
  int : 0;
  int c;
} bb;
struct F {
  int n;
  char data[];
} ff;
WANT

snippet c14_attributes <<'SNIP'
struct __attribute__((packed)) P2 { char c; int i; };
struct P2 p2;
struct __attribute__((aligned(16))) A2 { int x; };
struct A2 a2;
SNIP

expect c14_attributes <<'WANT'
struct P2 {
  char c;
  int i;
} __attribute__((packed)) p2;
struct A2 {
  int x;
} __attribute__((aligned(16))) a2;
WANT

snippet c15_dims <<'SNIP'
int n;
int a[n];
int b[3];
int c[];
int d[2][n];
SNIP

expect c15_dims <<'WANT'
int n, a[n], b[3], c[], d[2][n];
WANT

snippet c16_binary <<'SNIP'
int a, b, c, d;
int e1 = a + b - c * d / a % b;
int e2 = (a + b) * (c - d);
int e3 = a << b >> c;
int e4 = a < b <= c > d >= a;
int e5 = a == b != c;
int e6 = a & b ^ c | d;
int e7 = a && b || c && d;
int e8 = a + b * c - d / a;
int e9 = a ? b : c ? d : a;
int e10 = (a ? b : c) ? d : a;
SNIP

expect c16_binary <<'WANT'
int a, b, c, d, e1 = a + b - c * d / a % b, e2 = (a + b) * (c - d), e3 = a << b >> c, e4 = a < b <= c > d >= a, e5 = a == b != c, e6 = a & b ^ c | d, e7 = a && b || c && d, e8 = a + b * c - d / a, e9 = a ? b : c ? d : a, e10 = (a ? b : c) ? d : a;
WANT

snippet c17_assign <<'SNIP'
int a, b, c;
int e1 = a = b = c;
int e2[] = {0};
int e3 = (a += 1);
int e4 = (a -= 2) + (b *= 3) + (c /= 4) + (a %= 5) + (b &= 6) + (c |= 7) + (a ^= 8) + (b <<= 9) + (c >>= 10);
SNIP

expect c17_assign <<'WANT'
int a, b, c, e1 = a = b = c, e2[] = { 0 }, e3 = a += 1, e4 = (a -= 2) + (b *= 3) + (c /= 4) + (a %= 5) + (b &= 6) + (c |= 7) + (a ^= 8) + (b <<= 9) + (c >>= 10);
WANT

snippet c18_unary <<'SNIP'
int a, b;
int e1 = - -a;
int e2 = -+a;
int e3 = &a;
int e4 = *(&a);
int e5 = !-a;
int e6 = ~a;
int e7 = --(a + b);
int e8 = (a = b, b);
SNIP

expect c18_unary <<'WANT'
int a, b, e1 = - -a, e2 = -a, e3 = &a, e4 = *&a, e5 = !-a, e6 = ~a, e7 = --(a + b), e8 = (a = b, b);
WANT

snippet c19_cast_sizeof <<'SNIP'
int a;
void *p;
int e1 = (int)(long)(char)a;
int e2 = (int *)p;
int e3 = sizeof(int *);
int e4 = sizeof a;
int e5 = sizeof(a + 1);
int e6 = _Alignof(int);
SNIP

expect c19_cast_sizeof <<'WANT'
int a;
void *p;
int e1 = (int)(long)(char)a, e2 = (int *)p, e3 = sizeof(int *), e4 = sizeof a, e5 = sizeof (a + 1), e6 = _Alignof(int);
WANT

snippet c20_cond <<'SNIP'
int a, b, c;
int e1 = a ?: b;
int e2 = (a ?: b) ?: c;
int e3 = a ? (b, c) : (a, b);
SNIP

expect c20_cond <<'WANT'
int a, b, c, e1 = a ?: b, e2 = (a ?: b) ?: c, e3 = a ? b, c : (a, b);
WANT

snippet c21_postfix <<'SNIP'
struct S { int x, y; };
struct S s, *sp;
typedef int (*fp)(int);
fp f;
int a[3];
int e1 = s.x + sp->y;
int e2 = f(a[0]);
int e3 = (*f)(1);
int e4 = (&s)->x;
int e5 = a[0] + (&a)[0][1];
int e6 = 3[a];
SNIP

expect c21_postfix <<'WANT'
struct S {
  int x;
  int y;
} s, *sp;
typedef int (*fp)(int);
fp f;
int a[3], e1 = s.x + sp->y, e2 = f(a[0]), e3 = (*f)(1), e4 = (&s)->x, e5 = a[0] + (&a)[0][1], e6 = 3[a];
WANT

snippet c22_strings <<'SNIP'
char s1[] = "a" "b" "c";
char s2[] = "tab\there";
char s3[] = "quote\"inside";
char s4[] = "back\\slash";
int s5[] = L"wide";
int s6[] = L"a" L"b";
short s7[] = u"u16";
SNIP

expect c22_strings <<'WANT'
char s1[] = "abc", s2[] = "tab\there", s3[] = "quote\"inside", s4[] = "back\\slash";
int s5[] = L"wide", s6[] = L"ab";
short s7[] = u"u16";
WANT

snippet c23_literals <<'SNIP'
int e1 = 0x1f + 0377 + 07;
int e2 = 'a' + '\n' + '\0';
unsigned e3 = 1u + 2U;
long e4 = 1l + 2L;
double e5 = .5 + 1. + 1e10 + 1.5e-3 + 0x1p3;
float e6 = 1.0f + 2.0F;
SNIP

expect c23_literals <<'WANT'
int e1 = 0x1f + 0377 + 07, e2 = 'a' + '\n' + '\0';
unsigned int e3 = 1u + 2U;
long e4 = 1l + 2L;
double e5 = .5 + 1. + 1e10 + 1.5e-3 + 0x1p3;
float e6 = 1.0f + 2.0F;
WANT

snippet c24_generic <<'SNIP'
int a;
float f;
int *p;
int e1 = _Generic(a, int: 1, float: 2, default: 3);
int e2 = _Generic(a + 1, int *: 1, default: 0);
int e3 = __builtin_types_compatible_p(int, unsigned int);
int e4 = __builtin_reg_class(int);
SNIP

expect c24_generic <<'WANT'
int a;
float f;
int *p, e1 = _Generic(a, int: 1, float: 2, default: 3), e2 = _Generic(a + 1, int *: 1, default: 0), e3 = __builtin_types_compatible_p(int, unsigned int), e4 = __builtin_reg_class(int);
WANT

snippet c25_complit <<'SNIP'
struct S { int x, y; };
struct S g = (struct S){ 1, 2 };
int *p = (int[]){ 1, 2, 3 };
int e = (int){ 5 };
SNIP

expect c25_complit <<'WANT'
struct S {
  int x;
  int y;
} g = (struct S){ 1, 2 };
int *p = (int []){ 1, 2, 3 }, e = (int){ 5 };
WANT

snippet c26_atomics <<'SNIP'
_Atomic(int) ap, ao;
int an = 1;
int r = __builtin_compare_and_swap(&ap, &ao, an);
int q = __builtin_atomic_exchange(&ap, an);
SNIP

expect c26_atomics <<'WANT'
_Atomic int ap, ao;
int an = 1, r = __builtin_compare_and_swap(&ap, &ao, an), q = __builtin_atomic_exchange(&ap, an);
WANT


snippet c27_if_else <<'SNIP'
int f(int a, int b) {
  if (a) b = 1;
  if (a) b = 2; else b = 3;
  if (a) { b = 4; } else { b = 5; }
  if (a) b = 6; else if (b) b = 7; else if (a) b = 8; else b = 9;
  if (a) ;
  return b;
}
SNIP

expect c27_if_else <<'WANT'
int f(int a, int b) {
  if (a)
    b = 1;
  if (a)
    b = 2;
  else
    b = 3;
  if (a) {
    b = 4;
  } else {
    b = 5;
  }
  if (a)
    b = 6;
  else if (b)
    b = 7;
  else if (a)
    b = 8;
  else
    b = 9;
  if (a)
    ;
  return b;
}
WANT

snippet c28_loops <<'SNIP'
int f(int n) {
  int s = 0;
  while (n) { n--; s += n; }
  do s++; while (s < 10);
  for (int i = 0, j = 1; i < n; i++, j *= 2) s += i * j;
  for (;;) break;
  for (; s > 100;) s--;
  for (int i = 0; i < 3; i++) { if (i == 1) continue; s += i; }
  return s;
}
SNIP

expect c28_loops <<'WANT'
int f(int n) {
  int s = 0;
  while (n) {
    n--;
    s += n;
  }
  do
    s++;
  while (s < 10);
  for (int i = 0, j = 1; i < n; i++, j *= 2)
    s += i * j;
  for (;;)
    break;
  for (; s > 100;)
    s--;
  for (int i = 0; i < 3; i++) {
    if (i == 1)
      continue;
    s += i;
  }
  return s;
}
WANT

snippet c29_switch <<'SNIP'
int f(int a) {
  switch (a) {
    case 1:
      return 1;
    case 2 ... 5:
      return 2;
    default:
      break;
  }
  switch (a) { case 0: a = 1; }
  return a;
}
SNIP

expect c29_switch <<'WANT'
int f(int a) {
  switch (a) {
    case 1:
      return 1;
    case 2 ... 5:
      return 2;
    default:
      break;
  }
  switch (a) {
    case 0:
      a = 1;
  }
  return a;
}
WANT

snippet c30_labels_goto <<'SNIP'
int f(int n) {
  void *p;
  top:
  if (n) { n--; goto top; }
  p = &&end;
  goto *p;
  end:
  return n;
}
SNIP

expect c30_labels_goto <<'WANT'
int f(int n) {
  void *p;
  top:
    if (n) {
      n--;
      goto top;
    }
  p = &&end;
  goto *p;
  end:
    return n;
}
WANT

snippet c31_stmt_expr <<'SNIP'
int f(int a) {
  int b = ({ int t = a * 2; t + 1; });
  b += ({ a++; a; });
  return b;
}
SNIP

expect c31_stmt_expr <<'WANT'
int f(int a) {
  int b = ({
    int t = a * 2;
    t + 1;
  });
  b += ({
    a++;
    a;
  });
  return b;
}
WANT

snippet c32_asm <<'SNIP'
void f(void) { asm("nop"); }
void g(void) {
  asm("mov $1, %rax\n\t"
      "ret");
}
SNIP

expect c32_asm <<'WANT'
void f(void) {
  asm("nop");
}
void g(void) {
  asm("mov $1, %rax\n\tret");
}
WANT

snippet c33_local_records <<'SNIP'
static int g = 1;
int f(void) {
  static int s = 2;
  extern int e;
  typedef int T;
  enum E { A, B };
  struct S { int x; };
  T t = A + B;
  struct S v = { .x = 3 };
  return t + v.x + s + e + g;
}
SNIP

expect c33_local_records <<'WANT'
static int g = 1;
int f(void) {
  static int s = 2;
  extern int e;
  typedef int T;
  enum E {
    A,
    B
  };
  int t = A + B;
  struct S {
    int x;
  } v = { .x = 3 };
  return t + v.x + s + e + g;
}
WANT

snippet c34_coverage <<'SNIP'
int cvg = 5;
typedef struct Cv { int a, b; } Cv;
enum CvE { CVA, CVB = 2 };
extern int cvext(int, ...);
int cvfn(int x, int y) {
  int a = 1, b = 2, r = 0;
  Cv s, *sp = &s;
  int arr[3] = { 1, 2, 3 };
  char msg[] = "ok";
  void *lp;
  a += b; a -= b; a *= b; a /= b; a %= b; a &= b; a |= b; a ^= b;
  a <<= b; a >>= b; a = b;
  a = -a; a = !a; a = ~a; a = +a;
  a = a + b * a / b % a - a;
  a = a << b >> a;
  a = a < b; a = a <= b; a = a > b; a = a >= b;
  a = a == b; a = a != b;
  a = a & b; a = a ^ b; a = a | b;
  a = a && b; a = a || b;
  a = a++ + ++b; a = a-- - --b;
  a = arr[a]; a = 2[arr]; a = *sp + *(&arr[0]);
  a = s.a + sp->b;
  a = (int)a; a = (char)(long)a;
  a = sizeof a + sizeof(int) + _Alignof(int);
  a = a ? b : a; a = a ?: b;
  a = (a, b);
  a = _Generic(a, float: 1, int *: 2, default: 3);
  a = __builtin_types_compatible_p(int, long) + __builtin_reg_class(int *);
  if (a) b = 1; else b = 2;
  if (a) { b = 3; } else if (b) { b = 4; } else { b = 5; }
  while (a) a--;
  do b++; while (b < 3);
  for (int i = 0; i < 3; i++) a += i;
  for (;;) break;
  switch (a) {
    case 1:
      b = 1;
      break;
    case 2 ... 4:
      break;
    default:
      break;
  }
  for (int i = 0; i < 3; i++) { if (i) continue; a += i; }
  a = "xy"[0] + (int)"q";
  goto cvlab;
cvlab:
  ;
  lp = &&cvlab;
  goto *lp;
  {
    a = ({ int t = b; t; });
  }
  a = (Cv){ 7, 8 }.a;
  return a + CVA + msg[0] + (int)cvext(1, 2, 3);
}
int cvq(_Atomic(int) *p, _Atomic(int) *o, int n) {
  return __builtin_compare_and_swap(p, o, n) + __builtin_atomic_exchange(p, n);
}
void cvasm(void) { asm("nop"); }
SNIP

expect c34_coverage <<'WANT'
int cvg = 5;
typedef struct Cv {
  int a;
  int b;
} Cv;
enum CvE {
  CVA,
  CVB = 2
};
extern int cvext(int, ...);
int cvfn(int x, int y) {
  int a = 1, b = 2, r = 0;
  Cv s, *sp = &s;
  int arr[3] = { 1, 2, 3 };
  char msg[] = "ok";
  void *lp;
  a += b;
  a -= b;
  a *= b;
  a /= b;
  a %= b;
  a &= b;
  a |= b;
  a ^= b;
  a <<= b;
  a >>= b;
  a = b;
  a = -a;
  a = !a;
  a = ~a;
  a = a;
  a = a + b * a / b % a - a;
  a = a << b >> a;
  a = a < b;
  a = a <= b;
  a = a > b;
  a = a >= b;
  a = a == b;
  a = a != b;
  a = a & b;
  a = a ^ b;
  a = a | b;
  a = a && b;
  a = a || b;
  a = a++ + ++b;
  a = a-- - --b;
  a = arr[a];
  a = 2[arr];
  a = *sp + *&arr[0];
  a = s.a + sp->b;
  a = (int)a;
  a = (char)(long)a;
  a = sizeof a + sizeof(int) + _Alignof(int);
  a = a ? b : a;
  a = a ?: b;
  a = (a, b);
  a = _Generic(a, float: 1, int *: 2, default: 3);
  a = __builtin_types_compatible_p(int, long) + __builtin_reg_class(int *);
  if (a)
    b = 1;
  else
    b = 2;
  if (a) {
    b = 3;
  } else if (b) {
    b = 4;
  } else {
    b = 5;
  }
  while (a)
    a--;
  do
    b++;
  while (b < 3);
  for (int i = 0; i < 3; i++)
    a += i;
  for (;;)
    break;
  switch (a) {
    case 1:
      b = 1;
      break;
    case 2 ... 4:
      break;
    default:
      break;
  }
  for (int i = 0; i < 3; i++) {
    if (i)
      continue;
    a += i;
  }
  a = "xy"[0] + (int)"q";
  goto cvlab;
  cvlab:
    ;
  lp = &&cvlab;
  goto *lp;
  {
    a = ({
      int t = b;
      t;
    });
  }
  a = (Cv){ 7, 8 }.a;
  return a + CVA + msg[0] + (int)cvext(1, 2, 3);
}
int cvq(_Atomic int *p, _Atomic int *o, int n) {
  return __builtin_compare_and_swap(p, o, n) + __builtin_atomic_exchange(p, n);
}
void cvasm(void) {
  asm("nop");
}
WANT

# 检查: 每个片段 (1) 输出与期望逐字节相同; (2) 对打印产物再打印逐字节不变。
for name in $names; do
    checks=$((checks + 2))
    if ! "$c2c_bin" "$tmp/$name.c" > "$tmp/$name.got" 2> "$tmp/$name.err"; then
        echo "c2c/$name: c2c failed:"; cat "$tmp/$name.err"
        fails=$((fails + 1))
        continue
    fi
    if ! cmp -s "$tmp/$name.got" "$tmp/$name.expected"; then
        echo "c2c/$name: output differs:"
        diff "$tmp/$name.expected" "$tmp/$name.got" | head -20
        fails=$((fails + 1))
        continue
    fi
    if ! "$c2c_bin" "$tmp/$name.got" > "$tmp/$name.got2" 2> "$tmp/$name.err"; then
        echo "c2c/$name: re-print failed:"; cat "$tmp/$name.err"
        fails=$((fails + 1))
        continue
    fi
    if ! cmp -s "$tmp/$name.got" "$tmp/$name.got2"; then
        echo "c2c/$name: not idempotent:"
        diff "$tmp/$name.got" "$tmp/$name.got2" | head -20
        fails=$((fails + 1))
    fi
done

# 覆盖审计: 逐片段 dump 后合并 NodeKind 集合。69 个 kind 中 4 个是 sema
# 专属 (ND_VAR / ND_VLA_PTR / ND_MEMZERO / ND_NULL_EXPR), parse 树可达的
# 65 个必须全部被本脚本的片段触达。
: > "$tmp/kinds.all"
for name in $names; do
    "$chibicc" -dump-ast "$tmp/$name.c" 2>/dev/null | grep -oE 'ND_[A-Z_]+' >> "$tmp/kinds.all"
done
kinds=$(sort -u "$tmp/kinds.all" | wc -l | tr -d ' ')
if [ "$kinds" -lt 65 ]; then
    echo "c2c coverage: only $kinds node kinds reached (want >= 65)"
    fails=$((fails + 1))
fi

if [ $fails -ne 0 ]; then
    echo "c2c lock: $fails checks FAILED"
    exit 1
fi
echo "c2c lock: $((checks / 2)) golden samples byte-exact and idempotent, $kinds node kinds covered"