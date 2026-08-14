#pragma once

#include "encoding/video-encoder.hpp"

#include <functional>
#include <memory>

namespace obs_sync_replay {

// Windows/D3D11 NVENC implementation. It deliberately owns a driver session
// rather than an obs_encoder_t because libobs exposes no public API for
// submitting an arbitrary gs_texture_t with caller-controlled PTS.
class NvencVideoEncoder final : public VideoEncoder {
  public:
    using PacketCallback = std::function<void(EncodedVideoPacket&&)>;
    using FailureCallback = std::function<void(const MasterFrame&, const std::string&)>;

    NvencVideoEncoder(OutputSlot output, PacketCallback packet_callback,
                      FailureCallback failure_callback);
    ~NvencVideoEncoder() override;

    VideoEncoderSubmitResult Prepare(const RetainedGpuFrame& frame) override;
    VideoEncoderSubmitResult Submit(const RetainedGpuFrame& frame, int64_t encoder_pts) override;
    void Shutdown() noexcept override;
    const std::string& last_error() const noexcept override;

  private:
    bool EnsureInitialized(const RetainedGpuFrame& frame);
    bool Initialize(uint32_t width, uint32_t height);
    bool CreateReusableResources(uint32_t width, uint32_t height);
    bool SubmitAsync(const RetainedGpuFrame& frame, int64_t encoder_pts);
    void CompletionThread() noexcept;
    void CompleteNextSubmission() noexcept;
    void ReleaseReusableResources() noexcept;
    void NotifyFailure(const MasterFrame& master_frame, const std::string& detail) noexcept;
    void SetError(const char* operation, int status) noexcept;

    struct State;
    std::unique_ptr<State> state_;
    OutputSlot output_;
    PacketCallback packet_callback_;
    FailureCallback failure_callback_;
    std::string last_error_;
};

} // namespace obs_sync_replay
