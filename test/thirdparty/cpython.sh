#!/bin/bash
# 已知残留 (x86-64 Linux 上跑完整套件, 并以同一 commit 的 gcc 构建对照):
# - test_peg_generator: chibicc 不支持 -fvisibility=hidden, 解释器导出内部
#   _PyPegen_* / PyTokenizer_* 符号, 抢占扩展自带的自定义解析器.
# - test_distutils: search_cpp 依赖 -E 输出 # 1 "file" 行标记, chibicc 不输出.
# - ssl / socket / urllib* / imaplib / c_locale_coercion: gcc 构建同样失败,
#   属环境或新版 OpenSSL / glibc 兼容问题.
repo='git@github.com:python/cpython.git'
. test/thirdparty/common
git reset --hard c75330605d4795850ec74fdc4d69aa5d92f76c00

# Python's './configure' command misidentifies chibicc as icc
# (Intel C Compiler) because icc is a substring of chibicc.
# Modify the configure file as a workaround.
sed -i -e 1996,2011d configure.ac
autoreconf

CC=$chibicc ./configure
$make clean
$make
$make test
