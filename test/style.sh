#!/usr/bin/env bash
# 风格闸门: tracked 源码与文档不得含中文标点, HEAD 提交信息须为
# 大写开头, 无结尾句号的纯 ASCII.
# 用法: test/style.sh [root]
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
  printf 'style: %s\n' "$*" >&2
  fail=1
}

# 七种中文标点的 UTF-8 字节. 这里用 printf 构造而不是写字面量,
# 否则本脚本会命中自己.
puncts=(
  "$(printf '\343\200\202')"
  "$(printf '\357\274\214')"
  "$(printf '\357\274\232')"
  "$(printf '\357\274\233')"
  "$(printf '\357\274\210')"
  "$(printf '\357\274\211')"
  "$(printf '\343\200\201')"
)

punct_args=()
for punct in "${puncts[@]}"; do
  punct_args+=(-e "$punct")
done

scan_file() {
  local file=$1
  local hit line

  while IFS= read -r hit; do
    # 唯一放行: AGENTS.md 中说明该规则本身的示例行.
    case $file in
      AGENTS.md) case $hit in *不使用中文标点*) continue ;; esac ;;
    esac
    line=${hit%%:*}
    report "$file:$line: Chinese punctuation"
  done < <(grep -nF "${punct_args[@]}" -- "$file" || true)
}

check_commit() {
  local subject first

  if ! subject=$(git log -1 --format=%s 2>/dev/null); then
    report "commit subject: cannot read HEAD commit"
    return 0
  fi
  if [ -z "$subject" ]; then
    report "commit subject: empty"
    return 0
  fi
  if printf '%s' "$subject" | LC_ALL=C grep -q '[^ -~]'; then
    report "commit subject: not ASCII: $subject"
  fi
  first=${subject:0:1}
  case $first in
    [A-Z]) ;;
    *) report "commit subject: first character not uppercase: $subject" ;;
  esac
  case $subject in
    *.) report "commit subject: ends with a period: $subject" ;;
  esac
}

while IFS= read -r file; do
  [ -f "$file" ] || continue
  checked=$((checked + 1))
  scan_file "$file"
done < <(git ls-files -- '*.md' '*.c' '*.h' '*.sh')

check_commit

if [ "$fail" -ne 0 ]; then
  printf 'style: FAILED (%d files checked)\n' "$checked" >&2
  exit 1
fi

printf 'style: ok (%d files checked, commit subject ok)\n' "$checked"
