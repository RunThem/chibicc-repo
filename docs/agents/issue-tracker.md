# Issue tracker: GitHub

本仓库的 issue, spec, 计划与执行记录都存于 GitHub Issues, 统一用 `gh` CLI 操作. 目标仓库是 `RunThem/chibicc-repo`.

计划的唯一事实来源是 issue tracker, 不再写入 `plans/`. 描述性事实仍在 `docs/`, 规范约束仍在 `AGENTS.md`.

首次使用前确认 `gh auth status` 成功; 否则 `to-tickets`, `triage`, `to-spec` 无法读写 issue.

## 约定

- 创建 issue: `gh issue create --title "..." --body "..."`; 多行正文用 heredoc.
- 读取 issue: `gh issue view <number> --json number,title,body,labels,comments`.
- 列 issue: `gh issue list --state open --json number,title,body,labels,comments --jq '[.[] | {number, title, body, labels: [.labels[].name], comments: [.comments[].body]}]'`, 按需加 `--label` / `--state`.
- 设为子 issue: `gh issue create --parent <parent> ...`, 或用 `gh issue edit <parent> --add-sub-issue <child>` (`gh` 2.94+). 旧版 `gh`: `gh api --method POST repos/<owner>/<repo>/issues/<parent>/sub_issues -F sub_issue_id=<child-db-id>` (数据库 id). 不支持子 issue 时, 在子 issue 正文顶部写 `Part of #<parent>`.
- 阻塞关系: 用 GitHub 原生 issue dependencies. `gh issue create --blocked-by <n>,<n> ...` 或 `gh issue edit <child> --add-blocked-by <blocker>` (`gh` 2.94+). 旧版 `gh`: `gh api --method POST repos/<owner>/<repo>/issues/<child>/dependencies/blocked_by -F issue_id=<blocker-db-id>`. 不可用时在子 issue 正文顶部写 `Blocked by: #<n>, #<n>`.
- 统计未关闭的阻塞: `gh api repos/<owner>/<repo>/issues/<n> --jq .issue_dependencies_summary.blocked_by`; 0 表示可开工. `gh issue view <n> --json blockedBy` 会列出已关闭的阻塞, 需要过滤.
- 评论: `gh issue comment <number> --body "..."`.
- 加 / 删 label: `gh issue edit <number> --add-label "..."` / `--remove-label "..."`.
- 关闭: 先 `gh issue comment <number> --body "..."`, 再 `gh issue close <number>`. 对已关闭 issue, `gh issue close --comment` 会丢弃评论但仍退出 0.
- spec 正文只写问题, 方案, 决策与验收; "未提交 / 已提交 / 待 review" 这类临时状态写在评论里, 避免正文随流程过期.

`gh` 在仓库内会自动从 `git remote -v` 推断仓库. 只向自己的仓库提 issue; 在 fork 中 `gh` 可能默认指向 upstream parent, 首次 `gh issue create` 前用 `gh repo view --json nameWithOwner` 确认目标仓库.

## Pull requests as a triage surface

**PRs as a request surface: no.** (若本仓库把外部 PR 当作功能请求, 改为 `yes`; `/triage` 会读这个开关.)

设为 `yes` 时, PR 走与 issue 相同的 label 与状态:

- 读取 PR: `gh pr view <number>`, 再看 `gh pr view <number> --comments`; diff 用 `gh pr diff <number>`.
- 列出待 triage 的外部 PR: `gh api --paginate 'repos/{owner}/{repo}/pulls?state=open' --jq '.[] | select(.author_association | IN("OWNER","MEMBER","COLLABORATOR") | not) | {number, title, author: .user.login, author_association, labels: [.labels[].name]}'`.
- 评论 / 加 label / 关闭: `gh pr comment`, `gh pr edit --add-label` / `--remove-label`, `gh pr close`.

GitHub 的 issue 与 PR 共用编号空间, 裸 `#42` 可能是 PR: 先用 `gh pr view 42` 解析, 再回退到 `gh issue view 42`.

## 当 skill 说 "publish to the issue tracker"

创建一个 GitHub issue.

## 当 skill 说 "fetch the relevant ticket"

按上面的 "读取 issue" 读取.

## Wayfinding operations

供 `/wayfinder` 使用. map 是一个带 `wayfinder:map` label 的 issue, ticket 是它的子 issue.

- Map: `gh issue create --label wayfinder:map`, 正文保存 Notes / Decisions-so-far / Fog.
- 子 ticket: 用 GitHub sub-issue 链接到 map; 不可用时在 map 正文加 task list, 并在子 issue 顶部写 `Part of #<map>`. Label 用 `wayfinder:<type>` (`research` / `prototype` / `grilling` / `task`). 认领后指派给执行者.
- 阻塞: 见上面 "阻塞关系".
- 前沿查询: 列出 map 的未关闭子 issue, 去掉有未关闭阻塞或已有 assignee 的; 按 map 顺序取第一个.
- 认领: `gh issue edit <n> --add-assignee @me`, 这是本次会话的第一次写操作.
- 解决: `gh issue comment <n> --body "<answer>"`, 再 `gh issue close <n>`, 然后把 context 指针 (gist + 链接) 追加到 map 的 Decisions-so-far.
