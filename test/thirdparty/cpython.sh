#!/bin/bash
# 已知残留 (x86-64 Linux 上跳过 test_load_cert_chain 后跑其余套件,
# 并以同一 commit 的 gcc 构建对照):
# - test_ssl.test_load_cert_chain: OpenSSL 3.6.5 的环境性崩溃, 见
#   docs/10-build-install.md 10.10 与 issue #43; 测试时用 -i 跳过该用例.
# - ssl / socket / urllib* / imaplib / c_locale_coercion: gcc 构建同样失败,
#   属环境或新版 OpenSSL / glibc 兼容问题.
repo='git@github.com:python/cpython.git'
. test/thirdparty/common
git reset --hard c75330605d4795850ec74fdc4d69aa5d92f76c00

# 让 peg 扩展的内部引用绑定到自身定义, 避免被解释器导出的同名符号抢占;
# 为什么不用 -fvisibility=hidden 见 docs/adr/0002-cpython-peg-bsymbolic.md
# (issue #43).
peg_build=Tools/peg_generator/pegen/build.py
peg_anchor='    extra_link_args = get_extra_flags("LDFLAGS", "PY_LDFLAGS_NODIST")'
grep -qF "$peg_anchor" "$peg_build" ||
  { echo "cpython.sh: peg link anchor not found in $peg_build" >&2; exit 1; }
sed -i "s|^$peg_anchor\$|&\n    extra_link_args.append(\"-Wl,-Bsymbolic\")|" "$peg_build"
[ "$(grep -cF 'extra_link_args.append("-Wl,-Bsymbolic")' "$peg_build")" = 1 ] ||
  { echo "cpython.sh: -Bsymbolic patch not applied exactly once to $peg_build" >&2; exit 1; }

# Python's './configure' command misidentifies chibicc as icc
# (Intel C Compiler) because icc is a substring of chibicc.
# Modify the configure file as a workaround.
sed -i -e 1996,2011d configure.ac
autoreconf

CC=$chibicc ./configure
$make clean
$make
# test_load_cert_chain 的崩溃与 chibicc 无关(见上), 跳过该用例, 其余 test_ssl 用例照常运行.
$make test TESTOPTS="-i test_load_cert_chain"
