#!/bin/bash
# trivia 保留测试 (PLAN.md 阶段 T1-T3): token 流的 trivia 通道与文本级保真.
#
# 本线不改任何既有输出的行为, 所以第一段锁的是现状: -E 的归一化(丢注释, 折空白,
# 续行拼接, 补末行换行, 丢 BOM)在本线所有步上逐字节保持. 第二段锁 -dump-tokens:
# 每个 T 行是一个 token, 每个 R 行是它前面被跳过的字节(行注释 lc / 块注释 bc /
# 换行 nl / 空白串 ws), 文本按 C 字面量式转义(空格也转义为 \x20, 行尾无空白,
# 空文本的 token - 只有 EOF - 省略文本字段). 后续段落随各步增量添加:
#   首段   (T0.1) - -E 的归一化锁定样本(实测于归档提交 e8c8608)
#   第二段 (T1.1) - -dump-tokens 的 token/trivia 流断言
#   第三段 (T1.2) - -reconstruct 的全量 round-trip(重建 == 原文件)与边界样本
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

# ---- T1.1: -dump-tokens 的 trivia 通道 -----------------------------------------

n_d=0

# exp_dump <name> <input-printf> <expected-printf>: 输入与期望直接写 printf 转义,
# 期望文件里 dump 的转义文本要再转义一层(\x20 写成 \\x20, \n 写成 \\n).
exp_dump() {
    name=$1; shift
    n_d=$((n_d + 1))
    printf "$1" > "$tmp/d-$name.c"
    printf "$2" > "$tmp/d-$name.expected"
    $chibicc -dump-tokens "$tmp/d-$name.c" > "$tmp/d-$name.out"
    cmp -s "$tmp/d-$name.out" "$tmp/d-$name.expected"
    check "-dump/$name"
}

# 行注释与行尾 trivia: 注释与它前面的空白都是 R 行, EOF 收尾部的换行.
exp_dump line_comment 'int a; // x\n' \
'T 1 0 ident int\nR 1 3 ws \\x20\nT 1 4 ident a\nT 1 5 punct ;\nR 1 6 ws \\x20\nR 1 7 lc //\\x20x\nR 1 11 nl \\n\nT 2 12 eof\n'

# 跨行块注释: 一个 bc piece 跨两行, R 行的行号是注释的起始行, 之后的 token 在第二行.
exp_dump block_comment 'a /* one\ntwo */ b\n' \
'T 1 0 ident a\nR 1 1 ws \\x20\nR 1 2 bc /*\\x20one\\ntwo\\x20*/\nR 2 15 ws \\x20\nT 2 16 ident b\nR 2 17 nl \\n\nT 3 18 eof\n'

# 空白与制表符: 行首是 \t, 词间是两个空格(空格转义为 \x20, 制表符为 \t).
exp_dump ws_tabs '\tint  x;\n' \
'R 1 0 ws \\t\nT 1 1 ident int\nR 1 4 ws \\x20\\x20\nT 1 6 ident x\nT 1 7 punct ;\nR 1 8 nl \\n\nT 2 9 eof\n'

# 空行群: 每个换行是独立的 nl piece, R 行的行号逐行递增.
exp_dump blank_lines 'a\n\n\nb\n' \
'T 1 0 ident a\nR 1 1 nl \\n\nR 2 2 nl \\n\nR 3 3 nl \\n\nT 4 4 ident b\nR 4 5 nl \\n\nT 5 6 eof\n'

# 注释与空白混合: ws / bc / ws / lc / nl 五连.
exp_dump mixed 'a /* c */ // d\nb\n' \
'T 1 0 ident a\nR 1 1 ws \\x20\nR 1 2 bc /*\\x20c\\x20*/\nR 1 9 ws \\x20\nR 1 10 lc //\\x20d\nR 1 14 nl \\n\nT 2 15 ident b\nR 2 16 nl \\n\nT 3 17 eof\n'

# 行首 trivia: 文件头两行空行加缩进都挂在第一个 token 上.
exp_dump bol_trivia '  \n\n  int x;\n' \
'R 1 0 ws \\x20\\x20\nR 1 2 nl \\n\nR 2 3 nl \\n\nR 3 4 ws \\x20\\x20\nT 3 6 ident int\nR 3 9 ws \\x20\nT 3 10 ident x\nT 3 11 punct ;\nR 3 12 nl \\n\nT 4 13 eof\n'

# 无末行换行: read_file 补出的 \n 在流里可见(T1.2 的 round-trip 例外由此而来).
exp_dump no_final_newline 'int x;' \
'T 1 0 ident int\nR 1 3 ws \\x20\nT 1 4 ident x\nT 1 5 punct ;\nR 1 6 nl \\n\nT 2 7 eof\n'

# 字符串字面量: token 文本里的 \ 与 t 是源码的两个字节, 转义后成 \\t.
exp_dump string_escape 'x = "a\\tb";\n' \
'T 1 0 ident x\nR 1 1 ws \\x20\nT 1 2 punct =\nR 1 3 ws \\x20\nT 1 4 str "a\\\\tb"\nT 1 10 punct ;\nR 1 11 nl \\n\nT 2 12 eof\n'

# 空文件: 只有补出的换行与 EOF.
exp_dump empty '' \
'R 1 0 nl \\n\nT 2 1 eof\n'

# 只含注释的文件.
exp_dump comment_only '/* only */\n' \
'R 1 0 bc /*\\x20only\\x20*/\nR 1 10 nl \\n\nT 2 11 eof\n'

# ---- T1.2: -reconstruct 的重建合同(重建 == 原文件) ------------------------------

# 全量: test/*.c 与 include/*.h 的每个文件重建后与原文件逐字节相同. 例外清单
# (计划内的破坏性前置换写, 括号里是退役步骤):
#   test/literal.c, include/stdarg.h, include/stdatomic.h - 反斜杠续行 (T2.1)
#   test/unicode.c - universal chars (T2.2)
# 例外只许缩小, 缩小步骤: 先删对应的 skip, 再把下面的边界样本改成"重建 == 输入".
n_rt=0
exc="test/literal.c include/stdarg.h include/stdatomic.h test/unicode.c"

for f in test/*.c include/*.h; do
    case " $exc " in *" $f "*) continue;; esac
    $chibicc -xc -reconstruct "$f" > "$tmp/rec.out"
    cmp -s "$tmp/rec.out" "$f"
    n_rt=$((n_rt + 1))
    check "-reconstruct/$f"
done

n_rb=0

# exp_rec <name> <input-printf> <expected-printf>: 边界样本, 期望锁的是当前已知的
# 偏差(T2.1/T2.2 退役对应 pass 后, 这些期望改成等于输入).
exp_rec() {
    name=$1; shift
    n_rb=$((n_rb + 1))
    printf "$1" > "$tmp/r-$name.c"
    printf "$2" > "$tmp/r-$name.expected"
    $chibicc -reconstruct "$tmp/r-$name.c" > "$tmp/r-$name.out"
    cmp -s "$tmp/r-$name.out" "$tmp/r-$name.expected"
    check "-reconstruct/$name"
}

# CRLF 被折成 \n (canonicalize_newline; T2.1 退役).
exp_rec crlf 'a\r\nb\r\n' 'a\nb\n'

# 孤立 CR 被折成 \n (同上).
exp_rec lone_cr 'a\rb\n' 'a\nb\n'

# 无末行换行被补出 (read_file; T2.1 改为 File 级事实后不再出现在重建里).
exp_rec no_final_newline 'int x;' 'int x;\n'

# BOM 被丢掉 (tokenize_file; T2.1 同上).
exp_rec bom '\xef\xbb\xbfint x;\n' 'int x;\n'

# 反斜杠续行被删, 换行数挪到下一个真实换行之后 (remove_backslash_newline; T2.1 归 trivia).
exp_rec continuation '1 +\\\n2\n' '1 +2\n\n'

# universal chars 被就地改写成 UTF-8 (convert_universal_chars; T2.2 后保原拼写).
exp_rec universal_chars 'char *s = "\\u00e9";\n' 'char *s = "\xc3\xa9";\n'

echo "trivia lock: $n_e -E samples, $n_d dump samples, $n_rt round-trip files, $n_rb boundary samples"
