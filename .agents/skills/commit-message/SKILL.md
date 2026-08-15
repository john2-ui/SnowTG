---
name: commit-message
description: 根据 DPDK-L 仓库改动生成或审查中文 Conventional Commit 提交说明。用于撰写、修改或检查 commit message，准备 git commit，或把暂存区 diff、工作区 diff、变更摘要整理为提交信息；除非用户明确要求，否则不要执行提交。
---

# Commit Message

## 生成流程

1. 优先检查 `git status --short`、`git diff --cached --stat` 和 `git diff --cached`，以暂存区内容为提交边界。
2. 暂存区为空时，只在用户要求基于未暂存改动生成说明的情况下检查 `git diff`，并明确说明依据的是未暂存改动。
3. 必要时检查近期提交主题，并以提交 `ae1707c` 的风格为基准。
4. 判断改动是否只包含一个逻辑主题。若包含多个独立主题，先建议拆分，再分别给出提交说明；不要把例如 UDP 短读修复与 DNS 场景接入写进同一提交。
5. 选择能够表达改动语义的 `type` 和组件边界明确的 `scope`，然后生成中文摘要与正文条目。
6. 输出提交说明供用户确认。只有用户明确要求时才执行 `git commit`。

## 格式

严格使用以下结构：

```text
<type>(<scope>): <中文摘要>

- <变更要点 1>
- <变更要点 2>

[可选 trailer]
```

## 约束

- 使用 Conventional Commits 首行：`type(scope): 摘要`。
- 优先从 `feat`、`fix`、`test`、`docs`、`refactor`、`chore` 中选择 `type`；根据实际语义选择，不要仅按文件类型判断。
- 使用改动所属组件或边界作为 `scope`，例如 `traffic-gen`、`stack`、`socket`。
- 使用中文撰写摘要与正文。让摘要简洁说明“做了什么”以及必要时的“为什么”，不要只罗列文件名。
- 在首行后留一个空行。正文使用 `-` 列表，每条一行，写清关键行为、边界、不变量或验证影响。
- 让正文补充首行而非机械重复首行；只保留与本次逻辑主题直接相关的要点。
- 仅当 Cursor 实际参与该提交时，保留 `Co-authored-by: Cursor <cursoragent@cursor.com>`；不要凭空添加该署名或 Codex 署名。
- 在正文与 trailer 之间留一个空行。

## 示例

```text
docs(traffic-gen): 补充 owner reactor 架构与实施路线

- 记录流量发生器的分期目标、事件语义、容量约束和验证口径。
- 同步更新协议栈目录边界、构建入口及 owner-local I/O 使用说明。

Co-authored-by: Cursor <cursoragent@cursor.com>
```

## 输出检查

提交说明交付前确认：

- 首行包含合法的 `type`、明确的 `scope` 和中文摘要。
- 正文至少覆盖关键行为或约束，没有无关改动。
- 整体只描述一个逻辑主题；需要拆分时给出多个独立候选。
- trailer 与实际贡献者一致。
