#!/usr/bin/env bash
# 文档事实闸门: 机械比对当前矩阵文档里的计数与公共面事实, 不校验散文.
# 事实来源是 test 脚本, 公共头目录, LIBCP_VERSION, Makefile 与
# test/install.sh; 不访问网络, 不读构建产物.
# docs/12 是历史章节, 其中的样本数是当时的事实, 不检查.
# 用法: test/docs-facts.sh [root]
set -euo pipefail

if [ "$#" -gt 0 ]; then
  root=$(cd -- "$1" && pwd)
else
  root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
fi

cd "$root"

fail=0
checked=0

report() {
  printf 'docs-facts: %s\n' "$*" >&2
  fail=1
}

first_number() {
  awk 'match($0, /[0-9]+/) { print substr($0, RSTART, RLENGTH); exit }'
}

first_version() {
  awk 'match($0, /[0-9]+\.[0-9]+\.[0-9]+/) { print substr($0, RSTART, RLENGTH); exit }'
}

# check_count <fact> <expected> <pattern>...
# 每个 pattern 在当前矩阵文档中的每个匹配都必须带 expected 这个数,
# 且该事实至少匹配一次, 避免事实被悄悄删掉.
check_count() {
  local fact=$1 expected=$2
  shift 2
  local file pattern matched value seen=0

  for file in docs/*.md; do
    case $file in
      docs/12-project-stages.md) continue ;;
    esac
    for pattern in "$@"; do
      while IFS= read -r matched; do
        [ -n "$matched" ] || continue
        value=$(printf '%s\n' "$matched" | first_number)
        seen=$((seen + 1))
        checked=$((checked + 1))
        [ "$value" = "$expected" ] ||
          report "$file: $fact is $value, expected $expected"
      done < <(grep -oE -- "$pattern" "$file" || true)
    done
  done

  if [ "$seen" -eq 0 ]; then
    report "$fact: no current-doc count found"
  fi
}

# check_version <fact> <expected> <pattern>...
check_version() {
  local fact=$1 expected=$2
  shift 2
  local file pattern matched value seen=0

  for file in docs/*.md; do
    case $file in
      docs/12-project-stages.md) continue ;;
    esac
    for pattern in "$@"; do
      while IFS= read -r matched; do
        [ -n "$matched" ] || continue
        value=$(printf '%s\n' "$matched" | first_version)
        seen=$((seen + 1))
        checked=$((checked + 1))
        [ "$value" = "$expected" ] ||
          report "$file: $fact is ${value:-<none>}, expected $expected"
      done < <(grep -oE -- "$pattern" "$file" || true)
    done
  done

  if [ "$seen" -eq 0 ]; then
    report "$fact: no current-doc version found"
  fi
}

diagnostic_count=$(grep -cE '^expect[[:space:]]' test/diagnostic.sh || true)
ast_count=$(grep -cE '^snippet[[:space:]]' test/ast.sh || true)
expand_dump_count=$(grep -cE '^exp_dump[[:space:]]' test/expand.sh || true)
d7_count=$(sed -n '/^d7="/,/^libcp_version"$/p' test/public-api.sh |
  wc -w | tr -d ' ')

[ "$diagnostic_count" -gt 0 ] ||
  report "test/diagnostic.sh: no expect cases found"
[ "$ast_count" -gt 0 ] || report "test/ast.sh: no snippet cases found"
[ "$expand_dump_count" -gt 0 ] ||
  report "test/expand.sh: no exp_dump cases found"
[ "$d7_count" -gt 0 ] || report "test/public-api.sh: no D7 list found"

check_count "diagnostic case count" "$diagnostic_count" \
  '诊断套件当前锁定 [0-9]+ 例' \
  '[0-9]+ 例诊断' \
  '逐字节锁定,? [0-9]+ 例' \
  '诊断 [0-9]+ 例'

check_count "AST case count" "$ast_count" \
  'AST [0-9]+ 例' \
  'test/ast\.sh.*[0-9]+ 例'

check_count "expand dump sample count" "$expand_dump_count" \
  '[0-9]+ 个 provenance dump 样本' \
  'test/expand\.sh.*[0-9]+ 个'

# 公共头数量: 目录是事实, 文档与 install 的期望必须一致.
header_names=$(cd libcp/include/libcp && ls -1 *.h | sort)
header_count=$(printf '%s\n' "$header_names" | wc -l | tr -d ' ')

check_count "public header count" "$header_count" \
  '[0-9]+ 个公共头,' \
  '恰好 [0-9]+ 个公共头'
check_count "umbrella sibling header count" "$((header_count - 1))" \
  '其余 [0-9]+ 个公共头'

install_count=$(awk '/-ne [0-9]+/ {
  for (i = 1; i <= NF; i++)
    if ($i == "-ne") {
      print $(i + 1)
      exit
    }
}' test/install.sh)
if [ "$install_count" != "$header_count" ]; then
  report "test/install.sh expects ${install_count:-<none>} public headers, found $header_count"
fi

install_headers=$(sed -n 's/^for h in \(.*\); do$/\1/p' test/install.sh |
  tr ' ' '\n' | sed 's/$/.h/' | sort)
if [ "$install_headers" != "$header_names" ]; then
  report "test/install.sh header list differs from libcp/include/libcp:"
  diff -u <(printf '%s\n' "$install_headers") \
    <(printf '%s\n' "$header_names") >&2 || true
fi

# docs/10 的安装布局必须列出全部公共头, 含 backend.h.
layout_headers=$(awk '
  /^\$PREFIX\/include\/libcp\/$/ { block = 1; next }
  block && /^```$/ { exit }
  block { print }
' docs/10-build-install.md | sed -n 's/^  \([A-Za-z0-9_]*\.h\)$/\1/p' | sort)
if [ "$layout_headers" != "$header_names" ]; then
  report "docs/10-build-install.md install layout differs from libcp/include/libcp:"
  diff -u <(printf '%s\n' "$header_names") \
    <(printf '%s\n' "$layout_headers") >&2 || true
fi

# 版本: 公共伞头与 Makefile 必须一致, 当前矩阵文档必须复述同一个版本.
header_version=$(sed -n 's/^#define LIBCP_VERSION "\(.*\)"/\1/p' \
  libcp/include/libcp/libcp.h)
make_version=$(awk -F= '$1 == "VERSION" { print $2; exit }' Makefile)
if [ -z "$header_version" ]; then
  report "LIBCP_VERSION: not found in libcp/include/libcp/libcp.h"
elif [ "$make_version" != "$header_version" ]; then
  report "Makefile VERSION is ${make_version:-<none>}, LIBCP_VERSION is $header_version"
else
  check_version "library version" "$header_version" \
    '库版本: `[0-9]+\.[0-9]+\.[0-9]+`' \
    '#define LIBCP_VERSION "[0-9]+\.[0-9]+\.[0-9]+"' \
    'modversion libcp.*# [0-9]+\.[0-9]+\.[0-9]+' \
    '当前 `[0-9]+\.[0-9]+\.[0-9]+`' \
    '版本 `[0-9]+\.[0-9]+\.[0-9]+`'
fi

# D7 数量: test/public-api.sh 的白名单是事实.
check_count "D7 function count" "$d7_count" \
  '[0-9]+ 个公共函数' \
  '[0-9]+ 个是公共函数' \
  '[0-9]+ 个 D7 函数' \
  '当前 [0-9]+ 个\.' \
  '本章的 [0-9]+ 个函数'

if [ "$fail" -ne 0 ]; then
  printf 'docs-facts: FAILED (%d facts checked)\n' "$checked" >&2
  exit 1
fi

printf 'docs-facts: ok (%d facts checked)\n' "$checked"
