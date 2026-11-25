#!/bin/sh
# 树外消费者验收 (issue #27, 决议 #14): 一个只包含公共头与归档的消费者
# 完成一次真实降级与 x86-64 发射, 并走通诊断回调. 脚本把公共头复制到
# 临时目录, 只以该目录作 include path; 消费者用 -std=c11 -Wall -Wextra
# 编译并链接归档. LIBCP_LIB 选择归档: 默认 libcp.a, test-stage2 用
# stage2/libcp.a.
#
# 覆盖:
#   1. int f(int *p) { return *(p + 1); } 的 tokenize / preprocess /
#      parse / sema, 按公共类型信息缩放到 4 字节, 发射出
#      mov 4(%rdi), %eax 与 ret.
#   2. 语法错误与语义错误各一次: 回调收到文件名, 消息与 token, 公共
#      入口返回 NULL, libcp_context_failed 为 true.
#
# 不覆盖: -c 与汇编器调用, 依赖输出 (归 driver 测试); -E / CST /
# formatter 已有各自的测试.
#
# 用法: test/outside.sh (由 make test 与 make test-stage2 调用)

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
# LIBCP_LIB 选择要链接的归档; test-stage2 指向自举库.
lib=${LIBCP_LIB:-$root/libcp.a}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

if [ ! -f "$lib" ]; then
  echo "outside: $lib is missing; run make libcp.a" >&2
  exit 1
fi

# 源文件检查: 消费者不得包含内部伞头, 也不得出现 libcp/src 路径.
if grep -n -e 'libcp/src' -e '#include "libcp.h"' \
  "$root/test/outside/consumer.c"; then
  echo "outside: consumer.c reaches into libcp/src" >&2
  exit 1
fi

# 只复制公共头: 内部头不在搜索路径上, 任何意外依赖都会编译失败.
mkdir -p "$tmp/pubinc"
cp -R "$root/libcp/include/libcp" "$tmp/pubinc/"

"$cc" -std=c11 -Wall -Wextra -I"$tmp/pubinc" \
  -c "$root/test/outside/consumer.c" -o "$tmp/consumer.o"

# 消费者目标文件的未定义符号与公共 API 闸门共用同一份 deny list
# (test/public-api.deny): 引用内部符号说明公共 seam 泄漏了.
nm -u "$tmp/consumer.o" | awk '{print $NF}' > "$tmp/undef"
bad=
while IFS= read -r pat; do
  case $pat in
    ''|'#'*) continue ;;
  esac
  hit=$(grep -E -- "$pat" "$tmp/undef" || true)
  if [ -n "$hit" ]; then
    bad="$bad $hit"
  fi
done < "$root/test/public-api.deny"
if [ -n "$bad" ]; then
  echo "outside: the consumer references internal symbols:$bad" >&2
  exit 1
fi

# 输入: 一条真实降级路径, 两个错误输入 (语法错误在 parse 阶段, 语义
# 错误在 sema 阶段).
printf 'int f(int *p) { return *(p + 1); }\n' > "$tmp/good.c"
printf 'int f(void) { return 1 }\n' > "$tmp/bad-syntax.c"
printf 'int f(void) { return missing; }\n' > "$tmp/bad-semantic.c"

"$cc" -std=c11 -Wall -Wextra -I"$tmp/pubinc" \
  "$tmp/consumer.o" "$lib" -o "$tmp/consumer"
"$tmp/consumer" "$tmp/good.c" "$tmp/bad-syntax.c" "$tmp/bad-semantic.c" \
  > "$tmp/out.s"

# 降级发射: 偏移来自公共类型信息 (1 * sizeof(int) = 4), 返回走 %eax.
if ! grep -qx '  mov 4(%rdi), %eax' "$tmp/out.s" ||
  ! grep -qx '  ret' "$tmp/out.s"; then
  echo "outside: unexpected assembly:" >&2
  cat "$tmp/out.s" >&2
  exit 1
fi

echo "outside: ok"
