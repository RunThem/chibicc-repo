#!/bin/bash
# trivia 保留测试 (PLAN.md 阶段 T1-T3): token 流的 trivia 通道与文本级保真.
#
# 本线不改任何既有输出的行为, 所以第一段锁的是现状: -E 的归一化(丢注释, 折空白,
# 续行拼接, 补末行换行, 丢 BOM)在本线所有步上逐字节保持. 后续段落随各步增量添加:
#   首段   (T0.1) - -E 的归一化锁定样本(实测于归档提交 e8c8608)
#   第二段 (T1.1) - -dump-tokens 的 token/trivia 流断言
#   第三段 (T1.2) - -reconstruct 的全量 round-trip(重建 == 原文件)
#
# 用法: test/trivia.sh ./chibicc (由 make test 与 make test-stage2 调用)

chibicc="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
tmp=`mktemp -d /tmp/chibicc-trivia-XXXXXX`
trap 'rm -rf $tmp' INT TERM HUP EXIT

check() {
    if [ $? -eq 0 ]; then
        echo "testing trivia/$1 ... passed"
    else
        echo "testing trivia/$1 ... failed"
        exit 1
    fi
}

n_e=0

# exp_e <name> <input-printf> <expected-printf>: 输入与期望直接写 printf 转义
# (\n, \xHH, \\), 跑 -E 后与期望文件逐字节比对(cmp, 含末行换行).
exp_e() {
    name=$1; shift
    n_e=$((n_e + 1))
    printf "$1" > "$tmp/$name.c"
    printf "$2" > "$tmp/$name.expected"
    $chibicc -E "$tmp/$name.c" > "$tmp/$name.out"
    cmp -s "$tmp/$name.out" "$tmp/$name.expected"
    check "-E/$name"
}

# ---- T0.1: -E 的归一化锁定 ----------------------------------------------------

# 注释被丢弃, 连续空白折叠为单个空格.
exp_e comment_ws 'int  a ;  /* c1 */ // c2\n' 'int a ;\n'

# 反斜杠续行直接拼接(`\` 与换行被删除, 不插入空格).
exp_e backslash_cont '1 +\\\n2\n' '1 +2\n'

# 行注释吞掉续行后的文本(行注释扫描不看续行).
exp_e line_comment_cont '// first \\\nstill comment\n' '\n'

# 无末行换行的输入被补出(read_file 的补 \n).
exp_e no_final_newline 'int x;' 'int x;\n'

# BOM 不输出.
exp_e bom '\xef\xbb\xbfint x;\n' 'int x;\n'

# 空文件输出仅一个换行.
exp_e empty '' '\n'

# 空行群折成单个换行(at_bol 只置一次).
exp_e blank_lines 'int\n\na;\n' 'int\na;\n'

echo "trivia lock: $n_e -E samples"
