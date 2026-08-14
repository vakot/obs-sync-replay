# Architecture

This document turns the product model in [`../mvp-plan.md`](../mvp-plan.md) into
implementation boundaries, source organization, and architectural decisions that
protect synchronization invariants.

## Synchronization Contract

The plugin owns one logical sequence of master video ticks. For each tick, one
coordinator assigns immutable identity before either scene is rendered:

```text
MasterFrame { master_frame_id, master_pts }
                  |
          one dispatch decision
           /                  \
  render Scene A          render Scene B
         |                       |
  encode with identity    encode with identity
           \                  /
            synchronized replay
```

For every retained temporal slot `N`:

```text
A[N].master_frame_id == B[N].master_frame_id
A[N].PTS             == B[N].PTS
```

Encoder concurrency may reorder completion, but it cannot redefine identity. Packet
association must use submitted master metadata/PTS, never callback order, queue
position alone, or elapsed wall-clock time.

## Guarantee Boundary

OBS sources and capture devices may have different exposure, buffering, transport,
decode, or source-frame latencies before the plugin renders them. Those upstream
latencies are visible content and are outside the MVP guarantee.

The guarantee begins when the plugin selects a shared master video tick. From that
point onward, both scene renderings, encoder submissions, replay selection, and muxed
outputs must preserve the tick's identity and PTS. The plugin synchronizes its
projections of OBS scene state; it does not claim to align device-internal capture
instants.

## Conceptual Components

Keep components separate by responsibility, but introduce concrete directories only
when implementation needs them.

| Component | Responsibility | Must not do |
| --- | --- | --- |
| Plugin module | OBS lifecycle, registration, and top-level wiring | Own a second timing path |
| Configuration | Validate two scenes, duration, output path, encoder settings, and names | Mutate active synchronization state without a defined transition |
| Scene selection | Hold safe OBS references to selected scenes | Guess libobs ownership or lifetime |
| Master frame coordinator | Produce the sole `master_frame_id` and PTS sequence; dispatch both renders | Read encoder completion time as a clock |
| Scene render targets A/B | Render each selected scene for the supplied master tick | Generate timestamps independently |
| Encoders A/B | Encode their render target while preserving submitted identity/PTS | Pair frames by completion order |
| Synchronized replay buffer | Retain both projections on one logical timeline and apply deterministic eviction | Advance one output past the other silently |
| Replay save/muxing | Snapshot one range and write two MKV projections with a shared replay ID | Select per-output boundaries |
| Validation | Check range, frame/slot counts, identities, PTS, and declared missing-slot policy | Downgrade drift to a warning and call the pair synchronized |
| Logging | Emit joinable lifecycle, queue, missing-slot, range, and validation events | Report only generic success/failure text |

## Module-Oriented Source Layout

The source root is an internal include root. Organization is:

```text
repository
  ↓
module
  ↓
component files
```

Modules are meaningful architectural/subsystem boundaries. Components normally live
directly inside their owning module; a module already supplies the organizational
context. For example:

```text
src/
|-- plugin/
|   `-- plugin-main.cpp
`-- timeline/
    |-- master-frame.hpp
    |-- master-frame-timeline.hpp
    |-- master-frame-timeline.cpp
    |-- master-frame-coordinator.hpp
    `-- master-frame-coordinator.cpp
```

Code includes project headers from that root, for example
`#include "timeline/master-frame.hpp"`; relative upward paths and `src/...` includes
are not used. Component files retain explicit descriptive names; `index.hpp` and
`index.cpp` are not used.

Introduce a nested directory inside a module only when a concrete organizational need
exists, such as a large subsystem with several closely related components or resources.
Do not create a directory for every C++ class or `.hpp`/`.cpp` pair, and do not use
redundant paths such as
`timeline/master-frame-coordinator/master-frame-coordinator.cpp` when
`timeline/master-frame-coordinator.cpp` is sufficient. Do not create empty future
module or component directories.

Current implementations live in `src/plugin/`, `src/timeline/`, `src/rendering/`,
and `src/pipeline/`, with tests mirroring the module hierarchy. `timeline/master-frame.hpp`
owns the reusable immutable `MasterFrame`, `MasterFrameId`, and `MasterFramePts`
domain types, so rendering and downstream consumers need not include the timeline
state machine.

The intended modules are:

| Module | Responsibility |
| --- | --- |
| `plugin` | OBS module lifecycle and top-level composition; never a timing-logic dumping ground |
| `timeline` | Canonical frame identity, PTS, OBS timing observation, and master timeline lifecycle |
| `rendering` | Selected-scene render targets associated with an existing `MasterFrame` |
| `pipeline` | GPU-side retention of one completed A/B render pair and its bounded shared FIFO |
| `encoding` | Encoder ownership and packet association with submitted master identity |
| `replay` | One synchronized replay buffer, common ranges, and packet retention |
| `muxing` | MKV outputs, common replay boundaries, and paired output naming |
| `validation` | Invariant checks and diagnostics without a circular dependency from `timeline` |
| `ui` | OBS configuration and controls without synchronization logic |

Future modules follow the same flat-within-module convention, for example:

```text
src/rendering/scene-renderer.hpp
src/rendering/scene-renderer.cpp
src/pipeline/synchronized-frame-pipeline.hpp
src/pipeline/synchronized-frame-pipeline.cpp
src/replay/synchronized-replay-buffer.hpp
src/replay/synchronized-replay-buffer.cpp
src/muxing/replay-muxer.hpp
src/muxing/replay-muxer.cpp
```

These paths are a convention only; do not create future module directories or files
until the corresponding implementation work begins.

The intended dependency direction is
`plugin -> timeline -> rendering -> pipeline -> encoding -> replay/muxing`, while `validation`
observes relevant domains and `ui` acts as a configuration/control layer. In
particular, `rendering`, `encoding`, and `replay` may consume `timeline`, but
`timeline` must not depend on rendering, encoding, replay, muxing, or UI.

## Frame Lifecycle

1. The coordinator accepts or generates the next master tick.
2. It creates one immutable master-frame identity and supplies it to both render paths.
3. Each path renders the selected scene for that tick or reports a missing result.
4. The configured deterministic missing-slot policy preserves the tick on both logical
   timelines; later content is never shifted backward.
5. Each available/substituted render is submitted with the same master PTS to its
   encoder.
6. Encoded results are associated back to their output and master identity regardless
   of completion order.
7. The logical replay buffer retains sufficient information to project a shared frame
   range to both outputs and to validate the projection.

The exact representation may use separate bounded queues for efficiency. The queues
must expose one shared logical range and deterministic behavior under backpressure.

## Master Timeline Integration (Phase 1)

The first implementation uses the public libobs `obs_add_tick_callback` lifecycle
hook and reads `obs_get_video_frame_time()` inside that callback. In OBS Studio
32.2.1, libobs invokes tick callbacks once per graphics-loop iteration before source
ticking. The callback runs on libobs's graphics thread, but after libobs has left its
graphics context; this phase therefore performs no rendering there.

`master_pts` is the returned `uint64_t` OBS video time in nanoseconds. A coordinator
session begins at `master_frame_id = 0`, accepts only strictly increasing observed
PTS values, and resets both its frame ID and PTS acceptance state when stopped.
`MasterFrame` fields are immutable to consumers and only the coordinator's internal
timeline creates them.

The coordinator refreshes the configured OBS frame interval on every tick before
validating observed cadence. A nonzero interval change is logged once as a timing
configuration transition and takes effect immediately for later cadence checks; the
transition tick is not misreported as a graphics lag discontinuity. This refresh never
resets frame IDs, rebases PTS, or manufactures frames: `obs_get_video_frame_time()`
remains the canonical PTS source throughout the session.

When the graphics loop falls behind, libobs advances its video time by whole frame
intervals. The coordinator creates one master frame for the observed tick and emits a
cadence-discontinuity diagnostic instead of inventing unobserved frames. Future
rendering work must attach to the already-issued `MasterFrame`; it must not fill the
gap with an independently generated timeline.

## Dual-Scene Rendering Integration (Phase 2)

Phase 2 consumes the `MasterFrame` directly from `MasterFrameCoordinator`; it creates
no render timer, per-scene callback, or secondary PTS source. The coordinator tick is
on libobs's graphics thread but executes after libobs leaves its graphics context.
For each accepted frame, `SynchronizedSceneRenderer` calls `obs_enter_graphics()`,
renders Scene A and Scene B synchronously, and calls `obs_leave_graphics()` after both
attempts. OBS 32.2.1's own `ScreenshotObj` uses this same supported context-enter
pattern from a tick callback. This is deliberately not a queued "latest frame"
handoff: the two attempts occur in the callback that owns the supplied frame.

Each `SceneRenderer` resolves its development scene name (`Gameplay Test` or `Camera
Test`) with `obs_get_source_by_name` for the individual attempt. That API returns a
strong source reference, released immediately after rendering. The renderer verifies
that the source is an `obs_scene_t`, obtains its native `obs_source_get_width` and
`obs_source_get_height`, and renders it with `obs_source_video_render` into its own
`gs_texrender_t` target. The target uses the source's reported graphics color space
and is recreated if that space changes; target dimensions are the source's native
dimensions for that attempt. `gs_texrender_end` marks the target rendered, so each
subsequent master-frame attempt resets its own target before beginning; this is a
graphics-resource lifecycle operation, not a timing decision. The target is created and destroyed only while the
graphics context is entered. Its texture is GPU-only and remains owned by the
renderer until that renderer's next render or destruction. Phase 3 must therefore
copy a successful result before the renderer reaches its next render.

`SceneRenderResult` copies the immutable `MasterFrame` and records a fixed A/B slot,
status, dimensions, and non-owning texture pointer. `SceneRenderPairTracker` accepts
only the active frame's exact ID and PTS and only one result per slot. Every frame
therefore attempts A and B even if either fails. Missing, invalid, or unavailable
scenes retain the current master slot and emit a diagnostic; subsequent frames never
replace that missing result. Phase 3 retains only the complete successful pair before
either short-lived source texture can be reused.

## Synchronized GPU Frame Pipeline (Phase 3)

Phase 3 inserts one pair-only boundary directly after both render attempts:

```text
MasterFrame N
    |
render A[N] and B[N]
    |
copy A[N] and B[N] into pipeline-owned GPU textures
    |
SynchronizedFramePair { MasterFrame N, retained A[N], retained B[N] }
```

`SynchronizedFramePipeline` owns each `SynchronizedFramePair`; each pair owns two
move-only `RetainedGpuFrame` values. A retained frame stores its immutable
`MasterFrame`, fixed A/B output slot, dimensions, color metadata, and exactly one
owned `gs_texture_t`. Neither retained frame aliases the non-owning texture pointer
in `SceneRenderResult` or the reusable `SceneRenderer::render_target_`. Copying the
owner is prohibited, and moving is the only ownership transfer available.

On OBS 32.2.1, the pipeline allocates each destination with `gs_texture_create` and
submits `gs_copy_texture` while `SynchronizedSceneRenderer` has entered the OBS
graphics context. The D3D11 backend implements that operation with ordered device
copy commands, so a later `gs_texrender_reset`/render into the source target cannot
replace the copied destination's content. The implementation deliberately does not
stage textures or read pixels back to the CPU. Allocation, copy, and destruction all
occur inside `obs_enter_graphics()`/`obs_leave_graphics()`; the pipeline is reset in
that context during renderer stop, before its destructor runs. `TakeNext` provides a
move-only FIFO handoff for a future consumer; taking and final destruction retain the
same graphics-context precondition. No retained resource is handed to another thread
or graphics context in this phase. Future encoders must consume pipeline-owned
textures under an explicitly designed graphics-context and completion-lifetime
contract, not raw `SceneRenderer` pointers.

There is one fixed-capacity FIFO for complete pairs, not one queue per output. The
current development wiring uses capacity four because no encoder yet drains it. The
queue checks that both resources are present and that both copied identities have
equal frame ID and PTS before it accepts a pair. When full, it rejects the entire
new pair before allocating or copying either GPU texture. A missing scene, stale
result, invalid dimensions, allocation failure, or unequal identity similarly retains
no half-pair. This phase does not substitute a frame or let a later frame occupy the
failed slot. `sync-pipeline` diagnostics report `master_frame_id`, `master_pts`,
status, queue size, and capacity; retained frames are debug-level except for sampled
observations, while invalid pairs and pressure are explicit errors/warnings.

## Replay Save Contract

One hotkey invocation creates a replay ID and snapshots one immutable inclusive range:

```text
ReplayFrameRange {
    replay_id,
    start_master_frame,
    end_master_frame
}
```

Both mux jobs consume that same value. Buffer rollover after the snapshot must not
change either output's selected range. Container/keyframe constraints may require
careful packet retention or mux planning, but may not produce different logical start
or end frames. The two filenames share the replay ID/timestamp and differ only by
their configured output label.

Before reporting success, validation must establish at least equal requested bounds
and expected slot counts. Where implementation metadata permits, validate every
master-frame ID and PTS. A failed validation is an explicit failed replay pair, not a
successful pair with a warning.

## Missing Frames and Pressure

Queue pressure, late rendering, or encoder delay cannot justify timeline shifting.
Before implementing missing-frame behavior, research what OBS/libobs and the chosen
render/encoder path support, then document and test one simple deterministic policy.
Candidate policies include:

- repeat the previous rendered image for the missing master slot; or
- retain an explicit missing slot and resolve it deterministically before encoding.

These are options, not a decision made by this document. The selected policy must also
define startup behavior when no prior image exists. Record the affected output,
master-frame ID, master PTS, reason, chosen action, and queue depth. Do not add
arbitrary sleeps, per-output clock adjustment, magic offsets, or after-the-fact drift
repair.

## Observability Contract

Use stable, joinable field names for synchronization diagnostics:

```text
master_frame_id
master_pts
output
replay_id
start_master_frame
end_master_frame
expected_frame_count
actual_frame_count
encoder_queue_depth
validation_result
```

Log lifecycle transitions, missing/substituted slots, queue pressure, range snapshots,
and validation outcomes with relevant fields. Errors must name the violated invariant
and affected output/range; a generic `Synchronization error` is insufficient when
specific context exists.

## State and Concurrency

Make start, running, saving, stopping, and stopped/error transitions explicit. A save
uses a stable snapshot while capture continues or fails atomically with a precise
reason. Stop/restart must not reuse stale frame IDs, packets, ranges, or callbacks.

Shared data requires declared ownership and synchronization. Render-thread and OBS
callback requirements must be confirmed from official OBS documentation/source
before implementation. Expensive encoding and muxing may be asynchronous; mutation
of temporal identity may not be.

## Scope and Evolution

The MVP has exactly two outputs at one FPS. Prefer clear A/B types or a fixed-size
two-output representation over a speculative arbitrary-output framework. New
abstractions are justified only by current behavior, testing, ownership, or safety.

Any proposal for an independently advancing timeline, per-output replay range, or
post-hoc synchronization is an architectural change that conflicts with the current
product contract and should normally be rejected.

## Bootstrap Integration Baseline

The initial module is built against the official OBS Studio 32.2.1 source tag and the
official 2026-07-15 Windows x64 dependency bundle. CMake builds and installs only the
matching `libobs` development target into ignored local state before building the
plugin. This mirrors the dependency model used by the official OBS plugin template
while keeping the repository independent of an installed OBS SDK.

The runtime module owns the master-frame coordinator, synchronous dual-scene
renderers, and the bounded retained GPU-pair pipeline. It still owns no encoder,
replay buffer, muxer, save action, audio, or output files. It exports the standard OBS
module lifecycle functions and emits stable load/unload messages; its current
synchronization boundary ends at a successfully retained GPU frame pair.

Windows deployment uses OBS's default module search layout:
`obs-plugins/64bit/obs-sync-replay.dll` for the binary and
`data/obs-plugins/obs-sync-replay` for module data. Development deployment is limited
to an explicitly configured portable OBS root with a portable-mode marker.
