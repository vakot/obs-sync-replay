#include "pipeline/synchronized-frame-pipeline.hpp"

#include <graphics/graphics.h>

#include <cassert>
#include <utility>

namespace obs_sync_replay {

namespace {

SynchronizedFramePairIdentity PairIdentity(const SceneRenderResult& output_a,
                                           const SceneRenderResult& output_b) {
    return {output_a.master_frame, output_b.master_frame,
            output_a.status == SceneRenderStatus::Rendered &&
                output_b.status == SceneRenderStatus::Rendered &&
                output_a.output == OutputSlot::A && output_b.output == OutputSlot::B &&
                output_a.texture && output_b.texture && output_a.width != 0 &&
                output_a.height != 0 && output_b.width != 0 && output_b.height != 0};
}

} // namespace

RetainedGpuFrame::RetainedGpuFrame(const SceneRenderResult& result,
                                   gs_texture_t* const texture) noexcept
    : master_frame_(result.master_frame), output_(result.output), width_(result.width),
      height_(result.height), color_space_(result.color_space), color_format_(result.color_format),
      texture_(texture) {}

RetainedGpuFrame::RetainedGpuFrame(RetainedGpuFrame&& other) noexcept
    : master_frame_(other.master_frame_), output_(other.output_), width_(other.width_),
      height_(other.height_), color_space_(other.color_space_), color_format_(other.color_format_),
      texture_(other.texture_) {
    other.texture_ = nullptr;
}

RetainedGpuFrame::~RetainedGpuFrame() {
    if (texture_) {
        // The pipeline confines every owning destruction path to obs_enter_graphics.
        assert(gs_get_context());
        gs_texture_destroy(texture_);
    }
}

const MasterFrame& RetainedGpuFrame::master_frame() const noexcept {
    return master_frame_;
}

OutputSlot RetainedGpuFrame::output() const noexcept {
    return output_;
}

uint32_t RetainedGpuFrame::width() const noexcept {
    return width_;
}

uint32_t RetainedGpuFrame::height() const noexcept {
    return height_;
}

uint32_t RetainedGpuFrame::color_space() const noexcept {
    return color_space_;
}

uint32_t RetainedGpuFrame::color_format() const noexcept {
    return color_format_;
}

gs_texture_t* RetainedGpuFrame::texture() const noexcept {
    return texture_;
}

std::unique_ptr<RetainedGpuFrame> RetainedGpuFrame::CopyFrom(const SceneRenderResult& result) {
    const gs_color_format color_format = static_cast<gs_color_format>(result.color_format);
    gs_texture_t* const texture =
        gs_texture_create(result.width, result.height, color_format, 1, nullptr, 0);
    if (!texture) {
        return nullptr;
    }

    // The copy is submitted before the source texrender target can be reset on
    // the next master tick; GPU command ordering preserves this retained content.
    gs_copy_texture(texture, result.texture);
    try {
        return std::unique_ptr<RetainedGpuFrame>(new RetainedGpuFrame(result, texture));
    } catch (...) {
        gs_texture_destroy(texture);
        throw;
    }
}

SynchronizedFramePair::SynchronizedFramePair(std::unique_ptr<RetainedGpuFrame> output_a,
                                             std::unique_ptr<RetainedGpuFrame> output_b)
    : master_frame_(output_a->master_frame()), output_a_(std::move(output_a)),
      output_b_(std::move(output_b)) {
    assert(output_a_->output() == OutputSlot::A && output_b_->output() == OutputSlot::B);
    assert(output_a_->master_frame().frame_id() == output_b_->master_frame().frame_id());
    assert(output_a_->master_frame().pts_ns() == output_b_->master_frame().pts_ns());
}

const MasterFrame& SynchronizedFramePair::master_frame() const noexcept {
    return master_frame_;
}

const RetainedGpuFrame& SynchronizedFramePair::output_a() const noexcept {
    return *output_a_;
}

const RetainedGpuFrame& SynchronizedFramePair::output_b() const noexcept {
    return *output_b_;
}

SynchronizedFramePipeline::SynchronizedFramePipeline(const size_t capacity) : queue_(capacity) {
    retained_pairs_.reserve(capacity);
}

SynchronizedFramePipeline::~SynchronizedFramePipeline() {
    assert(retained_pairs_.empty());
}

SynchronizedFramePipelineResult
SynchronizedFramePipeline::Capture(const SceneRenderResult& output_a,
                                   const SceneRenderResult& output_b) {
    const SynchronizedFramePairIdentity identity = PairIdentity(output_a, output_b);
    const SynchronizedFrameQueueResult acceptance = queue_.CanRetain(identity);
    if (acceptance == SynchronizedFrameQueueResult::InvalidPair) {
        return SynchronizedFramePipelineResult::InvalidPair;
    }
    if (acceptance == SynchronizedFrameQueueResult::Capacity) {
        return SynchronizedFramePipelineResult::Capacity;
    }

    std::unique_ptr<RetainedGpuFrame> retained_a = RetainedGpuFrame::CopyFrom(output_a);
    std::unique_ptr<RetainedGpuFrame> retained_b = RetainedGpuFrame::CopyFrom(output_b);
    if (!retained_a || !retained_b) {
        return SynchronizedFramePipelineResult::TextureCreationFailed;
    }

    std::unique_ptr<SynchronizedFramePair> pair =
        std::make_unique<SynchronizedFramePair>(std::move(retained_a), std::move(retained_b));
    if (queue_.TryRetain(identity) != SynchronizedFrameQueueResult::Retained) {
        return SynchronizedFramePipelineResult::InvalidPair;
    }
    retained_pairs_.push_back(std::move(pair));
    return SynchronizedFramePipelineResult::Retained;
}

std::unique_ptr<SynchronizedFramePair> SynchronizedFramePipeline::TakeNext() {
    if (retained_pairs_.empty()) {
        return nullptr;
    }

    std::unique_ptr<SynchronizedFramePair> pair = std::move(retained_pairs_.front());
    retained_pairs_.erase(retained_pairs_.begin());
    const std::optional<MasterFrame> queued_frame = queue_.TakeNext();
    assert(queued_frame.has_value());
    assert(queued_frame->frame_id() == pair->master_frame().frame_id());
    assert(queued_frame->pts_ns() == pair->master_frame().pts_ns());
    return pair;
}

void SynchronizedFramePipeline::Reset() noexcept {
    retained_pairs_.clear();
    queue_.Reset();
}

size_t SynchronizedFramePipeline::size() const noexcept {
    return queue_.size();
}

size_t SynchronizedFramePipeline::capacity() const noexcept {
    return queue_.capacity();
}

const char*
SynchronizedFramePipelineResultName(const SynchronizedFramePipelineResult result) noexcept {
    switch (result) {
    case SynchronizedFramePipelineResult::Retained:
        return "retained";
    case SynchronizedFramePipelineResult::InvalidPair:
        return "invalid-pair";
    case SynchronizedFramePipelineResult::Capacity:
        return "capacity";
    case SynchronizedFramePipelineResult::TextureCreationFailed:
        return "texture-creation-failed";
    }

    return "unknown";
}

} // namespace obs_sync_replay
