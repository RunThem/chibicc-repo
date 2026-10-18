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

# ---- M1.2: 对象宏与函数宏的 body 层 ---------------------------------------------
# 宏体 token 的位置是定义行(拼写坐标); 链的 entry 记录调用点 / 层 / 定义处.

# 对象宏: 宏体 token 的 chain 是 <macro>@<调用>:body:<定义>.
exp_dump obj '#define M 1\nint x = M;\n' \
'T obj.c:2 keyword int -
T obj.c:2 ident x -
T obj.c:2 punct = -
T obj.c:1 num 1 M@obj.c:2:body:obj.c:1
T obj.c:2 punct ; -
'

# 函数宏: 宏体(不引用实参时)同样只有 body 层; 调用范围由 rparen 提供.
exp_dump fn '#define F(x) 42\nint y = F(1);\n' \
'T fn.c:2 keyword int -
T fn.c:2 ident y -
T fn.c:2 punct = -
T fn.c:1 num 42 F@fn.c:2:body:fn.c:1
T fn.c:2 punct ; -
'

# 嵌套对象宏: 链最内层在前, 外层的调用点是宏体里那个名字 token 的拼写位置.
exp_dump nest '#define A 1\n#define B A\nint x = B;\n' \
'T nest.c:3 keyword int -
T nest.c:3 ident x -
T nest.c:3 punct = -
T nest.c:1 num 1 A@nest.c:2:body:nest.c:1>B@nest.c:3:body:nest.c:2
T nest.c:3 punct ; -
'

# #undef / 重定义: 记录持有展开时刻的 Macro, 旧展开仍指向旧定义.
exp_dump redef '#define M 1\nint a = M;\n#undef M\n#define M 2\nint b = M;\n' \
'T redef.c:2 keyword int -
T redef.c:2 ident a -
T redef.c:2 punct = -
T redef.c:1 num 1 M@redef.c:2:body:redef.c:1
T redef.c:2 punct ; -
T redef.c:5 keyword int -
T redef.c:5 ident b -
T redef.c:5 punct = -
T redef.c:4 num 2 M@redef.c:5:body:redef.c:4
T redef.c:5 punct ; -
'

# #line 之后: dump 用 line_no - line_delta 还原物理行(#define 行的 token 未
# 经过 pass-through, line_delta 仍为 0, 也还原正确).
exp_dump line '#line 100\n#define M 1\nint x = M;\n' \
'T line.c:3 keyword int -
T line.c:3 ident x -
T line.c:3 punct = -
T line.c:2 num 1 M@line.c:3:body:line.c:2
T line.c:3 punct ; -
'

# 空宏: 不产生 token, 因而不出现记录(PLAN.md 判据 1 的已知限制).
exp_dump empty_macro '#define E\nE\nint x;\n' \
'T empty_macro.c:3 keyword int -
T empty_macro.c:3 ident x -
T empty_macro.c:3 punct ; -
'

# 头文件定义 + 主文件调用: 定义处与调用处可以不同文件.
printf '#define M 7\n' > "$tmp/defs.h"
exp_dump hdr '#include "defs.h"\nint x = M;\n' \
'T hdr.c:2 keyword int -
T hdr.c:2 ident x -
T hdr.c:2 punct = -
T ./defs.h:1 num 7 M@hdr.c:2:body:./defs.h:1
T hdr.c:2 punct ; -
'

exp_verify obj '#define M 1\nint x = M;\n'
exp_verify nest '#define A 1\n#define B A\nint x = B;\n'
exp_verify hdr '#include "defs.h"\nint x = M;\n'

echo "expand lock: $n_d dump samples, $n_v verify samples"
