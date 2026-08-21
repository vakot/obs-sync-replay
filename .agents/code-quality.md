# Code Quality

This document defines repository-wide code quality expectations for automated agents.

These rules are intentionally language-, framework-, and platform-agnostic.

## Core Principles

Prefer code that is:

- correct;
- simple;
- readable;
- maintainable;
- explicit where ambiguity would be harmful;
- consistent with the existing codebase;
- appropriately tested.

Do not optimize for cleverness.

Do not introduce abstractions, patterns, dependencies, or architectural layers without a current need.

Prefer the smallest complete solution that satisfies the task.

## Scope Discipline

Keep changes focused on the requested task.

Do not perform unrelated:

- refactors;
- cleanup;
- renaming;
- formatting;
- dependency upgrades;
- architectural redesign;
- API redesign;
- speculative future work.

If nearby code could be improved but is not required for the task, leave it unchanged unless the user explicitly asks for it.

## Existing Conventions

Inspect and follow existing repository conventions before introducing new ones.

This includes:

- naming;
- file organization;
- module boundaries;
- formatting;
- error handling;
- testing patterns;
- logging;
- dependency usage;
- public API style.

Prefer consistency with the codebase over personal stylistic preference.

Introduce a new convention only when the existing codebase does not provide one or when the task explicitly requires a change.

## Simplicity

Prefer straightforward implementations over unnecessarily generic ones.

Avoid speculative abstractions such as:

- generic frameworks for one concrete use case;
- unnecessary factories;
- unnecessary service locators;
- dependency injection without a clear need;
- custom event buses where direct calls are sufficient;
- wrapper layers that add no meaningful boundary;
- configuration systems for fixed behavior;
- extensibility points with no current consumer.

A useful abstraction should reduce real duplication, isolate a meaningful responsibility, or represent a stable boundary.

Do not create abstractions solely because they might become useful later.

## Separation of Responsibilities

Keep responsibilities clear and appropriately scoped.

A component should not need unrelated knowledge to perform its role.

Avoid mixing concerns such as:

- domain logic with platform-specific details;
- persistence with presentation;
- parsing with unrelated business logic;
- transport concerns with core logic;
- configuration loading with runtime behavior.

Do not over-separate trivial logic into unnecessary layers.

The goal is clear boundaries, not maximum file count.

## Functions and Methods

Functions should have a clear purpose.

Prefer:

- focused responsibilities;
- clear inputs and outputs;
- predictable side effects;
- early handling of invalid or exceptional states where appropriate.

Avoid:

- functions that perform many unrelated operations;
- hidden state changes;
- unnecessary mutation;
- excessive parameter lists caused by poor responsibility boundaries;
- boolean parameters whose meaning is unclear at the call site.

Split functions when doing so makes behavior easier to understand, test, or reuse.

Do not split code mechanically when the result is harder to follow.

## Naming

Names should communicate intent.

Prefer names that describe:

- what a value represents;
- what a function does;
- what responsibility a type or module owns.

Avoid:

- ambiguous abbreviations;
- generic names such as `data`, `manager`, `helper`, or `utils` when a more specific name is available;
- names tied to incidental implementation details when the concept is broader;
- misleading names that no longer match behavior.

Do not rename established concepts without a concrete reason.

## Data and State

Keep state ownership clear.

Prefer local state where possible.

Avoid unnecessary shared or global mutable state.

When state must be shared:

- define who owns it;
- define who may modify it;
- make lifetime expectations clear;
- avoid multiple competing sources of truth.

Do not cache or duplicate state unless there is a concrete reason.

## Control Flow

Prefer control flow that is easy to follow.

Avoid excessive:

- nesting;
- branching;
- implicit fallthrough;
- hidden callbacks;
- state-machine behavior without clear representation.

Use guard clauses or decomposition when they improve readability.

Do not compress logic into dense expressions merely to reduce line count.

## Error Handling

Handle expected failure modes deliberately.

Errors should be:

- propagated;
- returned;
- logged;
- converted;
- or safely ignored

according to the responsibility of the current layer.

Do not silently swallow meaningful failures.

Do not turn recoverable errors into fatal failures without reason.

Do not use exceptions, error codes, sentinel values, or logging inconsistently with the surrounding codebase.

Preserve useful context when forwarding errors.

## Validation

Validate inputs at the boundary where invalid data can enter the system.

Do not repeatedly validate the same invariant throughout internal code once it has been established.

Validation should be proportional to the trust boundary.

Avoid accepting invalid state and compensating for it later with scattered defensive checks.

## Comments

Prefer self-explanatory code over comments that restate implementation.

Comments are useful for explaining:

- non-obvious constraints;
- important invariants;
- platform or API limitations;
- intentional tradeoffs;
- reasons behind unusual behavior.

Avoid comments that merely translate code into prose.

Keep comments accurate when behavior changes.

## TODOs

Do not add speculative TODOs.

A TODO should represent a concrete known follow-up.

When the repository has a ticketing workflow, reference the relevant ticket where practical.

Do not leave TODOs for behavior required by the current task.

## Duplication

Avoid unnecessary duplication, but do not abstract too early.

Small duplication is preferable to a premature abstraction that incorrectly couples unrelated behavior.

Extract shared logic when the duplicated behavior is genuinely the same responsibility and likely to remain aligned.

## APIs and Interfaces

Keep public interfaces minimal.

Do not expose internal implementation details without a requirement.

Prefer stable, intention-revealing interfaces.

Avoid expanding public APIs "just in case."

When changing an existing public interface:

- preserve compatibility unless the task requires otherwise;
- consider existing callers;
- update affected tests and documentation;
- surface breaking changes explicitly.

## Dependencies

Do not add dependencies unless they provide clear value that cannot be reasonably achieved with existing project capabilities.

Before adding a dependency, consider:

- existing repository dependencies;
- standard platform capabilities;
- maintenance cost;
- security implications;
- build impact;
- runtime impact.

Do not upgrade unrelated dependencies as part of another task.

Do not replace an established dependency merely because another option is preferred.

## Performance

Do not optimize without a reason.

Prefer clear correct code first.

Optimize when:

- performance is part of the requirement;
- profiling or evidence identifies a meaningful bottleneck;
- the cost is obvious and avoidable.

Avoid premature caching, batching, concurrency, memoization, or low-level optimization.

When optimizing, preserve readability where practical and verify the improvement when possible.

## Concurrency

Introduce concurrency only when it provides a concrete benefit or is required by the environment.

Keep ownership and synchronization explicit.

Avoid:

- shared mutable state without synchronization;
- unnecessary background workers;
- races introduced by callbacks;
- holding locks across slow or external operations;
- concurrency used only to appear faster.

Follow the repository's existing concurrency model.

## Resource Management

Resources should have clear ownership and lifetime.

This applies to any resource that requires cleanup, including:

- files;
- sockets;
- processes;
- handles;
- subscriptions;
- callbacks;
- timers;
- memory;
- database connections;
- temporary artifacts.

Release resources deterministically where the platform and language support it.

Ensure failure paths do not leak resources or leave invalid registrations behind.

## Security and Privacy

Do not introduce avoidable security risks.

Never hardcode or commit:

- credentials;
- tokens;
- private keys;
- secrets;
- sensitive user data.

Do not log sensitive values.

Validate untrusted input before using it in security-sensitive operations.

Prefer established security mechanisms over custom implementations.

## Logging

Use the repository's existing logging mechanism.

Logs should provide meaningful operational or debugging value.

Avoid:

- noisy logs in hot paths;
- duplicate messages;
- logging normal control flow as errors;
- exposing sensitive information;
- adding a second logging framework without need.

Use appropriate severity levels when the logging system supports them.

## Formatting

Follow the repository's established formatter and style configuration.

Do not manually impose a different style.

Avoid broad formatting changes unrelated to the task.

When automatic formatting is required, apply it only to relevant files where practical.

## Generated Files

Do not manually edit generated files unless the repository explicitly expects it.

Modify the source of generation and regenerate using the established workflow.

Do not regenerate unrelated files.

Generated output should remain deterministic where the existing tooling supports it.

## Tests

Code should remain testable.

When behavior changes, add or update tests when the repository normally covers that behavior.

Prefer tests that verify externally meaningful behavior over implementation details.

Avoid brittle tests that depend unnecessarily on:

- internal call order;
- private structure;
- timing;
- incidental formatting;
- unrelated implementation choices.

Do not weaken valid tests simply to make new code pass.

Verification details are defined in `.agents/verification.md`.

## Backward Compatibility

Preserve existing behavior unless the task explicitly changes it.

Do not introduce breaking changes incidentally.

If a breaking change is required:

- make it intentional;
- update affected callers;
- update documentation where relevant;
- report it clearly.

## Dead Code

Do not leave:

- unused implementations;
- abandoned branches of logic;
- temporary debug code;
- commented-out code;
- obsolete compatibility paths

unless they serve a documented current purpose.

Prefer deleting code that is no longer needed when that deletion is clearly within the task scope.

## Configuration

Do not introduce configuration for behavior that is intentionally fixed.

Configuration adds maintenance and validation cost.

Add configurable behavior only when there is a concrete requirement for variation.

Do not create placeholder configuration keys for hypothetical future features.

## Extensibility

Do not design for undefined future consumers.

Extensibility should emerge from real requirements.

Prefer:

```text
current requirement
→ clear implementation
→ future requirement
→ refactor when evidence exists
```

over:

```text
current requirement
→ generic framework for hypothetical future cases
```

## Reviewability

Changes should be easy for another developer to understand and review.

Prefer:

- focused diffs;
- predictable structure;
- clear names;
- minimal unrelated churn;
- logical commits;
- explicit behavior.

Avoid hiding substantial behavior changes inside broad cleanup.

## Guiding Rule

Write code for the requirements that exist now, in the style of the repository that already exists.

Prefer clear, correct, focused code over clever, generic, or speculative code.