#include "config/replay-configuration.hpp"

#include <cstdlib>
#include <iostream>

namespace {

using namespace obs_sync_replay;

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void TestDisabledConfigurationUsesObsDefaults() {
    ReplayProfileValues values;
    const ReplayConfiguration configuration = MakeReplayConfiguration(values);
    Require(!configuration.enabled, "disabled OBS replay must remain unavailable");
    Require(configuration.target_duration_ns == 20'000'000'000ULL, "OBS default duration must be 20 seconds");
    Require(configuration.memory_budget_bytes == 512ULL * 1024ULL * 1024ULL,
            "configured OBS memory must be converted from MB to bytes");
    Require(configuration.memory_limit_configured, "stock memory limit must be reported as configured");
}

void TestConfiguredReplayValues() {
    ReplayProfileValues values;
    values.replay_enabled = true;
    values.duration_seconds = 15;
    values.memory_megabytes = 64;
    const ReplayConfiguration configuration = MakeReplayConfiguration(values);
    Require(configuration.enabled, "available enabled OBS replay must be available");
    Require(configuration.target_duration_ns == 15'000'000'000ULL, "duration must use one shared nanosecond target");
    Require(configuration.memory_budget_bytes == 64ULL * 1024ULL * 1024ULL,
            "memory must use the OBS binary-MB conversion");
    Require(configuration.memory_limit_configured, "configured stock memory must be marked configured");
}

void TestStockUnlimitedModesUseExplicitFallback() {
    ReplayProfileValues values;
    values.replay_enabled = true;
    values.stock_memory_limit_applies = false;
    const ReplayConfiguration configuration = MakeReplayConfiguration(values);
    Require(configuration.enabled, "unlimited stock mode must not disable plugin replay");
    Require(configuration.memory_budget_bytes == kReplayEmergencyMemoryBudgetBytes,
            "unlimited stock mode must use the explicit emergency bound");
    Require(!configuration.memory_limit_configured, "unlimited stock mode must be observable");

    values.stock_backend_available = false;
    Require(!MakeReplayConfiguration(values).enabled, "unsupported stock backend must reject replay controls");
}

} // namespace

int main() {
    TestDisabledConfigurationUsesObsDefaults();
    TestConfiguredReplayValues();
    TestStockUnlimitedModesUseExplicitFallback();
    return EXIT_SUCCESS;
}
