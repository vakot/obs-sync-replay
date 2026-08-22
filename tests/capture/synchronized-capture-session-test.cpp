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
    for (size_t track = 0; track < 6; ++track) {
        config.audio_streams.push_back(
            AudioStreamConfig{track, "ffmpeg_aac", 48'000, 2, 160, {}});
    }
    return config;
}

EncodedPacket AudioPacket(const uint64_t source_cts) {
    EncodedPacket packet;
    packet.kind = EncodedPacketKind::Audio;
    packet.source_cts = source_cts;
    packet.pts = static_cast<int64_t>(source_cts / 1'000'000'000ULL * 48'000);
    packet.dts = packet.pts;
    packet.timebase_num = 1;
    packet.timebase_den = 48'000;
    packet.payload = {0x11, 0x22, 0x33};
    return packet;
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
    for (AudioTrackId track = 0; track < 6; ++track) {
        Require(session->RegisterAudioTrack(track, StreamConfig().audio_streams[track]), "register audio track");
    }
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
    // Packets arrive one stream at a time, so the fixture leaves room for the
    // transient fan-in head while still exercising one shared capacity bound.
    auto session = MakeSession(27);
    IngestAll(*session, 100, true);
    IngestAll(*session, 110);
    IngestAll(*session, 120);
    IngestAll(*session, 130, true);
    IngestAll(*session, 140);
    const auto metrics = session->metrics();
    Require(metrics.evicted_packet_count > 0 && metrics.retained_bytes <= 27,
            "ring eviction must remain globally bounded");
    const auto snapshot = session->SnapshotCommonRange(1);
    Require(snapshot && snapshot->range.start_cts == 130,
            "oldest retained replay history must begin at a common keyframe");
}

void TestCapacityUpdateEvictsSharedHistory() {
    auto session = MakeSession(1024);
    IngestAll(*session, 100, true);
    IngestAll(*session, 110);
    IngestAll(*session, 120);
    session->SetRingCapacityBytes(6);
    const auto metrics = session->metrics();
    Require(metrics.retained_bytes <= 6, "runtime capacity update must bound the shared replay history");
    Require(!session->SnapshotCommonRange(1), "capacity update must not fabricate a replay range");
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

void TestSharedConfiguredAudioTracks() {
    auto session = MakeSession();
    IngestAll(*session, 100, true);
    IngestAll(*session, 110);
    for (AudioTrackId track = 0; track < 6; ++track) {
        Require(session->IngestAudio(track, AudioPacket(100)), "ingest audio packet at start");
        Require(session->IngestAudio(track, AudioPacket(110)), "ingest audio packet at end");
    }
    const auto snapshot = session->SnapshotCommonRange(1);
    Require(snapshot && snapshot->audio_streams.size() == 6 && snapshot->audio_packets.size() == 6,
            "snapshot must preserve all configured audio tracks");
    for (size_t track = 0; track < 6; ++track) {
        Require(snapshot->audio_streams[track].mixer_index == track && snapshot->audio_packets[track].size() == 2,
                "audio track order and packet history must be shared across outputs");
    }
    Require(session->common_watermark_cts() && *session->common_watermark_cts() == 110,
            "audio progress must not redefine the common video watermark");
}

} // namespace

int main() {
    TestN3CommonRangeAndWatermark();
    TestTemporarilyAheadStreamAndInsufficientHistory();
    TestKeyframeAndGopSafeEviction();
    TestCapacityUpdateEvictsSharedHistory();
    TestSnapshotOwnershipAndFanout();
    TestNoCommonKeyframe();
    TestSharedConfiguredAudioTracks();
    return EXIT_SUCCESS;
}
