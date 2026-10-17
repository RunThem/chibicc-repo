#!/bin/bash
# CST 无损测试 (PLAN.md 阶段 C1): 第 2 层(预处理前的 lossless 具体语法树).
#
# CST 只走三个只读通道, 不接编译路径:
#   -reconstruct-cst - 按叶子序输出 trivia + 拼写, 逐字节重建输入文件
#   -verify-cst      - 检查叶子按序与 token 流指针一一对应(每个 token 恰好一次)
#   -dump-cst        - 锁树形; 叶子文本按 trivia.sh 的转义规则(空格也转义为 \x20),
#                      EOF 无文本, 缩进固定两空格
#
# 本线每步都是行为不变步, 本脚本按步增量添加段落:
#   C1.1 - 叶子树的全量 round-trip(48 文件)/ 12 个边界样本 / -verify-cst / 叶子 dump
#   C1.2 - 定界符与语句骨架
#   C2.x - 表达式与声明 / 语句结构
#   C3.x - 预处理指令与条件分组
#
# 用法: test/cst.sh ./chibicc (由 make test 与 make test-stage2 调用)

chibicc="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
tmp=`mktemp -d /tmp/chibicc-cst-XXXXXX`
trap 'rm -rf $tmp' INT TERM HUP EXIT

check() {
    if [ $? -eq 0 ]; then
        echo "testing cst/$1 ... passed"
    else
        echo "testing cst/$1 ... failed"
        exit 1
    fi
}

# ---- C1.1: 叶子树 --------------------------------------------------------------

# 全量: test/*.c 与 include/*.h 的每个文件经 CST 叶子重建后与原文件逐字节相同.
# 这正是 trivia 线的 -reconstruct 合同, 但走树而不是 token 链; 语料清单与
# test/trivia.sh 的全量循环一致(41 + 7).
n_rt=0

for f in test/*.c include/*.h; do
    $chibicc -xc -reconstruct-cst "$f" > "$tmp/rec.out"
    cmp -s "$tmp/rec.out" "$f"
    n_rt=$((n_rt + 1))
    check "-reconstruct-cst/$f"
done

# -verify-cst: 叶子序列与 tokenize_file 的输出逐指针相等, 且无遗漏 / 重复.
n_vf=0

for f in test/*.c include/*.h; do
    $chibicc -xc -verify-cst "$f"
    n_vf=$((n_vf + 1))
    check "-verify-cst/$f"
done

# 边界样本: 输入文本里的字节原样穿过树(CRLF / 孤立 CR / BOM / 续行 / universal
# chars / 无末行换行 / 空文件), 期望一律等于输入.
n_rb=0

# exp_rec <name> <input-printf>: 重建 == 输入, 且同一棵树上 -verify-cst 通过.
exp_rec() {
    name=$1; shift
    n_rb=$((n_rb + 1))
    printf "$1" > "$tmp/r-$name.c"
    $chibicc -reconstruct-cst "$tmp/r-$name.c" > "$tmp/r-$name.out"
    cmp -s "$tmp/r-$name.out" "$tmp/r-$name.c"
    check "-reconstruct-cst/$name"
    $chibicc -verify-cst "$tmp/r-$name.c"
    check "-verify-cst/$name"
}

exp_rec crlf 'a\r\nb\r\n'
exp_rec lone_cr 'a\rb\n'
exp_rec no_final_newline 'int x;'
exp_rec bom '\xef\xbb\xbfint x;\n'
exp_rec continuation '1 +\\\n2\n'
exp_rec cont_ident 'in\\\nt x;\n'
exp_rec cont_string 'char *s = "ab\\\ncd";\n'
exp_rec universal_chars 'char *s = "\\u00e9";\n'
exp_rec cont_unicode 'int caf\\\n\\u00e9 = 1;\n'
exp_rec mixed 'a\r\nb\\\r\nc'
exp_rec mixed_deep '\xef\xbb\xbf/* a \\\r\n b */ int x\r\n = 1 +\\\n2 ;  // tail'
exp_rec empty ''

# 叶子树 dump: 缩进两空格, 叶子行是种类 + 原始拼写, EOF 无文本. 空文件的树只有
# EOF; EOF 同时收文件尾部 trivia, 所以 dump 里看不到它, round-trip 里不能少.
n_d=0

# exp_dump <name> <input-printf> <expected-printf>: 期望里的 dump 转义要再转义一层.
exp_dump() {
    name=$1; shift
    n_d=$((n_d + 1))
    printf "$1" > "$tmp/d-$name.c"
    printf "$2" > "$tmp/d-$name.expected"
    $chibicc -dump-cst "$tmp/d-$name.c" > "$tmp/d-$name.out"
    cmp -s "$tmp/d-$name.out" "$tmp/d-$name.expected"
    check "-dump-cst/$name"
    $chibicc -verify-cst "$tmp/d-$name.c"
    check "-verify-cst/$name"
}

# 空文件: 没有 token, 只剩收尾的 EOF.
exp_dump empty '' \
'TU\n  EOF\n'

# 单 token.
exp_dump single 'int\n' \
'TU\n  STMT\n    TOKEN int\n  EOF\n'

# 多 token: 每个 token 与标点各占一行, 不合并同类项.
exp_dump multi 'int x;\n' \
'TU\n  STMT\n    TOKEN int\n    TOKEN x\n    TOKEN ;\n  EOF\n'

# 转义: 字符串字面量里的空格转成 \x20, 引号原样(它属于拼写).
exp_dump string_space 'char *s = "a b";\n' \
'TU\n  STMT\n    TOKEN char\n    TOKEN *\n    TOKEN s\n    TOKEN =\n    TOKEN "a\\x20b"\n    TOKEN ;\n  EOF\n'

# EOF 收尾 trivia: 末尾的注释与空行不进 dump, 但重建逐字节保留它们.
exp_dump eof_trivia 'a /* tail */\n\n' \
'TU\n  STMT\n    TOKEN a\n  EOF\n'
$chibicc -reconstruct-cst "$tmp/d-eof_trivia.c" > "$tmp/d-eof_trivia.out"
cmp -s "$tmp/d-eof_trivia.out" "$tmp/d-eof_trivia.c"
check "-reconstruct-cst/eof_trivia"

# ---- C1.2: 定界符与语句骨架 ----------------------------------------------------

# 顶层与块内按 ';' 切 STMT; 每个定界符成组, 开闭符号是显式叶子. 语句切分只发生
# 在语句层, 组内部保持原样 - for 头里的两个分号不是语句边界.
exp_dump statements 'int x; y = 1;\n' \
'TU\n  STMT\n    TOKEN int\n    TOKEN x\n    TOKEN ;\n  STMT\n    TOKEN y\n    TOKEN =\n    TOKEN 1\n    TOKEN ;\n  EOF\n'

exp_dump nested_delims 'f((a), [b]);\n' \
'TU\n  STMT\n    TOKEN f\n    GROUP_PAREN\n      TOKEN (\n      GROUP_PAREN\n        TOKEN (\n        TOKEN a\n        TOKEN )\n      TOKEN ,\n      GROUP_BRACKET\n        TOKEN [\n        TOKEN b\n        TOKEN ]\n      TOKEN )\n    TOKEN ;\n  EOF\n'

exp_dump for_header 'for (;;) ;\n' \
'TU\n  STMT\n    TOKEN for\n    GROUP_PAREN\n      TOKEN (\n      TOKEN ;\n      TOKEN ;\n      TOKEN )\n    TOKEN ;\n  EOF\n'

# 块: 花括号体在语句层单独成 BLOCK, 空语句是 NULL_STMT.
exp_dump block '{ int x; ; }\n' \
'TU\n  BLOCK\n    TOKEN {\n    STMT\n      TOKEN int\n      TOKEN x\n      TOKEN ;\n    NULL_STMT\n      TOKEN ;\n    TOKEN }\n  EOF\n'

exp_dump nested_block '{ { } }\n' \
'TU\n  BLOCK\n    TOKEN {\n    BLOCK\n      TOKEN {\n      TOKEN }\n    TOKEN }\n  EOF\n'

# 语句里出现的花括号(初始化器 / 函数体)只是原始组, 不按块切分.
exp_dump brace_in_stmt 'int a[] = {1, 2};\n' \
'TU\n  STMT\n    TOKEN int\n    TOKEN a\n    GROUP_BRACKET\n      TOKEN [\n      TOKEN ]\n    TOKEN =\n    GROUP_BRACE\n      TOKEN {\n      TOKEN 1\n      TOKEN ,\n      TOKEN 2\n      TOKEN }\n    TOKEN ;\n  EOF\n'

# UNPARSED: 未闭合的组 / 块, 以及语句层没有开括号的孤立闭括号. 已消费的 token
# 全部留在树里, 重建与校验照旧.
exp_dump unclosed_paren 'int x = (1 + 2;\n' \
'TU\n  STMT\n    TOKEN int\n    TOKEN x\n    TOKEN =\n    UNPARSED\n      TOKEN (\n      TOKEN 1\n      TOKEN +\n      TOKEN 2\n      TOKEN ;\n  EOF\n'

exp_dump unclosed_block '{ int x;\n' \
'TU\n  UNPARSED\n    TOKEN {\n    STMT\n      TOKEN int\n      TOKEN x\n      TOKEN ;\n  EOF\n'

exp_dump stray_close ');\n' \
'TU\n  UNPARSED\n    TOKEN )\n  NULL_STMT\n    TOKEN ;\n  EOF\n'

for f in "$tmp"/d-statements.c "$tmp"/d-nested_delims.c "$tmp"/d-for_header.c \
         "$tmp"/d-block.c "$tmp"/d-nested_block.c "$tmp"/d-brace_in_stmt.c \
         "$tmp"/d-unclosed_paren.c "$tmp"/d-unclosed_block.c "$tmp"/d-stray_close.c; do
    $chibicc -reconstruct-cst "$f" > "$tmp/rec2.out"
    cmp -s "$tmp/rec2.out" "$f"
    check "-reconstruct-cst/$(basename "$f" .c)"
done

echo "cst lock: $n_rt round-trip files, $n_vf verify files, $n_rb boundary samples, $n_d dump samples"
