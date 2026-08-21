# Verification

This document defines the verification required before automated agents report implementation work as complete.

Verification should be proportional to the affected scope and should use the checks already established by the repository.

## Core Rule

Do not report implementation work as complete without reviewing and verifying the resulting changes.

Never claim that a check passed if it was not actually run.

If a check cannot be performed in the current environment, report that limitation explicitly.

## Final Diff Review

Before completion, inspect the final diff.

Verify that:

- all changes are intentional;
- the requested behavior is implemented;
- no unrelated files were modified;
- existing user changes remain intact;
- no debugging code or temporary work remains;
- generated changes are intentional;
- the implementation does not exceed the requested scope.

Review the complete change, not only the most recently edited files.

## Project Checks

Run the relevant checks provided by the repository.

Depending on the project, these may include:

- build;
- unit tests;
- integration tests;
- end-to-end tests;
- type checking;
- linting;
- formatting checks;
- static analysis;
- project-specific validation commands.

Use existing project scripts and tooling where available.

Do not introduce new verification tooling solely to validate a routine change.

## Scope of Verification

Verification should match the affected scope.

A small isolated change may require only targeted checks.

A change affecting shared infrastructure, public interfaces, build configuration, or cross-cutting behavior may require broader verification.

Prefer the smallest set of checks that provides meaningful confidence, not the smallest set that merely executes quickly.

## Tests

Run tests relevant to the changed behavior.

When practical:

- run focused tests for the affected area first;
- run broader suites when the change can affect shared behavior;
- add or update tests when the repository normally tests the changed behavior.

Do not modify tests merely to make an incorrect implementation pass.

If an existing test fails, determine whether the failure is:

- caused by the current change;
- pre-existing;
- environment-specific;
- unrelated.

Do not silently ignore failures.

## Build

If the project has a build step relevant to the changed code, run it when practical.

Build failures introduced by the change must be resolved before completion.

If building is impossible because required tooling, dependencies, credentials, services, or platforms are unavailable, report that limitation.

## Linting, Formatting, and Static Checks

Run configured checks relevant to the affected files.

Prefer repository-defined commands over manually constructed equivalents.

Do not run broad auto-fix or formatting commands when they would modify unrelated files.

Verification commands should not create unnecessary diff noise.

## Manual Verification

When behavior cannot be meaningfully verified through automated checks alone, identify the relevant manual verification.

Perform it when the environment supports it.

If manual verification requires unavailable infrastructure, hardware, credentials, external services, or a specific platform, report it as not performed.

Do not imply that code inspection is equivalent to runtime verification.

## Self-Review

For substantial changes, review the final result as if reviewing another developer's pull request.

Check for:

- correctness;
- regressions;
- edge cases;
- incorrect assumptions;
- error-handling gaps;
- lifetime or resource-management issues;
- concurrency issues where relevant;
- accidental public API changes;
- unnecessary complexity;
- duplicated logic;
- dead code;
- missing tests;
- scope expansion.

Fix issues discovered during self-review before reporting completion.

For complex or high-risk work, an independent verification agent may be used according to `.agents/orchestration.md`.

## Failed Verification

If a verification step fails:

1. inspect the failure;
2. determine whether the current changes caused it;
3. fix failures caused by the current work;
4. rerun the relevant check.

Do not repeatedly modify unrelated code in an attempt to make an unrelated failure disappear.

If a failure is confirmed to be pre-existing or unrelated, preserve it and report it accurately.

## Environment Limitations

Do not work around missing environment capabilities by weakening verification requirements or fabricating results.

Examples include unavailable:

- operating systems;
- hardware;
- credentials;
- external services;
- SDKs;
- compilers;
- runtime dependencies.

State what could and could not be verified.

## Repository State

Before completion, inspect the final repository state.

At minimum, check:

```bash
git status
```

Confirm that:

- expected files are modified;
- unexpected files are not modified;
- staging state is intentional;
- unrelated user changes remain intact;
- no temporary files were accidentally introduced.

Git-specific rules remain defined in `.agents/git.md`.

## Verification Reporting

Report verification concisely and factually.

When useful, separate checks by status.

Example:

```text
Build: passed
Unit tests: passed
Lint: passed
Integration tests: not run — required service is unavailable
Manual verification: not run — required platform is unavailable
```

Do not report unavailable checks as successful.

Do not use broad claims such as:

```text
all tests pass
```

unless all relevant test suites were actually run and passed.

## Documentation-Only Changes

Documentation-only changes generally do not require application builds or runtime tests unless the documentation affects generated output or executable examples.

Still verify:

- formatting;
- links where practical;
- referenced paths, commands, and names;
- consistency with the current implementation.

## Configuration and Build Changes

Changes to build systems, CI, dependency configuration, packaging, or environment setup should be verified using the affected configuration where practical.

Do not consider a configuration change verified solely because its syntax appears correct.

## Completion

Implementation work is ready to report as complete when:

1. the final diff has been reviewed;
2. relevant available checks have passed;
3. failures caused by the change have been resolved;
4. unavailable verification has been identified;
5. repository state has been inspected;
6. no known task-blocking issue remains.

## Guiding Rule

Verification must reflect what was actually checked.

Prefer precise evidence over confidence statements.