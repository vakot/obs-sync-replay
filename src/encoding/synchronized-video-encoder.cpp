#include "encoding/synchronized-video-encoder.hpp"

#include "encoding/encoder-timestamp.hpp"
#include "encoding/nvenc-video-encoder.hpp"

#include <obs-module.h>

#include <utility>

namespace obs_sync_replay {

namespace {

bool IsSampled(const MasterFrame& master_frame) noexcept {
    return master_frame.frame_id() < 3 || master_frame.frame_id() % 300 == 0;
}

} // namespace

SynchronizedVideoEncoder::SynchronizedVideoEncoder()
    : encoder_a_(std::make_unique<NvencVideoEncoder>(
          OutputSlot::A, [this](EncodedVideoPacket&& packet) { OnEncoderPacket(std::move(packet)); },
          [this](const MasterFrame& frame, const std::string& detail) { OnEncoderFailure(frame, detail); }, operation_gate_)),
      encoder_b_(std::make_unique<NvencVideoEncoder>(
          OutputSlot::B, [this](EncodedVideoPacket&& packet) { OnEncoderPacket(std::move(packet)); },
          [this](const MasterFrame& frame, const std::string& detail) { OnEncoderFailure(frame, detail); }, operation_gate_)) {}

SynchronizedVideoEncoder::~SynchronizedVideoEncoder() {
    Stop();
}

void SynchronizedVideoEncoder::Consume(SynchronizedFramePipeline& pipeline) {
    if (stopped_.load() || failed_.load()) {
        return;
    }

    while (std::unique_ptr<SynchronizedFramePair> pair = pipeline.TakeNext()) {
        if (!SubmitPair(*pair)) {
            return;
        }
    }
}

void SynchronizedVideoEncoder::Stop() noexcept {
    if (stopped_) {
        return;
    }

    stopped_.store(true);
    if (encoder_a_) {
        encoder_a_->Shutdown();
    }
    if (encoder_b_) {
        encoder_b_->Shutdown();
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    packet_tracker_.Reset();
}

bool SynchronizedVideoEncoder::failed() const noexcept {
    return failed_.load();
}

size_t SynchronizedVideoEncoder::pending_packet_pairs() const noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return packet_tracker_.size();
}

bool SynchronizedVideoEncoder::SubmitPair(const SynchronizedFramePair& pair) {
    const MasterFrame& master_frame = pair.master_frame();
    const auto encoder_pts = MasterPtsToEncoderPts(master_frame);
    if (!encoder_pts) {
        Fail(master_frame, "timestamp-overflow", "master PTS cannot fit the NVENC timestamp type");
        return false;
    }
    std::unique_lock<std::recursive_mutex> operation_lock(*operation_gate_, std::try_to_lock);
    if (!operation_lock.owns_lock()) {
        blog(LOG_WARNING,
             "[sync-encode] invariant=8,9 master_frame_id=%llu master_pts=%llu status=dropped "
             "reason=nvenc-operation-gate; complete A/B pair was not submitted",
             static_cast<unsigned long long>(master_frame.frame_id()),
             static_cast<unsigned long long>(master_frame.pts_ns()));
        return true;
    }
    const VideoEncoderSubmitResult prepare_a = encoder_a_->Prepare(pair.output_a());
    const VideoEncoderSubmitResult prepare_b = encoder_b_->Prepare(pair.output_b());
    if (prepare_a == VideoEncoderSubmitResult::Capacity || prepare_b == VideoEncoderSubmitResult::Capacity) {
        blog(LOG_WARNING,
             "[sync-encode] invariant=8,9 master_frame_id=%llu master_pts=%llu status=dropped "
             "reason=async-ring-capacity; complete A/B pair was not submitted",
             static_cast<unsigned long long>(master_frame.frame_id()),
             static_cast<unsigned long long>(master_frame.pts_ns()));
        return true;
    }
    if (prepare_a != VideoEncoderSubmitResult::Submitted || prepare_b != VideoEncoderSubmitResult::Submitted) {
        Fail(master_frame, "prepare-pair", "could not prepare both reusable NVENC input slots");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (packet_tracker_.Begin(master_frame, *encoder_pts) != EncodedPacketTrackerResult::Accepted) {
            Fail(master_frame, "packet-tracker", "duplicate or bounded in-flight packet state");
            return false;
        }
    }
    if (IsSampled(master_frame)) {
        blog(LOG_INFO, "[sync-encode] master_frame_id=%llu master_pts=%llu output=A encoder_pts=%lld status=submitted",
             static_cast<unsigned long long>(master_frame.frame_id()),
             static_cast<unsigned long long>(master_frame.pts_ns()), static_cast<long long>(*encoder_pts));
    }
    if (encoder_a_->Submit(pair.output_a(), *encoder_pts) != VideoEncoderSubmitResult::Submitted) {
        Fail(master_frame, "submission-a", encoder_a_->last_error().c_str());
        return false;
    }
    if (IsSampled(master_frame)) {
        blog(LOG_INFO, "[sync-encode] master_frame_id=%llu master_pts=%llu output=B encoder_pts=%lld status=submitted",
             static_cast<unsigned long long>(master_frame.frame_id()),
             static_cast<unsigned long long>(master_frame.pts_ns()), static_cast<long long>(*encoder_pts));
    }
    if (encoder_b_->Submit(pair.output_b(), *encoder_pts) != VideoEncoderSubmitResult::Submitted) {
        // A is intentionally discarded and the session stops. Continuing here
        // would create independent output timelines after a partial operation.
        Fail(master_frame, "submission-b-after-a", encoder_b_->last_error().c_str());
        return false;
    }
    return true;
}

bool SynchronizedVideoEncoder::RecordPacket(const EncodedVideoPacket& packet) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const EncodedPacketTrackerResult result = packet_tracker_.Record(packet);
    if (result != EncodedPacketTrackerResult::Accepted) {
        Fail(packet.master_frame, "packet-association", EncodedPacketTrackerResultName(result));
        return false;
    }

    if (IsSampled(packet.master_frame)) {
        blog(LOG_INFO,
             "[sync-encode] master_frame_id=%llu output=%s pts=%lld dts=%lld keyframe=%s status=encoded",
             static_cast<unsigned long long>(packet.master_frame.frame_id()), OutputSlotName(packet.output),
             static_cast<long long>(packet.pts), static_cast<long long>(packet.dts),
             packet.keyframe ? "true" : "false");
    }
    return true;
}

void SynchronizedVideoEncoder::OnEncoderPacket(EncodedVideoPacket&& packet) {
    RecordPacket(packet);
}

void SynchronizedVideoEncoder::OnEncoderFailure(const MasterFrame& master_frame,
                                                const std::string& detail) {
    Fail(master_frame, "async-completion", detail.c_str());
}

void SynchronizedVideoEncoder::Fail(const MasterFrame& master_frame, const char* const reason,
                                    const char* const detail) noexcept {
    failed_.store(true);
    blog(LOG_ERROR,
         "[sync-encode] invariant=7,8,9 master_frame_id=%llu master_pts=%llu status=failed "
         "reason=%s detail=%s; encoding session halted",
         static_cast<unsigned long long>(master_frame.frame_id()),
         static_cast<unsigned long long>(master_frame.pts_ns()), reason, detail ? detail : "none");
}

} // namespace obs_sync_replay
