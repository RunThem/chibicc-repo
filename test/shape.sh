#!/bin/bash
# 形状断言测试 (PLAN B1.3): 阶段 B 各发射点的指令形回归锁.
#
# 阶段 B 把"只需换发射方式"的整形项从改写成树里搬进了发射点, 生成的指令序列
# 逐字节保持(唯一预期变化是 elvis 的临时槽). 本文件的断言锁的是"直接发射仍产出
# 该形态", 不是"旧树形消失"的证明 - .s 里看不见 DEREF/FOR 这类树形, 树形的消失
# 由源码层核对证明(整形遍不再含对应 case, 见 PLAN.md 阶段 B 段与 RESULT.md 各步).
#
# 断言按步增量添加: B1.1a 下标 / B1.1b 箭头成员 / B1.1c GT-GE / B1.2a while 与
# break-continue / B1.1e elvis(无槽) / B1.2b ND_DECL(若做).
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
        else
            if grep -qE "$arg" "$tmp/$n.s"; then
                echo "shape $n ... FAILED (unexpected: $arg)"
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
