#pragma once

#include "pipeline/synchronized-frame-queue.hpp"
#include "rendering/scene-renderer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace obs_sync_replay {

// Move-only ownership of one GPU texture copied from a SceneRenderer target.
// The destructor runs gs_texture_destroy, so the owner must be destroyed only
// while the OBS graphics context is entered.
class RetainedGpuFrame final {
  public:
    RetainedGpuFrame(const RetainedGpuFrame&) = delete;
    RetainedGpuFrame& operator=(const RetainedGpuFrame&) = delete;
    RetainedGpuFrame(RetainedGpuFrame&& other) noexcept;
    RetainedGpuFrame& operator=(RetainedGpuFrame&&) = delete;
    ~RetainedGpuFrame();

    const MasterFrame& master_frame() const noexcept;
    OutputSlot output() const noexcept;
    uint32_t width() const noexcept;
    uint32_t height() const noexcept;
    uint32_t color_space() const noexcept;
    uint32_t color_format() const noexcept;
    gs_texture_t* texture() const noexcept;

    static std::unique_ptr<RetainedGpuFrame> CopyFrom(const SceneRenderResult& result);

  private:
    RetainedGpuFrame(const SceneRenderResult& result, gs_texture_t* texture) noexcept;

    MasterFrame master_frame_;
    OutputSlot output_;
    uint32_t width_;
    uint32_t height_;
    uint32_t color_space_;
    uint32_t color_format_;
    gs_texture_t* texture_;
};

// Owns one completed A/B GPU frame pair. It is move-only so a retained texture
// cannot be copied into an unrelated owner.
class SynchronizedFramePair final {
  public:
    SynchronizedFramePair(std::unique_ptr<RetainedGpuFrame> output_a,
                          std::unique_ptr<RetainedGpuFrame> output_b);

    SynchronizedFramePair(const SynchronizedFramePair&) = delete;
    SynchronizedFramePair& operator=(const SynchronizedFramePair&) = delete;
    SynchronizedFramePair(SynchronizedFramePair&&) noexcept = default;
    SynchronizedFramePair& operator=(SynchronizedFramePair&&) = delete;

    const MasterFrame& master_frame() const noexcept;
    const RetainedGpuFrame& output_a() const noexcept;
    const RetainedGpuFrame& output_b() const noexcept;

  private:
    MasterFrame master_frame_;
    std::unique_ptr<RetainedGpuFrame> output_a_;
    std::unique_ptr<RetainedGpuFrame> output_b_;
};

enum class SynchronizedFramePipelineResult : uint8_t {
    Retained,
    InvalidPair,
    Capacity,
    TextureCreationFailed,
};

// Graphics-context-confined owner of the bounded retained frame-pair stream.
// Capture and Reset must be called with OBS graphics entered. Future encoders
// consume these resources rather than SceneRenderer-owned texrender textures.
class SynchronizedFramePipeline final {
  public:
    explicit SynchronizedFramePipeline(size_t capacity);
    ~SynchronizedFramePipeline();

    SynchronizedFramePipeline(const SynchronizedFramePipeline&) = delete;
    SynchronizedFramePipeline& operator=(const SynchronizedFramePipeline&) = delete;

    SynchronizedFramePipelineResult Capture(const SceneRenderResult& output_a,
                                            const SceneRenderResult& output_b);
    // Transfers the oldest complete pair to a future consumer. Both taking and
    // final destruction must occur while the OBS graphics context is entered.
    std::unique_ptr<SynchronizedFramePair> TakeNext();
    void Reset() noexcept;

    size_t size() const noexcept;
    size_t capacity() const noexcept;

  private:
    SynchronizedFrameQueue queue_;
    std::vector<std::unique_ptr<SynchronizedFramePair>> retained_pairs_;
};

const char* SynchronizedFramePipelineResultName(SynchronizedFramePipelineResult result) noexcept;

} // namespace obs_sync_replay
