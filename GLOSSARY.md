# chibicc-repo

C 前端库 libcp 与其参考驱动器 chibicc 的上下文. 本文件只定义领域词汇, 不记录实现细节.

## Language

**-E 行标记**:
预处理输出中 `# <line> "<file>"` 形式的行, 声明其后输出行的源文件与行号.
_Avoid_: GNU 行标记, linemarker

**#line 指令**:
源文件中改变后续行号与文件名的预处理指令, 也接受 `# 123 "file"` 的 GNU 形式.
_Avoid_: 行标记, 行号指令
