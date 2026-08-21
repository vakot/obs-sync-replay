#pragma once

#include "recording/encoded-packet-buffer.hpp"
#include "recording/packet-sink.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct AVFormatContext;
struct AVStream;

namespace obs_sync_replay {

using MkvStreamConfig = PacketStreamConfig;

struct MkvWriteResult final {
    bool success = false;
    uint64_t first_source_cts = 0;
    uint64_t last_source_cts = 0;
    uint64_t packet_count = 0;
    uint64_t bytes = 0;
    uint64_t wall_time_ms = 0;
    uint64_t finalization_time_ms = 0;
    std::string error;
};

// Packet-only MKV writer promoted from the Phase 4 proof. It accepts already
// validated compressed packets and never decodes or re-encodes them.
class MkvPacketWriter final {
  public:
    MkvPacketWriter() = default;
    ~MkvPacketWriter();

    MkvPacketWriter(const MkvPacketWriter&) = delete;
    MkvPacketWriter& operator=(const MkvPacketWriter&) = delete;

    bool Open(const std::string& path, const MkvStreamConfig& config);
    bool Write(const EncodedPacket& packet);
    MkvWriteResult Finalize();
    void Abort() noexcept;

    bool is_open() const noexcept;
    const std::string& path() const noexcept;
    const std::string& error() const noexcept;

  private:
    bool Fail(const char* reason) noexcept;
    void ReleaseFormat() noexcept;

    std::string path_;
    AVFormatContext* format_ = nullptr;
    AVStream* stream_ = nullptr;
    int32_t timebase_num_ = 0;
    int32_t timebase_den_ = 0;
    int64_t timestamp_origin_ = 0;
    bool has_timestamp_origin_ = false;
    bool has_last_written_dts_ = false;
    int64_t last_written_dts_ = 0;
    bool has_packet_ = false;
    uint64_t first_source_cts_ = 0;
    uint64_t last_source_cts_ = 0;
    uint64_t packet_count_ = 0;
    uint64_t bytes_ = 0;
    uint64_t start_time_ns_ = 0;
    std::string error_;
};

class MkvPacketSink final : public SynchronizedPacketSink {
  public:
    explicit MkvPacketSink(std::string path);

    bool Open(const PacketStreamConfig& config, uint64_t common_start_cts) override;
    bool Write(const EncodedPacket& packet) override;
    bool CommitThrough(uint64_t source_cts) override;
    bool Finalize(uint64_t common_end_cts) override;
    void Abort() noexcept override;

    const MkvWriteResult& result() const noexcept;
    const std::string& error() const noexcept;

  private:
    bool FlushPendingThroughDts(std::optional<int64_t> dts_watermark);
    bool Fail(const char* reason) noexcept;

    MkvPacketWriter writer_;
    std::string path_;
    MkvWriteResult result_;
    std::vector<EncodedPacket> pending_;
    size_t pending_bytes_ = 0;
    size_t pending_capacity_bytes_ = 0;
    uint64_t muxer_reorder_safety_cts_ = 0;
    uint64_t common_start_cts_ = 0;
    int32_t packet_timebase_num_ = 0;
    int32_t packet_timebase_den_ = 0;
    bool has_max_observed_dts_ = false;
    int64_t max_observed_dts_ = 0;
    std::string error_;
};

} // namespace obs_sync_replay
