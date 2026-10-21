#!/bin/bash
# trivia 保留测试 (plans/PLAN.md 阶段 T1-T3): token 流的 trivia 通道与文本级保真.
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

# 无末行换行: read_file 补出的 \n 不属于 raw 文本, 所以 dump 里没有它(EOF 行号仍是 2).
exp_dump no_final_newline 'int x;' \
'T 1 0 ident int\nR 1 3 ws \\x20\nT 1 4 ident x\nT 1 5 punct ;\nT 2 6 eof\n'

# 字符串字面量: token 文本里的 \ 与 t 是源码的两个字节, 转义后成 \\t.
exp_dump string_escape 'x = "a\\tb";\n' \
'T 1 0 ident x\nR 1 1 ws \\x20\nT 1 2 punct =\nR 1 3 ws \\x20\nT 1 4 str "a\\\\tb"\nT 1 10 punct ;\nR 1 11 nl \\n\nT 2 12 eof\n'

# 空文件: raw 里没有任何字节, 只剩 EOF(行号 2 来自补出的 \n).
exp_dump empty '' \
'T 2 0 eof\n'

# 只含注释的文件.
exp_dump comment_only '/* only */\n' \
'R 1 0 bc /*\\x20only\\x20*/\nR 1 10 nl \\n\nT 2 11 eof\n'

# ---- T2.1: 原始文本的 trivia(CRLF / 孤立 CR / BOM / 续行) ----------------------
# 坐标从"扫描缓冲"换成"原始文本"后, 这四类字节以原样出现在 R/T 行里.

# CRLF 是一个 nl piece, 文本保留 \r\n.
exp_dump crlf 'a\r\nb\r\n' \
'T 1 0 ident a\nR 1 1 nl \\r\\n\nT 2 3 ident b\nR 2 4 nl \\r\\n\nT 3 6 eof\n'

# 孤立 CR 也是一个换行(与 canonicalize 的旧行为一致).
exp_dump lone_cr 'a\rb\n' \
'T 1 0 ident a\nR 1 1 nl \\r\nT 2 2 ident b\nR 2 3 nl \\n\nT 3 4 eof\n'

# BOM 成为首 token 的前置 trivia, 原样可见.
exp_dump bom '\xef\xbb\xbfint x;\n' \
'R 1 0 bom \\xef\\xbb\\xbf\nT 1 3 ident int\nR 1 6 ws \\x20\nT 1 7 ident x\nT 1 8 punct ;\nR 1 9 nl \\n\nT 2 10 eof\n'

# 两 token 之间的续行是 cont piece(行号不因它改变).
exp_dump cont_between '1 +\\\n2\n' \
'T 1 0 pp-num 1\nR 1 1 ws \\x20\nT 1 2 punct +\nR 1 3 cont \\\\\\n\nT 1 5 pp-num 2\nR 1 6 nl \\n\nT 3 7 eof\n'

# 七类扫描点里的续行: 标识符 / pp-number / 字符串 / 字符 里的是拼写的一部分.
exp_dump cont_ident 'in\\\nt x;\n' \
'T 1 0 ident in\\\\\\nt\nR 1 5 ws \\x20\nT 1 6 ident x\nT 1 7 punct ;\nR 1 8 nl \\n\nT 3 9 eof\n'

exp_dump cont_ppnum '12\\\n3\n' \
'T 1 0 pp-num 12\\\\\\n3\nR 1 5 nl \\n\nT 3 6 eof\n'

exp_dump cont_string 'char *s = "ab\\\ncd";\n' \
'T 1 0 ident char\nR 1 4 ws \\x20\nT 1 5 punct *\nT 1 6 ident s\nR 1 7 ws \\x20\nT 1 8 punct =\nR 1 9 ws \\x20\nT 1 10 str "ab\\\\\\ncd"\nT 1 18 punct ;\nR 1 19 nl \\n\nT 3 20 eof\n'

exp_dump cont_char "char c = '\\\\\nx';\n" \
'T 1 0 ident char\nR 1 4 ws \\x20\nT 1 5 ident c\nR 1 6 ws \\x20\nT 1 7 punct =\nR 1 8 ws \\x20\nT 1 9 num \x27\\\\\\nx\x27\nT 1 14 punct ;\nR 1 15 nl \\n\nT 3 16 eof\n'

# 行注释与块注释里的续行把注释延续到下一行(lc 到下一个真实换行, bc 到闭合).
exp_dump cont_lc '// c1 \\\nstill\nint x;\n' \
'R 1 0 lc //\\x20c1\\x20\\\\\\nstill\nR 1 13 nl \\n\nT 3 14 ident int\nR 3 17 ws \\x20\nT 3 18 ident x\nT 3 19 punct ;\nR 3 20 nl \\n\nT 4 21 eof\n'

exp_dump cont_bc 'a /* x\\\ny */ b\n' \
'T 1 0 ident a\nR 1 1 ws \\x20\nR 1 2 bc /*\\x20x\\\\\\ny\\x20*/\nR 1 12 ws \\x20\nT 1 13 ident b\nR 1 14 nl \\n\nT 3 15 eof\n'

# 预处理指令行里的续行: 指令名被拼成一个标识符, 展开照旧.
exp_dump cont_directive '#def\\\nine M 1\nint x = M;\n' \
'T 1 0 punct #\nT 1 1 ident def\\\\\\nine\nR 1 9 ws \\x20\nT 1 10 ident M\nR 1 11 ws \\x20\nT 1 12 pp-num 1\nR 1 13 nl \\n\nT 3 14 ident int\nR 3 17 ws \\x20\nT 3 18 ident x\nR 3 19 ws \\x20\nT 3 20 punct =\nR 3 21 ws \\x20\nT 3 22 ident M\nT 3 23 punct ;\nR 3 24 nl \\n\nT 4 25 eof\n'

# 续行 + universal chars 在同一标识符里: 拼写含 `\`+换行和 \u00e9 原样.
exp_dump cont_unicode 'int caf\\\n\\u00e9 = 1;\n' \
'T 1 0 ident int\nR 1 3 ws \\x20\nT 1 4 ident caf\\\\\\n\\\\u00e9\nR 1 15 ws \\x20\nT 1 16 punct =\nR 1 17 ws \\x20\nT 1 18 pp-num 1\nT 1 19 punct ;\nR 1 20 nl \\n\nT 3 21 eof\n'

# 深层混合: BOM + 块注释内续行(跨 CRLF) + CRLF + 词间续行 + 行注释, 且无末行换行.
exp_dump mixed_deep '\xef\xbb\xbf/* a \\\r\n b */ int x\r\n = 1 +\\\n2 ;  // tail' \
'R 1 0 bom \\xef\\xbb\\xbf\nR 1 3 bc /*\\x20a\\x20\\\\\\r\\n\\x20b\\x20*/\nR 1 16 ws \\x20\nT 1 17 ident int\nR 1 20 ws \\x20\nT 1 21 ident x\nR 1 22 nl \\r\\n\nR 3 24 ws \\x20\nT 3 25 punct =\nR 3 26 ws \\x20\nT 3 27 pp-num 1\nR 3 28 ws \\x20\nT 3 29 punct +\nR 3 30 cont \\\\\\n\nT 3 32 pp-num 2\nR 3 33 ws \\x20\nT 3 34 punct ;\nR 3 35 ws \\x20\\x20\nR 3 37 lc //\\x20tail\nT 5 44 eof\n'

# ---- T1.2/T2.1: -reconstruct 的重建合同(重建 == 原文件) ------------------------

# 全量: test/*.c 与 include/*.h 的每个文件重建后与原文件逐字节相同. T1.2 的例外
# 清单(三处续行文件与 test/unicode.c)随 T2.1 清零: 坐标换到原始文本后, CRLF /
# 孤立 CR / BOM / 末行换行 / 续行 / universal chars 都按原样重建, 这里不再有 skip;
# 若将来出现例外, 必须与 plans/PLAN.md 的账本对账.
n_rt=0

for f in test/*.c include/*.h; do
    $chibicc -xc -reconstruct "$f" > "$tmp/rec.out"
    cmp -s "$tmp/rec.out" "$f"
    n_rt=$((n_rt + 1))
    check "-reconstruct/$f"
done

n_rb=0

# exp_rec <name> <input-printf> <expected-printf>: 边界样本. T2.1 之后期望一律
# 等于输入(这些正是 T1.2 锁过、当时期望小于输入的偏差样本, 现在全部转正).
exp_rec() {
    name=$1; shift
    n_rb=$((n_rb + 1))
    printf "$1" > "$tmp/r-$name.c"
    printf "$2" > "$tmp/r-$name.expected"
    $chibicc -reconstruct "$tmp/r-$name.c" > "$tmp/r-$name.out"
    cmp -s "$tmp/r-$name.out" "$tmp/r-$name.expected"
    check "-reconstruct/$name"
}

# CRLF 保原样.
exp_rec crlf 'a\r\nb\r\n' 'a\r\nb\r\n'

# 孤立 CR 保原样.
exp_rec lone_cr 'a\rb\n' 'a\rb\n'

# 无末行换行保原样(不再补出).
exp_rec no_final_newline 'int x;' 'int x;'

# BOM 保原样(成为首 token 的前置 trivia).
exp_rec bom '\xef\xbb\xbfint x;\n' '\xef\xbb\xbfint x;\n'

# 续行保原样(含反斜杠与换行).
exp_rec continuation '1 +\\\n2\n' '1 +\\\n2\n'

# 续行落在标识符与字符串里(拼写的一部分).
exp_rec cont_ident 'in\\\nt x;\n' 'in\\\nt x;\n'
exp_rec cont_string 'char *s = "ab\\\ncd";\n' 'char *s = "ab\\\ncd";\n'

# universal chars 保原拼写(raw 保留转义, 扫描缓冲里才是 UTF-8).
exp_rec universal_chars 'char *s = "\\u00e9";\n' 'char *s = "\\u00e9";\n'

# 续行 + universal chars 在同一标识符里.
exp_rec cont_unicode 'int caf\\\n\\u00e9 = 1;\n' 'int caf\\\n\\u00e9 = 1;\n'

# CRLF + 续行 + 末尾无换行.
exp_rec mixed 'a\r\nb\\\r\nc' 'a\r\nb\\\r\nc'

# 深层混合: BOM + 块注释内续行(跨 CRLF) + CRLF + 词间续行 + 行注释, 且无末行换行.
exp_rec mixed_deep '\xef\xbb\xbf/* a \\\r\n b */ int x\r\n = 1 +\\\n2 ;  // tail' \
'\xef\xbb\xbf/* a \\\r\n b */ int x\r\n = 1 +\\\n2 ;  // tail'

# 空文件.
exp_rec empty '' ''

# ---- T2.1: 行号与插入符锚点(旧行为逐字节保持) ---------------------------------
# 续行的行号语义来自被退役的 remove_backslash_newline 的"挪换行"技巧, 由
# make_scan_buffer 的 lines 表复现: 续行内的 token 报逻辑行起始的物理行号且插入
# 符行显示拼接后的文本, 续行之后的 token 报物理行号. 期望是在 T1.2 二进制(与归档
# 提交 e8c8608 行为逐字节相同)上实测并逐字节对照过的.

n_dg=0

# exp_diag <name> <input-printf> <expected-stderr-printf>: 在 $tmp 里以相对文件
# 名跑 -S(诊断里出现稳定文件名), 与期望 stderr 逐字节比对.
exp_diag() {
    name=$1; shift
    n_dg=$((n_dg + 1))
    printf "$1" > "$tmp/$name.c"
    printf "$2" > "$tmp/$name.expected"
    (cd "$tmp" && "$chibicc" -S -o /dev/null "$name.c" > /dev/null 2> "$name.out")
    cmp -s "$tmp/$name.out" "$tmp/$name.expected"
    check "diag/$name"
}

# 续行内的 token 报逻辑行起始的物理行号, 插入符行显示拼接后的文本.
exp_diag p2b 'int y;\nint x = 1 +\\\n@;\n' \
'p2b.c:2: int x = 1 +@;\n                    ^ expected an expression\n'

# 续行之后的 token 报物理行号.
exp_diag p2 'int y;\nint x = 1 +\\\n2;\n@\n' \
'p2.c:4: @\n        ^ variable name omitted\n'

# CRLF 的行号与显示(\r 不进入打印行).
exp_diag p3 'int x = ;\r\nint y;\r\n' \
'p3.c:1: int x = ;\n                ^ expected an expression\n'

# .loc 同源: 续行合并处的全部 .loc 落在逻辑行起始号(1).
n_dg=$((n_dg + 1))
printf 'int f(void) { int x = 1 +\\\n2; return x; }\n' > "$tmp/p2c.c"
p2c_loc=$(cd "$tmp" && "$chibicc" -S -o - p2c.c | grep '^  \.loc ' | sort -u)
[ "$p2c_loc" = "  .loc 1 1" ]
check "diag/p2c_loc"

echo "trivia lock: $n_e -E samples, $n_d dump samples, $n_rt round-trip files, $n_rb boundary samples, $n_dg anchor cases"
