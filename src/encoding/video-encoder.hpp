#pragma once

#include "encoding/encoded-video-packet.hpp"
#include "pipeline/synchronized-frame-pipeline.hpp"

#include <optional>
#include <string>

namespace obs_sync_replay {

enum class VideoEncoderSubmitResult : uint8_t {
    Submitted,
    Capacity,
    Failed,
};

// Submit is called with OBS graphics entered. It must copy or otherwise retain
// the source before returning, but completion and packet retrieval are async.
class VideoEncoder {
  public:
    virtual ~VideoEncoder() = default;

    virtual VideoEncoderSubmitResult Prepare(const RetainedGpuFrame& frame) = 0;
    virtual VideoEncoderSubmitResult Submit(const RetainedGpuFrame& frame, int64_t encoder_pts) = 0;
    virtual void Shutdown() noexcept = 0;
    virtual const std::string& last_error() const noexcept = 0;
};

} // namespace obs_sync_replay
