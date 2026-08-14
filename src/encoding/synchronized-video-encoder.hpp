#pragma once

#include "encoding/encoded-packet-tracker.hpp"
#include "encoding/video-encoder.hpp"
#include "pipeline/synchronized-frame-pipeline.hpp"

#include <memory>

namespace obs_sync_replay {

// Consumes the one complete-pair FIFO while OBS graphics is entered. A failed
// half-submission transitions the encoder session to Failed; it does not let a
// subsequent B frame fill the missing slot or advance either output silently.
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
    void Fail(const MasterFrame& master_frame, const char* reason, const char* detail) noexcept;

    std::unique_ptr<VideoEncoder> encoder_a_;
    std::unique_ptr<VideoEncoder> encoder_b_;
    EncodedPacketTracker packet_tracker_{4};
    bool stopped_ = false;
    bool failed_ = false;
};

} // namespace obs_sync_replay
