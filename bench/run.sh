#!/bin/bash
# libcp 前端 benchmark 的运行器 (PLAN D4/D5): 只在 Linux 上采集基线,
# 先打印环境块与语料指纹, 再按组调用 bench/libcp-bench.
#
# 语料: 仓库自身(tests / lib / driver) + thirdparty/sqlite/sqlite3.c,
# 全部走原始源码与真实 include. 系统头路径从 C 编译器的搜索列表提取,
# 以便在不同发行版上找到 glibc 头.

set -u

# 输出一律用 C locale: 列对齐与哈希不随宿主 locale 变化.
export LC_ALL=C

cd "$(dirname "$0")/.." || exit 1

if [ "$(uname -s)" != "Linux" ]; then
  echo "bench: 只支持 Linux (本机 $(uname -s)); 其它宿主不做计时" >&2
  exit 1
fi

# 从 `cc -E -v -x c /dev/null` 的 "#include <...> search starts here:"
# 区块提取系统 include 路径. macOS 的 "(framework directory)" 后缀一并
# 去掉(守卫之后到不了, 留着让过滤器本身保持通用).
discover_system_includes() {
  cc -E -v -x c /dev/null 2>&1 | awk '
    /#include <...> search starts here:/ { on = 1; next }
    /End of search list\./ { on = 0 }
    on {
      sub(/^ /, "")
      sub(/ \(framework directory\)$/, "")
      print
    }
  '
}

system_includes=()
while IFS= read -r path; do
  [ -n "$path" ] && system_includes+=("$path")
done < <(discover_system_includes)

if [ "${#system_includes[@]}" -eq 0 ]; then
  echo "bench: cc -E -v -x c /dev/null 没有给出系统 include 路径" >&2
  exit 1
fi

bin=bench/libcp-bench
if [ ! -x "$bin" ]; then
  echo "bench: 缺少 $bin, 先运行 make bench-tool" >&2
  exit 1
fi

repeat=${BENCH_REPEAT:-5}

# 语料组. tests 里的 test/common 没有 .c 后缀, 但它是一个合法 TU;
# thirdparty/sqlite 缺失时(sqlite 组未 clone)整组跳过.
tests=(test/*.c test/common)
lib=(libcp/src/*.c)
driver=(main.c codegen.c)
sqlite=()
if [ -f thirdparty/sqlite/sqlite3.c ]; then
  sqlite=(thirdparty/sqlite/sqlite3.c)
fi

invoke() {
  local group=$1
  shift
  echo
  echo "== group: $group"
  "$bin" --group "$group" --repeat "$repeat" "$@"
}

# 环境块: 让保存下来的 stdout 自描述, 相同的绝对数字只在同一宿主可比.
print_environment() {
  local commit dirty cpu libc
  commit=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
  if [ -n "$(git status --porcelain 2>/dev/null)" ]; then
    dirty="-dirty"
  else
    dirty=""
  fi
  cpu=$(awk -F': ' '/^model name/ { print $2; exit }' /proc/cpuinfo 2>/dev/null)
  [ -n "$cpu" ] || cpu=$(uname -m)
  libc=$(ldd --version 2>/dev/null | head -n 1)
  [ -n "$libc" ] || libc=$(getconf GNU_LIBC_VERSION 2>/dev/null || echo unknown)

  echo "== environment"
  echo "git: $commit$dirty"
  echo "kernel: $(uname -srm)"
  echo "cpu: $cpu"
  echo "nproc: $(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo unknown)"
  echo "cc: $(cc --version 2>/dev/null | head -n 1)"
  echo "libc: $libc"
  echo "bench_cflags: ${BENCH_CFLAGS:-(unset)}"
  echo "libcp: $("$bin" --version 2>/dev/null || echo unknown)"
  echo "repeat: $repeat (warmup 1)"
}

hash_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{ print $1 }'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{ print $1 }'
  else
    echo unavailable
  fi
}

# 语料指纹: 每个输入文件的字节数与 SHA-256, 用来确认两次运行读的是同一
# 份源码.
print_corpus() {
  local f size
  echo
  echo "== corpus"
  printf '%-38s %10s  %s\n' "file" "bytes" "sha256"
  for f in "${tests[@]}" "${lib[@]}" "${driver[@]}" "${sqlite[@]}"; do
    size=$(wc -c < "$f" | tr -d ' ')
    printf '%-38s %10s  %s\n' "$f" "$size" "$(hash_file "$f")"
  done
}

print_environment
print_corpus

# 公共 include 前缀: 仓库的 include/ 是 chibicc 自带的自举头(stdarg,
# stddef, stdbool, ...), 必须排在从 cc 发现的编译器内部 include 之前,
# 否则 gcc 的 stdarg.h 会被选中(驱动器对 <argv0>/include 做同样的事).
# lib 与 driver 组按库的内部构建解析 allocator.h, 因此补上默认分配器
# 开关; driver.h 包含内部 umbrella, 所以 driver 组还要 libcp 的两条
# include 路径.

# 把系统 include 追加到调用方的 args 数组(bash 的动态作用域).
add_system_includes() {
  local p
  for p in "${system_includes[@]}"; do
    args+=(--include "$p")
  done
}

# tests: chibicc 的 include/ 与 test/(test/test.h).
args=(--include include --include test)
add_system_includes
for f in "${tests[@]}"; do
  args+=(--file "$f")
done
invoke tests "${args[@]}"

# lib: 库源码的公共头与内部 umbrella.
args=(--include include --include libcp/include --include libcp/src
      --define LIBCP_DEFAULT_ALLOCATOR)
add_system_includes
for f in "${lib[@]}"; do
  args+=(--file "$f")
done
invoke lib "${args[@]}"

# driver: driver.h 包含库的内部 umbrella.
args=(--include include --include . --include libcp/src --include libcp/include
      --define LIBCP_DEFAULT_ALLOCATOR)
add_system_includes
for f in "${driver[@]}"; do
  args+=(--file "$f")
done
invoke driver "${args[@]}"

# sqlite: 依赖 checkout 自身的头文件与 -D_GNU_SOURCE(与第三方脚本一致).
if [ "${#sqlite[@]}" -gt 0 ]; then
  args=(--include include --include thirdparty/sqlite --define _GNU_SOURCE)
  add_system_includes
  for f in "${sqlite[@]}"; do
    args+=(--file "$f")
  done
  invoke sqlite "${args[@]}"
fi
