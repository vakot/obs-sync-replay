#pragma once

#include "recording/encoded-packet-buffer.hpp"
#include "recording/packet-sink.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace obs_sync_replay {

using CaptureStreamId = uint8_t;
using AudioTrackId = uint8_t;

struct CapturedEncodedPacket final {
    CaptureStreamId stream_id = 0;
    EncodedPacket packet;
    std::vector<uint8_t> codec_extra_data;
};

using OwnedCapturedEncodedPacket = std::shared_ptr<const CapturedEncodedPacket>;

class CapturePacketConsumer {
  public:
    virtual ~CapturePacketConsumer() = default;
    // Consumers must enqueue or retain the immutable packet and return quickly.
    virtual void OnPacket(OwnedCapturedEncodedPacket packet) = 0;
};

struct ReplayFrameRange final {
    uint64_t start_cts = 0;
    uint64_t end_cts = 0;
};

struct ReplaySnapshot final {
    ReplayFrameRange range;
    std::vector<std::string> stream_names;
    std::vector<PacketStreamConfig> stream_configs;
    std::vector<std::vector<OwnedCapturedEncodedPacket>> packets;
    std::vector<AudioStreamConfig> audio_streams;
    std::vector<std::vector<OwnedCapturedEncodedPacket>> audio_packets;
};

struct ReplaySnapshotAttempt final {
    std::optional<ReplaySnapshot> snapshot;
    std::string reason;
};

struct SynchronizedCaptureConfig final {
    size_t ring_capacity_bytes = 30 * 1024 * 1024;
    uint64_t expected_source_cts_step = 0;
};

struct SynchronizedCaptureMetrics final {
    size_t stream_count = 0;
    size_t retained_bytes = 0;
    size_t peak_retained_bytes = 0;
    uint64_t evicted_packet_count = 0;
    uint64_t duplicate_packet_count = 0;
    uint64_t rejected_packet_count = 0;
};

class SynchronizedCaptureSession final {
  public:
    explicit SynchronizedCaptureSession(SynchronizedCaptureConfig config = {});
    ~SynchronizedCaptureSession();

    SynchronizedCaptureSession(const SynchronizedCaptureSession&) = delete;
    SynchronizedCaptureSession& operator=(const SynchronizedCaptureSession&) = delete;

    bool RegisterStream(CaptureStreamId stream_id, std::string name, PacketStreamConfig config);
    bool RegisterAudioTrack(AudioTrackId track_id, AudioStreamConfig config);
    bool Subscribe(CapturePacketConsumer* consumer);
    bool Unsubscribe(CapturePacketConsumer* consumer);

    bool Start();
    bool Ingest(CaptureStreamId stream_id, EncodedPacket packet, std::vector<uint8_t> codec_extra_data = {});
    bool IngestAudio(AudioTrackId track_id, EncodedPacket packet, std::vector<uint8_t> codec_extra_data = {});
    void SetRingCapacityBytes(size_t capacity_bytes) noexcept;
    void SetReplayRetentionEnabled(bool enabled) noexcept;
    void Stop() noexcept;

    bool running() const noexcept;
    size_t stream_count() const noexcept;
    std::optional<uint64_t> common_watermark_cts() const noexcept;
    std::optional<ReplaySnapshot> SnapshotCommonRange(uint64_t duration_ns) const;
    std::optional<ReplaySnapshot> SnapshotCommonRange(const std::vector<CaptureStreamId>& stream_ids,
                                                      uint64_t duration_ns) const;
    ReplaySnapshotAttempt SnapshotCommonRangeDetailed(const std::vector<CaptureStreamId>& stream_ids,
                                                      uint64_t duration_ns) const;
    ReplaySnapshotAttempt SnapshotCommonRangeDetailed(uint64_t duration_ns) const;
    SynchronizedCaptureMetrics metrics() const noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace obs_sync_replay
