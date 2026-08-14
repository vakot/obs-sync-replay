#include "rendering/scene-render-pair-tracker.hpp"
#include "timeline/master-frame-timeline.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>

namespace {

using obs_sync_replay::MasterFrame;
using obs_sync_replay::OutputSlot;
using obs_sync_replay::SceneRenderPairTracker;
using obs_sync_replay::SceneRenderResult;
using obs_sync_replay::SceneRenderStatus;
using obs_sync_replay::detail::MasterFrameTimeline;

void Require(const bool condition, const char *const message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

MasterFrame Frame(const uint64_t pts_ns) {
    static MasterFrameTimeline timeline;
    std::optional<MasterFrame> frame;
    Require(timeline.Observe(pts_ns, frame) == obs_sync_replay::MasterFrameObservationResult::Accepted,
            "test master frame must be accepted");
    return *frame;
}

SceneRenderResult Result(const MasterFrame &frame, const OutputSlot output) {
    return {frame, output, SceneRenderStatus::Rendered, "test scene", 1920, 1080, 0, 0, nullptr};
}

void TestOneMasterFrameRequiresOneAttemptPerOutput() {
    SceneRenderPairTracker tracker;
    const MasterFrame frame = Frame(700);

    Require(tracker.Begin(frame), "first master frame dispatch must begin");
    Require(tracker.Record(Result(frame, OutputSlot::A)), "output A must accept the current master frame");
    Require(!tracker.IsComplete(), "a pair must remain incomplete until output B is attempted");
    Require(tracker.Record(Result(frame, OutputSlot::B)), "output B must accept the current master frame");
    Require(tracker.IsComplete(), "both output attempts must complete the one master frame pair");
}

void TestDuplicateOutputIsRejected() {
    SceneRenderPairTracker tracker;
    const MasterFrame frame = Frame(900);

    Require(tracker.Begin(frame), "master frame dispatch must begin");
    Require(tracker.Record(Result(frame, OutputSlot::A)), "first output A attempt must be accepted");
    Require(!tracker.Record(Result(frame, OutputSlot::A)), "duplicate output A attempt must be rejected");
    Require(!tracker.IsComplete(), "duplicate output must not fabricate the missing output B attempt");
}

void TestWrongOrStaleFrameIsRejected() {
    SceneRenderPairTracker tracker;
    const MasterFrame stale = Frame(1'000);
    const MasterFrame current = Frame(1'001);
    const MasterFrame wrong_frame = Frame(1'002);

    Require(tracker.Begin(current), "master frame dispatch must begin");
    Require(!tracker.Record(Result(stale, OutputSlot::A)), "stale master frame result must be rejected");
    Require(!tracker.Record(Result(wrong_frame, OutputSlot::A)), "wrong master frame result must be rejected");
    Require(tracker.Record(Result(current, OutputSlot::A)), "current output A attempt must remain valid");
    Require(tracker.Record(Result(current, OutputSlot::B)), "current output B attempt must remain valid");
    Require(tracker.IsComplete(), "rejected stale results must not alter the active pair");
}

void TestIncompletePairPreventsNewDispatchUntilReset() {
    SceneRenderPairTracker tracker;
    const MasterFrame first = Frame(1'100);
    const MasterFrame second = Frame(1'200);

    Require(tracker.Begin(first), "first dispatch must begin");
    Require(tracker.Record(Result(first, OutputSlot::A)), "first output A attempt must be accepted");
    Require(!tracker.Begin(second), "incomplete pair must reject a new master-frame dispatch");
    tracker.Reset();
    Require(tracker.Begin(second), "explicit reset must clear incomplete pair state");
}

} // namespace

int main() {
    TestOneMasterFrameRequiresOneAttemptPerOutput();
    TestDuplicateOutputIsRejected();
    TestWrongOrStaleFrameIsRejected();
    TestIncompletePairPreventsNewDispatchUntilReset();
    return EXIT_SUCCESS;
}
