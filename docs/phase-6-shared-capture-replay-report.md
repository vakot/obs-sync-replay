# Phase 6 Shared Capture and Replay Report

This continuation implements the reusable encoded-packet layer required after the
three-stream source POC. It remains research/runtime integration work: UI, hotkeys,
audio, final output naming, and stock Replay Buffer integration are out of scope.

## Architecture

`SynchronizedCaptureSession` is the single N-stream capture authority. Streams are
registered with dense stable IDs, names, codec configuration, and one source CTS
index each. The OBS packet callback copies the encoded payload and its timing fields
into one immutable `shared_ptr<const CapturedEncodedPacket>`. The session stores the
same reference in each stream's bounded compressed ring and fans that reference out
to subscribed consumers after releasing the capture mutex.

The three native video encoders remain the existing `Master`, `Scene A`, and
`Scene B` encoders. Neither Recording nor Replay creates an encoder or re-encodes a
packet. A shared capture session and one encoder group serve both consumers.

## Fan-out and lifecycle

The live Recording consumer receives packet references into a worker queue and writes
one MKV per stream asynchronously. Its callback path only takes a short mutex and
enqueues a reference; muxing does not run on the encoder callback. Its queue is
bounded and reports `recording-queue-overflow` instead of blocking the encoder.

The Replay consumer snapshots references from the capture ring, then muxes the
snapshot on a worker thread. A save is rejected while another save is active, so
concurrent-save behavior is explicit. The capture session continues while a save is
being muxed, and stopping Recording does not stop the capture session or Replay.
Shutdown removes OBS packet callbacks, stops the capture session, drains/finalizes
Recording, waits for Replay, and only then destroys the encoder group.

## Ring, eviction, and common range

Each stream has a byte-bounded compressed packet ring. Eviction removes the oldest
packets and then discards any partial GOP prefix, leaving the oldest retained packet
keyframe-aligned. Snapshots retain shared packet references, so later ring eviction
cannot invalidate an in-progress save.

`SnapshotCommonRange(duration)` computes one range for all streams. It requires
enough shared history, chooses a keyframe CTS present and marked keyframe in every
stream, and stops at the first missing common CTS. That same `{start_cts,end_cts}`
is applied to Master, Scene A, and Scene B. Duplicate, rejected, and missing-range
conditions remain observable through metrics and result errors; packet completion
order never assigns temporal identity.

## Automated coverage

`synchronized-capture-session-test` covers:

- N=3 registration, common watermark, and one common snapshot range;
- a temporarily ahead stream and insufficient history rejection;
- byte-bounded GOP-safe eviction;
- immutable snapshot ownership after capture stop;
- one packet reference fanned out to Recording and Replay observers;
- rejection when no common keyframe exists.

The existing recording-session, frame-queue, renderer, and timeline tests remain in
the CTest suite. The runtime harness additionally exercises two Replay saves during
one continuous Recording session for both x264 and NVENC.

## Runtime evidence

The clean portable runtime was run for 25 seconds with 1.5 seconds warmup, 8-second
Replay saves, a 20-second ring, real scene transitions, and `-SkipUpdateCheck`.

| Encoder | Recording packets per stream | Replay saves | Replay packet counts | Decode/signature result |
| --- | ---: | ---: | --- | --- |
| `obs_x264` | 1,565 | 2 | 486 / 484 per stream | all nine files decoded; each trio had identical PTS/DTS/keyframe signatures |
| `obs_nvenc_h264_tex` | 1,577 | 2 | 496 / 496 per stream | all nine files decoded; each trio had identical PTS/DTS/keyframe signatures |

The x264 capture ring peaked at 30,008,334 bytes and evicted 1,260 packets; NVENC
peaked at 30,008,333 bytes and evicted 1,260 packets. Replay logs reported common
ranges for every save, and all inspected saved trios began at normalized `0,0,K__`
and ended with equal PTS/DTS. Recording trios ended at equal normalized PTS/DTS as
well (`26067` for x264 and `26267` for NVENC). FFmpeg decode returned exit code 0
for every inspected recording and replay file.

The same transition signal check still showed Master intermediate content while
Scene A and Scene B remained their distinct synthetic colors for both encoders.

## Resource and scope notes

The POC uses three native video encoders and three AAC harness encoders because the
OBS null output requires audio setup; audio is not part of this synchronization
claim. Replay snapshots add reference ownership, not a second compressed payload
copy. The tested 20-second ring used about 30 MB per stream at its configured
capacity. A production implementation still needs explicit policy for callback
subscription synchronization, broader encoder packet-order contracts, and longer
endurance runs before UI or audio work.

## Result

The shared capture foundation now demonstrates one native three-stream encoder
topology serving live Recording and asynchronous Replay, with bounded GOP-safe
retention, common keyframe-aligned ranges, repeated saves, and independent consumer
shutdown. No merge or pull request was performed; this branch is ready for review.
