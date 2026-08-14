#include "rendering/synchronized-scene-renderer.hpp"

#include <obs-module.h>

#include <utility>

namespace obs_sync_replay {

namespace {

const char* PipelineFailureReason(const SceneRenderResult& output_a,
                                  const SceneRenderResult& output_b,
                                  const SynchronizedFramePipelineResult result) noexcept {
    if (result == SynchronizedFramePipelineResult::Capacity) {
        return "capacity";
    }
    if (result == SynchronizedFramePipelineResult::TextureCreationFailed) {
        return "texture-creation";
    }
    if (result != SynchronizedFramePipelineResult::InvalidPair) {
        return "none";
    }
    if (output_a.master_frame.frame_id() != output_b.master_frame.frame_id() ||
        output_a.master_frame.pts_ns() != output_b.master_frame.pts_ns()) {
        return "master-identity-mismatch";
    }
    return output_a.status != SceneRenderStatus::Rendered ||
                   output_b.status != SceneRenderStatus::Rendered
               ? "render-incomplete"
               : "render-resource-invalid";
}

} // namespace

SynchronizedSceneRenderer::SynchronizedSceneRenderer(std::string scene_a_name,
                                                     std::string scene_b_name)
    : scene_a_renderer_(OutputSlot::A, std::move(scene_a_name)),
      scene_b_renderer_(OutputSlot::B, std::move(scene_b_name)) {}

SynchronizedSceneRenderer::~SynchronizedSceneRenderer() {
    Stop();
}

void SynchronizedSceneRenderer::Render(const MasterFrame& master_frame) {
    if (stopped_) {
        blog(LOG_ERROR,
             "[sync-render] invariant=2 rejected render after stop master_frame_id=%llu "
             "master_pts=%llu",
             static_cast<unsigned long long>(master_frame.frame_id()),
             static_cast<unsigned long long>(master_frame.pts_ns()));
        return;
    }

    if (!pair_tracker_.Begin(master_frame)) {
        blog(LOG_ERROR,
             "[sync-render] invariant=1 duplicate or reentrant dispatch master_frame_id=%llu "
             "master_pts=%llu",
             static_cast<unsigned long long>(master_frame.frame_id()),
             static_cast<unsigned long long>(master_frame.pts_ns()));
        return;
    }

    // The tick callback has no active context. Enter once and render both
    // outputs before leaving, so neither output establishes a separate clock.
    obs_enter_graphics();
    const SceneRenderResult output_a = scene_a_renderer_.Render(master_frame);
    const SceneRenderResult output_b = scene_b_renderer_.Render(master_frame);
    RecordAndLog(output_a);
    RecordAndLog(output_b);
    CaptureAndLog(output_a, output_b);
    // TakeNext transfers the complete pair only while graphics is entered.
    // NVENC's blocking bitstream lock finishes texture consumption before the
    // moved pair is destroyed at the end of this call.
    video_encoder_.Consume(pipeline_);
    obs_leave_graphics();

    if (!pair_tracker_.IsComplete()) {
        blog(LOG_ERROR,
             "[sync-render] invariant=4 incomplete render pair master_frame_id=%llu "
             "master_pts=%llu; "
             "later frames will not fill this slot",
             static_cast<unsigned long long>(master_frame.frame_id()),
             static_cast<unsigned long long>(master_frame.pts_ns()));
    }
    pair_tracker_.Reset();
}

void SynchronizedSceneRenderer::Stop() {
    if (stopped_) {
        return;
    }

    stopped_ = true;
    pair_tracker_.Reset();
    obs_enter_graphics();
    video_encoder_.Stop();
    pipeline_.Reset();
    scene_a_renderer_.DestroyRenderTarget();
    scene_b_renderer_.DestroyRenderTarget();
    obs_leave_graphics();
}

void SynchronizedSceneRenderer::CaptureAndLog(const SceneRenderResult& output_a,
                                              const SceneRenderResult& output_b) {
    const SynchronizedFramePipelineResult result = pipeline_.Capture(output_a, output_b);
    const MasterFrame& master_frame = output_a.master_frame;
    const bool sampled = master_frame.frame_id() < 3 || master_frame.frame_id() % 300 == 0;
    const bool status_changed =
        !last_reported_pipeline_result_.has_value() || *last_reported_pipeline_result_ != result;
    last_reported_pipeline_result_ = result;
    if (result != SynchronizedFramePipelineResult::Retained && !status_changed && !sampled) {
        return;
    }
    const int log_level =
        result == SynchronizedFramePipelineResult::Retained
            ? (sampled || status_changed ? LOG_INFO : LOG_DEBUG)
            : (result == SynchronizedFramePipelineResult::InvalidPair ? LOG_ERROR : LOG_WARNING);
    blog(log_level,
         "[sync-pipeline] master_frame_id=%llu master_pts=%llu status=%s reason=%s queue_size=%zu "
         "queue_capacity=%zu",
         static_cast<unsigned long long>(master_frame.frame_id()),
         static_cast<unsigned long long>(master_frame.pts_ns()),
         SynchronizedFramePipelineResultName(result),
         PipelineFailureReason(output_a, output_b, result), pipeline_.size(), pipeline_.capacity());
}

void SynchronizedSceneRenderer::RecordAndLog(const SceneRenderResult& result) {
    if (!pair_tracker_.Record(result)) {
        blog(LOG_ERROR,
             "[sync-render] invariant=4 rejected duplicate or stale result master_frame_id=%llu "
             "master_pts=%llu output=%s",
             static_cast<unsigned long long>(result.master_frame.frame_id()),
             static_cast<unsigned long long>(result.master_frame.pts_ns()),
             OutputSlotName(result.output));
        return;
    }

    const size_t output_index = result.output == OutputSlot::A ? 0 : 1;
    const bool status_changed = !last_reported_status_[output_index].has_value() ||
                                *last_reported_status_[output_index] != result.status;
    last_reported_status_[output_index] = result.status;
    const bool sampled =
        result.master_frame.frame_id() < 3 || result.master_frame.frame_id() % 300 == 0;
    if (result.status != SceneRenderStatus::Rendered && !status_changed && !sampled) {
        return;
    }

    const int log_level = result.status == SceneRenderStatus::Rendered
                              ? (sampled ? LOG_INFO : LOG_DEBUG)
                              : LOG_WARNING;
    blog(log_level,
         "[sync-render] invariant=4 master_frame_id=%llu master_pts=%llu output=%s scene=%s "
         "width=%u height=%u "
         "color_space=%u color_format=%u status=%s",
         static_cast<unsigned long long>(result.master_frame.frame_id()),
         static_cast<unsigned long long>(result.master_frame.pts_ns()),
         OutputSlotName(result.output), result.scene_name.c_str(), result.width, result.height,
         result.color_space, result.color_format, SceneRenderStatusName(result.status));
}

} // namespace obs_sync_replay
