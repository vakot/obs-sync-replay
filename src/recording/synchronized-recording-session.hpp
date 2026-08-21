#pragma once

#include "recording/encoded-packet-buffer.hpp"
#include "recording/packet-sink.hpp"
#include "sync/common-packet-range.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace obs_sync_replay {

enum class RecordingStream : uint8_t { A, B };

enum class SynchronizedRecordingState : uint8_t {
    Idle,
    Starting,
    Running,
    Draining,
    Stopped,
    Failed,
};

enum class SynchronizedRecordingFailure : uint8_t {
    None,
    Aborted,
    InvalidTransition,
    StartTimeout,
    PacketTiming,
    DuplicateSourceCts,
    BufferCapacity,
    SinkOpen,
    SinkWrite,
    MissingCommonRange,
    RangeMismatch,
    SinkFinalize,
};

struct SynchronizedRecordingConfig final {
    size_t pre_roll_capacity_bytes = 2 * 1024 * 1024;
    size_t tail_capacity_bytes = 4 * 1024 * 1024;
    uint64_t max_start_wait_cts = 2'000'000'000;
};

struct SynchronizedRecordingMetrics final {
    size_t pre_roll_packet_count_a = 0;
    size_t pre_roll_packet_count_b = 0;
    size_t pre_roll_bytes_a = 0;
    size_t pre_roll_bytes_b = 0;
    size_t tail_packet_count_a = 0;
    size_t tail_packet_count_b = 0;
    size_t tail_bytes_a = 0;
    size_t tail_bytes_b = 0;
    size_t peak_retained_bytes = 0;
    uint64_t common_start_cts = 0;
    uint64_t common_end_cts = 0;
};

class SynchronizedRecordingSession final {
  public:
    SynchronizedRecordingSession(SynchronizedRecordingConfig config,
                                 PacketStreamConfig stream_a,
                                 PacketStreamConfig stream_b,
                                 std::unique_ptr<SynchronizedPacketSink> sink_a,
                                 std::unique_ptr<SynchronizedPacketSink> sink_b);

    bool Start(uint64_t requested_start_cts);
    bool SetStreamExtraData(RecordingStream stream, std::vector<uint8_t> extra_data);
    bool SubmitPacket(RecordingStream stream, EncodedPacket packet);
    bool PollStart(uint64_t current_source_cts);
    bool RequestStop(uint64_t requested_stop_cts);
    bool CompleteDrain();
    void Abort() noexcept;

    SynchronizedRecordingState state() const noexcept;
    SynchronizedRecordingFailure failure() const noexcept;
    std::optional<CommonPacketRange> selected_range() const noexcept;
    SynchronizedRecordingMetrics metrics() const noexcept;

  private:
    bool EstablishCommonStart();
    bool FlushStablePackets();
    bool FlushThrough(uint64_t source_cts);
    bool FinalizeAt(uint64_t common_end_cts);
    bool Fail(SynchronizedRecordingFailure failure) noexcept;
    void AbortUnlocked() noexcept;
    EncodedPacketBuffer& Buffer(RecordingStream stream) noexcept;
    const EncodedPacketBuffer& Buffer(RecordingStream stream) const noexcept;
    SynchronizedPacketSink& Sink(RecordingStream stream) noexcept;

    SynchronizedRecordingConfig config_;
    PacketStreamConfig stream_a_;
    PacketStreamConfig stream_b_;
    std::unique_ptr<SynchronizedPacketSink> sink_a_;
    std::unique_ptr<SynchronizedPacketSink> sink_b_;
    EncodedPacketBuffer buffer_a_;
    EncodedPacketBuffer buffer_b_;
    size_t pre_roll_packet_count_a_ = 0;
    size_t pre_roll_packet_count_b_ = 0;
    size_t pre_roll_bytes_a_ = 0;
    size_t pre_roll_bytes_b_ = 0;
    size_t peak_retained_bytes_ = 0;
    SynchronizedRecordingState state_ = SynchronizedRecordingState::Idle;
    SynchronizedRecordingFailure failure_ = SynchronizedRecordingFailure::None;
    uint64_t requested_start_cts_ = 0;
    uint64_t requested_stop_cts_ = 0;
    uint64_t last_flushed_cts_ = 0;
    bool has_flushed_cts_ = false;
    std::optional<CommonPacketRange> selected_range_;
    mutable std::mutex mutex_;
};

const char* SynchronizedRecordingStateName(SynchronizedRecordingState state) noexcept;
const char* SynchronizedRecordingFailureName(SynchronizedRecordingFailure failure) noexcept;

} // namespace obs_sync_replay
