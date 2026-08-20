# Native libobs video/encoder pipeline research

Status: experimental research branch only.  Reference implementation: OBS Studio
32.2.1 (`.deps/sources/obs-studio-32.2.1`, tag
[`32.2.1`](https://github.com/obsproject/obs-studio/tree/32.2.1)).  This document
does not change the production Phase 4 decision.

## Decision

**YES, WITH A GRAPHICS-LAG BLOCKER.** Two independent selected scenes can use the
native `obs_view_t -> video_t -> obs_encoder_t` path without weakening
frame-boundary synchronization during the observed normal cadence, provided that
the plugin treats the OBS composition timestamp (CTS, in nanoseconds) as its
`MasterPTS` and validates a per-packet CTS-to-master mapping. It must not require
the raw `encoder_packet.pts` integer to equal the nanosecond `MasterPTS` integer:
libobs deliberately makes encoder PTS a value in the encoder timebase, beginning
at zero for each activation.

The target invariant remains:

```text
A[N].master_frame_id == B[N].master_frame_id
A[N].cts_ns          == B[N].cts_ns == MasterPTS[N]
A[N].encoder_pts     == B[N].encoder_pts
```

`encoder_pts` is not a substitute clock. It is a packet-local presentation value
joined to the immutable master frame by CTS. Equal A/B native PTS is presently a
**normal-cadence experimental check**, not yet a proven invariant for every lagged
slot. The two preserved NVENC anomalies require a paired missing/duplicated-slot
model before this path can be accepted for production replay. A later production
replay record must store both `master_pts_ns` and native `encoder_pts`, and reject
or explicitly represent a packet that lacks an unambiguous CTS-to-master join. It
must not use offset correction or callback order.

## Public API surface

The following are exported declarations in `libobs/obs.h` and are suitable for a
plugin compiled against the pinned SDK:

| API | Role in the experiment |
| --- | --- |
| `obs_view_create`, `obs_view_set_source`, `obs_view_add`, `obs_view_add2`, `obs_view_remove`, `obs_view_destroy` | Create an independent view, select its scene source, make it an additional core video mix, and remove it. |
| `obs_video_encoder_create`, `obs_encoder_set_video`, `obs_encoder_set_group`, `obs_encoder_group_create/destroy` | Create standard registered encoders, attach each to its own `video_t`, and synchronize activation. |
| `obs_output_create`, `obs_output_set_video_encoder`, `obs_output_start/stop`, `obs_output_add_packet_callback` | Use a normal OBS output to own the non-public encoder start/stop work and observe packets. |
| `obs_frontend_add_event_callback` / `obs_frontend_remove_event_callback` | Observe `OBS_FRONTEND_EVENT_FINISHED_LOADING` for opt-in activation and `OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN` for early experiment cleanup only; this is not Replay Buffer lifecycle integration. |
| `obs_get_video_frame_time`, `obs_add_tick_callback` | Existing Phase 1 canonical nanosecond timeline. |
| `encoder_packet` and `encoder_packet_time` from `obs-encoder.h` | Observe packet PTS/DTS/keyframe and its PTS/CTS/FERC timing record. |

`obs_encoder_initialize`, `obs_encoder_start`, `obs_encoder_stop`,
`obs_create_video_mix`, the `obs_core_video_mix` structure, the graphics loop,
and direct mix queues are implementation details in `obs-internal.h` / private
translation units.  The experiment neither includes private headers nor calls those
symbols.

### Submission-time association feasibility (OBS 32.2.1)

The requested `EncoderInputAssociation` cannot be created by a plugin using the
current public API.

**PUBLIC API SOLUTION — lifecycle yes, generic submission association no.**

`obs_output_info` exposes output `start`/`stop`, raw callbacks, and the final
`encoded_packet` callback, but no callback around an individual encoder request
([`libobs/obs-output.h:41-58`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-output.h#L41-L58)).
`obs_output_add_packet_callback` is explicitly invoked by `send_interleaved()`
before forwarding a packet to the output service, after the encoder has already
received the input ([`libobs/obs.h:2162-2171`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs.h#L2162-L2171)).
`obs_add_raw_video_callback` observes only the raw path, while
`obs_encoder_info.encode_texture2` is an encoder implementation callback; using
it would mean owning/replacing the encoder, which is out of scope
([`libobs/obs.h:897-901`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs.h#L897-L901),
[`libobs/obs-encoder.h:198-210`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-encoder.h#L198-L210),
[`libobs/obs-encoder.h:343-349`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-encoder.h#L343-L349)).

The public output extension point does solve the separate video-only lifecycle
question. `OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED` is a valid flag combination;
`can_begin_data_capture()` validates video and audio independently, and
`obs_output_begin_data_capture()` initializes and starts only the flagged
encoders ([`libobs/obs-output.c:1353-1365`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-output.c#L1353-L1365),
[`libobs/obs-output.c:2638-2655`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-output.c#L2638-L2655),
[`libobs/obs-output.c:2758-2783`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-output.c#L2758-L2783)).
A plugin-owned registered output can therefore call the public initialize/begin
and end-capture functions with no audio encoder. The research `null_output`
uses `OBS_OUTPUT_AV` only because it is an existing no-op fixture; its temporary
AAC encoder is not required by the public video-only output contract.

**MINIMAL INTERNAL DEPENDENCY — technically possible, not production-safe as a
plugin dependency.**

The smallest private fork would carry an opaque source-slot token through the
existing submission path and invoke a callback immediately before the generic
encoder implementation:

* `libobs/obs-video.c:442-515` (`queue_frame`) must preserve a distinct token
  when the duplicate branch increments an existing `obs_tex_frame.count`; a
  single timestamp/count is exactly what causes the proven texture CTS alias.
* `libobs/obs-video-gpu-encode.c:145-210` must pass that token, the selected
  `obs_encoder_t`, `encoder->cur_pts`, composition CTS, and a monotonic request
  token immediately before `encode_texture`/`encode_texture2`.
* `libobs/obs-encoder.c:1585-1640` must make the same callback at the raw
  `receive_video`/`do_encode` boundary, so raw and texture encoders expose one
  generic contract.
* The private `obs_vframe_info`/`obs_tex_frame` definitions in
  `libobs/obs-internal.h:312-326` need the opaque source token (or an equivalent
  per-count token queue).

Hooking only the final GPU call is insufficient: by then the duplicate texture
has already lost which logical slot in the `count` sequence it represents.
This fork would have to be rebased and retested against every OBS change to
`obs-video.c`, `obs-video-gpu-encode.c`, `obs-encoder.c`, and the internal queue
layout, so it is a research instrument rather than a production dependency.

### Branch prototype: generic input association hook

This branch carries that fork as a bounded upstream-quality prototype. The pinned
OBS SDK patch adds `obs_encoder_set_input_callback()` and the public
`obs_encoder_input` / `encoder_packet_time` fields in `libobs/obs.h` and
`libobs/obs-encoder.h`. The callback runs synchronously immediately before the
registered encoder implementation on both paths. It receives a monotonically
unique `request_id`, `source_frame_id`, composition CTS, and encoder-local PTS;
the plugin may fill an opaque `association_id`, which libobs copies into the
packet timing record. The callback does not own the encoder, block, retain the
input pointer, or observe completion order.

The source token is created in `video_sleep()` (`libobs/obs-video.c`), transported
through `video_data` (`libobs/media-io/video-io.{h,c}`), copied into
`obs_tex_frame` (`libobs/obs-internal.h`), and incremented for every repeated
count slot, including the `encode_gpu()` count expansion in `obs-video.c`.
`obs-video-gpu-encode.c` invokes the hook for each texture request;
`obs-encoder.c` invokes the same hook for raw requests and records the association
with the existing PTS-keyed timing entry. Consequently the 40 ms texture case
can retain two requests with the aliased rendered CTS while still identifying
their distinct logical source slots. The plugin's `InputCallback()` maps the
source sequence to its immutable `logical_slot_id`; packet validation then joins
by that association, treating CTS aliasing as observed evidence rather than
inventing a corrected timestamp.

This is intentionally a private, pinned-OBS research patch, not a production
plugin dependency. It changes the libobs public ABI and requires rebuilding the
SDK; the patch is applied by `cmake/ObsSdk.cmake` and marked in the local SDK
stamp. The exact maintenance surface is the eight files named above plus the
patch application step. The production recommendation remains **UPSTREAM HOOK
REQUIRED**: upstream the smallest opt-in equivalent (preferably with an opaque
core token and packet timing association) and consume it only after API/ABI and
threading semantics are reviewed by OBS. The video-only output lifecycle can be
used independently through the public `OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED`
output contract; Replay Buffer frontend integration is still not implemented.

**UPSTREAM HOOK REQUIRED — production recommendation.**

Request a small public, opt-in encoder-input hook (per `obs_encoder_t` or the
video-only `obs_output_t`) whose callback runs synchronously immediately before
`encode`/`encode_texture2` and receives:

```text
EncoderInputAssociation {
  source_slot_token;       // opaque core token, distinct for every logical slot
  composition_cts_ns;
  encoder;                 // obs_encoder_t*
  encoder_request_token;  // stable per submission, not callback order
  encoder_pts;
}
```

The core must create and carry `source_slot_token` from the `count` expansion
through both raw and texture queues. The plugin then maps that token to its
canonical `logical_slot_id`; CTS remains the composition timestamp and native
PTS remains only the packet-local join value. This is the smallest generic hook
that preserves encoder choice, supports texture reuse, and does not expose or
replace NVENC internals. It should be proposed upstream rather than maintained
as a private libobs fork.

### Important public-lifecycle limitation

There is no public direct `obs_encoder_start` API.  Native encoders must be driven
by a normal `obs_output_t`.  OBS 32.2.1's registered `null_output` is AV-only, so
the no-file experiment attaches a stock `ffmpeg_aac` encoder to each null output
only to satisfy its activation contract.  It writes no file, retains no packets,
and performs no audio synchronization work; video is the only observed stream.  A
production video-only replay integration should register its own
`OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED` output and use the public output lifecycle;
that removes the activation-audio workaround. This lifecycle solution does not
remove the separate submission-association blocker above.

## Lifecycle: research activation versus target product

### Current research activation

Plugin load alone never creates an `obs_output_t`, adds an `obs_view_t`, or starts a
video encoder. The default is disabled, and logs that fact once. Setting
`OBS_SYNC_REPLAY_EXPERIMENT_AUTOSTART=1` explicitly opts into the research run. In
that mode the plugin registers one public frontend callback and, at
`OBS_FRONTEND_EVENT_FINISHED_LOADING`, performs one explicit `Start()` attempt after
the scene collection is available. It logs the explicit activation, successful
start, and any explicit `Stop()` once each.

`ObserveMasterFrame()` only records canonical frames while the experiment is already
running. It does not start or retry outputs as master frames advance. Therefore a
normal OBS startup with the plugin installed leaves no `sync_replay_native_output_*`
active and must leave Video settings editable. The current explicit mode is solely a
test control; it is not user-facing configuration, a production hotkey, or Replay
Buffer frontend integration.

### Shutdown coordination

The experiment also uses `OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN` only to stop an
already-active experiment during normal application exit. In OBS 32.2.1 this event is
emitted before `ClearSceneData()` releases scenes/sources and before the following
`OBS_FRONTEND_EVENT_EXIT` ([`frontend/widgets/OBSBasic.cpp:2034-2045`](https://github.com/obsproject/obs-studio/blob/32.2.1/frontend/widgets/OBSBasic.cpp#L2034-L2045)).
`EXIT` is therefore too late for output cleanup coordination.

This ordering fixes the observed crash boundary: `obs_output_release()` joins the
output's capture thread, whose normal encoded-output shutdown disconnects audio. The
capture thread must complete while global audio is still valid. The experiment first
marks itself inactive, removes each packet callback (libobs serializes callback
invocation and removal with the output packet-callback mutex), stops/releases both
outputs, destroys the inactive encoder group, releases video and activation-audio
encoders, and finally removes/destroys the views. The views hold strong source
references, so releasing them before `ClearSceneData()` also avoids extending scene
source lifetime into OBS teardown.

Every transition is logged: stop request, callback removal, each output stop
completion, group destruction, encoder releases, view remove/destroy, and stop
completion. This is application-shutdown cleanup only; it does not add Replay Buffer
start, stop, or save integration.

### Target product lifecycle

The product must keep the synchronized native outputs inactive while OBS is idle or
the vanilla Replay Buffer is off. Vanilla **Start Replay Buffer** must eventually
activate the synchronized outputs; **Stop Replay Buffer** must stop them; and
**Save Replay** must eventually save the one shared buffered range. None of those
Replay Buffer callbacks, buffering, or saving semantics is implemented by this
research branch. In particular, merely loading the plugin must never cause OBS to
consider recording or video output active.

## Source trace: view to `video_t`

### PROVEN

1. `obs_view_add` calls `obs_view_add2` using the main canvas video info
   ([`libobs/obs-view.c:143-166`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-view.c#L143-L166)).
   `obs_view_add2` creates an `obs_core_video_mix`, assigns its `view`, appends it
   to the core mix list, and returns that mix's `video_t`.
2. On every graphics-loop iteration, `video_sleep` constructs **one**
   `obs_vframe_info { timestamp = cur_time, count }`, then appends that same value
   to every active mix's raw/GPU queue
   ([`libobs/obs-video.c:807-861`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video.c#L807-L861)).
3. The same loop calls `output_frames`, which iterates every added mix and renders
   it sequentially on the one libobs graphics thread
   ([`libobs/obs-video.c:914-930`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video.c#L914-L930),
   [`1069-1145`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video.c#L1069-L1145)).
   Rendering a mix calls `obs_view_render(view)`, which renders the selected view
   sources ([`libobs/obs-video.c:171-216`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video.c#L171-L216)).
4. The raw path passes the queued timestamp to `video_output_lock_frame`; the
   resulting `video_data.timestamp` is delivered to the encoder connection
   ([`libobs/obs-video.c:773-795`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video.c#L773-L795),
   [`896-910`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video.c#L896-L910)).

Therefore two active additional views do not acquire independent clocks.  They are
separate rendered mixes, but each accepted non-lagged core frame receives the same
`cur_time` timestamp record before either mix is rendered.

### Lag and missing slots

When the graphics loop is late, `video_sleep` sets `count > 1`, advances core video
time by whole frame intervals, and records lagged frames
([`libobs/obs-video.c:807-833`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video.c#L807-L833)).
`MasterFrameCoordinator` is a rendered-frame observer: it accepts only graphics
ticks it actually sees and logs a cadence discontinuity. That is not sufficient to
describe every OBS logical video slot. The experiment now keeps a separate,
source-derived logical-slot registry for the fixed-FPS research case; it never calls
those OBS-owned repeated slots fabricated rendered frames. Production must make the
two identities explicit rather than infer either one from callback order.

## Graphics lag / duplicated-slot semantics

The following source findings use the pinned OBS Studio 32.2.1 tree. The runtime
evidence is from the explicit 40 ms NVENC lag-injection run in the portable OBS log.

### PROVEN

1. A late graphics iteration intentionally produces `obs_vframe_info.count > 1`.
   `video_sleep` calculates the number of elapsed frame intervals, advances core
   video time by that count, increments `lagged_frames` by `count - 1`, and queues
   `{ timestamp = cur_time, count }` to every active mix
   ([`libobs/obs-video.c:805-860`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video.c#L805-L860)).
2. The **raw/software** path intentionally reuses the rendered image for each
   count slot. `video_output_lock_frame` stores `count` and the initial timestamp;
   its video thread invokes each input once per count and increments
   `video_data.timestamp` by the configured frame interval after every invocation
   ([`libobs/obs-video.c:777-795`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video.c#L777-L795),
   [`libobs/media-io/video-io.c:126-214`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/media-io/video-io.c#L126-L214)).
   Thus a repeated image occupies consecutive CTS slots; `count > 1` does not by
   itself assign one CTS to multiple repeated raw frames.
3. The **GPU/texture** path is not equivalent to passing that `count` straight to
   one texture. `output_gpu_encoders` pops the queued `obs_vframe_info` on a later
   graphics iteration and calls `queue_frame` once per remaining count
   ([`libobs/obs-video.c:518-536`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video.c#L518-L536),
   [`libobs/obs-video.c:512-515`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video.c#L512-L515)).
   For the first iteration of a `count = 2` record when the GPU queue already has
   a texture, `queue_frame` increments the *last queued* `obs_tex_frame.count`
   without changing its timestamp.  The second iteration has `count = 1`, so it
   allocates a new texture with the original `vframe_info.timestamp`
   ([`libobs/obs-video.c:442-507`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video.c#L442-L507)).
4. The GPU encode thread records CTS from the texture frame it popped.  It advances
   a texture timestamp only when requeuing a remaining count
   ([`libobs/obs-video-gpu-encode.c:60-194`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video-gpu-encode.c#L60-L194),
   [`libobs/obs-video-gpu-encode.c:199-210`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video-gpu-encode.c#L199-L210)).
   Thus, if the last queued texture is the preceding rendered slot `X - Δ`, the
   augmented texture emits `X - Δ` then `X`, while the newly queued texture also
   emits `X`. The texture queue has produced two `X` CTS requests and no request
   carrying the logical repeated-slot CTS `X + Δ`.
5. `encoder->cur_pts` advances once for each successful raw encode request and for
   every texture encode iteration. It is independent of the composition timestamp
   ([`libobs/obs-encoder.c:1623-1638`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-encoder.c#L1623-L1638),
   [`libobs/obs-video-gpu-encode.c:154-194`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video-gpu-encode.c#L154-L194)).
6. For each encode request, libobs records an `encoder_packet_time` keyed by native
   PTS with the supplied raw frame timestamp or texture timestamp as CTS. A packet
   callback receives the timing record selected by exact native PTS match; it is not
   selected by callback order
   ([`libobs/obs-encoder.c:1417-1503`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-encoder.c#L1417-L1503),
   [`libobs/obs-output.c:1716-1754`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-output.c#L1716-L1754)).
7. `obs_nvenc_h264_tex` selects this texture route and passes the libobs local PTS
   to NVENC as `inputTimeStamp`; it does not manufacture or alter CTS
   ([`plugins/obs-nvenc/venc.c:1403-1421`](https://github.com/obsproject/obs-studio/blob/32.2.1/plugins/obs-nvenc/venc.c#L1403-L1421),
   [`plugins/obs-nvenc/venc-d3d11.c:210-263`](https://github.com/obsproject/obs-studio/blob/32.2.1/plugins/obs-nvenc/venc-d3d11.c#L210-L263),
   [`plugins/obs-nvenc/venc.c:1284-1296`](https://github.com/obsproject/obs-studio/blob/32.2.1/plugins/obs-nvenc/venc.c#L1284-L1296)).
8. Each `obs_view_add` mix owns its raw/GPU queues and, for texture encoders, its
   own GPU encode thread. An encoder group gives a common start timestamp; it does
   not make later per-mix queue consumption or `cur_pts` advancement atomic across
   two views.

### PROVEN: the 40 ms NVENC CTS alias

The 40 ms run directly matches the texture-queue transition above. For example,
at `00:29:33.919` its source-derived timeline recorded a repeated logical slot
`X + Δ = 90797321373222` anchored to rendered slot
`X = 90797304706556`. Starting at `00:29:34.179`, both outputs accumulated two
packet observations for CTS `X`, with consecutive local PTS values; the log contains
no packet observation for `X + Δ`. The same shape repeated at every injected lag
boundary, although the A/B callback order varied. Normal single-packet validation
resumed after each boundary without observed accumulating drift.

The exact source/runtime sequence is:

```text
vframe_info { timestamp = X, count = 2 }
  first queue_frame call: increment pending texture { timestamp = X - Δ, count }
  second queue_frame call: enqueue new texture { timestamp = X, count = 1 }

GPU worker: pending texture -> CTS X - Δ, then CTS X
GPU worker: newly queued texture -> CTS X
```

This is a libobs 32.2.1 texture-queue timing behavior, not an NVENC reordering or
an error in `LogicalVideoSlot` generation. The separate raw path gives x264 one
input callback per `count`, incrementing its timestamp after each callback, so it
correctly emits `X` and `X + Δ` for the same source event. The invariant affected is
the asynchronous identity-preservation invariant: a texture packet CTS is not a
one-to-one public key for a logical slot across this lag boundary.

### Encoder-agnostic association requirement

The smallest model that supports both paths keeps `LogicalVideoSlot` canonical and
adds an explicit, submission-time association:

```text
EncoderInputAssociation {
  logical_slot_id;          // canonical source identity
  composition_cts_ns;       // may be many-to-one for a reused texture
  encoder_request_token;    // per-encoder local correlation only
  composition_disposition;  // direct render or reused composition
}
```

Raw/x264 produces a direct association for each slot. The texture path must produce
an association at its private queue/encode boundary for every request, including a
reused composition. The output callback may use the request token only to recover
that already-created association; neither native PTS nor callback order may create
or replace the canonical slot identity.

OBS's public packet callback exposes only native PTS and CTS, not this queue decision,
the private `count`, or a source-supplied logical-slot tag. Therefore the current
plugin cannot safely infer the missing `X + Δ` association for texture encoders.
It continues to retain the bounded same-CTS packet set as an explicit unsupported
lag condition. Implementing a correct association requires a supported libobs
association hook (or plugin-owned submission boundary) before this native path can
meet the production replay guarantee.

### Research instrumentation and production direction

The experiment now retains a bounded raw packet set per CTS/master frame instead
of treating a second observation as an immediate invariant failure. Every packet
logs output, PTS, DTS, CTS, keyframe, matching master identity, per-CTS observation
index, and previous per-output PTS/CTS at DEBUG level. A multiple-observation or
single-packet PTS mismatch emits one structured record containing all observed A
and B packets for that CTS. Timeline logs carry previous master PTS, configured
interval, cadence delta, and lagged-frame count.

This is instrumentation only: it does not pair by arrival order, repair PTS,
rewrite CTS, fabricate MasterFrames, or silently discard extra packets. The bounded
diagnostic remains the correct behavior until a supported association hook supplies
the exact texture-submission decision.

### Deterministic graphics-lag reproduction

The research branch provides a deliberately opt-in injector for reproducing this
boundary without touching packet callbacks, encoder workers, capture code, PTS,
CTS, or MasterFrame creation:

```text
OBS_SYNC_REPLAY_EXPERIMENT_INJECT_LAG_MS=<1..100>
OBS_SYNC_REPLAY_EXPERIMENT_INJECT_LAG_EVERY=<1..1000000>  # optional; defaults to 600
```

It is disabled when `OBS_SYNC_REPLAY_EXPERIMENT_INJECT_LAG_MS` is absent or invalid.
The coordinator logs the disabled/enabled state once at startup, and logs each
injection with its canonical master-frame ID, master PTS, intended delay, and
cadence. An invalid delay or cadence disables the injector; no production setting
or frame identity is changed.

The delay runs in the existing `obs_add_tick_callback` on the OBS graphics thread,
immediately after the coordinator has observed and dispatched the canonical
MasterFrame. OBS invokes that callback at the beginning of its graphics iteration
([`libobs/obs-video.c:42-56`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video.c#L42-L56)).
The normal `video_sleep` calculation later in that same iteration remains solely
responsible for detecting lateness and emitting any real `obs_vframe_info.count >
1` slots. The injector therefore exercises the existing libobs lag path instead of
simulating duplication in the plugin.

At 60 fps, a 20--25 ms delay is a useful control because it makes the next core
timing calculation late without necessarily producing `count > 1`: libobs floors
the elapsed interval count. A delay above two 16.67 ms intervals (35--40 ms in
practice) is required to request a duplicated-slot event. These are controlled
stress inputs, not a guarantee about host scheduling.

The first NVENC control run at 1920x1080/60 applied 25 ms at a cadence
of 600 master frames. It recorded 13 injection events and continued to emit normal
single-packet A/B validations, but recorded zero graphics-lag counter changes and
zero cadence discontinuities. This is expected control evidence, not a successful
duplicated-slot run and makes no claim about reproducing the same-CTS anomaly. The
subsequent 40 ms NVENC run and x264 comparison established the distinct raw and
texture behaviors documented above.

Baseline context remains important: the previous clean long run processed 133,342
master frames in about 37 minutes with zero observed rendering/encoding lag and an
average render time of roughly 0.5 ms. The injected runs must be compared against
that normal-cadence evidence, not treated as normal operation.

### Canonical logical-slot model — OBS 32.2.1 source trace

`MasterFrame` and a logical video slot are separate identities:

```text
rendered observation X
  -> logical slot X       (rendered; anchor X)
  -> logical slot X + Δ   (repeated; anchor X)  # only when count > 1
next rendered observation X + 2Δ
  -> logical slot X + 2Δ (rendered; anchor X + 2Δ)
```

The source establishes the following.

1. `video_sleep` takes the current core video time as the first slot timestamp,
   calculates `count = floor(elapsed / interval)` when late, advances core video
   time by `count * interval`, and queues `{ timestamp = current_time, count }` to
   every active mix. Thus `count` is the number of logical slots represented by
   the rendered image, not an encoder-local retry count
   ([`obs-video.c:805-860`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video.c#L805-L860)).
2. On the raw path, `video_output_lock_frame` stores that initial timestamp and
   count. Its video thread invokes the encoder once, then increments the cached
   timestamp by the configured frame interval and decrements count. A `count = 2`
   therefore reaches the raw encoder as CTS `X`, then CTS `X + Δ`; larger counts
   continue `X + 2Δ`, `X + 3Δ`, and so on
   ([`video-io.c:126-214`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/media-io/video-io.c#L126-L214),
   [`video-io.c:509-543`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/media-io/video-io.c#L509-L543)).
3. The texture path consumes a lagged record at a separate, later queue boundary.
   For `count = 2` with a pending texture, the first `queue_frame` call increments
   that previous texture's count without changing its timestamp; the second call
   enqueues a new texture with the original `X` timestamp. The GPU worker advances
   the previous texture timestamp only after encoding it
   ([`obs-video.c:442-515`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video.c#L442-L515),
   [`obs-video-gpu-encode.c:60-210`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video-gpu-encode.c#L60-L210)).
   With the usual preceding pending texture `X - Δ`, both the advanced prior texture
   and newly queued texture carry CTS `X`; no texture request carries `X + Δ`.
4. Both paths issue encoder requests with the encoder's local `cur_pts` and
   advance it once after each successful request. Libobs records each
   `encoder_packet_time` with that PTS and the request CTS; output delivery matches
   packet timing by exact PTS, while B-frame reorder can change DTS/order without
   changing this PTS association
   ([`obs-encoder.c:1417-1503`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-encoder.c#L1417-L1503),
   [`obs-encoder.c:1585-1638`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-encoder.c#L1585-L1638),
   [`obs-output.c:1716-1754`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-output.c#L1716-L1754)).

This proves a continuous OBS logical-slot sequence for a stable video
configuration. It does **not** make native encoder PTS a global canonical clock:
`cur_pts` resets when an encoder starts, belongs to one encoder, only advances after
successful requests, and encoder groups coordinate start timestamps rather than a
shared PTS counter. A restart, different frame-rate divisor, failure, or separately
started output creates a new per-encoder epoch. A frame-rate change is likewise an
explicit logical-timeline boundary; this prototype does not infer repeated slots
across it.

The 40 ms x264 run proves the raw-path source model:
x264's `CTS = X + Δ` is an OBS-created repeated logical slot, not an encoder error.
The 40 ms NVENC same-CTS double observations are the proven texture-queue alias
above. The experiment does not reinterpret either behavior through a PTS activation
offset or callback arrival order.

For validation only, the branch now expands the constant-FPS gap between adjacent
rendered `MasterFrame` observations into `LogicalVideoSlot` records. A repeated
slot carries its preceding rendered frame as an anchor and is indexed by its source
derived CTS. Direct CTS matching accepts x264's legitimate
`X + Δ` slot while retaining a repeated same-CTS NVENC packet set as observable
evidence. It is not a production refactor, does not alter `MasterFrame`, and does
not claim that `encoder_pts - activation_base_pts` is safe for synchronization.

The local 1920x1080/60 x264 probe used a 40 ms delay every 300 rendered frames. It
produced 12 cadence discontinuities, 12 increments of `lagged_frames`, and 12
registered repeated logical slots, with zero `has no known OBS logical slot`
diagnostics. Its repeated slots retained distinct CTS values and passed the existing
A/B packet validation.

## Source trace: `video_t` to `obs_encoder_t`

### PROVEN

1. `obs_encoder_set_video` is public and sets the video media/timebase before
   initialization; it rejects changes after initialization or activation
   ([`libobs/obs-encoder.c:1183-1225`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-encoder.c#L1183-L1225)).
2. The internal output lifecycle initializes then starts an encoder.  Its public
   substitute is an `obs_output_t` start operation
   ([`libobs/obs-output.c:2580-2646`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-output.c#L2580-L2646),
   [`2419-2447`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-output.c#L2419-L2447)).
3. A raw/software encoder connection calls `receive_video`.  It sets
   `encoder_frame.pts = encoder->cur_pts`, calls the normal encoder
   `encode` implementation, then advances `cur_pts` by the encoder timebase
   ([`libobs/obs-encoder.c:1585-1640`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-encoder.c#L1585-L1640)).
4. Before sending a received packet, libobs associates it by PTS with the captured
   encoder timing entry.  That entry retains `cts = frame->timestamp`
   ([`libobs/obs-encoder.c:1424-1510`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-encoder.c#L1424-L1510)).
   The public output packet callback receives this `encoder_packet_time`.
5. `obs_encoder_group` is public.  The implementation waits until all group members
   start and assigns one common core-video start timestamp
   ([`libobs/obs-encoder.c:370-403`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-encoder.c#L370-L403),
   [`libobs/obs-video.c:835-850`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video.c#L835-L850)).

This explains the packet model: equal native encoder PTS was observed for
group-started same-FPS outputs at normal cadence, but source does not make it an
atomic cross-view guarantee after activation. It remains a frame-timebase coordinate,
not the absolute nanosecond composition time. CTS is the authoritative join key to
Phase 1.

## Hardware and software paths

### PROVEN

The abstraction supports both paths.  `add_connection` selects texture encoding
when the encoder declares `OBS_ENCODER_CAP_PASS_TEXTURE` and its mix has an NV12 or
P010 texture; otherwise it starts the raw video connection
([`libobs/obs-encoder.c:370-389`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-encoder.c#L370-L389)).

For texture encoders, libobs owns a GPU encode thread per mix, queues texture frames
with the queued mix timestamp, and passes normal `encoder->cur_pts` into
`encode_texture` / `encode_texture2`
([`libobs/obs-video-gpu-encode.c:22-179`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video-gpu-encode.c#L22-L179)).
This replaces the custom graphics-thread NVENC handoff with OBS-owned resource
lifetimes and threading.

The installed 32.2.1 portable runtime enumerated both `obs_x264` and
`obs_nvenc_h264_tex`; the experiment selects the encoder ID through the process
environment variable `OBS_SYNC_REPLAY_EXPERIMENT_ENCODER_ID` without changing the
view/video/output topology.

## Experimental prototype

`NativeObsEncoderExperiment` is deliberately narrow:

```text
MasterFrameCoordinator (obs_get_video_frame_time)
              |
              +-- records {master_frame_id, master_pts_ns}
              |
Gameplay Test -> obs_view A -> video_t A -> obs_encoder_t A -> null_output A
Camera Test   -> obs_view B -> video_t B -> obs_encoder_t B -> null_output B
```

It creates two views, fixes each to the configured scene source, adds both to the
main render loop, attaches one normal registered video encoder to each returned
`video_t`, and groups the video encoders before starting the outputs. The packet
callback validates the packet PTS/timing PTS identity and joins CTS to the recorded
canonical master timestamp. It retains the complete bounded raw set before making
the normal-cadence single-packet/equal-PTS observation; it never pairs packets by
arrival order. Its outputs can run only after the
explicit research activation described above. No custom NVENC code, file output,
muxer, replay buffer, Replay Buffer frontend integration, or scene UI is included.

## Runtime evidence

### x264 — PROVEN normal-cadence run (10 minutes)

Portable OBS 32.2.1, configured at a 1920x1080 base, 1280x720 output, and 60 FPS,
selected `obs_x264` for both outputs. The experiment started after scene collection
load with explicit research activation. Representative structured records were:

```text
master_frame_id=300 master_pts=73223176489222 encoder_pts=239 ... validation=ok
master_frame_id=600 master_pts=73228176489022 encoder_pts=539 ... validation=ok
master_frame_id=900 master_pts=73233176488822 encoder_pts=839 ... validation=ok
```

The run lasted 614 seconds, remained responsive, and produced 122 sampled successful
normal-cadence single-packet validations. The smaller encoder PTS is expected:
activation begins after the master session and encoder PTS is in 1/60-second units.
It was not a graphics-lag stress test and does not prove the unresolved lag model.

### NVENC — PROVEN normal-cadence run (10 minutes)

The same 1280x720/60 output topology used `obs_nvenc_h264_tex` for both outputs.
The run lasted 612 seconds, remained responsive, and produced 122 sampled successful
normal-cadence single-packet validations. No graphics-thread freeze or sustained
rendering lag was observed. The native OBS GPU encode thread handled texture
encoding; the prototype adds no custom encoder worker or graphics-thread wait. The
later preserved NVENC session described above did encounter two lag-boundary
anomalies, so this earlier run is normal-cadence evidence only.

### Scene switching — PROVEN (manual NVENC exercise)

The selected sources are held by `obs_view_set_source`, while Program scene changes
operate on the main view. During the NVENC run, Program scene switching was
exercised manually and the independent native A/B validation continued successfully.
This supports the expected view independence: Program changes affect the main view,
not the selected sources held by the experimental views. It does not replace a future
systematic switching/stress matrix.

### Shutdown lifecycle — VERIFIED (NVENC, x264, and disabled)

The original active-experiment shutdown crash report recorded an access violation in
`audio_output_disconnect` from `end_data_capture_thread`, while
`obs_output_release` in `NativeObsEncoderExperiment::ReleaseResources()` waited to
join that thread during module unload. The preceding lifecycle logs showed that OBS
had already entered scene/audio teardown. It was therefore not safe to wait until
`obs_module_unload()`.

The experiment now uses the earlier public `SCRIPTING_SHUTDOWN` notification and
performs a three-phase output shutdown: remove both packet callbacks, request stop
on both grouped outputs, then release both outputs. Requesting both stops before
releasing either avoids waiting for the first grouped output while its peer is still
active. Only after both capture threads complete does it destroy the group, release
the encoders, and remove/destroy the views.

Portable OBS normal window-close probes completed with exit code 0 for both
`obs_nvenc_h264_tex` and `obs_x264`. Each log contained both output-stop completions,
group/encoder/view cleanup, experiment stop completion, and plugin unload. A disabled
probe also exited with code 0 and logged neither a native-output start nor teardown.
The runtime logs are the acceptance evidence; no crash report was generated by the
fixed probes.

## Recommendation for the main plan

**PUBLIC API SOLUTION:** use a plugin-owned
`OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED` output per fixed output slot, with
`obs_view_t`, returned `video_t`, one normal `obs_encoder_t`, and the public output
start/stop lifecycle. This is production-safe for keeping outputs inactive while
OBS is idle, but public packet/output callbacks cannot create the required
submission-time association for texture encoders.

**MINIMAL INTERNAL DEPENDENCY:** a private libobs fork can carry an opaque source
slot token through `obs-video.c`/`obs-video-gpu-encode.c` and invoke a common raw
and texture input callback before the encoder implementation. It is useful for
proving the association, but its queue-layout and symbol dependencies make it
unsuitable as the plugin's production contract.

**UPSTREAM HOOK REQUIRED:** request the small generic encoder-input hook described
above. Once available, retain the native output adapter, encoder grouping, and
plugin-owned `MasterFrameCoordinator`; map the hook's source token to immutable
`logical_slot_id` and retain that association with every packet. Do not use native
PTS or callback order as a replacement authority.

Discard PR #5's `NvencVideoEncoder`, its direct D3D11/NVENC queueing, custom worker
thread/lifetime ownership, and assumptions that completion order carries frame
identity.  Retain the project's Phase 1 master coordinator and its explicit
diagnostics, adapting downstream records to store native packet timebase metadata.

## Remaining gates

- Repeat both runtime checks at the requested 1920x1080/60 setting. The portable
  profile used by this research run was already configured for 1280x720 output and
  was not modified by the experiment.
- Repeat Program switching between `Gameplay Test`, `Camera Test`, and `Combined
  Reference` as part of the future systematic switching/stress matrix.
- Exercise graphics lag deliberately and prove the production missing-slot policy
  handles every packet set unambiguously, including repeated/missing slots and
  same-CTS multiple observations.
- Run the new per-packet instrumentation through deliberate lag at 1920x1080/60
  with both `obs_nvenc_h264_tex` and `obs_x264`; do not change the production
  synchronization invariant until those raw packet sets explain the anomaly.
- Obtain or upstream the generic input-association hook before implementing the
  production replay adapter; the public video-only output lifecycle is now
  resolved, while the association blocker remains open.
- Implement and verify Replay Buffer-owned native-output start and stop transitions
  before source teardown; the experimental environment flag is not that lifecycle.
