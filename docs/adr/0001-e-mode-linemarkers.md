# -E 默认输出行标记

`-E` 的输出默认携带 GCC 形式的行标记 (`# <line> "<file>"`), 在起始, 文件切换与行号跳变处重同步, 以便 `distutils` 等下游工具能把输出行映射回源位置. 历史的不带标记输出由 `-P` 逐字节保留. 实现留在驱动器 `main.c`, 库不新增 `-E` 输出函数.

## Considered Options

- 只输出起始标记: 足以让 `test_distutils.test_search_cpp` 通过, 但标记对行映射是误导的.
- 完整 GCC 仿真: chibicc 没有 `<built-in>` / `<command-line>` 伪文件模型, 也没有消费者需要标志位.
- 行标记作为新开关: 不符合 GCC 语义, 裸 `-E` 的兼容问题仍在.

## Consequences

`test/trivia.sh` 的归一化金样改用 `-P`; 默认 `-E` 的输出多出标记行. 宏展开产物按最外层调用点归属.
