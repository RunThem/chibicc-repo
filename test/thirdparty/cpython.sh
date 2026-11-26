#!/bin/bash
# 已知残留 (x86-64 Linux 上跑完整套件, 并以同一 commit 的 gcc 构建对照):
# - test_ssl.test_load_cert_chain: OpenSSL 3.6.5 二次调用密码回调, 第一次
#   回调设置的 ValueError 未清除就再次进入求值器; 带断言的构建命中
#   ceval.c:1395 `!_PyErr_Occurred(tstate)` 并 SIGABRT. 同一 commit 的 gcc
#   构建保留断言时同样复现, 与 chibicc 无关; 测试时用 -i 跳过该用例.
# - ssl / socket / urllib* / imaplib / c_locale_coercion: gcc 构建同样失败,
#   属环境或新版 OpenSSL / glibc 兼容问题.
repo='git@github.com:python/cpython.git'
. test/thirdparty/common
git reset --hard c75330605d4795850ec74fdc4d69aa5d92f76c00

# chibicc 不支持 -fvisibility=hidden, 静态解释器以 -export-dynamic 链接后
# 仍导出内部 _PyPegen_* / PyTokenizer_*; peg 扩展自带的同名定义会被解释器
# 抢占. 给扩展链接加 -Bsymbolic, 让扩展内部引用绑定到自身 (等价于隐藏可见性),
# 同时保留 PyInit_parse 的导出. 决策与被否决的 -fvisibility=hidden 路径见
# docs/adr/0002-cpython-peg-bsymbolic.md.
sed -i 's/^\(    extra_link_args = get_extra_flags("LDFLAGS", "PY_LDFLAGS_NODIST")\)$/\1\n    extra_link_args.append("-Wl,-Bsymbolic")/' \
  Tools/peg_generator/pegen/build.py
grep -qF 'extra_link_args.append("-Wl,-Bsymbolic")' Tools/peg_generator/pegen/build.py

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
