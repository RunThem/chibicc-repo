#!/bin/sh
# 驱动器 seam 闸门 (issue #29, 决议 #15): main.c, codegen.c, driver.h 与
# driver-util.h 只允许包含公共头. 脚本做两件事:
#   1. 源码检查: 拒绝内部伞头与任何 libcp/src 路径.
#   2. 用只含 libcp 公共头的临时目录作 include path 重新编译驱动器对象.
# 第一个参数是编译器: make test 传宿主 cc, make test-stage2 传
# ./stage2/chibicc, 因此自举构建的驱动器也受同一约束. 第二个参数是
# 额外的 include 选项: 自举编译器需要 -Iinclude 找它自己的 stddef.h 等
# 引导头, 该目录不含任何 libcp 头.
#
# 用法: test/driver-seam.sh [cc] [extra-include-flags]

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cc=${1:-${CC:-cc}}
extra=${2:-}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# 源码检查: 驱动器不得包含内部伞头, 也不得出现 libcp/src 路径.
for src in "$root/main.c" "$root/codegen.c" "$root/driver.h" "$root/driver-util.h"; do
  if grep -n -e 'libcp/src' -e '#include "libcp.h"' "$src"; then
    echo "driver-seam: $(basename "$src") reaches into libcp/src" >&2
    exit 1
  fi
done

# 只复制公共头: 内部头不在搜索路径上, 任何意外依赖都会编译失败.
mkdir -p "$tmp/pubinc"
cp -R "$root/libcp/include/libcp" "$tmp/pubinc/"

flags="-std=c11 -Wall -Wno-switch -DLIBCP_DEFAULT_ALLOCATOR -I$tmp/pubinc $extra"

# 负向自检: 多包一层含内部伞头的 driver.h 后, 同一组 flags 必须编译
# 失败; 否则说明 public-only 路径其实还能看到内部头, 正向检查就是空的.
mkdir -p "$tmp/negative"
cp "$root/main.c" "$root/codegen.c" "$root/driver-util.h" "$tmp/negative/"
{
  echo '#include "libcp.h"'
  cat "$root/driver.h"
} > "$tmp/negative/driver.h"
if "$cc" $flags -c "$tmp/negative/main.c" -o "$tmp/negative/main.o" 2>/dev/null; then
  echo "driver-seam: public-only flags accepted an internal include" >&2
  exit 1
fi

# 正向检查: 两个驱动器对象都只用公共 include 就能编译.
for src in main codegen; do
  "$cc" $flags -c "$root/$src.c" -o "$tmp/$src.o"
done

echo "driver-seam: ok"
