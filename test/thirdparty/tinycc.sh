#!/bin/bash
repo='git@github.com:TinyCC/tinycc.git'
. test/thirdparty/common
git reset --hard df67d8617b7d1d03a480a28f9f901848ffbfb7ec

./configure --cc=$chibicc
$make clean
$make
# tcctest.c 是 K&R 风格, 还带冗余花括号的标量初始化; GCC 14+ 默认把这两类
# 诊断当错误, test.ref 阶段用宿主 cc 生成参考输出时直接中止. 参考输出与
# chibicc 无关, 用 -fpermissive 降级成警告, 再由测试自带的 -w 吞掉.
$make CC="cc -fpermissive" test
