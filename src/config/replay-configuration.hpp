#pragma once

#include <cstddef>
#include <cstdint>

namespace obs_sync_replay {

constexpr uint32_t kDefaultReplayDurationSeconds = 20;
constexpr size_t kDefaultReplayMemoryMegabytes = 512;
constexpr size_t kReplayEmergencyMemoryBudgetBytes = 30 * 1024 * 1024;

enum class ReplayOutputMode : uint8_t {
    Simple,
    Advanced,
};

struct ReplayProfileValues final {
    ReplayOutputMode output_mode = ReplayOutputMode::Simple;
    bool replay_enabled = false;
    bool stock_backend_available = true;
    bool stock_memory_limit_applies = true;
    uint32_t duration_seconds = kDefaultReplayDurationSeconds;
    size_t memory_megabytes = kDefaultReplayMemoryMegabytes;
};

struct ReplayConfiguration final {
    bool enabled = false;
    uint64_t target_duration_ns = static_cast<uint64_t>(kDefaultReplayDurationSeconds) * 1'000'000'000ULL;
    size_t memory_budget_bytes = kReplayEmergencyMemoryBudgetBytes;
    bool memory_limit_configured = false;
};

ReplayConfiguration MakeReplayConfiguration(const ReplayProfileValues& values) noexcept;

} // namespace obs_sync_replay
