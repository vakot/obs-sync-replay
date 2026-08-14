# Git Workflow

This policy is mandatory for Codex and every other coding agent working in this
repository unless the user explicitly overrides it for a specific task. Repository
history should communicate the architecture through small, coherent review units.

## One Logical Change per Branch and Pull Request

Every independently reviewable feature, fix, refactor, test improvement,
documentation change, or infrastructure change gets its own branch and pull request:

```text
one logical change
      -> one branch
      -> one pull request
      -> one squash commit on master
```

Do not create umbrella branches or mix unrelated work. Initial plugin scaffolding,
the master-frame coordinator, scene-selection UI, encoder integration, synchronized
replay buffering, replay validation, and logging improvements are normally separate
changes. If work splits naturally, create separate branches and PRs. Avoid long-lived
dependent branch chains; prefer merging PR A, updating `master`, and then branching
for B. If B must depend on an unmerged A, state that dependency in B's PR and rebase B
onto updated `master` after A merges.

## Branch Names

Use exactly:

```text
<type>/vakot/<optional_ticket_id>/<description>
```

`vakot` is the project author identifier and is required. Omit the ticket segment
entirely when there is no ticket; do not create empty path segments. Choose the
narrowest accurate type:

```text
feature  fix  refactor  test  docs  chore  build  ci  perf
```

The description must be concise lowercase kebab-case, describe one logical change,
and avoid vague terms such as `updates`, `changes`, `work`, `misc`, `fixes`, or
`new-stuff`. A ticket ID may retain its canonical case.

```text
# Valid
feature/vakot/initial-setup
feature/vakot/OBS-142/shared-replay-buffer
fix/vakot/encoder-pts-mismatch
refactor/vakot/render-pipeline
test/vakot/sync-stress-tests
docs/vakot/architecture-rules
chore/vakot/cmake-cleanup

# Invalid
feature/vakot//initial-setup
feature/initial-setup
vakot/feature/initial-setup
feature/vakot/bunch-of-changes
```

## Protected `master`

Treat `master` as the protected integration/release branch. Before repository
changes, verify the current branch, bring local `master` up to date, and create the
dedicated task branch. Never perform feature, fix, refactor, test, documentation, or
infrastructure work directly on `master`.

The only permitted path to `master` is:

```text
task branch -> open PR -> review and CI -> Squash and Merge -> master
```

Direct commits and pushes to `master` are forbidden. Ordinary merge commits and
rebase-and-merge into `master` are forbidden. The required strategy is **Squash and
Merge** from an open pull request. Agents may open a requested PR. They must not merge
it unless the user explicitly requests that action, and then may use only Squash and
Merge.

If the working directory is not a Git worktree, stop before implementation and
report that prerequisite. Do not initialize or clone a repository without user
authorization.

## Commits

Every commit, including the final squash commit, uses:

```text
<type>(<context>): <description>
```

Use the same types as branch names. The project spelling is `feature`, never `feat`.
The context is a short, stable subsystem name, preferably one of:

```text
plugin  sync  timeline  render  encoder  replay  muxer  validation
ui      config  audio  build     ci      docs
```

Avoid filenames as contexts unless the file itself is the subsystem. Write the
description in concise imperative style, state what the commit does, omit the trailing
period, and avoid vague wording.

```text
feature(sync): add master frame coordinator
feature(render): render scenes from the shared frame tick
fix(replay): use shared replay frame boundaries
refactor(encoder): isolate encoder state ownership
test(sync): cover dropped-frame timeline preservation
docs(architecture): document the shared timeline invariant
chore(build): simplify Windows CMake configuration
```

Each commit should answer one question: what single logical modification does this
introduce? Keep commits compact, coherent, reviewable, and buildable where practical.
Do not create meaningless micro-commits. Do not combine implementation with unrelated
refactoring, formatting, documentation rewrites, or cleanup. Put discovered unrelated
work on another logical branch.

Order commits by dependency so history can be reviewed forward, for example data
model, coordinator, render integration, then tests. Do not hide dependencies by
committing them in a confusing reverse order.

## Before Each Commit

1. Inspect the working-tree and staged diffs.
2. Confirm that only intended files and hunks are included.
3. Remove accidental debug/generated changes.
4. Confirm the commit represents one coherent modification.
5. Build or run relevant validation where practical.
6. Use the required commit format.

Stage intentionally by path or hunk. Do not blindly run `git add .`.

## Pull Requests

Before a PR is ready, confirm:

- the branch contains only its intended logical work;
- commit messages follow the project format;
- the working tree is clean;
- relevant builds and tests pass;
- synchronization-critical work has the validation required by `AGENTS.md` and
  [`testing.md`](testing.md);
- no accidental generated or debug files are included;
- the branch is reasonably current with `master`.

### Pull Request Titles

Every pull request title uses exactly:

```text
<type>(<context>): [<optional_ticket_id>] <title>
```

`type` and `context` are mandatory. Use the established type vocabulary from branch
names and commits where applicable:

```text
feature  fix  refactor  test  docs  chore  build  ci  perf
```

The ticket ID is optional. When present, it must be wrapped in square brackets and
follow the colon before the title. The title must be concise, imperative or
descriptive, and represent the complete PR scope.

```text
feature(sync): add master frame coordinator
fix(timeline): handle runtime video timing changes
refactor(structure): group components into directories
feature(rendering): [OBS-123] add dual scene renderer
```

A PR title does not need to match any individual commit message; it describes the
complete change that will be squashed into `master`. **Squash and Merge** remains the
only allowed merge strategy into `master`.

The PR description must explain what changed, why it changed, how it was validated,
and whether synchronization-critical invariants were affected. For a sensitive
change, name the invariant explicitly, for example:

```text
Synchronization impact:
Both scene outputs continue to derive frame ID and PTS from the same master frame
coordinator.
```

A branch may contain several coherent development commits. When merging the PR, use
**Squash and Merge** so `master` receives one logical commit. The squash commit follows
the normal format and represents the PR as a whole, for example:

```text
feature(replay): add synchronized replay buffer
```

## Updating Branches

When a task branch needs current `master`, prefer rebasing when safe:

```text
git fetch origin
git rebase origin/master
```

Do not repeatedly merge `master` into the branch without reason. Rebasing is preferred
for autonomous, unshared agent branches. Do not rewrite the history of a shared branch
when that would disrupt collaborators.

Never discard user-authored work. Inspect unexpected modifications before proceeding.
Do not casually use destructive commands such as:

```text
git reset --hard
git clean -fd
git push --force
```

Use them only when genuinely necessary, consequences are understood, and the action
is explicitly justified and authorized.

## Repository Protection Expectations

During repository/bootstrap setup, configure the hosting platform so `master`:

- requires a pull request before merge;
- blocks direct pushes;
- requires CI checks after those checks exist;
- permits or requires Squash and Merge;
- disables merge commits and rebase merging;
- automatically deletes merged task branches where appropriate.

Do not invent required check names before CI exists. Record the actual check names
when the CI workflow is implemented, and keep protection settings aligned with them.

## Future CI Contract

The later bootstrap task should establish this pull-request path:

```text
pull request -> Windows build -> automated tests -> required checks
             -> eligible for Squash and Merge
```

CI must compile the relevant C++ targets and run the available automated tests; code
is not complete merely because static inspection suggests it should build. Exact
runner images, toolchain/dependency versions, cache strategy, workflow files, and
required check names must be selected and documented during bootstrap rather than
guessed here. See [`building.md`](building.md) for the intended local environment.

## Agent Procedures

At the start of every implementation task:

1. Read `AGENTS.md` and its relevant linked documents.
2. Inspect `git status` and the current branch.
3. Identify the single logical scope of the request.
4. Select the narrowest branch type and identify any ticket ID.
5. Update local `master` and create the compliant branch if necessary.
6. Create an implementation-specific plan when the task is non-trivial.
7. Begin work only after the branch is ready.

At completion, report:

```text
Branch:
feature/vakot/master-frame-coordinator

Commits:
- feature(sync): add master frame data model
- feature(sync): add master frame coordinator
- test(sync): cover deterministic frame sequence

Validation:
- Windows build passed
- synchronization tests passed

PR:
ready to open
```

Create the pull request when the user requested publication and the environment
supports it. Do not merge it unless the user explicitly requests it; if requested,
use only the approved Squash and Merge path.
