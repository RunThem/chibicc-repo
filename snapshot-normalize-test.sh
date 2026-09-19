#!/bin/bash
# snapshot-normalize-test.sh - snapshot-normalize.awk 的性质自测(PLAN R0.2).
# 前提: 先 `make docker-snapshot` 生成 .cache/snapshot 基线, 然后直接运行本脚本.
# 验证归一化器的两条核心性质与边界:
#   1. 幂等性: norm(norm(f)) == norm(f), 全部快照文件.
#   2. 双射重排被吃(等价类): 同前缀标签互换(.L..N <-> .L..M, 计数器交织)与
#      负 rbp 偏移互换(-N(%rbp) <-> -M(%rbp), lvar 重排)归一化后不可见.
#   3. 结构变化必须可见: 跨前缀标签互换, 指令增删, .loc/.file 折叠到位.
# token 互换用 perl 占位符三步换法(N 前缀绕开 perl 对 "-..." 参数的开关解析,
# 模式侧 quotemeta, 替换侧用原文 - quotemeta 文本直接做替换串会留下反斜杠).
cd "$(dirname "$0")" || exit 1
T=$(mktemp -d /tmp/ndt.XXXXXX); fail=0
F=.cache/snapshot/alignof.s; FL=.cache/snapshot/arith.s

swaptok() { # swaptok A B use_lookaround in out : 全局互换两个 token/offset
  local p="$1" q="$2" l="$3" in="$4" out="$5" lp="" rp=""
  [ "$l" = 1 ] && { lp='(?<![A-Za-z0-9_.$])'; rp='(?![A-Za-z0-9_.$])'; }
  perl -pe 'BEGIN{$p=substr(shift,1);$q=substr(shift,1);$lp=shift;$rp=shift;$pp=quotemeta $p;$qq=quotemeta $q}
    s/$lp$pp$rp/\x00/g; s/$lp$qq$rp/$p/g; s/\x00/$q/g' \
    "N$p" "N$q" "$lp" "$rp" "$in" > "$out"
}

if [ ! -d .cache/snapshot ]; then
  echo "no baseline (.cache/snapshot); run 'make docker-snapshot' first"; exit 1
fi

# 1. 幂等性: norm(f) == norm(norm(f)), 全部快照文件
for f in .cache/snapshot/*.s; do
  b=$(basename "$f")
  awk -f snapshot-normalize.awk "$f" > "$T/$b"
  awk -f snapshot-normalize.awk "$T/$b" | cmp -s - "$T/$b" || { echo "FAIL idempotence: $b"; fail=1; }
done
[ $fail -eq 0 ] && echo "PASS idempotence ($(ls .cache/snapshot | wc -l | tr -d ' ') files)"

# 2a. 同前缀标签双射互换(.L..N <-> .L..M)应被吃掉 - 计数器交织的等价类
A=$(grep -oE '\.L\.\.[0-9]+' "$FL" | sort -u | sed -n 1p)
B=$(grep -oE '\.L\.\.[0-9]+' "$FL" | sort -u | sed -n 2p)
swaptok "$A" "$B" 1 "$FL" "$T/swapL.s"
cmp -s "$FL" "$T/swapL.s" && { echo "FAIL label swap vacuous"; fail=1; }
awk -f snapshot-normalize.awk "$FL" > "$T/baseL.norm"
awk -f snapshot-normalize.awk "$T/swapL.s" > "$T/swapL.norm"
cmp -s "$T/baseL.norm" "$T/swapL.norm" \
  && echo "PASS label bijection swap eaten ($A <-> $B)" \
  || { echo "FAIL label bijection swap NOT eaten"; fail=1; }

# 2b. 负 rbp 偏移双射互换(-N(%rbp) <-> -M(%rbp))应被吃掉 - lvar 重排的等价类
C=$(grep -o -- '-[0-9]*(%rbp)' "$F" | sort -u | sed -n 1p)
D=$(grep -o -- '-[0-9]*(%rbp)' "$F" | sort -u | sed -n 2p)
swaptok "$C" "$D" 0 "$F" "$T/swapO.s"
cmp -s "$F" "$T/swapO.s" && { echo "FAIL offset swap vacuous"; fail=1; }
awk -f snapshot-normalize.awk "$F" > "$T/base.norm"
awk -f snapshot-normalize.awk "$T/swapO.s" > "$T/swapO.norm"
cmp -s "$T/base.norm" "$T/swapO.norm" \
  && echo "PASS offset bijection swap eaten ($C <-> $D)" \
  || { echo "FAIL offset bijection swap NOT eaten"; fail=1; }

# 2c. 跨前缀标签互换(.L..N <-> .L.end.M)必须仍被抓 - 前缀是结构
E=$(grep -oE '\.L\.end\.[0-9]+' "$FL" | sort -u | sed -n 1p)
if [ -n "$A" ] && [ -n "$E" ]; then
  swaptok "$A" "$E" 1 "$FL" "$T/swapX.s"
  cmp -s "$FL" "$T/swapX.s" && { echo "FAIL cross-prefix swap vacuous"; fail=1; }
  awk -f snapshot-normalize.awk "$T/swapX.s" | cmp -s - "$T/baseL.norm" \
    && { echo "FAIL cross-prefix swap masked"; fail=1; } \
    || echo "PASS cross-prefix swap caught (by design)"
fi

# 3. 结构变化必须可见: 删除一条普通指令 / 删除一条含标签引用的指令
awk '/^  mov/ && !d {d=1; next} {print}' "$F" > "$T/st1.s"
awk '/^  j(e|mp|ne)/ && !d {d=1; next} {print}' "$FL" > "$T/st2.s"
cmp -s "$F" "$T/st1.s" && { echo "FAIL st1 vacuous"; fail=1; }
cmp -s "$FL" "$T/st2.s" && { echo "FAIL st2 vacuous"; fail=1; }
awk -f snapshot-normalize.awk "$T/st1.s" | cmp -s - "$T/base.norm" \
  && { echo "FAIL structural (mov) masked"; fail=1; } || echo "PASS structural (mov) caught"
awk -f snapshot-normalize.awk "$T/st2.s" | cmp -s - "$T/baseL.norm" \
  && { echo "FAIL structural (jump) masked"; fail=1; } || echo "PASS structural (jump) caught"

# 4. .loc/.file 折叠确认
nloc=$(grep -cE '^[[:space:]]*\.(loc|file)[[:space:]]' "$F")
nloc2=$(grep -cE '^[[:space:]]*\.(loc|file)[[:space:]]' "$T/base.norm")
[ "$nloc" -gt 0 ] && [ "$nloc2" -eq 0 ] && echo "PASS .loc/.file folded ($nloc lines removed in $F)" \
  || echo "FAIL .loc/.file not folded ($nloc -> $nloc2)"

[ $fail -eq 0 ] && echo "ALL LOCAL TESTS PASSED" || echo "LOCAL TESTS FAILED"
rm -rf "$T"
exit $fail
