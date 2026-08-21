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
                                      const ReplayFrameRange range) {
    std::vector<EncodedPacket> packets;
    packets.reserve(owned.size());
    for (const OwnedCapturedEncodedPacket& packet : owned) {
        if (packet) {
            packets.push_back(packet->packet);
        }
    }
    packets = SortForDecodeOrder(std::move(packets));
    if (packets.empty() || packets.front().timebase_num <= 0 || packets.front().timebase_den <= 0) {
        return packets;
    }

    const AVRational source_timebase{1, 1'000'000'000};
    const AVRational packet_timebase{packets.front().timebase_num, packets.front().timebase_den};
    const int64_t dts_origin = packets.front().dts;
    for (EncodedPacket& packet : packets) {
        packet.pts = av_rescale_q(static_cast<int64_t>(packet.source_cts - range.start_cts), source_timebase,
                                  packet_timebase);
        packet.dts -= dts_origin;
    }
    return packets;
}

} // namespace

SynchronizedReplayConsumer::SynchronizedReplayConsumer(SynchronizedCaptureSession& capture,
                                                       const uint32_t test_save_delay_ms)
    : capture_(capture), test_save_delay_ms_(test_save_delay_ms) {}

SynchronizedReplayConsumer::~SynchronizedReplayConsumer() {
    Wait();
}

bool SynchronizedReplayConsumer::RequestSave(std::vector<std::filesystem::path> paths, const uint64_t duration_ns) {
    if (paths.empty()) {
        return false;
    }
    std::optional<ReplaySnapshot> snapshot = capture_.SnapshotCommonRange(duration_ns);
    if (!snapshot || snapshot->packets.size() != paths.size()) {
        return false;
    }

    std::thread previous;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (active_) {
            return false;
        }
        if (worker_.joinable()) {
            previous = std::move(worker_);
        }
        active_ = true;
        last_result_.reset();
        worker_ = std::thread([this, snapshot = std::move(*snapshot), paths = std::move(paths)]() mutable {
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

    for (size_t index = 0; index < snapshot.packets.size(); ++index) {
        for (const auto& packet : snapshot.packets[index]) {
            if (packet) {
                result.snapshot_payload_bytes += packet->packet.payload.size();
            }
        }
        MkvPacketWriter writer;
        if (!writer.Open(result.paths[index].string(), snapshot.stream_configs[index])) {
            result.error = "stream-open-failed:" + writer.error();
            return result;
        }
        const std::vector<EncodedPacket> packets = MuxPackets(snapshot.packets[index], snapshot.range);
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
