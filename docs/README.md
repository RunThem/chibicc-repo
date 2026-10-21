# libchibicc 文档

`libchibicc` 是 chibicc 的 C 前端库. 本文档面向要在自己的软件里嵌入 C 词法, 预处理, 语法树或语义分析能力的第三方开发者, 说明这个库的设计, 分层, 每一层的产物与用法, 以及当前版本的真实缺陷.

本文档不是公共头的替代品. 公共头是最终事实来源, 位于 `libchibicc/include/chibicc/`, 入口是 `<chibicc/chibicc.h>`. 当文档与头文件冲突时, 以头文件为准, 并应该把文档当作缺陷修掉.

## 版本与状态

- 库版本: `0.1.0` (`CHIBICC_VERSION`, `chibicc_version()`).
- 文档对应的代码基线: `main` 分支 `dd3e631` 之后的文档提交.
- 路线图阶段 5, 即库边界固化, 已完成: 公共头拆分, 不透明 context, arena 生命周期, 诊断回调, `libchibicc.a`, Linux 下的 `libchibicc.so`, install 与 pkg-config, 以及 36 个公共函数的白名单, 都有测试闸门.
- 阶段 6, 即 L5 产品化验证, **尚未开工**. 目前没有随库发布的 formatter, 没有 AST 到源码的映射 API, 没有诊断展开点字段. 第 11 章会逐项说明这些缺口对使用者的影响.
- 只承诺源码兼容, 不承诺 ABI. 公共结构体字段直接暴露, 换字段就是换版本.

## 建议阅读路径

第一次接触这个库:

1. 读 [01 库概览](01-overview.md), 弄清它是什么, 不是什么.
2. 读 [02 架构与分层](02-architecture.md), 建立五层模型.
3. 读 [09 API 参考与用法](09-api-reference.md), 把最小管线跑通.

要写一个具体工具:

- 格式化, 重构, 保留注释: [03 Token 层](03-layer1-token.md) 与 [05 CST 层](05-layer2-cst.md).
- C 转 C, 语法级 lint: [06 语法层](06-layer3-parse.md).
- 类型相关分析, 符号表, 数据布局: [07 语义层](07-layer4-sema.md).
- 宏展开追踪: [04 预处理与 provenance](04-preprocess.md).

要修改库本身:

- 先读 [02 架构与分层](02-architecture.md) 与 [11 缺陷与边界](11-limitations.md), 再读 [12 工程阶段史](12-project-stages.md).
- 历史计划与执行记录在 `plans/`, 那些文件记录的是当时的口径, 不是当前 API 文档.

## 目录

| 编号 | 文件 | 内容 |
|---|---|---|
| 00 | [README.md](README.md) | 索引, 阅读路径, 约定 |
| 01 | [01-overview.md](01-overview.md) | 库的定位, 目标, 设计原则, 适用与不适用场景, 术语 |
| 02 | [02-architecture.md](02-architecture.md) | 五层架构, 横切的预处理, 生命周期, 错误模型, 线程模型 |
| 03 | [03-layer1-token.md](03-layer1-token.md) | 第 1 层: token 流与 trivia, 逐字节重建 |
| 04 | [04-preprocess.md](04-preprocess.md) | 预处理与宏展开 provenance |
| 05 | [05-layer2-cst.md](05-layer2-cst.md) | 第 2 层: lossless CST |
| 06 | [06-layer3-parse.md](06-layer3-parse.md) | 第 3 层: 忠实语法 AST |
| 07 | [07-layer4-sema.md](07-layer4-sema.md) | 第 4 层: sema 标注树 |
| 08 | [08-codegen.md](08-codegen.md) | 第 5 层: x86-64 codegen 参考消费者 |
| 09 | [09-api-reference.md](09-api-reference.md) | 公共 API 逐项说明, 完整示例, 常见配方 |
| 10 | [10-build-install.md](10-build-install.md) | 构建, 安装, 链接, 测试闸门 |
| 11 | [11-limitations.md](11-limitations.md) | 缺陷, 限制, 坑, 规避方式 |
| 12 | [12-project-stages.md](12-project-stages.md) | 工程阶段史与每个阶段的产物 |

## 五分钟跑通

下面的程序验证过可以编译并运行. 输入是内存里的字符串, 不需要文件.

```c
#include <chibicc/chibicc.h>
#include <stdio.h>

static void on_diagnostic(void *user, const ChibiccDiagnostic *d) {
  (void)user;
  fprintf(stderr, "%s:%d:%d: %s\n",
          d->filename ? d->filename : "<input>", d->line, d->column,
          d->message);
}

int main(void) {
  ChibiccContext *ctx = chibicc_context_new();
  chibicc_context_set_diagnostic(ctx, on_diagnostic, NULL);

  char *src = "int square(int x) { return x * x; }\n";

  ChibiccToken *tok = chibicc_tokenize_buffer(ctx, "<demo>", src);
  tok = chibicc_preprocess(ctx, tok);
  ChibiccNode *ast = chibicc_parse(ctx, tok);
  ChibiccObj *prog = chibicc_sema(ctx, ast);

  for (ChibiccObj *o = prog; o; o = o->next)
    printf("global: %s, function=%d\n", o->name, o->is_function);

  chibicc_context_free(ctx);
  return 0;
}
```

在仓库根目录构建并运行:

```sh
make libchibicc.a
cc -std=c11 -Ilibchibicc/include demo.c libchibicc.a -o demo
./demo
```

预期输出包含:

```text
global: square, function=1
```

## 约定

- 本文里的"层"指中间表示层, 即第 1 到第 5 层; "阶段"指工程实施阶段, 即路线图里的阶段 0 到 6 以及若干附加线. 两者不是同一个概念.
- 代码路径一律相对于仓库根目录.
- `ctx` 指 `ChibiccContext *`.
- "TU" 指一个翻译单元. 一个 context 服务一个 TU.
- 文中所有 API 名称与 C 标识符保留英文原文.
- 文档里的示例最小化到能独立编译, 省略的错误处理会在正文说明.
