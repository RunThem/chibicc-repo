#!/bin/bash
# 宏展开 provenance 测试 (PLAN.md M1.1-M2.2, 路线图阶段 5 的前置).
#
# 两个只读通道:
#   -dump-expansions   - preprocess() 之后每个非 EOF token 一行:
#                        T <file>:<phys-line|@synth> <kind> <spelling> <chain>
#   -verify-expansions - 无输出; 成功 0, 失败 1.
#
# 本线每步都是行为不变步, 本脚本按步增量添加段落:
#   M1.1 - 通道骨架: 无宏样本的 dump(chain 全为 -)与 verify
#
# 用法: test/expand.sh ./chibicc (由 make test 与 make test-stage2 调用)

chibicc="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
tmp=`mktemp -d /tmp/chibicc-expand-XXXXXX`
trap 'rm -rf $tmp' INT TERM HUP EXIT

check() {
    if [ $? -eq 0 ]; then
        echo "testing expand/$1 ... passed"
    else
        echo "testing expand/$1 ... failed"
        exit 1
    fi
}

# exp_dump <name> <input-printf> <expected-printf>: 在 $tmp 里以相对文件名
# 跑 -dump-expansions, 与期望文件逐字节比对(文件名因此稳定为 <name>.c).
n_d=0
exp_dump() {
    name=$1; shift
    n_d=$((n_d + 1))
    printf "$1" > "$tmp/$name.c"
    printf "$2" > "$tmp/$name.expected"
    (cd "$tmp" && "$chibicc" -dump-expansions "$name.c") > "$tmp/$name.out"
    cmp -s "$tmp/$name.out" "$tmp/$name.expected"
    check "-dump/$name"
}

# exp_verify <name> <input-printf>: -verify-expansions 必须成功且无输出.
n_v=0
exp_verify() {
    name=$1; shift
    n_v=$((n_v + 1))
    printf "$1" > "$tmp/$name.c"
    (cd "$tmp" && "$chibicc" -verify-expansions "$name.c") > "$tmp/$name.out" 2>&1
    [ ! -s "$tmp/$name.out" ]
    check "-verify/$name"
}

# ---- M1.1: 通道骨架 ------------------------------------------------------------

# 无宏的文件: 每个 token 一行, 位置是物理行, chain 为 -.
exp_dump plain 'int x = 1;\n' \
'T plain.c:1 keyword int -
T plain.c:1 ident x -
T plain.c:1 punct = -
T plain.c:1 num 1 -
T plain.c:1 punct ; -
'

# 预处理指令不产出 token; 行号是物理行.
exp_dump directive '#define M 1\n\nint x;\n' \
'T directive.c:3 keyword int -
T directive.c:3 ident x -
T directive.c:3 punct ; -
'

# 空文件: EOF 不输出, 因而 dump 为空.
exp_dump empty '' ''

exp_verify plain 'int x = 1;\n'
exp_verify directive '#define M 1\nint x = M;\n'
exp_verify empty ''

echo "expand lock: $n_d dump samples, $n_v verify samples"
