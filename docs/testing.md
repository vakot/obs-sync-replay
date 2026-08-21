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
| 30 minutes | MVP endurance acceptance | Zero accumulating drift across multiple arbitrary saves |

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

The focused session test also runs a three-hour synthetic 60 FPS logical timeline and
asserts that retained tail bytes remain bounded after incremental commits. It covers
asymmetric callback arrival, strict-prefix safety when one stream is ahead, reordered
PTS/DTS at Stop, partial-streaming failure, and transactional finalization failure.

On 2026-08-21, a clean portable smoke run completed both x264 and NVENC H.264
sessions with `state=stopped failure=none`. FFprobe decoded the resulting A/B MKVs;
the x264 pair contained 1798 frames each and the NVENC pair 1804 frames each, with
identical per-packet PTS/DTS/keyframe metadata and equal durations within each pair.

### OBS end-to-end tests

Exercise the real plugin with the deterministic visual counter, two selected scenes,
NVENC H.264, MKV output, and one save hotkey. Retain paired files and structured logs
for failures. Confirm the filenames share a replay identifier and both files represent
the same selected master range.

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
passing and any new failure mode is visible in diagnostics. The MVP is not done until
the 30-minute scenario in `mvp-plan.md` passes with multiple arbitrary replay saves
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

## Clean Runtime Bootstrap Validation

The stock-OBS research experiment must be launched with
`scripts/run-obs-research.ps1` after deployment. The launcher resets the configured
portable runtime and generates only the documented `Sync Replay Research` profile
video keys; it must not be replaced with manual UI setup or a prior Phase 1–7 runtime.

The runtime log must show this ordered evidence before any timeline or encoder result
is considered:

```text
[sync-bootstrap] scheduled for frontend finished-loading after scene-collection activation
[sync-bootstrap] begin clean_runtime=true ...
[sync-bootstrap] video-check observed base=1920x1080 output=1920x1080 fps=60/1 ...
[sync-bootstrap] initial-source-check inputs=0 scenes=1
[sync-bootstrap] stock-placeholder-check name=... items=0
[sync-bootstrap] stock-placeholder remove complete name=...
[sync-bootstrap] clean-source-check inputs=0 scenes=0
[sync-bootstrap] create-scene complete name=Sync Research Scene A ...
[sync-bootstrap] create-scene complete name=Sync Research Scene B ...
[sync-bootstrap] complete scene_a=... scene_b=...
[obs-sync-replay] research bootstrap ready; coordinator started ...
```

Any bootstrap failure, name collision, nonzero initial input count, additional scene,
non-empty stock placeholder, video mismatch, or missing completion line invalidates
the run. The synthetic `color_source` inputs
are the only experiment content and require no camera, display capture, external file,
recording output, replay-buffer configuration, or existing scene/source name.
