#pragma once

#include "encoding/video-encoder.hpp"

#include <memory>

namespace obs_sync_replay {

// Windows/D3D11 NVENC implementation. It deliberately owns a driver session
// rather than an obs_encoder_t because libobs exposes no public API for
// submitting an arbitrary gs_texture_t with caller-controlled PTS.
class NvencVideoEncoder final : public VideoEncoder {
  public:
    explicit NvencVideoEncoder(OutputSlot output);
    ~NvencVideoEncoder() override;

    VideoEncoderSubmitResult Submit(const RetainedGpuFrame& frame, int64_t encoder_pts,
                                    EncodedVideoPacket* packet) override;
    void Shutdown() noexcept override;
    const std::string& last_error() const noexcept override;

  private:
    bool EnsureInitialized(const RetainedGpuFrame& frame);
    bool Initialize(uint32_t width, uint32_t height);
    bool Encode(const RetainedGpuFrame& frame, int64_t encoder_pts, EncodedVideoPacket* packet);
    void SetError(const char* operation, int status) noexcept;

    struct State;
    std::unique_ptr<State> state_;
    OutputSlot output_;
    std::string last_error_;
};

} // namespace obs_sync_replay
