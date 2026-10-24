#!/bin/sh
# formatter 闸门 (PLAN-l5 W4): 对全部语料文件验证
#   1. 幂等: format(format(x)) == format(x)
#   2. 行数不变(行保持策略)
#   3. 注释数量不变(naive 计数在输入与输出之间仍是不变量: 拼写不动,
#      字符串里的误计在两侧相同)
#   4. (仅容器内) -S 等价: format 后的 -S 汇编与原文件逐字节相同,
#      时间宏固定. 本机缺 Linux 系统头时探测跳过, docker 内为权威.
#
# 用法: test/fmt.sh <chibicc> <fmt>

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
chibicc=${1:?usage: test/fmt.sh <chibicc> <fmt>}
fmt=${2:?usage: test/fmt.sh <chibicc> <fmt>}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

n=0
for f in test/*.c include/*.h; do
  n=$((n + 1))

  "$fmt" "$root/$f" > "$tmp/f1"
  "$fmt" "$tmp/f1" > "$tmp/f2"
  cmp -s "$tmp/f1" "$tmp/f2"
  echo "testing fmt/idempotent/$f ... passed"

  [ "$(wc -l < "$f" | tr -d ' ')" = "$(wc -l < "$tmp/f1" | tr -d ' ')" ]
  echo "testing fmt/lines/$f ... passed"

  a=$(grep -o '/\*\|//' "$f" | wc -l | tr -d ' ')
  b=$(grep -o '/\*\|//' "$tmp/f1" | wc -l | tr -d ' ')
  [ "$a" = "$b" ]
  echo "testing fmt/comments/$f ... passed"
done

# -S 等价: 只在 chibicc 能就地汇编前置处理(有系统头)的环境跑, 即容器内.
# -S 输出首行的 .file 嵌入源路径, 自我包含的文件(test/pragma-once.c)也
# 依赖路径, 所以这里原地工作: 原文件先编译, 用格式化产物覆盖后再编译,
# 然后还原. 容器里的 /work 是一次性副本, 就地改写是安全的.
if "$chibicc" -Iinclude -Itest -S -o "$tmp/probe.s" test/typeof.c 2>/dev/null; then
  # 时间宏固定成带字面引号的字符串(与 Makefile 的快照目标同款):
  # test/macro.c 把 __DATE__/__TIME__ 当字符串字面量用.
  n2=0
  for f in test/*.c; do
    n2=$((n2 + 1))
    cp "$root/$f" "$tmp/orig.bak"
    "$chibicc" -Iinclude -Itest -D__TIMESTAMP__='"Mon Jan  1 00:00:00 2024"' \
      -D__DATE__='"Jan  1 2024"' -D__TIME__='"00:00:00"' -S -o "$tmp/a.s" "$f"
    "$fmt" "$root/$f" > "$tmp/f1.c"
    cp "$tmp/f1.c" "$root/$f"
    "$chibicc" -Iinclude -Itest -D__TIMESTAMP__='"Mon Jan  1 00:00:00 2024"' \
      -D__DATE__='"Jan  1 2024"' -D__TIME__='"00:00:00"' -S -o "$tmp/b.s" "$f"
    mv "$tmp/orig.bak" "$root/$f"
    cmp -s "$tmp/a.s" "$tmp/b.s"
    echo "testing fmt/s-equiv/$f ... passed"
  done
  echo "fmt lock: $n files idempotent, lines and comments kept, $n2 files -S equivalent"
else
  echo "fmt lock: $n files idempotent, lines and comments kept (-S skipped: no system headers)"
fi
