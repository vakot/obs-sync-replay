#include "config/replay-configuration.hpp"

#include <limits>

namespace obs_sync_replay {

ReplayConfiguration MakeReplayConfiguration(const ReplayProfileValues& values) noexcept {
    ReplayConfiguration configuration;
    const uint64_t duration_seconds = values.duration_seconds == 0 ? 1 : values.duration_seconds;
    configuration.enabled = values.replay_enabled && values.stock_backend_available;
    configuration.target_duration_ns = duration_seconds > std::numeric_limits<uint64_t>::max() / 1'000'000'000ULL
                                         ? std::numeric_limits<uint64_t>::max()
                                         : duration_seconds * 1'000'000'000ULL;
    configuration.memory_limit_configured = values.stock_memory_limit_applies;

    const size_t memory_megabytes = values.memory_megabytes == 0 ? 1 : values.memory_megabytes;
    if (values.stock_memory_limit_applies &&
        memory_megabytes <= std::numeric_limits<size_t>::max() / (1024 * 1024)) {
        configuration.memory_budget_bytes = memory_megabytes * 1024 * 1024;
    } else {
        configuration.memory_budget_bytes = kReplayEmergencyMemoryBudgetBytes;
        configuration.memory_limit_configured = false;
    }
    return configuration;
}

} // namespace obs_sync_replay
