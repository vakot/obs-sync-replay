# Stock OBS shared-video-timeline experiment

Date: 2026-08-21

Previous startup-boundary result: stock independent encoder pipelines can begin or progress on different logical slots; packet-level CTS analysis is documented below.

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

The previous startup-boundary conclusion remains valid for physical encoder
activation and private source-frame IDs. The follow-up CTS experiment below refines
the packet-level result: stock packet timing can expose the absolute source timestamp
for packets whose timing association is present.

## Common steady-state epoch and packet-only normalization

This follow-up tests a different product boundary: encoders remain continuously
running, the plugin establishes an epoch from a public root-tick callback, and a
packet-owned range layer selects common source timestamps for Recording or Replay.

### 1. Possible stock synchronization primitives

The available public mechanisms are:

- `obs_add_tick_callback()`, which runs during the root graphics-thread tick before
  source/render processing; the callback can sample
  `obs_get_video_frame_time()` and `obs_get_total_frames()`;
- `obs_encoder_group_t`, which coordinates startup and grouped reconfiguration;
- `obs_encoder_update()`, which can request an encoder settings update and, for a
  group, schedules reconfiguration at a group alignment point;
- `obs_output_start()`/`obs_output_stop()` and
  `obs_output_add_packet_callback()`;
- encoder settings such as keyframe interval and the public `encoder_packet.keyframe`
  flag.

There is no stock public `request_keyframe` or synchronous reset-to-this-root-tick
API. A keyframe request on all encoders cannot be assumed atomic; this probe did not
pretend that one exists. Group reconfiguration is a coordination mechanism, not a
public callback that returns the exact source tick at which all encoders accepted a
frame.

### 2. Exact common epoch

An exact packet epoch can be established after both encoders are running by executing
the epoch marker in `obs_add_tick_callback()` and recording the current root video
timestamp. The marker is a root OBS timestamp, not a worker-thread wall-clock guess.
Packets are admitted only when their public `encoder_packet_time.cts` is at or after
that epoch. If either packet lacks timing metadata, the range must fail validation
rather than infer identity from callback order.

The epoch is exact at the packet source-timestamp level. It is not an atomic request
to both encoder implementations; the proof comes from the packet CTS values that
are later observed, not from the act of requesting the marker.

### 3. Tie to one root OBS slot

Official 32.2.1 `obs-video.c` assigns the current root timestamp to each video mix;
the raw encoder path passes the frame timestamp into `do_encode`, and the texture
encoder path records the texture frame timestamp in the encoder packet timing entry.
The official `obs-encoder.c` then associates that timing entry with the encoded
packet by encoder-local PTS. Therefore `encoder_packet_time.cts` is the absolute
source timestamp for that encoded packet, while local PTS remains only the encoder
ordering coordinate.

The public timestamp does not expose OBS's private source-frame ID, but at fixed
60-FPS it is an exact root-slot coordinate: the slot is identified by its immutable
root timestamp, including a repeated slot's distinct timestamp when OBS advances a
late frame by multiple intervals.

### 4. Packet metadata after the epoch

The packet callback exposes packet data, PTS, DTS, timebase, keyframe status, and
`encoder_packet_time` CTS/FER/FERC/PIR. CTS is the source-slot coordinate; FER/FERC
describe encoder processing; PIR describes output interleave timing. DTS must govern
decode-order muxing, while CTS/source timestamp governs product-range membership.

B-frame reordering is therefore not an identity ambiguity when timing metadata is
present: packets may arrive or decode in a different order, but their source CTS is
still the range key. Missing packet timing, duplicate CTS values, or a source-CTS
set mismatch is an explicit synchronization failure.

### 5. x264 result

Fresh continuous run log: [`2026-08-21 16-34-42.txt`](../obs-dev/config/obs-studio/logs/2026-08-21%2016-34-42.txt).

The x264 run collected 360 root-tick epoch attempts; 358 were usable. All 358 had
equal first packet CTS values, all 358 first CTS values exactly equaled the sampled
root epoch, and all 358 had a common keyframe CTS. Over 180 seconds, both outputs
had 10,765 packets, zero source-CTS mismatches, zero local packet-map mismatches,
four source-PTS gap diagnostics, and zero lagged frames.

### 6. NVENC result

The NVENC run collected 360 root-tick epoch attempts; 359 were usable. All 359 had
equal first packet CTS values, all 359 first CTS values exactly equaled the sampled
root epoch, and 358 had a common keyframe CTS. Over 180 seconds, both outputs had
10,785 packets, zero source-CTS mismatches, zero local packet-map mismatches, four
source-PTS gap diagnostics, and zero lagged frames.

The missing one or two epoch attempts were not repaired or guessed; they were
excluded as unusable because the post-epoch packet observation did not provide both
required packet records.

### 7. Keyframe and GOP analysis

The probe found no public stock API for forcing a keyframe synchronously. Natural or
encoder-scheduled keyframes can nevertheless be compared by their exact source CTS;
the run observed common keyframe CTS for nearly every usable epoch. A plugin can
choose a common decodable keyframe after observing it, or retain earlier dependency
packets as codec pre-roll if the chosen mux/container/player semantics are explicitly
validated.

An arbitrary visible start inside a GOP is not automatically independently decodable.
An arbitrary visible end can also require packets whose decode dependencies have a
later presentation timestamp. Without re-encoding, the robust portable guarantee is
to select a common keyframe/GOP boundary. Exact arbitrary frame-range presentation
requires tested decode-only pre-roll/discard semantics; it cannot be assumed from
PTS rebasing.

### 8. Recording design

Recording can use a compressed pre-roll ring per output. The plugin records the root
epoch CTS, admits only packets whose source CTS belongs to the validated common
range, and muxes packets in DTS order. At the end, it marks one root end CTS, keeps
accepting packets until encoder/output drain completes, discards packets after the
common end, and finalizes both files from the same source-CTS range.

The writer must fail closed if timing metadata is missing, if the two source-CTS
sets differ, or if codec dependency handling cannot make the selected boundary
decodable. No pixel readback or second video encode is needed.

### 9. Replay design

Replay uses the same packet ring and source-CTS index. Save Replay selects one
`[common_start_cts, common_end_cts]` interval for both outputs, retains packets in
decode order plus any explicitly supported codec pre-roll, and submits each packet
once to the muxer. The two files must report the same selected source-CTS set and
frame count before the save is accepted.

### 10. Common-end design

The plugin can mark `common_end_cts` from a root tick while encoders continue. It
must not use stop-call time or callback arrival order as the end. It stops or pauses
acceptance only after sufficient encoded packets have arrived and the encoder/output
flush has drained, then discards every packet with source CTS after the marker.

The last visible frame is exact at the packet-source level when both streams contain
the same final CTS. For independently decodable files, the end must also satisfy
codec dependency rules; otherwise the plugin selects the earlier common safe GOP
boundary or rejects the requested exact save.

### 11. Muxing choice

Stock `obs_output_t` packet callbacks are observers, not packet filters: stock output
continues its own interleave/mux path after invoking callbacks. They cannot enforce a
common range in a normal stock Recording or Replay file.

The production path therefore needs a plugin-owned compressed-packet muxer, most
naturally libavformat/MKV, using `obs_encoder_packet_ref()`/release ownership rules,
encoder extradata, packet timebases, DTS ordering, and the validated source-CTS
range. This is remuxing, not re-encoding. A stock output can remain a disposable
probe sink, but not the enforcement point.

### 12. Resource cost

The path requires packet references/copies, a small metadata index, and a bounded
compressed ring. It performs no continuous GPU-to-CPU raw-frame readback and adds no
second encoder. At a nominal 10,000 kbps stream, one second of compressed buffering
is approximately 1.25 MB per output before container/metadata overhead; a 3-second
pre-roll for two outputs is approximately 7.5 MB. Muxing cost is packet interleave
and container I/O, with latency determined by the configured pre-roll and codec
drain depth rather than another encode.

### 13. Remaining ambiguity

Exact packet source timestamps are now directly observable through public stock
`encoder_packet_time.cts`, and the runtime evidence supports a common steady-state
epoch for x264 and NVENC. Remaining constraints are: packets with missing timing,
source slots for which an encoder emits no packet, root/packet observation failure,
codec dependency packets outside an arbitrary visible range, and player/container
behavior for decode-only pre-roll or discard semantics.

The safe product contract is therefore: enforce equal source-CTS sets and equal
first/last CTS values, fail closed on any mismatch, and use a common decodable
keyframe/GOP boundary unless the chosen libavformat/MKV playback contract proves
arbitrary-frame pre-roll/end handling.

The preceding architecture experiment established that common decodable
keyframe/GOP boundaries are the safe stock-OBS product boundary. The packet-range
POC below tests that boundary end to end.

## Synchronized packet-range MKV POC

This POC uses the existing clean deterministic Scene A/Scene B bootstrap and the
same two plugin-owned OBS views/video pipelines. It creates two stock native video
encoders, captures their compressed packets through stock `null_output` packet
callbacks, selects one common source-CTS interval, and writes two separate MKV
files with in-process libavformat. It does not use the patched association API,
custom video encoders, decoding, re-encoding, or raw-frame CPU readback.

### 1. Packet capture topology

```text
Scene A -> plugin-owned view/video_t -> stock encoder A -> stock null_output -> packet capture A
Scene B -> plugin-owned view/video_t -> stock encoder B -> stock null_output -> packet capture B
                                                               |                    |
                                                               `-> common CTS range -'
                                                                      |
                                                               libavformat/MKV A/B
```

The POC runs independently for `obs_x264` and `obs_nvenc_h264_tex`. A/B settings
are created explicitly and identically within each encoder run: 4000 kbps CBR,
one-second keyframe interval, H.264 High profile, x264 `ultrafast` or NVENC `p1`,
and NVENC two B-frames. The stock null output requires an audio encoder by its
public output contract; two stock AAC encoders are attached only to satisfy that
contract and their packets are discarded. The POC muxes video only.

### 2. Packet ownership

The callback receives borrowed OBS packet memory. The POC immediately calls the
public `obs_encoder_packet_ref()` and retains the referenced encoded payload,
packet type, keyframe flag, PTS, DTS, packet timebase, and stream identity until
both MKV files are finalized. It calls `obs_encoder_packet_release()` for every
retained packet during teardown. Codec extradata is copied while the encoder is
active from the packet's public encoder using `obs_encoder_get_extra_data()`;
this is required because the accessor returned no data after stop in the first
implementation attempt. No borrowed pointer survives the callback.

### 3. Common-start algorithm

Both encoders are started, then allowed to warm up for two seconds while all
compressed packets are retained. The requested recording start is a root OBS CTS
sampled after warm-up. `commonStartCTS` is the first CTS at or after that request
for which both streams have a packet and both packets carry the keyframe flag.
Packets before it are startup/pre-roll and are never submitted to either MKV.
The algorithm matches by public source CTS; it does not assume that the first
keyframe from A and B is naturally identical.

### 4. Common-end algorithm

The stop request samples one root OBS CTS before calling `obs_output_stop()` on
either output. Both outputs are then allowed to drain completely. `commonEndCTS`
is the greatest CTS no later than the requested stop for which both captures have
a packet. All packets in the inclusive common CTS interval are selected, including
non-keyframe tail packets. The selection is made from source CTS, never callback
arrival order, and the two selected CTS sets must be identical or the POC fails
closed.

### 5. Keyframe handling

Start is keyframe-aligned because the stock public API has no synchronous
force-keyframe operation. End does not require a keyframe: the selected start
keyframe establishes decoder state, and the final selected packets are retained in
DTS order. The POC records head/tail discard counts and rejects any source-CTS set
mismatch. This satisfies the product allowance to lose a small number of frames
around requested boundaries.

### 6. PTS/DTS/CTS transformation

CTS remains the immutable source-range key and is logged for both streams. The
selected packets are sorted by source encoder DTS for muxing. Each stream's local
PTS/DTS is rebased by that stream's first selected PTS, then rescaled from the OBS
packet timebase into the MKV stream timebase after `avformat_write_header()`.
This last rescale is required because Matroska selected a 1/1000 timebase while
OBS supplied a finer encoder timebase. B-frame decode order is preserved by DTS;
presentation order is verified independently from decoded frame timestamps.

### 7. Muxing implementation

The POC uses in-process libavformat and the Matroska muxer. It creates one H.264
video stream per file, supplies width/height, codec ID, copied encoder extradata,
packet timebase, rebased/rescaled PTS/DTS, payload bytes, and keyframe flags, then
writes the original compressed bitstream once. The repository links the FFmpeg
libraries already shipped by the pinned OBS dependency bundle. No external
`ffmpeg.exe` process is used during recording and no second video encoder is
created.

### 8. x264 results

Evidence log: [`2026-08-21 17-03-13.txt`](../obs-dev/config/obs-studio/logs/2026-08-21%2017-03-13.txt).

The five-second run selected:

```text
commonStartCTS = 22206959549970
commonEndCTS   = 22211526216454
first CTS A/B  = 22206959549970 / 22206959549970
last CTS A/B   = 22211526216454 / 22211526216454
CTS mismatches = 0
head packets   = 120 / 120
tail packets   = 0 / 0
muxed packets  = 275 / 275
```

Independent ffprobe validation reported 275 decoded frames in each file, duration
4.567000 seconds in each file, and identical first/last decoded presentation
timestamps. Decode-only ffmpeg validation returned no errors. DTS was monotonic;
decoded presentation timestamps were monotonic and matched A/B with zero
timestamp mismatches.

### 9. NVENC results

The same code path selected:

```text
commonStartCTS = 22216159549602
commonEndCTS   = 22220909549412
first CTS A/B  = 22216159549602 / 22216159549602
last CTS A/B   = 22220909549412 / 22220909549412
CTS mismatches = 0
head packets   = 120 / 120
tail packets   = 0 / 0
muxed packets  = 285 / 285
```

Independent ffprobe validation reported 285 decoded frames in each file, duration
4.750000 seconds in each file, and identical first/last decoded presentation
timestamps. Decode-only ffmpeg validation returned no errors. Packet PTS appears
out of order when inspected in DTS/decode order, as expected for B-frames; DTS was
monotonic and decoded presentation timestamps were monotonic and matched A/B with
zero timestamp mismatches. No NVENC-specific synchronization path was added.

### 10. ffprobe/decode validation

The four generated files were checked independently after the OBS process ended:

| Pair | Frames A/B | Duration A/B | Packet PTS/DTS check | Decode check | Decoded PTS mismatches |
| --- | ---: | ---: | --- | --- | ---: |
| x264 | 275 / 275 | 4.567000 / 4.567000 s | DTS monotonic; B-frame PTS order expected | passed | 0 |
| NVENC | 285 / 285 | 4.750000 / 4.750000 s | DTS monotonic; B-frame PTS order expected | passed | 0 |

The source-CTS equality is proven by the structured OBS log; decoded frame count,
duration, decode success, DTS ordering, and decoded presentation-timestamp
alignment are proven independently by ffprobe/ffmpeg. Equal rebased PTS alone was
not used as the synchronization proof.

### 11. Resource cost

The x264 run retained 6,484,704 compressed payload bytes across both streams,
including 1,901,370 bytes of startup pre-roll. NVENC retained 6,750,012 bytes,
including 2,000,012 bytes of startup pre-roll. The selected range itself was
275 or 285 packets per stream. The POC performs two stock video encodes, packet
reference/copy buffering, and two compressed MKV muxes; it performs no decode,
raw-frame readback, or second encode. The code also records per-stream mux wall
time. A follow-up three-second run logged x264 mux wall times of 2 ms and 1 ms
for A/B, and NVENC times of 1 ms and 1 ms for A/B.

### 12. Known limitations

- The POC uses a bounded finite capture rather than a long-term replay ring.
- Audio, Replay Buffer integration, normal OBS Recording settings inheritance,
  UI, naming policy, recovery, and production configuration are excluded.
- The selected common end is source-CTS exact for the captured packet interval;
  arbitrary visible starts remain intentionally disallowed unless keyframe/GOP
  semantics are explicitly extended and validated.
- The ignored local OBS SDK/runtime retains the previously documented source/input
  association additions, but the POC does not call those APIs. Stock provenance is
  based on the public API usage and official OBS architecture; a clean official
  binary rebuild remains a separate provenance hardening step.

### 13. Production implications

The end-to-end result supports a production architecture with continuously running
stock encoders, a bounded compressed packet ring per scene, one common CTS range
decision, fail-closed source-CTS validation, and plugin-owned libavformat/MKV
writers. It preserves the required invariant:

```text
first_source_CTS[A] == first_source_CTS[B]
last_source_CTS[A]  == last_source_CTS[B]
frame_count[A]      == frame_count[B]
duration[A]         == duration[B]
relative_skew      == 0 frames
```

The POC demonstrates that this invariant can be enforced without modifying OBS,
adding custom encoders, decoding, re-encoding, or reading raw frames back to the
CPU. Production must retain the same common-CTS and codec-boundary validation and
must reject a save when any required packet metadata or decodable boundary is
missing.

Result: A. PASS — PACKET-ONLY SYNCHRONIZED RECORDING PROVEN
