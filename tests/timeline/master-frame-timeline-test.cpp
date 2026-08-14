#include "timeline/experimental-graphics-lag-injector.hpp"
#include "timeline/master-frame-timeline.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>

namespace {

using obs_sync_replay::MasterFrame;
using obs_sync_replay::MasterFrameObservationResult;
using obs_sync_replay::MasterFrameTimingConfigurationResult;
using obs_sync_replay::detail::ExperimentalGraphicsLagInjectionConfiguration;
using obs_sync_replay::detail::ExperimentalGraphicsLagInjectionStatus;
using obs_sync_replay::detail::ExperimentalGraphicsLagInjector;
using obs_sync_replay::detail::MasterFrameTimeline;
using obs_sync_replay::detail::MasterFrameTimingConfiguration;

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

MasterFrame Observe(MasterFrameTimeline& timeline, const uint64_t pts_ns) {
    std::optional<MasterFrame> frame;
    Require(timeline.Observe(pts_ns, frame) == MasterFrameObservationResult::Accepted,
            "expected master PTS observation to be accepted");
    return *frame;
}

void TestInitialIdentityAndMonotonicProgression() {
    MasterFrameTimeline timeline;
    const MasterFrame first = Observe(timeline, 1'000'000'000);
    const MasterFrame second = Observe(timeline, 1'016'666'667);
    const MasterFrame third = Observe(timeline, 1'033'333'334);

    Require(first.frame_id() == 0, "first master frame ID must be zero");
    Require(first.pts_ns() == 1'000'000'000, "first master frame must retain its observed PTS");
    Require(second.frame_id() == 1 && third.frame_id() == 2,
            "master frame IDs must increase by one");
    Require(first.pts_ns() < second.pts_ns() && second.pts_ns() < third.pts_ns(),
            "master PTS values must be strictly monotonic");
}

void TestResetStartsANewSynchronizationSession() {
    MasterFrameTimeline timeline;
    Observe(timeline, 100);
    Observe(timeline, 200);

    timeline.Reset();
    const MasterFrame first_after_reset = Observe(timeline, 50);

    Require(first_after_reset.frame_id() == 0, "reset must restart frame identity at zero");
    Require(first_after_reset.pts_ns() == 50, "reset must discard the prior session PTS bound");
}

void TestNonMonotonicPtsAreRejectedWithoutAdvancingIdentity() {
    MasterFrameTimeline timeline;
    const MasterFrame first = Observe(timeline, 1'000);
    std::optional<MasterFrame> rejected;

    Require(timeline.Observe(1'000, rejected) == MasterFrameObservationResult::NonMonotonicPts,
            "duplicate PTS must be rejected");
    Require(timeline.Observe(999, rejected) == MasterFrameObservationResult::NonMonotonicPts,
            "decreasing PTS must be rejected");

    const MasterFrame second = Observe(timeline, 1'001);
    Require(first.frame_id() == 0 && second.frame_id() == 1,
            "rejected PTS must not advance master frame identity");
}

void TestRuntimeIntervalChangePreservesTimelineIdentity() {
    MasterFrameTimeline timeline;
    const MasterFrame first = Observe(timeline, 1'000'000'000);
    const MasterFrame second = Observe(timeline, 1'033'333'333);
    const MasterFrame third = Observe(timeline, 1'050'000'000);
    const MasterFrame fourth = Observe(timeline, 1'066'666'667);
    const MasterFrame fifth = Observe(timeline, 1'100'000'000);

    Require(first.frame_id() == 0 && second.frame_id() == 1 && third.frame_id() == 2 &&
                fourth.frame_id() == 3 && fifth.frame_id() == 4,
            "live interval changes must not reset or skip master frame IDs");
    Require(first.pts_ns() < second.pts_ns() && second.pts_ns() < third.pts_ns() &&
                third.pts_ns() < fourth.pts_ns() && fourth.pts_ns() < fifth.pts_ns(),
            "live interval changes must retain observed monotonic master PTS values");
}

void TestTimingConfigurationTracksRuntimeIntervalChanges() {
    MasterFrameTimingConfiguration timing_configuration;

    Require(timing_configuration.ObserveFrameInterval(33'333'333) ==
                MasterFrameTimingConfigurationResult::Initialized,
            "first configured interval must initialize timing configuration");
    Require(timing_configuration.ObserveFrameInterval(33'333'333) ==
                MasterFrameTimingConfigurationResult::Unchanged,
            "unchanged configured interval must not be reported as a transition");
    Require(timing_configuration.ObserveFrameInterval(16'666'667) ==
                MasterFrameTimingConfigurationResult::Changed,
            "30 to 60 FPS interval change must be reported once");
    Require(timing_configuration.ObserveFrameInterval(16'666'667) ==
                MasterFrameTimingConfigurationResult::Unchanged,
            "stable 60 FPS interval must not be reported as a transition");
    Require(timing_configuration.ObserveFrameInterval(33'333'333) ==
                MasterFrameTimingConfigurationResult::Changed,
            "60 to 30 FPS interval change must be reported once");
    Require(timing_configuration.frame_interval_ns().has_value() &&
                *timing_configuration.frame_interval_ns() == 33'333'333,
            "timing configuration must retain the latest configured interval");
    Require(timing_configuration.ObserveFrameInterval(0) ==
                MasterFrameTimingConfigurationResult::InvalidInterval,
            "zero configured interval must be rejected without replacing the last valid interval");
    Require(timing_configuration.frame_interval_ns().has_value() &&
                *timing_configuration.frame_interval_ns() == 33'333'333,
            "invalid timing configuration must not alter the cached interval");
}

void TestExperimentalGraphicsLagInjectorIsBoundedAndDeterministic() {
    const ExperimentalGraphicsLagInjector injector =
        ExperimentalGraphicsLagInjector::ForTesting({25, 600});

    Require(injector.status() == ExperimentalGraphicsLagInjectionStatus::Enabled,
            "valid research lag configuration must be enabled");
    Require(!injector.ShouldInject(0), "research lag injection must not trigger at frame zero");
    Require(!injector.ShouldInject(599),
            "research lag injection must respect the configured cadence");
    Require(injector.ShouldInject(600),
            "research lag injection must trigger at the configured cadence");
    Require(injector.ShouldInject(1'200), "research lag injection must repeat deterministically");

    const ExperimentalGraphicsLagInjector zero_delay =
        ExperimentalGraphicsLagInjector::ForTesting({0, 600});
    Require(zero_delay.status() == ExperimentalGraphicsLagInjectionStatus::InvalidDelay,
            "zero research lag delay must be disabled");

    const ExperimentalGraphicsLagInjector zero_cadence =
        ExperimentalGraphicsLagInjector::ForTesting({25, 0});
    Require(zero_cadence.status() == ExperimentalGraphicsLagInjectionStatus::InvalidCadence,
            "zero research lag cadence must be disabled");
}

} // namespace

int main() {
    TestInitialIdentityAndMonotonicProgression();
    TestResetStartsANewSynchronizationSession();
    TestNonMonotonicPtsAreRejectedWithoutAdvancingIdentity();
    TestRuntimeIntervalChangePreservesTimelineIdentity();
    TestTimingConfigurationTracksRuntimeIntervalChanges();
    TestExperimentalGraphicsLagInjectorIsBoundedAndDeterministic();
    return EXIT_SUCCESS;
}
