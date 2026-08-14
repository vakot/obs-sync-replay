#pragma once

#include "timeline/master-frame.hpp"

#include <cstdint>

namespace obs_sync_replay::detail {

enum class ExperimentalGraphicsLagInjectionStatus {
    Disabled,
    Enabled,
    InvalidDelay,
    InvalidCadence,
};

struct ExperimentalGraphicsLagInjectionConfiguration final {
    uint32_t delay_ms = 0;
    uint64_t every_master_frames = 0;
};

// Research-only process configuration. It has no effect unless the explicit
// delay environment variable is valid at module startup.
class ExperimentalGraphicsLagInjector final {
  public:
    ExperimentalGraphicsLagInjector() = default;

    static ExperimentalGraphicsLagInjector FromEnvironment();
    static ExperimentalGraphicsLagInjector
    ForTesting(ExperimentalGraphicsLagInjectionConfiguration configuration);

    ExperimentalGraphicsLagInjectionStatus status() const noexcept;
    const ExperimentalGraphicsLagInjectionConfiguration& configuration() const noexcept;
    bool ShouldInject(MasterFrameId frame_id) const noexcept;

  private:
    ExperimentalGraphicsLagInjector(
        ExperimentalGraphicsLagInjectionStatus status,
        ExperimentalGraphicsLagInjectionConfiguration configuration) noexcept;

    ExperimentalGraphicsLagInjectionStatus status_ =
        ExperimentalGraphicsLagInjectionStatus::Disabled;
    ExperimentalGraphicsLagInjectionConfiguration configuration_{};
};

} // namespace obs_sync_replay::detail
