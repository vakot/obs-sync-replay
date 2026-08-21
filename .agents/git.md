# Git Workflow

This document defines the Git workflow for automated agents working in this repository.

## Before Starting

Before modifying the repository, inspect:

```bash
git status
git branch --show-current
```

Understand the current branch and existing working-tree changes before editing.

Treat unrelated user-authored changes as protected. Never discard, overwrite, reset, revert, or unnecessarily reformat them.

## Branches

Every logically independent feature or unit of work must use a separate branch.

Create a new branch when:

* starting a logically independent feature, fix, refactor, or other unit of work;
* the user explicitly requests a separate branch.

Do not create another branch for follow-up work that belongs to the same logical task.

Do not perform implementation work directly on protected primary branches such as `main` or `master`.

### Branch Naming

Use:

```text
<type>/<username>/<optional_ticket_id>/<title>
```

Examples without a ticket:

```text
feature/johndoe/user-preferences
fix/johndoe/session-expiration
refactor/johndoe/request-handler
```

Examples with a ticket:

```text
feature/johndoe/PROJ-142/user-preferences
fix/johndoe/PROJ-231/session-expiration
```

If there is no ticket, omit the segment completely.

Use the GitHub username of the person whose development workflow the branch belongs to. Do not use an automated-agent identity such as `codex`, `chatgpt`, or `bot`.

Preferred branch types:

```text
feature
fix
refactor
test
docs
chore
build
ci
perf
```

The title must use concise lowercase kebab-case.

## Working Tree

Keep repository changes scoped to the current task.

Do not modify unrelated files merely to clean them up or make them consistent with new code.

When unrelated changes exist, stage only the files or hunks belonging to the intended commit.

Before committing, inspect the relevant working-tree and staged diffs.

Do not use broad staging such as `git add .` or `git add -A` unless the entire working tree is known to belong to the intended change.

## Preserve Repository State

Do not perform destructive or history-rewriting Git operations unless explicitly requested by the user.

This includes operations that:

* discard working-tree changes;
* reset existing commits;
* amend existing commits;
* squash, reorder, or drop commits;
* rebase existing history;
* force-push;
* delete branches or tags.

If committed work requires a correction, create a new commit unless the user explicitly requests history cleanup.

## Remote Operations

Do not push automatically.

A request to implement, fix, refactor, create a branch, or create commits does not authorize a push.

Push only when explicitly requested or when the user explicitly requests creation of a pull request and publishing the branch is required to create it.

When pushing, modify only the remote state necessary for the requested operation.

Do not merge, delete remote branches, create tags, or publish releases unless explicitly requested.

## Pull Requests

Create a pull request only when explicitly requested.

A PR request authorizes the minimum push required to publish the requested branch.

Before creating a PR:

1. verify the final branch state;
2. verify the intended commits;
3. run the required project verification;
4. read the current template under `.github`;
5. follow `.agents/pull-requests.md`.

The repository PR template is authoritative.

## Commits

Commit naming, authorship, scope, and atomicity are defined in:

```text
.agents/commits.md
```

Git operations must preserve those rules.

In particular, never include unrelated user changes in a commit.

## Final Check

Before reporting Git-related work complete, inspect:

```bash
git status
```

Confirm that:

* the current branch is correct;
* unrelated user changes remain intact;
* staged and unstaged changes are intentional;
* no unauthorized push occurred;
* no history was rewritten without explicit instruction.

## Guiding Rule

Perform only the Git operations necessary for the user's request.

When an operation is unnecessary, destructive, history-rewriting, or changes remote repository state, leave it alone unless explicitly requested.
