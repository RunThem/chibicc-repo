#!/bin/bash
# 警告锁定测试 (C4): 数组初始化器的超额元素逐元素给出诊断, 编译继续.
# 每个用例编译必须成功, stderr 与期望逐字节 diff. 用 -S 输出到
# /dev/null, 不依赖汇编器, 因此本机 (macOS) 也能跑.
#
# 用法: test/warning.sh ./chibicc (由 make test 与 make test-stage2 调用)

chibicc="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
tmp=`mktemp -d /tmp/chibicc-warning-XXXXXX`
trap 'rm -rf $tmp' INT TERM HUP EXIT

names=""

# snippet <name>: 后随 heredoc 为触发该警告的源码片段.
snippet() {
    names="$names $1"
    cat > "$tmp/$1.c"
}

# expect <name>: 后随 heredoc 为期望的 stderr 原文(逐字节).
expect() {
    cat > "$tmp/$1.expected"
}

snippet w01_excess_array_elements <<'SNIP'
int a[2] = {1, 2, 3, 4};
SNIP

expect w01_excess_array_elements <<'WANT'
w01_excess_array_elements.c:1: int a[2] = {1, 2, 3, 4};
                                                 ^ excess elements in array initializer
w01_excess_array_elements.c:1: int a[2] = {1, 2, 3, 4};
                                                    ^ excess elements in array initializer
WANT

fail=""
count=0
for n in $names; do
    count=$((count + 1))
    if ! (cd "$tmp" && "$chibicc" -S -o /dev/null "$n.c") 2> "$tmp/$n.actual"; then
        echo "warning $n ... failed (expected success)"
        fail="y"
        continue
    fi
    if diff -u "$tmp/$n.expected" "$tmp/$n.actual" > "$tmp/$n.diff" 2>&1; then
        echo "warning $n ... passed"
    else
        echo "warning $n ... failed (stderr drift)"
        cat "$tmp/$n.diff"
        fail="y"
    fi
done

if [ -n "$fail" ]; then
    echo "warning lock: FAILED"
    exit 1
fi
echo "warning lock: $count samples byte-exact"
