#pragma once

#include "encoding/encoded-packet-tracker.hpp"
#include "encoding/video-encoder.hpp"
#include "pipeline/synchronized-frame-pipeline.hpp"

#include <memory>
#include <atomic>
#include <mutex>

namespace obs_sync_replay {

// Submits complete pairs while OBS graphics is entered. NVENC completion runs
// on encoder-owned threads and records packets by submitted identity.
class SynchronizedVideoEncoder final {
  public:
    SynchronizedVideoEncoder();
    ~SynchronizedVideoEncoder();

    SynchronizedVideoEncoder(const SynchronizedVideoEncoder&) = delete;
    SynchronizedVideoEncoder& operator=(const SynchronizedVideoEncoder&) = delete;

    void Consume(SynchronizedFramePipeline& pipeline);
    void Stop() noexcept;

    bool failed() const noexcept;
    size_t pending_packet_pairs() const noexcept;

  private:
    bool SubmitPair(const SynchronizedFramePair& pair);
    bool RecordPacket(const EncodedVideoPacket& packet);
    void OnEncoderPacket(EncodedVideoPacket&& packet);
    void OnEncoderFailure(const MasterFrame& master_frame, const std::string& detail);
    void Fail(const MasterFrame& master_frame, const char* reason, const char* detail) noexcept;

    std::shared_ptr<std::recursive_mutex> operation_gate_{std::make_shared<std::recursive_mutex>()};
    std::unique_ptr<VideoEncoder> encoder_a_;
    std::unique_ptr<VideoEncoder> encoder_b_;
    EncodedPacketTracker packet_tracker_{6};
    mutable std::mutex state_mutex_;
    std::atomic<bool> stopped_{false};
    std::atomic<bool> failed_{false};
};

} // namespace obs_sync_replay
