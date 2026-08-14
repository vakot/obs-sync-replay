#pragma once

#include "encoding/encoded-video-packet.hpp"
#include "pipeline/synchronized-frame-pipeline.hpp"

#include <optional>
#include <string>

namespace obs_sync_replay {

enum class VideoEncoderSubmitResult : uint8_t {
    Encoded,
    Failed,
};

// Submit is called with OBS graphics entered. On Encoded, the implementation
// has finished reading frame.texture() and the caller may destroy the frame.
class VideoEncoder {
  public:
    virtual ~VideoEncoder() = default;

    virtual VideoEncoderSubmitResult Submit(const RetainedGpuFrame& frame, int64_t encoder_pts,
                                            EncodedVideoPacket* packet) = 0;
    virtual void Shutdown() noexcept = 0;
    virtual const std::string& last_error() const noexcept = 0;
};

} // namespace obs_sync_replay
