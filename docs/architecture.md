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

## OBS-Native Product Integration Target

The current Phase 1--4 implementation deliberately has explicit A/B development
outputs. That is not the intended final user experience. The shipped plugin should
have no visible plugin-owned replay surface: no settings page, dock, custom scene
selector, duplicate encoder or duration settings, plugin buttons, or plugin hotkeys.
Users should work through the ordinary OBS UI, settings, profiles, scene collections,
Replay Buffer controls, and Replay Buffer hotkeys.

> The active OBS profile, scene collection, Replay Buffer controls, and Replay Buffer hotkeys are intended to remain the single user-facing source of configuration and interaction. obs-sync-replay should extend this workflow transparently rather than introduce a parallel replay interface.

Where OBS exposes the needed state safely, the active configuration is the source of
truth. A later integration derives encoder choice, quality and rate-control settings,
preset, profile, keyframe interval, replay duration, output directory, filename
behavior, video settings, and other relevant output settings from that active OBS
configuration. It must not introduce a second persistent configuration that can drift
from the selected OBS profile or scene collection. Supported lifecycle/configuration
events must eventually update the synchronized engine when a profile or relevant OBS
setting changes; the exact event-to-engine mapping is future research, not an
assumption made by the current implementation.

The final output model is dynamic but intentionally not generalized now:

```text
Program + each top-level scene in the active scene collection
```

`Program` is a distinct output that includes normal scene switches and transitions.
For example, a Program sequence `Gameplay -> Camera -> Gameplay` produces the same
saved master-frame range for Program, Gameplay, and Camera streams. Nested scene
sources do not automatically become output streams. A future phase may replace the
current fixed A/B wiring with that model; Phases 1--4 remain exactly two explicit
outputs so their synchronization behavior stays narrow and testable.

Replay interaction likewise belongs to OBS. A standard Start Replay Buffer, Stop
Replay Buffer, or Save Replay Buffer action/hotkey should operate the synchronized
engine without giving users a second workflow. The desired architecture is a vanilla
OBS frontend interaction feeding the plugin engine, not the synchronized engine plus
a concurrent vanilla replay encoder/storage pipeline.

> The plugin should not run the vanilla OBS replay encoding pipeline in parallel when equivalent Program output is already produced by the synchronized replay engine, unless OBS integration constraints make this unavoidable.

The preceding parallel-pipeline rule is a target architecture, pending frontend API
research; it is not an established claim that the current public OBS API can provide
the necessary interception.

### Required Frontend Integration Research Gate

Before implementing replay lifecycle integration, research OBS Studio 32.2.1 public
frontend APIs and relevant internals to determine whether Start, Stop, Save, and their
hotkeys can safely be reused or intercepted before vanilla OBS performs its own replay
action. Public APIs are preferred. A `Replay Buffer Saved` notification is too late to
control the capture/save operation. If public APIs are insufficient, document the
minimum safe integration point and its lifecycle/upgrade risks before proposing a
fallback. This document makes no commitment to Qt hooks, private frontend APIs, or a
parallel vanilla pipeline.

## Conceptual Components

Keep components separate by responsibility, but introduce concrete directories only
when implementation needs them.

| Component | Responsibility | Must not do |
| --- | --- | --- |
| Plugin module | OBS lifecycle, registration, and top-level wiring | Own a second timing path |
| Configuration | Current MVP: validate explicit A/B development values. Target: derive supported state from the active OBS configuration through defined transitions | Create a duplicate persistent plugin configuration |
| Scene selection | Current MVP: hold safe A/B scene references. Target: project Program and top-level scene-collection scenes | Guess libobs ownership or lifetime or promote nested scenes automatically |
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
| `ui` | Future OBS-native frontend integration without synchronization logic or a parallel plugin UI |

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
`timeline` must not depend on rendering, encoding, replay, muxing, or UI. The future
`ui` layer is an adapter to native OBS surfaces, not a separate product interface.

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
move-only FIFO handoff to Phase 4; taking and final destruction retain the same
graphics-context precondition. No retained resource is handed to another thread or
graphics context: Phase 4 copies it into its own persistent typed input ring before
asynchronous NVENC ownership begins. It never uses raw `SceneRenderer` pointers.

There is one fixed-capacity FIFO for complete pairs, not one queue per output. The
current development wiring uses capacity four. The Phase 4 consumer drains this FIFO
inside the same entered graphics context on every successful render tick. The
queue checks that both resources are present and that both copied identities have
equal frame ID and PTS before it accepts a pair. When full, it rejects the entire
new pair before allocating or copying either GPU texture. A missing scene, stale
result, invalid dimensions, allocation failure, or unequal identity similarly retains
no half-pair. This phase does not substitute a frame or let a later frame occupy the
failed slot. `sync-pipeline` diagnostics report `master_frame_id`, `master_pts`,
status, queue size, and capacity; retained frames are debug-level except for sampled
observations, while invalid pairs and pressure are explicit errors/warnings.

## Synchronized Dual Video Encoding (Phase 4)

Phase 4 consumes, encodes, validates, and discards retained pairs. It does not retain
encoded history, mux an MKV, capture audio, or save a replay.

OBS 32.2.1 exposes `obs_video_encoder_create`, `obs_encoder_set_video`, and
`obs_encoder_start` publicly, but it does not expose a public operation to submit an
arbitrary externally rendered `gs_texture_t` with a caller-supplied PTS.
`obs_encoder_info::encode_texture2` is an encoder implementation callback invoked by
libobs's private GPU-video worker over its own converted-texture queues and PTS
sequence. Therefore, creating two `obs_encoder_t` instances would attach this feature
to OBS's normal video/output timeline rather than make `MasterFrame` authoritative.

`obs_nvenc_h264_tex` confirms the backend is texture-capable: on Windows its internal
`d3d11_encode` callback imports an OBS texture and passes a PTS to
`nvEncEncodePicture`. The callback is not public to modules. Phase 4 instead uses the
supported NVIDIA NVENC driver API directly over the public libobs graphics accessors
`gs_get_device_obj` and `gs_texture_get_obj`, while `obs_enter_graphics()` is active.
`VideoEncoder` isolates this H.264/D3D11 implementation; it does not copy OBS private
encoder structures or call private libobs symbols.

Both outputs use identical development settings: H.264 High 4:4:4 (required for the
direct RGBA input), NVENC P3 ultra-low-
latency tuning, CBR 16 Mb/s, a 120-master-frame GOP/IDR cadence, and no B-frames.
Both force an IDR/SPS/PPS at the same `master_frame_id % 120 == 0` slots. Phase 4
accepts SDR `GS_RGBA` retained textures only; another retained format is an explicit
encoder failure, not an implicit color conversion or shifted frame.

Each output owns a bounded ring of six persistent typed RGBA D3D11 input textures,
NVENC registrations, bitstream buffers, and Windows completion events. They are
created and registered once during graphics-context initialization and reused only
after their own asynchronous completion has been copied and unmapped. A slot has the
explicit lifecycle `Free -> Reserved -> Submitted -> Completed -> Free`; its master
identity, mapped resource, and completion event belong to that one lifecycle. The
bounded ring is the resource-lifetime authority: no slot is reused while NVENC still
owns its mapped input or output bitstream.

The graphics tick copies a retained typeless `GS_RGBA` texture into a reserved typed
ring texture with `CopyResource`, maps it, and makes the non-blocking
`nvEncEncodePicture` submission with the frame-specific completion event. It performs
no `Flush`, D3D11 query polling, event wait, blocking bitstream lock, resource
registration, bitstream allocation, or resource destruction. D3D command ordering
keeps the retained source valid through the copy, so the pair can be destroyed after
submission without a frame-count or wall-clock lifetime assumption.

NVENC runs with `enableEncodeAsync = 1`. Each output has a completion worker that
waits for its registered events in that output's submission order, uses
`nvEncLockBitstream(doNotWait = 0)` only after the event signals, copies packet bytes,
then unlocks and unmaps the slot. Workers never call `gs_*` or use OBS's immediate D3D
context. The only D3D immediate-context operation is the graphics-thread handoff copy.
This follows the NVIDIA asynchronous Windows model while respecting libobs graphics
ownership; the worker's NVENC output retrieval is separate from OBS rendering. See the
[NVIDIA NVENC programming guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html)
and OBS 32.2.1's
[`d3d11_encode`](https://github.com/obsproject/obs-studio/blob/32.2.1/plugins/obs-nvenc/nvenc-d3d11.c)
for the researched API and texture-encoder boundaries.

For DirectX sessions, NVENC warns that worker-side bitstream lock/unlock can internally
use the application's D3D device. Therefore the implementation has one shared NVENC/D3D
operation gate across both sessions. The graphics thread acquires it with `try_lock`
for the complete paired preflight/copy/map/submit operation. If output retrieval owns
the gate, the entire A/B master frame is explicitly dropped; the graphics tick never
waits for that gate, an event, capacity, or hardware completion. The worker waits for
its completion event without the gate, then serializes lock, unlock, and unmap through
the gate. This prevents concurrent DirectX/NVENC driver calls, avoids cyclic lock
ordering, and preserves the shared timeline by rejecting a pair atomically rather than
submitting only one output.

Each slot keeps an atomic last-operation marker for the before/after graphics copy,
map, and submit calls, and for event wait, bitstream lock/unlock, unmap, and release
on the completion worker. Sampled `sync-nvenc` begin/end diagnostics and a 10 ms
operation warning limit normal log volume. If no graphics-side preparation occurs for
one second, a worker watchdog emits every slot's output, lifecycle state, master frame,
and last operation, making a graphics stall distinguishable from an event or NVENC API
stall.

The source unit is the canonical nanosecond PTS from `obs_get_video_frame_time()`.
The encoder timebase is fixed at `1/1,000,000,000`, so the checked conversion is exact:
`encoder_pts = int64(master_pts_ns)`. There is no FPS-dependent division or rounding.
Strictly monotonic master PTS produces strictly monotonic encoder PTS; a 60 -> 30 ->
60 cadence change continues the same IDs and PTS without reset or rebase. NVENC must
return the submitted timestamp or the encoder fails. Its nominal configured frame rate
is codec/rate-control configuration, never timeline authority.

`EncodedVideoPacket` owns copied bytes plus immutable `MasterFrame`, output slot, PTS,
DTS, and keyframe state. `EncodedPacketTracker` records expected A/B identities before
either submission and associates results by master frame and encoder PTS, never
completion order. It detects duplicate, unknown, stale, and timestamp-mismatched
packets in a fixed-capacity table.

Submission is one logical pair operation. Both output rings must have capacity before
either submission begins. If either ring is full, the complete A/B master frame is
dropped explicitly and neither output advances that slot; the graphics thread never
waits for capacity. If A cannot submit, B is not submitted. If B fails after A was
accepted, the session halts rather than allowing later work to conceal the partial
operation. Packet association capacity is six complete pairs and completion order is
never temporal authority. Sampled `[sync-encode]` logs record submitted/encoded
identities, while full-ring drops and asynchronous completion failures name invariants
7, 8, and 9. Shutdown signals each completion worker's explicit stop event before
joining it, so the worker never remains indefinitely blocked in an NVENC completion
wait. Teardown then takes the same operation gate before unmapping, unregistering, and
destroying persistent NVENC resources; an interrupted completion is logged as an
aborted shutdown operation and cannot leave a worker accessing a destroyed session.
Phase 5 begins only with synchronized rolling packet history; muxing, audio, and file
saving remain outside this phase.

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

Phases 1--4 have exactly two outputs at one FPS. Prefer clear A/B types or a
fixed-size two-output representation over a speculative arbitrary-output framework.
The later Program-plus-top-level-scenes model and native Replay Buffer integration are
separate phases; neither authorizes generalizing the current encoder, rendering, or
configuration code early. New abstractions are justified only by current behavior,
testing, ownership, or safety.

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
renderers, bounded retained GPU-pair pipeline, and two direct NVENC H.264 sessions.
It still owns no replay buffer, muxer, save action, audio, or output files. It exports
the standard OBS module lifecycle functions and emits stable load/unload messages; its
current synchronization boundary ends at owned encoded packets, which are observed and
discarded after validation.

Windows deployment uses OBS's default module search layout:
`obs-plugins/64bit/obs-sync-replay.dll` for the binary and
`data/obs-plugins/obs-sync-replay` for module data. Development deployment is limited
to an explicitly configured portable OBS root with a portable-mode marker.
