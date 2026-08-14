#include "encoding/encoded-packet-tracker.hpp"
#include "encoding/encoder-timestamp.hpp"
#include "timeline/master-frame-timeline.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>

namespace {

using obs_sync_replay::EncodedPacketTracker;
using obs_sync_replay::EncodedPacketTrackerResult;
using obs_sync_replay::EncodedVideoPacket;
using obs_sync_replay::MasterFrame;
using obs_sync_replay::MasterFrameObservationResult;
using obs_sync_replay::MasterPtsToEncoderPts;
using obs_sync_replay::OutputSlot;
using obs_sync_replay::detail::MasterFrameTimeline;

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

MasterFrame Observe(MasterFrameTimeline& timeline, const uint64_t pts_ns) {
    std::optional<MasterFrame> frame;
    Require(timeline.Observe(pts_ns, frame) == MasterFrameObservationResult::Accepted,
            "expected a monotonic master frame");
    return *frame;
}

EncodedVideoPacket Packet(const MasterFrame& frame, const OutputSlot output, const uint8_t byte) {
    const auto encoder_pts = MasterPtsToEncoderPts(frame);
    Require(encoder_pts.has_value(), "master PTS must map to an encoder PTS");
    return {frame, output, *encoder_pts, *encoder_pts, false, {byte}};
}

void TestTimestampMappingIsExactAndMonotonicAcrossCadenceChanges() {
    MasterFrameTimeline timeline;
    const MasterFrame at_60_a = Observe(timeline, 1'000'000'000);
    const MasterFrame at_60_b = Observe(timeline, 1'016'666'667);
    const MasterFrame at_30 = Observe(timeline, 1'050'000'000);
    const MasterFrame at_60_c = Observe(timeline, 1'066'666'667);

    const auto pts_a = MasterPtsToEncoderPts(at_60_a);
    const auto pts_b = MasterPtsToEncoderPts(at_60_b);
    const auto pts_c = MasterPtsToEncoderPts(at_30);
    const auto pts_d = MasterPtsToEncoderPts(at_60_c);
    Require(*pts_a == 1'000'000'000 && *pts_b == 1'016'666'667 && *pts_c == 1'050'000'000 &&
                *pts_d == 1'066'666'667,
            "nanosecond master PTS must map exactly to the 1/1e9 encoder timebase");
    Require(*pts_a < *pts_b && *pts_b < *pts_c && *pts_c < *pts_d,
            "FPS changes must not rebase or make encoder PTS nonmonotonic");
}

void TestOutOfOrderCompletionRetainsSubmittedIdentity() {
    MasterFrameTimeline timeline;
    const MasterFrame frame_0 = Observe(timeline, 100);
    const MasterFrame frame_1 = Observe(timeline, 200);
    EncodedPacketTracker tracker(4);
    Require(tracker.Begin(frame_0, 100) == EncodedPacketTrackerResult::Accepted,
            "first pair submission must be accepted");
    Require(tracker.Begin(frame_1, 200) == EncodedPacketTrackerResult::Accepted,
            "second pair submission must be accepted");

    Require(tracker.Record(Packet(frame_1, OutputSlot::B, 0xB1)) == EncodedPacketTrackerResult::Accepted,
            "B[N+1] must retain its own submitted identity");
    Require(tracker.Record(Packet(frame_0, OutputSlot::A, 0xA0)) == EncodedPacketTrackerResult::Accepted,
            "A[N] must retain its own submitted identity");
    Require(tracker.Record(Packet(frame_1, OutputSlot::A, 0xA1)) == EncodedPacketTrackerResult::Accepted,
            "A[N+1] must complete independently");
    Require(tracker.Record(Packet(frame_0, OutputSlot::B, 0xB0)) == EncodedPacketTrackerResult::Accepted,
            "B[N] must complete independently");
    Require(tracker.size() == 0, "both completed pairs must be removed without completion-order pairing");
}

void TestDuplicateAndUnknownPacketsAreRejected() {
    MasterFrameTimeline timeline;
    const MasterFrame frame = Observe(timeline, 100);
    const MasterFrame unknown = Observe(timeline, 200);
    EncodedPacketTracker tracker(2);
    Require(tracker.Begin(frame, 100) == EncodedPacketTrackerResult::Accepted,
            "submission must be recorded before completion");
    const EncodedVideoPacket output_a = Packet(frame, OutputSlot::A, 0xA0);
    Require(tracker.Record(output_a) == EncodedPacketTrackerResult::Accepted, "first A packet must be accepted");
    Require(tracker.Record(output_a) == EncodedPacketTrackerResult::Duplicate,
            "duplicate packet must be observable");
    Require(tracker.Record(Packet(unknown, OutputSlot::B, 0xB1)) ==
                EncodedPacketTrackerResult::UnknownMasterFrame,
            "a packet cannot be attached to an unsubmitted master frame");
}

void TestPacketOwnershipAndBoundedPressure() {
    MasterFrameTimeline timeline;
    const MasterFrame frame_0 = Observe(timeline, 100);
    const MasterFrame frame_1 = Observe(timeline, 200);
    const MasterFrame frame_2 = Observe(timeline, 300);
    auto packet = Packet(frame_0, OutputSlot::A, 0xA0);
    const uint8_t original = packet.bytes.front();
    std::vector<uint8_t> consumer_copy = packet.bytes;
    packet.bytes.front() = 0;
    Require(consumer_copy.front() == original,
            "encoded packet consumers must own bytes independently of reusable producer storage");

    EncodedPacketTracker tracker(2);
    Require(tracker.Begin(frame_0, 100) == EncodedPacketTrackerResult::Accepted,
            "first bounded pair must be accepted");
    Require(tracker.Begin(frame_1, 200) == EncodedPacketTrackerResult::Accepted,
            "second bounded pair must be accepted");
    Require(tracker.Begin(frame_2, 300) == EncodedPacketTrackerResult::Capacity,
            "packet tracking pressure must reject whole later pairs deterministically");
}

void TestNoABIdentityDrift() {
    MasterFrameTimeline timeline;
    const MasterFrame frame = Observe(timeline, 123'456'789);
    EncodedPacketTracker tracker(1);
    Require(tracker.Begin(frame, 123'456'789) == EncodedPacketTrackerResult::Accepted,
            "synchronized submission must have one shared encoder timestamp");
    const auto output_a = Packet(frame, OutputSlot::A, 0xA0);
    const auto output_b = Packet(frame, OutputSlot::B, 0xB0);
    Require(output_a.master_frame.frame_id() == output_b.master_frame.frame_id() &&
                output_a.master_frame.pts_ns() == output_b.master_frame.pts_ns() && output_a.pts == output_b.pts,
            "A and B packet identities must derive from the same master frame");
    Require(tracker.Record(output_a) == EncodedPacketTrackerResult::Accepted,
            "A result must preserve shared identity");
    Require(tracker.Record(output_b) == EncodedPacketTrackerResult::Accepted,
            "B result must preserve shared identity");
}

} // namespace

int main() {
    TestTimestampMappingIsExactAndMonotonicAcrossCadenceChanges();
    TestOutOfOrderCompletionRetainsSubmittedIdentity();
    TestDuplicateAndUnknownPacketsAreRejected();
    TestPacketOwnershipAndBoundedPressure();
    TestNoABIdentityDrift();
    return EXIT_SUCCESS;
}
