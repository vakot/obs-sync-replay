# Pull Request Guidelines

This document defines how automated agents create and populate pull requests for this repository.

## Creation Policy

Create a pull request only when explicitly requested by the user.

A request to implement, commit, or push changes does not automatically authorize PR creation.

When a PR is requested:

1. verify the current branch;
2. verify the intended commits;
3. run relevant verification;
4. push only the branch required for the PR;
5. read the current PR template under `.github`;
6. populate that template strictly;
7. create the PR without replacing or restructuring the repository-defined template.

The template under `.github` is authoritative.

## Pull Request Title

Use the same type, context, and title conventions defined in `.agents/commits.md`.

Format:

```text
<type>(<context>): [<optional_ticket_id>] <title>
```

Examples with a ticket:

```text
feat(settings): [PROJ-123] add notification preferences
fix(auth): [PROJ-456] handle expired sessions
```

Examples without a ticket:

```text
feat(settings): add notification preferences
fix(auth): handle expired sessions
```

The ticket segment is optional. If no ticket exists, omit the entire bracketed segment.

Do not invent ticket IDs.

The PR title should describe the complete logical change represented by the branch and does not need to match any individual commit title.

## Pull Request Template

The expected repository template is:

```markdown
# Overview

<--list of related links, ticket link, upstream PRs, etc-->

<--description-->

### 🛠️ Testing Steps

<--START: testing section-->
#### Prerequisites (optional)

- list of prerequisites (optional)

#### Optional (required if > 1 sections): testing section title-->

- [ ] Testing step 1
- [ ] Testing step 2
<--END: testing section-->
```

Always read the actual repository template before creating a PR in case it has changed.

Do not replace it with an agent-generated format.

Do not remove or rename required sections.

Template comments are instructions and placeholders. Replace or remove them as appropriate when preparing the final PR body.

## Overview

The `# Overview` section should contain:

1. related links when available;
2. ticket links when available;
3. upstream, dependent, or related PR links when relevant;
4. a concise description of the change.

Do not invent links, tickets, or related work.

The description should explain:

* what changed;
* why the change was needed;
* important architectural decisions when they materially help review.

Do not reproduce the complete commit history.

Avoid excessive implementation detail that does not help review the change.

## Testing Steps

Testing steps are instructions for the reviewer to verify the change.

Use unchecked Markdown checkboxes:

```markdown
- [ ] Open the affected screen.
- [ ] Perform the updated action.
- [ ] Verify the expected result.
```

Do not mark reviewer testing steps as completed when creating the PR.

Use:

```markdown
- [ ]
```

not:

```markdown
- [x]
```

Testing steps should be concrete, reproducible, and ordered where sequence matters.

## Testing Structure

For a single straightforward testing flow, keep the testing section simple.

Use `Prerequisites` only when setup is actually required.

Example:

```markdown
### 🛠️ Testing Steps

#### Prerequisites

- Configure the required local environment.

- [ ] Start the application.
- [ ] Navigate to the affected area.
- [ ] Perform the updated action.
- [ ] Verify the expected result.
```

When more than one independent testing flow exists, divide them into clearly named sections.

Example:

```markdown
### 🛠️ Testing Steps

#### Primary flow

- [ ] Open the affected screen.
- [ ] Perform the primary action.
- [ ] Verify the expected result.

#### Error handling

- [ ] Trigger the relevant failure condition.
- [ ] Verify the expected error behavior.
```

Do not create unnecessary testing subsections for a simple change.

## Scope

A pull request should represent one logical unit of work.

Do not combine unrelated:

* features;
* fixes;
* refactors;
* cleanup;
* dependency updates;
* formatting changes

into the same PR.

Before creating the PR, verify that the branch contains only changes relevant to its intended scope.

## Verification Before Creation

Before creating a pull request:

1. inspect the final diff;
2. verify that the branch contains only intended changes;
3. run the relevant build, tests, and configured checks where possible;
4. inspect the final repository state;
5. confirm the correct head branch;
6. confirm the correct base branch;
7. push the required branch;
8. read the current `.github` PR template;
9. prepare the PR body from that template.

Do not claim verification that was not performed.

If an important check cannot be performed in the current environment, mention the limitation in the PR description when it is relevant to reviewers.

## Base and Head Branches

Use the task branch as the PR head.

Use the repository's intended integration branch as the base.

Do not retarget the work to another base branch without user instruction or a clear repository-specific requirement.

Do not include unrelated branches or commits.

## Draft State

If the user explicitly requests a draft PR, create a draft.

If the user explicitly requests a ready-for-review PR, create it as ready.

If the user does not specify a state, follow the repository's established workflow rather than inventing a new policy.

## Post-Creation Actions

Creating a PR does not authorize additional PR management.

Do not automatically:

* merge the PR;
* enable auto-merge;
* close the PR;
* change draft/ready state;
* request reviewers;
* add or remove labels;
* modify milestones;
* modify projects;
* rewrite the PR title or body after creation;
* delete the remote branch.

Perform these actions only when explicitly requested or required by repository-specific instructions.

## Git Operations

A PR request authorizes the minimum remote operations required to create the requested pull request.

This includes pushing the required task branch when necessary.

It does not authorize:

* unrelated pushes;
* force-pushes;
* history rewriting;
* merging;
* remote branch cleanup.

Follow `.agents/git.md` for all Git operations.

## Guiding Rule

When the user requests a pull request, create exactly the requested PR using the repository's current template, established naming conventions, and only the remote operations necessary to publish it.
