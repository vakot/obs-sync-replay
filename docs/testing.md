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
- validation failures for unequal bounds, counts, identities, or PTS.

Prefer injected ticks and deterministic fake render/encoder completions. Tests must
not depend on sleeps to create ordering.

### Integration tests

When libobs and encoder integration exists, verify lifecycle, ownership, thread use,
render cadence, PTS propagation, and mux boundaries. Force completion reordering and
encoder delay where the harness permits. Inspect decoded frames and container timing,
not just in-memory expectations.

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
