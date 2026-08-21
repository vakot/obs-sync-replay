#pragma once

#include "capture/synchronized-capture-session.hpp"
#include "muxing/mkv-packet-writer.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace obs_sync_replay {

struct ReplaySaveResult final {
    bool success = false;
    ReplayFrameRange range;
    std::vector<std::filesystem::path> paths;
    std::vector<MkvWriteResult> streams;
    uint64_t snapshot_payload_bytes = 0;
    uint64_t wall_time_ms = 0;
    std::string error;
};

class SynchronizedReplayConsumer final {
  public:
    explicit SynchronizedReplayConsumer(SynchronizedCaptureSession& capture, uint32_t test_save_delay_ms = 0);
    ~SynchronizedReplayConsumer();

    SynchronizedReplayConsumer(const SynchronizedReplayConsumer&) = delete;
    SynchronizedReplayConsumer& operator=(const SynchronizedReplayConsumer&) = delete;

    bool RequestSave(std::vector<std::filesystem::path> paths, uint64_t duration_ns);
    void Wait() noexcept;
    bool active() const noexcept;
    std::optional<ReplaySaveResult> last_result() const;

  private:
    static ReplaySaveResult WriteSnapshot(ReplaySnapshot snapshot, std::vector<std::filesystem::path> paths);

    SynchronizedCaptureSession& capture_;
    const uint32_t test_save_delay_ms_;
    mutable std::mutex mutex_;
    std::thread worker_;
    bool active_ = false;
    std::optional<ReplaySaveResult> last_result_;
};

} // namespace obs_sync_replay
