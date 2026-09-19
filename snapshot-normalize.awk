# snapshot-normalize.awk - 汇编快照归一化(PLAN R0.2).
# docker-snapshot-ndiff 在 diff 前对基线与新快照施加同一变换, 吃掉三类预期字节
# 变化, 让 [重组] 步骤能以"结构等价"为形状闸门:
#   1. 折叠 .loc/.file 行 - 吃掉语句链整形(PLAN R3)造成的调试行号重排.
#   2. 末段为纯数字的 .L 标签按文件内首现顺序重编号
#      (.L.else.5 -> .L.else.1, .L..3 -> .L..1) - 吃掉 parse/sema 计数器交织与
#      标签分配后置(R1.1/R2.6)造成的重排. 覆盖 codegen count() 系(.L.begin/.
#      end/.else/.true/.false.N)与 new_unique_name 系(.L..N, 匿名全局与唯一
#      标签). .L.return.<函数名> 不带 ".纯数字" 尾段, 不受影响.
#   3. 负 rbp 局部偏移 -N(%rbp) 按每函数首现顺序映射为序号
#      (-16(%rbp) -> -1(%rbp)) - 吃掉隐藏变量创建时机变化(R2.6/R2.8)造成的
#      lvar 偏移重排. 函数边界 = 列 0 的非点号标签.
# 不触碰: 指令文本与助记符, 立即数, 正偏移(实参), 函数名/全局名,
# .L.return.*, .cfi_*, .string/.long 等数据内容 - 结构性变化仍然可见.
# 对同一输入逐字节幂等(重跑时两套映射均为恒等).

function renum(line,   out, rest, tok, pre) {
  out = ""
  rest = line
  while (match(rest, /\.L[A-Za-z0-9_.$]+/)) {
    tok = substr(rest, RSTART, RLENGTH)
    out = out substr(rest, 1, RSTART - 1)
    rest = substr(rest, RSTART + RLENGTH)
    if (tok ~ /\.[0-9]+$/) {
      if (!(tok in lmap)) { lseq++; lmap[tok] = lseq }
      pre = tok
      sub(/[0-9]+$/, "", pre)
      out = out pre lmap[tok]
    } else {
      out = out tok
    }
  }
  return out rest
}

function seqoff(line,   out, rest, tok) {
  out = ""
  rest = line
  while (match(rest, /-[0-9]+\(%rbp\)/)) {
    tok = substr(rest, RSTART, RLENGTH)
    if (!(tok in omap)) { oseq++; omap[tok] = oseq }
    out = out substr(rest, 1, RSTART - 1) "-" omap[tok] "(%rbp)"
    rest = substr(rest, RSTART + RLENGTH)
  }
  return out rest
}

{
  if ($0 ~ /^[[:space:]]*\.(loc|file)[[:space:]]/) next
  if ($0 ~ /^[A-Za-z_$][A-Za-z0-9_$]*:/) { split("", omap); oseq = 0 }
  print seqoff(renum($0))
}
