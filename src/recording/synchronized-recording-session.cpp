#include "recording/synchronized-recording-session.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace obs_sync_replay {

namespace {

std::optional<uint64_t> LargestCommonCts(const EncodedPacketBuffer& a, const EncodedPacketBuffer& b,
                                         const uint64_t start_inclusive,
                                         const std::optional<uint64_t> lower_exclusive, const uint64_t upper_inclusive) {
    std::optional<uint64_t> result;
    for (const EncodedPacket& packet : a.Snapshot()) {
        if (packet.source_cts >= start_inclusive &&
            (!lower_exclusive || packet.source_cts > *lower_exclusive) && packet.source_cts <= upper_inclusive &&
            b.Contains(packet.source_cts)) {
            result = packet.source_cts;
        }
    }
    return result;
}

} // namespace

SynchronizedRecordingSession::SynchronizedRecordingSession(
    SynchronizedRecordingConfig config, PacketStreamConfig stream_a, PacketStreamConfig stream_b,
    std::unique_ptr<SynchronizedPacketSink> sink_a, std::unique_ptr<SynchronizedPacketSink> sink_b)
    : config_(config), stream_a_(std::move(stream_a)), stream_b_(std::move(stream_b)), sink_a_(std::move(sink_a)),
      sink_b_(std::move(sink_b)), buffer_a_(config_.pre_roll_capacity_bytes),
      buffer_b_(config_.pre_roll_capacity_bytes) {}

bool SynchronizedRecordingSession::Start(const uint64_t requested_start_cts) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != SynchronizedRecordingState::Idle || !sink_a_ || !sink_b_) {
        return Fail(SynchronizedRecordingFailure::InvalidTransition);
    }
    requested_start_cts_ = requested_start_cts;
    state_ = SynchronizedRecordingState::Starting;
    return true;
}

bool SynchronizedRecordingSession::SetStreamExtraData(const RecordingStream stream, std::vector<uint8_t> extra_data) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != SynchronizedRecordingState::Starting || extra_data.empty()) {
        return state_ != SynchronizedRecordingState::Failed;
    }
    if (stream == RecordingStream::A) {
        stream_a_.extra_data = std::move(extra_data);
    } else {
        stream_b_.extra_data = std::move(extra_data);
    }
    return true;
}

bool SynchronizedRecordingSession::SubmitPacket(const RecordingStream stream, EncodedPacket packet) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != SynchronizedRecordingState::Starting && state_ != SynchronizedRecordingState::Running &&
        state_ != SynchronizedRecordingState::Draining) {
        return Fail(SynchronizedRecordingFailure::InvalidTransition);
    }

    EncodedPacketBuffer& buffer = Buffer(stream);
    const size_t packet_bytes = packet.payload.size();
    const EncodedPacketBufferResult result = buffer.Push(std::move(packet));
    switch (result) {
    case EncodedPacketBufferResult::Retained:
        if (state_ == SynchronizedRecordingState::Starting) {
            if (stream == RecordingStream::A) {
                ++pre_roll_packet_count_a_;
                pre_roll_bytes_a_ += packet_bytes;
            } else {
                ++pre_roll_packet_count_b_;
                pre_roll_bytes_b_ += packet_bytes;
            }
        }
        peak_retained_bytes_ = std::max(peak_retained_bytes_, buffer_a_.bytes() + buffer_b_.bytes());
        break;
    case EncodedPacketBufferResult::MissingTiming:
        return Fail(SynchronizedRecordingFailure::PacketTiming);
    case EncodedPacketBufferResult::DuplicateSourceCts:
        return Fail(SynchronizedRecordingFailure::DuplicateSourceCts);
    case EncodedPacketBufferResult::Capacity:
        return Fail(SynchronizedRecordingFailure::BufferCapacity);
    }

    if (state_ == SynchronizedRecordingState::Starting) {
        return EstablishCommonStart();
    }
    if (state_ == SynchronizedRecordingState::Running) {
        return FlushStablePackets();
    }
    return true;
}

bool SynchronizedRecordingSession::PollStart(const uint64_t current_source_cts) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != SynchronizedRecordingState::Starting) {
        return state_ != SynchronizedRecordingState::Failed;
    }
    if (EstablishCommonStart()) {
        return true;
    }
    if (current_source_cts >= requested_start_cts_ &&
        current_source_cts - requested_start_cts_ > config_.max_start_wait_cts) {
        return Fail(SynchronizedRecordingFailure::StartTimeout);
    }
    return true;
}

bool SynchronizedRecordingSession::RequestStop(const uint64_t requested_stop_cts) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != SynchronizedRecordingState::Running) {
        return Fail(SynchronizedRecordingFailure::InvalidTransition);
    }
    requested_stop_cts_ = requested_stop_cts;
    state_ = SynchronizedRecordingState::Draining;
    return true;
}

bool SynchronizedRecordingSession::CompleteDrain() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != SynchronizedRecordingState::Draining || !selected_range_) {
        return Fail(SynchronizedRecordingFailure::InvalidTransition);
    }

    uint64_t common_end_cts = 0;
    if (has_flushed_cts_) {
        common_end_cts = last_flushed_cts_;
    }
    const std::optional<uint64_t> lower_exclusive = has_flushed_cts_
                                                        ? std::optional<uint64_t>(last_flushed_cts_)
                                                        : std::nullopt;
    const std::optional<uint64_t> pending_candidate =
        LargestCommonCts(buffer_a_, buffer_b_, selected_range_->start_cts, lower_exclusive, requested_stop_cts_);
    if (pending_candidate) {
        common_end_cts = *pending_candidate;
    }
    if (common_end_cts < selected_range_->start_cts) {
        return Fail(SynchronizedRecordingFailure::MissingCommonRange);
    }
    if (!ValidateCommonPacketRange(buffer_a_, buffer_b_,
                                   CommonPacketRange{selected_range_->start_cts, common_end_cts})
             .range) {
        return Fail(SynchronizedRecordingFailure::RangeMismatch);
    }

    if (!FlushThrough(common_end_cts)) {
        return false;
    }
    return FinalizeAt(common_end_cts);
}

void SynchronizedRecordingSession::Abort() noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    AbortUnlocked();
}

void SynchronizedRecordingSession::AbortUnlocked() noexcept {
    if (sink_a_) {
        sink_a_->Abort();
    }
    if (sink_b_) {
        sink_b_->Abort();
    }
    buffer_a_.Clear();
    buffer_b_.Clear();
    if (failure_ == SynchronizedRecordingFailure::None) {
        failure_ = SynchronizedRecordingFailure::Aborted;
    }
    state_ = SynchronizedRecordingState::Failed;
}

bool SynchronizedRecordingSession::EstablishCommonStart() {
    if (state_ != SynchronizedRecordingState::Starting) {
        return state_ == SynchronizedRecordingState::Running;
    }
    const CommonPacketRangeResult result = SelectCommonStart(buffer_a_, buffer_b_, requested_start_cts_);
    if (!result.range) {
        return true;
    }
    selected_range_ = result.range;
    if (!buffer_a_.SetCapacityBytes(config_.tail_capacity_bytes) ||
        !buffer_b_.SetCapacityBytes(config_.tail_capacity_bytes)) {
        return Fail(SynchronizedRecordingFailure::BufferCapacity);
    }
    if (!sink_a_->Open(stream_a_, selected_range_->start_cts) ||
        !sink_b_->Open(stream_b_, selected_range_->start_cts)) {
        sink_a_->Abort();
        sink_b_->Abort();
        return Fail(SynchronizedRecordingFailure::SinkOpen);
    }
    state_ = SynchronizedRecordingState::Running;
    return FlushStablePackets();
}

bool SynchronizedRecordingSession::FlushStablePackets() {
    // Encoder callbacks can deliver a higher source CTS before an earlier
    // encoded packet. Retain the bounded packet window until the shared stop
    // range is known; committing a partial prefix would make late packets
    // impossible to place without changing temporal identity.
    return true;
}

bool SynchronizedRecordingSession::FlushThrough(const uint64_t source_cts) {
    if (!selected_range_ || source_cts < selected_range_->start_cts) {
        return Fail(SynchronizedRecordingFailure::MissingCommonRange);
    }
    const std::optional<uint64_t> lower_exclusive = has_flushed_cts_
                                                        ? std::optional<uint64_t>(last_flushed_cts_)
                                                        : std::nullopt;
    const std::vector<EncodedPacket> packets_a = buffer_a_.SnapshotThrough(source_cts);
    const std::vector<EncodedPacket> packets_b = buffer_b_.SnapshotThrough(source_cts);
    for (const EncodedPacket& packet : packets_a) {
        if (packet.source_cts >= selected_range_->start_cts &&
            (!lower_exclusive || packet.source_cts > *lower_exclusive) && packet.source_cts <= source_cts &&
            !buffer_b_.Contains(packet.source_cts)) {
            return Fail(SynchronizedRecordingFailure::RangeMismatch);
        }
    }
    for (const EncodedPacket& packet : packets_b) {
        if (packet.source_cts >= selected_range_->start_cts &&
            (!lower_exclusive || packet.source_cts > *lower_exclusive) && packet.source_cts <= source_cts &&
            !buffer_a_.Contains(packet.source_cts)) {
            return Fail(SynchronizedRecordingFailure::RangeMismatch);
        }
    }

    std::vector<EncodedPacket> selected_a;
    std::vector<EncodedPacket> selected_b;
    for (EncodedPacket packet : packets_a) {
        if (packet.source_cts >= selected_range_->start_cts &&
            (!lower_exclusive || packet.source_cts > *lower_exclusive) && packet.source_cts <= source_cts &&
            packet.source_cts >= selected_range_->start_cts) {
            selected_a.push_back(std::move(packet));
        }
    }
    for (EncodedPacket packet : packets_b) {
        if (packet.source_cts >= selected_range_->start_cts &&
            (!lower_exclusive || packet.source_cts > *lower_exclusive) && packet.source_cts <= source_cts &&
            packet.source_cts >= selected_range_->start_cts) {
            selected_b.push_back(std::move(packet));
        }
    }
    selected_a = SortForDecodeOrder(std::move(selected_a));
    selected_b = SortForDecodeOrder(std::move(selected_b));
    for (const EncodedPacket& packet : selected_a) {
        if (!sink_a_->Write(packet)) {
            return Fail(SynchronizedRecordingFailure::SinkWrite);
        }
    }
    for (const EncodedPacket& packet : selected_b) {
        if (!sink_b_->Write(packet)) {
            return Fail(SynchronizedRecordingFailure::SinkWrite);
        }
    }
    if (!sink_a_->CommitThrough(source_cts) || !sink_b_->CommitThrough(source_cts)) {
        return Fail(SynchronizedRecordingFailure::SinkWrite);
    }
    buffer_a_.DiscardBefore(source_cts + 1);
    buffer_b_.DiscardBefore(source_cts + 1);
    last_flushed_cts_ = source_cts;
    has_flushed_cts_ = true;
    return true;
}

bool SynchronizedRecordingSession::FinalizeAt(const uint64_t common_end_cts) {
    if (!sink_a_->Finalize(common_end_cts) || !sink_b_->Finalize(common_end_cts)) {
        sink_a_->Abort();
        sink_b_->Abort();
        return Fail(SynchronizedRecordingFailure::SinkFinalize);
    }
    selected_range_->end_cts = common_end_cts;
    state_ = SynchronizedRecordingState::Stopped;
    return true;
}

bool SynchronizedRecordingSession::Fail(const SynchronizedRecordingFailure failure) noexcept {
    failure_ = failure;
    state_ = SynchronizedRecordingState::Failed;
    if (sink_a_) {
        sink_a_->Abort();
    }
    if (sink_b_) {
        sink_b_->Abort();
    }
    return false;
}

EncodedPacketBuffer& SynchronizedRecordingSession::Buffer(const RecordingStream stream) noexcept {
    return stream == RecordingStream::A ? buffer_a_ : buffer_b_;
}

const EncodedPacketBuffer& SynchronizedRecordingSession::Buffer(const RecordingStream stream) const noexcept {
    return stream == RecordingStream::A ? buffer_a_ : buffer_b_;
}

SynchronizedPacketSink& SynchronizedRecordingSession::Sink(const RecordingStream stream) noexcept {
    return stream == RecordingStream::A ? *sink_a_ : *sink_b_;
}

SynchronizedRecordingState SynchronizedRecordingSession::state() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

SynchronizedRecordingFailure SynchronizedRecordingSession::failure() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return failure_;
}

std::optional<CommonPacketRange> SynchronizedRecordingSession::selected_range() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return selected_range_;
}

SynchronizedRecordingMetrics SynchronizedRecordingSession::metrics() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return {pre_roll_packet_count_a_, pre_roll_packet_count_b_, pre_roll_bytes_a_, pre_roll_bytes_b_,
            buffer_a_.size(), buffer_b_.size(), buffer_a_.bytes(), buffer_b_.bytes(), peak_retained_bytes_,
            selected_range_ ? selected_range_->start_cts : 0, selected_range_ ? selected_range_->end_cts : 0};
}

const char* SynchronizedRecordingStateName(const SynchronizedRecordingState state) noexcept {
    switch (state) {
    case SynchronizedRecordingState::Idle:
        return "idle";
    case SynchronizedRecordingState::Starting:
        return "starting";
    case SynchronizedRecordingState::Running:
        return "running";
    case SynchronizedRecordingState::Draining:
        return "draining";
    case SynchronizedRecordingState::Stopped:
        return "stopped";
    case SynchronizedRecordingState::Failed:
        return "failed";
    }
    return "unknown";
}

const char* SynchronizedRecordingFailureName(const SynchronizedRecordingFailure failure) noexcept {
    switch (failure) {
    case SynchronizedRecordingFailure::None:
        return "none";
    case SynchronizedRecordingFailure::Aborted:
        return "aborted";
    case SynchronizedRecordingFailure::InvalidTransition:
        return "invalid-transition";
    case SynchronizedRecordingFailure::StartTimeout:
        return "start-timeout";
    case SynchronizedRecordingFailure::PacketTiming:
        return "packet-timing";
    case SynchronizedRecordingFailure::DuplicateSourceCts:
        return "duplicate-source-cts";
    case SynchronizedRecordingFailure::BufferCapacity:
        return "buffer-capacity";
    case SynchronizedRecordingFailure::SinkOpen:
        return "sink-open";
    case SynchronizedRecordingFailure::SinkWrite:
        return "sink-write";
    case SynchronizedRecordingFailure::MissingCommonRange:
        return "missing-common-range";
    case SynchronizedRecordingFailure::RangeMismatch:
        return "range-mismatch";
    case SynchronizedRecordingFailure::SinkFinalize:
        return "sink-finalize";
    }
    return "unknown";
}

} // namespace obs_sync_replay
