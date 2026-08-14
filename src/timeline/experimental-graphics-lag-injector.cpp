#include "timeline/experimental-graphics-lag-injector.hpp"

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>

namespace obs_sync_replay::detail {

namespace {

constexpr char kDelayEnvironmentVariable[] = "OBS_SYNC_REPLAY_EXPERIMENT_INJECT_LAG_MS";
constexpr char kCadenceEnvironmentVariable[] = "OBS_SYNC_REPLAY_EXPERIMENT_INJECT_LAG_EVERY";
constexpr uint32_t kMaximumDelayMs = 100;
constexpr uint64_t kDefaultCadenceFrames = 600;
constexpr uint64_t kMaximumCadenceFrames = 1'000'000;

std::optional<std::string> ReadEnvironmentValue(const char* const name) {
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || !value) {
        return std::nullopt;
    }

    std::string copied_value(value);
    std::free(value);
    return copied_value;
}

bool ParseUnsigned(const char* const value, const uint64_t maximum, uint64_t& result) noexcept {
    if (!value || *value == '\0') {
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0 || parsed > maximum ||
        parsed > std::numeric_limits<uint64_t>::max()) {
        return false;
    }

    result = static_cast<uint64_t>(parsed);
    return true;
}

} // namespace

ExperimentalGraphicsLagInjector::ExperimentalGraphicsLagInjector(
    const ExperimentalGraphicsLagInjectionStatus status,
    const ExperimentalGraphicsLagInjectionConfiguration configuration) noexcept
    : status_(status), configuration_(configuration) {}

ExperimentalGraphicsLagInjector ExperimentalGraphicsLagInjector::FromEnvironment() {
    const std::optional<std::string> delay_value = ReadEnvironmentValue(kDelayEnvironmentVariable);
    if (!delay_value.has_value()) {
        return {ExperimentalGraphicsLagInjectionStatus::Disabled, {}};
    }

    uint64_t delay_ms = 0;
    if (!ParseUnsigned(delay_value->c_str(), kMaximumDelayMs, delay_ms)) {
        return {ExperimentalGraphicsLagInjectionStatus::InvalidDelay, {}};
    }

    uint64_t cadence_frames = kDefaultCadenceFrames;
    const std::optional<std::string> cadence_value =
        ReadEnvironmentValue(kCadenceEnvironmentVariable);
    if (cadence_value.has_value() &&
        !ParseUnsigned(cadence_value->c_str(), kMaximumCadenceFrames, cadence_frames)) {
        return {ExperimentalGraphicsLagInjectionStatus::InvalidCadence, {}};
    }

    return {ExperimentalGraphicsLagInjectionStatus::Enabled,
            {static_cast<uint32_t>(delay_ms), cadence_frames}};
}

ExperimentalGraphicsLagInjector ExperimentalGraphicsLagInjector::ForTesting(
    const ExperimentalGraphicsLagInjectionConfiguration configuration) {
    if (configuration.delay_ms == 0 || configuration.delay_ms > kMaximumDelayMs) {
        return {ExperimentalGraphicsLagInjectionStatus::InvalidDelay, {}};
    }
    if (configuration.every_master_frames == 0 ||
        configuration.every_master_frames > kMaximumCadenceFrames) {
        return {ExperimentalGraphicsLagInjectionStatus::InvalidCadence, {}};
    }
    return {ExperimentalGraphicsLagInjectionStatus::Enabled, configuration};
}

ExperimentalGraphicsLagInjectionStatus ExperimentalGraphicsLagInjector::status() const noexcept {
    return status_;
}

const ExperimentalGraphicsLagInjectionConfiguration&
ExperimentalGraphicsLagInjector::configuration() const noexcept {
    return configuration_;
}

bool ExperimentalGraphicsLagInjector::ShouldInject(const MasterFrameId frame_id) const noexcept {
    return status_ == ExperimentalGraphicsLagInjectionStatus::Enabled && frame_id != 0 &&
           frame_id % configuration_.every_master_frames == 0;
}

} // namespace obs_sync_replay::detail
