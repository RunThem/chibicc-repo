#!/bin/bash
# AST 形态锁定测试 (PLAN-pratt-parse P1.3): 逐字节锁定 -dump-ast 的输出.
#
# -dump-ast 是驱动器的只读通道: 在 libcp_parse 之后, libcp_sema 之前早退,
# 不写目标文件, 不汇编. 输出只含 parse 阶段写的字段 (节点种类, 锚点 token 的
# 文件/行/列与拼写, 复合赋值的 op, 自增自减的方向, elvis, 箭头成员, 标签名,
# storage class 与 _Alignas, 目标类型签名, 初始化器记录), 不含地址与 sema 结论.
# 宏展开产物打印 @synth: 它们的位置在宏体或合成缓冲里, 不是表达式的源码位置.
#
# 本测试是 Pratt 重写的判据 7: 形态锁必须先于任何 Pratt 代码落地并跑绿, 之后两个
# 构建 (默认级联与 LIBCP_PRATT_PARSE_FEATURE=1) 的输出都必须与这里逐字节相同.
#
# 覆盖: 全部二元运算符; 十个复合赋值与普通 =; 结合性; 逗号在语句/括号/实参/初始化器与
# ?: 中段的区别; elvis; 前缀与后缀自增自减; 一元 + 透明; 嵌套 cast; sizeof 与
# _Alignof 两种形式; 复合字面量; 语句表达式; _Generic; 两个 builtin; &&label;
# 调用/下标/点/箭头; 列表/指示符/区间/字符串/嵌套/表达式初始化器; 语句层形状;
# 声明记录与 _Alignas; 宏展开的 @synth 锚点; 三个语义非法但语法合法的输入.
#
# 用法: test/ast.sh ./chibicc (由 make test 与 make test-stage2 调用)

chibicc="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
tmp=`mktemp -d /tmp/chibicc-ast-XXXXXX`
trap 'rm -rf $tmp' INT TERM HUP EXIT

names=""

# snippet <name>: 后随 heredoc 为被 dump 的源码片段.
snippet() {
    names="$names $1"
    cat > "$tmp/$1.c"
}

# expect <name>: 后随 heredoc 为期望的 stdout 原文(逐字节).
expect() {
    cat > "$tmp/$1.expected"
}

snippet a01_add <<'SNIP'
int f(int x, int y) { return x + y; }
SNIP

expect a01_add <<'WANT'
ND_FUNCDEF @a01_add.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a01_add.c:1:23 "return" is_scope_block=1
    ND_RETURN @a01_add.c:1:23 "return"
      ND_ADD @a01_add.c:1:32 "+"
        ND_IDENT @a01_add.c:1:30 "x"
        ND_IDENT @a01_add.c:1:34 "y"
WANT

snippet a02_sub <<'SNIP'
int f(int x, int y) { return x - y; }
SNIP

expect a02_sub <<'WANT'
ND_FUNCDEF @a02_sub.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a02_sub.c:1:23 "return" is_scope_block=1
    ND_RETURN @a02_sub.c:1:23 "return"
      ND_SUB @a02_sub.c:1:32 "-"
        ND_IDENT @a02_sub.c:1:30 "x"
        ND_IDENT @a02_sub.c:1:34 "y"
WANT

snippet a03_mul <<'SNIP'
int f(int x, int y) { return x * y; }
SNIP

expect a03_mul <<'WANT'
ND_FUNCDEF @a03_mul.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a03_mul.c:1:23 "return" is_scope_block=1
    ND_RETURN @a03_mul.c:1:23 "return"
      ND_MUL @a03_mul.c:1:32 "*"
        ND_IDENT @a03_mul.c:1:30 "x"
        ND_IDENT @a03_mul.c:1:34 "y"
WANT

snippet a04_div <<'SNIP'
int f(int x, int y) { return x / y; }
SNIP

expect a04_div <<'WANT'
ND_FUNCDEF @a04_div.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a04_div.c:1:23 "return" is_scope_block=1
    ND_RETURN @a04_div.c:1:23 "return"
      ND_DIV @a04_div.c:1:32 "/"
        ND_IDENT @a04_div.c:1:30 "x"
        ND_IDENT @a04_div.c:1:34 "y"
WANT

snippet a05_mod <<'SNIP'
int f(int x, int y) { return x % y; }
SNIP

expect a05_mod <<'WANT'
ND_FUNCDEF @a05_mod.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a05_mod.c:1:23 "return" is_scope_block=1
    ND_RETURN @a05_mod.c:1:23 "return"
      ND_MOD @a05_mod.c:1:32 "%"
        ND_IDENT @a05_mod.c:1:30 "x"
        ND_IDENT @a05_mod.c:1:34 "y"
WANT

snippet a06_shl <<'SNIP'
int f(int x, int y) { return x << y; }
SNIP

expect a06_shl <<'WANT'
ND_FUNCDEF @a06_shl.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a06_shl.c:1:23 "return" is_scope_block=1
    ND_RETURN @a06_shl.c:1:23 "return"
      ND_SHL @a06_shl.c:1:32 "<<"
        ND_IDENT @a06_shl.c:1:30 "x"
        ND_IDENT @a06_shl.c:1:35 "y"
WANT

snippet a07_shr <<'SNIP'
int f(int x, int y) { return x >> y; }
SNIP

expect a07_shr <<'WANT'
ND_FUNCDEF @a07_shr.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a07_shr.c:1:23 "return" is_scope_block=1
    ND_RETURN @a07_shr.c:1:23 "return"
      ND_SHR @a07_shr.c:1:32 ">>"
        ND_IDENT @a07_shr.c:1:30 "x"
        ND_IDENT @a07_shr.c:1:35 "y"
WANT

snippet a08_lt <<'SNIP'
int f(int x, int y) { return x < y; }
SNIP

expect a08_lt <<'WANT'
ND_FUNCDEF @a08_lt.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a08_lt.c:1:23 "return" is_scope_block=1
    ND_RETURN @a08_lt.c:1:23 "return"
      ND_LT @a08_lt.c:1:32 "<"
        ND_IDENT @a08_lt.c:1:30 "x"
        ND_IDENT @a08_lt.c:1:34 "y"
WANT

snippet a09_le <<'SNIP'
int f(int x, int y) { return x <= y; }
SNIP

expect a09_le <<'WANT'
ND_FUNCDEF @a09_le.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a09_le.c:1:23 "return" is_scope_block=1
    ND_RETURN @a09_le.c:1:23 "return"
      ND_LE @a09_le.c:1:32 "<="
        ND_IDENT @a09_le.c:1:30 "x"
        ND_IDENT @a09_le.c:1:35 "y"
WANT

snippet a10_gt <<'SNIP'
int f(int x, int y) { return x > y; }
SNIP

expect a10_gt <<'WANT'
ND_FUNCDEF @a10_gt.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a10_gt.c:1:23 "return" is_scope_block=1
    ND_RETURN @a10_gt.c:1:23 "return"
      ND_GT @a10_gt.c:1:32 ">"
        ND_IDENT @a10_gt.c:1:30 "x"
        ND_IDENT @a10_gt.c:1:34 "y"
WANT

snippet a11_ge <<'SNIP'
int f(int x, int y) { return x >= y; }
SNIP

expect a11_ge <<'WANT'
ND_FUNCDEF @a11_ge.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a11_ge.c:1:23 "return" is_scope_block=1
    ND_RETURN @a11_ge.c:1:23 "return"
      ND_GE @a11_ge.c:1:32 ">="
        ND_IDENT @a11_ge.c:1:30 "x"
        ND_IDENT @a11_ge.c:1:35 "y"
WANT

snippet a12_eq <<'SNIP'
int f(int x, int y) { return x == y; }
SNIP

expect a12_eq <<'WANT'
ND_FUNCDEF @a12_eq.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a12_eq.c:1:23 "return" is_scope_block=1
    ND_RETURN @a12_eq.c:1:23 "return"
      ND_EQ @a12_eq.c:1:32 "=="
        ND_IDENT @a12_eq.c:1:30 "x"
        ND_IDENT @a12_eq.c:1:35 "y"
WANT

snippet a13_ne <<'SNIP'
int f(int x, int y) { return x != y; }
SNIP

expect a13_ne <<'WANT'
ND_FUNCDEF @a13_ne.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a13_ne.c:1:23 "return" is_scope_block=1
    ND_RETURN @a13_ne.c:1:23 "return"
      ND_NE @a13_ne.c:1:32 "!="
        ND_IDENT @a13_ne.c:1:30 "x"
        ND_IDENT @a13_ne.c:1:35 "y"
WANT

snippet a14_bitand <<'SNIP'
int f(int x, int y) { return x & y; }
SNIP

expect a14_bitand <<'WANT'
ND_FUNCDEF @a14_bitand.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a14_bitand.c:1:23 "return" is_scope_block=1
    ND_RETURN @a14_bitand.c:1:23 "return"
      ND_BITAND @a14_bitand.c:1:32 "&"
        ND_IDENT @a14_bitand.c:1:30 "x"
        ND_IDENT @a14_bitand.c:1:34 "y"
WANT

snippet a15_bitxor <<'SNIP'
int f(int x, int y) { return x ^ y; }
SNIP

expect a15_bitxor <<'WANT'
ND_FUNCDEF @a15_bitxor.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a15_bitxor.c:1:23 "return" is_scope_block=1
    ND_RETURN @a15_bitxor.c:1:23 "return"
      ND_BITXOR @a15_bitxor.c:1:32 "^"
        ND_IDENT @a15_bitxor.c:1:30 "x"
        ND_IDENT @a15_bitxor.c:1:34 "y"
WANT

snippet a16_bitor <<'SNIP'
int f(int x, int y) { return x | y; }
SNIP

expect a16_bitor <<'WANT'
ND_FUNCDEF @a16_bitor.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a16_bitor.c:1:23 "return" is_scope_block=1
    ND_RETURN @a16_bitor.c:1:23 "return"
      ND_BITOR @a16_bitor.c:1:32 "|"
        ND_IDENT @a16_bitor.c:1:30 "x"
        ND_IDENT @a16_bitor.c:1:34 "y"
WANT

snippet a17_logand <<'SNIP'
int f(int x, int y) { return x && y; }
SNIP

expect a17_logand <<'WANT'
ND_FUNCDEF @a17_logand.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a17_logand.c:1:23 "return" is_scope_block=1
    ND_RETURN @a17_logand.c:1:23 "return"
      ND_LOGAND @a17_logand.c:1:32 "&&"
        ND_IDENT @a17_logand.c:1:30 "x"
        ND_IDENT @a17_logand.c:1:35 "y"
WANT

snippet a18_logor <<'SNIP'
int f(int x, int y) { return x || y; }
SNIP

expect a18_logor <<'WANT'
ND_FUNCDEF @a18_logor.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a18_logor.c:1:23 "return" is_scope_block=1
    ND_RETURN @a18_logor.c:1:23 "return"
      ND_LOGOR @a18_logor.c:1:32 "||"
        ND_IDENT @a18_logor.c:1:30 "x"
        ND_IDENT @a18_logor.c:1:35 "y"
WANT

snippet a19_addassign <<'SNIP'
int f(int x, int y) { x += y; return x; }
SNIP

expect a19_addassign <<'WANT'
ND_FUNCDEF @a19_addassign.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a19_addassign.c:1:23 "x" is_scope_block=1
    ND_EXPR_STMT @a19_addassign.c:1:23 "x"
      ND_ASSIGN @a19_addassign.c:1:25 "+=" op=ND_ADD
        ND_IDENT @a19_addassign.c:1:23 "x"
        ND_IDENT @a19_addassign.c:1:28 "y"
    ND_RETURN @a19_addassign.c:1:31 "return"
      ND_IDENT @a19_addassign.c:1:38 "x"
WANT

snippet a20_subassign <<'SNIP'
int f(int x, int y) { x -= y; return x; }
SNIP

expect a20_subassign <<'WANT'
ND_FUNCDEF @a20_subassign.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a20_subassign.c:1:23 "x" is_scope_block=1
    ND_EXPR_STMT @a20_subassign.c:1:23 "x"
      ND_ASSIGN @a20_subassign.c:1:25 "-=" op=ND_SUB
        ND_IDENT @a20_subassign.c:1:23 "x"
        ND_IDENT @a20_subassign.c:1:28 "y"
    ND_RETURN @a20_subassign.c:1:31 "return"
      ND_IDENT @a20_subassign.c:1:38 "x"
WANT

snippet a21_mulassign <<'SNIP'
int f(int x, int y) { x *= y; return x; }
SNIP

expect a21_mulassign <<'WANT'
ND_FUNCDEF @a21_mulassign.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a21_mulassign.c:1:23 "x" is_scope_block=1
    ND_EXPR_STMT @a21_mulassign.c:1:23 "x"
      ND_ASSIGN @a21_mulassign.c:1:25 "*=" op=ND_MUL
        ND_IDENT @a21_mulassign.c:1:23 "x"
        ND_IDENT @a21_mulassign.c:1:28 "y"
    ND_RETURN @a21_mulassign.c:1:31 "return"
      ND_IDENT @a21_mulassign.c:1:38 "x"
WANT

snippet a22_divassign <<'SNIP'
int f(int x, int y) { x /= y; return x; }
SNIP

expect a22_divassign <<'WANT'
ND_FUNCDEF @a22_divassign.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a22_divassign.c:1:23 "x" is_scope_block=1
    ND_EXPR_STMT @a22_divassign.c:1:23 "x"
      ND_ASSIGN @a22_divassign.c:1:25 "/=" op=ND_DIV
        ND_IDENT @a22_divassign.c:1:23 "x"
        ND_IDENT @a22_divassign.c:1:28 "y"
    ND_RETURN @a22_divassign.c:1:31 "return"
      ND_IDENT @a22_divassign.c:1:38 "x"
WANT

snippet a23_modassign <<'SNIP'
int f(int x, int y) { x %= y; return x; }
SNIP

expect a23_modassign <<'WANT'
ND_FUNCDEF @a23_modassign.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a23_modassign.c:1:23 "x" is_scope_block=1
    ND_EXPR_STMT @a23_modassign.c:1:23 "x"
      ND_ASSIGN @a23_modassign.c:1:25 "%=" op=ND_MOD
        ND_IDENT @a23_modassign.c:1:23 "x"
        ND_IDENT @a23_modassign.c:1:28 "y"
    ND_RETURN @a23_modassign.c:1:31 "return"
      ND_IDENT @a23_modassign.c:1:38 "x"
WANT

snippet a24_bitandassign <<'SNIP'
int f(int x, int y) { x &= y; return x; }
SNIP

expect a24_bitandassign <<'WANT'
ND_FUNCDEF @a24_bitandassign.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a24_bitandassign.c:1:23 "x" is_scope_block=1
    ND_EXPR_STMT @a24_bitandassign.c:1:23 "x"
      ND_ASSIGN @a24_bitandassign.c:1:25 "&=" op=ND_BITAND
        ND_IDENT @a24_bitandassign.c:1:23 "x"
        ND_IDENT @a24_bitandassign.c:1:28 "y"
    ND_RETURN @a24_bitandassign.c:1:31 "return"
      ND_IDENT @a24_bitandassign.c:1:38 "x"
WANT

snippet a25_bitorassign <<'SNIP'
int f(int x, int y) { x |= y; return x; }
SNIP

expect a25_bitorassign <<'WANT'
ND_FUNCDEF @a25_bitorassign.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a25_bitorassign.c:1:23 "x" is_scope_block=1
    ND_EXPR_STMT @a25_bitorassign.c:1:23 "x"
      ND_ASSIGN @a25_bitorassign.c:1:25 "|=" op=ND_BITOR
        ND_IDENT @a25_bitorassign.c:1:23 "x"
        ND_IDENT @a25_bitorassign.c:1:28 "y"
    ND_RETURN @a25_bitorassign.c:1:31 "return"
      ND_IDENT @a25_bitorassign.c:1:38 "x"
WANT

snippet a26_bitxorassign <<'SNIP'
int f(int x, int y) { x ^= y; return x; }
SNIP

expect a26_bitxorassign <<'WANT'
ND_FUNCDEF @a26_bitxorassign.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a26_bitxorassign.c:1:23 "x" is_scope_block=1
    ND_EXPR_STMT @a26_bitxorassign.c:1:23 "x"
      ND_ASSIGN @a26_bitxorassign.c:1:25 "^=" op=ND_BITXOR
        ND_IDENT @a26_bitxorassign.c:1:23 "x"
        ND_IDENT @a26_bitxorassign.c:1:28 "y"
    ND_RETURN @a26_bitxorassign.c:1:31 "return"
      ND_IDENT @a26_bitxorassign.c:1:38 "x"
WANT

snippet a27_shlassign <<'SNIP'
int f(int x, int y) { x <<= y; return x; }
SNIP

expect a27_shlassign <<'WANT'
ND_FUNCDEF @a27_shlassign.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a27_shlassign.c:1:23 "x" is_scope_block=1
    ND_EXPR_STMT @a27_shlassign.c:1:23 "x"
      ND_ASSIGN @a27_shlassign.c:1:25 "<<=" op=ND_SHL
        ND_IDENT @a27_shlassign.c:1:23 "x"
        ND_IDENT @a27_shlassign.c:1:29 "y"
    ND_RETURN @a27_shlassign.c:1:32 "return"
      ND_IDENT @a27_shlassign.c:1:39 "x"
WANT

snippet a28_shrassign <<'SNIP'
int f(int x, int y) { x >>= y; return x; }
SNIP

expect a28_shrassign <<'WANT'
ND_FUNCDEF @a28_shrassign.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a28_shrassign.c:1:23 "x" is_scope_block=1
    ND_EXPR_STMT @a28_shrassign.c:1:23 "x"
      ND_ASSIGN @a28_shrassign.c:1:25 ">>=" op=ND_SHR
        ND_IDENT @a28_shrassign.c:1:23 "x"
        ND_IDENT @a28_shrassign.c:1:29 "y"
    ND_RETURN @a28_shrassign.c:1:32 "return"
      ND_IDENT @a28_shrassign.c:1:39 "x"
WANT

snippet a29_left_sub <<'SNIP'
int f(int x, int y, int z) { return x - y - z; }
SNIP

expect a29_left_sub <<'WANT'
ND_FUNCDEF @a29_left_sub.c:1:28 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a29_left_sub.c:1:30 "return" is_scope_block=1
    ND_RETURN @a29_left_sub.c:1:30 "return"
      ND_SUB @a29_left_sub.c:1:43 "-"
        ND_SUB @a29_left_sub.c:1:39 "-"
          ND_IDENT @a29_left_sub.c:1:37 "x"
          ND_IDENT @a29_left_sub.c:1:41 "y"
        ND_IDENT @a29_left_sub.c:1:45 "z"
WANT

snippet a30_right_assign <<'SNIP'
int f(int x, int y, int z) { return x = y = z; }
SNIP

expect a30_right_assign <<'WANT'
ND_FUNCDEF @a30_right_assign.c:1:28 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a30_right_assign.c:1:30 "return" is_scope_block=1
    ND_RETURN @a30_right_assign.c:1:30 "return"
      ND_ASSIGN @a30_right_assign.c:1:39 "="
        ND_IDENT @a30_right_assign.c:1:37 "x"
        ND_ASSIGN @a30_right_assign.c:1:43 "="
          ND_IDENT @a30_right_assign.c:1:41 "y"
          ND_IDENT @a30_right_assign.c:1:45 "z"
WANT

snippet a31_right_cond <<'SNIP'
int f(int a, int b, int c, int d, int e) { return a ? b : c ? d : e; }
SNIP

expect a31_right_cond <<'WANT'
ND_FUNCDEF @a31_right_cond.c:1:42 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a31_right_cond.c:1:44 "return" is_scope_block=1
    ND_RETURN @a31_right_cond.c:1:44 "return"
      ND_COND @a31_right_cond.c:1:53 "?"
        ND_IDENT @a31_right_cond.c:1:51 "a"
        ND_IDENT @a31_right_cond.c:1:55 "b"
        ND_COND @a31_right_cond.c:1:61 "?"
          ND_IDENT @a31_right_cond.c:1:59 "c"
          ND_IDENT @a31_right_cond.c:1:63 "d"
          ND_IDENT @a31_right_cond.c:1:67 "e"
WANT

snippet a32_comma_stmt <<'SNIP'
int f(int x, int y) { x, y; return x; }
SNIP

expect a32_comma_stmt <<'WANT'
ND_FUNCDEF @a32_comma_stmt.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a32_comma_stmt.c:1:23 "x" is_scope_block=1
    ND_EXPR_STMT @a32_comma_stmt.c:1:23 "x"
      ND_COMMA @a32_comma_stmt.c:1:24 ","
        ND_IDENT @a32_comma_stmt.c:1:23 "x"
        ND_IDENT @a32_comma_stmt.c:1:26 "y"
    ND_RETURN @a32_comma_stmt.c:1:29 "return"
      ND_IDENT @a32_comma_stmt.c:1:36 "x"
WANT

snippet a33_comma_paren <<'SNIP'
int f(int x, int y) { return (x, y); }
SNIP

expect a33_comma_paren <<'WANT'
ND_FUNCDEF @a33_comma_paren.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a33_comma_paren.c:1:23 "return" is_scope_block=1
    ND_RETURN @a33_comma_paren.c:1:23 "return"
      ND_COMMA @a33_comma_paren.c:1:32 ","
        ND_IDENT @a33_comma_paren.c:1:31 "x"
        ND_IDENT @a33_comma_paren.c:1:34 "y"
WANT

snippet a34_comma_arg <<'SNIP'
int f(int x, int y, int z) { return f((x, y), z); }
SNIP

expect a34_comma_arg <<'WANT'
ND_FUNCDEF @a34_comma_arg.c:1:28 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a34_comma_arg.c:1:30 "return" is_scope_block=1
    ND_RETURN @a34_comma_arg.c:1:30 "return"
      ND_FUNCALL @a34_comma_arg.c:1:48 ")"
        ND_IDENT @a34_comma_arg.c:1:37 "f"
        ND_COMMA @a34_comma_arg.c:1:41 ","
          ND_IDENT @a34_comma_arg.c:1:40 "x"
          ND_IDENT @a34_comma_arg.c:1:43 "y"
        ND_IDENT @a34_comma_arg.c:1:47 "z"
WANT

snippet a35_comma_init <<'SNIP'
int a[] = {1, 2};
SNIP

expect a35_comma_init <<'WANT'
ND_GVAR_DECL @a35_comma_init.c:1:9 "=" ty_op=array(-1,int) name="a" attr=none
  INIT kind=LIST @a35_comma_init.c:1:11
    ITEM
      INIT kind=EXPR @a35_comma_init.c:1:12
        ND_NUM @a35_comma_init.c:1:12 "1" val=1
    ITEM comma=@a35_comma_init.c:1:13
      INIT kind=EXPR @a35_comma_init.c:1:15
        ND_NUM @a35_comma_init.c:1:15 "2" val=2
WANT

snippet a36_cond_mid_comma <<'SNIP'
int f(int x, int y, int z, int w) { return x ? y, z : w; }
SNIP

expect a36_cond_mid_comma <<'WANT'
ND_FUNCDEF @a36_cond_mid_comma.c:1:35 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a36_cond_mid_comma.c:1:37 "return" is_scope_block=1
    ND_RETURN @a36_cond_mid_comma.c:1:37 "return"
      ND_COND @a36_cond_mid_comma.c:1:46 "?"
        ND_IDENT @a36_cond_mid_comma.c:1:44 "x"
        ND_COMMA @a36_cond_mid_comma.c:1:49 ","
          ND_IDENT @a36_cond_mid_comma.c:1:48 "y"
          ND_IDENT @a36_cond_mid_comma.c:1:51 "z"
        ND_IDENT @a36_cond_mid_comma.c:1:55 "w"
WANT

snippet a37_elvis <<'SNIP'
int f(int x, int y) { return x ?: y; }
SNIP

expect a37_elvis <<'WANT'
ND_FUNCDEF @a37_elvis.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a37_elvis.c:1:23 "return" is_scope_block=1
    ND_RETURN @a37_elvis.c:1:23 "return"
      ND_COND @a37_elvis.c:1:32 "?" is_elvis=1
        ND_IDENT @a37_elvis.c:1:30 "x"
        ND_IDENT @a37_elvis.c:1:35 "y"
WANT

snippet a38_prefix_incdec <<'SNIP'
int f(int x) { ++x; --x; return x; }
SNIP

expect a38_prefix_incdec <<'WANT'
ND_FUNCDEF @a38_prefix_incdec.c:1:14 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a38_prefix_incdec.c:1:16 "++" is_scope_block=1
    ND_EXPR_STMT @a38_prefix_incdec.c:1:16 "++"
      ND_INCDEC @a38_prefix_incdec.c:1:16 "++" is_post=0 addend=1
        ND_IDENT @a38_prefix_incdec.c:1:18 "x"
    ND_EXPR_STMT @a38_prefix_incdec.c:1:21 "--"
      ND_INCDEC @a38_prefix_incdec.c:1:21 "--" is_post=0 addend=-1
        ND_IDENT @a38_prefix_incdec.c:1:23 "x"
    ND_RETURN @a38_prefix_incdec.c:1:26 "return"
      ND_IDENT @a38_prefix_incdec.c:1:33 "x"
WANT

snippet a39_postfix_incdec <<'SNIP'
int f(int x) { x++; x--; return x; }
SNIP

expect a39_postfix_incdec <<'WANT'
ND_FUNCDEF @a39_postfix_incdec.c:1:14 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a39_postfix_incdec.c:1:16 "x" is_scope_block=1
    ND_EXPR_STMT @a39_postfix_incdec.c:1:16 "x"
      ND_INCDEC @a39_postfix_incdec.c:1:17 "++" is_post=1 addend=1
        ND_IDENT @a39_postfix_incdec.c:1:16 "x"
    ND_EXPR_STMT @a39_postfix_incdec.c:1:21 "x"
      ND_INCDEC @a39_postfix_incdec.c:1:22 "--" is_post=1 addend=-1
        ND_IDENT @a39_postfix_incdec.c:1:21 "x"
    ND_RETURN @a39_postfix_incdec.c:1:26 "return"
      ND_IDENT @a39_postfix_incdec.c:1:33 "x"
WANT

snippet a40_incdec_nest <<'SNIP'
int f(int x) { return ++x++; }
SNIP

expect a40_incdec_nest <<'WANT'
ND_FUNCDEF @a40_incdec_nest.c:1:14 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a40_incdec_nest.c:1:16 "return" is_scope_block=1
    ND_RETURN @a40_incdec_nest.c:1:16 "return"
      ND_INCDEC @a40_incdec_nest.c:1:23 "++" is_post=0 addend=1
        ND_INCDEC @a40_incdec_nest.c:1:26 "++" is_post=1 addend=1
          ND_IDENT @a40_incdec_nest.c:1:25 "x"
WANT

snippet a41_unary_plus <<'SNIP'
int f(int x, int y) { return x + +y; }
SNIP

expect a41_unary_plus <<'WANT'
ND_FUNCDEF @a41_unary_plus.c:1:21 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a41_unary_plus.c:1:23 "return" is_scope_block=1
    ND_RETURN @a41_unary_plus.c:1:23 "return"
      ND_ADD @a41_unary_plus.c:1:32 "+"
        ND_IDENT @a41_unary_plus.c:1:30 "x"
        ND_IDENT @a41_unary_plus.c:1:35 "y"
WANT

snippet a42_unary_ops <<'SNIP'
int f(int x, int *p) { return -x + !x + ~x + *p + (int)&x; }
SNIP

expect a42_unary_ops <<'WANT'
ND_FUNCDEF @a42_unary_ops.c:1:22 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a42_unary_ops.c:1:24 "return" is_scope_block=1
    ND_RETURN @a42_unary_ops.c:1:24 "return"
      ND_ADD @a42_unary_ops.c:1:49 "+"
        ND_ADD @a42_unary_ops.c:1:44 "+"
          ND_ADD @a42_unary_ops.c:1:39 "+"
            ND_ADD @a42_unary_ops.c:1:34 "+"
              ND_NEG @a42_unary_ops.c:1:31 "-"
                ND_IDENT @a42_unary_ops.c:1:32 "x"
              ND_NOT @a42_unary_ops.c:1:36 "!"
                ND_IDENT @a42_unary_ops.c:1:37 "x"
            ND_BITNOT @a42_unary_ops.c:1:41 "~"
              ND_IDENT @a42_unary_ops.c:1:42 "x"
          ND_DEREF @a42_unary_ops.c:1:46 "*"
            ND_IDENT @a42_unary_ops.c:1:47 "p"
        ND_CAST @a42_unary_ops.c:1:51 "(" ty_op=int
          ND_ADDR @a42_unary_ops.c:1:56 "&"
            ND_IDENT @a42_unary_ops.c:1:57 "x"
WANT

snippet a43_cast_nested <<'SNIP'
int f(int x) { return (long)(int)(char)x; }
SNIP

expect a43_cast_nested <<'WANT'
ND_FUNCDEF @a43_cast_nested.c:1:14 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a43_cast_nested.c:1:16 "return" is_scope_block=1
    ND_RETURN @a43_cast_nested.c:1:16 "return"
      ND_CAST @a43_cast_nested.c:1:23 "(" ty_op=long
        ND_CAST @a43_cast_nested.c:1:29 "(" ty_op=int
          ND_CAST @a43_cast_nested.c:1:34 "(" ty_op=char
            ND_IDENT @a43_cast_nested.c:1:40 "x"
WANT

snippet a44_sizeof_expr <<'SNIP'
int f(int x) { return sizeof x; }
SNIP

expect a44_sizeof_expr <<'WANT'
ND_FUNCDEF @a44_sizeof_expr.c:1:14 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a44_sizeof_expr.c:1:16 "return" is_scope_block=1
    ND_RETURN @a44_sizeof_expr.c:1:16 "return"
      ND_SIZEOF @a44_sizeof_expr.c:1:23 "sizeof"
        ND_IDENT @a44_sizeof_expr.c:1:30 "x"
WANT

snippet a45_sizeof_type <<'SNIP'
int f(void) { return sizeof(int); }
SNIP

expect a45_sizeof_type <<'WANT'
ND_FUNCDEF @a45_sizeof_type.c:1:13 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a45_sizeof_type.c:1:15 "return" is_scope_block=1
    ND_RETURN @a45_sizeof_type.c:1:15 "return"
      ND_SIZEOF @a45_sizeof_type.c:1:22 "sizeof" ty_op=int
WANT

snippet a46_alignof_expr <<'SNIP'
int f(int x) { return _Alignof x; }
SNIP

expect a46_alignof_expr <<'WANT'
ND_FUNCDEF @a46_alignof_expr.c:1:14 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a46_alignof_expr.c:1:16 "return" is_scope_block=1
    ND_RETURN @a46_alignof_expr.c:1:16 "return"
      ND_ALIGNOF @a46_alignof_expr.c:1:23 "_Alignof"
        ND_IDENT @a46_alignof_expr.c:1:32 "x"
WANT

snippet a47_alignof_type <<'SNIP'
int f(void) { return _Alignof(long); }
SNIP

expect a47_alignof_type <<'WANT'
ND_FUNCDEF @a47_alignof_type.c:1:13 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a47_alignof_type.c:1:15 "return" is_scope_block=1
    ND_RETURN @a47_alignof_type.c:1:15 "return"
      ND_ALIGNOF @a47_alignof_type.c:1:22 "_Alignof" ty_op=long
WANT

snippet a48_compound_literal <<'SNIP'
struct S { int a; int b; };
int f(void) { return ((struct S){.b = 2}).b; }
SNIP

expect a48_compound_literal <<'WANT'
ND_FUNCDEF @a48_compound_literal.c:2:13 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a48_compound_literal.c:2:15 "return" is_scope_block=1
    ND_RETURN @a48_compound_literal.c:2:15 "return"
      ND_MEMBER @a48_compound_literal.c:2:43 "b"
        ND_COMPOUND_LITERAL @a48_compound_literal.c:2:23 "(" ty_op=struct
          INIT kind=LIST @a48_compound_literal.c:2:33
            ITEM
              DESIG name="b" @a48_compound_literal.c:2:34
              INIT kind=EXPR @a48_compound_literal.c:2:39
                ND_NUM @a48_compound_literal.c:2:39 "2" val=2
WANT

snippet a49_stmt_expr <<'SNIP'
int f(int x) { return ({ int y = x; y + 1; }); }
SNIP

expect a49_stmt_expr <<'WANT'
ND_FUNCDEF @a49_stmt_expr.c:1:14 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a49_stmt_expr.c:1:16 "return" is_scope_block=1
    ND_RETURN @a49_stmt_expr.c:1:16 "return"
      ND_STMT_EXPR @a49_stmt_expr.c:1:23 "("
        ND_DECL @a49_stmt_expr.c:1:30 "y" ty_op=int name="y" attr=none
          INIT kind=EXPR @a49_stmt_expr.c:1:34 eq=@a49_stmt_expr.c:1:32
            ND_IDENT @a49_stmt_expr.c:1:34 "x"
        ND_EXPR_STMT @a49_stmt_expr.c:1:37 "y"
          ND_ADD @a49_stmt_expr.c:1:39 "+"
            ND_IDENT @a49_stmt_expr.c:1:37 "y"
            ND_NUM @a49_stmt_expr.c:1:41 "1" val=1
WANT

snippet a50_generic <<'SNIP'
int f(int x) { return _Generic(x, int: 1, default: 2); }
SNIP

expect a50_generic <<'WANT'
ND_FUNCDEF @a50_generic.c:1:14 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a50_generic.c:1:16 "return" is_scope_block=1
    ND_RETURN @a50_generic.c:1:16 "return"
      ND_GENERIC @a50_generic.c:1:31 "("
        ND_IDENT @a50_generic.c:1:32 "x"
        ND_GENERIC_ASSOC @a50_generic.c:1:35 "int" ty_op=int
          ND_NUM @a50_generic.c:1:40 "1" val=1
        ND_GENERIC_ASSOC @a50_generic.c:1:43 "default"
          ND_NUM @a50_generic.c:1:52 "2" val=2
WANT

snippet a51_types_compatible <<'SNIP'
int f(void) { return __builtin_types_compatible_p(int, char); }
SNIP

expect a51_types_compatible <<'WANT'
ND_FUNCDEF @a51_types_compatible.c:1:13 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a51_types_compatible.c:1:15 "return" is_scope_block=1
    ND_RETURN @a51_types_compatible.c:1:15 "return"
      ND_TYPES_COMPATIBLE @a51_types_compatible.c:1:22 "__builtin_types_compatible_p" ty_op=int ty_op2=char
WANT

snippet a52_reg_class <<'SNIP'
int f(void) { return __builtin_reg_class(int); }
SNIP

expect a52_reg_class <<'WANT'
ND_FUNCDEF @a52_reg_class.c:1:13 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a52_reg_class.c:1:15 "return" is_scope_block=1
    ND_RETURN @a52_reg_class.c:1:15 "return"
      ND_REG_CLASS @a52_reg_class.c:1:22 "__builtin_reg_class" ty_op=int
WANT

snippet a53_labels_as_values <<'SNIP'
int f(void) { void *p = &&done; goto *p; done: return 0; }
SNIP

expect a53_labels_as_values <<'WANT'
ND_FUNCDEF @a53_labels_as_values.c:1:13 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a53_labels_as_values.c:1:15 "void" is_scope_block=1
    ND_DECL @a53_labels_as_values.c:1:21 "p" ty_op=ptr(void) name="p" attr=none
      INIT kind=EXPR @a53_labels_as_values.c:1:25 eq=@a53_labels_as_values.c:1:23
        ND_LABEL_VAL @a53_labels_as_values.c:1:25 "&&" label="done"
    ND_GOTO_EXPR @a53_labels_as_values.c:1:33 "goto"
      ND_IDENT @a53_labels_as_values.c:1:39 "p"
    ND_LABEL @a53_labels_as_values.c:1:42 "done" label="done"
      ND_RETURN @a53_labels_as_values.c:1:48 "return"
        ND_NUM @a53_labels_as_values.c:1:55 "0" val=0
WANT

snippet a54_call_args <<'SNIP'
int g(int a, int b); int f(int x, int y) { return g(x, y); }
SNIP

expect a54_call_args <<'WANT'
ND_FUNCDEF @a54_call_args.c:1:20 ";" ty_op=func(int) name="g" attr=none
ND_FUNCDEF @a54_call_args.c:1:42 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a54_call_args.c:1:44 "return" is_scope_block=1
    ND_RETURN @a54_call_args.c:1:44 "return"
      ND_FUNCALL @a54_call_args.c:1:57 ")"
        ND_IDENT @a54_call_args.c:1:51 "g"
        ND_IDENT @a54_call_args.c:1:53 "x"
        ND_IDENT @a54_call_args.c:1:56 "y"
WANT

snippet a55_subscript <<'SNIP'
int f(int a[3][4], int i, int j) { return a[i][j]; }
SNIP

expect a55_subscript <<'WANT'
ND_FUNCDEF @a55_subscript.c:1:34 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a55_subscript.c:1:36 "return" is_scope_block=1
    ND_RETURN @a55_subscript.c:1:36 "return"
      ND_SUBSCRIPT @a55_subscript.c:1:47 "["
        ND_SUBSCRIPT @a55_subscript.c:1:44 "["
          ND_IDENT @a55_subscript.c:1:43 "a"
          ND_IDENT @a55_subscript.c:1:45 "i"
        ND_IDENT @a55_subscript.c:1:48 "j"
WANT

snippet a56_member_dot <<'SNIP'
struct S { int a; };
int f(struct S s) { return s.a; }
SNIP

expect a56_member_dot <<'WANT'
ND_FUNCDEF @a56_member_dot.c:2:19 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a56_member_dot.c:2:21 "return" is_scope_block=1
    ND_RETURN @a56_member_dot.c:2:21 "return"
      ND_MEMBER @a56_member_dot.c:2:30 "a"
        ND_IDENT @a56_member_dot.c:2:28 "s"
WANT

snippet a57_member_arrow <<'SNIP'
struct S { int a; };
int f(struct S *s) { return s->a; }
SNIP

expect a57_member_arrow <<'WANT'
ND_FUNCDEF @a57_member_arrow.c:2:20 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a57_member_arrow.c:2:22 "return" is_scope_block=1
    ND_RETURN @a57_member_arrow.c:2:22 "return"
      ND_MEMBER @a57_member_arrow.c:2:32 "a" arrow=1
        ND_IDENT @a57_member_arrow.c:2:29 "s"
WANT

snippet a58_array_init <<'SNIP'
int a[3] = {1, 2, 3};
SNIP

expect a58_array_init <<'WANT'
ND_GVAR_DECL @a58_array_init.c:1:10 "=" ty_op=array(-1,int) name="a" attr=none
  INIT kind=LIST @a58_array_init.c:1:12
    ITEM
      INIT kind=EXPR @a58_array_init.c:1:13
        ND_NUM @a58_array_init.c:1:13 "1" val=1
    ITEM comma=@a58_array_init.c:1:14
      INIT kind=EXPR @a58_array_init.c:1:16
        ND_NUM @a58_array_init.c:1:16 "2" val=2
    ITEM comma=@a58_array_init.c:1:17
      INIT kind=EXPR @a58_array_init.c:1:19
        ND_NUM @a58_array_init.c:1:19 "3" val=3
WANT

snippet a59_range_desig <<'SNIP'
int a[4] = {[1 ... 2] = 3};
SNIP

expect a59_range_desig <<'WANT'
ND_GVAR_DECL @a59_range_desig.c:1:10 "=" ty_op=array(-1,int) name="a" attr=none
  INIT kind=LIST @a59_range_desig.c:1:12
    ITEM
      DESIG idx @a59_range_desig.c:1:13
        ND_NUM @a59_range_desig.c:1:14 "1" val=1
        ND_NUM @a59_range_desig.c:1:20 "2" val=2
      INIT kind=EXPR @a59_range_desig.c:1:25
        ND_NUM @a59_range_desig.c:1:25 "3" val=3
WANT

snippet a60_struct_desig <<'SNIP'
struct S { int a; int b; };
struct S s = {.a = 1, .b = 2};
SNIP

expect a60_struct_desig <<'WANT'
ND_GVAR_DECL @a60_struct_desig.c:2:12 "=" ty_op=struct name="s" attr=none
  INIT kind=LIST @a60_struct_desig.c:2:14
    ITEM
      DESIG name="a" @a60_struct_desig.c:2:15
      INIT kind=EXPR @a60_struct_desig.c:2:20
        ND_NUM @a60_struct_desig.c:2:20 "1" val=1
    ITEM comma=@a60_struct_desig.c:2:21
      DESIG name="b" @a60_struct_desig.c:2:23
      INIT kind=EXPR @a60_struct_desig.c:2:28
        ND_NUM @a60_struct_desig.c:2:28 "2" val=2
WANT

snippet a61_string_init <<'SNIP'
char s[] = "hi";
SNIP

expect a61_string_init <<'WANT'
ND_GVAR_DECL @a61_string_init.c:1:10 "=" ty_op=array(-1,char) name="s" attr=none
  INIT kind=STR @a61_string_init.c:1:12
    STR @a61_string_init.c:1:12 "\"hi\""
WANT

snippet a62_nested_init <<'SNIP'
int a[2][2] = {{1, 2}, {3, 4}};
SNIP

expect a62_nested_init <<'WANT'
ND_GVAR_DECL @a62_nested_init.c:1:13 "=" ty_op=array(-1,array(-1,int)) name="a" attr=none
  INIT kind=LIST @a62_nested_init.c:1:15
    ITEM
      INIT kind=LIST @a62_nested_init.c:1:16
        ITEM
          INIT kind=EXPR @a62_nested_init.c:1:17
            ND_NUM @a62_nested_init.c:1:17 "1" val=1
        ITEM comma=@a62_nested_init.c:1:18
          INIT kind=EXPR @a62_nested_init.c:1:20
            ND_NUM @a62_nested_init.c:1:20 "2" val=2
    ITEM comma=@a62_nested_init.c:1:22
      INIT kind=LIST @a62_nested_init.c:1:24
        ITEM
          INIT kind=EXPR @a62_nested_init.c:1:25
            ND_NUM @a62_nested_init.c:1:25 "3" val=3
        ITEM comma=@a62_nested_init.c:1:26
          INIT kind=EXPR @a62_nested_init.c:1:28
            ND_NUM @a62_nested_init.c:1:28 "4" val=4
WANT

snippet a63_expr_init <<'SNIP'
int g = 1 + 2 * 3;
SNIP

expect a63_expr_init <<'WANT'
ND_GVAR_DECL @a63_expr_init.c:1:7 "=" ty_op=int name="g" attr=none
  INIT kind=EXPR @a63_expr_init.c:1:9
    ND_ADD @a63_expr_init.c:1:11 "+"
      ND_NUM @a63_expr_init.c:1:9 "1" val=1
      ND_MUL @a63_expr_init.c:1:15 "*"
        ND_NUM @a63_expr_init.c:1:13 "2" val=2
        ND_NUM @a63_expr_init.c:1:17 "3" val=3
WANT

snippet a64_if_else <<'SNIP'
int f(int x) { if (x) return 1; else return 2; }
SNIP

expect a64_if_else <<'WANT'
ND_FUNCDEF @a64_if_else.c:1:14 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a64_if_else.c:1:16 "if" is_scope_block=1
    ND_IF @a64_if_else.c:1:16 "if"
      ND_IDENT @a64_if_else.c:1:20 "x"
      ND_RETURN @a64_if_else.c:1:23 "return"
        ND_NUM @a64_if_else.c:1:30 "1" val=1
      ND_RETURN @a64_if_else.c:1:38 "return"
        ND_NUM @a64_if_else.c:1:45 "2" val=2
WANT

snippet a65_loops <<'SNIP'
int f(int x) {
  for (int i = 0; i < 2; i++) x += i;
  while (x) x--;
  do x++; while (x < 3);
  return x;
}
SNIP

expect a65_loops <<'WANT'
ND_FUNCDEF @a65_loops.c:1:14 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a65_loops.c:2:3 "for" is_scope_block=1
    ND_FOR @a65_loops.c:2:3 "for"
      ND_LT @a65_loops.c:2:21 "<"
        ND_IDENT @a65_loops.c:2:19 "i"
        ND_NUM @a65_loops.c:2:23 "2" val=2
      ND_EXPR_STMT @a65_loops.c:2:31 "x"
        ND_ASSIGN @a65_loops.c:2:33 "+=" op=ND_ADD
          ND_IDENT @a65_loops.c:2:31 "x"
          ND_IDENT @a65_loops.c:2:36 "i"
      ND_DECL @a65_loops.c:2:12 "i" ty_op=int name="i" attr=none
        INIT kind=EXPR @a65_loops.c:2:16 eq=@a65_loops.c:2:14
          ND_NUM @a65_loops.c:2:16 "0" val=0
      ND_INCDEC @a65_loops.c:2:27 "++" is_post=1 addend=1
        ND_IDENT @a65_loops.c:2:26 "i"
    ND_WHILE @a65_loops.c:3:3 "while"
      ND_IDENT @a65_loops.c:3:10 "x"
      ND_EXPR_STMT @a65_loops.c:3:13 "x"
        ND_INCDEC @a65_loops.c:3:14 "--" is_post=1 addend=-1
          ND_IDENT @a65_loops.c:3:13 "x"
    ND_DO @a65_loops.c:4:3 "do"
      ND_LT @a65_loops.c:4:20 "<"
        ND_IDENT @a65_loops.c:4:18 "x"
        ND_NUM @a65_loops.c:4:22 "3" val=3
      ND_EXPR_STMT @a65_loops.c:4:6 "x"
        ND_INCDEC @a65_loops.c:4:7 "++" is_post=1 addend=1
          ND_IDENT @a65_loops.c:4:6 "x"
    ND_RETURN @a65_loops.c:5:3 "return"
      ND_IDENT @a65_loops.c:5:10 "x"
WANT

snippet a66_switch_case <<'SNIP'
int f(int x) { switch (x) { case 1 ... 2: x = 3; break; default: x = 4; } return x; }
SNIP

expect a66_switch_case <<'WANT'
ND_FUNCDEF @a66_switch_case.c:1:14 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a66_switch_case.c:1:16 "switch" is_scope_block=1
    ND_SWITCH @a66_switch_case.c:1:16 "switch"
      ND_IDENT @a66_switch_case.c:1:24 "x"
      ND_BLOCK @a66_switch_case.c:1:29 "case" is_scope_block=1
        ND_CASE @a66_switch_case.c:1:29 "case"
          ND_EXPR_STMT @a66_switch_case.c:1:43 "x"
            ND_ASSIGN @a66_switch_case.c:1:45 "="
              ND_IDENT @a66_switch_case.c:1:43 "x"
              ND_NUM @a66_switch_case.c:1:47 "3" val=3
          ND_NUM @a66_switch_case.c:1:34 "1" val=1
          ND_NUM @a66_switch_case.c:1:40 "2" val=2
        ND_BREAK @a66_switch_case.c:1:50 "break"
        ND_CASE @a66_switch_case.c:1:57 "default" default=1
          ND_EXPR_STMT @a66_switch_case.c:1:66 "x"
            ND_ASSIGN @a66_switch_case.c:1:68 "="
              ND_IDENT @a66_switch_case.c:1:66 "x"
              ND_NUM @a66_switch_case.c:1:70 "4" val=4
    ND_RETURN @a66_switch_case.c:1:75 "return"
      ND_IDENT @a66_switch_case.c:1:82 "x"
WANT

snippet a67_goto_label <<'SNIP'
int f(void) { goto out; out: return 0; }
SNIP

expect a67_goto_label <<'WANT'
ND_FUNCDEF @a67_goto_label.c:1:13 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a67_goto_label.c:1:15 "goto" is_scope_block=1
    ND_GOTO @a67_goto_label.c:1:15 "goto" label="out"
    ND_LABEL @a67_goto_label.c:1:25 "out" label="out"
      ND_RETURN @a67_goto_label.c:1:30 "return"
        ND_NUM @a67_goto_label.c:1:37 "0" val=0
WANT

snippet a68_asm <<'SNIP'
int f(void) { asm("nop"); return 0; }
SNIP

expect a68_asm <<'WANT'
ND_FUNCDEF @a68_asm.c:1:13 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a68_asm.c:1:15 "asm" is_scope_block=1
    ND_ASM @a68_asm.c:1:15 "asm" asm_str="nop"
    ND_BLOCK @a68_asm.c:1:25 ";"
    ND_RETURN @a68_asm.c:1:27 "return"
      ND_NUM @a68_asm.c:1:34 "0" val=0
WANT

snippet a69_block_scope <<'SNIP'
int f(int x) { { int y = x; x = y; } return x; }
SNIP

expect a69_block_scope <<'WANT'
ND_FUNCDEF @a69_block_scope.c:1:14 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a69_block_scope.c:1:16 "{" is_scope_block=1
    ND_BLOCK @a69_block_scope.c:1:18 "int" is_scope_block=1
      ND_DECL @a69_block_scope.c:1:22 "y" ty_op=int name="y" attr=none
        INIT kind=EXPR @a69_block_scope.c:1:26 eq=@a69_block_scope.c:1:24
          ND_IDENT @a69_block_scope.c:1:26 "x"
      ND_EXPR_STMT @a69_block_scope.c:1:29 "x"
        ND_ASSIGN @a69_block_scope.c:1:31 "="
          ND_IDENT @a69_block_scope.c:1:29 "x"
          ND_IDENT @a69_block_scope.c:1:33 "y"
    ND_RETURN @a69_block_scope.c:1:38 "return"
      ND_IDENT @a69_block_scope.c:1:45 "x"
WANT

snippet a70_return <<'SNIP'
int f(void) { return 1; }
SNIP

expect a70_return <<'WANT'
ND_FUNCDEF @a70_return.c:1:13 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a70_return.c:1:15 "return" is_scope_block=1
    ND_RETURN @a70_return.c:1:15 "return"
      ND_NUM @a70_return.c:1:22 "1" val=1
WANT

snippet a71_break_continue <<'SNIP'
int f(int x) { while (x) { if (x) break; continue; } return x; }
SNIP

expect a71_break_continue <<'WANT'
ND_FUNCDEF @a71_break_continue.c:1:14 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a71_break_continue.c:1:16 "while" is_scope_block=1
    ND_WHILE @a71_break_continue.c:1:16 "while"
      ND_IDENT @a71_break_continue.c:1:23 "x"
      ND_BLOCK @a71_break_continue.c:1:28 "if" is_scope_block=1
        ND_IF @a71_break_continue.c:1:28 "if"
          ND_IDENT @a71_break_continue.c:1:32 "x"
          ND_BREAK @a71_break_continue.c:1:35 "break"
        ND_CONTINUE @a71_break_continue.c:1:42 "continue"
    ND_RETURN @a71_break_continue.c:1:54 "return"
      ND_IDENT @a71_break_continue.c:1:61 "x"
WANT

snippet a72_atomic_builtins <<'SNIP'
int f(int *x) { __builtin_compare_and_swap(x, 1, 2); __builtin_atomic_exchange(x, 3); return 0; }
SNIP

expect a72_atomic_builtins <<'WANT'
ND_FUNCDEF @a72_atomic_builtins.c:1:15 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a72_atomic_builtins.c:1:17 "__builtin_compare_and_swap" is_scope_block=1
    ND_EXPR_STMT @a72_atomic_builtins.c:1:17 "__builtin_compare_and_swap"
      ND_CAS @a72_atomic_builtins.c:1:17 "__builtin_compare_and_swap"
        ND_IDENT @a72_atomic_builtins.c:1:44 "x"
        ND_NUM @a72_atomic_builtins.c:1:47 "1" val=1
        ND_NUM @a72_atomic_builtins.c:1:50 "2" val=2
    ND_EXPR_STMT @a72_atomic_builtins.c:1:54 "__builtin_atomic_exchange"
      ND_EXCH @a72_atomic_builtins.c:1:54 "__builtin_atomic_exchange"
        ND_IDENT @a72_atomic_builtins.c:1:80 "x"
        ND_NUM @a72_atomic_builtins.c:1:83 "3" val=3
    ND_RETURN @a72_atomic_builtins.c:1:87 "return"
      ND_NUM @a72_atomic_builtins.c:1:94 "0" val=0
WANT

snippet a73_decl_records <<'SNIP'
typedef int T;
enum E { A, B = 2 };
int g;
int h(int a);
int f(T x) { return h(x) + A + g; }
SNIP

expect a73_decl_records <<'WANT'
ND_TYPEDEF @a73_decl_records.c:1:13 "T"
ND_ENUM_CONST @a73_decl_records.c:2:10 "A" ty_op=enum
ND_ENUM_CONST @a73_decl_records.c:2:13 "B" ty_op=enum
  ND_NUM @a73_decl_records.c:2:17 "2" val=2
ND_GVAR_DECL @a73_decl_records.c:3:6 ";" ty_op=int name="g" attr=none
ND_FUNCDEF @a73_decl_records.c:4:13 ";" ty_op=func(int) name="h" attr=none
ND_FUNCDEF @a73_decl_records.c:5:12 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a73_decl_records.c:5:14 "return" is_scope_block=1
    ND_RETURN @a73_decl_records.c:5:14 "return"
      ND_ADD @a73_decl_records.c:5:30 "+"
        ND_ADD @a73_decl_records.c:5:26 "+"
          ND_FUNCALL @a73_decl_records.c:5:24 ")"
            ND_IDENT @a73_decl_records.c:5:21 "h"
            ND_IDENT @a73_decl_records.c:5:23 "x"
          ND_IDENT @a73_decl_records.c:5:28 "A"
        ND_IDENT @a73_decl_records.c:5:32 "g"
WANT

snippet a74_alignas <<'SNIP'
_Alignas(8) int g;
int f(void) { _Alignas(16) int x = 1; return x; }
SNIP

expect a74_alignas <<'WANT'
ND_GVAR_DECL @a74_alignas.c:1:18 ";" ty_op=int name="g" attr=none align=expr
  ND_NUM @a74_alignas.c:1:10 "8" val=8
ND_FUNCDEF @a74_alignas.c:2:13 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a74_alignas.c:2:15 "_Alignas" is_scope_block=1
    ND_DECL @a74_alignas.c:2:32 "x" ty_op=int name="x" attr=none align=expr
      INIT kind=EXPR @a74_alignas.c:2:36 eq=@a74_alignas.c:2:34
        ND_NUM @a74_alignas.c:2:36 "1" val=1
      ND_NUM @a74_alignas.c:2:24 "16" val=16
    ND_RETURN @a74_alignas.c:2:39 "return"
      ND_IDENT @a74_alignas.c:2:46 "x"
WANT

snippet a75_macro_synth <<'SNIP'
#define PLUS(a, b) ((a) + (b))
int f(int x) { return PLUS(x, 1); }
SNIP

expect a75_macro_synth <<'WANT'
ND_FUNCDEF @a75_macro_synth.c:2:14 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a75_macro_synth.c:2:16 "return" is_scope_block=1
    ND_RETURN @a75_macro_synth.c:2:16 "return"
      ND_ADD @synth "+"
        ND_IDENT @synth "x"
        ND_NUM @synth "1" val=1
WANT

snippet a76_invalid_incomplete <<'SNIP'
struct S;
int f(void) { return 1 + (struct S){0}; }
SNIP

expect a76_invalid_incomplete <<'WANT'
ND_FUNCDEF @a76_invalid_incomplete.c:2:13 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a76_invalid_incomplete.c:2:15 "return" is_scope_block=1
    ND_RETURN @a76_invalid_incomplete.c:2:15 "return"
      ND_ADD @a76_invalid_incomplete.c:2:24 "+"
        ND_NUM @a76_invalid_incomplete.c:2:22 "1" val=1
        ND_COMPOUND_LITERAL @a76_invalid_incomplete.c:2:26 "(" ty_op=struct
          INIT kind=LIST @a76_invalid_incomplete.c:2:36
            ITEM
              INIT kind=EXPR @a76_invalid_incomplete.c:2:37
                ND_NUM @a76_invalid_incomplete.c:2:37 "0" val=0
WANT

snippet a77_invalid_operands <<'SNIP'
int f(int *p) { return p * p; }
SNIP

expect a77_invalid_operands <<'WANT'
ND_FUNCDEF @a77_invalid_operands.c:1:15 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a77_invalid_operands.c:1:17 "return" is_scope_block=1
    ND_RETURN @a77_invalid_operands.c:1:17 "return"
      ND_MUL @a77_invalid_operands.c:1:26 "*"
        ND_IDENT @a77_invalid_operands.c:1:24 "p"
        ND_IDENT @a77_invalid_operands.c:1:28 "p"
WANT

snippet a78_invalid_ident <<'SNIP'
int f(void) { return unknown + 1; }
SNIP

expect a78_invalid_ident <<'WANT'
ND_FUNCDEF @a78_invalid_ident.c:1:13 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a78_invalid_ident.c:1:15 "return" is_scope_block=1
    ND_RETURN @a78_invalid_ident.c:1:15 "return"
      ND_ADD @a78_invalid_ident.c:1:30 "+"
        ND_IDENT @a78_invalid_ident.c:1:22 "unknown"
        ND_NUM @a78_invalid_ident.c:1:32 "1" val=1
WANT

snippet a79_comma_three <<'SNIP'
int f(int a, int b, int c) { return a, b, c; }
SNIP

expect a79_comma_three <<'WANT'
ND_FUNCDEF @a79_comma_three.c:1:28 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a79_comma_three.c:1:30 "return" is_scope_block=1
    ND_RETURN @a79_comma_three.c:1:30 "return"
      ND_COMMA @a79_comma_three.c:1:41 ","
        ND_COMMA @a79_comma_three.c:1:38 ","
          ND_IDENT @a79_comma_three.c:1:37 "a"
          ND_IDENT @a79_comma_three.c:1:40 "b"
        ND_IDENT @a79_comma_three.c:1:43 "c"
WANT

snippet a80_cond_assign_else <<'SNIP'
int f(int a, int b, int c, int d) { return a ? b : c = d; }
SNIP

expect a80_cond_assign_else <<'WANT'
ND_FUNCDEF @a80_cond_assign_else.c:1:35 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a80_cond_assign_else.c:1:37 "return" is_scope_block=1
    ND_RETURN @a80_cond_assign_else.c:1:37 "return"
      ND_ASSIGN @a80_cond_assign_else.c:1:54 "="
        ND_COND @a80_cond_assign_else.c:1:46 "?"
          ND_IDENT @a80_cond_assign_else.c:1:44 "a"
          ND_IDENT @a80_cond_assign_else.c:1:48 "b"
          ND_IDENT @a80_cond_assign_else.c:1:52 "c"
        ND_IDENT @a80_cond_assign_else.c:1:56 "d"
WANT

snippet a81_cond_assign_mid <<'SNIP'
int f(int a, int b, int c, int d) { return a ? b = c : d; }
SNIP

expect a81_cond_assign_mid <<'WANT'
ND_FUNCDEF @a81_cond_assign_mid.c:1:35 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a81_cond_assign_mid.c:1:37 "return" is_scope_block=1
    ND_RETURN @a81_cond_assign_mid.c:1:37 "return"
      ND_COND @a81_cond_assign_mid.c:1:46 "?"
        ND_IDENT @a81_cond_assign_mid.c:1:44 "a"
        ND_ASSIGN @a81_cond_assign_mid.c:1:50 "="
          ND_IDENT @a81_cond_assign_mid.c:1:48 "b"
          ND_IDENT @a81_cond_assign_mid.c:1:52 "c"
        ND_IDENT @a81_cond_assign_mid.c:1:56 "d"
WANT

snippet a82_cast_postfix <<'SNIP'
int f(int a) { return (int)a++ + (long)a--; }
SNIP

expect a82_cast_postfix <<'WANT'
ND_FUNCDEF @a82_cast_postfix.c:1:14 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a82_cast_postfix.c:1:16 "return" is_scope_block=1
    ND_RETURN @a82_cast_postfix.c:1:16 "return"
      ND_ADD @a82_cast_postfix.c:1:32 "+"
        ND_CAST @a82_cast_postfix.c:1:23 "(" ty_op=int
          ND_INCDEC @a82_cast_postfix.c:1:29 "++" is_post=1 addend=1
            ND_IDENT @a82_cast_postfix.c:1:28 "a"
        ND_CAST @a82_cast_postfix.c:1:34 "(" ty_op=long
          ND_INCDEC @a82_cast_postfix.c:1:41 "--" is_post=1 addend=-1
            ND_IDENT @a82_cast_postfix.c:1:40 "a"
WANT

snippet a83_postfix_chain <<'SNIP'
struct S { int m; };
int f(struct S *p, int a[2][3]) { return p[0].m + a[1][2]++; }
SNIP

expect a83_postfix_chain <<'WANT'
ND_FUNCDEF @a83_postfix_chain.c:2:33 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a83_postfix_chain.c:2:35 "return" is_scope_block=1
    ND_RETURN @a83_postfix_chain.c:2:35 "return"
      ND_ADD @a83_postfix_chain.c:2:49 "+"
        ND_MEMBER @a83_postfix_chain.c:2:47 "m"
          ND_SUBSCRIPT @a83_postfix_chain.c:2:43 "["
            ND_IDENT @a83_postfix_chain.c:2:42 "p"
            ND_NUM @a83_postfix_chain.c:2:44 "0" val=0
        ND_INCDEC @a83_postfix_chain.c:2:58 "++" is_post=1 addend=1
          ND_SUBSCRIPT @a83_postfix_chain.c:2:55 "["
            ND_SUBSCRIPT @a83_postfix_chain.c:2:52 "["
              ND_IDENT @a83_postfix_chain.c:2:51 "a"
              ND_NUM @a83_postfix_chain.c:2:53 "1" val=1
            ND_NUM @a83_postfix_chain.c:2:56 "2" val=2
WANT

snippet a84_unary_chain <<'SNIP'
int f(int a) { return - -a + !!a + ~-a; }
SNIP

expect a84_unary_chain <<'WANT'
ND_FUNCDEF @a84_unary_chain.c:1:14 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a84_unary_chain.c:1:16 "return" is_scope_block=1
    ND_RETURN @a84_unary_chain.c:1:16 "return"
      ND_ADD @a84_unary_chain.c:1:34 "+"
        ND_ADD @a84_unary_chain.c:1:28 "+"
          ND_NEG @a84_unary_chain.c:1:23 "-"
            ND_NEG @a84_unary_chain.c:1:25 "-"
              ND_IDENT @a84_unary_chain.c:1:26 "a"
          ND_NOT @a84_unary_chain.c:1:30 "!"
            ND_NOT @a84_unary_chain.c:1:31 "!"
              ND_IDENT @a84_unary_chain.c:1:32 "a"
        ND_BITNOT @a84_unary_chain.c:1:36 "~"
          ND_NEG @a84_unary_chain.c:1:37 "-"
            ND_IDENT @a84_unary_chain.c:1:38 "a"
WANT

snippet a85_string_expr <<'SNIP'
int f(void) { return "hi"[0]; }
SNIP

expect a85_string_expr <<'WANT'
ND_FUNCDEF @a85_string_expr.c:1:13 "{" ty_op=func(int) name="f" attr=none
  ND_BLOCK @a85_string_expr.c:1:15 "return" is_scope_block=1
    ND_RETURN @a85_string_expr.c:1:15 "return"
      ND_SUBSCRIPT @a85_string_expr.c:1:26 "["
        ND_STRING @a85_string_expr.c:1:22 "\"hi\""
        ND_NUM @a85_string_expr.c:1:27 "0" val=0
WANT

fail=""
count=0
for n in $names; do
    count=$((count + 1))
    if (cd "$tmp" && "$chibicc" -dump-ast "$n.c") > "$tmp/$n.actual" 2> "$tmp/$n.err"; then
        :
    else
        echo "ast $n ... failed (dump failed)"
        cat "$tmp/$n.err"
        fail="y"
        continue
    fi
    if diff -u "$tmp/$n.expected" "$tmp/$n.actual" > "$tmp/$n.diff" 2>&1; then
        echo "ast $n ... passed"
    else
        echo "ast $n ... failed (dump drift)"
        cat "$tmp/$n.diff"
        fail="y"
    fi
done

if [ -n "$fail" ]; then
    echo "ast lock: FAILED"
    exit 1
fi
echo "ast lock: $count cases byte-exact"
