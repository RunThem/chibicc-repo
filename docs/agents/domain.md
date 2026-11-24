# Domain Docs

engineering skills 在探索代码库前应如何读取本仓库的领域文档.

## 探索前先读

- 根目录 `GLOSSARY.md`; 或
- 根目录 `GLOSSARY-MAP.md` (若存在): 它指向每个 context 的 `GLOSSARY.md`, 只读与当前主题相关的那些.
- `docs/adr/`: 读与即将改动的领域相关的 ADR. 多 context 仓库还要看 `libcp/docs/adr/`.

这些文件不存在时静默继续. 不要提示缺失, 也不要主动建议创建; `/domain-modeling` skill (经 `/grill-with-docs` 与 `/improve-codebase-architecture` 到达) 会在术语或决策真正落定时惰性创建.

## 文件结构

本仓库是 single-context:

```
/
├── GLOSSARY.md
├── docs/adr/
│   ├── 0001-....md
│   └── 0002-....md
├── docs/
└── libcp/
```

## 使用 glossary 的词汇

输出中提到领域概念时 (issue 标题, 重构提案, 假设, 测试名), 使用 `GLOSSARY.md` 定义的术语, 不要漂移到 glossary 明确避免的同义词.

需要的概念还不在 glossary 里时, 这是一个信号: 要么你在发明项目不使用的语言 (重新考虑), 要么存在真实缺口 (记录给 `/domain-modeling`).

## 标记 ADR 冲突

输出与现有 ADR 矛盾时, 显式指出, 不要静默覆盖:

> _与 ADR-0007 (event-sourced orders) 矛盾, 但值得重开, 因为..._
