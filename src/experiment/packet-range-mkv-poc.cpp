#include "experiment/packet-range-mkv-poc.hpp"

#include <obs.h>
#include <obs-encoder.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace obs_sync_replay {

namespace {

constexpr char kNullOutputId[] = "null_output";
constexpr char kAudioEncoderId[] = "ffmpeg_aac";
constexpr char kX264EncoderId[] = "obs_x264";
constexpr uint32_t kFallbackWidth = 1920;
constexpr uint32_t kFallbackHeight = 1080;
constexpr int32_t kFallbackTimebaseNum = 1;
constexpr int32_t kFallbackTimebaseDen = 60000;

struct CapturedPacket final {
    struct encoder_packet packet{};
    uint64_t source_cts = 0;
};

struct PacketCapture final {
    const char* stream_id = nullptr;
    std::vector<CapturedPacket> packets;
    std::vector<uint8_t> extra_data;
    std::map<uint64_t, size_t> packets_by_source_cts;
    uint64_t missing_timing = 0;
    uint64_t duplicate_source_cts = 0;
    uint64_t retained_bytes = 0;
};

struct MuxResult final {
    bool success = false;
    uint64_t first_source_cts = 0;
    uint64_t last_source_cts = 0;
    uint64_t packet_count = 0;
    uint64_t bytes = 0;
    uint64_t mux_wall_ms = 0;
};

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

void ReleasePackets(PacketCapture* capture) {
    if (!capture) {
        return;
    }
    for (CapturedPacket& captured : capture->packets) {
        obs_encoder_packet_release(&captured.packet);
    }
    capture->packets.clear();
    capture->packets_by_source_cts.clear();
}

void OnPocPacket(obs_output_t*, struct encoder_packet* packet, struct encoder_packet_time* packet_time, void* param) {
    auto* capture = static_cast<PacketCapture*>(param);
    if (!capture || !packet || packet->type != OBS_ENCODER_VIDEO) {
        return;
    }
    if (!packet_time) {
        ++capture->missing_timing;
        blog(LOG_ERROR, "[packet-poc] packet-missing-timing stream=%s", capture->stream_id);
        return;
    }

    CapturedPacket captured;
    captured.source_cts = packet_time->cts;
    obs_encoder_packet_ref(&captured.packet, packet);
    if (capture->extra_data.empty() && packet->encoder) {
        uint8_t* extra_data = nullptr;
        size_t extra_size = 0;
        if (obs_encoder_get_extra_data(packet->encoder, &extra_data, &extra_size) && extra_data && extra_size > 0) {
            capture->extra_data.assign(extra_data, extra_data + extra_size);
            blog(LOG_INFO, "[packet-poc] extradata-captured stream=%s bytes=%llu", capture->stream_id,
                 static_cast<unsigned long long>(extra_size));
        }
    }
    capture->retained_bytes += packet->size;
    const size_t index = capture->packets.size();
    capture->packets.push_back(captured);
    const auto [it, inserted] = capture->packets_by_source_cts.emplace(captured.source_cts, index);
    if (!inserted) {
        ++capture->duplicate_source_cts;
        blog(LOG_ERROR, "[packet-poc] duplicate-source-cts stream=%s cts=%llu", capture->stream_id,
             static_cast<unsigned long long>(it->first));
    }
}

void WaitForOutputInactive(obs_output_t* output) {
    for (uint32_t attempt = 0; output && obs_output_active(output) && attempt < 1000; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void LogOutputFailure(const char* stage, const char* stream_id, obs_output_t* output) {
    blog(LOG_ERROR, "[packet-poc] output-failure stage=%s stream=%s error=%s", stage, stream_id,
         output && obs_output_get_last_error(output) ? obs_output_get_last_error(output) : "none");
}

obs_data_t* CreatePocVideoSettings(const char* encoder_id) {
    obs_data_t* settings = obs_encoder_defaults(encoder_id);
    if (!settings) {
        return nullptr;
    }

    obs_data_set_int(settings, "bitrate", 4000);
    obs_data_set_int(settings, "max_bitrate", 4000);
    obs_data_set_int(settings, "keyint_sec", 1);
    obs_data_set_bool(settings, "repeat_headers", false);
    if (std::string(encoder_id) == kX264EncoderId) {
        obs_data_set_string(settings, "rate_control", "CBR");
        obs_data_set_string(settings, "preset", "ultrafast");
        obs_data_set_string(settings, "profile", "high");
    } else {
        obs_data_set_string(settings, "rate_control", "cbr");
        obs_data_set_string(settings, "preset", "p1");
        obs_data_set_string(settings, "profile", "high");
        obs_data_set_int(settings, "bf", 2);
    }
    return settings;
}

bool FindCommonStart(const PacketCapture& capture_a, const PacketCapture& capture_b, uint64_t requested_start_cts,
                     uint64_t* common_start_cts) {
    if (!common_start_cts) {
        return false;
    }
    for (const auto& [source_cts, index_a] : capture_a.packets_by_source_cts) {
        if (source_cts < requested_start_cts || !capture_a.packets[index_a].packet.keyframe) {
            continue;
        }
        const auto it_b = capture_b.packets_by_source_cts.find(source_cts);
        if (it_b != capture_b.packets_by_source_cts.end() && capture_b.packets[it_b->second].packet.keyframe) {
            *common_start_cts = source_cts;
            return true;
        }
    }
    return false;
}

bool FindCommonEnd(const PacketCapture& capture_a, const PacketCapture& capture_b, uint64_t common_start_cts,
                   uint64_t requested_stop_cts, uint64_t* common_end_cts) {
    if (!common_end_cts) {
        return false;
    }
    uint64_t candidate = 0;
    for (const auto& [source_cts, index_a] : capture_a.packets_by_source_cts) {
        (void)index_a;
        if (source_cts < common_start_cts || source_cts > requested_stop_cts) {
            continue;
        }
        if (capture_b.packets_by_source_cts.find(source_cts) != capture_b.packets_by_source_cts.end()) {
            candidate = source_cts;
        }
    }
    if (candidate == 0 && common_start_cts != 0) {
        return false;
    }
    *common_end_cts = candidate;
    return candidate >= common_start_cts;
}

std::vector<const CapturedPacket*> SelectPackets(const PacketCapture& capture, uint64_t common_start_cts,
                                                  uint64_t common_end_cts) {
    std::vector<const CapturedPacket*> selected;
    for (const auto& [source_cts, index] : capture.packets_by_source_cts) {
        if (source_cts >= common_start_cts && source_cts <= common_end_cts) {
            selected.push_back(&capture.packets[index]);
        }
    }
    std::sort(selected.begin(), selected.end(), [](const CapturedPacket* left, const CapturedPacket* right) {
        if (left->packet.dts != right->packet.dts) {
            return left->packet.dts < right->packet.dts;
        }
        return left->packet.pts < right->packet.pts;
    });
    return selected;
}

uint64_t CountPacketsBefore(const PacketCapture& capture, uint64_t common_start_cts) {
    uint64_t count = 0;
    for (const auto& [source_cts, index] : capture.packets_by_source_cts) {
        (void)index;
        if (source_cts < common_start_cts) {
            ++count;
        }
    }
    return count;
}

uint64_t CountPacketsAfter(const PacketCapture& capture, uint64_t common_end_cts) {
    uint64_t count = 0;
    for (const auto& [source_cts, index] : capture.packets_by_source_cts) {
        (void)index;
        if (source_cts > common_end_cts) {
            ++count;
        }
    }
    return count;
}

uint64_t CountBytesBefore(const PacketCapture& capture, uint64_t common_start_cts) {
    uint64_t bytes = 0;
    for (const auto& [source_cts, index] : capture.packets_by_source_cts) {
        if (source_cts < common_start_cts) {
            bytes += capture.packets[index].packet.size;
        }
    }
    return bytes;
}

uint64_t CountBytesAfter(const PacketCapture& capture, uint64_t common_end_cts) {
    uint64_t bytes = 0;
    for (const auto& [source_cts, index] : capture.packets_by_source_cts) {
        if (source_cts > common_end_cts) {
            bytes += capture.packets[index].packet.size;
        }
    }
    return bytes;
}

bool HasIdenticalSourceCts(const PacketCapture& capture_a, const PacketCapture& capture_b, uint64_t start_cts,
                           uint64_t end_cts, uint64_t* mismatch_count) {
    uint64_t mismatches = 0;
    for (const auto& [source_cts, index_a] : capture_a.packets_by_source_cts) {
        (void)index_a;
        if (source_cts < start_cts || source_cts > end_cts) {
            continue;
        }
        if (capture_b.packets_by_source_cts.find(source_cts) == capture_b.packets_by_source_cts.end()) {
            ++mismatches;
        }
    }
    for (const auto& [source_cts, index_b] : capture_b.packets_by_source_cts) {
        (void)index_b;
        if (source_cts < start_cts || source_cts > end_cts) {
            continue;
        }
        if (capture_a.packets_by_source_cts.find(source_cts) == capture_a.packets_by_source_cts.end()) {
            ++mismatches;
        }
    }
    if (mismatch_count) {
        *mismatch_count = mismatches;
    }
    return mismatches == 0;
}

MuxResult MuxMkv(const PacketCapture& capture, const char* path, uint32_t width, uint32_t height,
                 uint64_t common_start_cts, uint64_t common_end_cts) {
    MuxResult result;
    const std::vector<const CapturedPacket*> selected = SelectPackets(capture, common_start_cts, common_end_cts);
    if (selected.empty()) {
        blog(LOG_ERROR, "[packet-poc] mux-empty stream=%s", capture.stream_id);
        return result;
    }

    int32_t timebase_num = selected.front()->packet.timebase_num;
    int32_t timebase_den = selected.front()->packet.timebase_den;
    if (timebase_num <= 0 || timebase_den <= 0) {
        timebase_num = kFallbackTimebaseNum;
        timebase_den = kFallbackTimebaseDen;
    }

    AVFormatContext* format = nullptr;
    int error = avformat_alloc_output_context2(&format, nullptr, "matroska", path);
    if (error < 0 || !format) {
        blog(LOG_ERROR, "[packet-poc] mux-context-failed stream=%s path=%s error=%s", capture.stream_id, path,
             AvError(error).c_str());
        return result;
    }

    AVStream* stream = avformat_new_stream(format, nullptr);
    if (!stream) {
        blog(LOG_ERROR, "[packet-poc] mux-stream-failed stream=%s", capture.stream_id);
        avformat_free_context(format);
        return result;
    }
    stream->time_base = AVRational{timebase_num, timebase_den};
    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    stream->codecpar->codec_id = AV_CODEC_ID_H264;
    stream->codecpar->width = static_cast<int>(width);
    stream->codecpar->height = static_cast<int>(height);
    stream->codecpar->codec_tag = 0;

    if (capture.extra_data.empty()) {
        blog(LOG_ERROR, "[packet-poc] mux-missing-extradata stream=%s", capture.stream_id);
        avformat_free_context(format);
        return result;
    }
    const size_t extra_size = capture.extra_data.size();
    stream->codecpar->extradata = static_cast<uint8_t*>(av_mallocz(extra_size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!stream->codecpar->extradata) {
        avformat_free_context(format);
        return result;
    }
    stream->codecpar->extradata_size = static_cast<int>(extra_size);
    std::copy_n(capture.extra_data.data(), extra_size, stream->codecpar->extradata);

    if (!(format->oformat->flags & AVFMT_NOFILE)) {
        error = avio_open(&format->pb, path, AVIO_FLAG_WRITE);
        if (error < 0) {
            blog(LOG_ERROR, "[packet-poc] mux-open-failed stream=%s path=%s error=%s", capture.stream_id, path,
                 AvError(error).c_str());
            avformat_free_context(format);
            return result;
        }
    }

    error = avformat_write_header(format, nullptr);
    if (error < 0) {
        blog(LOG_ERROR, "[packet-poc] mux-header-failed stream=%s error=%s", capture.stream_id,
             AvError(error).c_str());
        if (!(format->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&format->pb);
        }
        avformat_free_context(format);
        return result;
    }

    const AVRational source_timebase{timebase_num, timebase_den};
    const int64_t timestamp_origin = selected.front()->packet.pts;
    const uint64_t mux_start_ns = WallClockNs();
    for (const CapturedPacket* captured : selected) {
        AVPacket* packet = av_packet_alloc();
        if (!packet || av_new_packet(packet, static_cast<int>(captured->packet.size)) < 0) {
            av_packet_free(&packet);
            error = AVERROR(ENOMEM);
            break;
        }
        std::copy_n(captured->packet.data, captured->packet.size, packet->data);
        packet->pts = av_rescale_q(captured->packet.pts - timestamp_origin, source_timebase, stream->time_base);
        packet->dts = av_rescale_q(captured->packet.dts - timestamp_origin, source_timebase, stream->time_base);
        packet->stream_index = stream->index;
        if (captured->packet.keyframe) {
            packet->flags |= AV_PKT_FLAG_KEY;
        }
        error = av_interleaved_write_frame(format, packet);
        av_packet_free(&packet);
        if (error < 0) {
            break;
        }
        ++result.packet_count;
        result.bytes += captured->packet.size;
    }

    if (error >= 0) {
        error = av_write_trailer(format);
    }
    if (error < 0) {
        blog(LOG_ERROR, "[packet-poc] mux-write-failed stream=%s error=%s", capture.stream_id, AvError(error).c_str());
    }
    if (!(format->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&format->pb);
    }
    avformat_free_context(format);

    result.success = error >= 0;
    result.first_source_cts = (*std::min_element(
                                  selected.begin(), selected.end(),
                                  [](const CapturedPacket* left, const CapturedPacket* right) {
                                      return left->source_cts < right->source_cts;
                                  }))
                                 ->source_cts;
    result.last_source_cts = (*std::max_element(
                                 selected.begin(), selected.end(),
                                 [](const CapturedPacket* left, const CapturedPacket* right) {
                                     return left->source_cts < right->source_cts;
                                 }))
                                ->source_cts;
    result.mux_wall_ms = (WallClockNs() - mux_start_ns) / 1000000;
    return result;
}

} // namespace

void RunPacketRangeMkvPoc(const char* encoder_id, video_t* video_a, video_t* video_b, uint32_t duration_seconds,
                          uint32_t warmup_milliseconds) {
    if (!encoder_id || !video_a || !video_b || duration_seconds == 0) {
        return;
    }

    const enum obs_module_load_state load_state = obs_encoder_load_state(encoder_id);
    if (load_state != OBS_MODULE_ENABLED) {
        blog(LOG_WARNING, "[packet-poc] skipped encoder=%s reason=stock-module-not-loaded", encoder_id);
        return;
    }

    const uint64_t run_id = WallClockNs();
    const std::filesystem::path output_directory = std::filesystem::current_path() / "stock-packet-range-poc";
    std::error_code directory_error;
    std::filesystem::create_directories(output_directory, directory_error);
    if (directory_error) {
        blog(LOG_ERROR, "[packet-poc] output-directory-failed path=%s error=%s", output_directory.string().c_str(),
             directory_error.message().c_str());
        return;
    }

    const std::string stem = std::string("poc-") + encoder_id + "-" + std::to_string(run_id);
    const std::string path_a = (output_directory / (stem + "-A.mkv")).string();
    const std::string path_b = (output_directory / (stem + "-B.mkv")).string();
    blog(LOG_INFO,
         "[packet-poc] begin encoder=%s duration_seconds=%u warmup_milliseconds=%u topology=two-stock-encoders "
         "encoding_passes=2 raw_readback=false decode_during_recording=false output_directory=%s",
         encoder_id, duration_seconds, warmup_milliseconds, output_directory.string().c_str());

    obs_data_t* settings_a = CreatePocVideoSettings(encoder_id);
    obs_data_t* settings_b = CreatePocVideoSettings(encoder_id);
    obs_data_t* audio_settings_a = obs_encoder_defaults(kAudioEncoderId);
    obs_data_t* audio_settings_b = obs_encoder_defaults(kAudioEncoderId);
    obs_encoder_t* encoder_a = obs_video_encoder_create(encoder_id, "Packet POC Encoder A", settings_a, nullptr);
    obs_encoder_t* encoder_b = obs_video_encoder_create(encoder_id, "Packet POC Encoder B", settings_b, nullptr);
    obs_encoder_t* audio_encoder_a = obs_audio_encoder_create(kAudioEncoderId, "Packet POC Audio A", audio_settings_a,
                                                               0, nullptr);
    obs_encoder_t* audio_encoder_b = obs_audio_encoder_create(kAudioEncoderId, "Packet POC Audio B", audio_settings_b,
                                                               0, nullptr);
    if (settings_a) obs_data_release(settings_a);
    if (settings_b) obs_data_release(settings_b);
    if (audio_settings_a) obs_data_release(audio_settings_a);
    if (audio_settings_b) obs_data_release(audio_settings_b);

    obs_output_t* output_a = obs_output_create(kNullOutputId, "Packet POC Output A", nullptr, nullptr);
    obs_output_t* output_b = obs_output_create(kNullOutputId, "Packet POC Output B", nullptr, nullptr);
    PacketCapture capture_a{"A"};
    PacketCapture capture_b{"B"};
    obs_encoder_group_t* group = nullptr;
    const bool ready = encoder_a && encoder_b && audio_encoder_a && audio_encoder_b && output_a && output_b;
    bool started_a = false;
    bool started_b = false;

    if (ready) {
        obs_encoder_set_video(encoder_a, video_a);
        obs_encoder_set_video(encoder_b, video_b);
        obs_encoder_set_audio(audio_encoder_a, obs_get_audio());
        obs_encoder_set_audio(audio_encoder_b, obs_get_audio());
        obs_output_set_video_encoder(output_a, encoder_a);
        obs_output_set_video_encoder(output_b, encoder_b);
        obs_output_set_audio_encoder(output_a, audio_encoder_a, 0);
        obs_output_set_audio_encoder(output_b, audio_encoder_b, 0);
        obs_output_add_packet_callback(output_a, OnPocPacket, &capture_a);
        obs_output_add_packet_callback(output_b, OnPocPacket, &capture_b);
        group = obs_encoder_group_create();
        if (!group || !obs_encoder_set_group(encoder_a, group) || !obs_encoder_set_group(encoder_b, group)) {
            blog(LOG_ERROR, "[packet-poc] encoder-group-failed encoder=%s", encoder_id);
        } else {
            started_a = obs_output_start(output_a);
            if (started_a) {
                started_b = obs_output_start(output_b);
            }
        }
    }

    if (!started_a) {
        LogOutputFailure("start", "A", output_a);
    }
    if (started_a && !started_b) {
        LogOutputFailure("start", "B", output_b);
    }

    const uint64_t requested_start_cts = obs_get_video_frame_time();
    if (started_a && started_b) {
        std::this_thread::sleep_for(std::chrono::milliseconds(warmup_milliseconds));
        const uint64_t actual_start_cts = obs_get_video_frame_time();
        std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
        const uint64_t requested_stop_cts = obs_get_video_frame_time();
        blog(LOG_INFO, "[packet-poc] requested-range encoder=%s requested_start_cts=%llu actual_start_cts=%llu "
                       "requested_stop_cts=%llu",
             encoder_id, static_cast<unsigned long long>(requested_start_cts),
             static_cast<unsigned long long>(actual_start_cts), static_cast<unsigned long long>(requested_stop_cts));
        obs_output_stop(output_a);
        obs_output_stop(output_b);
        WaitForOutputInactive(output_a);
        WaitForOutputInactive(output_b);

        uint64_t common_start_cts = 0;
        uint64_t common_end_cts = 0;
        const bool common_start = FindCommonStart(capture_a, capture_b, actual_start_cts, &common_start_cts);
        const bool common_end = common_start &&
                                FindCommonEnd(capture_a, capture_b, common_start_cts, requested_stop_cts,
                                               &common_end_cts);
        uint64_t cts_mismatches = 0;
        const bool identical_cts = common_end &&
                                   HasIdenticalSourceCts(capture_a, capture_b, common_start_cts, common_end_cts,
                                                         &cts_mismatches);
        const std::vector<const CapturedPacket*> selected_a =
            common_end ? SelectPackets(capture_a, common_start_cts, common_end_cts) : std::vector<const CapturedPacket*>();
        const std::vector<const CapturedPacket*> selected_b =
            common_end ? SelectPackets(capture_b, common_start_cts, common_end_cts) : std::vector<const CapturedPacket*>();
        const uint32_t width = obs_encoder_get_width(encoder_a) ? obs_encoder_get_width(encoder_a) : kFallbackWidth;
        const uint32_t height = obs_encoder_get_height(encoder_a) ? obs_encoder_get_height(encoder_a) : kFallbackHeight;

        MuxResult mux_a;
        MuxResult mux_b;
        if (common_start && common_end && identical_cts && selected_a.size() == selected_b.size()) {
            mux_a = MuxMkv(capture_a, path_a.c_str(), width, height, common_start_cts, common_end_cts);
            mux_b = MuxMkv(capture_b, path_b.c_str(), width, height, common_start_cts, common_end_cts);
        }

        const uint64_t head_packets_a = CountPacketsBefore(capture_a, common_start_cts);
        const uint64_t head_packets_b = CountPacketsBefore(capture_b, common_start_cts);
        const uint64_t tail_packets_a = CountPacketsAfter(capture_a, common_end_cts);
        const uint64_t tail_packets_b = CountPacketsAfter(capture_b, common_end_cts);
        const uint64_t head_bytes = CountBytesBefore(capture_a, common_start_cts) +
                                    CountBytesBefore(capture_b, common_start_cts);
        const uint64_t tail_bytes = CountBytesAfter(capture_a, common_end_cts) +
                                    CountBytesAfter(capture_b, common_end_cts);
        const uint64_t peak_retained_bytes = capture_a.retained_bytes + capture_b.retained_bytes;
        blog((mux_a.success && mux_b.success && identical_cts) ? LOG_INFO : LOG_ERROR,
             "[packet-poc] result encoder=%s common_start_cts=%llu common_end_cts=%llu "
             "requested_start_cts=%llu requested_stop_cts=%llu cts_mismatches=%llu "
             "packets_discarded_head_a=%llu packets_discarded_head_b=%llu "
             "packets_discarded_tail_a=%llu packets_discarded_tail_b=%llu "
             "first_muxed_source_cts_a=%llu first_muxed_source_cts_b=%llu "
             "last_muxed_source_cts_a=%llu last_muxed_source_cts_b=%llu "
             "muxed_packet_count_a=%llu muxed_packet_count_b=%llu mux_wall_ms_a=%llu mux_wall_ms_b=%llu "
             "decoded_frame_count_equal=pending "
             "duration_equal=pending zero_frame_skew_source_cts=%s mkv_a=%s mkv_b=%s "
             "peak_retained_packet_bytes=%llu pre_roll_bytes=%llu tail_bytes=%llu additional_video_encoders=0 "
             "decode_during_recording=false reencoded=false",
             encoder_id, static_cast<unsigned long long>(common_start_cts), static_cast<unsigned long long>(common_end_cts),
             static_cast<unsigned long long>(actual_start_cts), static_cast<unsigned long long>(requested_stop_cts),
             static_cast<unsigned long long>(cts_mismatches),
             static_cast<unsigned long long>(head_packets_a), static_cast<unsigned long long>(head_packets_b),
             static_cast<unsigned long long>(tail_packets_a), static_cast<unsigned long long>(tail_packets_b),
             static_cast<unsigned long long>(mux_a.first_source_cts), static_cast<unsigned long long>(mux_b.first_source_cts),
             static_cast<unsigned long long>(mux_a.last_source_cts), static_cast<unsigned long long>(mux_b.last_source_cts),
             static_cast<unsigned long long>(mux_a.packet_count), static_cast<unsigned long long>(mux_b.packet_count),
             static_cast<unsigned long long>(mux_a.mux_wall_ms), static_cast<unsigned long long>(mux_b.mux_wall_ms),
             (mux_a.success && mux_b.success && identical_cts) ? "true" : "false", mux_a.success ? path_a.c_str() : "none",
              mux_b.success ? path_b.c_str() : "none", static_cast<unsigned long long>(peak_retained_bytes),
              static_cast<unsigned long long>(head_bytes), static_cast<unsigned long long>(tail_bytes));
    } else if (started_a || started_b) {
        obs_output_stop(output_a);
        obs_output_stop(output_b);
        WaitForOutputInactive(output_a);
        WaitForOutputInactive(output_b);
    }

    if (output_a) obs_output_remove_packet_callback(output_a, OnPocPacket, &capture_a);
    if (output_b) obs_output_remove_packet_callback(output_b, OnPocPacket, &capture_b);
    if (group) obs_encoder_group_destroy(group);
    if (output_a) obs_output_release(output_a);
    if (output_b) obs_output_release(output_b);
    if (encoder_a) obs_encoder_release(encoder_a);
    if (encoder_b) obs_encoder_release(encoder_b);
    if (audio_encoder_a) obs_encoder_release(audio_encoder_a);
    if (audio_encoder_b) obs_encoder_release(audio_encoder_b);
    ReleasePackets(&capture_a);
    ReleasePackets(&capture_b);
}

} // namespace obs_sync_replay
