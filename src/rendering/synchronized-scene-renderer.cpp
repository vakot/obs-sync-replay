#include "rendering/synchronized-scene-renderer.hpp"

#include <obs-module.h>

#include <utility>

namespace obs_sync_replay {

SynchronizedSceneRenderer::SynchronizedSceneRenderer(std::string scene_a_name, std::string scene_b_name)
    : scene_a_renderer_(OutputSlot::A, std::move(scene_a_name)), scene_b_renderer_(OutputSlot::B, std::move(scene_b_name)) {}

SynchronizedSceneRenderer::~SynchronizedSceneRenderer() {
    Stop();
}

void SynchronizedSceneRenderer::Render(const MasterFrame &master_frame) {
    if (stopped_) {
        blog(LOG_ERROR,
             "[sync-render] invariant=2 rejected render after stop master_frame_id=%llu master_pts=%llu",
             static_cast<unsigned long long>(master_frame.frame_id()),
             static_cast<unsigned long long>(master_frame.pts_ns()));
        return;
    }

    if (!pair_tracker_.Begin(master_frame)) {
        blog(LOG_ERROR,
             "[sync-render] invariant=1 duplicate or reentrant dispatch master_frame_id=%llu master_pts=%llu",
             static_cast<unsigned long long>(master_frame.frame_id()),
             static_cast<unsigned long long>(master_frame.pts_ns()));
        return;
    }

    // The tick callback has no active context. Enter once and render both
    // outputs before leaving, so neither output establishes a separate clock.
    obs_enter_graphics();
    RecordAndLog(scene_a_renderer_.Render(master_frame));
    RecordAndLog(scene_b_renderer_.Render(master_frame));
    obs_leave_graphics();

    if (!pair_tracker_.IsComplete()) {
        blog(LOG_ERROR,
             "[sync-render] invariant=4 incomplete render pair master_frame_id=%llu master_pts=%llu; "
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
    scene_a_renderer_.DestroyRenderTarget();
    scene_b_renderer_.DestroyRenderTarget();
    obs_leave_graphics();
}

void SynchronizedSceneRenderer::RecordAndLog(const SceneRenderResult &result) {
    if (!pair_tracker_.Record(result)) {
        blog(LOG_ERROR,
             "[sync-render] invariant=4 rejected duplicate or stale result master_frame_id=%llu master_pts=%llu output=%s",
             static_cast<unsigned long long>(result.master_frame.frame_id()),
             static_cast<unsigned long long>(result.master_frame.pts_ns()), OutputSlotName(result.output));
        return;
    }

    const int log_level = result.status == SceneRenderStatus::Rendered
                              ? ((result.master_frame.frame_id() < 3 || result.master_frame.frame_id() % 300 == 0)
                                     ? LOG_INFO
                                     : LOG_DEBUG)
                              : LOG_WARNING;
    blog(log_level,
         "[sync-render] invariant=4 master_frame_id=%llu master_pts=%llu output=%s scene=%s width=%u height=%u "
         "status=%s",
         static_cast<unsigned long long>(result.master_frame.frame_id()),
         static_cast<unsigned long long>(result.master_frame.pts_ns()), OutputSlotName(result.output), result.scene_name.c_str(),
         result.width, result.height, SceneRenderStatusName(result.status));
}

} // namespace obs_sync_replay
