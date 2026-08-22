#include "timeline/master-frame-coordinator.hpp"
#include "plugin/plugin-log.hpp"

#include <obs-module.h>

#include <utility>

namespace obs_sync_replay {

MasterFrameCoordinator::MasterFrameCoordinator(FrameSink frame_sink) : frame_sink_(std::move(frame_sink)) {}

MasterFrameCoordinator::~MasterFrameCoordinator() {
    Stop();
}

void MasterFrameCoordinator::Start() {
    if (running_) {
        OBS_SYNC_REPLAY_LOG(LOG_WARNING, "timeline", "coordinator start ignored: already running");
        return;
    }

    timeline_.Reset();
    timing_configuration_.Reset();
    last_observed_pts_ns_.reset();
    lagged_frames_ = obs_get_lagged_frames();
    invalid_timing_configuration_logged_ = false;
    running_ = true;

    RefreshTimingConfiguration();
    obs_add_tick_callback(OnObsTick, this);
    OBS_SYNC_REPLAY_LOG(LOG_INFO, "timeline", "coordinator started; PTS units=nanoseconds");
}

void MasterFrameCoordinator::Stop() {
    if (!running_) {
        return;
    }

    // libobs serializes removal with tick execution, so the coordinator is no
    // longer callable before its module-owned lifetime ends.
    obs_remove_tick_callback(OnObsTick, this);
    running_ = false;
    timeline_.Reset();
    timing_configuration_.Reset();
    last_observed_pts_ns_.reset();
    OBS_SYNC_REPLAY_LOG(LOG_INFO, "timeline", "coordinator stopped");
}

void MasterFrameCoordinator::OnObsTick(void *const parameter, float) {
    static_cast<MasterFrameCoordinator *>(parameter)->ObserveObsTick();
}

void MasterFrameCoordinator::ObserveObsTick() {
    if (!running_) {
        return;
    }

    const MasterFrameTimingConfigurationResult timing_result = RefreshTimingConfiguration();

    const MasterFramePts pts_ns = obs_get_video_frame_time();
    std::optional<MasterFrame> observed_frame;
    const MasterFrameObservationResult result = timeline_.Observe(pts_ns, observed_frame);
    if (result != MasterFrameObservationResult::Accepted) {
        const char *const reason = result == MasterFrameObservationResult::NonMonotonicPts
                                       ? "non-monotonic PTS"
                                       : "master frame ID exhausted";
        OBS_SYNC_REPLAY_LOG(LOG_ERROR, "timeline", "invariant=3 rejected master PTS=%llu reason=%s",
             static_cast<unsigned long long>(pts_ns), reason);
        return;
    }

    const MasterFrame &frame = *observed_frame;

    const std::optional<uint64_t> frame_interval_ns = timing_configuration_.frame_interval_ns();
    if (timing_result == MasterFrameTimingConfigurationResult::Unchanged &&
        last_observed_pts_ns_.has_value() && frame_interval_ns.has_value()) {
        const uint64_t delta_ns = pts_ns - *last_observed_pts_ns_;
        if (delta_ns != *frame_interval_ns) {
            OBS_SYNC_REPLAY_LOG(LOG_WARNING, "timeline",
                 "observed OBS cadence discontinuity previous_pts=%llu master_pts=%llu "
                 "delta_ns=%llu configured_interval_ns=%llu lagged_frames=%u; no frames were fabricated",
                 static_cast<unsigned long long>(*last_observed_pts_ns_), static_cast<unsigned long long>(pts_ns),
                 static_cast<unsigned long long>(delta_ns), static_cast<unsigned long long>(*frame_interval_ns),
                 obs_get_lagged_frames());
        }
    }
    last_observed_pts_ns_ = pts_ns;

    const uint32_t current_lagged_frames = obs_get_lagged_frames();
    if (current_lagged_frames != lagged_frames_) {
        OBS_SYNC_REPLAY_LOG(LOG_WARNING, "timeline",
             "OBS graphics lag changed previous_lagged_frames=%u lagged_frames=%u "
             "master_frame_id=%llu master_pts=%llu",
             lagged_frames_, current_lagged_frames, static_cast<unsigned long long>(frame.frame_id()),
             static_cast<unsigned long long>(frame.pts_ns()));
        lagged_frames_ = current_lagged_frames;
    }

    if (frame.frame_id() < 3 || frame.frame_id() % 300 == 0) {
        OBS_SYNC_REPLAY_LOG(LOG_INFO, "timeline", "master_frame_id=%llu master_pts=%llu",
             static_cast<unsigned long long>(frame.frame_id()), static_cast<unsigned long long>(frame.pts_ns()));
    } else {
        OBS_SYNC_REPLAY_LOG(LOG_DEBUG, "timeline", "master_frame_id=%llu master_pts=%llu",
             static_cast<unsigned long long>(frame.frame_id()), static_cast<unsigned long long>(frame.pts_ns()));
    }

    try {
        frame_sink_(frame);
    } catch (...) {
        OBS_SYNC_REPLAY_LOG(LOG_ERROR, "timeline",
             "frame sink failed master_frame_id=%llu master_pts=%llu; "
             "the coordinator retained its canonical timeline",
             static_cast<unsigned long long>(frame.frame_id()), static_cast<unsigned long long>(frame.pts_ns()));
    }
}

MasterFrameTimingConfigurationResult MasterFrameCoordinator::RefreshTimingConfiguration() {
    const std::optional<uint64_t> previous_interval_ns = timing_configuration_.frame_interval_ns();
    const MasterFrameTimingConfigurationResult result =
        timing_configuration_.ObserveFrameInterval(obs_get_frame_interval_ns());
    if (result == MasterFrameTimingConfigurationResult::InvalidInterval) {
        if (!invalid_timing_configuration_logged_) {
            OBS_SYNC_REPLAY_LOG(LOG_ERROR, "timeline",
                 "invalid OBS timing configuration frame_interval_ns=0; "
                 "cadence validation paused without changing the master timeline");
            invalid_timing_configuration_logged_ = true;
        }
        return result;
    }

    invalid_timing_configuration_logged_ = false;
    if (result == MasterFrameTimingConfigurationResult::Unchanged) {
        return result;
    }

    obs_video_info video_info{};
    const bool has_video_info = obs_get_video_info(&video_info);
    const uint64_t frame_interval_ns = *timing_configuration_.frame_interval_ns();
    if (result == MasterFrameTimingConfigurationResult::Changed) {
        OBS_SYNC_REPLAY_LOG(LOG_INFO, "timeline",
             "OBS timing configuration changed previous_interval_ns=%llu "
             "frame_interval_ns=%llu fps_num=%u fps_den=%u; master timeline remains continuous",
             static_cast<unsigned long long>(*previous_interval_ns), static_cast<unsigned long long>(frame_interval_ns),
             has_video_info ? video_info.fps_num : 0, has_video_info ? video_info.fps_den : 0);
    } else {
        OBS_SYNC_REPLAY_LOG(LOG_INFO, "timeline",
             "OBS timing fps_num=%u fps_den=%u frame_interval_ns=%llu "
             "PTS units=nanoseconds source=obs_get_video_frame_time",
             has_video_info ? video_info.fps_num : 0, has_video_info ? video_info.fps_den : 0,
             static_cast<unsigned long long>(frame_interval_ns));
    }

    return result;
}

} // namespace obs_sync_replay
