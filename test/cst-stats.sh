#!/bin/sh
# CST 覆盖率统计与 golden 锁 (PLAN-l5 W3): 统计每个语料文件的
# CST_UNPARSED 节点数, 与 golden 逐行比对. golden 把"覆盖率现状"变成显式
# 契约: 覆盖变好或剩余成因变化必须显式更新 golden, 任何意外变化立即失败.
#
# 用法: test/cst-stats.sh ./chibicc [update]
#   update: 用当前输出重写 golden (覆盖变化后显式执行一次并随提交走).
# 由 make test 与 make test-stage2 调用 (stage2 编译器必须给出相同计数).

set -eu

chibicc=${1:?usage: test/cst-stats.sh <chibicc> [update]}
mode=${2:-check}
root=$(cd "$(dirname "$0")/.." && pwd)
golden="$root/test/cst-stats.golden"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

: > "$tmp/now"
for f in test/*.c include/*.h; do
  # grep -c prints 0 and exits 1 on no match; keep the count either way.
  n=$("$chibicc" -xc -dump-cst "$f" 2>/dev/null | grep -cx '  *UNPARSED' || true)
  printf '%s %s\n' "$n" "$f" >> "$tmp/now"
done

if [ "$mode" = update ]; then
  cp "$tmp/now" "$golden"
  total=$(awk '{s+=$1} END {print s}' "$golden")
  echo "cst-stats: golden updated ($total UNPARSED, $(wc -l < "$golden" | tr -d ' ') files)"
  exit 0
fi

if [ ! -f "$golden" ]; then
  echo "cst-stats: golden missing; run: test/cst-stats.sh ./chibicc update" >&2
  exit 1
fi

if cmp -s "$tmp/now" "$golden"; then
  total=$(awk '{s+=$1} END {print s}' "$golden")
  echo "cst-stats: golden matches ($total UNPARSED, $(wc -l < "$golden" | tr -d ' ') files)"
else
  echo "cst-stats: UNPARSED counts differ from golden:" >&2
  diff -u "$golden" "$tmp/now" >&2 || true
  echo "cst-stats: if intended, update with: test/cst-stats.sh ./chibicc update" >&2
  exit 1
fi
