// 诊断 (PLAN L3.1b): 注册回调收到的消息. 库本身从不通过这条路径打印;
// 默认处理器逐字节复现驱动器的 stderr 格式.

#ifndef LIBCP_DIAGNOSTIC_H
#define LIBCP_DIAGNOSTIC_H

typedef enum libcp_diagnostic_severity {
  LIBCP_DIAG_ERROR   = 0,
  LIBCP_DIAG_WARNING = 1,
} LibcpDiagnosticSeverity;

// 交给注册回调的诊断. `filename` 与 `message` 归 context 所有,
// 在下一次 libcp_context_reset 之前保持有效; 处理器若要在那之后继续
// 保存它们, 必须自行复制. 默认处理器还会打印给定位置处的源码行与
// 插入符.
//
// 当诊断锚定在 token 上时 (error_tok / warn_tok; 普通 error() 为 NULL),
// `tok` 是锚点 token, `prov` 是该 token 的宏展开链 (源 token 为 NULL).
// 处理器可以沿链走到最外层调用点 (PLAN-l5 W5). 默认处理器忽略两者,
// 打印结果与以前完全一致.
struct libcp_macro_prov;
struct libcp_token;
typedef struct libcp_diagnostic {
  const char* filename;
  int line;
  int column;
  LibcpDiagnosticSeverity severity;
  const char* message;
  const struct libcp_token* tok;
  const struct libcp_macro_prov* prov;
} LibcpDiagnostic;

typedef void (*LibcpDiagnosticFn)(void* user, const LibcpDiagnostic*);

#endif
