#include "replay/synchronized-replay-consumer.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
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

PacketStreamConfig StreamConfig() {
    PacketStreamConfig config;
    config.width = 16;
    config.height = 16;
    config.timebase_num = 1;
    config.timebase_den = 1000;
    config.extra_data = {0x01, 0x42, 0xc0, 0x2a, 0xff, 0xe1, 0x00, 0x1c, 0x67, 0x42, 0xc0,
                         0x2a, 0xda, 0x01, 0xe0, 0x08, 0x9f, 0x97, 0x01, 0x6a, 0x02, 0x02,
                         0x02, 0x80, 0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x3c, 0x47,
                         0x8c, 0x19, 0x50, 0x01, 0x00, 0x04, 0x68, 0xce, 0x3c, 0x80};
    for (size_t track = 0; track < 6; ++track) {
        config.audio_streams.push_back(
            AudioStreamConfig{track, "ffmpeg_aac", 48'000, 2, 160, {0x12, 0x10}});
    }
    return config;
}

EncodedPacket Packet(const uint64_t source_cts, const bool keyframe) {
    EncodedPacket packet;
    packet.source_cts = source_cts;
    packet.pts = static_cast<int64_t>(source_cts / 1'000'000);
    packet.dts = packet.pts;
    packet.timebase_num = 1;
    packet.timebase_den = 1000;
    packet.keyframe = keyframe;
    packet.payload = {0x00, 0x00, 0x01, static_cast<uint8_t>(keyframe ? 0x65 : 0x41), 0x88, 0x84};
    return packet;
}

EncodedPacket AudioPacket(const uint64_t source_cts, const AudioTrackId track) {
    EncodedPacket packet;
    packet.kind = EncodedPacketKind::Audio;
    packet.audio_track_index = track;
    packet.source_cts = source_cts;
    packet.pts = static_cast<int64_t>(source_cts / 1'000'000'000ULL * 48'000);
    packet.dts = packet.pts;
    packet.timebase_num = 1;
    packet.timebase_den = 48'000;
    packet.payload = {0x21, 0x10, 0x56, 0xe5};
    return packet;
}

std::vector<std::filesystem::path> Paths(const char* const stem) {
    const std::filesystem::path directory = std::filesystem::temp_directory_path();
    return {directory / (std::string(stem) + "-master.mkv"), directory / (std::string(stem) + "-scene-a.mkv"),
            directory / (std::string(stem) + "-scene-b.mkv")};
}

void RemoveFiles(const std::vector<std::filesystem::path>& paths) {
    for (const auto& path : paths) {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
}

} // namespace

int main() {
    SynchronizedCaptureConfig capture_config;
    capture_config.ring_capacity_bytes = 1024 * 1024;
    SynchronizedCaptureSession capture(capture_config);
    Require(capture.RegisterStream(0, "master", StreamConfig()), "register master");
    Require(capture.RegisterStream(1, "scene_a", StreamConfig()), "register scene A");
    Require(capture.RegisterStream(2, "scene_b", StreamConfig()), "register scene B");
    for (AudioTrackId track = 0; track < 6; ++track) {
        Require(capture.RegisterAudioTrack(track, StreamConfig().audio_streams[track]), "register audio track");
    }
    Require(capture.Start(), "start capture");
    for (CaptureStreamId stream = 0; stream < 3; ++stream) {
        Require(capture.Ingest(stream, Packet(1'000'000'000, true)), "ingest common keyframe");
        Require(capture.Ingest(stream, Packet(2'000'000'000, false)), "ingest common packet");
    }
    for (AudioTrackId track = 0; track < 6; ++track) {
        Require(capture.IngestAudio(track, AudioPacket(1'000'000'000, track)), "ingest audio keyframe");
        Require(capture.IngestAudio(track, AudioPacket(2'000'000'000, track)), "ingest audio packet");
    }

    const auto first_paths = Paths("obs-sync-replay-consumer-test-first");
    const auto second_paths = Paths("obs-sync-replay-consumer-test-second");
    SynchronizedReplayConsumer consumer(capture, 250);
    Require(consumer.RequestSave(first_paths, 1'000'000'000), "start first save");
    Require(!consumer.RequestSave(second_paths, 1'000'000'000), "reject concurrent save");
    Require(consumer.active(), "save must remain active behind test barrier");

    capture.Stop();
    consumer.Wait();
    const auto first_result = consumer.last_result();
    if (first_result && !first_result->success) {
        std::cerr << "first save error: " << first_result->error << '\n';
        for (const auto& stream : first_result->streams) {
            std::cerr << "stream success=" << stream.success << " error=" << stream.error << '\n';
        }
    }
    Require(first_result && first_result->success, "first save must complete after capture stop");
    Require(first_result->range.start_cts == 1'000'000'000 && first_result->range.end_cts == 2'000'000'000,
            "first save must retain one common range");

    auto failed_paths = Paths("obs-sync-replay-consumer-test-missing-directory");
    for (auto& path : failed_paths) {
        path = path.parent_path() / "obs-sync-replay-consumer-test-missing-directory" / path.filename();
    }
    Require(consumer.RequestSave(failed_paths, 1'000'000'000), "start failing save");
    consumer.Wait();
    const auto failed_result = consumer.last_result();
    Require(failed_result && !failed_result->success, "failed save must be observable");

    Require(consumer.RequestSave(second_paths, 1'000'000'000), "save after capture stop must use frozen history");
    consumer.Wait();
    const auto second_result = consumer.last_result();
    Require(second_result && second_result->success, "second save must complete after prior save");

    RemoveFiles(first_paths);
    RemoveFiles(second_paths);
    return EXIT_SUCCESS;
}
