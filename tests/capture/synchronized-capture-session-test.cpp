#include "capture/synchronized-capture-session.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace obs_sync_replay;

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

EncodedPacket Packet(const uint64_t source_cts, const bool keyframe = false) {
    EncodedPacket packet;
    packet.source_cts = source_cts;
    packet.pts = static_cast<int64_t>(source_cts / 1000);
    packet.dts = packet.pts;
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

class FanoutProbe final : public CapturePacketConsumer {
  public:
    void OnPacket(OwnedCapturedEncodedPacket packet) override {
        packets.push_back(std::move(packet));
    }

    std::vector<OwnedCapturedEncodedPacket> packets;
};

std::unique_ptr<SynchronizedCaptureSession> MakeSession(const size_t capacity = 1024) {
    SynchronizedCaptureConfig config;
    config.ring_capacity_bytes = capacity;
    auto session = std::make_unique<SynchronizedCaptureSession>(config);
    Require(session->RegisterStream(0, "master", StreamConfig()), "register master");
    Require(session->RegisterStream(1, "scene_a", StreamConfig()), "register scene A");
    Require(session->RegisterStream(2, "scene_b", StreamConfig()), "register scene B");
    Require(session->Start(), "start capture session");
    return session;
}

void IngestAll(SynchronizedCaptureSession& session, const uint64_t cts, const bool keyframe = false) {
    for (CaptureStreamId stream = 0; stream < 3; ++stream) {
        Require(session.Ingest(stream, Packet(cts, keyframe)), "ingest synchronized packet");
    }
}

void TestN3CommonRangeAndWatermark() {
    auto session = MakeSession();
    IngestAll(*session, 100, true);
    IngestAll(*session, 110);
    Require(session->common_watermark_cts() && *session->common_watermark_cts() == 110,
            "watermark must be the minimum stream progress");
    const auto snapshot = session->SnapshotCommonRange(10);
    Require(snapshot && snapshot->range.start_cts == 100 && snapshot->range.end_cts == 110,
            "N=3 snapshot must select one common range");
    Require(snapshot->packets.size() == 3 && snapshot->packets[0].size() == snapshot->packets[1].size() &&
                snapshot->packets[1].size() == snapshot->packets[2].size(),
            "all streams must receive the same snapshot cardinality");
}

void TestTemporarilyAheadStreamAndInsufficientHistory() {
    auto session = MakeSession();
    IngestAll(*session, 100, true);
    for (CaptureStreamId stream = 0; stream < 2; ++stream) {
        Require(session->Ingest(stream, Packet(110)), "ahead stream packet");
    }
    Require(session->common_watermark_cts() && *session->common_watermark_cts() == 100,
            "ahead streams must not advance the common watermark");
    Require(!session->SnapshotCommonRange(1'000), "insufficient duration history must remain explicit");
}

void TestKeyframeAndGopSafeEviction() {
    auto session = MakeSession(6);
    IngestAll(*session, 100, true);
    IngestAll(*session, 110);
    IngestAll(*session, 120);
    IngestAll(*session, 130, true);
    IngestAll(*session, 140);
    const auto metrics = session->metrics();
    Require(metrics.evicted_packet_count > 0 && metrics.retained_bytes <= 18,
            "ring eviction must remain bounded");
    const auto snapshot = session->SnapshotCommonRange(1);
    Require(snapshot && snapshot->range.start_cts == 130,
            "oldest retained replay history must begin at a common keyframe");
}

void TestSnapshotOwnershipAndFanout() {
    auto session = MakeSession();
    FanoutProbe recording_consumer;
    FanoutProbe replay_observer;
    Require(session->Subscribe(&recording_consumer), "subscribe recording consumer");
    Require(session->Subscribe(&replay_observer), "subscribe replay consumer");
    IngestAll(*session, 100, true);
    IngestAll(*session, 110);
    const auto snapshot = session->SnapshotCommonRange(1);
    Require(snapshot && recording_consumer.packets.size() == 2 * 3 && replay_observer.packets.size() == 2 * 3,
            "one encoded packet must fan out to recording and replay consumers");
    session->Stop();
    Require(snapshot->packets[0][0]->packet.source_cts == 100,
            "snapshot-owned packet must remain valid after live capture stops");
    Require(session->Unsubscribe(&recording_consumer) && session->Unsubscribe(&replay_observer),
            "consumers must detach independently");
}

void TestNoCommonKeyframe() {
    auto session = MakeSession();
    IngestAll(*session, 100, false);
    IngestAll(*session, 110, false);
    Require(!session->SnapshotCommonRange(1), "no common keyframe must reject replay snapshot");
}

} // namespace

int main() {
    TestN3CommonRangeAndWatermark();
    TestTemporarilyAheadStreamAndInsufficientHistory();
    TestKeyframeAndGopSafeEviction();
    TestSnapshotOwnershipAndFanout();
    TestNoCommonKeyframe();
    return EXIT_SUCCESS;
}
