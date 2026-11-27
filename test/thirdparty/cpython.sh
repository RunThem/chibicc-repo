#!/bin/bash
# 已知残留 (x86-64 Linux, 并以同一 commit 的 gcc 构建对照):
# - test_ssl: OpenSSL 3.6.5 二次调用密码回调, 带断言的构建命中
#   ceval.c:1395 断言; 同 commit 的 gcc 构建保留断言时同样复现, 与 chibicc
#   无关. 其余 SSL 用例也与新版 OpenSSL 不兼容, 整模块跳过. 详见
#   docs/10-build-install.md 10.10 与 issue #43.
# - test_c_locale_coercion / test_imaplib / test_socket / test_urllib /
#   test_urllib2 / test_urllib2_localnet / test_urllib2net: 同一 commit 的
#   gcc 构建同样失败或表现不稳定 (locale, OpenSSL, 内核/网络), 与 chibicc
#   无关, 整模块跳过.
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
# 以下模块的失败与 chibicc 无关(见上), 整模块跳过, 其余套件照常运行.
$make test TESTOPTS="-x test_c_locale_coercion test_imaplib test_socket test_ssl test_urllib test_urllib2 test_urllib2_localnet test_urllib2net"
