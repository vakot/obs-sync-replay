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
