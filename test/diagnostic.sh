#!/bin/bash
# 诊断锁定测试 (PLAN R0.1): 逐字节锁定 parse.c 全部 33 处 error_tok 与 sema 判定表 E
# 错误路径的 "stderr 文案 + 插入符位置". 每个用例 = 一个编译失败的独立源码片段 + 期望的
# stderr 原文; 运行时在临时目录里以 <用例名>.c 编译, stderr 与期望逐字节 diff.
#
# 用途: 忠实层收尾线(R2)要把大量检查从 parse 移入 sema, 移动时 "文案 + 锚点" 必须保真;
# 保真时本测试不修改即通过, 若锚点 token 需要重新指定, 须在同一提交内更新期望并在
# RESULT.md 的偏差记录中说明.
#
# 已锁定的 4.2c 诊断时机规范(拆分线偏差, 由本测试拍板为标准):
#   - "too many arguments" 锚定调用右括号 (ND_FUNCALL.tok);
#   - 块域 "void x = <初始化器>;" 锚定初始化器之后的 token, 块域 static 路径仍锚 "=".
#
# 覆盖清单: 判定表 A 15 处 + B 4 处 + C 11 处 + D 3 处 = parse.c 全部 33 处 error_tok;
# 判定表 E 10 处中的 9 处. 唯一未覆盖: sema.c 的 "redeclared as a different kind of
# symbol" - find_func 只返回 is_function 为真的对象, 该守卫恒假, 当前树中不可达.
# 另: 文件域 "void x;" 当前被接受(无检查), 亦无用例.
#
# 用法: test/diagnostic.sh ./chibicc (由 make test 与 make test-stage2 调用)

chibicc="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
tmp=`mktemp -d /tmp/chibicc-diag-XXXXXX`
trap 'rm -rf $tmp' INT TERM HUP EXIT

names=""

# snippet <name>: 后随 heredoc 为触发该诊断的源码片段.
snippet() {
    names="$names $1"
    cat > "$tmp/$1.c"
}

# expect <name>: 后随 heredoc 为期望的 stderr 原文(逐字节).
expect() {
    cat > "$tmp/$1.expected"
}

# ---- 判定表 A: parse.c 文法/词法可判 (15 处) ----

snippet a01_expected_identifier <<'SNIP'
int main() { goto; }
SNIP

expect a01_expected_identifier <<'WANT'
a01_expected_identifier.c:1: int main() { goto; }
                                              ^ expected an identifier
WANT

snippet a02_storage_class_not_allowed <<'SNIP'
int x = (static int)3;
SNIP

expect a02_storage_class_not_allowed <<'WANT'
a02_storage_class_not_allowed.c:1: int x = (static int)3;
                                            ^ storage class specifier is not allowed in this context
WANT

snippet a03_typedef_with_static <<'SNIP'
typedef static extern int x;
SNIP

expect a03_typedef_with_static <<'WANT'
a03_typedef_with_static.c:1: typedef static extern int x;
                                            ^ typedef may not be used together with static, extern, inline, __thread or _Thread_local
WANT

snippet a04_alignas_not_allowed <<'SNIP'
int x = (_Alignas(8) int)3;
SNIP

expect a04_alignas_not_allowed <<'WANT'
a04_alignas_not_allowed.c:1: int x = (_Alignas(8) int)3;
                                      ^ _Alignas is not allowed in this context
WANT

snippet a05_invalid_type <<'SNIP'
char float x;
SNIP

expect a05_invalid_type <<'WANT'
a05_invalid_type.c:1: char float x;
                           ^ invalid type
WANT

snippet a06_unknown_enum_type <<'SNIP'
enum Undeclared x;
SNIP

expect a06_unknown_enum_type <<'WANT'
a06_unknown_enum_type.c:1: enum Undeclared x;
                                ^ unknown enum type
WANT

snippet a07_not_an_enum_tag <<'SNIP'
struct S { int a; };
enum S x;
SNIP

expect a07_not_an_enum_tag <<'WANT'
a07_not_an_enum_tag.c:2: enum S x;
                              ^ not an enum tag
WANT

snippet a08_variable_name_omitted_block <<'SNIP'
int main() { int 5; }
SNIP

expect a08_variable_name_omitted_block <<'WANT'
a08_variable_name_omitted_block.c:1: int main() { int 5; }
                                                      ^ variable name omitted
WANT

snippet a09_expected_field_designator <<'SNIP'
struct S { int a; };
struct S s = {.+ = 1};
SNIP

expect a09_expected_field_designator <<'WANT'
a09_expected_field_designator.c:2: struct S s = {.+ = 1};
                                                  ^ expected a field designator
WANT

snippet a10_expected_string_literal <<'SNIP'
int main() { asm(1); }
SNIP

expect a10_expected_string_literal <<'WANT'
a10_expected_string_literal.c:1: int main() { asm(1); }
                                                  ^ expected string literal
WANT

snippet a11_unknown_attribute <<'SNIP'
struct __attribute__((nonsense)) S { int a; };
SNIP

expect a11_unknown_attribute <<'WANT'
a11_unknown_attribute.c:1: struct __attribute__((nonsense)) S { int a; };
                                                 ^ unknown attribute
WANT

snippet a12_expected_an_expression <<'SNIP'
int main() { int x = ; }
SNIP

expect a12_expected_an_expression <<'WANT'
a12_expected_an_expression.c:1: int main() { int x = ; }
                                                     ^ expected an expression
WANT

snippet a13_typedef_name_omitted <<'SNIP'
typedef int 5;
SNIP

expect a13_typedef_name_omitted <<'WANT'
a13_typedef_name_omitted.c:1: typedef int 5;
                                          ^ typedef name omitted
WANT

snippet a14_function_name_omitted <<'SNIP'
int ()() { return 0; }
SNIP

expect a14_function_name_omitted <<'WANT'
a14_function_name_omitted.c:1: int ()() { return 0; }
                                    ^ function name omitted
WANT

snippet a15_variable_name_omitted_global <<'SNIP'
int 5;
SNIP

expect a15_variable_name_omitted_global <<'WANT'
a15_variable_name_omitted_global.c:1: int 5;
                                          ^ variable name omitted
WANT

# ---- 判定表 B: 解析器上下文状态 (4 处) ----

snippet b01_stray_case <<'SNIP'
int main() { case 1: return 0; }
SNIP

expect b01_stray_case <<'WANT'
b01_stray_case.c:1: int main() { case 1: return 0; }
                                 ^ stray case
WANT

snippet b02_stray_default <<'SNIP'
int main() { default: return 0; }
SNIP

expect b02_stray_default <<'WANT'
b02_stray_default.c:1: int main() { default: return 0; }
                                    ^ stray default
WANT

snippet b03_stray_break <<'SNIP'
int main() { break; }
SNIP

expect b03_stray_break <<'WANT'
b03_stray_break.c:1: int main() { break; }
                                  ^ stray break
WANT

snippet b04_stray_continue <<'SNIP'
int main() { continue; }
SNIP

expect b04_stray_continue <<'WANT'
b04_stray_continue.c:1: int main() { continue; }
                                     ^ stray continue
WANT

# ---- 判定表 C: 建树必需 (11 处) ----

snippet c01_designator_exceeds_begin <<'SNIP'
int x[2] = {[5] = 1};
SNIP

expect c01_designator_exceeds_begin <<'WANT'
c01_designator_exceeds_begin.c:1: int x[2] = {[5] = 1};
                                                ^ array designator index exceeds array bounds
WANT

snippet c02_designator_exceeds_end <<'SNIP'
int x[2] = {[1 ... 5] = 1};
SNIP

expect c02_designator_exceeds_end <<'WANT'
c02_designator_exceeds_end.c:1: int x[2] = {[1 ... 5] = 1};
                                                    ^ array designator index exceeds array bounds
WANT

snippet c03_designator_range_empty <<'SNIP'
int x[4] = {[2 ... 1] = 1};
SNIP

expect c03_designator_range_empty <<'WANT'
c03_designator_range_empty.c:1: int x[4] = {[2 ... 1] = 1};
                                                    ^ array designator range [2, 1] is empty
WANT

snippet c04_struct_no_such_member_init <<'SNIP'
struct S { int a; };
struct S s = {.b = 1};
SNIP

expect c04_struct_no_such_member_init <<'WANT'
c04_struct_no_such_member_init.c:2: struct S s = {.b = 1};
                                                   ^ struct has no such member
WANT

snippet c05_array_index_non_array <<'SNIP'
struct S { int a; };
struct S s = {.a[0] = 1};
SNIP

expect c05_array_index_non_array <<'WANT'
c05_array_index_non_array.c:2: struct S s = {.a[0] = 1};
                                               ^ array index in non-array initializer
WANT

snippet c06_field_name_not_in_struct <<'SNIP'
int x[2] = {[0].a = 1};
SNIP

expect c06_field_name_not_in_struct <<'WANT'
c06_field_name_not_in_struct.c:1: int x[2] = {[0].a = 1};
                                                 ^ field name not in struct or union initializer
WANT

snippet c07_invalid_ptr_deref <<'SNIP'
struct S { int a; };
void f(struct S s) { s->a; }
SNIP

expect c07_invalid_ptr_deref <<'WANT'
c07_invalid_ptr_deref.c:2: void f(struct S s) { s->a; }
                                                 ^ invalid pointer dereference
WANT

snippet c08_void_ptr_deref <<'SNIP'
void f(void *p) { p->a; }
SNIP

expect c08_void_ptr_deref <<'WANT'
c08_void_ptr_deref.c:1: void f(void *p) { p->a; }
                                           ^ dereferencing a void pointer
WANT

snippet c09_not_struct_arrow <<'SNIP'
void f(int *p) { p->a; }
SNIP

expect c09_not_struct_arrow <<'WANT'
c09_not_struct_arrow.c:1: void f(int *p) { p->a; }
                                            ^ not a struct nor a union
WANT

snippet c10_not_struct_dot <<'SNIP'
void f(int x) { x.a; }
SNIP

expect c10_not_struct_dot <<'WANT'
c10_not_struct_dot.c:1: void f(int x) { x.a; }
                                        ^ not a struct nor a union
WANT

snippet c11_no_such_member <<'SNIP'
struct S { int a; };
void f(struct S *s) { s->b; }
SNIP

expect c11_no_such_member <<'WANT'
c11_no_such_member.c:2: void f(struct S *s) { s->b; }
                                                 ^ no such member
WANT

# ---- 判定表 D: 常量求值/语法选择顺带 (3 处) ----

snippet d01_vla_initialized <<'SNIP'
void f(int n) { int x[n] = {1}; }
SNIP

expect d01_vla_initialized <<'WANT'
d01_vla_initialized.c:1: void f(int n) { int x[n] = {1}; }
                                                  ^ variable-sized object may not be initialized
WANT

snippet d02_empty_case_range <<'SNIP'
int main() { int x; switch (x) { case 2 ... 1: return 0; } }
SNIP

expect d02_empty_case_range <<'WANT'
d02_empty_case_range.c:1: int main() { int x; switch (x) { case 2 ... 1: return 0; } }
                                                                       ^ empty case range specified
WANT

snippet d03_generic_not_compatible <<'SNIP'
int main() { int x = _Generic(1, double: 0); return x; }
SNIP

expect d03_generic_not_compatible <<'WANT'
d03_generic_not_compatible.c:1: int main() { int x = _Generic(1, double: 0); return x; }
                                                             ^ controlling expression type not compatible with any generic association type
WANT

# ---- 判定表 E: sema (9 处; redeclared-kind 不可达, 见文件头) ----

snippet e01_not_a_function <<'SNIP'
int main() { int x; x(); }
SNIP

expect e01_not_a_function <<'WANT'
e01_not_a_function.c:1: int main() { int x; x(); }
                                            ^ not a function
WANT

snippet e02_too_many_args <<'SNIP'
int f(int a);
int main() { f(1, 2); }
SNIP

expect e02_too_many_args <<'WANT'
e02_too_many_args.c:2: int main() { f(1, 2); }
                                          ^ too many arguments
WANT

snippet e03_too_few_args <<'SNIP'
int f(int a);
int main() { f(); }
SNIP

expect e03_too_few_args <<'WANT'
e03_too_few_args.c:2: int main() { f(); }
                                     ^ too few arguments
WANT

snippet e04_bitfield_addr <<'SNIP'
struct S { int a : 4; };
int main() { struct S s; int *p = &s.a; }
SNIP

expect e04_bitfield_addr <<'WANT'
e04_bitfield_addr.c:2: int main() { struct S s; int *p = &s.a; }
                                                         ^ cannot take address of bitfield
WANT

snippet e05_void_decl_block <<'SNIP'
int main() { void x; }
SNIP

expect e05_void_decl_block <<'WANT'
e05_void_decl_block.c:1: int main() { void x; }
                                            ^ variable declared void
WANT

snippet e06_void_decl_block_init <<'SNIP'
int main() { void x = 1; }
SNIP

expect e06_void_decl_block_init <<'WANT'
e06_void_decl_block_init.c:1: int main() { void x = 1; }
                                                     ^ variable declared void
WANT

snippet e07_void_decl_static <<'SNIP'
int main() { static void x = 1; }
SNIP

expect e07_void_decl_static <<'WANT'
e07_void_decl_static.c:1: int main() { static void x = 1; }
                                                     ^ variable declared void
WANT

snippet e08_incomplete_type <<'SNIP'
struct S;
int main() { struct S x; }
SNIP

expect e08_incomplete_type <<'WANT'
e08_incomplete_type.c:2: int main() { struct S x; }
                                               ^ variable has incomplete type
WANT

snippet e09_redefinition <<'SNIP'
void f() {}
void f() {}
SNIP

expect e09_redefinition <<'WANT'
e09_redefinition.c:2: void f() {}
                               ^ redefinition of f
WANT

snippet e10_static_follows_nonstatic <<'SNIP'
int x();
static int x();
SNIP

expect e10_static_follows_nonstatic <<'WANT'
e10_static_follows_nonstatic.c:2: static int x();
                                                ^ static declaration follows a non-static declaration
WANT

fail=""
count=0
for n in $names; do
    count=$((count + 1))
    if (cd "$tmp" && "$chibicc" -c "$n.c") 2> "$tmp/$n.actual"; then
        echo "diag $n ... failed (expected an error, got success)"
        fail="y"
        continue
    fi
    if diff -u "$tmp/$n.expected" "$tmp/$n.actual" > "$tmp/$n.diff" 2>&1; then
        echo "diag $n ... passed"
    else
        echo "diag $n ... failed (stderr drift)"
        cat "$tmp/$n.diff"
        fail="y"
    fi
done

if [ -n "$fail" ]; then
    echo "diagnostic lock: FAILED"
    exit 1
fi
echo "diagnostic lock: $count cases byte-exact"
