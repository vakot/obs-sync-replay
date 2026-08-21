#include "muxing/mkv-packet-writer.hpp"

#include "sync/common-packet-range.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include <chrono>
#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>

namespace obs_sync_replay {

namespace {

uint64_t WallClockNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::string AvError(const int error) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, buffer, sizeof(buffer));
    return buffer;
}

} // namespace

MkvPacketWriter::~MkvPacketWriter() {
    Abort();
}

bool MkvPacketWriter::Open(const std::string& path, const MkvStreamConfig& config) {
    error_.clear();
    if (format_ || path.empty() || config.width == 0 || config.height == 0 || config.extra_data.empty()) {
        error_ = "invalid-mkv-writer-configuration";
        return false;
    }

    path_ = path;
    int error = avformat_alloc_output_context2(&format_, nullptr, "matroska", path.c_str());
    if (error < 0 || !format_) {
        error_ = "mkv-context-failed:" + AvError(error);
        format_ = nullptr;
        return false;
    }

    stream_ = avformat_new_stream(format_, nullptr);
    if (!stream_) {
        error_ = "mkv-stream-failed";
        ReleaseFormat();
        return false;
    }
    timebase_num_ = config.timebase_num;
    timebase_den_ = config.timebase_den;
    if (timebase_num_ <= 0 || timebase_den_ <= 0) {
        error_ = "invalid-mkv-timebase";
        ReleaseFormat();
        return false;
    }
    stream_->time_base = AVRational{timebase_num_, timebase_den_};
    stream_->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    stream_->codecpar->codec_id = AV_CODEC_ID_H264;
    stream_->codecpar->width = static_cast<int>(config.width);
    stream_->codecpar->height = static_cast<int>(config.height);
    stream_->codecpar->codec_tag = 0;
    stream_->codecpar->extradata = static_cast<uint8_t*>(
        av_mallocz(config.extra_data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!stream_->codecpar->extradata) {
        error_ = "mkv-extradata-allocation-failed";
        ReleaseFormat();
        return false;
    }
    stream_->codecpar->extradata_size = static_cast<int>(config.extra_data.size());
    std::memcpy(stream_->codecpar->extradata, config.extra_data.data(), config.extra_data.size());

    if (!(format_->oformat->flags & AVFMT_NOFILE)) {
        error = avio_open(&format_->pb, path.c_str(), AVIO_FLAG_WRITE);
        if (error < 0) {
            error_ = "mkv-open-failed:" + AvError(error);
            ReleaseFormat();
            return false;
        }
    }
    error = avformat_write_header(format_, nullptr);
    if (error < 0) {
        error_ = "mkv-header-failed:" + AvError(error);
        ReleaseFormat();
        return false;
    }
    start_time_ns_ = WallClockNs();
    return true;
}

bool MkvPacketWriter::Write(const EncodedPacket& packet) {
    if (!format_ || !stream_ || packet.payload.empty() || packet.timebase_num <= 0 || packet.timebase_den <= 0) {
        return Fail("invalid-mkv-packet");
    }
    if (!has_timestamp_origin_) {
        timestamp_origin_ = std::min(packet.pts, packet.dts);
        has_timestamp_origin_ = true;
    }
    AVPacket* output_packet = av_packet_alloc();
    if (!output_packet || av_new_packet(output_packet, static_cast<int>(packet.payload.size())) < 0) {
        av_packet_free(&output_packet);
        return Fail("mkv-packet-allocation-failed");
    }
    std::memcpy(output_packet->data, packet.payload.data(), packet.payload.size());
    const AVRational source_timebase{packet.timebase_num, packet.timebase_den};
    const AVRational target_timebase = stream_->time_base;
    output_packet->pts = av_rescale_q(packet.pts - timestamp_origin_, source_timebase, target_timebase);
    output_packet->dts = av_rescale_q(packet.dts - timestamp_origin_, source_timebase, target_timebase);
    if (has_last_written_dts_ && output_packet->dts < last_written_dts_) {
        std::ostringstream reason;
        reason << "packet-dts-order-invalid:source_cts=" << packet.source_cts << ":dts=" << packet.dts
               << ":muxed_dts=" << output_packet->dts << ":last_muxed_dts=" << last_written_dts_;
        av_packet_free(&output_packet);
        return Fail(reason.str().c_str());
    }
    output_packet->stream_index = stream_->index;
    if (packet.keyframe) {
        output_packet->flags |= AV_PKT_FLAG_KEY;
    }
    const int64_t muxed_dts = output_packet->dts;
    const int error = av_interleaved_write_frame(format_, output_packet);
    av_packet_free(&output_packet);
    if (error < 0) {
        std::ostringstream reason;
        reason << "mkv-write-failed:" << AvError(error) << ":source_cts=" << packet.source_cts
               << ":pts=" << packet.pts << ":dts=" << packet.dts << ":last_written_dts_target="
               << (has_last_written_dts_ ? std::to_string(last_written_dts_) : "none");
        return Fail(reason.str().c_str());
    }

    if (!has_packet_) {
        first_source_cts_ = packet.source_cts;
        has_packet_ = true;
    } else {
        first_source_cts_ = std::min(first_source_cts_, packet.source_cts);
    }
    last_source_cts_ = std::max(last_source_cts_, packet.source_cts);
    last_written_dts_ = muxed_dts;
    has_last_written_dts_ = true;
    ++packet_count_;
    bytes_ += packet.payload.size();
    return true;
}

MkvWriteResult MkvPacketWriter::Finalize() {
    MkvWriteResult result;
    result.first_source_cts = first_source_cts_;
    result.last_source_cts = last_source_cts_;
    result.packet_count = packet_count_;
    result.bytes = bytes_;
    result.wall_time_ms = start_time_ns_ == 0 ? 0 : (WallClockNs() - start_time_ns_) / 1000000;
    result.error = error_;
    if (!format_) {
        result.success = error_.empty();
        return result;
    }

    const int error = av_write_trailer(format_);
    if (error < 0) {
        error_ = "mkv-trailer-failed:" + AvError(error);
    }
    result.success = error >= 0 && error_.empty();
    result.error = error_;
    ReleaseFormat();
    return result;
}

void MkvPacketWriter::Abort() noexcept {
    ReleaseFormat();
}

bool MkvPacketWriter::is_open() const noexcept {
    return format_ != nullptr;
}

const std::string& MkvPacketWriter::path() const noexcept {
    return path_;
}

const std::string& MkvPacketWriter::error() const noexcept {
    return error_;
}

bool MkvPacketWriter::Fail(const char* reason) noexcept {
    if (error_.empty() && reason) {
        error_ = reason;
    }
    return false;
}

void MkvPacketWriter::ReleaseFormat() noexcept {
    if (!format_) {
        return;
    }
    if (!(format_->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&format_->pb);
    }
    avformat_free_context(format_);
    format_ = nullptr;
    stream_ = nullptr;
}

MkvPacketSink::MkvPacketSink(std::string path) : writer_(), path_(std::move(path)) {}

bool MkvPacketSink::Open(const PacketStreamConfig& config, const uint64_t common_start_cts) {
    error_.clear();
    pending_.clear();
    pending_bytes_ = 0;
    pending_capacity_bytes_ = config.muxer_tail_capacity_bytes;
    muxer_reorder_safety_cts_ = config.muxer_reorder_safety_cts;
    common_start_cts_ = common_start_cts;
    packet_timebase_num_ = 0;
    packet_timebase_den_ = 0;
    has_max_observed_dts_ = false;
    max_observed_dts_ = 0;
    return pending_capacity_bytes_ > 0 && writer_.Open(path_, config);
}

bool MkvPacketSink::Write(const EncodedPacket& packet) {
    if (packet.payload.empty() || packet.payload.size() > pending_capacity_bytes_ -
                                      (pending_bytes_ <= pending_capacity_bytes_ ? pending_bytes_ : pending_capacity_bytes_)) {
        return Fail("mkv-reorder-buffer-capacity");
    }
    if (packet_timebase_num_ == 0) {
        packet_timebase_num_ = packet.timebase_num;
        packet_timebase_den_ = packet.timebase_den;
    } else if (packet.timebase_num != packet_timebase_num_ || packet.timebase_den != packet_timebase_den_) {
        return Fail("mkv-reorder-buffer-timebase-mismatch");
    }
    if (!has_max_observed_dts_ || packet.dts > max_observed_dts_) {
        max_observed_dts_ = packet.dts;
        has_max_observed_dts_ = true;
    }
    pending_bytes_ += packet.payload.size();
    pending_.push_back(packet);
    return true;
}

bool MkvPacketSink::CommitThrough(const uint64_t) {
    if (!has_max_observed_dts_ || packet_timebase_num_ <= 0 || packet_timebase_den_ <= 0) {
        return true;
    }
    const int64_t safety_ticks = av_rescale_q(
        static_cast<int64_t>(std::min<uint64_t>(muxer_reorder_safety_cts_,
                                                static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))),
        AVRational{1, 1'000'000'000}, AVRational{packet_timebase_num_, packet_timebase_den_});
    if (max_observed_dts_ <= std::numeric_limits<int64_t>::min() + safety_ticks) {
        return true;
    }
    // Source CTS selects the shared common prefix. DTS selects the safe packet
    // prefix within that range because callback order is not decode order.
    return FlushPendingThroughDts(max_observed_dts_ - safety_ticks);
}

bool MkvPacketSink::Finalize(const uint64_t common_end_cts) {
    const auto finalization_start = std::chrono::steady_clock::now();
    if (!FlushPendingThroughDts(std::nullopt)) {
        result_.finalization_time_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - finalization_start)
                .count());
        return false;
    }
    result_ = writer_.Finalize();
    result_.finalization_time_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - finalization_start)
            .count());
    return result_.success && result_.first_source_cts == common_start_cts_ &&
           result_.last_source_cts == common_end_cts;
}

void MkvPacketSink::Abort() noexcept {
    pending_.clear();
    pending_bytes_ = 0;
    pending_capacity_bytes_ = 0;
    muxer_reorder_safety_cts_ = 0;
    packet_timebase_num_ = 0;
    packet_timebase_den_ = 0;
    has_max_observed_dts_ = false;
    max_observed_dts_ = 0;
    writer_.Abort();
}

const MkvWriteResult& MkvPacketSink::result() const noexcept {
    return result_;
}

const std::string& MkvPacketSink::error() const noexcept {
    return error_.empty() ? writer_.error() : error_;
}

bool MkvPacketSink::FlushPendingThroughDts(const std::optional<int64_t> dts_watermark) {
    std::vector<EncodedPacket> selected;
    std::vector<EncodedPacket> remaining;
    selected.reserve(pending_.size());
    remaining.reserve(pending_.size());
    for (EncodedPacket packet : std::move(pending_)) {
        if (!dts_watermark || packet.dts <= *dts_watermark) {
            selected.push_back(std::move(packet));
        } else {
            remaining.push_back(std::move(packet));
        }
    }
    pending_ = std::move(remaining);
    pending_bytes_ = 0;
    for (const EncodedPacket& packet : pending_) {
        pending_bytes_ += packet.payload.size();
    }
    selected = SortForDecodeOrder(std::move(selected));
    for (const EncodedPacket& packet : selected) {
        if (!writer_.Write(packet)) {
            return false;
        }
    }
    return true;
}

bool MkvPacketSink::Fail(const char* reason) noexcept {
    if (error_.empty() && reason) {
        error_ = reason;
    }
    return false;
}

} // namespace obs_sync_replay
