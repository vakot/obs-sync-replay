# Phase 8 audio research

## Required conclusion

### C. BLOCKED BY STOCK OBS API — STOP

Production audio implementation was intentionally not started. The pinned OBS
Studio 32.2.1 public APIs can provide the normal Program mix and can register a
custom audio source, but they do not provide an independently isolated,
scene-exact audio graph for the required Master + every-scene topology.

The blocker is not audio encoding or packet transport. It is preserving OBS
scene semantics when the same source is present in more than one audio tree,
including the Master tree and an inactive scene stream.

## Required product audio topology

The audio requirement is a configured multi-track topology, not one mixed
stereo track.

### Master output

Master must follow normal OBS Recording audio semantics as closely as possible.
The active OBS Recording configuration is the source of truth for:

- configured track count and ordering;
- enabled tracks and routing;
- sample format and sample rate; and
- audio encoder behavior.

If OBS Recording is configured with six audio tracks, the Master file must
contain tracks 1 through 6. A configured track may be silent, but it must not
be removed merely because no source currently contributes audio.

### Individual scene outputs

Every separately captured scene file must expose the same configured audio-track
structure as Master. For example, with six configured tracks:

```text
Master.mkv   -> tracks 1..6
Gameplay.mkv -> tracks 1..6
Camera.mkv   -> tracks 1..6
```

For each scene and each configured track, the encoded audio must contain only
the audio belonging to that scene under equivalent OBS routing and mixing
semantics. A track may be present and silent. The result must not collapse the
configured structure into one mixed stereo track.

The target topology is therefore:

```text
Master audio mix       -> configured tracks 1..N
Scene A audio mix      -> configured tracks 1..N
Scene B audio mix      -> configured tracks 1..N
...                    -> configured tracks 1..N
                              |
                              v
                     OBS/native audio encoding
                              |
                              v
             encoded audio packets with source timestamps
                              |
                 shared Recording + Replay consumers
```

Recording and Replay must reuse the resulting encoded audio streams. Save
Replay must not decode and re-encode audio.

## Evidence reviewed

The repository pins OBS Studio 32.2.1 at
`.deps/sources/obs-studio-32.2.1`. The relevant public headers and source are:

- `libobs/obs.h`: `obs_get_audio`, raw-audio callbacks,
  `obs_source_get_audio_mix`, `obs_source_add_audio_capture_callback`,
  `obs_encoder_set_audio`, and `obs_output_set_audio_encoder`;
- `libobs/obs-source.h`: the public `obs_source_info.audio_render` callback
  for custom composite sources;
- `libobs/media-io/audio-io.h`: six `MAX_AUDIO_MIXES` slots;
- `libobs/obs-audio.c`: the global audio render graph and mixer output;
- `libobs/obs-scene.c`: the stock scene audio renderer;
- `libobs/obs-source.c`: source audio buffering, source callbacks, and the
  non-exported `obs_source_audio_render` implementation; and
- `libobs/obs-output.c` and `libobs/obs-encoder.c`: output/encoder attachment
  and audio mixer selection.

The corresponding upstream references are the [32.2.1 audio core source](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-audio.c),
[32.2.1 scene source](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-scene.c),
[32.2.1 source implementation](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-source.c),
[source API reference](https://docs.obsproject.com/reference-sources),
[media I/O reference](https://docs.obsproject.com/reference-libobs-media-io), and
[output API reference](https://docs.obsproject.com/reference-outputs).

## Master Program audio

This part is feasible with public APIs, including the required multi-track
shape:

1. `obs_get_audio()` returns the one global OBS `audio_t`.
2. OBS builds that audio output from active canvas channels and global audio
   sources.
3. OBS exposes up to six mixer slots, and stock outputs select the configured
   mixer mask and attach one audio encoder per selected track.
4. A stock encoded output or audio encoder can bind to that `audio_t` and a
   selected mixer index.
5. Program transitions are represented by transition sources and their public
   transition audio rendering path, so the normal Program mix remains under
   OBS's own authority.

The Master path can therefore preserve the configured track ordering, enabled
track set, sample format/rate, encoder settings, and silent configured tracks.
It does not produce a second independent scene mix.

## Per-scene audio findings

### Public source callbacks are not scene-mix callbacks

`obs_source_add_audio_capture_callback` captures raw audio as a source submits
audio through `obs_source_output_audio`. It is useful for individual input
sources, but it is not a callback for the finished audio of a scene. The stock
scene source uses its internal `audio_render` callback to read child source
buffers, apply scene-item visibility and volume, account for show/hide
transitions, and write the scene mix.

The public `obs_source_info.audio_render` member allows a plugin to implement a
custom composite source. However, the core function that invokes a source's
audio renderer, `obs_source_audio_render`, is not exported through the public
API. More importantly, invoking only the scene renderer would not populate the
inactive child-source buffers or reproduce the global audio graph's activation
and buffering decisions. A plugin therefore cannot ask stock OBS to render an
arbitrary inactive scene into an independent six-track `audio_t` using the
public API.

### `obs_source_get_audio_mix` is not an inactive-scene renderer

`obs_source_get_audio_mix` is public, but it copies the source's already
rendered per-mixer buffers. It does not create a render context, activate an
inactive scene, traverse its children, or provide a new scene-specific audio
graph. The six mixer slots are track-shaped outputs of the one global graph,
not six independent scene graphs.

### Mixer slots do not create independent scene graphs

OBS exposes mixer masks and raw callbacks. These are useful for selecting
tracks from the one global audio graph, but they do not create separate source
activation or duplication semantics.

The stock audio graph enumerates active trees from every audio-enabled canvas
channel and then enumerates global audio sources. When an individual audio
source occurs more than once in those trees, OBS marks it as duplicated and
adds it as a root node. Root nodes are mixed directly into the requested mixer
outputs rather than once per scene renderer. Consequently, a source that is
present in the Master tree and in one scene wrapper cannot be confined to that
scene's mixer slots by the public mixer-mask APIs.

This is a concrete failure case for the required topology:

```text
Master scene contains Mic
Scene A contains Mic
Scene B does not contain Mic

Required:
  Master = Mic on the configured tracks
  A      = Mic on the configured tracks
  B      = silence on those tracks

Stock duplicated-source root behavior:
  Mic is rendered as a root contribution to enabled mixer slots,
  so a mixer-based wrapper cannot prove B is silent.
```

The same issue applies to shared sources, nested scenes, and sources that
appear through more than one active path. Copying or replacing those sources
would change their stateful ownership and is not equivalent to OBS's shared
source semantics.

### Inactive scenes and global devices

The stock audio graph renders scene children through active source trees. There
is no public audio equivalent of creating a video view that independently
renders an inactive scene. A plugin can activate a scene through a custom
composite source, but doing so places that scene in the same global audio graph
and therefore does not remove the duplicated-source limitation above.

Desktop Audio, Mic/Aux, monitoring deduplication, source mute/volume, sync
offset, filters, and mixer-track selection are source/global audio semantics.
The public API has no scene association for those global sources. Assigning
them to every scene, to Master only, or to a selected scene set would be a
product policy rather than a consequence of OBS Program semantics. The Phase 8
gate forbids choosing that policy implicitly.

## Exact missing capability

The missing capability is not another packet callback or another mixer mask. A
usable OBS API must expose an OBS-owned scene audio render boundary with all of
these properties:

1. Given a scene source and an audio interval, render that scene as a complete
   scene audio graph even when it is not the active canvas scene.
2. Preserve stock scene-item visibility, show/hide transitions, volume, mute,
   filters, sync offsets, nested scenes, and shared-source behavior.
3. Apply the same global-source and source-activation semantics that OBS uses
   for the corresponding Program graph, without making a plugin invent a
   scene-routing policy.
4. Return all requested mixer slots with their OBS timestamps, including
   zero-filled slots for configured tracks with no current contribution.
5. Avoid mutating the live canvas, double-registering a source as a global root,
   or changing source ownership and lifecycle behavior.
6. Define the owning OBS/audio thread, buffering lifetime, reentrancy rules,
   sample format/rate, and failure behavior well enough for one audio interval
   to map to the existing common video timeline.

An API that merely exports `obs_source_audio_render`, exposes another copy of
`obs_source_get_audio_mix`, or exposes raw source callbacks would not satisfy
this contract.

## Architecture direction evaluation

| Direction | Finding | Gate result |
| --- | --- | --- |
| A. Existing public libobs API previously missed | No. `obs_get_audio`, `obs_source_get_audio_mix`, mixer masks, source callbacks, and transition helpers expose or select existing buffers; none renders an inactive scene graph with stock semantics. | Does not solve the blocker. |
| B. Exact plugin-side mixer from public source APIs | Not proven and not safely implementable. It would have to duplicate OBS activation, buffering, filters, nested/shared-source traversal, global devices, transitions, mute/volume, sync offsets, and six-track routing. Source callbacks provide inputs, not the completed scene mix. | Reject for production; approximation is not allowed. |
| C. Small generic OBS API extension | Most promising. Add an OBS-owned scene-audio render/mix boundary with the exact contract above, returning the configured mixer slots and timestamps without requiring a plugin to reimplement libobs audio. | Requires an OBS API change; production remains blocked under the stock-public-API gate. |
| D. Deeper custom mixer | This would duplicate too much OBS audio logic and create a second semantic authority for the product's most synchronization-sensitive media path. | Reject; it is not an acceptable workaround. |

Direction C is the next research target, not an implementation approval. It
must first be specified and validated against the stock scene renderer for
shared sources, nested scenes, global devices, filters, transitions, inactive
activation, all six mixer slots, and source lifecycle/threading behavior.

## Timestamp and encoded packet reuse

The packet portion is feasible independently of the topology blocker:

- `audio_data.timestamp` is an OBS nanosecond timestamp and audio frames have
  a sample-rate-derived duration;
- audio encoders use the selected mixer and the audio sample-rate time base;
- encoded output packet callbacks receive compressed audio and video packets,
  including packet PTS/DTS and timebase information; and
- Recording and Replay could retain those encoded packets in one capture
  session and hand references or copies to both consumers, avoiding
  re-encoding on Save Replay.

This still requires implementation-level validation for encoder delay, packet
boundaries, the selected common video interval, configured silent tracks, and
MKV mux start/end rules. Those are not the reason for the stop; they cannot
compensate for an incorrect per-scene audio mix.

## Synchronization consequence

The existing video design has one Master frame authority and a shared video
timeline. A stock Program audio encoder can share the same OBS media-time
domain, and packet PTS can be mapped to nanoseconds. That is insufficient for
the Phase 8 requirement because scene audio packets would not be known to
represent the correct six-track scene mix.

Introducing an unproven mixer would violate the synchronization rule that
missing or incorrect work must not be concealed by later timing repair. No
production source, audio encoder path, replay packet path, or runtime audio
workaround was added on this branch.

## Next research direction

The next most promising direction is a small generic OBS/libobs extension
(Direction C) that exposes an OBS-owned, inactive-scene audio render boundary.
The extension must return the complete configured mixer-slot array and
timestamps while preserving stock activation and routing semantics. The
plugin-side design can then mirror the proven video topology:

```text
OBS Master scene-audio render  -> configured audio encoders
OBS Scene A scene-audio render -> configured audio encoders
OBS Scene B scene-audio render -> configured audio encoders
                                  |
                       one encoded-packet fan-out
                                  |
                          Recording + Replay
```

Only after that API is available and the comparison tests prove exact behavior
for the cases above may audio implementation begin. Until then, production
audio must remain stopped.
