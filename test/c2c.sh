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
#
# 用法: test/c2c.sh <chibicc> <c2c> (由 make test 与 make test-stage2 调用;
# CC2 阶段尚不在 make test 内, 由开发循环手动调用)

c2c_bin="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
[ $# -ge 2 ] && [ "$2" != "-" ] && c2c_bin="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
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

if [ $fails -ne 0 ]; then
    echo "c2c lock: $fails of $checks checks FAILED"
    exit 1
fi
echo "c2c lock: $((checks / 2)) golden samples byte-exact and idempotent"
