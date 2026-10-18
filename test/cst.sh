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
#   C3.1 - 预处理指令与条件分组
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
'TU\n  UNPARSED\n    TOKEN int\n  EOF\n'

# 多 token: 每个 token 与标点各占一行, 不合并同类项. C2.2 起这是声明.
exp_dump multi 'int x;\n' \
'TU\n  DECL\n    DECL_SPECS\n      TOKEN int\n    DECLARATOR\n      TOKEN x\n    TOKEN ;\n  EOF\n'

# 转义: 字符串字面量里的空格转成 \x20, 引号原样(它属于拼写).
exp_dump string_space 'char *s = "a b";\n' \
'TU\n  DECL\n    DECL_SPECS\n      TOKEN char\n    INIT_DECLARATOR\n      DECLARATOR\n        TOKEN *\n        TOKEN s\n      TOKEN =\n      PRIMARY\n        TOKEN "a\\x20b"\n    TOKEN ;\n  EOF\n'

# EOF 收尾 trivia: 末尾的注释与空行不进 dump, 但重建逐字节保留它们.
exp_dump eof_trivia 'a /* tail */\n\n' \
'TU\n  UNPARSED\n    TOKEN a\n  EOF\n'
$chibicc -reconstruct-cst "$tmp/d-eof_trivia.c" > "$tmp/d-eof_trivia.out"
cmp -s "$tmp/d-eof_trivia.out" "$tmp/d-eof_trivia.c"
check "-reconstruct-cst/eof_trivia"

# ---- C1.2: 定界符与语句骨架 ----------------------------------------------------

# 顶层与块内按 ';' 切语句; 每个定界符成组, 开闭符号是显式叶子. 语句切分只发生
# 在语句层, 组内部保持原样 - for 头里的两个分号不是语句边界.
exp_dump statements 'int x; y = 1;\n' \
'TU\n  DECL\n    DECL_SPECS\n      TOKEN int\n    DECLARATOR\n      TOKEN x\n    TOKEN ;\n  EXPR_STMT\n    ASSIGN\n      PRIMARY\n        TOKEN y\n      TOKEN =\n      PRIMARY\n        TOKEN 1\n    TOKEN ;\n  EOF\n'

exp_dump nested_delims 'f((a), [b]);\n' \
'TU\n  UNPARSED\n    TOKEN f\n    GROUP_PAREN\n      TOKEN (\n      GROUP_PAREN\n        TOKEN (\n        TOKEN a\n        TOKEN )\n      TOKEN ,\n      GROUP_BRACKET\n        TOKEN [\n        TOKEN b\n        TOKEN ]\n      TOKEN )\n    TOKEN ;\n  EOF\n'

exp_dump for_header 'for (;;) ;\n' \
'TU\n  FOR\n    TOKEN for\n    TOKEN (\n    TOKEN ;\n    TOKEN ;\n    TOKEN )\n    NULL_STMT\n      TOKEN ;\n  EOF\n'

# 块: 花括号体在语句层单独成 BLOCK, 空语句是 NULL_STMT.
exp_dump block '{ int x; ; }\n' \
'TU\n  BLOCK\n    TOKEN {\n    DECL\n      DECL_SPECS\n        TOKEN int\n      DECLARATOR\n        TOKEN x\n      TOKEN ;\n    NULL_STMT\n      TOKEN ;\n    TOKEN }\n  EOF\n'

exp_dump nested_block '{ { } }\n' \
'TU\n  BLOCK\n    TOKEN {\n    BLOCK\n      TOKEN {\n      TOKEN }\n    TOKEN }\n  EOF\n'

# 语句里出现的花括号在 C2.2 起是初始化列表; 设计符与嵌套列表在 C2.2 段落里锁.
exp_dump brace_in_stmt 'int a[] = {1, 2};\n' \
'TU\n  DECL\n    DECL_SPECS\n      TOKEN int\n    INIT_DECLARATOR\n      DECLARATOR\n        TOKEN a\n        GROUP_BRACKET\n          TOKEN [\n          TOKEN ]\n      TOKEN =\n      INIT_LIST\n        TOKEN {\n        INITIALIZER\n          PRIMARY\n            TOKEN 1\n        TOKEN ,\n        INITIALIZER\n          PRIMARY\n            TOKEN 2\n        TOKEN }\n    TOKEN ;\n  EOF\n'

# UNPARSED: 未闭合的组 / 块, 以及语句层没有开括号的孤立闭括号. 已消费的 token
# 全部留在树里, 重建与校验照旧.
exp_dump unclosed_paren 'int x = (1 + 2;\n' \
'TU\n  UNPARSED\n    TOKEN int\n    TOKEN x\n    TOKEN =\n    UNPARSED\n      TOKEN (\n      TOKEN 1\n      TOKEN +\n      TOKEN 2\n      TOKEN ;\n  EOF\n'

exp_dump unclosed_block '{ int x;\n' \
'TU\n  UNPARSED\n    TOKEN {\n    DECL\n      DECL_SPECS\n        TOKEN int\n      DECLARATOR\n        TOKEN x\n      TOKEN ;\n  EOF\n'

exp_dump stray_close ');\n' \
'TU\n  UNPARSED\n    TOKEN )\n  NULL_STMT\n    TOKEN ;\n  EOF\n'

for f in "$tmp"/d-statements.c "$tmp"/d-nested_delims.c "$tmp"/d-for_header.c \
         "$tmp"/d-block.c "$tmp"/d-nested_block.c "$tmp"/d-brace_in_stmt.c \
         "$tmp"/d-unclosed_paren.c "$tmp"/d-unclosed_block.c "$tmp"/d-stray_close.c; do
    $chibicc -reconstruct-cst "$f" > "$tmp/rec2.out"
    cmp -s "$tmp/rec2.out" "$f"
    check "-reconstruct-cst/$(basename "$f" .c)"
done

# ---- C2.1: 表达式结构 ----------------------------------------------------------

# 整条优先级链: * / % 高于 + - 高于 << >> 高于关系 高于相等 高于 & ^ | &&
# ||, 全部左结合. 每个运算符都是显式叶子.
exp_dump precedence 'a * b + c << d < e == f & g ^ h | i && j || k;\n' \
'TU\n  EXPR_STMT\n    BINARY\n      BINARY\n        BINARY\n          BINARY\n            BINARY\n              BINARY\n                BINARY\n                  BINARY\n                    BINARY\n                      BINARY\n                        PRIMARY\n                          TOKEN a\n                        TOKEN *\n                        PRIMARY\n                          TOKEN b\n                      TOKEN +\n                      PRIMARY\n                        TOKEN c\n                    TOKEN <<\n                    PRIMARY\n                      TOKEN d\n                  TOKEN <\n                  PRIMARY\n                    TOKEN e\n                TOKEN ==\n                PRIMARY\n                  TOKEN f\n              TOKEN &\n              PRIMARY\n                TOKEN g\n            TOKEN ^\n            PRIMARY\n              TOKEN h\n          TOKEN |\n          PRIMARY\n            TOKEN i\n        TOKEN &&\n        PRIMARY\n          TOKEN j\n      TOKEN ||\n      PRIMARY\n        TOKEN k\n    TOKEN ;\n  EOF\n'

# 赋值右结合; 左结合已由上一条锁定.
exp_dump assign_chain 'a = b = c;\n' \
'TU\n  EXPR_STMT\n    ASSIGN\n      PRIMARY\n        TOKEN a\n      TOKEN =\n      ASSIGN\n        PRIMARY\n          TOKEN b\n        TOKEN =\n        PRIMARY\n          TOKEN c\n    TOKEN ;\n  EOF\n'

# 括号是显式 PAREN 节点, 不改写树形.
exp_dump paren_explicit '(a + b) * c;\n' \
'TU\n  EXPR_STMT\n    BINARY\n      PAREN\n        TOKEN (\n        BINARY\n          PRIMARY\n            TOKEN a\n          TOKEN +\n          PRIMARY\n            TOKEN b\n        TOKEN )\n      TOKEN *\n      PRIMARY\n        TOKEN c\n    TOKEN ;\n  EOF\n'

# 调用 / 下标 / 成员 / 后缀自增: 每个都是 POSTFIX, 运算符与括号是叶子.
exp_dump postfix_chain 'f(x)[i].m->p++;\n' \
'TU\n  EXPR_STMT\n    POSTFIX\n      POSTFIX\n        POSTFIX\n          POSTFIX\n            POSTFIX\n              PRIMARY\n                TOKEN f\n              ARG_LIST\n                TOKEN (\n                PRIMARY\n                  TOKEN x\n                TOKEN )\n            TOKEN [\n            PRIMARY\n              TOKEN i\n            TOKEN ]\n          TOKEN .\n          TOKEN m\n        TOKEN ->\n        TOKEN p\n      TOKEN ++\n    TOKEN ;\n  EOF\n'

# 条件表达式右结合; `?:` 是 GNU 的 elvis, 没有中间操作数.
exp_dump conditional 'a ? b : c ? d : e;\n' \
'TU\n  EXPR_STMT\n    COND\n      PRIMARY\n        TOKEN a\n      TOKEN ?\n      PRIMARY\n        TOKEN b\n      TOKEN :\n      COND\n        PRIMARY\n          TOKEN c\n        TOKEN ?\n        PRIMARY\n          TOKEN d\n        TOKEN :\n        PRIMARY\n          TOKEN e\n    TOKEN ;\n  EOF\n'

exp_dump elvis 'a ?: b;\n' \
'TU\n  EXPR_STMT\n    COND\n      PRIMARY\n        TOKEN a\n      TOKEN ?\n      TOKEN :\n      PRIMARY\n        TOKEN b\n    TOKEN ;\n  EOF\n'

# 逗号是优先级最低的二元节点.
exp_dump comma 'a = 1, b = 2;\n' \
'TU\n  EXPR_STMT\n    COMMA\n      ASSIGN\n        PRIMARY\n          TOKEN a\n        TOKEN =\n        PRIMARY\n          TOKEN 1\n      TOKEN ,\n      ASSIGN\n        PRIMARY\n          TOKEN b\n        TOKEN =\n        PRIMARY\n          TOKEN 2\n    TOKEN ;\n  EOF\n'

# 前缀一元运算符与后缀自减.
exp_dump unary_ops '*p + -x + !y + ~z + &w + ++i + j--;\n' \
'TU\n  EXPR_STMT\n    BINARY\n      BINARY\n        BINARY\n          BINARY\n            BINARY\n              BINARY\n                UNARY\n                  TOKEN *\n                  PRIMARY\n                    TOKEN p\n                TOKEN +\n                UNARY\n                  TOKEN -\n                  PRIMARY\n                    TOKEN x\n              TOKEN +\n              UNARY\n                TOKEN !\n                PRIMARY\n                  TOKEN y\n            TOKEN +\n            UNARY\n              TOKEN ~\n              PRIMARY\n                TOKEN z\n          TOKEN +\n          UNARY\n            TOKEN &\n            PRIMARY\n              TOKEN w\n        TOKEN +\n        UNARY\n          TOKEN ++\n          PRIMARY\n            TOKEN i\n      TOKEN +\n      POSTFIX\n        PRIMARY\n          TOKEN j\n        TOKEN --\n    TOKEN ;\n  EOF\n'

# 括号内以类型开头判 cast / sizeof(type); 否则括号里是表达式.
exp_dump sizeof_type '(int)x + sizeof(int) + sizeof y;\n' \
'TU\n  EXPR_STMT\n    BINARY\n      BINARY\n        CAST\n          TYPE_NAME\n            TOKEN (\n            DECL_SPECS\n              TOKEN int\n            TOKEN )\n          PRIMARY\n            TOKEN x\n        TOKEN +\n        UNARY\n          TOKEN sizeof\n          TYPE_NAME\n            TOKEN (\n            DECL_SPECS\n              TOKEN int\n            TOKEN )\n      TOKEN +\n      UNARY\n        TOKEN sizeof\n        PRIMARY\n          TOKEN y\n    TOKEN ;\n  EOF\n'

exp_dump sizeof_expr 'sizeof(a + b);\n' \
'TU\n  EXPR_STMT\n    UNARY\n      TOKEN sizeof\n      PAREN\n        TOKEN (\n        BINARY\n          PRIMARY\n            TOKEN a\n          TOKEN +\n          PRIMARY\n            TOKEN b\n        TOKEN )\n    TOKEN ;\n  EOF\n'

# 文件内 typedef 是 cast 的判据; 声明本身现在也是 DECL.
exp_dump typedef_cast 'typedef int T;\n(T)z;\n' \
'TU\n  DECL\n    DECL_SPECS\n      TOKEN typedef\n      TOKEN int\n    DECLARATOR\n      TOKEN T\n    TOKEN ;\n  EXPR_STMT\n    CAST\n      TYPE_NAME\n        TOKEN (\n        DECL_SPECS\n          TOKEN T\n        TOKEN )\n      PRIMARY\n        TOKEN z\n    TOKEN ;\n  EOF\n'

# 结构体 typedef: 预扫描跳过成员名, 只收 T; 结构体体在 DECL_SPECS 里是原始组.
exp_dump typedef_struct 'typedef struct { int x; } T;\n(T)z;\n' \
'TU\n  DECL\n    DECL_SPECS\n      TOKEN typedef\n      TOKEN struct\n      GROUP_BRACE\n        TOKEN {\n        TOKEN int\n        TOKEN x\n        TOKEN ;\n        TOKEN }\n    DECLARATOR\n      TOKEN T\n    TOKEN ;\n  EXPR_STMT\n    CAST\n      TYPE_NAME\n        TOKEN (\n        DECL_SPECS\n          TOKEN T\n        TOKEN )\n      PRIMARY\n        TOKEN z\n    TOKEN ;\n  EOF\n'

# 未知名字(U)不猜: 不作为 cast, 整条语句回退 UNPARSED.
exp_dump unknown_type 'typedef int T;\n(U)z;\n' \
'TU\n  DECL\n    DECL_SPECS\n      TOKEN typedef\n      TOKEN int\n    DECLARATOR\n      TOKEN T\n    TOKEN ;\n  UNPARSED\n    GROUP_PAREN\n      TOKEN (\n      TOKEN U\n      TOKEN )\n    TOKEN z\n    TOKEN ;\n  EOF\n'

# 复合字面量: (type){...}; 初始化列表的元素是表达式或嵌套列表.
exp_dump compound_literal '(int[]){1, 2};\n' \
'TU\n  EXPR_STMT\n    COMPOUND_LITERAL\n      TYPE_NAME\n        TOKEN (\n        DECL_SPECS\n          TOKEN int\n        DECLARATOR\n          GROUP_BRACKET\n            TOKEN [\n            TOKEN ]\n        TOKEN )\n      INIT_LIST\n        TOKEN {\n        INITIALIZER\n          PRIMARY\n            TOKEN 1\n        TOKEN ,\n        INITIALIZER\n          PRIMARY\n            TOKEN 2\n        TOKEN }\n    TOKEN ;\n  EOF\n'

# 相邻字符串字面量是一个 primary, 但每个仍是显式叶子.
exp_dump string_concat '"a" "b";\n' \
'TU\n  EXPR_STMT\n    PRIMARY\n      TOKEN "a"\n      TOKEN "b"\n    TOKEN ;\n  EOF\n'

for f in "$tmp"/d-precedence.c "$tmp"/d-assign_chain.c \
         "$tmp"/d-paren_explicit.c "$tmp"/d-postfix_chain.c \
         "$tmp"/d-conditional.c "$tmp"/d-elvis.c "$tmp"/d-comma.c \
         "$tmp"/d-unary_ops.c "$tmp"/d-sizeof_type.c "$tmp"/d-sizeof_expr.c \
         "$tmp"/d-typedef_cast.c "$tmp"/d-typedef_struct.c \
         "$tmp"/d-unknown_type.c \
         "$tmp"/d-compound_literal.c "$tmp"/d-string_concat.c; do
    $chibicc -reconstruct-cst "$f" > "$tmp/rec3.out"
    cmp -s "$tmp/rec3.out" "$f"
    check "-reconstruct-cst/$(basename "$f" .c)"
done

# ---- C2.2: 声明与语句结构 ------------------------------------------------------

# 标量声明: decl-specs + declarator + ';'.
exp_dump decl_scalar 'int x;\n' \
'TU\n  DECL\n    DECL_SPECS\n      TOKEN int\n    DECLARATOR\n      TOKEN x\n    TOKEN ;\n  EOF\n'

# 初始化器: INIT_DECLARATOR 包住 declarator, '=' 与初值.
exp_dump decl_init 'int x = 1 + 2;\n' \
'TU\n  DECL\n    DECL_SPECS\n      TOKEN int\n    INIT_DECLARATOR\n      DECLARATOR\n        TOKEN x\n      TOKEN =\n      BINARY\n        PRIMARY\n          TOKEN 1\n        TOKEN +\n        PRIMARY\n          TOKEN 2\n    TOKEN ;\n  EOF\n'

# 多声明符与指针.
exp_dump decl_multi 'int a, *b = 0;\n' \
'TU\n  DECL\n    DECL_SPECS\n      TOKEN int\n    DECLARATOR\n      TOKEN a\n    TOKEN ,\n    INIT_DECLARATOR\n      DECLARATOR\n        TOKEN *\n        TOKEN b\n      TOKEN =\n      PRIMARY\n        TOKEN 0\n    TOKEN ;\n  EOF\n'

# 函数指针: 内层 declarator 在 GROUP_PAREN 里, 参数表是 PARAM_LIST.
exp_dump decl_funcptr 'int (*fp)(int);\n' \
'TU\n  DECL\n    DECL_SPECS\n      TOKEN int\n    DECLARATOR\n      GROUP_PAREN\n        TOKEN (\n        DECLARATOR\n          TOKEN *\n          TOKEN fp\n        TOKEN )\n      PARAM_LIST\n        TOKEN (\n        PARAM\n          DECL_SPECS\n            TOKEN int\n        TOKEN )\n    TOKEN ;\n  EOF\n'

# 数组维度与初始化列表.
exp_dump decl_array 'int a[3] = {1, 2, 3};\n' \
'TU\n  DECL\n    DECL_SPECS\n      TOKEN int\n    INIT_DECLARATOR\n      DECLARATOR\n        TOKEN a\n        GROUP_BRACKET\n          TOKEN [\n          PRIMARY\n            TOKEN 3\n          TOKEN ]\n      TOKEN =\n      INIT_LIST\n        TOKEN {\n        INITIALIZER\n          PRIMARY\n            TOKEN 1\n        TOKEN ,\n        INITIALIZER\n          PRIMARY\n            TOKEN 2\n        TOKEN ,\n        INITIALIZER\n          PRIMARY\n            TOKEN 3\n        TOKEN }\n    TOKEN ;\n  EOF\n'

# 结构体 specifier: 标签与成员体(原始组)在 DECL_SPECS 里.
exp_dump decl_struct 'struct S { int x; } s;\n' \
'TU\n  DECL\n    DECL_SPECS\n      TOKEN struct\n      TOKEN S\n      GROUP_BRACE\n        TOKEN {\n        TOKEN int\n        TOKEN x\n        TOKEN ;\n        TOKEN }\n    DECLARATOR\n      TOKEN s\n    TOKEN ;\n  EOF\n'

# 指派初始化器与嵌套花括号: DESIGNATOR 显式, 每个元素是 INITIALIZER.
exp_dump decl_nested_init 'int m[2][2] = { [0] = {1, 2}, [1] = {3, 4} };\n' \
'TU\n  DECL\n    DECL_SPECS\n      TOKEN int\n    INIT_DECLARATOR\n      DECLARATOR\n        TOKEN m\n        GROUP_BRACKET\n          TOKEN [\n          PRIMARY\n            TOKEN 2\n          TOKEN ]\n        GROUP_BRACKET\n          TOKEN [\n          PRIMARY\n            TOKEN 2\n          TOKEN ]\n      TOKEN =\n      INIT_LIST\n        TOKEN {\n        INITIALIZER\n          DESIGNATOR\n            TOKEN [\n            PRIMARY\n              TOKEN 0\n            TOKEN ]\n          TOKEN =\n          INIT_LIST\n            TOKEN {\n            INITIALIZER\n              PRIMARY\n                TOKEN 1\n            TOKEN ,\n            INITIALIZER\n              PRIMARY\n                TOKEN 2\n            TOKEN }\n        TOKEN ,\n        INITIALIZER\n          DESIGNATOR\n            TOKEN [\n            PRIMARY\n              TOKEN 1\n            TOKEN ]\n          TOKEN =\n          INIT_LIST\n            TOKEN {\n            INITIALIZER\n              PRIMARY\n                TOKEN 3\n            TOKEN ,\n            INITIALIZER\n              PRIMARY\n                TOKEN 4\n            TOKEN }\n        TOKEN }\n    TOKEN ;\n  EOF\n'

# 字段指派与匿名结构体.
exp_dump decl_field_desig 'struct { int x; int y; } s = { .y = 2, .x = 1 };\n' \
'TU\n  DECL\n    DECL_SPECS\n      TOKEN struct\n      GROUP_BRACE\n        TOKEN {\n        TOKEN int\n        TOKEN x\n        TOKEN ;\n        TOKEN int\n        TOKEN y\n        TOKEN ;\n        TOKEN }\n    INIT_DECLARATOR\n      DECLARATOR\n        TOKEN s\n      TOKEN =\n      INIT_LIST\n        TOKEN {\n        INITIALIZER\n          DESIGNATOR\n            TOKEN .\n            TOKEN y\n          TOKEN =\n          PRIMARY\n            TOKEN 2\n        TOKEN ,\n        INITIALIZER\n          DESIGNATOR\n            TOKEN .\n            TOKEN x\n          TOKEN =\n          PRIMARY\n            TOKEN 1\n        TOKEN }\n    TOKEN ;\n  EOF\n'

# 函数定义: specs + declarator(含参数表) + 函数体 BLOCK.
exp_dump func_def 'int add(int a, int b) { return a + b; }\n' \
'TU\n  FUNC_DEF\n    DECL_SPECS\n      TOKEN int\n    DECLARATOR\n      TOKEN add\n      PARAM_LIST\n        TOKEN (\n        PARAM\n          DECL_SPECS\n            TOKEN int\n          DECLARATOR\n            TOKEN a\n        TOKEN ,\n        PARAM\n          DECL_SPECS\n            TOKEN int\n          DECLARATOR\n            TOKEN b\n        TOKEN )\n    BLOCK\n      TOKEN {\n      RETURN\n        TOKEN return\n        BINARY\n          PRIMARY\n            TOKEN a\n          TOKEN +\n          PRIMARY\n            TOKEN b\n        TOKEN ;\n      TOKEN }\n  EOF\n'

# if / else.
exp_dump stmt_if 'if (x) y = 1; else y = 2;\n' \
'TU\n  IF\n    TOKEN if\n    TOKEN (\n    PRIMARY\n      TOKEN x\n    TOKEN )\n    EXPR_STMT\n      ASSIGN\n        PRIMARY\n          TOKEN y\n        TOKEN =\n        PRIMARY\n          TOKEN 1\n      TOKEN ;\n    TOKEN else\n    EXPR_STMT\n      ASSIGN\n        PRIMARY\n          TOKEN y\n        TOKEN =\n        PRIMARY\n          TOKEN 2\n      TOKEN ;\n  EOF\n'

# while 与 do-while.
exp_dump stmt_while_do 'while (x) x--; do x++; while (x);\n' \
'TU\n  WHILE\n    TOKEN while\n    TOKEN (\n    PRIMARY\n      TOKEN x\n    TOKEN )\n    EXPR_STMT\n      POSTFIX\n        PRIMARY\n          TOKEN x\n        TOKEN --\n      TOKEN ;\n  DO\n    TOKEN do\n    EXPR_STMT\n      POSTFIX\n        PRIMARY\n          TOKEN x\n        TOKEN ++\n      TOKEN ;\n    TOKEN while\n    TOKEN (\n    PRIMARY\n      TOKEN x\n    TOKEN )\n    TOKEN ;\n  EOF\n'

# for: 声明式 init 自带的 ';' 与两个分隔符都是显式叶子.
exp_dump stmt_for 'for (int i = 0; i < 3; i++) sum += i;\n' \
'TU\n  FOR\n    TOKEN for\n    TOKEN (\n    DECL\n      DECL_SPECS\n        TOKEN int\n      INIT_DECLARATOR\n        DECLARATOR\n          TOKEN i\n        TOKEN =\n        PRIMARY\n          TOKEN 0\n      TOKEN ;\n    BINARY\n      PRIMARY\n        TOKEN i\n      TOKEN <\n      PRIMARY\n        TOKEN 3\n    TOKEN ;\n    POSTFIX\n      PRIMARY\n        TOKEN i\n      TOKEN ++\n    TOKEN )\n    EXPR_STMT\n      ASSIGN\n        PRIMARY\n          TOKEN sum\n        TOKEN +=\n        PRIMARY\n          TOKEN i\n      TOKEN ;\n  EOF\n'

# switch / case / default / break / continue.
exp_dump stmt_switch 'switch (x) { case 1: break; default: continue; }\n' \
'TU\n  SWITCH\n    TOKEN switch\n    TOKEN (\n    PRIMARY\n      TOKEN x\n    TOKEN )\n    BLOCK\n      TOKEN {\n      CASE\n        TOKEN case\n        PRIMARY\n          TOKEN 1\n        TOKEN :\n        BREAK\n          TOKEN break\n          TOKEN ;\n      DEFAULT\n        TOKEN default\n        TOKEN :\n        CONTINUE\n          TOKEN continue\n          TOKEN ;\n      TOKEN }\n  EOF\n'

# 标签与 goto.
exp_dump stmt_label_goto 'start: goto start;\n' \
'TU\n  LABEL\n    TOKEN start\n    TOKEN :\n    GOTO\n      TOKEN goto\n      TOKEN start\n      TOKEN ;\n  EOF\n'

# 汇编语句: 操作数保持原始组.
exp_dump stmt_asm 'asm("nop");\n' \
'TU\n  ASM\n    TOKEN asm\n    GROUP_PAREN\n      TOKEN (\n      TOKEN "nop"\n      TOKEN )\n    TOKEN ;\n  EOF\n'

# 歧义 T *x;: 文件内 typedef 已知 -> 声明; 未知 -> 乘法表达式.
exp_dump ambig_ptr_typedef 'typedef int T;\nT *x;\n' \
'TU\n  DECL\n    DECL_SPECS\n      TOKEN typedef\n      TOKEN int\n    DECLARATOR\n      TOKEN T\n    TOKEN ;\n  DECL\n    DECL_SPECS\n      TOKEN T\n    DECLARATOR\n      TOKEN *\n      TOKEN x\n    TOKEN ;\n  EOF\n'

exp_dump ambig_ptr_unknown 'T *x;\n' \
'TU\n  EXPR_STMT\n    BINARY\n      PRIMARY\n        TOKEN T\n      TOKEN *\n      PRIMARY\n        TOKEN x\n    TOKEN ;\n  EOF\n'

# 歧义 T(x);: typedef 已知 -> 声明(括号是声明符的一部分); 未知 -> 调用.
exp_dump ambig_call_typedef 'typedef int T;\nT(x);\n' \
'TU\n  DECL\n    DECL_SPECS\n      TOKEN typedef\n      TOKEN int\n    DECLARATOR\n      TOKEN T\n    TOKEN ;\n  DECL\n    DECL_SPECS\n      TOKEN T\n    DECLARATOR\n      GROUP_PAREN\n        TOKEN (\n        DECLARATOR\n          TOKEN x\n        TOKEN )\n    TOKEN ;\n  EOF\n'

exp_dump ambig_call_unknown 'T(x);\n' \
'TU\n  EXPR_STMT\n    POSTFIX\n      PRIMARY\n        TOKEN T\n      ARG_LIST\n        TOKEN (\n        PRIMARY\n          TOKEN x\n        TOKEN )\n    TOKEN ;\n  EOF\n'

for f in "$tmp"/d-decl_scalar.c "$tmp"/d-decl_init.c "$tmp"/d-decl_multi.c \
         "$tmp"/d-decl_funcptr.c "$tmp"/d-decl_array.c "$tmp"/d-decl_struct.c \
         "$tmp"/d-decl_nested_init.c "$tmp"/d-decl_field_desig.c \
         "$tmp"/d-func_def.c "$tmp"/d-stmt_if.c "$tmp"/d-stmt_while_do.c \
         "$tmp"/d-stmt_for.c "$tmp"/d-stmt_switch.c "$tmp"/d-stmt_label_goto.c \
         "$tmp"/d-stmt_asm.c "$tmp"/d-ambig_ptr_typedef.c \
         "$tmp"/d-ambig_ptr_unknown.c "$tmp"/d-ambig_call_typedef.c \
         "$tmp"/d-ambig_call_unknown.c; do
    $chibicc -reconstruct-cst "$f" > "$tmp/rec4.out"
    cmp -s "$tmp/rec4.out" "$f"
    check "-reconstruct-cst/$(basename "$f" .c)"
done

# ---- C3.1: 预处理指令与条件分组 ------------------------------------------------

# 指令行 = 行首 '#' 到下一个行首 token 之前; 指令的 token 全是叶子, 树只按行分组.
# #include 的引号形与尖括号形原样保留路径 token(不解释转义, 不查文件).
exp_dump pp_include '#include "a.h"\n#include <b/c.h>\n' \
'TU\n  PP_INCLUDE\n    TOKEN #\n    TOKEN include\n    TOKEN "a.h"\n  PP_INCLUDE\n    TOKEN #\n    TOKEN include\n    TOKEN <\n    TOKEN b\n    TOKEN /\n    TOKEN c\n    TOKEN .\n    TOKEN h\n    TOKEN >\n  EOF\n'

# #define: 名字后紧跟 '(' 且无空白才是函数宏, 参数表是一个 GROUP_PAREN;
# 对象宏(名字后有空白)与替换列表是平铺叶子. 变参 '...' 留在参数表里.
exp_dump pp_define '#define X 1 + 2\n#define F(x, y) x + y\n#define OBJ (x)\n' \
'TU\n  PP_DEFINE\n    TOKEN #\n    TOKEN define\n    TOKEN X\n    TOKEN 1\n    TOKEN +\n    TOKEN 2\n  PP_DEFINE\n    TOKEN #\n    TOKEN define\n    TOKEN F\n    GROUP_PAREN\n      TOKEN (\n      TOKEN x\n      TOKEN ,\n      TOKEN y\n      TOKEN )\n    TOKEN x\n    TOKEN +\n    TOKEN y\n  PP_DEFINE\n    TOKEN #\n    TOKEN define\n    TOKEN OBJ\n    TOKEN (\n    TOKEN x\n    TOKEN )\n  EOF\n'

exp_dump pp_variadic '#define V(fmt, ...) f(fmt, __VA_ARGS__)\n' \
'TU\n  PP_DEFINE\n    TOKEN #\n    TOKEN define\n    TOKEN V\n    GROUP_PAREN\n      TOKEN (\n      TOKEN fmt\n      TOKEN ,\n      TOKEN ...\n      TOKEN )\n    TOKEN f\n    TOKEN (\n    TOKEN fmt\n    TOKEN ,\n    TOKEN __VA_ARGS__\n    TOKEN )\n  EOF\n'

# #undef / #line(含 GNU 行标记 `# 123 "file"`) 各自成节点, 行内 token 全保留.
exp_dump pp_undef_line '#undef X\n#line 10 "f.c"\n# 200 "xyz" 2 3\n' \
'TU\n  PP_UNDEF\n    TOKEN #\n    TOKEN undef\n    TOKEN X\n  PP_LINE\n    TOKEN #\n    TOKEN line\n    TOKEN 10\n    TOKEN "f.c"\n  PP_LINE\n    TOKEN #\n    TOKEN 200\n    TOKEN "xyz"\n    TOKEN 2\n    TOKEN 3\n  EOF\n'

# #pragma / #error / 空指令 / 未知指令: 空指令只有 '#', 未知拼写走通用指令节点.
exp_dump pp_pragma_error '#pragma once\n#error boom\n#\n#unknown stuff\n' \
'TU\n  PP_PRAGMA\n    TOKEN #\n    TOKEN pragma\n    TOKEN once\n  PP_ERROR\n    TOKEN #\n    TOKEN error\n    TOKEN boom\n  PP_NULL\n    TOKEN #\n  PP_DIRECTIVE\n    TOKEN #\n    TOKEN unknown\n    TOKEN stuff\n  EOF\n'

# #if / #ifdef / #ifndef 成 PP_IF, 每个 #if / #elif / #else 是一个 PP_BRANCH,
# 收尾的 #endif 是 PP_IF 的最后一个孩子; 条件不求值, 所有分支全部保留.
exp_dump pp_if_branches '#if A\nint x;\n#elif B\nint y;\n#else\nint z;\n#endif\n' \
'TU\n  PP_IF\n    PP_BRANCH\n      PP_DIRECTIVE\n        TOKEN #\n        TOKEN if\n        TOKEN A\n      DECL\n        DECL_SPECS\n          TOKEN int\n        DECLARATOR\n          TOKEN x\n        TOKEN ;\n    PP_BRANCH\n      PP_DIRECTIVE\n        TOKEN #\n        TOKEN elif\n        TOKEN B\n      DECL\n        DECL_SPECS\n          TOKEN int\n        DECLARATOR\n          TOKEN y\n        TOKEN ;\n    PP_BRANCH\n      PP_DIRECTIVE\n        TOKEN #\n        TOKEN else\n      DECL\n        DECL_SPECS\n          TOKEN int\n        DECLARATOR\n          TOKEN z\n        TOKEN ;\n    PP_DIRECTIVE\n      TOKEN #\n      TOKEN endif\n  EOF\n'

# 嵌套条件在分支体里递归; 内层 PP_IF 是外层分支的一个孩子.
exp_dump pp_if_nested '#if A\n#if B\nint x;\n#endif\n#else\nint y;\n#endif\n' \
'TU\n  PP_IF\n    PP_BRANCH\n      PP_DIRECTIVE\n        TOKEN #\n        TOKEN if\n        TOKEN A\n      PP_IF\n        PP_BRANCH\n          PP_DIRECTIVE\n            TOKEN #\n            TOKEN if\n            TOKEN B\n          DECL\n            DECL_SPECS\n              TOKEN int\n            DECLARATOR\n              TOKEN x\n            TOKEN ;\n        PP_DIRECTIVE\n          TOKEN #\n          TOKEN endif\n    PP_BRANCH\n      PP_DIRECTIVE\n        TOKEN #\n        TOKEN else\n      DECL\n        DECL_SPECS\n          TOKEN int\n        DECLARATOR\n          TOKEN y\n        TOKEN ;\n    PP_DIRECTIVE\n      TOKEN #\n      TOKEN endif\n  EOF\n'

# 块内的条件与块外一样成节点: 分支体按语句 / 声明建树.
exp_dump pp_if_block 'int main() {\n#if 1\n  return 0;\n#else\n  return 1;\n#endif\n}\n' \
'TU\n  FUNC_DEF\n    DECL_SPECS\n      TOKEN int\n    DECLARATOR\n      TOKEN main\n      PARAM_LIST\n        TOKEN (\n        TOKEN )\n    BLOCK\n      TOKEN {\n      PP_IF\n        PP_BRANCH\n          PP_DIRECTIVE\n            TOKEN #\n            TOKEN if\n            TOKEN 1\n          RETURN\n            TOKEN return\n            PRIMARY\n              TOKEN 0\n            TOKEN ;\n        PP_BRANCH\n          PP_DIRECTIVE\n            TOKEN #\n            TOKEN else\n          RETURN\n            TOKEN return\n            PRIMARY\n              TOKEN 1\n            TOKEN ;\n        PP_DIRECTIVE\n          TOKEN #\n          TOKEN endif\n      TOKEN }\n  EOF\n'

# 孤立的条件指令是普通指令行, 不报错也不丢弃.
exp_dump pp_orphan '#endif\n#else\n#elif X\n' \
'TU\n  PP_DIRECTIVE\n    TOKEN #\n    TOKEN endif\n  PP_DIRECTIVE\n    TOKEN #\n    TOKEN else\n  PP_DIRECTIVE\n    TOKEN #\n    TOKEN elif\n    TOKEN X\n  EOF\n'

# 未闭合的宏参数表停在行尾(UNPARSED), 未闭合的条件保留已消费的分支.
exp_dump pp_bad_params '#define F(x\nint y;\n' \
'TU\n  PP_DEFINE\n    TOKEN #\n    TOKEN define\n    TOKEN F\n    UNPARSED\n      TOKEN (\n      TOKEN x\n  DECL\n    DECL_SPECS\n      TOKEN int\n    DECLARATOR\n      TOKEN y\n    TOKEN ;\n  EOF\n'

exp_dump pp_unclosed_if '#if X\nint x;\n' \
'TU\n  PP_IF\n    PP_BRANCH\n      PP_DIRECTIVE\n        TOKEN #\n        TOKEN if\n        TOKEN X\n      DECL\n        DECL_SPECS\n          TOKEN int\n        DECLARATOR\n          TOKEN x\n        TOKEN ;\n  EOF\n'

# 续行与注释是 trivia: 指令仍是一行, dump 里看不到它们, 重建逐字节保留.
exp_dump pp_continuation '#define LONG a \\\n  b\n' \
'TU\n  PP_DEFINE\n    TOKEN #\n    TOKEN define\n    TOKEN LONG\n    TOKEN a\n    TOKEN b\n  EOF\n'

exp_dump pp_comment '#define X 1 /* c */\nint y;\n' \
'TU\n  PP_DEFINE\n    TOKEN #\n    TOKEN define\n    TOKEN X\n    TOKEN 1\n  DECL\n    DECL_SPECS\n      TOKEN int\n    DECLARATOR\n      TOKEN y\n    TOKEN ;\n  EOF\n'

# 边界样本: BOM / CRLF 在指令行前, 缺分号的语句后紧跟指令(指令是回退的边界).
exp_rec pp_crlf '#include <a.h>\r\n#define X 1\r\n'
exp_rec pp_bom '\xef\xbb\xbf#include "a.h"\n#define Y 2\n'
exp_rec pp_malformed_directive 'int x\n#define Y 1\nint z;\n'

for f in "$tmp"/d-pp_include.c "$tmp"/d-pp_define.c "$tmp"/d-pp_variadic.c \
         "$tmp"/d-pp_undef_line.c "$tmp"/d-pp_pragma_error.c \
         "$tmp"/d-pp_if_branches.c "$tmp"/d-pp_if_nested.c \
         "$tmp"/d-pp_if_block.c "$tmp"/d-pp_orphan.c \
         "$tmp"/d-pp_bad_params.c "$tmp"/d-pp_unclosed_if.c \
         "$tmp"/d-pp_continuation.c "$tmp"/d-pp_comment.c; do
    $chibicc -reconstruct-cst "$f" > "$tmp/rec5.out"
    cmp -s "$tmp/rec5.out" "$f"
    check "-reconstruct-cst/$(basename "$f" .c)"
done

echo "cst lock: $n_rt round-trip files, $n_vf verify files, $n_rb boundary samples, $n_d dump samples"
