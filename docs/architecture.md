# Architecture

This document defines implementation boundaries, source organization, and
architectural decisions that protect synchronization invariants.

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
 latencies are visible content and are outside the synchronization guarantee.

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
| Recording session | Own one A/B transaction, common packet boundaries, drain, and rollback | Start/stop one output independently or use callback order as identity |
| Encoded packet buffers | Own compressed bytes and bounded pre-roll/tail state | Evict a packet without an explicit capacity failure |
| Synchronized replay buffer | Retain both projections on one logical timeline and apply deterministic eviction | Advance one output past the other silently |
| Recording/muxing | Write validated compressed packets to two MKVs with one common source range | Decode, re-encode, or select per-output boundaries |
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
`src/pipeline/`, `src/recording/`, `src/sync/`, and `src/muxing/`, with tests mirroring the module hierarchy. `timeline/master-frame.hpp`
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
| `recording` | One transactional normal-Recording session and owned compressed packet buffers |
| `sync` | Common packet-range selection and exact A/B range validation |
| `replay` | One synchronized replay buffer, common ranges, and packet retention |
| `muxing` | Packet-only MKV output, codec setup, timestamp rebasing, and finalization |
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
`plugin -> timeline -> rendering -> pipeline -> encoding -> recording/sync -> muxing/replay`, while `validation`
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

## Scene topology and rendering integration

Production startup waits for OBS's finished-loading frontend event, then discovers
the current collection through public `obs_enum_scenes()`. Discovery creates a
Master/Program stream followed by every top-level OBS scene in callback order. A
collection with no real scenes is valid and produces a Master-only topology. The
plugin retains owned scene-source references and uses UUIDs for identity; names are
display metadata only.

`obs_enum_scenes()` is intentionally not a recursive scene-item walk. Groups, nested
scene sources, and ordinary sources remain content rendered by their containing
top-level scene. They do not create independent video streams. Audio traversal may
define different semantics in Phase 8, but it must not change this video topology.

The renderer consumes the `MasterFrame` directly from `MasterFrameCoordinator`; it
creates no render timer, per-scene callback, or secondary PTS source. Every active
top-level scene render inherits the same master identity and PTS. Add/remove/order
changes are staged until the active capture epoch ends, while a rename updates only
display metadata and never restarts an active encoder.

Each scene target owns the public OBS scene source reference used by its view. A
missing or unavailable scene is reported explicitly; the topology layer never
substitutes a named fixture or shifts another scene into its temporal slot.

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

## Research runtime isolation

The optional research launcher may reset a disposable portable runtime and write a
deterministic video profile for reproducible encoder tests. That preparation is
outside the production plugin. It never supplies scene names or scene objects to the
plugin: production discovery sees the resulting real collection through the same
public topology path as a normal OBS launch.

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

The product has exactly two outputs at one FPS. Prefer clear A/B types or a fixed-size
two-output representation over a speculative arbitrary-output framework. New
abstractions are justified only by current behavior, testing, ownership, or safety.

Any proposal for an independently advancing timeline, per-output replay range, or
post-hoc synchronization is an architectural change that conflicts with the current
product contract and should normally be rejected.

## Bootstrap Integration Baseline

The following section records the earlier Phase 5 stock-output research baseline;
the active Phase 7 product path is the plugin-owned control runtime described below.

The initial module is built against the official OBS Studio 32.2.1 source tag and the
official 2026-07-15 Windows x64 dependency bundle. CMake builds and installs only the
matching `libobs` development target into ignored local state before building the
plugin. This mirrors the dependency model used by the official OBS plugin template
while keeping the repository independent of an installed OBS SDK.

The earlier research runtime owned the master-frame coordinator, synchronous
dual-scene renderers, and the bounded retained GPU-pair pipeline. Its Phase 5
Recording runner additionally owned two stock native OBS encoder/output lifecycles,
the transactional compressed-packet session, and two plugin-owned MKV sinks. That
research path did not patch OBS or introduce a second encoder. Its lack of Replay
Buffer, Save Replay, normal OBS hotkeys, and settings inheritance was a baseline
limitation; Phase 7 now provides plugin-owned equivalents below.

The Recording session starts both stock pipelines as a preparation step, buffers
packets until a common keyframe CTS is observed, then opens both MKV sinks from that
same CTS. After startup, each stream retains an ordered compressed-packet tail and
tracks its highest observed public source CTS. The shared running watermark is:

```text
min(highest_observed_cts_A, highest_observed_cts_B) - reorder_safety_cts
```

Only the strict A/B common source-CTS prefix at or below that watermark is passed to
the sinks. The safety window is an explicit encoder-completion reordering budget
(5 seconds in the stock x264/NVENC research runner). Each sink converts that budget
to the packet timebase and applies a second DTS watermark, so callback/source order
cannot force an older decode timestamp after a committed packet. A tail overflow or
packet that arrives at or below a committed CTS fails the transaction instead of
allowing a late packet to rewrite muxed history. The session and each sink have fixed
byte limits, so recording duration does not increase retained compressed memory.

The common prefix also checks the configured OBS frame interval, allowing only one
nanosecond of rational-clock rounding. A missing or delayed logical slot therefore
blocks later CTS values even when both streams are missing that slot; it cannot be
mistaken for a completed common prefix. Unit tests may infer the interval from the
first paired packets, but the OBS runtime passes `obs_get_frame_interval_ns()`
explicitly so initial encoder sparsity cannot establish a false cadence.

Stop changes the session to `Draining`; both outputs are stopped, callbacks are
allowed to finish, the remaining strict common prefix through the requested stop CTS
selects one exact `commonEndCTS`, and both sinks receive that range in per-stream DTS
order before finalization. A missing packet-time, duplicate CTS, unsafe late packet,
range mismatch, sink failure, or capacity overflow is a failed transaction with no
synchronized success result.

The plugin also subscribes to stock OBS's public
`OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP` boundary. OBS emits it during close
after scene removal has begun but before its destroy queue is waited, which is the
earliest public shutdown boundary available in OBS 32.2.1 for quiescing plugin-owned
views while video and encoder callbacks are still alive. The callback schedules the
blocking worker stop on a plugin-owned thread, then waits for that thread before
returning. This keeps encoder/output teardown off the frontend callback's call stack
while making the cleanup event a true quiescence barrier: both stock outputs drain
and finalize their common range before callbacks, views, the coordinator, and the
renderer are released.
`OBS_FRONTEND_EVENT_EXIT` remains an idempotent fallback immediately before frontend
callbacks are destroyed. Starting, running, draining, stopped, and failed sessions
do not cause a second finalization or an invented range. OBS operations occur outside
the recording-session mutex.

Windows deployment uses OBS's default module search layout:
`obs-plugins/64bit/obs-sync-replay.dll` for the binary and
`data/obs-plugins/obs-sync-replay` for module data. Development deployment is limited
to an explicitly configured portable OBS root with a portable-mode marker.

## Phase 7 Control and Configuration Layer

Phase 7 adds an in-memory `CaptureConfiguration` over the Phase 6 shared capture
session. Production topology is an explicit Master/Program stream followed by all
eligible real scenes returned by public `obs_enum_scenes()` in collection order.
Master has the special identity `master`; each scene is keyed by its public OBS
source UUID and carries its display name separately. Each stream has one explicit
participation mode: `Disabled`, `Recording`, `Replay`, or `Both`.
Recording and Replay select their own configured stream subsets; a `Both` stream is
still represented by one native OBS video encoder whose immutable packet is fanned
out to both consumers.

`CaptureControlEngine` owns independent infrastructure, Recording, and Replay
states and exposes `StartRecording`, `StopRecording`, `StartReplay`, `StopReplay`,
and `SaveReplay` commands. Repeated starts are explicit no-ops; Save Replay while
Replay is off is an explicit invalid-state result. Stopping one consumer unsubscribes
only that consumer and reconciles aggregate encoder demand, so a handoff does not
restart an encoder or reset the shared CTS epoch. If both consumers become idle,
capture and all demanded encoders stop; the next start creates a new capture epoch.

Before activation, the control engine asks the encoder controller to create every
non-disabled configured stream. It then activates only streams required by the
aggregate demand. This two-phase lifecycle is required by OBS encoder groups,
which cannot accept a new member after the first member has started; inactive
resources therefore remain available for later consumer handoffs without restarting
active encoders.

The authoritative encoder rule is:

```text
stream needs encoder = recording consumer active and mode permits Recording
                     or replay consumer active and mode permits Replay
```

Replay retention is enabled only while Replay is active. Recording-only capture still
fans packets to its live consumer but does not retain a replay ring. Mixed-mode
Replay saves use one common range selected across only the streams configured for
Replay, while Recording receives only streams configured for Recording. Lifecycle
diagnostics report encoder activation, retention by another consumer, and release
with the active encoder count.

The product runtime is plugin-owned and initializes idle after frontend loading. Its
`ObsControlsAdapter` locates OBS 32.2.1's public Qt object structure at
`controlsDock`, then replaces the `recordButton`, `replayBufferButton`, and
`saveReplayButton` layout entries with plugin-owned buttons at the same indices. The
stock widgets are hidden and retained, never destroyed or used as backend state, and
are restored on teardown. The replacement buttons and plugin-owned frontend hotkeys
call the same `PluginCaptureRuntime` control methods; neither uses stock OBS
Recording or Replay Buffer state, buttons, or handlers. The runtime takes an
immutable topology snapshot when the first consumer activates. Idle topology
changes rebuild resources without starting encoders; active renames update
metadata, while add/remove/order changes stage until both consumers are off. Scene
source references are retained through an active epoch, and collection cleanup is
an explicit coordinated shutdown boundary.

The former scripted OBS development harness was removed from the product runtime;
the control engine's deterministic unit tests remain the separate validation path.
The plugin now consumes the active profile's public OBS Replay Buffer settings for
availability, shared duration, and shared memory policy. It never starts or uses
the stock replay output. Exact Simple/Advanced keys, unsupported encoder modes,
refresh boundaries, the active-disable transition, and the emergency bound are
documented in [`replay-configuration.md`](replay-configuration.md). Native control
lookup remains isolated in the adapter because its object names and layout
structure are OBS-version-sensitive.

The product idle invariant is checked at load: before any explicit UI or hotkey
action, Recording and Replay are Off and the plugin-owned active encoder count is
zero. Shutdown disables plugin controls, restores the retained stock widgets,
unregisters plugin hotkeys, stops both consumers, waits for active replay saves,
drains capture, and releases the plugin-owned scene views and encoder group.
