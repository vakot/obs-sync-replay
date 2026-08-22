# Testing Strategy

Synchronization is accepted through observable frame identity and timestamps, not by
similar start times, durations that merely look close, or subjective playback.

This strategy applies as implementation phases become available. Document executable
commands and preset names according to [`building.md`](building.md) when the build/test
harness is introduced.

## Required Observables

Every replay pair should expose enough test metadata or deterministic visual content
to establish:

```text
A.frame_count        == B.frame_count
A.start_master_frame == B.start_master_frame
A.end_master_frame   == B.end_master_frame
```

Where technically available, verify every temporal slot:

```text
for every i:
    A[i].master_frame_id == B[i].master_frame_id
    A[i].master_pts      == B[i].master_pts
```

Encoded packet count is not automatically decoded video-frame count; tests must use
the appropriate observable for the encoder/container path. Keep master-frame metadata
in diagnostics even if the final MKV does not expose it directly.

## Canonical End-to-End Source

Use a deterministic frame counter or equivalent timecode driven by the same master
tick and visible in both scene render paths. Decode the saved files and compare the
counter at matching output-frame indexes. The counter makes duplicated, missing,
shifted, and reordered frames observable independently of wall-clock timestamps.

Run paired saves at these minimum durations:

| Duration | Purpose | Required result |
| --- | --- | --- |
| 60 seconds | Fast regression | Equal bounds/counts and zero offset |
| 5 minutes | Repeated-save and moderate-run check | Zero accumulating drift |
| 30 minutes | Endurance acceptance | Zero accumulating drift across multiple arbitrary saves |

No test may accept accumulating drift as "close enough." A mismatch of one temporal
slot is a synchronization failure, even if total durations are similar.

## Test Layers

### Deterministic component tests

As components appear, cover:

- monotonic master-frame ID and strict acceptance/rejection of observed OBS PTS values;
- identical dispatch identity for both render requests;
- association of out-of-order encoder completions with the submitted master frames;
- one missing-render policy, including the first frame when no previous image exists;
- queue-pressure behavior without per-output advancement;
- buffer eviction that retains a coherent shared range;
- one replay-range snapshot applied unchanged to both projections;
- filename pairing with a shared replay ID/timestamp;
- stop/restart state reset and rejection of stale callbacks/packets;
- live OBS frame-interval changes without a master-frame ID reset, PTS rebase, or
  repeated false cadence-discontinuity diagnostics;
- validation failures for unequal bounds, counts, identities, or PTS.

Prefer injected ticks and deterministic fake render/encoder completions. Tests must
not depend on sleeps to create ordering.

Phase 5 adds `synchronized-recording-session-test`. It covers owned compressed
packet copies, bounded capacity, asymmetric startup, no-common-keyframe behavior,
common-end selection, reordered PTS/DTS, transactional startup rollback, delayed
stop/drain finalization, and identical selected A/B source ranges. The test uses
injected packets and fake sinks; it does not use OBS runtime state.

### Phase 3 retained-pipeline coverage

`synchronized-frame-queue-test` is OBS-independent and covers one complete pair,
unchanged master identity, FIFO ordering, deterministic bounded-capacity rejection,
half/divergent-pair rejection, and reset without stale identity. GPU copy behavior is
validated in portable OBS because the queue test intentionally does not mock libobs
graphics internals. Runtime logs must show a matching master identity in timeline,
render, and retained-pipeline events; capacity rejection is a valid explicit pressure
outcome, never a reason to advance one output separately.

### Integration tests

When libobs and encoder integration exists, verify lifecycle, ownership, thread use,
render cadence, PTS propagation, and mux boundaries. Force completion reordering and
encoder delay where the harness permits. Inspect decoded frames and container timing,
not just in-memory expectations.

The Phase 5 research runner uses stock `null_output` only as a compressed-packet
source. The plugin-owned `MkvPacketSink` receives copied packet payloads and codec
extradata, retains only a bounded DTS-ordering tail, and receives only the same strict
source-CTS common prefix for both streams. It then commits only the DTS-stable prefix
of that source range. Runtime success requires one common start and end CTS, equal
selected source bounds, zero range mismatches, successful incremental commits, and
successful finalization for both files. The default runtime safety budget is 5 seconds
of public source CTS, converted into each packet timebase for the DTS watermark;
packets arriving older than the committed watermark or exceeding either byte bound
fail explicitly. This makes a multi-hour normal recording use approximately the same
compressed-packet memory as a short recording.
The result log includes per-stream `peak_tail_bytes_a` and `peak_tail_bytes_b` in
addition to the final empty-tail counters, so steady-state memory can be checked
without treating finalization as the steady-state measurement.

Phase 6 adds `synchronized-capture-session-test`, which verifies N-stream common
watermark/range selection, keyframe-safe compressed-ring eviction, immutable
snapshot ownership, and fan-out of one encoded packet reference to independent
consumers. The clean three-stream runtime also runs live Recording and two
serialized asynchronous Replay saves concurrently from the same native x264 or
NVENC encoders. Validate every output trio with FFmpeg decode plus equal PTS/DTS/
keyframe signatures; equal wall-clock durations alone are insufficient.

`synchronized-replay-consumer-test` verifies concurrent-save rejection, save-worker
completion after capture stop, save-after-stop from frozen history, and repeated
save completion using the consumer's deterministic test barrier. It also verifies a
failed save is observable and does not prevent a subsequent save.

The focused session test also runs a three-hour synthetic 60 FPS logical timeline and
asserts that retained tail bytes remain bounded after incremental commits. It covers
asymmetric callback arrival, strict-prefix safety when one stream is ahead, reordered
PTS/DTS at Stop, partial-streaming failure, and transactional finalization failure.
It also covers idempotent stop/drain behavior after finalization and explicit abort
behavior when shutdown arrives before a common start exists.

On 2026-08-21, a clean portable smoke run completed both x264 and NVENC H.264
sessions with `state=stopped failure=none`. FFprobe decoded the resulting A/B MKVs;
the x264 pair contained 1798 frames each and the NVENC pair 1804 frames each, with
identical per-packet PTS/DTS/keyframe metadata and equal durations within each pair.

### OBS end-to-end tests

Exercise the real plugin with at least four ordinary scenes in a portable OBS scene
collection, NVENC H.264, MKV output, and one save hotkey. Retain paired files and
structured logs for failures. Confirm the topology log contains Master followed by
all discovered scene UUIDs in collection order, and that every saved output uses the
same selected master range.

## Stress Matrix

Cover, individually and in useful combinations:

- high GPU load;
- repeated replay saves, including saves near buffer rollover;
- a long-running replay buffer;
- asymmetric encoder delay and out-of-order completion;
- queue pressure/backpressure;
- OBS preview enabled and disabled;
- temporary absence or delay of one rendered source;
- stopping and restarting the replay system.

For each case, assert the same range and frame/slot invariants. Also verify that every
missing/substituted slot, validation failure, and pressure event is explicit in logs.

## Replay-Pair Validation Record

A successful save should produce or log a record equivalent to:

```text
[sync-replay] replay_id=42 start_master_frame=120000 end_master_frame=125399
expected_frame_count=5400 output_a_frames=5400 output_b_frames=5400
validation_result=ok
```

Failure records must include the violated invariant and relevant output,
`master_frame_id`, `master_pts`, range, counts, and queue state. Do not emit success
or publish a pair as synchronized when required validation fails.

## Change-Based Requirements

Use the smallest test set that proves the change, plus regression coverage for the
affected contract:

- timeline/PTS changes: component tests plus an appropriate duration drift test;
- render/encoder association changes: reordered-completion and missing-slot tests;
- buffer/range/mux changes: rollover, repeated-save, equal-boundary, and decoded-file
  validation;
- lifecycle/threading changes: start/stop/restart and stale-work tests;
- UI/configuration-only changes: focused validation/UI tests, unless a pipeline value
  or state transition is affected.

A synchronization-critical task is not done until the relevant invariant is observed
passing and any new failure mode is visible in diagnostics. The product is not complete until
the 30-minute scenario passes with multiple arbitrary replay saves
and zero frame-offset divergence.

## Bootstrap Validation

Before synchronization components exist, both committed build configurations use the
same CTest entry point:

```powershell
ctest --preset windows-debug
ctest --preset windows-release
```

The bootstrap smoke test proves only that CMake produced a non-empty native plugin
artifact. Runtime acceptance additionally requires deploying to the configured
portable OBS instance, observing the `[obs-sync-replay] plugin loaded` log entry, and
closing OBS normally with the matching unload entry. These checks establish module
integration only and are not synchronization evidence.

## Clean Runtime and Topology Validation

The stock-OBS research experiment must be launched with
`scripts/research.ps1` after `scripts/build.ps1`. The launcher resets the configured
portable runtime and generates only the documented `Sync Replay Research` profile
video keys; it must not be replaced with manual UI setup or a prior Phase 1–7 runtime.

The clean runtime log must show the fixed video check and idle plugin startup before
any timeline or encoder result is considered. The clean launcher creates no scenes;
create at least four ordinary scenes in the active collection for topology validation.
The relevant evidence is:

```text
[obs-sync-replay] plugin loaded ...
[plugin-control] initialized idle=true active_encoder_count=0
[topology] event=initial-discovery ...
[topology] stream kind=master identity=master ...
[topology] stream kind=scene identity=<uuid-1> ...
[topology] stream kind=scene identity=<uuid-2> ...
[topology] stream kind=scene identity=<uuid-3> ...
[topology] stream kind=scene identity=<uuid-4> ...
[plugin-ui] native controls replaced dock=controlsDock record=recordButton replay=replayBufferButton save=saveReplayButton
[obs-sync-replay] plugin-owned controls ready ui_replaced=true recording=off replay=off active_encoders=0
```

Missing UUIDs, topology discovery failure, participant changes inside an active epoch,
or encoder activation caused only by an idle topology change invalidates the run.
Rename a scene and confirm its UUID is unchanged and no encoder is recreated. Add a
scene during capture and confirm `discovery-staged` followed by `pending-applied`.
Remove an active scene and confirm its original participant remains in the epoch,
then disappears only after the pending topology applies.

### Phase 7 control validation

`scene-topology-test` covers explicit Master identity, collection ordering, duplicate
rejection, rename continuity, idle add/remove, immutable active-epoch participants,
staged topology, pending apply, and identity-keyed future participation flags.

`capture-control-test` covers the explicit stream modes, aggregate encoder demand,
idempotent and invalid commands, Recording/Replay handoff in both directions,
mixed identity-keyed scene modes, disabled streams, total-idle release, and fresh
capture-epoch creation. The test also verifies that Recording-only capture does not
retain replay packets, that unavailable replay rejects Start/Save, that disabling
replay stops only the replay consumer, and that re-enabling does not auto-start it.

`replay-configuration-test` covers the OBS default duration/memory conversion,
enabled configuration, unsupported stock backends, and the explicit emergency
bound for stock-unlimited modes. `synchronized-capture-session-test` covers the
global shared packet budget, common-keyframe eviction, and live capacity updates.

For the three-stream identity-keyed control-test fixture, tests must show three
active video encoders during Recording-only, Replay-only, and concurrent operation;
`encoder-retain` events must appear when a consumer handoff keeps an encoder alive,
and active count must reach zero only after both consumers stop. Runtime output
validation remains the Phase 6 requirement: the discovered Master-plus-scenes
participant set must preserve one common source-CTS range and equal PTS/DTS/keyframe
signatures for every participating stream.

### Phase 7 plugin-owned UI validation

The product plugin must load with Recording and Replay inactive and zero active
plugin-owned video encoders. With OBS Replay Buffer disabled or unsupported, Replay
and Save Replay are hidden while Recording remains visible. The UI adapter locates
`controlsDock` and replaces the
native `recordButton`, `replayBufferButton`, and `saveReplayButton` layout entries
with plugin-owned controls at the same positions. Labels, visibility, and enabled
state are derived from the translated active OBS profile and
`CaptureControlEngine` state. Plugin frontend hotkeys invoke the
same runtime toggle/save methods as the controls. Stock OBS Recording and Replay
Buffer buttons and lifecycle events are not used as product state.

Change the profile's Replay Buffer setting and confirm the plugin refreshes it
without starting replay. A disable while replay is active must stop replay and
leave Recording running; a later enable must expose controls without auto-starting.
The profile-key and transition contract is in
[`replay-configuration.md`](replay-configuration.md).

The Recording and Replay toggle controls also expose plugin-owned help indicators
inside the main buttons. Verify their localized tooltips in idle and active/blue
states, verify that the indicator hit area does not invoke the toggle action, and
verify that Save Replay remains a compact icon-only control without that indicator.
Repeat at 175% Windows scaling and confirm there is no clipping or overlap.

On shutdown, plugin controls are disabled before both consumers are stopped, replay
saves are joined, encoders reach zero, hotkeys are unregistered, and the native
widgets are restored before plugin controls are released. A manual acceptance run
should verify Recording-only, Replay-only, overlap/handoff, repeated Save Replay,
and graceful close for the discovered Master plus all-scene `Both` configuration.

The adapter deliberately uses no pixel or screenshot assertions. It relies on the
OBS 32.2.1 object names and row-layout structure documented above, copies native
button sizing/icons/style properties, and inherits the active Qt/OBS palette. A
future OBS update must re-check those object names and layout entries.

The product runtime also logs creation before activation for every non-disabled stream.
This is expected: resources are pre-created to keep the OBS encoder group complete,
while only aggregate-demand streams contribute active output and packet callbacks.
