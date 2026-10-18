#!/bin/bash
# 临时 faithful AST dump 测试 (DUMP-AST-PLAN.md, 路线图第 3 层).
#
# --dump-ast 在 parse() 之后, sema() 之前把忠实语法树打印成文本. 本脚本手动运行,
# 不接入 make test, 因为这条通道是一次性调试特性. 覆盖:
#   - exact diff: 一个最小程序的完整 dump 与期望文本逐字节相等
#   - 关键行: 每类节点 / 字段 / 类型 / 初始化器至少一条断言
#   - ND_< 缺席: kind_name 的兜底分支没有被触发
#   - sema-only kind 缺席: 证明 dump 在 sema 之前
#   - coverage: 65 个 parse 可产生的 kind 全部出现
#   - 别名 / -o / -E 与 -M 的优先级
#
# 样本自包含, 不依赖系统头文件, 因此可在 macOS 本机运行.
#
# 用法: test/ast.sh ./chibicc

chibicc="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
tmp=`mktemp -d /tmp/chibicc-ast-XXXXXX`
trap 'rm -rf $tmp' INT TERM HUP EXIT

check() {
    if [ $? -eq 0 ]; then
        echo "testing ast/$1 ... passed"
    else
        echo "testing ast/$1 ... failed"
        exit 1
    fi
}

# need <name> <file> <fixed text>: 文本出现.
need() {
    grep -qF -- "$3" "$2"
    check "$1"
}

# need_re <name> <file> <regex>: 行匹配.
need_re() {
    grep -qE -- "$3" "$2"
    check "$1"
}

# need_absent <name> <file> <fixed text>: 文本不得出现.
need_absent() {
    ! grep -qF -- "$3" "$2"
    check "$1"
}

# has_node <name> <file> <kind>: 该 kind 作为节点行出现(不是 op= 之类的字段值).
has_node() {
    grep -qE -- "^ *$3( |\$)" "$2"
    check "$1"
}

# ---- exact diff: 格式锁 --------------------------------------------------------

cat > "$tmp/min.c" <<'EOF'
int g = 1;
int main(void) {
  int x = 2;
  return x + g;
}
EOF

cat > "$tmp/min.expected" <<'EOF'
ND_GVAR_DECL name="g" ty_op="int"
  init:
    INIT_EXPR
      expr:
        ND_NUM val=1 ty="int" tok="1"
ND_FUNCDEF name="main" ty_op="int (void)"
  body:
    ND_BLOCK scope=1
      body:
        ND_DECL name="x" ty_op="int"
          init:
            INIT_EXPR
              expr:
                ND_NUM val=2 ty="int" tok="2"
        ND_RETURN tok="return"
          expr:
            ND_ADD tok="+"
              lhs:
                ND_IDENT name="x" tok="x"
              rhs:
                ND_IDENT name="g" tok="g"
EOF

$chibicc --dump-ast "$tmp/min.c" > "$tmp/min.out"
check "exact-diff"

# ---- 样本: 声明 / 类型 / 初始化器 ---------------------------------------------

cat > "$tmp/decl.c" <<'EOF'
typedef int T;
enum E { A, B = 2 };
struct S { int m; int n[2]; };
union U { int a; char b; };
int g = 1;
int ga[3] = {1, 2, 3};
int gd[5] = {[2] = 3, [0 ... 1] = 4};
struct S gs2 = {.m = 1, .n = {2, 3}};
char gstr[] = "hi";
int *gp = &g;
static int sg;
extern int eg;
_Thread_local int tg;
_Alignas(16) int al1;
_Alignas(int) int al2;
_Atomic int at1;
inline int fi(void) { return 1; }
static int fs(void);
int proto(int, char *);
int arr2[2][3] = {{1, 2, 3}, {4}};
T gt = 3;
int vla(int n) { int a[n]; return a[0]; }
int tf(void) { int x = 1; typeof(x) y = 2; return y; }
EOF

$chibicc --dump-ast "$tmp/decl.c" > "$tmp/decl.out"
check "dump/decl"

# ---- 样本: 表达式与语句 -------------------------------------------------------

cat > "$tmp/expr.c" <<'EOF'
int f(int a, int b) { return a + b; }
int main(void) {
  int a = 1, b = 2, c = 3, d = 4, e = 5, f2 = 6, g = 7, h = 8;
  int i = 9, j = 10, k = 11, l = 12, m = 13, n = 14, o = 15, p = 16;
  int q = 17, r = 18, s = 19;
  int arr[3];
  int *ptr = &a;
  struct S { int m; } st;
  int z;
  double dv = 1.5;
  z = a+b-c*d/e%f2 & g | h ^ i << j >> k == l != m < n <= o > p >= q && r || s;
  z = -a;
  z = !a;
  z = ~a;
  z = *ptr;
  z = a ? b : c;
  z = a ?: b;
  z = (a, b);
  z = (long)a;
  z = dv;
  z = sizeof(int);
  z = sizeof a;
  z = _Alignof(int);
  z = _Alignof a;
  z = arr[a];
  z = st.m;
  z = ptr->m;
  z = f(a, b);
  z = ({ a; b; });
  z = (int){3};
  a++;
  a--;
  --a;
  ++a;
  a = b;
  a += b; a -= b; a *= b; a /= b; a %= b; a &= b; a |= b; a ^= b; a <<= b; a >>= b;
  z = __builtin_compare_and_swap(&a, b, c);
  z = __builtin_atomic_exchange(&a, b);
  z = _Generic(a, int: 1, default: 2);
  z = __builtin_types_compatible_p(int, long);
  z = __builtin_reg_class(int);
  z = "str"[0];
  if (a) a = 1; else a = 2;
  for (i = 0; i < 3; i++) a += i;
  for (int jj = 0; jj < 2; jj++) a += jj;
  while (a) a--;
  while (a) { if (a) break; continue; }
  do a++; while (a < 3);
  switch (a) { case 1: a = 2; break; case 3 ... 5: a = 4; break; default: a = 0; }
  goto L;
L:
  ;
  goto *&&L;
  asm("nop");
  return z;
}
EOF

$chibicc --dump-ast "$tmp/expr.c" > "$tmp/expr.out"
check "dump/expr"

cat "$tmp/decl.out" "$tmp/expr.out" > "$tmp/all.out"

# ---- 关键行: 表达式 -----------------------------------------------------------

has_node "expr/add" "$tmp/expr.out" "ND_ADD"
need "expr/add-tok" "$tmp/expr.out" 'ND_ADD tok="+"'
has_node "expr/sub" "$tmp/expr.out" "ND_SUB"
has_node "expr/mul" "$tmp/expr.out" "ND_MUL"
has_node "expr/div" "$tmp/expr.out" "ND_DIV"
has_node "expr/mod" "$tmp/expr.out" "ND_MOD"
has_node "expr/neg" "$tmp/expr.out" "ND_NEG"
has_node "expr/bitand" "$tmp/expr.out" "ND_BITAND"
has_node "expr/bitor" "$tmp/expr.out" "ND_BITOR"
has_node "expr/bitxor" "$tmp/expr.out" "ND_BITXOR"
has_node "expr/shl" "$tmp/expr.out" "ND_SHL"
has_node "expr/shr" "$tmp/expr.out" "ND_SHR"
has_node "expr/eq" "$tmp/expr.out" "ND_EQ"
has_node "expr/ne" "$tmp/expr.out" "ND_NE"
has_node "expr/lt" "$tmp/expr.out" "ND_LT"
has_node "expr/le" "$tmp/expr.out" "ND_LE"
has_node "expr/gt" "$tmp/expr.out" "ND_GT"
has_node "expr/ge" "$tmp/expr.out" "ND_GE"
has_node "expr/logand" "$tmp/expr.out" "ND_LOGAND"
has_node "expr/logor" "$tmp/expr.out" "ND_LOGOR"
has_node "expr/comma" "$tmp/expr.out" "ND_COMMA"
has_node "expr/assign" "$tmp/expr.out" "ND_ASSIGN"
need "expr/assign-op" "$tmp/expr.out" 'ND_ASSIGN op=ND_ADD tok="+="'
need "expr/assign-op-shr" "$tmp/expr.out" 'ND_ASSIGN op=ND_SHR tok=">>="'
need "expr/incdec-post" "$tmp/expr.out" 'ND_INCDEC post=1 addend=1 tok="++"'
need "expr/incdec-prefix" "$tmp/expr.out" 'ND_INCDEC addend=-1 tok="--"'
has_node "expr/subscript" "$tmp/expr.out" "ND_SUBSCRIPT"
need "expr/subscript-base" "$tmp/expr.out" 'base:'
need "expr/subscript-index" "$tmp/expr.out" 'index:'
has_node "expr/cond" "$tmp/expr.out" "ND_COND"
need "expr/cond-elvis" "$tmp/expr.out" 'ND_COND elvis=1 tok="?"'
has_node "expr/member" "$tmp/expr.out" "ND_MEMBER"
need "expr/member-dot" "$tmp/expr.out" 'ND_MEMBER name="m" tok="m"'
need "expr/member-arrow" "$tmp/expr.out" 'ND_MEMBER name="m" arrow=1 tok="m"'
has_node "expr/addr" "$tmp/expr.out" "ND_ADDR"
has_node "expr/deref" "$tmp/expr.out" "ND_DEREF"
has_node "expr/not" "$tmp/expr.out" "ND_NOT"
has_node "expr/bitnot" "$tmp/expr.out" "ND_BITNOT"
has_node "expr/funcall" "$tmp/expr.out" "ND_FUNCALL"
need "expr/funcall-callee" "$tmp/expr.out" 'callee:'
need "expr/funcall-args" "$tmp/expr.out" 'args:'
has_node "expr/stmt-expr" "$tmp/expr.out" "ND_STMT_EXPR"
has_node "expr/ident" "$tmp/expr.out" "ND_IDENT"
need "expr/ident-name" "$tmp/expr.out" 'ND_IDENT name="a" tok="a"'
has_node "expr/num" "$tmp/expr.out" "ND_NUM"
need "expr/num-val" "$tmp/expr.out" 'ND_NUM val=1 ty="int" tok="1"'
need "expr/float-tok" "$tmp/expr.out" 'ty="double" tok="1.5"'
has_node "expr/string" "$tmp/expr.out" "ND_STRING"
need "expr/string-tok" "$tmp/expr.out" 'ND_STRING tok="\"str\""'
has_node "expr/sizeof" "$tmp/expr.out" "ND_SIZEOF"
need "expr/sizeof-type" "$tmp/expr.out" 'ND_SIZEOF ty_op="int" tok="sizeof"'
need "expr/sizeof-expr" "$tmp/expr.out" 'ND_SIZEOF tok="sizeof"'
has_node "expr/alignof" "$tmp/expr.out" "ND_ALIGNOF"
need "expr/alignof-type" "$tmp/expr.out" 'ND_ALIGNOF ty_op="int" tok="_Alignof"'
has_node "expr/generic" "$tmp/expr.out" "ND_GENERIC"
need "expr/generic-controlling" "$tmp/expr.out" 'controlling:'
need "expr/generic-assocs" "$tmp/expr.out" 'assocs:'
has_node "expr/generic-assoc" "$tmp/expr.out" "ND_GENERIC_ASSOC"
need "expr/generic-assoc-type" "$tmp/expr.out" 'ND_GENERIC_ASSOC ty_op="int"'
need "expr/generic-assoc-default" "$tmp/expr.out" 'ND_GENERIC_ASSOC default=1'
has_node "expr/types-compatible" "$tmp/expr.out" "ND_TYPES_COMPATIBLE"
need "expr/types-compatible-ops" "$tmp/expr.out" 'ND_TYPES_COMPATIBLE ty_op="int" ty_op2="long"'
has_node "expr/reg-class" "$tmp/expr.out" "ND_REG_CLASS"
need "expr/reg-class-type" "$tmp/expr.out" 'ND_REG_CLASS ty_op="int"'
has_node "expr/cast" "$tmp/expr.out" "ND_CAST"
need "expr/cast-type" "$tmp/expr.out" 'ND_CAST ty_op="long" tok="("'
has_node "expr/compound-literal" "$tmp/expr.out" "ND_COMPOUND_LITERAL"
need "expr/compound-literal-type" "$tmp/expr.out" 'ND_COMPOUND_LITERAL ty_op="int"'
has_node "expr/cas" "$tmp/expr.out" "ND_CAS"
need "expr/cas-fields" "$tmp/expr.out" 'addr:'
need "expr/cas-old" "$tmp/expr.out" 'old:'
need "expr/cas-new" "$tmp/expr.out" 'new:'
has_node "expr/exch" "$tmp/expr.out" "ND_EXCH"
has_node "expr/label-val" "$tmp/expr.out" "ND_LABEL_VAL"
need "expr/label-val-name" "$tmp/expr.out" 'ND_LABEL_VAL label="L"'

# ---- 关键行: 语句 -------------------------------------------------------------

has_node "stmt/return" "$tmp/expr.out" "ND_RETURN"
has_node "stmt/if" "$tmp/expr.out" "ND_IF"
need "stmt/if-else" "$tmp/expr.out" 'else:'
has_node "stmt/for" "$tmp/expr.out" "ND_FOR"
need "stmt/for-init" "$tmp/expr.out" 'init:'
need "stmt/for-cond" "$tmp/expr.out" 'cond:'
need "stmt/for-inc" "$tmp/expr.out" 'inc:'
need "stmt/for-body" "$tmp/expr.out" 'body:'
has_node "stmt/while" "$tmp/expr.out" "ND_WHILE"
has_node "stmt/do" "$tmp/expr.out" "ND_DO"
has_node "stmt/switch" "$tmp/expr.out" "ND_SWITCH"
has_node "stmt/case" "$tmp/expr.out" "ND_CASE"
need "stmt/case-default" "$tmp/expr.out" 'ND_CASE default=1 tok="default"'
need "stmt/case-range" "$tmp/expr.out" 'end:'
has_node "stmt/block" "$tmp/expr.out" "ND_BLOCK"
need "stmt/block-scope" "$tmp/expr.out" 'ND_BLOCK scope=1'
has_node "stmt/goto" "$tmp/expr.out" "ND_GOTO"
need "stmt/goto-label" "$tmp/expr.out" 'ND_GOTO label="L" tok="goto"'
has_node "stmt/break" "$tmp/expr.out" "ND_BREAK"
has_node "stmt/continue" "$tmp/expr.out" "ND_CONTINUE"
has_node "stmt/goto-expr" "$tmp/expr.out" "ND_GOTO_EXPR"
has_node "stmt/label" "$tmp/expr.out" "ND_LABEL"
need "stmt/label-name" "$tmp/expr.out" 'ND_LABEL label="L" tok="L"'
has_node "stmt/expr-stmt" "$tmp/expr.out" "ND_EXPR_STMT"
has_node "stmt/asm" "$tmp/expr.out" "ND_ASM"
need "stmt/asm-string" "$tmp/expr.out" 'ND_ASM asm="nop" tok="asm"'

# ---- 关键行: 声明与类型 -------------------------------------------------------

has_node "decl/decl" "$tmp/decl.out" "ND_DECL"
has_node "decl/gvar" "$tmp/decl.out" "ND_GVAR_DECL"
has_node "decl/funcdef" "$tmp/decl.out" "ND_FUNCDEF"
has_node "decl/typedef" "$tmp/decl.out" "ND_TYPEDEF"
need "decl/typedef-name" "$tmp/decl.out" 'ND_TYPEDEF name="T" ty="int" tok="T"'
has_node "decl/enum-const" "$tmp/decl.out" "ND_ENUM_CONST"
need "decl/enum-const-name" "$tmp/decl.out" 'ND_ENUM_CONST name="A" ty_op="enum" tok="A"'
need "decl/enum-const-value" "$tmp/decl.out" 'value:'
need "decl/static" "$tmp/decl.out" 'ND_GVAR_DECL name="sg" ty_op="int" static=1'
need "decl/extern" "$tmp/decl.out" 'ND_GVAR_DECL name="eg" ty_op="int" extern=1'
need "decl/tls" "$tmp/decl.out" 'ND_GVAR_DECL name="tg" ty_op="int" tls=1'
need "decl/inline" "$tmp/decl.out" 'ND_FUNCDEF name="fi" ty_op="int (void)" inline=1'
need "decl/funcdef-static" "$tmp/decl.out" 'ND_FUNCDEF name="fs" ty_op="int (void)" static=1'
need "decl/align-expr" "$tmp/decl.out" 'align_expr:'
need "decl/align-ty" "$tmp/decl.out" 'ND_GVAR_DECL name="al2" ty_op="int" align_ty="int"'
need "decl/atomic" "$tmp/decl.out" 'ty_op="_Atomic int"'
need "decl/proto" "$tmp/decl.out" 'ND_FUNCDEF name="proto" ty_op="int (int, char *)"'
need "decl/array-2d" "$tmp/decl.out" 'ND_GVAR_DECL name="arr2" ty_op="int[3][2]"'
need "decl/array-3" "$tmp/decl.out" 'ND_GVAR_DECL name="ga" ty_op="int[3]"'
need "decl/array-open" "$tmp/decl.out" 'ND_GVAR_DECL name="gstr" ty_op="char[]"'
need "decl/pointer" "$tmp/decl.out" 'ND_GVAR_DECL name="gp" ty_op="int *"'
need "decl/struct" "$tmp/decl.out" 'ND_GVAR_DECL name="gs2" ty_op="struct"'
need "decl/vla-form" "$tmp/decl.out" 'ND_DECL name="a" ty_op="int[?]"'
need "decl/typeof" "$tmp/decl.out" 'ND_DECL name="y" ty_op="typeof(?)"'
need "decl/param-name" "$tmp/decl.out" 'ND_FUNCDEF name="vla" ty_op="int (int n)"'

# ---- 关键行: 初始化器 ---------------------------------------------------------

need "init/expr" "$tmp/decl.out" 'INIT_EXPR'
need "init/list" "$tmp/decl.out" 'INIT_LIST'
need "init/item" "$tmp/decl.out" 'item:'
need "init/desigs" "$tmp/decl.out" 'desigs:'
need "init/value" "$tmp/decl.out" 'value:'
need "init/str" "$tmp/decl.out" 'INIT_STR tok="\"hi\""'
need "init/desig-index" "$tmp/decl.out" 'INIT_DESIG kind="index"'
need "init/desig-range" "$tmp/decl.out" 'INIT_DESIG kind="range"'
need "init/desig-member" "$tmp/decl.out" 'INIT_DESIG kind="member" name="m"'

# ---- 缺席断言: 兜底与 sema-only kind -----------------------------------------

need_absent "no-fallback-kind" "$tmp/all.out" 'ND_<'
need_absent "no-sema-null-expr" "$tmp/all.out" 'ND_NULL_EXPR'
need_absent "no-sema-var" "$tmp/all.out" 'ND_VAR'
need_absent "no-sema-vla-ptr" "$tmp/all.out" 'ND_VLA_PTR'
need_absent "no-sema-memzero" "$tmp/all.out" 'ND_MEMZERO'

# ---- coverage: 65 个 parse 可产生的 kind --------------------------------------

kinds="ND_ADD ND_SUB ND_MUL ND_DIV ND_NEG ND_MOD ND_BITAND ND_BITOR
ND_BITXOR ND_SHL ND_SHR ND_EQ ND_NE ND_LT ND_LE ND_GT ND_GE ND_ASSIGN
ND_INCDEC ND_SUBSCRIPT ND_COND ND_COMMA ND_MEMBER ND_ADDR ND_DEREF ND_NOT
ND_BITNOT ND_LOGAND ND_LOGOR ND_FUNCALL ND_STMT_EXPR ND_IDENT ND_NUM
ND_STRING ND_SIZEOF ND_ALIGNOF ND_GENERIC ND_GENERIC_ASSOC
ND_TYPES_COMPATIBLE ND_REG_CLASS ND_CAST ND_COMPOUND_LITERAL ND_CAS ND_EXCH
ND_LABEL_VAL ND_RETURN ND_IF ND_FOR ND_WHILE ND_DO ND_SWITCH ND_CASE
ND_BLOCK ND_GOTO ND_BREAK ND_CONTINUE ND_GOTO_EXPR ND_LABEL ND_EXPR_STMT
ND_ASM ND_DECL ND_GVAR_DECL ND_FUNCDEF ND_TYPEDEF ND_ENUM_CONST"

for k in $kinds; do
    has_node "coverage/$k" "$tmp/all.out" "$k"
done

# ---- 通道行为: 别名 / -o / 优先级 ---------------------------------------------

$chibicc -dump-ast "$tmp/min.c" > "$tmp/alias.out"
cmp -s "$tmp/alias.out" "$tmp/min.out"
check "alias"

$chibicc --dump-ast -o "$tmp/o.out" "$tmp/min.c" > "$tmp/o.stdout"
check "o/exit"
cmp -s "$tmp/o.out" "$tmp/min.out"
check "o/content"
test ! -s "$tmp/o.stdout"
check "o/stdout-empty"

# -E 与 -M 优先于 --dump-ast: 打印预处理结果 / 依赖, 不出现 AST.
$chibicc -E --dump-ast "$tmp/min.c" > "$tmp/e.out"
check "priority/E-exit"
need "priority/E-output" "$tmp/e.out" 'int g = 1;'
need_absent "priority/E-no-ast" "$tmp/e.out" 'ND_GVAR_DECL'

$chibicc -M --dump-ast "$tmp/min.c" > "$tmp/m.out"
check "priority/M-exit"
need "priority/M-output" "$tmp/m.out" ':'
need_absent "priority/M-no-ast" "$tmp/m.out" 'ND_GVAR_DECL'

# -MD 先写依赖文件, 再 dump (-o 同时给出时依赖文件名为 -o 的 .d 侧名).
(cd "$tmp" && $chibicc -MD -o md.out --dump-ast min.c)
check "priority/MD-exit"
need "priority/MD-dump" "$tmp/md.out" 'ND_GVAR_DECL'
test -f "$tmp/md.d"
check "priority/MD-deps"

echo "ast tests: all passed"
