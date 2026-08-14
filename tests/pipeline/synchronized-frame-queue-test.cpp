#include "pipeline/synchronized-frame-queue.hpp"
#include "timeline/master-frame-timeline.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>

namespace {

using obs_sync_replay::MasterFrame;
using obs_sync_replay::SynchronizedFramePairIdentity;
using obs_sync_replay::SynchronizedFrameQueue;
using obs_sync_replay::SynchronizedFrameQueueResult;
using obs_sync_replay::detail::MasterFrameTimeline;

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

MasterFrame Frame(const uint64_t pts_ns) {
    static MasterFrameTimeline timeline;
    std::optional<MasterFrame> frame;
    Require(timeline.Observe(pts_ns, frame) ==
                obs_sync_replay::MasterFrameObservationResult::Accepted,
            "test master frame must be accepted");
    return *frame;
}

SynchronizedFramePairIdentity CompletePair(const MasterFrame& frame) {
    return {frame, frame, true};
}

void TestAcceptsOneCompleteSynchronizedPair() {
    SynchronizedFrameQueue queue(2);
    const MasterFrame frame = Frame(100);

    Require(queue.TryRetain(CompletePair(frame)) == SynchronizedFrameQueueResult::Retained,
            "complete matching pair must be retained");
    Require(queue.size() == 1, "retained pair must occupy one shared queue slot");
}

void TestPreservesIdentityAndFifoOrdering() {
    SynchronizedFrameQueue queue(3);
    const MasterFrame first = Frame(200);
    const MasterFrame second = Frame(300);

    Require(queue.TryRetain(CompletePair(first)) == SynchronizedFrameQueueResult::Retained,
            "first pair must be retained");
    Require(queue.TryRetain(CompletePair(second)) == SynchronizedFrameQueueResult::Retained,
            "second pair must be retained");
    const std::optional<MasterFrame> first_taken = queue.TakeNext();
    const std::optional<MasterFrame> second_taken = queue.TakeNext();
    Require(first_taken.has_value() && first_taken->frame_id() == first.frame_id() &&
                first_taken->pts_ns() == first.pts_ns(),
            "first retained identity must be returned unchanged");
    Require(second_taken.has_value() && second_taken->frame_id() == second.frame_id() &&
                second_taken->pts_ns() == second.pts_ns(),
            "second retained identity must follow FIFO order");
}

void TestCapacityRejectsWholeIncomingPair() {
    SynchronizedFrameQueue queue(1);
    const MasterFrame first = Frame(400);
    const MasterFrame second = Frame(500);

    Require(queue.TryRetain(CompletePair(first)) == SynchronizedFrameQueueResult::Retained,
            "first pair must fill the queue");
    Require(queue.TryRetain(CompletePair(second)) == SynchronizedFrameQueueResult::Capacity,
            "full queue must reject the incoming pair atomically");
    Require(queue.size() == 1, "capacity rejection must not alter retained pairs");
    const std::optional<MasterFrame> retained = queue.TakeNext();
    Require(retained.has_value() && retained->frame_id() == first.frame_id(),
            "capacity rejection must preserve the earlier pair");
}

void TestRejectsHalfAndDivergentPairs() {
    SynchronizedFrameQueue queue(2);
    const MasterFrame first = Frame(600);
    const MasterFrame second = Frame(700);

    Require(queue.TryRetain({first, first, false}) == SynchronizedFrameQueueResult::InvalidPair,
            "half pair must be rejected");
    Require(queue.TryRetain({first, second, true}) == SynchronizedFrameQueueResult::InvalidPair,
            "different master identities must be rejected");
    Require(queue.size() == 0, "invalid pairs must never enter the shared queue");
}

void TestResetDropsAllPairsTogether() {
    SynchronizedFrameQueue queue(2);
    Require(queue.TryRetain(CompletePair(Frame(800))) == SynchronizedFrameQueueResult::Retained,
            "pair must be retained before reset");
    queue.Reset();
    Require(queue.size() == 0, "reset must remove all synchronized pairs");
    Require(!queue.TakeNext().has_value(), "reset queue must not expose stale identity");
}

} // namespace

int main() {
    TestAcceptsOneCompleteSynchronizedPair();
    TestPreservesIdentityAndFifoOrdering();
    TestCapacityRejectsWholeIncomingPair();
    TestRejectsHalfAndDivergentPairs();
    TestResetDropsAllPairsTogether();
    return EXIT_SUCCESS;
}
