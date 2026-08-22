#include "replay/synchronized-replay-consumer.hpp"

extern "C" {
#include <libavutil/mathematics.h>
}

#include "sync/common-packet-range.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace obs_sync_replay {

namespace {

uint64_t WallClockNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::vector<EncodedPacket> MuxPackets(const std::vector<OwnedCapturedEncodedPacket>& owned,
                                      const std::vector<std::vector<OwnedCapturedEncodedPacket>>& audio_owned,
                                      const ReplayFrameRange range) {
    std::vector<EncodedPacket> packets;
    size_t audio_count = 0;
    for (const auto& track : audio_owned) {
        audio_count += track.size();
    }
    packets.reserve(owned.size() + audio_count);
    for (const OwnedCapturedEncodedPacket& packet : owned) {
        if (packet) {
            packets.push_back(packet->packet);
        }
    }
    std::vector<int64_t> audio_dts_origins(audio_owned.size(), 0);
    std::vector<bool> audio_origin_set(audio_owned.size(), false);
    for (size_t track = 0; track < audio_owned.size(); ++track) {
        for (const auto& packet : audio_owned[track]) {
            if (packet) {
                packets.push_back(packet->packet);
                if (!audio_origin_set[track]) {
                    audio_dts_origins[track] = packet->packet.dts;
                    audio_origin_set[track] = true;
                }
            }
        }
    }

    const AVRational source_timebase{1, 1'000'000'000};
    for (EncodedPacket& packet : packets) {
        if (packet.timebase_num <= 0 || packet.timebase_den <= 0) {
            continue;
        }
        const AVRational packet_timebase{packet.timebase_num, packet.timebase_den};
        packet.pts = av_rescale_q(static_cast<int64_t>(packet.source_cts - range.start_cts), source_timebase,
                                  packet_timebase);
        if (packet.kind == EncodedPacketKind::Audio && packet.audio_track_index < audio_dts_origins.size() &&
            audio_origin_set[packet.audio_track_index]) {
            packet.dts -= audio_dts_origins[packet.audio_track_index];
        } else if (packet.kind == EncodedPacketKind::Video && !owned.empty() && owned.front()) {
            packet.dts -= owned.front()->packet.dts;
        }
    }
    return SortForDecodeOrder(std::move(packets));
}

} // namespace

SynchronizedReplayConsumer::SynchronizedReplayConsumer(SynchronizedCaptureSession& capture,
                                                       const uint32_t test_save_delay_ms)
    : capture_(capture), test_save_delay_ms_(test_save_delay_ms) {}

SynchronizedReplayConsumer::~SynchronizedReplayConsumer() {
    Wait();
}

bool SynchronizedReplayConsumer::RequestSave(std::vector<std::filesystem::path> paths, const uint64_t duration_ns,
                                              std::vector<CaptureStreamId> stream_ids) {
    const auto reject = [this](std::string reason) {
        const std::lock_guard<std::mutex> lock(mutex_);
        last_request_error_ = std::move(reason);
        return false;
    };
    if (paths.empty()) {
        return reject("invalid-paths");
    }
    const ReplaySnapshotAttempt attempt = stream_ids.empty()
                                              ? capture_.SnapshotCommonRangeDetailed(duration_ns)
                                              : capture_.SnapshotCommonRangeDetailed(stream_ids, duration_ns);
    if (!attempt.snapshot) {
        return reject(attempt.reason.empty() ? "snapshot-rejected" : attempt.reason);
    }
    if (attempt.snapshot->packets.size() != paths.size()) {
        return reject("snapshot-stream-count-mismatch");
    }

    std::thread previous;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (active_) {
            last_request_error_ = "save-already-active";
            return false;
        }
        if (worker_.joinable()) {
            previous = std::move(worker_);
        }
        active_ = true;
        last_request_error_.clear();
        last_result_.reset();
        worker_ = std::thread([this, snapshot = std::move(*attempt.snapshot), paths = std::move(paths)]() mutable {
            if (test_save_delay_ms_ > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(test_save_delay_ms_));
            }
            ReplaySaveResult result = WriteSnapshot(std::move(snapshot), std::move(paths));
            const std::lock_guard<std::mutex> lock(mutex_);
            last_result_ = std::move(result);
            active_ = false;
        });
    }
    if (previous.joinable()) {
        previous.join();
    }
    return true;
}

void SynchronizedReplayConsumer::Wait() noexcept {
    std::thread worker;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        worker = std::move(worker_);
    }
    if (worker.joinable()) {
        worker.join();
    }
}

bool SynchronizedReplayConsumer::active() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

std::optional<ReplaySaveResult> SynchronizedReplayConsumer::last_result() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return last_result_;
}

std::string SynchronizedReplayConsumer::last_request_error() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return last_request_error_;
}

ReplaySaveResult SynchronizedReplayConsumer::WriteSnapshot(ReplaySnapshot snapshot,
                                                           std::vector<std::filesystem::path> paths) {
    ReplaySaveResult result;
    result.range = snapshot.range;
    result.paths = std::move(paths);
    result.streams.resize(snapshot.packets.size());
    const uint64_t start_ns = WallClockNs();
    if (snapshot.packets.size() != snapshot.stream_configs.size() || snapshot.packets.size() != result.paths.size()) {
        result.error = "snapshot-stream-count-mismatch";
        return result;
    }
    for (const auto& track : snapshot.audio_packets) {
        for (const auto& packet : track) {
            if (packet) {
                result.snapshot_payload_bytes += packet->packet.payload.size();
            }
        }
    }

    for (size_t index = 0; index < snapshot.packets.size(); ++index) {
        for (const auto& packet : snapshot.packets[index]) {
            if (packet) {
                result.snapshot_payload_bytes += packet->packet.payload.size();
            }
        }
        snapshot.stream_configs[index].audio_streams = snapshot.audio_streams;
        MkvPacketWriter writer;
        if (!writer.Open(result.paths[index].string(), snapshot.stream_configs[index])) {
            result.error = "stream-open-failed:" + writer.error();
            return result;
        }
        const std::vector<EncodedPacket> packets =
            MuxPackets(snapshot.packets[index], snapshot.audio_packets, snapshot.range);
        if (packets.empty()) {
            writer.Abort();
            result.error = "empty-snapshot-stream";
            return result;
        }
        for (const EncodedPacket& packet : packets) {
            if (!writer.Write(packet)) {
                result.error = "stream-write-failed:" + writer.error();
                writer.Abort();
                return result;
            }
        }
        result.streams[index] = writer.Finalize();
        if (!result.streams[index].success || result.streams[index].first_source_cts != snapshot.range.start_cts ||
            result.streams[index].last_source_cts != snapshot.range.end_cts) {
            result.error = "stream-range-mismatch:" + result.streams[index].error;
            return result;
        }
    }
    result.wall_time_ms = (WallClockNs() - start_ns) / 1'000'000;
    result.success = true;
    return result;
}

} // namespace obs_sync_replay
