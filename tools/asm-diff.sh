#!/usr/bin/env bash
# 汇编等价性闸门: 用同一批 test/*.c 分别驱动 BASE ref 构建的 chibicc 与
# 当前工作树构建的 chibicc, 对 -S 产物做逐字节比较.
#
# 用法: tools/asm-diff.sh <BASE-ref> [chibicc 旗标...]
#
# BASE 是任意能解析为提交的 ref (SHA / 分支 / 标签), 脚本比较 BASE 构建
# 的 chibicc 与当前工作树构建的 chibicc.
#
# BASE 解析到 HEAD 且工作区干净时是自比, 脚本直接拒绝; 有未提交改动时
# BASE=HEAD 仍然合法, 那是在比较增量.
#
# 路径差异: 两边都先 cd 到各自的树根, 再用相对路径 test/*.c 编译, codegen
# 发射的 .file 伪指令因此记录同一串相对路径. 产物 .s 写在临时目录, 不会
# 进入 .file.
#
# 时间宏: BASE 之后的旗标由调用方提供 (Makefile 复用 SNAPSHOT_DATEFLAGS);
# 未提供时退回下面的固定值. test/macro.c 会把 __TIMESTAMP__ / __DATE__ /
# __TIME__ 编进字符串字面量, 不钉死的话两次构建的时间差会让 diff 每次都失败.
#
# 基线树是 `git worktree add --detach` 出来的临时工作树, 退出时 (含失败与
# 中断) 由 trap 删除. 当前树缺的 test 文件必须也在 BASE 里存在, 否则无法
# 比较, 直接报错.

set -euo pipefail

usage() {
    echo "usage: tools/asm-diff.sh <BASE-ref> [chibicc flags...]" >&2
}

if [ $# -lt 1 ] || [ -z "$1" ]; then
    usage
    exit 2
fi

base=$1
shift

if [ $# -eq 0 ]; then
    set -- \
        -D__TIMESTAMP__='"Mon Jan  1 00:00:00 2024"' \
        -D__DATE__='"Jan  1 2024"' \
        -D__TIME__='"00:00:00"'
fi
flags=("$@")

if ! root=$(git rev-parse --show-toplevel 2>/dev/null); then
    echo "asm-diff: not inside a git work tree" >&2
    exit 2
fi

# BASE 解析到 HEAD 且工作区完全干净时是自比, 直接拒绝, 避免把空 diff
# 当成等价性证据. 有未提交改动时 BASE=HEAD 仍然合法, 那是在比较增量.
head_commit=$(git -C "$root" rev-parse HEAD)
base_commit=$(git -C "$root" rev-parse --verify "$base^{commit}" 2>/dev/null || true)
if [ -n "$base_commit" ] && [ "$base_commit" = "$head_commit" ] &&
    git -C "$root" diff --quiet &&
    git -C "$root" diff --cached --quiet &&
    [ -z "$(git -C "$root" ls-files --others --exclude-standard)" ]; then
    echo "asm-diff: BASE resolves to HEAD and the tree is clean; this is a self-comparison" >&2
    echo "asm-diff: use the branch point (e.g. BASE=main) for branch-level evidence" >&2
    exit 2
fi

make_cmd=${MAKE:-make}

tmp=$(mktemp -d "${TMPDIR:-/tmp}/libcp-asm-diff.XXXXXX") || exit 1
tree=$tmp/base
cleanup() {
    if [ -e "$tree" ]; then
        git -C "$root" worktree remove --force "$tree" >/dev/null 2>&1 ||
            rm -rf "$tree"
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# 基线树: 从 BASE 的提交内容建一个 detached worktree, 不碰当前工作区.
echo "asm-diff: checking out $base into $tree" >&2
if ! git -C "$root" worktree add --detach "$tree" "$base" >&2; then
    echo "asm-diff: cannot create a worktree for $base" >&2
    exit 1
fi

build_tree() {
    local dir=$1
    (cd "$dir" && "$make_cmd" chibicc) >&2
}

echo "asm-diff: building chibicc at $base" >&2
if ! build_tree "$tree"; then
    echo "asm-diff: build failed at $base" >&2
    exit 1
fi

echo "asm-diff: building chibicc in the current tree" >&2
if ! build_tree "$root"; then
    echo "asm-diff: build failed in the current tree" >&2
    exit 1
fi

# 文件清单以当前树为准; 基线树里缺少同名文件就无法逐字节比较.
cd "$root" || exit 1
files=(test/*.c)
if [ ! -e "${files[0]}" ]; then
    echo "asm-diff: no test/*.c in the current tree" >&2
    exit 1
fi
for f in "${files[@]}"; do
    if [ ! -f "$tree/$f" ]; then
        echo "asm-diff: $f is missing from $base" >&2
        exit 1
    fi
done

mkdir -p "$tmp/base-out" "$tmp/cur-out" || exit 1

gen_tree() {
    local dir=$1 out=$2 f name
    for f in "${files[@]}"; do
        name=$(basename "$f" .c)
        if ! (cd "$dir" && ./chibicc -Iinclude -Itest "${flags[@]}" -S -o "$out/$name.s" "$f"); then
            echo "asm-diff: -S failed on $f in $dir" >&2
            return 1
        fi
    done
}

echo "asm-diff: emitting assembly at $base" >&2
gen_tree "$tree" "$tmp/base-out" || exit 1
echo "asm-diff: emitting assembly in the current tree" >&2
gen_tree "$root" "$tmp/cur-out" || exit 1

diffs=0
for f in "${files[@]}"; do
    name=$(basename "$f" .c)
    if ! cmp -s "$tmp/base-out/$name.s" "$tmp/cur-out/$name.s"; then
        echo "asm-diff: $f differs"
        diff -u "$tmp/base-out/$name.s" "$tmp/cur-out/$name.s" || true
        diffs=$((diffs + 1))
    fi
done

total=${#files[@]}
if [ "$diffs" -eq 0 ]; then
    echo "asm-diff: empty ($total files)"
    exit 0
fi

echo "asm-diff: NON-EMPTY ($diffs of $total files differ)" >&2
exit 1
