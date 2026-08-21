#pragma once

#include "capture/synchronized-capture-session.hpp"
#include "muxing/mkv-packet-writer.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace obs_sync_replay {

struct SynchronizedRecordingConsumerResult final {
    bool success = false;
    ReplayFrameRange range;
    std::vector<std::filesystem::path> paths;
    std::vector<MkvWriteResult> streams;
    uint64_t packet_count = 0;
    uint64_t wall_time_ms = 0;
    std::string error;
};

// A non-blocking fan-out consumer for the live Recording path. It has one
// packet queue and one packet-only MKV writer per registered stream; it never
// creates or owns an encoder.
class SynchronizedRecordingConsumer final : public CapturePacketConsumer {
  public:
    SynchronizedRecordingConsumer(std::vector<PacketStreamConfig> stream_configs,
                                  std::vector<std::filesystem::path> paths,
                                  std::vector<CaptureStreamId> stream_ids = {});
    ~SynchronizedRecordingConsumer();

    SynchronizedRecordingConsumer(const SynchronizedRecordingConsumer&) = delete;
    SynchronizedRecordingConsumer& operator=(const SynchronizedRecordingConsumer&) = delete;

    bool Start();
    void OnPacket(OwnedCapturedEncodedPacket packet) override;
    void Stop() noexcept;
    std::optional<SynchronizedRecordingConsumerResult> result() const;

  private:
    void Run();
    bool EstablishCommonStart();
    bool FlushCommonPrefix();
    void Fail(std::string error);
    void Finalize();

    std::vector<PacketStreamConfig> stream_configs_;
    std::vector<std::filesystem::path> paths_;
    std::vector<CaptureStreamId> stream_ids_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<OwnedCapturedEncodedPacket> queue_;
    std::vector<std::map<uint64_t, OwnedCapturedEncodedPacket>> pending_;
    std::vector<std::unique_ptr<MkvPacketWriter>> writers_;
    std::vector<int64_t> dts_origins_;
    std::thread worker_;
    bool running_ = false;
    bool stop_requested_ = false;
    bool failed_ = false;
    bool started_ = false;
    static constexpr size_t kMaxQueuePackets = 8192;
    ReplayFrameRange range_;
    uint64_t packet_count_ = 0;
    uint64_t start_wall_ns_ = 0;
    std::optional<SynchronizedRecordingConsumerResult> result_;

    std::optional<size_t> LocalStreamIndex(CaptureStreamId stream_id) const noexcept;
};

} // namespace obs_sync_replay
