# Phase 6: Stock OBS Replay Buffer Research

Date: 2026-08-21
OBS source: 32.2.1, source snapshot `0052d024fd6a5ff1aa04c76cbdffd3085a5dfacc`
Branch: `feature/vakot/phase-6-replay-buffer-integration`

## Scope and result

This is a source and public-API audit only. It does not add production replay
saving, change OBS, attach instrumentation to the running OBS process, or
modify the Phase 5 implementation.

The stock Replay Buffer is an encoded-packet deque owned by the FFmpeg mux
output. A save request records an internal wall-clock threshold. The next
encoded packet that crosses that threshold snapshots the current deque, which
is then rebased and muxed asynchronously. The public API exposes the output,
lifecycle events, packet callbacks, and final path, but it does not expose the
internal save threshold, the retained packet range, or the source timestamp
range written to the file.

## 1. Replay Buffer architecture

The stock Replay Buffer output is implemented by
[`plugins/obs-ffmpeg/obs-ffmpeg-mux.c@32.2.1`](https://github.com/obsproject/obs-studio/blob/32.2.1/plugins/obs-ffmpeg/obs-ffmpeg-mux.c),
not by a separate generic output plugin. Its output ID is `replay_buffer` and
its flags include encoded audio/video, multiple audio tracks, and pause support.
The output receives encoded packets through its `encoded_packet` callback.

The frontend creates one `obs_output_t` for Replay Buffer in
[`AdvancedOutput.cpp@32.2.1`](https://github.com/obsproject/obs-studio/blob/32.2.1/frontend/utility/AdvancedOutput.cpp)
or [`SimpleOutput.cpp@32.2.1`](https://github.com/obsproject/obs-studio/blob/32.2.1/frontend/utility/SimpleOutput.cpp).
Depending on the frontend configuration, it assigns the normal program video
encoder (or streaming encoder) and the configured audio encoders to the replay
output. Consequently, the stock replay output can hold references to encoder
objects also used by the normal recording or streaming configuration; it is not
guaranteed to own a unique video encoder.

Starting the output initializes encoders and begins encoded-data capture. The
replay output then retains packet references in its own deque. Saving copies
the retained deque and muxes that copy on a worker thread. The normal OBS
frontend and the stock replay output therefore remain separate output objects,
but may use shared encoder objects and the same OBS root video clock.

## 2. Public lifecycle APIs and events

The public frontend declarations are in
[`obs-frontend-api.h@32.2.1`](https://github.com/obsproject/obs-studio/blob/32.2.1/frontend/api/obs-frontend-api.h).

| Operation | Public surface | What it provides |
| --- | --- | --- |
| Start | `obs_frontend_replay_buffer_start()` | Queues start on the frontend thread |
| Stop | `obs_frontend_replay_buffer_stop()` | Queues stop on the frontend thread |
| Save | `obs_frontend_replay_buffer_save()` | Queues a save request; no request timestamp or range is returned |
| State | `obs_frontend_replay_buffer_active()` | Active-state query |
| Output | `obs_frontend_get_replay_buffer_output()` | Ref-counted `obs_output_t`; caller must release the reference |
| Completion path | `obs_frontend_get_last_replay()` | Last completed replay path only |
| Events | `OBS_FRONTEND_EVENT_REPLAY_BUFFER_*` | Starting, started, stopping, stopped, and saved notifications |

The frontend lifecycle in
[`OBSBasic_ReplayBuffer.cpp@32.2.1`](https://github.com/obsproject/obs-studio/blob/32.2.1/frontend/widgets/OBSBasic_ReplayBuffer.cpp)
has an important asymmetry:

* `STARTING` is emitted before `obs_output_start()`.
* `STARTED` is emitted after the output `start` signal.
* The public save function invokes the replay output's private-to-the-output
  procedure named `save`; there is no public `SAVE_REQUESTED` event.
* `SAVED` is emitted only after the output's `saved` signal, after the mux
  worker has finished.
* `get_last_replay` supplies a path, not timestamps or packet identity.

[`BasicOutputHandler.cpp@32.2.1`](https://github.com/obsproject/obs-studio/blob/32.2.1/frontend/utility/BasicOutputHandler.cpp)
bridges the output `start`, `stop`, `stopping`, and `saved` signals to the
frontend events. The output-specific `saved` signal has no range fields.

## 3. Save and retention algorithm

The implementation in
[`obs-ffmpeg-mux.c@32.2.1`](https://github.com/obsproject/obs-studio/blob/32.2.1/plugins/obs-ffmpeg/obs-ffmpeg-mux.c)
has the following behavior:

1. Each encoded packet is referenced and inserted in decode-time order.
2. Video and audio timestamps are retained as packet metadata; the deque also
   tracks compressed size and video keyframe count.
3. The time limit uses packet decode time (`dts_usec`) and the size limit uses
   compressed packet bytes.
4. Purging removes complete leading keyframe groups. The implementation keeps
   enough video keyframes for replay operation rather than guaranteeing an
   exact duration in source frames.
5. A save request stores `save_ts`, an internal `os_gettime_ns()` value expressed
   in microseconds.
6. The next encoded packet whose `sys_dts_usec` reaches that threshold triggers
   the snapshot. The triggering packet is included in the copied deque.
7. The copied packets are muxed asynchronously. A second save is not an
   independent exact range calculation while the previous mux is active.

The default duration and size settings are approximately 15 seconds and 500 MB,
but the actual retained interval is affected by GOP/keyframe placement, packet
arrival and decode order, encoder latency, and the configured limits. The stock
retention policy is not an exact master-frame range selector.

## 4. Start-boundary and keyframe behavior

The saved stream starts from the retained deque, which is maintained around
video keyframe boundaries. The first retained video packet is used to calculate
the video timestamp offset for the saved file. Therefore the effective start is
the first retained keyframe group, not an independently selected
`masterStartCTS`.

The exact source CTS of that packet is observable only while the encoded packet
is passing through a packet callback with valid timing metadata. The replay
output does not publish the retained deque or its selected first packet. GOP
length and encoder behavior can move the effective start relative to the
configured duration.

## 5. End-boundary behavior

The stock end boundary is not the instant at which the user calls Save Replay.
The save call only records `save_ts`. The first subsequent packet satisfying
`packet->sys_dts_usec >= save_ts` causes the current deque to be copied. The
frontend has no public event for that packet, and the replay output does not
publish the internal threshold or the selected packet.

A passive packet callback can see the packet stream and its `sys_dts_usec`, but
it cannot tell which packet the replay implementation selected as the save
trigger unless it also knows the private `save_ts`. The later `SAVED` event only
means that muxing completed; it does not identify the snapshot boundary.

## 6. Public packet observability

The public packet callback API is documented in
[`obs.h@32.2.1`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs.h).
`obs_output_add_packet_callback()` receives an `obs_output_t`, a borrowed
`encoder_packet`, and an optional `encoder_packet_time`.

In [`obs-output.c@32.2.1`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-output.c),
callbacks run synchronously in the output packet-interleave path before the
output's `encoded_packet` handler. A passive callback that only copies packet
references does not replace or suppress stock Replay Buffer handling. Retained
packets must be referenced immediately and released after use; callback work
must remain short because it runs on the calling thread.

The packet exposes PTS, DTS, timebase, keyframe state, `dts_usec`, and
`sys_dts_usec`. The timing structure in
[`obs-encoder.h@32.2.1`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-encoder.h)
can expose:

* `cts`: the frame's render/composition timestamp in the `os_gettime_ns()`
  timebase;
* `fer`, `ferc`, and `pir`: additional encoder/render timing values;
* the PTS association used to match timing metadata to the packet.

This gives a useful observation point for the master encoded timeline. It does
not provide a master-frame ID, replay deque, save threshold, or save-selection
marker. Timing can also be absent if packet-to-timing association fails, so a
future validator must fail closed rather than synthesize a CTS.

## 7. Master timeline and Scene A/B comparability

The stock root video path creates one root video timestamp and distributes the
same timestamp-bearing frame information to active video mixes. This behavior
is in [`obs-video.c@32.2.1`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video.c).
The view/mix implementation and media-IO delivery paths are separate, so
consumption and encoder completion are not a shared public per-frame queue.

The practical consequence is:

* master output packets and plugin Scene A/B packets can be compared in the
  common source CTS coordinate when valid `encoder_packet_time.cts` values are
  present;
* there is no public official frame ID equivalent to the plugin's
  `master_frame_id`;
* packet callback order, encoder-local PTS, and mux completion order must not be
  used as temporal identity;
* missing or delayed packets preserve a missing slot for validation rather than
  shifting later packets into it.

This is not a separate-master-clock problem. The stock master and plugin views
originate from the OBS root timing domain. The unresolved problem is identifying
the exact subset selected by stock Save Replay and proving that the saved file
preserves the source CTS range.

## 8. Encoder and packet timestamp semantics

The raw encoder path receives the root frame timestamp and assigns encoder-local
PTS values. The encoder timing queue retains the render CTS and later associates
it with the encoded packet by PTS. Thus the following values have different
roles:

| Value | Role | Suitable as exact shared identity? |
| --- | --- | --- |
| `encoder_packet.pts` | Encoder-local packet timestamp | No, not by itself |
| `encoder_packet.dts` | Encoder decode timestamp | No, not by itself |
| `dts_usec` | Replay ordering/retention decode time | No |
| `sys_dts_usec` | Wall-clock-like save-trigger comparison | No; private trigger threshold is missing |
| `encoder_packet_time.cts` | Render/source timing coordinate | Yes for cross-stream comparison when present, but no public frame ID |

The stock replay implementation uses PTS/DTS and packet timebase values for its
own muxing decisions. It does not carry the public `encoder_packet_time.cts`
field into the replay mux packet structure.

## 9. Saved-file timestamp behavior

On save, the replay output finds the first retained video packet and subtracts
its PTS-derived offset from the copied video packets. It similarly establishes
per-track audio offsets. The stock FFmpeg mux helper receives packet PTS, DTS,
timebase, size, stream index/type, and keyframe information; see
[`ffmpeg-mux.c@32.2.1`](https://github.com/obsproject/obs-studio/blob/32.2.1/plugins/obs-ffmpeg/ffmpeg-mux/ffmpeg-mux.c).

The resulting file therefore has a rebased local presentation/decode timeline.
It does not contain the original root CTS as a required source-time metadata
field. The filename timestamp is generated when the save snapshot is created
and is a naming value, not a frame-range anchor.

Post-save `ffprobe` or container inspection can recover local stream timestamps,
keyframe positions, durations, and packet ordering. It cannot reconstruct the
discarded root CTS epoch or distinguish the private save-trigger packet from
another packet with the same rebased relationship.

## 10. Post-save inspection options

After `OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED`, a disposable research probe can
use the public last-replay path and inspect the completed container with
libavformat or `ffprobe`. That can cheaply report stream start time, duration,
packet PTS/DTS, keyframes, and local packet ordering. It cannot recover the
original root CTS because the replay mux receives no CTS field and the video
timestamps were rebased from the first retained video packet.

This is useful for checking stock mux output and detecting malformed or
unexpected files. It is not an exact master-range recovery mechanism. A file
inspection result must therefore be labeled as local container timing, never as
`[masterStartCTS, masterEndCTS]`, unless an independent source-time anchor has
been recorded - which stock Save Replay does not provide.

## 11. Lifecycle integration points for a future implementation

The public integration points are sufficient to observe stock lifecycle state:

* obtain a reference with `obs_frontend_get_replay_buffer_output()`;
* attach a packet callback before capture starts if every master packet is
  required;
* optionally observe the output-specific `saved` signal for completion;
* handle frontend start/stop/saved events for lifecycle diagnostics;
* remove the callback before releasing the output reference and before plugin
  teardown.

These points are not sufficient to recover an exact stock replay range. A future
design that requires the Phase 5 invariant must either own the retention/save
range and muxing path, or obtain an explicit OBS extension that publishes the
selected master range. Calling the stock save procedure and timestamping the
caller is not exact because the implementation samples its own private
`save_ts` later in the frontend/output flow.

Audio is in scope for the stock output even though Phase 5 validation is video
focused. Audio packets participate in retention ordering and are rebased per
track during muxing. Audio therefore affects complete-file boundaries and must
not be silently treated as evidence of the video master range. It does not solve
the missing source CTS/save-marker problem.

## 12. Runtime evidence status

No stock Replay Buffer save was run as part of this research branch. The checked-
in Phase 5 probe attaches to the plugin-owned Scene A/B encoder outputs; it does
not attach to the frontend's stock `replay_buffer` output, and the repository
contains no corresponding saved replay artifact to inspect.

No disposable instrumentation was added because the exact 32.2.1 source path
already establishes the decisive missing observability: the private save
threshold and selected packet are not published, while the saved file rebases
away the source CTS. Adding a callback-only probe would demonstrate packet
metadata but could not change that conclusion. An already-running stock OBS
process was left untouched.

The existing Phase 5 runtime evidence remains valid for comparing continuously
running Scene A/B source CTS values. It is not evidence that stock Replay Buffer
saved ranges are exact and must not be reported as such.

## 13. Exact remaining limitation

For a stock frontend Save Replay operation, a plugin can observe the master
encoded packet stream, PTS/DTS/keyframes, and - when association succeeds - the
source render CTS. It cannot publicly observe the exact save request threshold,
the packet that crossed it, the retained deque's first packet, or a source CTS
range in the resulting file.

Therefore an exact `masterStartCTS`/`masterEndCTS` pair cannot be established
from the stock public lifecycle, packet callback, and post-save file APIs alone.

## Required conclusion

**D - MASTER RANGE NOT EXACTLY OBSERVABLE.**

The master and Scene A/B timelines share a comparable OBS root CTS domain, so
the result is not conclusion E. Public packet callbacks are valuable for
validation and diagnostics, but they do not turn stock Save Replay into an
exact, publicly range-addressable master buffer.
