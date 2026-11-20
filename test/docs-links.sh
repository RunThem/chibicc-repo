#!/usr/bin/env bash
# 文档链接闸门: 检查本地 markdown 链接与 AGENTS.md 的 docs 文件与章节引用.
# 用法: test/docs-links.sh [root]
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
  printf 'docs-links: %s\n' "$*" >&2
  fail=1
}

check_target() {
  local file=$1
  local target=$2
  local path resolved

  case $target in
    http://*|https://*|mailto:*|'#'*) return 0 ;;
  esac

  path=${target%%#*}
  [ -n "$path" ] || return 0

  case $path in
    /*) resolved=$root$path ;;
    *) resolved=$(dirname -- "$file")/$path ;;
  esac

  checked=$((checked + 1))
  [ -e "$resolved" ] || report "$file: missing link target: $target"
}

scan_markdown_links() {
  local file=$1
  local target

  while IFS= read -r target; do
    check_target "$file" "$target"
  done < <(grep -oE '\]\([^)]+\)' "$file" 2>/dev/null | sed 's/^](//; s/)$//' || true)
}

scan_agents_files() {
  local file=AGENTS.md
  local ref

  [ -f "$file" ] || return 0
  while IFS= read -r ref; do
    checked=$((checked + 1))
    [ -e "$ref" ] || report "$file: missing file reference: $ref"
  done < <(grep -oE '`(AGENTS|README)\.md`|`(docs|plans)/[A-Za-z0-9._/-]+\.md`' "$file" | tr -d '`' || true)
}

scan_agents_sections() {
  local file=AGENTS.md
  local pair ref section section_re

  [ -f "$file" ] || return 0
  while IFS= read -r pair; do
    ref=${pair% *}
    section=${pair##* }
    section_re=${section//./\\.}
    checked=$((checked + 1))
    if [ ! -f "$ref" ]; then
      report "$file: missing section file: $ref"
    elif ! grep -qE "^## ${section_re}( |$)" "$ref"; then
      report "$file: missing section: $ref $section"
    fi
  done < <(grep -oE '`docs/[A-Za-z0-9._/-]+\.md` [0-9]+\.[0-9]+' "$file" | tr -d '`' || true)
}

for file in AGENTS.md README.md docs/*.md plans/PLAN.md plans/RESULT.md; do
  [ -f "$file" ] || continue
  scan_markdown_links "$file"
done

scan_agents_files
scan_agents_sections

if [ "$fail" -ne 0 ]; then
  printf 'docs-links: FAILED (%d references checked)\n' "$checked" >&2
  exit 1
fi

printf 'docs-links: ok (%d references checked)\n' "$checked"
