#!/bin/bash
# A/B equivalence harness for the two expression parsers (PLAN-pratt-parse
# 4.8 and 判据 2/3). Two builds of the compiler - the default cascade and
# CHIBICC_PRATT_PARSE_FEATURE=1 - must produce byte-identical layer-3 trees
# and byte-identical assembly.
#
# Corpus mode (default): all 41 test/*.c through -dump-ast and -S, with
# the time macros pinned the way make docker-snapshot pins them.
#
# --thirdparty tinycc: clone/reset the pinned tinycc commit, configure
# once with the old compiler, then build twice - once with each compiler
# behind a shim that forwards every command and, for a `-c` command,
# additionally records -dump-ast and -S under a key derived from the source
# path and the normalized arguments. The two log trees must diff empty,
# and each build's own test suite must pass. The auxiliary runs write only
# into the temporary log directory.
#
# Usage: test/pratt-ab.sh <old> <new> [--thirdparty tinycc]

set -u

repo=$(cd "$(dirname "$0")/.." && pwd)

if [ $# -lt 2 ]; then
    echo "usage: test/pratt-ab.sh <old> <new> [--thirdparty tinycc]" >&2
    exit 1
fi

old=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
new=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
shift 2

mode=corpus
if [ "${1:-}" = "--thirdparty" ]; then
    if [ "${2:-}" != "tinycc" ]; then
        echo "pratt-ab: only --thirdparty tinycc is supported" >&2
        exit 1
    fi
    mode=tinycc
fi

tmp=$(mktemp -d /tmp/chibicc-pratt-ab-XXXXXX)
trap 'rm -rf "$tmp"' INT TERM HUP EXIT

# The same values make's SNAPSHOT_DATEFLAGS passes: test/macro.c folds the
# time macros into string literals, and they must not vary with the clock.
dateflags=(
    -D__TIMESTAMP__="\"Mon Jan  1 00:00:00 2024\""
    -D__DATE__="\"Jan  1 2024\""
    -D__TIME__="\"00:00:00\""
)

fail() {
    echo "pratt-ab: FAILED: $*" >&2
    exit 1
}

run_corpus() {
    mkdir -p "$tmp/old" "$tmp/new"
    local n=0
    local f b

    for f in test/*.c; do
        b=$(basename "$f" .c)
        n=$((n + 1))

        if ! "$old" -Iinclude -Itest "${dateflags[@]}" \
                -dump-ast -o "$tmp/old/$b.ast" "$f" >/dev/null 2>"$tmp/old/$b.err"; then
            cat "$tmp/old/$b.err" >&2
            fail "old -dump-ast on $f"
        fi
        if ! "$new" -Iinclude -Itest "${dateflags[@]}" \
                -dump-ast -o "$tmp/new/$b.ast" "$f" >/dev/null 2>"$tmp/new/$b.err"; then
            cat "$tmp/new/$b.err" >&2
            fail "new -dump-ast on $f"
        fi
        if ! diff -u "$tmp/old/$b.ast" "$tmp/new/$b.ast" > "$tmp/$b.ast.diff"; then
            echo "pratt-ab: AST mismatch: $f" >&2
            cat "$tmp/$b.ast.diff" >&2
            fail "first AST diff at $f"
        fi

        if ! "$old" -Iinclude -Itest "${dateflags[@]}" \
                -S -o "$tmp/old/$b.s" "$f" >/dev/null 2>"$tmp/old/$b.serr"; then
            cat "$tmp/old/$b.serr" >&2
            fail "old -S on $f"
        fi
        if ! "$new" -Iinclude -Itest "${dateflags[@]}" \
                -S -o "$tmp/new/$b.s" "$f" >/dev/null 2>"$tmp/new/$b.serr"; then
            cat "$tmp/new/$b.serr" >&2
            fail "new -S on $f"
        fi
        if ! diff -u "$tmp/old/$b.s" "$tmp/new/$b.s" > "$tmp/$b.s.diff"; then
            echo "pratt-ab: assembly mismatch: $f" >&2
            cat "$tmp/$b.s.diff" >&2
            fail "first assembly diff at $f"
        fi
    done

    echo "pratt-ab: corpus $n files, -dump-ast and -S byte-identical"
}

# The shim: forward the command unchanged, then for a -c command record
# -dump-ast and -S of the same source with the dependency-generation and
# output flags removed. PRATT_AB_REAL and PRATT_AB_LOG come from the
# environment of the make invocation.
make_shim() {
    mkdir -p "$tmp/bin"
    cat > "$tmp/bin/chibicc" <<'SHIM'
#!/bin/bash
real=$PRATT_AB_REAL
log=$PRATT_AB_LOG

"$real" "$@"
st=$?
[ $st -eq 0 ] || exit $st

has_c=0
src=""
args=()
while [ $# -gt 0 ]; do
    case "$1" in
    -c) has_c=1 ;;
    -o) shift ;;
    -MF|-MT|-MQ) shift ;;
    -MD|-MMD|-MP) ;;
    *.c) src=$1; args+=("$1") ;;
    *) args+=("$1") ;;
    esac
    shift
done

[ $has_c -eq 1 ] || exit 0
[ -n "$src" ] || exit 0

# A key from the source spelling and the normalized argument vector: the
# same command gets the same key in both builds, independent of the order
# the build happened to run.
key=$(printf '%s\n' "$src" "${args[@]}" | cksum | awk '{print $1 "_" $2}')
base=$(printf '%s' "$src" | tr '/ ' '__')

"$real" "${args[@]}" \
    -D__TIMESTAMP__="\"Mon Jan  1 00:00:00 2024\"" \
    -D__DATE__="\"Jan  1 2024\"" \
    -D__TIME__="\"00:00:00\"" \
    -dump-ast -o "$log/$base.$key.ast" || exit 1
"$real" "${args[@]}" \
    -D__TIMESTAMP__="\"Mon Jan  1 00:00:00 2024\"" \
    -D__DATE__="\"Jan  1 2024\"" \
    -D__TIME__="\"00:00:00\"" \
    -S -o "$log/$base.$key.s" || exit 1
exit 0
SHIM
    chmod +x "$tmp/bin/chibicc"
}

run_tinycc() {
    local pin=df67d8617b7d1d03a480a28f9f901848ffbfb7ec
    local dir="$repo/thirdparty/tinycc"
    local jobs
    jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)

    mkdir -p "$repo/thirdparty"
    if [ ! -d "$dir" ]; then
        (cd "$repo/thirdparty" && git clone git@github.com:TinyCC/tinycc.git) \
            || fail "cannot clone tinycc"
    fi

    (cd "$dir" && git reset --hard "$pin") || fail "cannot reset tinycc to $pin"

    make_shim
    mkdir -p "$tmp/old-log" "$tmp/new-log"

    (cd "$dir" && ./configure --cc="$old") || fail "tinycc configure"

    echo "pratt-ab: building tinycc with the old parser"
    (cd "$dir" && make -j"$jobs" clean) || fail "tinycc clean (old)"
    (cd "$dir" && PRATT_AB_REAL="$old" PRATT_AB_LOG="$tmp/old-log" \
        make -j"$jobs" CC="$tmp/bin/chibicc") || fail "tinycc build (old)"
    (cd "$dir" && make -j"$jobs" CC=cc test) || fail "tinycc test suite (old)"

    echo "pratt-ab: building tinycc with the new parser"
    (cd "$dir" && make -j"$jobs" clean) || fail "tinycc clean (new)"
    (cd "$dir" && PRATT_AB_REAL="$new" PRATT_AB_LOG="$tmp/new-log" \
        make -j"$jobs" CC="$tmp/bin/chibicc") || fail "tinycc build (new)"
    (cd "$dir" && make -j"$jobs" CC=cc test) || fail "tinycc test suite (new)"

    if ! diff -ru "$tmp/old-log" "$tmp/new-log" > "$tmp/tinycc.diff"; then
        echo "pratt-ab: tinycc AST/assembly mismatch" >&2
        head -200 "$tmp/tinycc.diff" >&2
        fail "tinycc -c equivalence"
    fi

    local ast s
    ast=$(find "$tmp/old-log" -name '*.ast' | wc -l | tr -d ' ')
    s=$(find "$tmp/old-log" -name '*.s' | wc -l | tr -d ' ')
    echo "pratt-ab: tinycc $ast translation units, $s assembly files byte-identical"
}

cd "$repo"
if [ "$mode" = corpus ]; then
    run_corpus
else
    run_tinycc
fi
