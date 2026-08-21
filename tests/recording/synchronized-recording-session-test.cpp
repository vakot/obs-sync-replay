#include "recording/synchronized-recording-session.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
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
        if (!write) {
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
}

} // namespace

int main() {
    TestPacketOwnershipAndBoundedCapacity();
    TestCommonStartAndAsymmetricStartup();
    TestNoCommonKeyframeAndCommonEnd();
    TestReorderedPtsDtsAndIdenticalRange();
    TestTransactionalStartStopAndFailureRollback();
    TestStopDrainStateTransitions();
    return EXIT_SUCCESS;
}
