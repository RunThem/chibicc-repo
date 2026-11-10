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

# B2.1 暂时只打印发现结果.
printf 'system include: %s\n' "${system_includes[@]}"
