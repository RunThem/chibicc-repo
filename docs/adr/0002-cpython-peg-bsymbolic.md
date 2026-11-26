# CPython peg 扩展用 -Bsymbolic 代替隐藏可见性

chibicc 不实现 `-fvisibility=hidden`. CPython 的 configure 用
`$CC -fvisibility=hidden` 探测编译器; 探测失败时, 静态解释器以
`-export-dynamic` 链接, 内部的 `_PyPegen_*` / `PyTokenizer_*` 仍留在全局
作用域. `test_peg_generator` 构建的扩展自带这些源文件的副本与生成的
`_PyPegen_parse`, 默认可见性让扩展内部调用优先命中解释器的版本, 自定义
语法解析器因此从未执行; 78 个用例里有 9 个以 `SyntaxError` 失败.

本次修复不改 chibicc, 而是在 `test/thirdparty/cpython.sh` 里给
`Tools/peg_generator/pegen/build.py` 的扩展链接追加 `-Wl,-Bsymbolic`.
扩展内部对自身定义的引用绑定到自身, 等价于这些符号对扩展本地可见,
`PyInit_parse` 仍然导出, 解释器照常加载扩展.

## Considered Options

- 实现 `-fvisibility=hidden` 并让 configure 探测通过: 探测通过后 CPython 会把
  该选项加进 `CFLAGS_NODIST` 并用于整个解释器. CPython 依赖
  `Py_EXPORTED_SYMBOL` 给公共 API 重新声明 default 可见性, 但该宏在
  chibicc 下为空 (`Include/exports.h` 要求 `__GNUC__` 或 `__clang__`);
  glibc 的 `sys/cdefs.h` 还会在缺少这些身份宏时把 `__attribute__` 定义为空.
  结果会是公共 `Py*` API 全部隐藏, 所有标准库扩展在 import 时解析失败.
- 只实现 `__attribute__((visibility(...)))`: 属性在包含系统头的翻译单元里
  已被 glibc 预处理掉, 到不了解析器; 而且没有 `-fvisibility=hidden` 的默认值,
  peg 扩展内部符号仍是默认可见性.
- 定义 `__GNUC__` 以启用 CPython 的可见性标注: 会把所有 GCC 分支打开,
  glibc 与 CPython 的 attribute, builtin 与代码路径都随之改变, 风险超出
  本次修复, 也偏离 chibicc 的身份宏边界.
- 用 linker version script 只导出 `PyInit_parse`: 与 `-Bsymbolic` 等效,
  但需要额外文件与路径处理; 本次选更小的链接参数.
- 继续把 `test_peg_generator` 记为已知残留: 放弃 9 个用例的覆盖.

## Consequences

`test_peg_generator` 的 78 个用例在 chibicc 构建的 CPython 上全部通过;
扩展仍然导出其全局符号, 但内部调用不再被解释器抢占. chibicc 继续不支持
`-fvisibility=hidden`; 若未来实现完整的可见性语义 (包括第三方头文件里的
attribute 与 `__GNUC__` 分支), 可以重新评估让 CPython 自己开启该选项.
补丁锚定在 CPython 的固定提交上, 并由 `grep` 守卫; 源形状变化时脚本会
硬失败, 不会静默跳过.
