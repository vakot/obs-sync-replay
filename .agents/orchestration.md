# Agent Orchestration

This document defines how automated agents should delegate work and coordinate subagents.

The goal is to use additional agents only when they improve correctness, speed, or independent verification without creating unnecessary coordination overhead.

## Default Workflow

Do not use subagents by default.

For small and well-scoped tasks, prefer:

```text
inspect
→ implement
→ verify
```

Use subagents only when the task contains meaningful independent work that benefits from delegation.

## When to Delegate

Delegation is appropriate for work such as:

- researching independent APIs or technologies;
- investigating separate parts of a codebase;
- comparing multiple implementation approaches;
- analyzing independent failures;
- performing an independent review of completed work;
- implementing clearly separated, non-overlapping subsystems.

Do not delegate trivial work merely because subagents are available.

The coordination cost should be justified by the task.

## Parallelization Strategy

Prefer parallelizing independent research and analysis.

Recommended:

```text
parallel research
        ↓
orchestrator decision
        ↓
implementation
        ↓
independent verification
```

For example:

```text
Agent A → investigate external API
Agent B → inspect existing project architecture
Agent C → research platform constraints

                ↓

Orchestrator → combine findings and choose approach

                ↓

Implementation

                ↓

Independent review
```

Do not ask multiple agents to independently implement competing solutions unless comparison of implementations is explicitly useful.

## Write Ownership

Never assign overlapping write scopes to multiple agents at the same time.

Avoid:

```text
Agent A → modify src/service/**
Agent B → modify src/service/**
```

Prefer:

```text
Agent A → research only
Agent B → research only

Orchestrator → decide implementation

Agent C → modify src/service/**
```

Parallel implementation is acceptable only when ownership is clearly separated.

Example:

```text
Agent A → modify src/client/**
Agent B → modify src/server/**
```

The scopes must be independent enough that neither agent needs to modify the other's files or assumptions during execution.

## Delegated Task Definition

Every delegated task should clearly define:

- goal;
- relevant context;
- read scope;
- write scope;
- constraints;
- expected output.

Example:

```text
Goal:
Determine how the existing authentication flow handles expired sessions.

Context:
A new refresh mechanism is being added and must preserve current behavior.

Read scope:
src/auth/**
tests/auth/**

Write scope:
none

Constraints:
Do not modify repository files.
Focus only on current session expiration behavior.

Expected output:
Summarize the current flow, relevant entry points, and constraints the implementation must preserve.
```

Do not delegate vague instructions such as:

```text
Look into authentication.
```

## Research Agents

Research agents should normally be read-only.

Use them to:

- inspect existing code;
- locate relevant entry points;
- verify external API behavior;
- identify constraints;
- compare implementation options;
- investigate failures.

Research output should provide findings and recommendations, not unrelated implementation.

The orchestrator must evaluate the findings before using them.

## Implementation Agents

Delegate implementation only when the scope is sufficiently clear.

An implementation task should define:

- files or subsystem owned by the agent;
- expected behavior;
- interfaces that must be preserved;
- relevant constraints;
- required verification.

Do not allow implementation agents to expand scope independently.

If an agent discovers work outside its assigned scope, it should report the finding rather than automatically implementing unrelated changes.

## Verification Agents

For substantial or high-risk changes, an independent verification pass is preferred when practical.

A verification agent should review the result as if reviewing another developer's pull request.

Focus on:

- correctness;
- regressions;
- edge cases;
- error handling;
- concurrency or lifetime issues where relevant;
- unnecessary complexity;
- missing tests;
- accidental scope expansion.

Verification agents should not rewrite the implementation unless explicitly assigned a write scope.

They should report findings to the orchestrator.

## Orchestrator Responsibility

The main agent remains responsible for the complete result.

Delegation does not transfer responsibility for:

- architecture decisions;
- correctness;
- integration;
- scope control;
- repository conventions;
- Git discipline;
- final verification.

The orchestrator must review delegated findings and changes before considering them complete.

Do not accept subagent output blindly.

## Conflicting Findings

When agents produce conflicting recommendations:

1. compare the evidence;
2. inspect the relevant source or documentation directly when necessary;
3. choose the approach that best satisfies the current requirements;
4. do not combine incompatible approaches merely to preserve both recommendations.

The orchestrator owns the final decision.

## Context Sharing

Provide delegated agents with the context required for their task, but avoid unnecessary information.

Include:

- the concrete goal;
- relevant constraints;
- known architectural decisions;
- relevant paths or interfaces.

Do not rely on an agent inferring critical requirements that were never included in its task.

## Scope Expansion

Subagents must not independently expand the requested scope.

If additional work is discovered:

```text
discover
→ report
→ orchestrator evaluates
→ implement only if required
```

Do not turn nearby cleanup, refactoring, or unrelated defects into delegated work automatically.

## Git Operations

Subagents should not perform independent Git workflow operations unless explicitly assigned.

By default, delegated agents must not:

- create branches;
- create commits;
- push;
- rewrite history;
- create pull requests;
- merge.

Git workflow remains controlled by the orchestrator and must follow `.agents/git.md` and `.agents/commits.md`.

## Completion

Before accepting delegated work, the orchestrator should verify:

- the requested output was produced;
- the assigned scope was respected;
- no overlapping or unrelated changes were introduced;
- findings are supported by evidence;
- implementation integrates correctly with the rest of the task.

Final task completion still requires the verification defined in `.agents/verification.md`.

## Guiding Rule

Use the fewest agents necessary to complete the task reliably.

Parallelize independent work, keep write ownership explicit, and preserve one clear orchestrator responsible for the final result.