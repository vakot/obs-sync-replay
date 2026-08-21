# Stock OBS shared-video-timeline experiment

Date: 2026-08-21

Decision: **C. NOT GUARANTEED — stock independent encoder pipelines can begin or progress on different logical slots.**

The experiment did not weaken the required zero-frame skew criterion. It observed
different first packet composition timestamps and unequal packet mappings during
independent activation, including repeated one-frame x264 skew and repeated
multi-frame NVENC skew. The public API still cannot expose the exact source-slot
identity, so packet timing is evidence of the boundary failure rather than a
substitute source-frame identifier.

## Architecture verification

The relevant OBS Studio 32.2.1 paths are:

- `libobs/obs-view.c:143-155`: `obs_view_add` creates a separate core video mix
  with `obs_create_video_mix`; `obs_view_remove` detaches it at `:168-179`.
- `libobs/obs-video.c:808-871`: `video_sleep` creates the root video timestamp
  and distributes the tick to active mixes. Encoder groups can record one
  `start_timestamp` after all grouped encoders have started, but this is a
  boundary on a later root tick, not an atomic two-encoder activation.
- `libobs/obs-video.c:922-958` and `:1131-1148`: the root graphics thread renders
  all active views and then advances the root video clock.
- `libobs/media-io/video-io.c:126-207`: each `video_t` has its own media-IO
  delivery loop. The callback threads consume their mix caches independently,
  even though the root tick timestamps are aligned.
- `libobs/obs-output.c:2424-2452`: stock output capture starts its encoders in
  output/encoder iteration order. `obs_output_start` is the public activation
  route; `obs_encoder_start` is internal in this OBS version.
- `libobs/obs-encoder.c:1641-1695`: raw encoder input is delivered through the
  `video_data` timestamp; grouped encoders wait for their shared group timestamp.
- `plugins/obs-x264/obs-x264.c` and `plugins/obs-nvenc/nvenc-d3d11.c`: the raw
  and texture paths both receive timestamps, but neither exposes a public
  cross-view source-slot identity to this probe.

Therefore:

1. Both views are driven by one root OBS video clock.
2. Their render timestamps originate from the same root cadence, but delivery to
   each `video_t` is through separate callback scheduling.
3. Sequential activation can cross a root tick: A can accept a first input before
   B is activated.
4. Public packet callbacks expose encoder-local PTS/DTS and
   `encoder_packet_time` CTS/FER/FERC/PIR. They do not expose the official
   stock source frame ID required to prove `source_slot_A[n] == source_slot_B[n]`.
5. Grouping provides a shared start timestamp after all grouped encoders are
   active. It does not provide a public atomic activation operation or make
   independent pipelines one encoder timeline.

Official source references: [`obs-view.c`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-view.c), [`obs-video.c`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-video.c), [`video-io.c`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/media-io/video-io.c), [`obs-output.c`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-output.c), [`obs-encoder.c`](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-encoder.c), [`null-output.c`](https://github.com/obsproject/obs-studio/blob/32.2.1/plugins/obs-outputs/null-output.c), [`obs-x264.c`](https://github.com/obsproject/obs-studio/blob/32.2.1/plugins/obs-x264/obs-x264.c), and [`nvenc-d3d11.c`](https://github.com/obsproject/obs-studio/blob/32.2.1/plugins/obs-nvenc/nvenc-d3d11.c).

## Probe topology

The disposable probe creates two plugin-owned views and binds them permanently
to the deterministic bootstrap scenes:

```text
View A -> Scene A -> video_t A -> video encoder A -> stock null_output A
View B -> Scene B -> video_t B -> video encoder B -> stock null_output B
```

It tests `obs_x264` and `obs_nvenc_h264_tex`, with sequential, prepared, and
grouped activation strategies. It writes no media files and uses no custom
encoder, muxer, or patched association API.

The stock `null_output` is flagged audio+video, so the harness attaches two stock
`ffmpeg_aac` encoders to OBS's existing audio output solely to satisfy the
public output contract. Audio packets are discarded from the observation; only
`OBS_ENCODER_VIDEO` packets are recorded.

The probe logs sampled local PTS/DTS, public packet CTS/FER/FERC/PIR, first packet
observations, PTS-to-CTS mapping mismatches, source-PTS gaps, activation strategy,
and `obs_get_lagged_frames()`. The repository has no existing safe lag injector,
so the run records `lag_injector=unavailable`; no OBS patch was added.

## Runtime evidence

Full run configuration:

```text
OBS_SYNC_REPLAY_PROBE_CYCLES=100
OBS_SYNC_REPLAY_PROBE_LONG_RUN_SECONDS=180
OBS_SYNC_REPLAY_PROBE_CYCLE_WARMUP_MS=1000
```

Boundary results, 100 cycles per encoder:

| Encoder / strategy | Cycles | First CTS skew | Packet-map failures |
| --- | ---: | ---: | ---: |
| x264 / sequential | 34 | 9 | 9 |
| x264 / prepared | 33 | 0 | 1 |
| x264 / grouped | 33 | 1 | 1 |
| NVENC / sequential | 34 | 34 | 34 |
| NVENC / prepared | 33 | 4 | 15 |
| NVENC / grouped | 33 | 0 | 9 |

Representative direct observations:

- x264 sequential cycle 1: first packet CTS differed by 16,666,666 ns, one
  60-FPS interval; packet mapping had 23 mismatches.
- x264 sequential cycles 88 and 97 reproduced the same one-frame first-CTS
  skew.
- NVENC sequential cycle 100: first packet CTS differed by 83,333,330 ns and
  the packet mapping had 48 mismatches.
- x264 long run: 10,765 video packets per output over 180 seconds, zero
  packet-map mismatches, zero source-PTS gaps, and zero lagged frames.
- NVENC long run: 10,788 video packets per output over 180 seconds, zero
  packet-map mismatches, zero source-PTS gaps, and zero lagged frames.
- Program-scene changes were issued every five seconds during both long runs;
  every switch logged `plugin_views_unchanged=true`.

The long-run alignment demonstrates stable observed packet cadence after the
pipelines are active. It does not repair or disprove the independently observed
start-boundary skew.

Evidence log: [`2026-08-21 13-53-33.txt`](../obs-dev/config/obs-studio/logs/2026-08-21%2013-53-33.txt).

## Provenance qualification

The probe source uses only public APIs common to stock OBS 32.2.1 and explicitly
logs `patched_association_api_used=false`. However, the ignored local `.deps`
source/SDK used to build this existing `obs-dev` runtime contains pre-existing
`source_frame_id` and input-association additions that are absent from the
official 32.2.1 tag. No such API was called by the probe, but this means the
runtime binary itself is not sufficient to claim a clean stock-binary validation.
The architectural conclusion above is based on the official tag; the runtime
measurements are corroborating evidence and should be repeated after rebuilding
`obs-dev` from an unmodified official 32.2.1 source tree if binary-level stock
provenance is required.

## Direct, inferred, and unobservable

Directly observed:

- independent view/video/output/encoder topology;
- output activation success and order;
- packet-local PTS/DTS;
- public packet CTS/FER/FERC/PIR;
- first observed packet timing and packet-map mismatches;
- long-run packet counts, source-PTS gaps, and OBS lag counters.

Inferred from official stock source:

- one root OBS video timestamp is distributed to all active mixes;
- separate `video_t` callback scheduling can deliver the two pipelines at
  different times;
- sequential activation can miss a root tick for B;
- grouping establishes a later shared timestamp boundary, not atomic activation.

Unobservable through the stock public API:

- the exact source frame/slot ID accepted by each encoder;
- a public atomic two-encoder start operation;
- a public proof that an equal rebased PTS or equal packet CTS represents the
  same source slot rather than two independently delivered inputs with equal
  timestamps.

The required zero-frame guarantee is therefore not available from this topology.

## Plugin-only timeline normalization

This section answers the follow-up question: whether a plugin can normalize two
independently started/stopped stock encoder streams to the exact common OBS timeline
intersection without changing OBS, adding an encoder/muxer, or using a patched
association API.

### 1. Interpretation of the prior skew evidence

The prior `first_observed_input_cts_*` value was the CTS of the first captured video
packet after ordering observations by encoder-local PTS. It was not a direct
observation of the first `video_data` input accepted by an encoder. A packet-map
failure means that the two captures differed in packet presence or in the public
local-PTS-to-CTS map; it is not a source-frame-ID comparison. The x264 one-frame
skews and NVENC multi-frame skews therefore prove an observed activation/packet
boundary difference, but do not independently identify the corresponding source
slots.

The defensible cadence model is: a common root cadence is distributed to both views,
then each pipeline may begin at a different phase and can exhibit encoder-specific
priming/reordering. The long-run equal maps show stable observed cadence in these
runs; they do not prove a global `slot N` identity.

### 2. Exact stock clock domains

The stock root graphics thread initializes `obs->video.video_time` from
`os_gettime_ns()`, ticks sources, renders all active mixes, and advances the root
time in `libobs/obs-video.c:1131-1148`. `video_sleep` at `:808-871` advances the
root timestamp by one or more nominal frame intervals and increments the public
global `obs_get_total_frames()` counter. `obs_get_video_frame_time()` returns the
current root timestamp; it is not a historical timestamp for an arbitrary encoder
input.

`obs_view_add` creates one core video mix and one `video_t` per view in
`libobs/obs-view.c:143-155`. The media-IO loop in
`libobs/media-io/video-io.c:126-207` independently delivers cached frames to each
connected callback. Thus both views inherit root timestamps, but callback delivery
and encoder acceptance are not one shared public queue.

### 3. Publicly observable events

The probe can directly observe only these useful anchors:

- root timestamp, global root frame count, and global lag counter sampled by public
  `obs_get_video_frame_time()`, `obs_get_total_frames()`, and
  `obs_get_lagged_frames()`;
- successful/failed `obs_output_start()` and the order in which the calls are made;
- output packet `PTS`, `DTS`, and public `encoder_packet_time` CTS/FER/FERC/PIR via
  `obs_output_add_packet_callback()`;
- the wall-clock/API-call sample immediately before start or stop and the final
  packet callback after asynchronous stop/flush completes.

None of these is an immutable source slot accepted by encoder A or B. In particular,
the output callback is downstream of encoder buffering and packet reordering.

### 4. Start-anchor derivation

There is no exact public start anchor. Sampling the root time/counter immediately
before each `obs_output_start()` is only a call-time observation. Sampling the root
counter in the first packet callback is later still and can occur after encoder
priming, B-frame reordering, or skipped/repeated media-IO delivery. The public
`obs_encoder_start()` path registers the encoder connection; it does not return the
first accepted `video_data` timestamp.

Prepared initialization removes some setup variability. Grouping can wait until
group members are ready and establish a later shared timestamp in
`libobs/obs-video.c:843-856`, but it is not an atomic public start operation and the
public callback still cannot expose the grouped input slot.

### 5. End-anchor derivation

There is no exact public end anchor either. `obs_output_stop()` is asynchronous;
encoder disconnect and flush can produce packets after the stop call. The last
packet callback identifies the last packet delivered by the output harness, not the
last source slot accepted by the encoder. A final packet may also be reordered with
respect to DTS/PTS, and a missing packet-time association is possible after the
output timing table has drained.

The new probe records root samples before both stop calls and the final local PTS,
but labels them as boundary observations rather than final source-slot IDs.

### 6. Cadence mapping

At 60 FPS, local PTS differences of one nominal frame interval are consistent with a
common cadence. A candidate mapping can subtract each stream's local start PTS and
compare normalized indices, or quantize packet CTS against a sampled root timestamp.
Neither operation proves the absolute root slot: local PTS is assigned by each
encoder, and packet CTS is composition timing carried through the output timing
association. Equal rebased PTS values therefore remain insufficient evidence for
`source_slot_A[n] == source_slot_B[n]`.

### 7. Repeated and dropped-frame behavior

The root scheduler can advance by more than one interval when late, while media-IO
cache delivery can repeat a timestamped frame or report skipped frames. The public
global lag counter describes root lateness, not which view/encoder consumed which
logical slot. The stock public API has no per-view slot ledger that a plugin can use
to distinguish an intentional repeated slot from a missing encoder input.

The probe's `source_pts_gaps` and root lag samples are therefore cadence diagnostics,
not exact slot evidence. This run observed zero lagged frames during both long runs;
that does not make the boundary anchors observable. No deliberate missed/repeated
OBS-frame injection was performed because the repository's existing lag injector is
unavailable; the probe logs `lag_injector=unavailable` and does not patch OBS for it.

### 8. Encoder reordering

Both tested H.264 paths can buffer and reorder frames. Packet DTS/PTS describe the
encoded packet timeline, while the public packet-time CTS/FER/FERC/PIR values describe
output/encoder timing events. A first packet by local PTS can be preceded or followed
by packets whose decode order differs, and stop can flush delayed packets. This makes
packet trimming by PTS alone unsafe for a zero-frame source-slot guarantee.

### 9. Common-range algorithm if exact anchors existed

If each pipeline exposed an immutable accepted source slot, the plugin-only
intersection would be deterministic:

```text
common_start = max(start_slot_A, start_slot_B)
common_end   = min(end_slot_A, end_slot_B)
keep exactly [common_start, common_end] in both streams
```

Every repeated, dropped, or delayed slot would remain represented by its source slot
identity; no later frame would be shifted to fill a hole. The current stock topology
cannot supply `start_slot_*` or `end_slot_*`, so this algorithm cannot be instantiated
with proof from public observations.

### 10. Packet/container normalization options

Stock packet callbacks can observe packets but cannot turn the stock null/output
pipeline into a normalized pair of files. Exact filtering would require a custom
output/mux path, an encoder-side association facility, or post-processing/remuxing.
Those options introduce GOP/keyframe, DTS/PTS, B-frame, codec extradata, and flush
boundary requirements; they are outside this stock-only experiment and cannot
recover an unobserved source slot. Equalizing packet timestamps after the fact would
be a rebasing operation, not proof of the required zero-frame source alignment.

### 11. x264 runtime evidence

Fresh run log: [`2026-08-21 14-26-11.txt`](../obs-dev/config/obs-studio/logs/2026-08-21%2014-26-11.txt).

Across 100 boundary cycles (34 sequential, 33 prepared, 33 grouped), x264 had 11
sequential first-packet CTS skews and 11 packet-map failures; prepared had 0/0; and
grouped had 1/1. Alternating stop order exposed 11 cycles with unequal final local
PTS. The 180-second grouped run produced 10,764 packets per output, zero observed
packet-map mismatches, six aggregate source-PTS-gap diagnostics, and zero root lagged
frames. Start and stop root-counter samples were equal for that long run. These are
consistent with common steady cadence plus unobservable exact boundaries.

### 12. NVENC runtime evidence

The same 100-cycle run produced 34/34 sequential, 6/6 prepared, and 0/0 grouped
first-packet-CTS-skew/packet-map-failure counts. Alternating stop order produced
34/6/0 cycles with unequal final local PTS for sequential/prepared/grouped. The
180-second grouped run produced 10,787 packets per output, zero observed packet-map
mismatches, zero source-PTS-gap diagnostics, and zero root lagged frames. The NVENC
start-call root counters were sometimes separated by several root counts even when
the final packet maps matched; this directly demonstrates that a sampled public root
counter is not an encoder-input identity.

### 13. Remaining uncertainty

Directly proven: the stock topology has one root cadence; independent activation can
produce different observed packet boundaries; and both tested encoders maintained
equal observed packet maps during these long runs. Inferred: the steady-state
pipelines likely consume the same cadence phase after activation in these runs.
Unobservable: the exact first and last source slots accepted by each encoder, every
slot's identity through repeats/drops, and an exact common intersection suitable for
zero-frame file normalization.

The local ignored OBS source/SDK used for this runtime contains pre-existing
association additions, but this probe did not call them. The architecture claims are
based on the official 32.2.1 source paths linked above; runtime evidence should be
repeated with an unmodified official binary if clean-binary provenance is required.

Conclusion: C. BOUNDARIES ARE NOT EXACTLY OBSERVABLE.
