#pragma once

#include <cstdint>

struct video_output;
using video_t = video_output;

namespace obs_sync_replay {

void RunPacketRangeMkvPoc(const char* encoder_id, video_t* video_a, video_t* video_b, uint32_t duration_seconds,
                          uint32_t warmup_milliseconds);

} // namespace obs_sync_replay
