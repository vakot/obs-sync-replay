#pragma once

#include <atomic>
#include <cstdint>

struct video_output;
using video_t = video_output;

namespace obs_sync_replay {

void RunSynchronizedRecording(const char* encoder_id, video_t* video_a, video_t* video_b,
                              uint32_t duration_seconds, uint32_t warmup_milliseconds,
                              const std::atomic<bool>* shutdown_requested = nullptr);

} // namespace obs_sync_replay
