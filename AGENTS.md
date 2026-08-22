# Agent Instructions

These instructions apply to the entire repository. The project is in the
documentation phase; do not bootstrap or implement the plugin unless the current task
explicitly asks for it. Add nested `AGENTS.md` files only when an existing subtree has
genuinely different build, testing, or ownership rules.

## Read First

For every non-trivial task, read the documents relevant to the change:

1. [`docs/architecture.md`](docs/architecture.md) -- synchronization model and
   architectural decisions;
2. [`docs/testing.md`](docs/testing.md) -- required synchronization evidence;
3. [`docs/building.md`](docs/building.md) -- intended Windows development environment;
4. [`docs/git-workflow.md`](docs/git-workflow.md) -- mandatory Git and PR policy.

If documents conflict, preserve the stricter synchronization guarantee, stop before
making a conflicting architectural assumption, and resolve the documentation in the
same logical change.

## Purpose and Core Model

Build a Windows-first OBS Studio plugin that replay-buffers exactly two independently
configurable scenes into two MKV files. Corresponding video frames must belong to the
same master tick:

```text
master frame N
|-- render Scene A -> A[N]
`-- render Scene B -> B[N]

A[N].master_frame_id == B[N].master_frame_id
A[N].PTS             == B[N].PTS
```

The outputs must behave temporally as if both scenes were rendered side-by-side on
one canvas and separated afterward. Device/source latency before OBS rendering is
outside this guarantee; the guarantee begins at the plugin's shared master tick.

## Hard Synchronization Invariants

1. There is exactly one logical video timeline and one authority for frame ID and PTS.
2. Both scene renders inherit temporal identity from the same master-frame decision.
3. Every master tick has one immutable canonical frame ID and PTS.
4. Corresponding output frames preserve equal master-frame ID and PTS.
5. Save Replay selects one master-frame range and applies it unchanged to both files.
6. Start and end boundaries are never calculated independently per output.
7. Encoder completion order never determines temporal identity.
8. Asynchronous work must preserve the submitted master-frame identity and PTS.
9. Missing or delayed work preserves its temporal slot; later frames never shift to
   conceal it.
10. Synchronization failures are explicit, observable, and logged with the violated
    invariant.
11. Similar wall-clock start times are not evidence of synchronization.
12. Reject independently advancing output timelines and post-hoc drift correction.

Optimize in this order and never trade an earlier item for a later one:

```text
1. Frame-perfect synchronization correctness
2. Deterministic behavior
3. Observability and validation
4. Stability
5. Performance
6. Code simplicity
7. Additional features
8. UI polish
```

## Synchronization-Critical Changes

Treat master timing, frame-ID assignment, PTS generation/propagation, render
scheduling, packet/frame association, replay-range selection, buffer eviction,
missing-frame behavior, encoder queue association, mux boundaries, and validation as
synchronization-critical.

Before changing such behavior:

1. identify the affected invariant in the plan or change summary;
2. inspect and understand the current data flow and relevant OBS/libobs behavior;
3. explain why the invariant remains valid;
4. add or update focused validation/tests where practical;
5. add diagnostic logging for every new failure mode.

Fail explicitly rather than inventing offsets, sleeps, or after-the-fact repair. Use
comments to explain timing assumptions and causality, not to narrate code:

```cpp
// Both renders inherit the master PTS; encoder completion time is not temporal identity.
```

## Task Workflow

Follow [`docs/git-workflow.md`](docs/git-workflow.md): one logical change per branch
and PR, branches named `<type>/vakot/<optional_ticket_id>/<description>`, focused
`<type>(<context>): <description>` commits, mandatory
`<type>(<context>): [<optional_ticket_id>] <title>` PR titles, and Squash and Merge
into protected `master`. Never work directly on `master`. If this directory is not a
Git worktree, stop before implementation; initialize or clone only with user
authorization.

At task startup:

1. read these instructions and the relevant detailed documents;
2. inspect Git status/current branch and preserve unexpected user changes;
3. identify one logical scope, branch type, and ticket ID if any;
4. understand/update `master` and create the compliant branch before editing;
5. inspect the implementation and identify synchronization risk;
6. for non-trivial work, plan the subsystem, likely files, risk, validation, and
   expected result.

Then make the smallest coherent change, build, run focused and relevant broader
tests, inspect applicable logs/artifacts, and review the complete diff. Do not make
drive-by changes. Update documentation only when behavior, architecture, environment,
or workflow actually changes.

At completion, report the branch, commits, validation performed, and PR status. Open
a PR when explicitly requested and supported. Do not merge it unless explicitly
instructed; any permitted integration into `master` must use the repository's Squash
and Merge policy.

## Engineering Conventions

Organize code primarily by `src/<module>/`; keep normal component files directly in
their module as `src/<module>/<component>.*`. Introduce deeper directories only when
concrete complexity justifies them; do not create speculative directories or generic
`index.*` files. See [`docs/architecture.md`](docs/architecture.md) for details.

Follow [`docs/building.md`](docs/building.md) and repository formatter/static-analysis
configuration once bootstrapped. Prefer modern, unsurprising C++ with:

- RAII, explicit ownership, and verified OBS/libobs lifetimes;
- narrow responsibilities and deterministic state transitions;
- descriptive synchronization-oriented types;
- fixed-width integers for cross-component frame/time values;
- injected ticks/clocks in tests rather than sleeps;
- assertions for impossible internal states where safe and explicit runtime errors
  for real OBS/hardware failures;
- minimal hidden mutable global state and no speculative framework abstractions.

Prefer names such as `MasterFrameCoordinator`, `SynchronizedReplayBuffer`,
`ReplayFrameRange`, `SceneRenderTarget`, `OutputStreamState`, and
`SynchronizationValidator`. Avoid vague `Manager`, `Helper`, `Processor`, or `Util`
names unless their narrow responsibility is self-evident.

For OBS/libobs choices, prefer official documentation and source. Verify ownership,
lifetime, threading, render cadence, timestamps, encoder behavior, and packet ordering
before synchronization logic depends on them. Avoid private APIs unless supported APIs
cannot satisfy the requirement and the tradeoff is documented.

## Scope Discipline

The product targets Windows, OBS Studio, exactly two scenes at one FPS, separate MKV files,
NVENC H.264 where practical, configurable replay duration, one save action/hotkey,
minimal configuration UI, and strong validation/logging. Video-only is acceptable
until synchronization is proven; audio must never weaken it.

Defer more than two outputs, streaming, macOS/Linux, advanced audio routing, multiple
encoder families, per-output FPS, updates/installers, extensive UI, generalized
source recording, and abstractions for hypothetical future versions.

## Definition of Done

Ordinary work is done when it is coherent, builds where applicable, relevant tests
pass, the diff contains no unintended changes, and Git conventions are followed.

Synchronization-critical work additionally requires the affected invariant to be
named and demonstrated valid, appropriate validation to pass, every new failure path
to be explicit and diagnosable, and changed architectural assumptions to be
documented. Compilation alone is insufficient.

The product is complete only when repeated paired saves and long-running tests show identical
master-frame ranges and temporal frame mapping with zero accumulating offset, as
defined by [`docs/testing.md`](docs/testing.md).
