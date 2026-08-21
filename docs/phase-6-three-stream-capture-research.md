# Phase 6 Three-Stream Capture Research

This report evaluates one always-enabled encoded capture foundation for normal
Recording and future Replay. It is a disposable research POC, not UI integration,
the stock Replay Buffer, or a production Recording/Replay session.

## 1. Correct public Master source

The Master stream is encoded directly from `obs_get_video()`. This is the public
libobs root video pipeline and therefore the real Program mix. It is not rebuilt by
compositing Scene A and Scene B in the plugin. The POC creates two additional public
`obs_view_t` pipelines for the independently encoded Scene A and Scene B feeds.

## 2. Transition behavior

The POC uses `obs_frontend_get_current_transition()` and, when needed, creates the
public `fade_transition`. It switches the Program scene through
`obs_frontend_set_current_scene()` on the OBS UI task queue. The runtime requests
Scene A -> Scene B -> Scene A every five seconds with a 750 ms fade.

Decoded signal statistics confirm that Master contains intermediate transition
frames: x264 Master YAVG values span 101 through 120, while Scene A is 101 and
Scene B is 120. NVENC shows the same behavior. This proves the Master is the real
Program output, including transitions.

## 3. Master, Scene A, and Scene B CTS comparability

Each public video output callback captures `encoder_packet_time.cts` as the source
timeline authority, together with packet PTS, DTS, timebase, keyframe state, and a
copy of the encoded payload. A common logical slot exists only when all three
bounded histories contain the same source CTS.

In both final short runs, every selected first and last source CTS was identical
across all three streams and the common range reported zero missing packets. Packet
completion order was not used to assign temporal identity.

## 4. Three-stream start normalization

The three video encoders are placed in one public `obs_encoder_group_t` and started
as one capture topology. The POC still does not claim that physical activation is
an atomic clock edge. It removes startup skew by selecting the latest retained first
CTS and then requiring a common keyframe at or after that point in all three
streams.

The selected start is therefore one immutable range boundary, never three
independently calculated starts.

## 5. Three-stream end normalization

The selected end is the latest CTS that remains present in all three histories up to
the earliest retained last CTS. If a common CTS is missing, the range stops before
that gap and logs the missing-packet count. The same range is passed unchanged to
all three packet-only MKV writers.

## 6. N-stream synchronization implications

The POC generalizes the two-output shape to a fixed array of three indexed streams:
Master, Scene A, and Scene B. The required N-stream coordinator should maintain one
source-CTS index per stream, commit only the common prefix defined by the minimum
watermark, and report a slot-preserving failure when a stream is delayed or missing.
No output may advance independently to conceal a missing slot.

## 7. Encoder packet fan-out

Public output packet callbacks are sufficient for passive packet observation. The
POC has one callback per native encoded output and copies each packet immediately
into a bounded history. This demonstrates that encoded packets can be consumed
without another encode pass. Callback ownership and output lifetime must belong to
one future synchronized capture session.

## 8. One encoder and multiple consumers

This POC did not run simultaneous live Recording and Replay consumers. It proves the
packet source and bounded storage layer, but not the complete shared-consumer
lifecycle. A production design should have one `SynchronizedCaptureSession` own
the encoder/output objects and expose separate consumer subscriptions to committed
packets. Consumer teardown must not stop the shared encoder while another consumer
is active.

## 9. Recording consumer integration

The normal Recording consumer should consume the committed common prefix
incrementally. The existing two-stream recording session would need an indexed
N-stream form with one packet history and one watermark decision per stream. It
must preserve the source CTS through packet association and use the same session
range decisions for every output.

## 10. Replay consumer integration

The future Replay consumer can use the same bounded histories as a ring. Save Replay
would snapshot one common keyframe-aligned CTS range and apply that range to Master,
Scene A, and Scene B. The POC intentionally has no Replay UI, hotkey, stock Replay
Buffer dependency, or full replay lifecycle.

## 11. x264 runtime result

The final 25-second clean run used three native `obs_x264` video encoders at
1920x1080 and 60 fps. The common source range was
`41022697689270..41030781022280`, with zero missing packets and a common keyframe
start. Each stream saw 1,566 packets and retained 1,200 packets in a 20-second
bounded history; retained payload bytes were 10,000,000 per stream. The three MKVs
decoded successfully, each contained 486 video packets, and their PTS/DTS/flag
signatures matched exactly.

## 12. NVENC runtime result

The final 25-second clean run used three native `obs_nvenc_h264_tex` video encoders.
The common source range was `41049847688184..41058131021186`, with zero missing
packets and a common keyframe start. Each stream saw 1,578 packets and retained
1,200 packets; retained payload bytes were 10,000,000 per stream. The three MKVs
decoded successfully, each contained 498 video packets, and their PTS/DTS/flag
signatures matched exactly.

The POC disables B-frame reordering for this packet-only mux experiment. This keeps
the normalized presentation CTS and decode order directly comparable for x264 and
NVENC; it is a POC constraint, not a claim that every production encoder setting
has the same packet-order behavior.

## 13. Long-run result

An earlier clean 120-second x264 run and 120-second NVENC run exercised repeated
Program transitions and bounded histories. Each stream retained 3,600 packets at
approximately 30 MB per stream, with no duplicate CTS values or common-range gaps.
The earlier long run happened before packet-callback extradata capture and was used
for timing, transition, and memory evidence only; its post-stop mux was rejected
because codec configuration was not yet retained. The final short run fixed that
lifecycle issue and validated both x264 and NVENC files.

## 14. Resource implications

The topology requires three native video encoders and three AAC harness encoders in
the current `null_output` POC; audio is only present because that output requires an
audio encoder and is not part of the synchronization claim. A 20-second ring held
about 10 MB of video payload per stream in the tested settings; the 60-second
long-run ring held about 30 MB per stream. Additional consumers should fan out
packet ownership, not introduce additional encoders, unless a consumer requires a
different codec or format.

## 15. Blockers and next research steps

The remaining blocker is the shared consumer model, not the three-stream Master
timeline. The next implementation should introduce an explicit N-stream capture
session, test concurrent Recording plus Replay subscriptions, specify consumer
reference/lifetime rules, and then add audio only after video timing remains proven.
The POC-specific PTS/DTS normalization and zero-B-frame setting also need a formal
encoder/mux contract before they are promoted into production code.

## Conclusion

**B. PARTIAL — MASTER TIMELINE PROVEN, SHARED CONSUMER MODEL NEEDS WORK.** Public
`obs_get_video()` provided the real Program stream, public scene views provided two
independent native encoded streams, common source CTS ranges were proven with zero
relative skew, real transitions were observed in Master, and both x264 and NVENC
three-file MKV runs decoded successfully. The POC did not yet prove one encoder
session serving live Recording and Replay consumers concurrently, so the shared
consumer foundation remains the next scoped design task.
