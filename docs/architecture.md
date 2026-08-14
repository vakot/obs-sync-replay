# Architecture

This document turns the product model in [`../mvp-plan.md`](../mvp-plan.md) into
implementation boundaries. It deliberately avoids prescribing a source tree or
specific libobs calls before those APIs have been validated.

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

The source root is an internal include root. Organization has two levels: module
directories group subsystem responsibilities, and component directories group
meaningful implementation units. For example:

```text
timeline/
`-- master-frame-coordinator/
    |-- master-frame-coordinator.hpp
    `-- master-frame-coordinator.cpp
```

Code includes project headers from that root, for example
`#include "timeline/master-frame/master-frame.hpp"`; relative upward paths and
`src/...` includes are not used. Component files retain their explicit component
names—`index.hpp` and `index.cpp` are not used. The repetition is intentional: it
makes search, compiler output, stack traces, and IDE navigation unambiguous even when
the directory context is absent.

Current implementations live in `src/plugin/` and component directories under
`src/timeline/`, with tests mirroring the relevant component hierarchy under
`tests/timeline/`. `timeline/master-frame/master-frame.hpp` owns the reusable
immutable `MasterFrame`, `MasterFrameId`, and `MasterFramePts` domain types, so
rendering and other consumers need not include the timeline state machine.

The intended modules are:

| Module | Responsibility |
| --- | --- |
| `plugin` | OBS module lifecycle and top-level composition; never a timing-logic dumping ground |
| `timeline` | Canonical frame identity, PTS, OBS timing observation, and master timeline lifecycle |
| `rendering` | Selected-scene render targets associated with an existing `MasterFrame` |
| `encoding` | Encoder ownership and packet association with submitted master identity |
| `replay` | One synchronized replay buffer, common ranges, and packet retention |
| `muxing` | MKV outputs, common replay boundaries, and paired output naming |
| `validation` | Invariant checks and diagnostics without a circular dependency from `timeline` |
| `ui` | OBS configuration and controls without synchronization logic |

Only modules and meaningful components with real implementation receive directories;
do not create empty future-module/component folders or placeholder headers. The
intended dependency direction is
`plugin -> timeline -> rendering -> encoding -> replay/muxing`, while `validation`
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

When the graphics loop falls behind, libobs advances its video time by whole frame
intervals. The coordinator creates one master frame for the observed tick and emits a
cadence-discontinuity diagnostic instead of inventing unobserved frames. Future
rendering work must attach to the already-issued `MasterFrame`; it must not fill the
gap with an independently generated timeline.

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

The runtime module currently owns no timing, rendering, encoder, buffer, or output
state. It exports only the standard OBS module lifecycle functions and emits stable
load/unload log messages. Consequently this bootstrap does not implement or alter any
synchronization invariant; it provides the host integration boundary on which later
synchronization-critical changes will build.

Windows deployment uses OBS's default module search layout:
`obs-plugins/64bit/obs-sync-replay.dll` for the binary and
`data/obs-plugins/obs-sync-replay` for module data. Development deployment is limited
to an explicitly configured portable OBS root with a portable-mode marker.
