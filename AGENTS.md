# Repository Agent Instructions

This file defines the top-level rules for automated coding agents working in this repository.

Detailed rules are maintained in focused documents under `.agents/`. Read the relevant instruction files before performing work.

## Priorities

Follow these priorities in order:

1. Correctness
2. Preserve existing user work
3. Respect the requested scope
4. Maintainability
5. Verification
6. Clean Git history
7. Simplicity

Prefer the smallest complete change that satisfies the task.

Do not implement speculative future requirements.

Do not perform unrelated cleanup, refactoring, formatting, dependency upgrades, or architectural changes unless required by the task.

Follow existing repository conventions before introducing new ones.

## Instruction Files

Detailed rules are organized as follows:

- `.agents/code-quality.md` — implementation quality, scope discipline, architecture, dependencies, naming, error handling, maintainability, and general healthy-code practices
- `.agents/commits.md` — commit structure, naming, scope, authorship, and atomicity
- `.agents/git.md` — branches, working-tree safety, staging, remote operations, and history preservation
- `.agents/orchestration.md` — subagent usage, delegation, ownership, parallelization, and orchestrator responsibilities
- `.agents/pull-requests.md` — pull request naming, creation, template usage, and PR workflow
- `.agents/verification.md` — builds, tests, checks, self-review, and completion verification

Pull requests must use:

- `.github/PULL_REQUEST_TEMPLATE.md`

These instruction files are part of the repository workflow and must be followed when relevant to the current task.

## Before Editing

Before modifying the repository:

1. inspect the current branch;
2. inspect the working tree;
3. identify existing uncommitted changes;
4. read the instruction files relevant to the task;
5. inspect the relevant code and existing architecture;
6. determine the minimum logical scope required.

At minimum, inspect:

```bash
git status
git branch --show-current
```

Treat unrelated user-authored changes as protected.

Never discard, overwrite, reset, revert, or unnecessarily reformat unrelated user work.

## Scope

Implement only what is required for the current task.

Do not introduce unrelated:

- refactors;
- cleanup;
- formatting changes;
- dependency upgrades;
- architectural redesign;
- speculative abstractions;
- future-facing functionality.

Prefer existing project patterns and direct solutions when they are sufficient.

See `.agents/code-quality.md`.

## Git Workflow

Every logically independent feature or unit of work must use its own branch.

Create a new branch whenever explicitly requested by the user.

Never perform implementation work directly on protected primary branches such as `main` or `master`.

Branch names must follow:

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

If there is no ticket ID, omit that segment completely.

Do not push automatically.

Do not perform destructive or history-rewriting Git operations unless explicitly requested.

See `.agents/git.md`.

## Commits

Every logically independent change or addition should be represented by its own commit.

Create a commit whenever explicitly requested by the user.

Commit messages must follow:

```text
<type>(<context>): <title>
```

Examples:

```text
feat(settings): add user preferences
fix(auth): handle expired sessions
refactor(api): simplify request handling
```

Commits must:

- contain one logical change;
- have exactly one author;
- use the repository/user's configured Git identity;
- contain no automated-agent co-author attribution;
- contain no generated contributor trailers;
- contain no commit body by default;
- not be pushed automatically.

Do not rewrite existing commits unless explicitly requested.

See `.agents/commits.md`.

## Pull Requests

Create a pull request only when explicitly requested by the user.

Pull request titles must follow:

```text
<type>(<context>): [<optional_ticket_id>] <title>
```

The ticket segment is optional and must be omitted completely when no ticket exists.

Pull requests must strictly use the current repository template:

```text
.github/PULL_REQUEST_TEMPLATE.md
```

Do not replace or restructure the template.

A PR request authorizes only the minimum remote operations required to publish the requested pull request. It does not authorize merging or unrelated remote changes.

See `.agents/pull-requests.md`.

## Agent Orchestration

Do not use subagents by default.

Use them when independent research, implementation, or verification materially improves the task.

Prefer:

```text
parallel research
→ orchestrator decision
→ controlled implementation
→ independent verification
```

Never assign overlapping write scopes to multiple agents simultaneously.

The main agent remains responsible for architecture, integration, scope, verification, and Git discipline.

See `.agents/orchestration.md`.

## Verification

Before reporting implementation work as complete:

1. inspect the final diff;
2. verify that no unrelated changes were introduced;
3. run relevant builds, tests, and configured checks where possible;
4. inspect the final repository state;
5. review substantial changes as if reviewing another developer's pull request.

Never claim that a check passed if it was not run.

Report unavailable verification explicitly.

See `.agents/verification.md`.

## Git Authorization Boundaries

Treat Git operations as separate permissions.

Implementation does not automatically authorize a commit.

A commit does not authorize a push.

A push does not authorize a pull request.

A pull request does not authorize a merge.

History rewriting, force-pushing, branch deletion, tagging, releasing, and merging require explicit authorization unless a repository-specific workflow explicitly states otherwise.

## Guiding Rule

Understand the existing repository, make the smallest correct change, preserve user work, verify the result, and perform only the Git and remote operations authorized by the user.