#include "recording/synchronized-recording-session.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace obs_sync_replay;

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

EncodedPacket Packet(const uint64_t source_cts, const bool keyframe = false, const int64_t pts = -1,
                     const int64_t dts = -1) {
    EncodedPacket packet;
    packet.source_cts = source_cts;
    packet.pts = pts < 0 ? static_cast<int64_t>(source_cts / 1000) : pts;
    packet.dts = dts < 0 ? packet.pts : dts;
    packet.timebase_num = 1;
    packet.timebase_den = 60000;
    packet.keyframe = keyframe;
    packet.payload = {static_cast<uint8_t>(source_cts & 0xffU), 0x01, 0x02};
    return packet;
}

PacketStreamConfig StreamConfig() {
    PacketStreamConfig config;
    config.width = 1920;
    config.height = 1080;
    config.timebase_num = 1;
    config.timebase_den = 60000;
    config.extra_data = {0x01, 0x64, 0x00, 0x1f};
    return config;
}

class FakeSink final : public SynchronizedPacketSink {
  public:
    bool open = true;
    bool write = true;
    bool finalize = true;
    size_t max_writes = 0;
    bool opened = false;
    bool aborted = false;
    uint64_t start_cts = 0;
    uint64_t end_cts = 0;
    std::vector<EncodedPacket> packets;

    bool Open(const PacketStreamConfig&, const uint64_t common_start_cts) override {
        opened = open;
        start_cts = common_start_cts;
        return opened;
    }

    bool Write(const EncodedPacket& packet) override {
        if (!write || (max_writes != 0 && packets.size() >= max_writes)) {
            return false;
        }
        packets.push_back(packet);
        return true;
    }

    bool CommitThrough(uint64_t) override {
        return write;
    }

    bool Finalize(const uint64_t common_end_cts) override {
        end_cts = common_end_cts;
        return finalize;
    }

    void Abort() noexcept override {
        aborted = true;
    }
};

void TestPacketOwnershipAndBoundedCapacity() {
    EncodedPacketBuffer buffer(4);
    EncodedPacket packet = Packet(10);
    Require(buffer.Push(packet) == EncodedPacketBufferResult::Retained, "packet must be copied into the buffer");
    packet.payload[0] = 0xff;
    Require(buffer.Find(10)->payload[0] != 0xff, "buffer must own an independent compressed-packet copy");
    Require(buffer.Push(Packet(20)) == EncodedPacketBufferResult::Capacity,
            "bounded packet storage must reject overflow explicitly");
}

void TestCommonStartAndAsymmetricStartup() {
    EncodedPacketBuffer a(1024);
    EncodedPacketBuffer b(1024);
    Require(a.Push(Packet(10, true)) == EncodedPacketBufferResult::Retained, "A startup packet");
    Require(a.Push(Packet(20, true)) == EncodedPacketBufferResult::Retained, "A second startup packet");
    Require(b.Push(Packet(20, true)) == EncodedPacketBufferResult::Retained, "B startup packet");
    const CommonPacketRangeResult result = SelectCommonStart(a, b, 10);
    Require(result.range && result.range->start_cts == 20, "common start must discard asymmetric startup packets");
}

void TestNoCommonKeyframeAndCommonEnd() {
    EncodedPacketBuffer a(1024);
    EncodedPacketBuffer b(1024);
    Require(a.Push(Packet(10, false)) == EncodedPacketBufferResult::Retained, "A non-keyframe");
    Require(b.Push(Packet(10, true)) == EncodedPacketBufferResult::Retained, "B keyframe");
    Require(SelectCommonStart(a, b, 0).failure == CommonPacketRangeFailure::NoCommonStartKeyframe,
            "missing common keyframe must remain pending");
    Require(a.Push(Packet(20, true)) == EncodedPacketBufferResult::Retained, "A common keyframe");
    Require(b.Push(Packet(20, true)) == EncodedPacketBufferResult::Retained, "B common keyframe");
    Require(a.Push(Packet(30)) == EncodedPacketBufferResult::Retained, "A end packet");
    Require(b.Push(Packet(30)) == EncodedPacketBufferResult::Retained, "B end packet");
    const CommonPacketRangeResult end = SelectCommonEnd(a, b, {20, 20}, 30);
    Require(end.range && end.range->end_cts == 30, "common end must be selected from one A/B intersection");
}

void TestCommonPrefixRejectsSharedLogicalGap() {
    EncodedPacketBuffer a(1024);
    EncodedPacketBuffer b(1024);
    for (const uint64_t source_cts : {100ULL, 110ULL, 130ULL}) {
        Require(a.Push(Packet(source_cts, source_cts == 100)) == EncodedPacketBufferResult::Retained,
                "A common-prefix packet");
        Require(b.Push(Packet(source_cts, source_cts == 100)) == EncodedPacketBufferResult::Retained,
                "B common-prefix packet");
    }
    const std::optional<uint64_t> prefix = SelectCommonPrefixEnd(a, b, 100, 130, 10);
    Require(prefix && *prefix == 110, "shared unresolved CTS gap must block later common packets");
}

void TestReorderedPtsDtsAndIdenticalRange() {
    EncodedPacketBuffer a(1024);
    EncodedPacketBuffer b(1024);
    Require(a.Push(Packet(10, true, 30, 10)) == EncodedPacketBufferResult::Retained, "A reordered packet 10");
    Require(a.Push(Packet(20, false, 10, 20)) == EncodedPacketBufferResult::Retained, "A reordered packet 20");
    Require(b.Push(Packet(10, true, 30, 10)) == EncodedPacketBufferResult::Retained, "B reordered packet 10");
    Require(b.Push(Packet(20, false, 10, 20)) == EncodedPacketBufferResult::Retained, "B reordered packet 20");
    const CommonPacketRangeResult validation = ValidateCommonPacketRange(a, b, {10, 20});
    Require(validation.range && validation.mismatch_count == 0,
            "reordered PTS/DTS must validate by source CTS and mux in decode order");
    const std::vector<EncodedPacket> sorted = SortForDecodeOrder(SelectPackets(a, {10, 20}));
    Require(sorted.front().source_cts == 10 && sorted.back().source_cts == 20,
            "decode-order sort must not change source-CTS identity");
}

SynchronizedRecordingConfig StreamingConfig(const uint64_t reorder_safety_cts = 10) {
    SynchronizedRecordingConfig config;
    config.pre_roll_capacity_bytes = 1024;
    config.tail_capacity_bytes = 128;
    config.reorder_safety_cts = reorder_safety_cts;
    config.max_start_wait_cts = 100000;
    return config;
}

void StartStreamingSession(SynchronizedRecordingSession& session) {
    Require(session.Start(100), "streaming session start request");
    Require(session.SubmitPacket(RecordingStream::A, Packet(100, true)), "streaming A start packet");
    Require(session.SubmitPacket(RecordingStream::B, Packet(100, true)), "streaming B start packet");
}

void TestIncrementalCommonPrefixAndAsymmetricArrival() {
    auto sink_a = std::make_unique<FakeSink>();
    auto sink_b = std::make_unique<FakeSink>();
    FakeSink* sink_a_view = sink_a.get();
    FakeSink* sink_b_view = sink_b.get();
    SynchronizedRecordingSession session(StreamingConfig(20), StreamConfig(), StreamConfig(), std::move(sink_a),
                                          std::move(sink_b));
    StartStreamingSession(session);
    Require(session.SubmitPacket(RecordingStream::A, Packet(110)), "A must accept ahead packet");
    Require(session.SubmitPacket(RecordingStream::A, Packet(120)), "A must accept second ahead packet");
    Require(sink_a_view->packets.empty() && sink_b_view->packets.empty(),
            "one stream temporarily ahead must not advance the common prefix");
    Require(session.SubmitPacket(RecordingStream::B, Packet(110)), "B catches up to first packet");
    Require(sink_a_view->packets.empty() && sink_b_view->packets.empty(),
            "watermark must retain the safety tail before committing");
    Require(session.SubmitPacket(RecordingStream::B, Packet(120)), "B catches up to second packet");
    Require(sink_a_view->packets.size() == 1 && sink_b_view->packets.size() == 1 &&
                sink_a_view->packets.front().source_cts == 100 && sink_b_view->packets.front().source_cts == 100,
            "incremental commit must advance only the proven common prefix");
    Require(session.RequestStop(120), "streaming stop request");
    Require(session.CompleteDrain(), "streaming drain must finalize");
    Require(sink_a_view->packets.size() == sink_b_view->packets.size(),
            "incremental A/B packet counts must remain equal");
}

void TestUnsafePrefixAndReorderedStop() {
    auto sink_a = std::make_unique<FakeSink>();
    auto sink_b = std::make_unique<FakeSink>();
    FakeSink* sink_a_view = sink_a.get();
    FakeSink* sink_b_view = sink_b.get();
    SynchronizedRecordingSession session(StreamingConfig(10), StreamConfig(), StreamConfig(), std::move(sink_a),
                                          std::move(sink_b));
    StartStreamingSession(session);
    Require(session.SubmitPacket(RecordingStream::A, Packet(110, false, 30, 20)), "A packet 110");
    Require(session.SubmitPacket(RecordingStream::B, Packet(110, false, 30, 20)), "B packet 110");
    Require(session.SubmitPacket(RecordingStream::A, Packet(120, false, 10, 30)), "A packet 120");
    Require(session.SubmitPacket(RecordingStream::A, Packet(130, false, 40, 40)), "A packet 130");
    Require(session.SubmitPacket(RecordingStream::B, Packet(130, false, 40, 40)), "B packet 130");
    Require(sink_a_view->packets.size() == 2 && sink_b_view->packets.size() == 2,
            "watermark must not skip B's temporarily missing CTS 120");
    Require(sink_a_view->packets.back().source_cts == 110 && sink_b_view->packets.back().source_cts == 110,
            "unsafe source CTS must remain in the unresolved tail");
    Require(session.SubmitPacket(RecordingStream::B, Packet(120, false, 10, 30)),
            "late-but-uncommitted reordered packet must be accepted");
    Require(session.RequestStop(130), "reordered stop request");
    Require(session.CompleteDrain(), "reordered stop must finalize");
    Require(session.selected_range() && session.selected_range()->end_cts == 130,
            "exact commonEndCTS must include the final common source CTS");
    Require(sink_a_view->packets.size() == sink_b_view->packets.size(),
            "reordered stop must preserve equal output packet counts");
}

void TestLongLogicalTimelineHasBoundedTail() {
    auto sink_a = std::make_unique<FakeSink>();
    auto sink_b = std::make_unique<FakeSink>();
    SynchronizedRecordingSession session(StreamingConfig(33'333'334), StreamConfig(), StreamConfig(),
                                          std::move(sink_a), std::move(sink_b));
    StartStreamingSession(session);
    constexpr uint64_t kFrameInterval = 16'666'667;
    constexpr uint64_t kThreeHours = 3ULL * 60ULL * 60ULL * 60ULL;
    for (uint64_t frame = 1; frame <= kThreeHours; ++frame) {
        const uint64_t source_cts = 100 + frame * kFrameInterval;
        Require(session.SubmitPacket(RecordingStream::B, Packet(source_cts)), "long-run B packet");
        Require(session.SubmitPacket(RecordingStream::A, Packet(source_cts)), "long-run A packet");
    }
    Require(session.RequestStop(100 + kThreeHours * kFrameInterval), "long-run stop request");
    Require(session.CompleteDrain(), "long-run drain");
    const SynchronizedRecordingMetrics metrics = session.metrics();
    Require(metrics.tail_bytes_a == 0 && metrics.tail_bytes_b == 0, "finalized session must release unresolved tails");
    Require(metrics.peak_tail_bytes_a <= 16 && metrics.peak_tail_bytes_b <= 16,
            "long-run per-stream tail memory must remain bounded");
    Require(metrics.peak_retained_bytes <= 32, "long-run compressed tail must remain bounded");
    Require(session.selected_range() && session.selected_range()->end_cts == 100 + kThreeHours * kFrameInterval,
            "long-run common end must remain exact");
}

void TestStreamingWriteFailureAndTransactionalFinalize() {
    auto sink_a = std::make_unique<FakeSink>();
    auto sink_b = std::make_unique<FakeSink>();
    FakeSink* sink_a_view = sink_a.get();
    FakeSink* sink_b_view = sink_b.get();
    sink_a_view->max_writes = 1;
    SynchronizedRecordingSession write_failure(StreamingConfig(), StreamConfig(), StreamConfig(), std::move(sink_a),
                                                std::move(sink_b));
    StartStreamingSession(write_failure);
    Require(write_failure.SubmitPacket(RecordingStream::A, Packet(110)), "partial write A packet");
    Require(write_failure.SubmitPacket(RecordingStream::B, Packet(110)), "partial write B packet");
    Require(write_failure.SubmitPacket(RecordingStream::A, Packet(120)), "partial write A second packet");
    Require(!write_failure.SubmitPacket(RecordingStream::B, Packet(120)),
            "writer failure after partial streaming must fail the transaction");
    Require(write_failure.state() == SynchronizedRecordingState::Failed && sink_a_view->aborted &&
                sink_b_view->aborted,
            "partial streaming failure must abort both outputs");

    auto finalize_a = std::make_unique<FakeSink>();
    auto finalize_b = std::make_unique<FakeSink>();
    FakeSink* finalize_a_view = finalize_a.get();
    FakeSink* finalize_b_view = finalize_b.get();
    finalize_b_view->finalize = false;
    SynchronizedRecordingSession finalize_failure(StreamingConfig(), StreamConfig(), StreamConfig(),
                                                  std::move(finalize_a), std::move(finalize_b));
    StartStreamingSession(finalize_failure);
    Require(finalize_failure.SubmitPacket(RecordingStream::A, Packet(110)), "finalize A packet");
    Require(finalize_failure.SubmitPacket(RecordingStream::B, Packet(110)), "finalize B packet");
    Require(finalize_failure.RequestStop(110), "finalize failure stop request");
    Require(!finalize_failure.CompleteDrain(), "finalization failure must be reported");
    Require(finalize_failure.state() == SynchronizedRecordingState::Failed && finalize_a_view->aborted &&
                finalize_b_view->aborted,
            "finalization failure must abort both outputs transactionally");
}

void TestUnresolvedTailOverflowFailsExplicitly() {
    SynchronizedRecordingConfig config = StreamingConfig(1'000'000);
    config.tail_capacity_bytes = 3;
    auto sink_a = std::make_unique<FakeSink>();
    auto sink_b = std::make_unique<FakeSink>();
    SynchronizedRecordingSession session(config, StreamConfig(), StreamConfig(), std::move(sink_a), std::move(sink_b));
    StartStreamingSession(session);
    Require(!session.SubmitPacket(RecordingStream::A, Packet(110)),
            "unresolved tail overflow must reject the packet");
    Require(session.state() == SynchronizedRecordingState::Failed &&
                session.failure() == SynchronizedRecordingFailure::BufferCapacity,
            "tail overflow must fail explicitly rather than evicting history");
}

void TestTransactionalStartStopAndFailureRollback() {
    auto sink_a = std::make_unique<FakeSink>();
    auto sink_b = std::make_unique<FakeSink>();
    FakeSink* sink_a_view = sink_a.get();
    FakeSink* sink_b_view = sink_b.get();
    sink_b_view->open = false;
    SynchronizedRecordingSession session({}, StreamConfig(), StreamConfig(), std::move(sink_a), std::move(sink_b));
    Require(session.Start(10), "session start request");
    Require(session.SubmitPacket(RecordingStream::A, Packet(10, true)), "A start packet accepted");
    Require(!session.SubmitPacket(RecordingStream::B, Packet(10, true)), "one-sided sink setup must fail transactionally");
    Require(session.state() == SynchronizedRecordingState::Failed && sink_a_view->aborted,
            "failed startup must roll back both outputs");
}

void TestStopDrainStateTransitions() {
    auto sink_a = std::make_unique<FakeSink>();
    auto sink_b = std::make_unique<FakeSink>();
    FakeSink* sink_a_view = sink_a.get();
    FakeSink* sink_b_view = sink_b.get();
    SynchronizedRecordingSession session({}, StreamConfig(), StreamConfig(), std::move(sink_a), std::move(sink_b));
    Require(session.Start(10), "recording start request");
    Require(session.SubmitPacket(RecordingStream::A, Packet(10, true)), "A common start");
    Require(session.SubmitPacket(RecordingStream::B, Packet(10, true)), "B common start");
    Require(session.SubmitPacket(RecordingStream::A, Packet(20)), "A runtime packet");
    Require(session.SubmitPacket(RecordingStream::B, Packet(20)), "B runtime packet");
    Require(session.RequestStop(20), "stop must enter draining state");
    Require(session.state() == SynchronizedRecordingState::Draining, "stop must not finalize immediately");
    Require(session.CompleteDrain(), "drain must finalize both outputs");
    Require(session.state() == SynchronizedRecordingState::Stopped, "drain must reach stopped state");
    Require(session.selected_range() && session.selected_range()->start_cts == 10 &&
                session.selected_range()->end_cts == 20,
            "A/B must finalize the identical selected source range");
    Require(sink_a_view->end_cts == sink_b_view->end_cts, "both sinks must receive one common end CTS");

    Require(session.RequestStop(30), "repeated stop on stopped session must be idempotent");
    Require(session.CompleteDrain(), "repeated drain on stopped session must be idempotent");
    session.Abort();
    Require(session.state() == SynchronizedRecordingState::Stopped,
            "abort after successful finalization must not regress stopped state");
}

void TestShutdownIsSafeWhileStartingAndAfterFailure() {
    auto starting_sink_a = std::make_unique<FakeSink>();
    auto starting_sink_b = std::make_unique<FakeSink>();
    SynchronizedRecordingSession starting({}, StreamConfig(), StreamConfig(), std::move(starting_sink_a),
                                          std::move(starting_sink_b));
    Require(starting.Start(10), "starting session request");
    Require(starting.RequestStop(10), "shutdown stop while starting must be harmless");
    Require(starting.state() == SynchronizedRecordingState::Starting,
            "shutdown stop while starting must not invent a common range");
    Require(!starting.CompleteDrain(), "starting session cannot drain without a common start");
    starting.Abort();
    Require(starting.state() == SynchronizedRecordingState::Failed, "starting shutdown must abort explicitly");
    Require(!starting.RequestStop(20), "stop on failed session must remain a no-op failure");
    starting.Abort();
    Require(starting.state() == SynchronizedRecordingState::Failed,
            "repeated abort on failed session must be idempotent");
}

} // namespace

int main() {
    TestPacketOwnershipAndBoundedCapacity();
    TestCommonStartAndAsymmetricStartup();
    TestNoCommonKeyframeAndCommonEnd();
    TestCommonPrefixRejectsSharedLogicalGap();
    TestReorderedPtsDtsAndIdenticalRange();
    TestIncrementalCommonPrefixAndAsymmetricArrival();
    TestUnsafePrefixAndReorderedStop();
    TestLongLogicalTimelineHasBoundedTail();
    TestStreamingWriteFailureAndTransactionalFinalize();
    TestUnresolvedTailOverflowFailsExplicitly();
    TestTransactionalStartStopAndFailureRollback();
    TestStopDrainStateTransitions();
    TestShutdownIsSafeWhileStartingAndAfterFailure();
    return EXIT_SUCCESS;
}
