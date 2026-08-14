#include "timeline/master-frame-timeline/master-frame-timeline.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>

namespace {

using obs_sync_replay::MasterFrame;
using obs_sync_replay::MasterFrameObservationResult;
using obs_sync_replay::detail::MasterFrameTimeline;

void Require(const bool condition, const char *const message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

MasterFrame Observe(MasterFrameTimeline &timeline, const uint64_t pts_ns) {
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
    Require(second.frame_id() == 1 && third.frame_id() == 2, "master frame IDs must increase by one");
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

} // namespace

int main() {
    TestInitialIdentityAndMonotonicProgression();
    TestResetStartsANewSynchronizationSession();
    TestNonMonotonicPtsAreRejectedWithoutAdvancingIdentity();
    return EXIT_SUCCESS;
}
