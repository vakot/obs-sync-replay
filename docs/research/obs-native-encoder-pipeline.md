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

### Important public-lifecycle limitation

There is no public direct `obs_encoder_start` API.  Native encoders must be driven
by a normal `obs_output_t`.  OBS 32.2.1's registered `null_output` is AV-only, so
the no-file experiment attaches a stock `ffmpeg_aac` encoder to each null output
only to satisfy its activation contract.  It writes no file, retains no packets,
and performs no audio synchronization work; video is the only observed stream.  A
production video-only replay integration should use a supported video-only output
type or an OBS-supported public extension point rather than making this activation
detail part of the architecture.

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
The existing `MasterFrameCoordinator` intentionally accepts only the observed tick
and logs its cadence discontinuity.  A production integration must reject or make
an explicit paired missing/duplicate-slot decision for any native CTS that cannot be
joined to an accepted `MasterFrame`; it must not infer identity from the duplicate
count or callback order.  This branch's experiment logs that condition as an
invariant 3 failure.

## Graphics lag / duplicated-slot semantics

The following source findings use the pinned OBS Studio 32.2.1 tree. The runtime
evidence below is from the still-running NVENC research session; it was read from
the portable log without restarting OBS.

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
3. The **GPU/texture** path intentionally duplicates its most recent texture when
   a queued frame has `count > 1`. The GPU encode thread decrements `tf.count`,
   increments `tf.timestamp` by its frame interval, and queues the texture again
   until all slots have been emitted
   ([`libobs/obs-video.c:442-515`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video.c#L442-L515),
   [`libobs/obs-video-gpu-encode.c:199-210`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video-gpu-encode.c#L199-L210)).
   Again, the intended repeated-image slots have consecutive CTS values.
4. `encoder->cur_pts` advances once for each successful raw encode request and for
   every texture encode iteration. It is independent of the composition timestamp
   ([`libobs/obs-encoder.c:1623-1638`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-encoder.c#L1623-L1638),
   [`libobs/obs-video-gpu-encode.c:154-194`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video-gpu-encode.c#L154-L194)).
5. For each encode request, libobs records an `encoder_packet_time` keyed by native
   PTS with the supplied raw frame timestamp or texture timestamp as CTS. A packet
   callback receives the timing record selected by exact native PTS match; it is not
   selected by callback order
   ([`libobs/obs-encoder.c:1417-1503`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-encoder.c#L1417-L1503),
   [`libobs/obs-output.c:1716-1754`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-output.c#L1716-L1754)).
6. Each `obs_view_add` mix owns its raw/GPU queues and, for texture encoders, its
   own GPU encode thread. An encoder group gives a common start timestamp; it does
   not make later per-mix queue consumption or `cur_pts` advancement atomic across
   two views.

### INFERRED FROM THE PRESERVED NVENC LOG

Two one-frame graphics-lag events produced the same shape:

```text
21:33:31.488  cadence discontinuity ... lagged_frames=13 master_frame_id=77465
21:33:31.723  single CTS set for master_frame_id=77464: A PTS=77444, B PTS=77445
21:33:31.764/781  additional B then A observations for CTS=80234874040704

21:34:48.764  cadence discontinuity ... lagged_frames=14 master_frame_id=82101
21:34:49.030  single CTS set for master_frame_id=82100: A PTS=82082, B PTS=82081
21:34:49.034/054  additional A then B observations for CTS=80312157370946
```

Later sampled validations returned to the single-packet/equal-PTS normal-cadence
shape, so this was not observed as accumulating drift. The anomaly CTS belongs to
the master slot immediately before the logged cadence discontinuity. This is
consistent with independent queue latency around the lag boundary, but it is not
yet a proven causal explanation.

### UNRESOLVED

- The documented `count` paths advance CTS for each repeated slot, so they do not
  alone explain multiple output callbacks with the **same** CTS. The next run must
  capture every packet, its previous packet, and the complete per-CTS packet set to
  determine whether the duplicate callback reflects separate encode requests,
  output interleaving/timing association, or another queue boundary.
- Whether x264 reproduces the same CTS/PTS shape under deliberate graphics lag is
  untested. Its raw path is source-distinct from NVENC's texture path.
- Public packet timing identifies CTS and native PTS, but does not expose an
  explicit original-versus-duplicated-slot flag or the private `count` value.
- Consequently, equal native encoder PTS is not yet proven as a universal A/B
  invariant, and the current prototype cannot yet claim the product's strong
  reconstruction guarantee under graphics lag.

### Research instrumentation and production direction

The experiment now retains a bounded raw packet set per CTS/master frame instead
of treating a second observation as an immediate invariant failure. Every packet
logs output, PTS, DTS, CTS, keyframe, matching master identity, per-CTS observation
index, and previous per-output PTS/CTS at DEBUG level. A multiple-observation or
single-packet PTS mismatch emits one structured record containing all observed A
and B packets for that CTS. Timeline logs carry previous master PTS, configured
interval, cadence delta, and lagged-frame count.

This is instrumentation only: it does not pair by arrival order, repair PTS,
rewrite CTS, fabricate MasterFrames, or silently discard extra packets. The
recommended production direction is to model every canonical temporal slot
explicitly and require both outputs to provide one unambiguous representation for
that slot. Until source-plus-runtime evidence defines the repeated/missing-slot
mapping, a lagged packet set must be retained as a diagnosed, unsupported condition
rather than accepted as approximately synchronized.

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

Replace the proposed custom Phase 4 NVENC submission/backend with a native output
adapter owning, per fixed output slot: `obs_view_t`, returned `video_t`, one normal
`obs_encoder_t`, and a public packet observer.  Use an encoder group for common
activation and keep one plugin-owned `MasterFrameCoordinator` plus a bounded map
from master CTS to immutable master identity.  Replay buffering/muxing must retain
the packet and this explicit association, not only `encoder_packet.pts`.

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
- Confirm a supported production video-only output lifecycle, or document the
  smallest accepted OBS extension point, before treating the prototype's
  `null_output` activation detail as a production design.
- Implement and verify Replay Buffer-owned native-output start and stop transitions
  before source teardown; the experimental environment flag is not that lifecycle.
