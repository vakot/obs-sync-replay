#include "rendering/scene-render-pair-tracker.hpp"

namespace obs_sync_replay {

bool SceneRenderPairTracker::Begin(const MasterFrame &master_frame) noexcept {
    if (active_frame_.has_value()) {
        return false;
    }

    active_frame_ = master_frame;
    output_a_recorded_ = false;
    output_b_recorded_ = false;
    return true;
}

bool SceneRenderPairTracker::Record(const SceneRenderResult &result) noexcept {
    if (!active_frame_.has_value() || result.master_frame.frame_id() != active_frame_->frame_id() ||
        result.master_frame.pts_ns() != active_frame_->pts_ns()) {
        return false;
    }

    bool &recorded = result.output == OutputSlot::A ? output_a_recorded_ : output_b_recorded_;
    if (recorded) {
        return false;
    }

    recorded = true;
    return true;
}

bool SceneRenderPairTracker::IsComplete() const noexcept {
    return active_frame_.has_value() && output_a_recorded_ && output_b_recorded_;
}

void SceneRenderPairTracker::Reset() noexcept {
    active_frame_.reset();
    output_a_recorded_ = false;
    output_b_recorded_ = false;
}

} // namespace obs_sync_replay
