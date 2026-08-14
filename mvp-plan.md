# MVP: Frame-Perfect Synchronized Multi-Scene Replay Plugin for OBS

## 1. Goal

Build a minimal OBS Studio plugin capable of recording/replay-buffering **two independent OBS scenes into two separate video files while guaranteeing frame-for-frame synchronization between the outputs**.

Example:

```text
Scene A: Gameplay
Scene B: Webcam

Output:
replay_2026-08-14_13-30-00_gameplay.mkv
replay_2026-08-14_13-30-00_webcam.mkv
```

The fundamental invariant is:

```text
outputA.frame[i] corresponds to exactly the same master video tick as outputB.frame[i]
```

For every output frame:

```text
frame_id_A == frame_id_B
PTS_A == PTS_B
```

The plugin must NOT implement synchronization by starting two ordinary independent OBS replay buffers simultaneously.

Instead, both scenes must be driven by a **single shared video timeline and replay coordinator**.

---

# 2. Scope

The MVP should intentionally remain small.

### Required

* OBS Studio plugin.
* Windows first.
* Two configurable OBS scenes.
* Both scenes rendered independently.
* One shared master frame clock/timeline.
* Same output FPS for both scenes.
* Separate video encoder for each scene.
* Hardware NVENC support.
* Shared replay buffer.
* Configurable replay duration.
* One hotkey to save both outputs.
* Two separate MKV files.
* Frame-for-frame synchronization.
* Basic logging and synchronization validation.

### Not required for MVP

* More than two scenes.
* Streaming.
* macOS/Linux support.
* Advanced UI.
* Multiple encoder types.
* Per-output FPS.
* Automatic scene creation.
* Audio mixing UI.
* Source-level recording.
* OBS frontend recording integration.
* Production-ready installer.
* Automatic updates.

Correct synchronization is significantly more important than feature completeness.

---

# 3. Core Architecture

Do NOT model the plugin as:

```text
Scene A
  ↓
OBS Output A
  ↓
Replay Buffer A

Scene B
  ↓
OBS Output B
  ↓
Replay Buffer B
```

This creates two independently operating output pipelines and does not provide the synchronization guarantee required by this project.

The intended architecture is:

```text
                   MASTER VIDEO TIMELINE
                           │
                           │
                    frame_id + PTS
                           │
               ┌───────────┴───────────┐
               │                       │
               ▼                       ▼
          Render Scene A          Render Scene B
               │                       │
               ▼                       ▼
           Texture A               Texture B
               │                       │
               ▼                       ▼
           Encoder A               Encoder B
               │                       │
               └───────────┬───────────┘
                           │
                           ▼
                  SHARED REPLAY BUFFER
                           │
                    Save Replay
                           │
               ┌───────────┴───────────┐
               ▼                       ▼
          gameplay.mkv             webcam.mkv
```

There is only **one logical replay timeline**.

---

# 4. Master Frame Timeline

Create a central coordinator responsible for assigning every video tick:

```cpp
struct MasterFrame {
    uint64_t frame_id;
    uint64_t pts;
};
```

Example at 60 FPS:

```text
frame_id    PTS
1000        16666666667
1001        16683333333
1002        16700000000
1003        16716666667
```

Both scenes must be rendered against the same master frame.

Conceptually:

```cpp
void process_master_frame(uint64_t frame_id, uint64_t pts)
{
    render_scene_a(frame_id, pts);
    render_scene_b(frame_id, pts);
}
```

Never derive Scene B's timestamp independently from Scene A.

---

# 5. Scene Rendering

The user selects two existing OBS scenes:

```text
Scene A: Gameplay
Scene B: Webcam
```

For every master video tick:

```text
Master frame N
      │
      ├── render Scene A → Texture A[N]
      │
      └── render Scene B → Texture B[N]
```

Rendering should happen from the same OBS video timing cycle.

The two scenes do NOT need to have identical resolutions.

Example:

```text
Scene A:
3840 × 2160 @ 60 FPS

Scene B:
1920 × 1080 @ 60 FPS
```

The temporal frame mapping must still remain:

```text
A[0] ↔ B[0]
A[1] ↔ B[1]
A[2] ↔ B[2]
...
```

---

# 6. Encoding

Create two encoder instances.

MVP target:

```text
Encoder: NVIDIA NVENC
Codec: H.264
Container: MKV
```

Conceptually:

```text
Texture A[N]
    ↓
NVENC A
    ↓
EncodedPacket A[N]

Texture B[N]
    ↓
NVENC B
    ↓
EncodedPacket B[N]
```

Both encoded packets belong to the same logical master frame:

```cpp
struct SynchronizedFrame {
    uint64_t frame_id;
    uint64_t pts;

    EncodedPacket output_a;
    EncodedPacket output_b;
};
```

The implementation does not necessarily need to literally store packets using this exact structure.

The important part is preserving this relationship.

---

# 7. Shared Replay Buffer

Do not create two independently controlled replay buffers.

Implement one logical synchronized replay buffer:

```text
SharedReplayBuffer
│
├── frame 1000
│   ├── encoded A
│   └── encoded B
│
├── frame 1001
│   ├── encoded A
│   └── encoded B
│
├── frame 1002
│   ├── encoded A
│   └── encoded B
│
└── ...
```

For a 90-second replay at 60 FPS:

```text
target ≈ 5400 synchronized video frames
```

The implementation may internally store encoded packets independently for efficiency, but they must remain associated with the same shared timeline.

---

# 8. Save Replay

There must be exactly one save operation.

Example hotkey:

```text
Save Synchronized Replay
```

When triggered, the coordinator determines one common range:

```cpp
start_frame = 120000;
end_frame   = 125399;
```

Both files are generated from that exact range.

Expected result:

```text
gameplay.mkv

frame 0    → master frame 120000
frame 1    → master frame 120001
...
frame 5399 → master frame 125399
```

and:

```text
webcam.mkv

frame 0    → master frame 120000
frame 1    → master frame 120001
...
frame 5399 → master frame 125399
```

Do not independently calculate replay start/end timestamps for each output.

---

# 9. Synchronization Invariants

Synchronization is the primary requirement.

The implementation should maintain these conceptual invariants:

```cpp
output_a.frame_count == output_b.frame_count
```

```cpp
output_a[i].master_frame_id ==
output_b[i].master_frame_id
```

and:

```cpp
output_a[i].pts ==
output_b[i].pts
```

for every video frame.

A replay should conceptually represent:

```text
R = [master_frame_start, master_frame_end]
```

Both files are projections of the same `R`.

---

# 10. Dropped/Missing Frames

The plugin must NOT allow the outputs to silently diverge.

Invalid behavior:

```text
Master    A       B

100       A100    B100
101       A101    B101
102       A102    -
103       A103    B103
```

followed by B shifting its frame numbering.

Scene B frame 103 must never become the temporal equivalent of Scene A frame 102.

For the MVP, choose the simplest deterministic strategy that preserves the shared timeline.

Possible strategies include:

```text
duplicate previous rendered frame
```

or:

```text
explicitly represent the missing temporal frame
```

depending on what OBS/libobs and the selected encoder pipeline support cleanly.

The priority is:

```text
NEVER SHIFT THE TIMELINE
```

Log every occurrence.

---

# 11. Replay Validation

Before finalizing a saved replay, validate synchronization metadata.

At minimum verify:

```text
A.start_frame == B.start_frame
A.end_frame   == B.end_frame

A.expected_frames == B.expected_frames
```

During development, maintain detailed diagnostic information:

```text
Replay #14

Start master frame: 120000
End master frame:   125399

Expected frames:    5400

Output A packets:   ...
Output B packets:   ...

Synchronization: OK
```

Any synchronization invariant failure should produce an explicit OBS log error.

Do not silently save a replay reported as synchronized if validation failed.

---

# 12. Minimal UI

Add a small dock/settings panel.

Example:

```text
Synchronized Replay
────────────────────────────

Scene A
[ Gameplay              ▼ ]

Scene B
[ Webcam                ▼ ]

Replay Duration
[ 90 ] seconds

Encoder
[ NVIDIA NVENC H.264    ▼ ]

Output Directory
[ D:\OBS\Replays        ]

Filename A
[ gameplay ]

Filename B
[ webcam ]

[ Start Replay Buffer ]
[ Stop Replay Buffer  ]

Status:
● Running
```

Register an OBS hotkey:

```text
Save Synchronized Replay
```

No complex UI is necessary for the MVP.

---

# 13. Output Naming

Saving a replay should create both files using the same identifier/timestamp.

Example:

```text
2026-08-14_13-45-32_gameplay.mkv
2026-08-14_13-45-32_webcam.mkv
```

This makes the relationship between files obvious.

---

# 14. Audio

Audio is secondary for the initial MVP.

Preferred first implementation:

```text
Gameplay file:
video + selected/main OBS audio

Webcam file:
video only
```

If audio substantially complicates development of the synchronized video pipeline, implement the first prototype without audio.

**Do not compromise video synchronization to implement audio.**

Audio support can be added after the video architecture is proven.

---

# 15. Threading

Encoding may happen asynchronously, but temporal identity must not depend on encoder completion order.

For example:

```text
Master frame 500
    ↓
A500 submitted
B500 submitted

Master frame 501
    ↓
A501 submitted
B501 submitted
```

It is acceptable for the hardware encoder to physically finish:

```text
A500
A501
B500
B501
```

as long as the packets retain their correct PTS/frame relationship.

Synchronization is based on the master timeline, not wall-clock encoder completion time.

---

# 16. MVP Development Phases

## Phase 1 — Plugin Skeleton

Create:

* OBS module;
* CMake/build configuration;
* Windows build;
* OBS logging;
* basic settings storage;
* scene selectors;
* start/stop controls;
* hotkey registration.

Success criterion:

Plugin loads correctly in OBS and can reference two selected scenes.

---

## Phase 2 — Shared Rendering Clock

Implement the master frame coordinator.

Render both scenes for every master video tick.

Initially do not encode anything.

Log:

```text
MASTER 100 → SceneA 100 / SceneB 100
MASTER 101 → SceneA 101 / SceneB 101
MASTER 102 → SceneA 102 / SceneB 102
```

Success criterion:

Both rendered outputs demonstrably share exactly the same frame sequence.

---

## Phase 3 — Dual Encoding

Connect each rendered scene to its own NVENC encoder.

Preserve the shared frame ID/PTS relationship.

Initially write continuous recordings rather than implementing replay buffering.

Success criterion:

Two separate files can be produced where:

```text
A[i] ↔ B[i]
```

for the entire recording.

---

## Phase 4 — Synchronization Test

Create a deterministic test scene.

For example, display a frame counter/timecode visible in both scenes:

```text
FRAME 000001
FRAME 000002
FRAME 000003
...
```

Record several minutes.

Verify:

```text
A frame N == counter N
B frame N == counter N
```

Test:

* 60 seconds;
* 5 minutes;
* 30 minutes.

There must be **zero accumulating drift**.

---

## Phase 5 — Shared Replay Buffer

Replace continuous output with an encoded packet ring buffer.

Implement:

```text
Start Buffer
Stop Buffer
Save Replay
```

Saving must select one master frame interval and mux both streams from it.

Success criterion:

Repeated replay saves produce two files with identical temporal boundaries.

---

## Phase 6 — Stress Testing

Test while:

* OBS preview is enabled;
* OBS preview is disabled;
* GPU load is high;
* gameplay is running;
* camera is active;
* multiple replays are saved;
* buffer has been running for hours.

Specifically look for:

* dropped frames;
* unequal frame counts;
* different starting frames;
* different ending frames;
* timestamp divergence;
* encoder queue problems.

---

# 17. Definition of Done

The MVP is successful only if this test passes.

Configure:

```text
OBS FPS: 60
Replay: 90 seconds

Scene A: synchronization test content A
Scene B: synchronization test content B
```

Run the replay buffer for at least 30 minutes.

Save multiple 90-second replays at arbitrary times.

For every pair:

```text
A.frames == B.frames
```

and:

```text
∀ i:
A[i].master_frame == B[i].master_frame
```

with no accumulating drift.

The two files should behave temporally as though the two scenes had originally been placed side-by-side on one large OBS canvas and then losslessly separated into two synchronized video sequences.

That is the synchronization model this plugin is intended to reproduce.

---

# 18. Guiding Principle

Whenever there is a choice between:

```text
OBS convenience
```

and:

```text
strict shared timeline
```

choose the **strict shared timeline**.

The plugin exists specifically because independently operating recording/replay outputs are insufficient for this use case.

The core abstraction should therefore always remain:

```text
ONE TIMELINE
     │
     ├── Scene A
     └── Scene B
```

not:

```text
TWO OUTPUTS
     │
"try to start them simultaneously"
```
