#!/bin/bash
# 形状断言测试 (PLAN B1.3): 阶段 B 各发射点的指令形回归锁.
#
# 阶段 B 把"只需换发射方式"的整形项从改写成树里搬进了发射点, 生成的指令序列
# 逐字节保持(唯一预期变化是 elvis 的临时槽). 本文件的断言锁的是"直接发射仍产出
# 该形态", 不是"旧树形消失"的证明 - .s 里看不见 DEREF/FOR 这类树形, 树形的消失
# 由源码层核对证明(整形遍不再含对应 case, 见 PLAN.md 阶段 B 段与 RESULT.md 各步).
#
# 断言按步增量添加, 覆盖清单(共 33 段):
#   b11a 下标(读/写/交换写法/VLA/多级)      - PLAN B1.1a
#   b11b 箭头成员/点成员/展平匿名链/复合赋值 - PLAN B1.1b
#   b11c 整数/无符号/浮点/长双精度 的 > 与 >= - PLAN B1.1c
#   b11d 字符串/sizeof/泛型(A9.1 已直读)     - PLAN B1.1 的核对项
#   b11e elvis 无槽(int 形 / long double 形) - PLAN B1.1e
#   b12a while 循环形 / break 与 continue / switch 只占 break - PLAN B1.2a
#   b12b 声明的初始化链                      - PLAN B1.2b
#   b2   加法缩放的十种落点(读/交换写法/减/ptr-ptr 除法/纯整数负例/混合转换/
#        VLA 基址/复合赋值/自增自减/指示符初始化) - PLAN B2
#
# 用法: test/shape.sh ./chibicc (由 make test 与 make test-stage2 调用)

chibicc="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
tmp=`mktemp -d /tmp/chibicc-shape-XXXXXX`
trap 'rm -rf $tmp' INT TERM HUP EXIT

: > "$tmp/checks"
names=""

# snippet <name>: 后随 heredoc 为源码片段; 末尾统一用 -S 汇编到 $tmp/<name>.s.
snippet() {
    names="$names $1"
    cat > "$tmp/$1.c"
}

# want <name> <mnemonic>...: 断言这些助记符在 .s 里按序出现(每行取首个词, 允许
# 中间夹杂其它指令与 .loc 行; 立即数与偏移不参与比较, 帧布局变化不会误报).
want() {
    name=$1; shift
    echo "$name W $*" >> "$tmp/checks"
}

# absent <name> <regex>: 断言 .s 里没有匹配该正则的行(锁"不得退化成的形态").
absent() {
    name=$1; shift
    echo "$name A $1" >> "$tmp/checks"
}

# wantline <name> <regex>: 断言 .s 里有一行匹配该正则(用于标号与跳转目标这类
# 不在行首的信息).
wantline() {
    name=$1; shift
    echo "$name L $1" >> "$tmp/checks"
}

# ---- B1.1a ND_SUBSCRIPT: `x[y]` 在发射点建缩放和, 不经中间树形 ----------------
# 读: 下标先按元素大小缩放(imul), 再加到基址(add); 无辅助调用.
snippet b11a_subscript_read <<'SNIP'
int f(int *p, long i) { return p[i]; }
SNIP
want b11a_subscript_read imul add
absent b11a_subscript_read '^  call'

# 写: 同一个地址计算落在 store 前(缩放仍在加之前).
snippet b11a_subscript_store <<'SNIP'
void f(int *p, long i) { p[i] = 1; }
SNIP
want b11a_subscript_store imul add

# 交换写法 `i[p]`: new_add 归一到 `p + i`, 形态与 `p[i]` 相同.
snippet b11a_subscript_swapped <<'SNIP'
int f(int *p, long i) { return i[p]; }
SNIP
want b11a_subscript_swapped imul add

# VLA 下标: 缩放因子是运行时的尺寸变量(一次取变量 + imul), 形态不变.
snippet b11a_subscript_vla <<'SNIP'
int f(int n, long i) { int a[n]; a[i] = 1; return a[i]; }
SNIP
want b11a_subscript_vla imul add

# 多级下标: 外层下标先求值它的(已缩放的)索引, 内层地址随后, 所以两个 imul 在前,
# 两个 add 在后 - 与降级形态逐字节一致.
snippet b11a_subscript_2d <<'SNIP'
int f(int a[2][3], long i, long j) { return a[i][j]; }
SNIP
want b11a_subscript_2d imul imul add add

# ---- B1.1b ND_MEMBER(arrow): 箭头成员读指针值 + 偏移, 点成员取操作数地址 ----
# 读(箭头): 先取指针的值(mov, 参数直接寻址), 再加成员偏移(add).
snippet b11b_member_arrow_read <<'SNIP'
struct S { int x; };
int f(struct S *p) { return p->x; }
SNIP
want b11b_member_arrow_read mov add movsxd
absent b11b_member_arrow_read '^  call'

# 读(点): 取的是操作数的地址(lea), 无指针取值 - 与箭头形区分.
snippet b11b_member_dot_read <<'SNIP'
struct S { int x; };
int f(struct S s) { return s.x; }
SNIP
want b11b_member_dot_read lea add movsxd

# 读(展平匿名链): 每一级 link 各加一次偏移(两个 add).
snippet b11b_member_anon_read <<'SNIP'
struct S { struct { int a; }; };
int f(struct S *p) { return p->a; }
SNIP
want b11b_member_anon_read add add

# 复合赋值(展平匿名链): 承载对象的地址经临时槽写入与读回, 链上的偏移仍在
# 发射点解析(指针取值 mov + 偏移 add).
snippet b11b_member_anon_op_assign <<'SNIP'
struct S { struct { int a; }; };
void f(struct S *p, int v) { p->a += v; }
SNIP
want b11b_member_anon_op_assign mov add mov
absent b11b_member_anon_op_assign '^  call'

# ---- B1.1c ND_GT/ND_GE: 交换在发射点做, 条件与求值顺序镜像降级形态 --------
# 整数 `>`: 左操作数先求值(降级后它落在 rhs 位), 条件取 setl - 等价于降级后的
# `<` 形; 不得取 setg(说明换了发射形态).
snippet b11c_gt_int <<'SNIP'
int f(int a, int b) { return a > b; }
SNIP
want b11c_gt_int cmp setl movzb
absent b11c_gt_int 'setg'

# 无符号 `>`: 条件取 setb(不得取 seta).
snippet b11c_gt_unsigned <<'SNIP'
int f(unsigned a, unsigned b) { return a > b; }
SNIP
want b11c_gt_unsigned cmp setb
absent b11c_gt_unsigned 'seta'

# 无符号 `>=`: 条件取 setbe.
snippet b11c_ge_unsigned <<'SNIP'
int f(unsigned a, unsigned b) { return a >= b; }
SNIP
want b11c_ge_unsigned cmp setbe

# 浮点 `>`: ucomi + seta(左操作数先求值).
snippet b11c_gt_float <<'SNIP'
int f(float a, float b) { return a > b; }
SNIP
want b11c_gt_float ucomiss seta

# long double `>`: fcomip + seta.
snippet b11c_gt_ldouble <<'SNIP'
int f(long double a, long double b) { return a > b; }
SNIP
want b11c_gt_ldouble fcomip seta

# ---- B1.2a ND_WHILE + ND_BREAK/ND_CONTINUE: 循环形与标签绑定在发射点 -------
# while 的形: begin 标号 + 条件 je 到 brk + 结尾 jmp 回 begin, cont 标号落在
# 收尾跳转之前 - 与旧的 `while` 降级成 `for` 的发射逐字节相同.
snippet b12a_while <<'SNIP'
int f(int n) { int s = 0; while (n) { s += n; n--; } return s; }
SNIP
want b12a_while je jmp
wantline b12a_while '^  je \.L\.\.2$'
wantline b12a_while '^  jmp \.L\.begin\.1$'
wantline b12a_while '^\.L\.\.3:$'

# break 与 continue 各自跳到本循环的两个标号(该形里 2 = brk, 3 = cont).
snippet b12a_break_continue <<'SNIP'
int f(int n) { while (n) { if (n == 3) break; n--; if (n > 5) continue; } return n; }
SNIP
wantline b12a_break_continue '^  jmp \.L\.\.2$'
wantline b12a_break_continue '^  jmp \.L\.\.3$'

# switch 只占 break 目标: 它里面的 continue 跳到循环的 cont(3), 它里面的
# break 跳到 switch 自己的 brk(4).
snippet b12a_switch_continue <<'SNIP'
int f(int n) { while (n) { switch (n) { case 2: continue; case 3: break; } n--; } return n; }
SNIP
wantline b12a_switch_continue '^  jmp \.L\.\.3$'
wantline b12a_switch_continue '^  jmp \.L\.\.4$'

# ---- B1.1e elvis: 测试值留在寄存器, 不建临时槽 -----------------------------
# int 形: 值留在 rax, 真值路径直跳 end; 该形里不出现经 %rdi 的间接存
# (旧降级把测试值存进临时槽又读回, 那正是本步去掉的东西).
snippet b11e_elvis_int <<'SNIP'
int f(int a, int b) { return a ?: b; }
SNIP
want b11e_elvis_int cmp je jmp
wantline b11e_elvis_int '^  je \.L\.else\.1$'
absent b11e_elvis_int '%\(%rdi\)$'

# long double 形: 测试值用 `fld %st(0)` 预复制(cmp_zero 的 fldz/fucomip/fstp
# 会吃掉 x87 栈顶), 真值路径把它留作结果, 假值路径显式丢弃; 不再有 fstpt 写槽.
snippet b11e_elvis_ldouble <<'SNIP'
long double f(long double a, long double b) { return a ?: b; }
SNIP
want b11e_elvis_ldouble fldt fldz fucomip fstp
absent b11e_elvis_ldouble 'fstpt'

# ---- B1.1d 已由 A9.1 直读的三项: 补上它们的指令形回归锁 ---------------------
# 字符串字面量: 表达式里用的是匿名全局的地址(lea .L..N(%rip)), 不复制内容.
snippet b11d_string <<'SNIP'
char *f(void) { return "hello"; }
SNIP
wantline b11d_string '^  lea \.L\.\.[0-9]+\(%rip\), %rax$'
absent b11d_string '^  call'

# sizeof/_Alignof: 结论记在 val 里, 直接发立即数.
snippet b11d_sizeof <<'SNIP'
int f(void) { return sizeof(int) + _Alignof(long); }
SNIP
wantline b11d_sizeof '^  mov \$4, %rax$'
wantline b11d_sizeof '^  mov \$8, %rax$'

# _Generic: 只有选中项进发射, 控制表达式与未选中项不发码.
snippet b11d_generic <<'SNIP'
int f(void) { return _Generic(1, int: 11, default: 22); }
SNIP
wantline b11d_generic '^  mov \$11, %rax$'
absent b11d_generic '\$22'

# ---- B1.2b ND_DECL: 声明在发射点摊成初始化链 ------------------------------
# 定长聚合的初始化 = 先整体清零(`rep stosb`)再逐元素赋值, 形状与旧降级相同;
# 语句仍发在声明所在的位置.
snippet b12b_decl_init <<'SNIP'
int f(void) { int a[3] = {1, 2, 3}; return a[2]; }
SNIP
wantline b12b_decl_init '^  rep stosb$'
want b12b_decl_init rep mov

# ---- B2 加法的缩放: 忠实 `+`/`-` 在发射点降级 -------------------------------
# 降级由 new_add/new_sub 现做(元素大小缩放 = imul, 规范化 = 交换操作数),
# 指令序列与旧的整形遍就地改写逐字节相同; 节点带 is_lowered 标记的降级形态
# (下标, 复合赋值, 指示符)不得被二次缩放 - 十段落点各锁一处.
# 指针 + 整数: 缩放(imul)在加(add)之前; 不出现辅助调用.
snippet b2_add_scale <<'SNIP'
int *f(int *p, long i) { return p + i; }
SNIP
want b2_add_scale imul add
absent b2_add_scale '^  call'

# 整数 + 指针: 归一到 `p + i` 后形态相同(语义由 test/pointer.c 的运行断言锁).
snippet b2_add_swapped <<'SNIP'
int *f(int *p, long i) { return i + p; }
SNIP
want b2_add_swapped imul add

# 指针 - 整数: 缩放后在减(sub)之前.
snippet b2_sub_scale <<'SNIP'
int *f(int *p, long i) { return p - i; }
SNIP
want b2_sub_scale imul sub

# 指针 - 指针: 差的元素数经 cqo/idiv 缩放 - 元素大小是除数, 所以不得出现乘法形.
snippet b2_ptr_diff <<'SNIP'
long f(int *p, int *q) { return p - q; }
SNIP
want b2_ptr_diff sub cqo idiv
wantline b2_ptr_diff '^  idiv %rdi$'
absent b2_ptr_diff '^  imul'

# 纯整数 + 的负例: 不缩放(锁住"不把普通加法当指针算术").
snippet b2_int_add <<'SNIP'
int f(int a, int b) { return a + b; }
SNIP
want b2_int_add movsxd add
absent b2_int_add 'imul'

# 混合宽度的 +: 发射点降级仍走 new_arith 的常规算术转换(int 提升到 long).
snippet b2_mixed_conv <<'SNIP'
long f(int a, long b) { return a + b; }
SNIP
wantline b2_mixed_conv '^  movsxd %eax, %rax$'
want b2_mixed_conv add

# 指针到 VLA 的加法: 缩放因子是运行时的尺寸变量(读 vla_size 槽 + imul).
snippet b2_vla_ptr <<'SNIP'
int f(int n, long i) { int (*p)[n] = 0; return (*(p + i))[0]; }
SNIP
want b2_vla_ptr imul add

# 复合赋值: 缩放只做一次(在 combine 的读改写值里), 指令形与旧降级相同.
snippet b2_compound_ptr <<'SNIP'
void f(int *p, long i) { p += i; }
SNIP
want b2_compound_ptr imul add

# 后缀自增: `(p += 1) + (-1)` - 前后两次缩放各一次(两个 imul: 先 -4, 后 +4),
# 差值以负数形式最后相加(外层是 ADD 而不是 SUB).
snippet b2_post_inc_ptr <<'SNIP'
int *f(int *p) { return p++; }
SNIP
want b2_post_inc_ptr imul imul add add

# 指示符初始化: 初始化链里的下标地址同样由 new_add 现建, 只缩放一次.
snippet b2_designator <<'SNIP'
int f(void) { int a[4] = {[2] = 7}; return a[2]; }
SNIP
want b2_designator imul add

# ---- 汇总 ----------------------------------------------------------------
fail=""
count=0
for n in $names; do
    count=$((count + 1))
    if ! "$chibicc" -S -o "$tmp/$n.s" -xc - < "$tmp/$n.c" 2> "$tmp/$n.err"; then
        echo "shape $n ... compile failed"
        cat "$tmp/$n.err"
        fail="y"
        continue
    fi
    ok="y"
    while read -r name kind arg; do
        [ "$name" = "$n" ] || continue
        if [ "$kind" = "W" ]; then
            if ! awk -v want="$arg" '
                BEGIN { n = split(want, a, " "); k = 1 }
                { if (k <= n && $1 == a[k]) k++ }
                END { exit (k > n) ? 0 : 1 }' "$tmp/$n.s"; then
                echo "shape $n ... FAILED (missing sequence: $arg)"
                ok="n"
            fi
        elif [ "$kind" = "A" ]; then
            if grep -qE "$arg" "$tmp/$n.s"; then
                echo "shape $n ... FAILED (unexpected: $arg)"
                ok="n"
            fi
        else
            if ! grep -qE "$arg" "$tmp/$n.s"; then
                echo "shape $n ... FAILED (missing line: $arg)"
                ok="n"
            fi
        fi
    done < "$tmp/checks"
    [ "$ok" = "y" ] && echo "shape $n ... passed" || fail="y"
done

if [ -n "$fail" ]; then
    echo "shape lock: FAILED"
    exit 1
fi
echo "shape lock: $count snippets"
