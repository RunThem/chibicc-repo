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

# ---- M1.3: 实参层 -----------------------------------------------------------------
# 普通实参的位置是调用点(实参 token 的拼写位置), 层是它在该次展开里的序号.

# 单个实参: 层为 arg1.
exp_dump arg '#define F(x) x\nint y = F(7);\n' \
'T arg.c:2 keyword int -
T arg.c:2 ident y -
T arg.c:2 punct = -
T arg.c:2 num 7 F@arg.c:2:arg1:arg.c:1
T arg.c:2 punct ; -
'

# 参数重复出现: 每次替换都带同一个实参层; 宏体里的 `+` 是 body 层.
exp_dump arg_repeat '#define F(x) x + x\nint y = F(7);\n' \
'T arg_repeat.c:2 keyword int -
T arg_repeat.c:2 ident y -
T arg_repeat.c:2 punct = -
T arg_repeat.c:2 num 7 F@arg_repeat.c:2:arg1:arg_repeat.c:1
T arg_repeat.c:1 punct + F@arg_repeat.c:2:body:arg_repeat.c:1
T arg_repeat.c:2 num 7 F@arg_repeat.c:2:arg1:arg_repeat.c:1
T arg_repeat.c:2 punct ; -
'

# 嵌套宏在实参内: 实参层在最前, 内层展开的链作为尾.
exp_dump arg_nested '#define G 2\n#define F(x) x\nint y = F(G);\n' \
'T arg_nested.c:3 keyword int -
T arg_nested.c:3 ident y -
T arg_nested.c:3 punct = -
T arg_nested.c:1 num 2 F@arg_nested.c:3:arg1:arg_nested.c:2>G@arg_nested.c:3:body:arg_nested.c:1
T arg_nested.c:3 punct ; -
'

# PLAN.md 的规范例子: 外层宏体里的调用, 实参也来自外层宏体. `1` 先由 A 的宏体
# 产生, 再作为 F 的实参被消费, 所以链是 [F arg1, A body].
exp_dump nested_call '#define A F(1)\n#define F(x) x\nA\n' \
'T nested_call.c:1 num 1 F@nested_call.c:1:arg1:nested_call.c:2>A@nested_call.c:3:body:nested_call.c:1
'

# `#` 的产物: 合成字符串 token(@synth), 层归 body.
exp_dump stringize '#define S(x) #x\nchar *s = S(abc);\n' \
'T stringize.c:2 keyword char -
T stringize.c:2 punct * -
T stringize.c:2 ident s -
T stringize.c:2 punct = -
T stringize.c:@synth str "abc" S@stringize.c:2:body:stringize.c:1
T stringize.c:2 punct ; -
'

# `#` 拼接多个实参 token: 词间空格进入字符串, dump 的转义规则同 trivia.sh.
exp_dump stringize_ws '#define S(x) #x\nchar *s = S(a b);\n' \
'T stringize_ws.c:2 keyword char -
T stringize_ws.c:2 punct * -
T stringize_ws.c:2 ident s -
T stringize_ws.c:2 punct = -
T stringize_ws.c:@synth str "a\\x20b" S@stringize_ws.c:2:body:stringize_ws.c:1
T stringize_ws.c:2 punct ; -
'

# `##` 两侧都是实参: 产物是合成 token, 层归 body(S3c-i).
exp_dump paste '#define C(a,b) a##b\nint x = C(foo,bar);\n' \
'T paste.c:2 keyword int -
T paste.c:2 ident x -
T paste.c:2 punct = -
T paste.c:@synth ident foobar C@paste.c:2:body:paste.c:1
T paste.c:2 punct ; -
'

# `##` 左侧实参为空: 右侧实参的 token 原样输出, 层是 arg2(S4a-i).
exp_dump paste_left_empty '#define C(a,b) a##b\nint x = C(,bar);\n' \
'T paste_left_empty.c:2 keyword int -
T paste_left_empty.c:2 ident x -
T paste_left_empty.c:2 punct = -
T paste_left_empty.c:2 ident bar C@paste_left_empty.c:2:arg2:paste_left_empty.c:1
T paste_left_empty.c:2 punct ; -
'

# `##` 右侧是宏体 token: paste 产物归 body(S4b + S3d).
exp_dump paste_body_rhs '#define C(a) a##x\nint y = C(ba);\n' \
'T paste_body_rhs.c:2 keyword int -
T paste_body_rhs.c:2 ident y -
T paste_body_rhs.c:2 punct = -
T paste_body_rhs.c:@synth ident bax C@paste_body_rhs.c:2:body:paste_body_rhs.c:1
T paste_body_rhs.c:2 punct ; -
'

# `,##__VA_ARGS__` 非空: 逗号归 body, 变参归 arg1(S2b + 普通参数).
exp_dump va_comma '#define V(...) f(0 ,##__VA_ARGS__)\nint x = V(1);\n' \
'T va_comma.c:2 keyword int -
T va_comma.c:2 ident x -
T va_comma.c:2 punct = -
T va_comma.c:1 ident f V@va_comma.c:2:body:va_comma.c:1
T va_comma.c:1 punct ( V@va_comma.c:2:body:va_comma.c:1
T va_comma.c:1 num 0 V@va_comma.c:2:body:va_comma.c:1
T va_comma.c:1 punct , V@va_comma.c:2:body:va_comma.c:1
T va_comma.c:2 num 1 V@va_comma.c:2:arg1:va_comma.c:1
T va_comma.c:1 punct ) V@va_comma.c:2:body:va_comma.c:1
T va_comma.c:2 punct ; -
'

# `,##__VA_ARGS__` 空变参: 逗号与 __VA_ARGS__ 都不产出 token(S2a).
exp_dump va_comma_empty '#define V(...) f(0 ,##__VA_ARGS__)\nint x = V();\n' \
'T va_comma_empty.c:2 keyword int -
T va_comma_empty.c:2 ident x -
T va_comma_empty.c:2 punct = -
T va_comma_empty.c:1 ident f V@va_comma_empty.c:2:body:va_comma_empty.c:1
T va_comma_empty.c:1 punct ( V@va_comma_empty.c:2:body:va_comma_empty.c:1
T va_comma_empty.c:1 num 0 V@va_comma_empty.c:2:body:va_comma_empty.c:1
T va_comma_empty.c:1 punct ) V@va_comma_empty.c:2:body:va_comma_empty.c:1
T va_comma_empty.c:2 punct ; -
'

# __VA_OPT__ 有变参: 括号内的 token 来自宏体, 归 body(S5a).
exp_dump va_opt '#define V(...) f(__VA_OPT__(1))\nint x = V(2);\n' \
'T va_opt.c:2 keyword int -
T va_opt.c:2 ident x -
T va_opt.c:2 punct = -
T va_opt.c:1 ident f V@va_opt.c:2:body:va_opt.c:1
T va_opt.c:1 punct ( V@va_opt.c:2:body:va_opt.c:1
T va_opt.c:1 num 1 V@va_opt.c:2:body:va_opt.c:1
T va_opt.c:1 punct ) V@va_opt.c:2:body:va_opt.c:1
T va_opt.c:2 punct ; -
'

# 变参整体是一个实参(标准模型): a 与 b 同属 arg1.
exp_dump variadic '#define V(...) __VA_ARGS__\nint x = V(a, b);\n' \
'T variadic.c:2 keyword int -
T variadic.c:2 ident x -
T variadic.c:2 punct = -
T variadic.c:2 ident a V@variadic.c:2:arg1:variadic.c:1
T variadic.c:2 punct , V@variadic.c:2:arg1:variadic.c:1
T variadic.c:2 ident b V@variadic.c:2:arg1:variadic.c:1
T variadic.c:2 punct ; -
'

exp_verify arg '#define F(x) x\nint y = F(7);\n'
exp_verify arg_nested '#define G 2\n#define F(x) x\nint y = F(G);\n'
exp_verify nested_call '#define A F(1)\n#define F(x) x\nA\n'
exp_verify stringize '#define S(x) #x\nchar *s = S(abc);\n'
exp_verify paste '#define C(a,b) a##b\nint x = C(foo,bar);\n'
exp_verify va_comma '#define V(...) f(0 ,##__VA_ARGS__)\nint x = V(1);\n'
exp_verify variadic '#define V(...) __VA_ARGS__\nint x = V(a, b);\n'

# ---- M2.1: 内建宏 / 合成 token / 相邻字符串 -------------------------------------
# 内建宏的产物没有源位置(合成缓冲, @synth), 也没有 #define 名 token(定义打 ?:?).

# __LINE__: 值是使用处的逻辑行, 产物是合成数字 token.
exp_dump builtin_line 'int x = __LINE__;\n' \
'T builtin_line.c:1 keyword int -
T builtin_line.c:1 ident x -
T builtin_line.c:1 punct = -
T builtin_line.c:@synth num 1 __LINE__@builtin_line.c:1:body:?:?
T builtin_line.c:1 punct ; -
'

# __FILE__: 值是文件名, 产物是合成字符串 token.
exp_dump builtin_file 'char *f = __FILE__;\n' \
'T builtin_file.c:1 keyword char -
T builtin_file.c:1 punct * -
T builtin_file.c:1 ident f -
T builtin_file.c:1 punct = -
T builtin_file.c:@synth str "builtin_file.c" __FILE__@builtin_file.c:1:body:?:?
T builtin_file.c:1 punct ; -
'

# __COUNTER__: 每次展开一个新值; 两次使用是两条独立记录.
exp_dump builtin_counter 'int a = __COUNTER__;\nint b = __COUNTER__;\n' \
'T builtin_counter.c:1 keyword int -
T builtin_counter.c:1 ident a -
T builtin_counter.c:1 punct = -
T builtin_counter.c:@synth num 0 __COUNTER__@builtin_counter.c:1:body:?:?
T builtin_counter.c:1 punct ; -
T builtin_counter.c:2 keyword int -
T builtin_counter.c:2 ident b -
T builtin_counter.c:2 punct = -
T builtin_counter.c:@synth num 1 __COUNTER__@builtin_counter.c:2:body:?:?
T builtin_counter.c:2 punct ; -
'

# __BASE_FILE__: 值是输入文件名.
exp_dump builtin_base_file 'char *b = __BASE_FILE__;\n' \
'T builtin_base_file.c:1 keyword char -
T builtin_base_file.c:1 punct * -
T builtin_base_file.c:1 ident b -
T builtin_base_file.c:1 punct = -
T builtin_base_file.c:@synth str "builtin_base_file.c" __BASE_FILE__@builtin_base_file.c:1:body:?:?
T builtin_base_file.c:1 punct ; -
'

# 内建宏嵌套在外层展开里: 外层链作为尾; __LINE__ 的 origin 走到最外层调用点
# (值是外层调用的行), 而链上 __LINE__ 的调用点是它的拼写位置(宏体定义行).
exp_dump builtin_nested '#define L __LINE__\nint x = L;\n' \
'T builtin_nested.c:2 keyword int -
T builtin_nested.c:2 ident x -
T builtin_nested.c:2 punct = -
T builtin_nested.c:@synth num 2 __LINE__@builtin_nested.c:1:body:?:?>L@builtin_nested.c:2:body:builtin_nested.c:1
T builtin_nested.c:2 punct ; -
'

# 相邻字符串合并: 合并结果保留第一个 literal 的 provenance, 第二个的链随 token
# 一起消失(已知限制).
exp_dump adjacent_strings '#define A "a"\n#define B "b"\nchar *s = A B;\n' \
'T adjacent_strings.c:3 keyword char -
T adjacent_strings.c:3 punct * -
T adjacent_strings.c:3 ident s -
T adjacent_strings.c:3 punct = -
T adjacent_strings.c:1 str "a" A@adjacent_strings.c:3:body:adjacent_strings.c:1
T adjacent_strings.c:3 punct ; -
'

# 取直不稳定的内建宏(__DATE__ / __TIMESTAMP__)只锁记录形状, 不断言文本.
n_c=0
exp_chain() {
    name=$1; shift
    n_c=$((n_c + 1))
    printf "$1" > "$tmp/$name.c"
    (cd "$tmp" && "$chibicc" -dump-expansions "$name.c") | grep -Eq "$2"
    check "-chain/$name"
}

exp_chain date_shape 'char *d = __DATE__;\n' \
'^T <built-in>:@synth str .* __DATE__@date_shape\.c:1:body:\?:\?$'

exp_chain timestamp_shape 'char *t = __TIMESTAMP__;\n' \
'^T timestamp_shape\.c:@synth str .* __TIMESTAMP__@timestamp_shape\.c:1:body:\?:\?$'

# 宽字符串合并前, 普通 literal 会被就地重扫成宽字面量; 重扫不得丢掉 provenance.
# (重扫出的 token 没设 line_no, 位置沿用既有的 0, 这里只锁链.)
exp_chain wide_concat '#define A "a"\nchar *s = A L"b";\n' \
' A@wide_concat\.c:2:body:wide_concat\.c:1$'

exp_verify builtin_line 'int x = __LINE__;\n'
exp_verify builtin_nested '#define L __LINE__\nint x = L;\n'
exp_verify adjacent_strings '#define A "a"\n#define B "b"\nchar *s = A B;\n'
exp_verify wide_concat '#define A "a"\nchar *s = A L"b";\n'

# ---- M2.2: 全量语料校验 --------------------------------------------------------
# 每个语料文件都必须让验证器成功且无输出. 本机(macOS)可能缺 Linux 系统头,
# 用探测决定是否跳过; docker 内(权威)必定执行. 语料清单与 trivia / CST 线一致.
# -Iinclude 是必需的: stage2 的 argv[0] 在 stage2/ 下, 自动 include 路径指向
# stage2/include(不存在), 而 <stdio.h> 链上的 <stddef.h> 由 chibicc 自带的
# include/ 提供; 主测试集本来就用 -Iinclude -Itest 编译.

n_f=0
if printf '#include <stdio.h>\n' > "$tmp/probe.c" && \
   "$chibicc" -Iinclude -Itest -E "$tmp/probe.c" > /dev/null 2>&1; then
    for f in test/*.c include/*.h; do
        $chibicc -Iinclude -Itest -xc -verify-expansions "$f" > "$tmp/full.out" 2>&1
        n_f=$((n_f + 1))
        check "-corpus/$f"
    done
    echo "expand corpus: $n_f files verified"
else
    echo "expand corpus: skipped (no Linux system headers)"
fi

echo "expand lock: $n_d dump samples, $n_c chain samples, $n_v verify samples, $n_f corpus files"
