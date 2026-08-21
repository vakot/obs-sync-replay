#include "recording/obs-synchronized-recording.hpp"

#include "muxing/mkv-packet-writer.hpp"
#include "recording/synchronized-recording-session.hpp"

#include <obs.h>
#include <obs-encoder.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace obs_sync_replay {

namespace {

constexpr char kNullOutputId[] = "null_output";
constexpr char kAudioEncoderId[] = "ffmpeg_aac";
constexpr char kX264EncoderId[] = "obs_x264";
constexpr uint32_t kFallbackWidth = 1920;
constexpr uint32_t kFallbackHeight = 1080;

uint64_t WallClockNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void WaitForOutputInactive(obs_output_t* output) {
    for (uint32_t attempt = 0; output && obs_output_active(output) && attempt < 1000; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool ShutdownRequested(const std::atomic<bool>* shutdown_requested) {
    return shutdown_requested && shutdown_requested->load(std::memory_order_acquire);
}

void LogOutputFailure(const char* stage, const char* stream_id, obs_output_t* output) {
    blog(LOG_ERROR, "[sync-recording] output-failure stage=%s stream=%s error=%s", stage, stream_id,
         output && obs_output_get_last_error(output) ? obs_output_get_last_error(output) : "none");
}

obs_data_t* CreateVideoSettings(const char* encoder_id) {
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

PacketStreamConfig StreamConfig(obs_encoder_t* encoder) {
    PacketStreamConfig config;
    config.width = obs_encoder_get_width(encoder) ? obs_encoder_get_width(encoder) : kFallbackWidth;
    config.height = obs_encoder_get_height(encoder) ? obs_encoder_get_height(encoder) : kFallbackHeight;
    config.timebase_num = 1;
    config.timebase_den = 60000;
    uint8_t* extra_data = nullptr;
    size_t extra_size = 0;
    if (encoder && obs_encoder_get_extra_data(encoder, &extra_data, &extra_size) && extra_data && extra_size > 0) {
        config.extra_data.assign(extra_data, extra_data + extra_size);
    }
    return config;
}

struct CallbackContext final {
    SynchronizedRecordingSession* session = nullptr;
    RecordingStream stream = RecordingStream::A;
    const char* stream_id = nullptr;
};

void OnRecordingPacket(obs_output_t*, struct encoder_packet* packet, struct encoder_packet_time* packet_time,
                       void* param) {
    auto* context = static_cast<CallbackContext*>(param);
    if (!context || !context->session || !packet || packet->type != OBS_ENCODER_VIDEO) {
        return;
    }
    if (!packet_time || packet->size == 0 || packet->timebase_num <= 0 || packet->timebase_den <= 0) {
        blog(LOG_ERROR,
             "[sync-recording] packet-rejected stream=%s invariant=source-CTS-and-packet-time-required",
             context->stream_id);
        context->session->Abort();
        return;
    }
    if (packet->encoder) {
        uint8_t* extra_data = nullptr;
        size_t extra_size = 0;
        if (obs_encoder_get_extra_data(packet->encoder, &extra_data, &extra_size) && extra_data && extra_size > 0) {
            context->session->SetStreamExtraData(context->stream,
                                                 std::vector<uint8_t>(extra_data, extra_data + extra_size));
        }
    }
    EncodedPacket copy;
    copy.source_cts = packet_time->cts;
    copy.pts = packet->pts;
    copy.dts = packet->dts;
    copy.timebase_num = packet->timebase_num;
    copy.timebase_den = packet->timebase_den;
    copy.keyframe = packet->keyframe;
    copy.payload.assign(packet->data, packet->data + packet->size);
    if (!context->session->SubmitPacket(context->stream, std::move(copy))) {
        blog(LOG_ERROR, "[sync-recording] packet-rejected stream=%s failure=%s", context->stream_id,
             SynchronizedRecordingFailureName(context->session->failure()));
    }
}

void Release(obs_output_t* output_a, obs_output_t* output_b, obs_encoder_t* encoder_a, obs_encoder_t* encoder_b,
             obs_encoder_t* audio_encoder_a, obs_encoder_t* audio_encoder_b, obs_encoder_group_t* group) {
    if (group) {
        obs_encoder_group_destroy(group);
    }
    if (output_a) {
        obs_output_release(output_a);
    }
    if (output_b) {
        obs_output_release(output_b);
    }
    if (encoder_a) {
        obs_encoder_release(encoder_a);
    }
    if (encoder_b) {
        obs_encoder_release(encoder_b);
    }
    if (audio_encoder_a) {
        obs_encoder_release(audio_encoder_a);
    }
    if (audio_encoder_b) {
        obs_encoder_release(audio_encoder_b);
    }
}

} // namespace

void RunSynchronizedRecording(const char* encoder_id, video_t* video_a, video_t* video_b,
                              const uint32_t duration_seconds, const uint32_t warmup_milliseconds,
                              const std::atomic<bool>* shutdown_requested) {
    if (!encoder_id || !video_a || !video_b || duration_seconds == 0) {
        return;
    }
    if (obs_encoder_load_state(encoder_id) != OBS_MODULE_ENABLED) {
        blog(LOG_WARNING, "[sync-recording] skipped encoder=%s reason=stock-module-not-loaded", encoder_id);
        return;
    }

    const std::filesystem::path output_directory = std::filesystem::current_path() / "synchronized-recordings";
    std::error_code directory_error;
    std::filesystem::create_directories(output_directory, directory_error);
    if (directory_error) {
        blog(LOG_ERROR, "[sync-recording] output-directory-failed path=%s error=%s", output_directory.string().c_str(),
             directory_error.message().c_str());
        return;
    }
    const std::string stem = std::string("recording-") + encoder_id + "-" + std::to_string(WallClockNs());
    const std::string path_a = (output_directory / (stem + "-A.mkv")).string();
    const std::string path_b = (output_directory / (stem + "-B.mkv")).string();

    obs_data_t* settings_a = CreateVideoSettings(encoder_id);
    obs_data_t* settings_b = CreateVideoSettings(encoder_id);
    obs_data_t* audio_settings_a = obs_encoder_defaults(kAudioEncoderId);
    obs_data_t* audio_settings_b = obs_encoder_defaults(kAudioEncoderId);
    obs_encoder_t* encoder_a = obs_video_encoder_create(encoder_id, "Synchronized Recording Encoder A", settings_a, nullptr);
    obs_encoder_t* encoder_b = obs_video_encoder_create(encoder_id, "Synchronized Recording Encoder B", settings_b, nullptr);
    obs_encoder_t* audio_encoder_a = obs_audio_encoder_create(kAudioEncoderId, "Synchronized Recording Audio A",
                                                               audio_settings_a, 0, nullptr);
    obs_encoder_t* audio_encoder_b = obs_audio_encoder_create(kAudioEncoderId, "Synchronized Recording Audio B",
                                                               audio_settings_b, 0, nullptr);
    if (settings_a) obs_data_release(settings_a);
    if (settings_b) obs_data_release(settings_b);
    if (audio_settings_a) obs_data_release(audio_settings_a);
    if (audio_settings_b) obs_data_release(audio_settings_b);

    obs_output_t* output_a = obs_output_create(kNullOutputId, "Synchronized Recording Output A", nullptr, nullptr);
    obs_output_t* output_b = obs_output_create(kNullOutputId, "Synchronized Recording Output B", nullptr, nullptr);
    obs_encoder_group_t* group = nullptr;
    const bool ready = encoder_a && encoder_b && audio_encoder_a && audio_encoder_b && output_a && output_b;
    bool started_a = false;
    bool started_b = false;
    std::unique_ptr<SynchronizedRecordingSession> session;
    CallbackContext context_a;
    CallbackContext context_b;
    MkvPacketSink* sink_a_diagnostics = nullptr;
    MkvPacketSink* sink_b_diagnostics = nullptr;

    if (ready) {
        obs_encoder_set_video(encoder_a, video_a);
        obs_encoder_set_video(encoder_b, video_b);
        obs_encoder_set_audio(audio_encoder_a, obs_get_audio());
        obs_encoder_set_audio(audio_encoder_b, obs_get_audio());
        obs_output_set_video_encoder(output_a, encoder_a);
        obs_output_set_video_encoder(output_b, encoder_b);
        obs_output_set_audio_encoder(output_a, audio_encoder_a, 0);
        obs_output_set_audio_encoder(output_b, audio_encoder_b, 0);

        auto sink_a = std::make_unique<MkvPacketSink>(path_a);
        auto sink_b = std::make_unique<MkvPacketSink>(path_b);
        sink_a_diagnostics = sink_a.get();
        sink_b_diagnostics = sink_b.get();
        SynchronizedRecordingConfig config;
        config.pre_roll_capacity_bytes = 2 * 1024 * 1024;
        config.tail_capacity_bytes = 8 * 1024 * 1024;
        config.reorder_safety_cts = 5'000'000'000;
        const uint64_t observed_frame_interval_ns = obs_get_frame_interval_ns();
        config.expected_source_cts_step = observed_frame_interval_ns != 0 ? observed_frame_interval_ns : 16'666'667;
        config.max_start_wait_cts = 2'000'000'000;
        PacketStreamConfig stream_config_a = StreamConfig(encoder_a);
        PacketStreamConfig stream_config_b = StreamConfig(encoder_b);
        stream_config_a.muxer_tail_capacity_bytes = config.tail_capacity_bytes;
        stream_config_b.muxer_tail_capacity_bytes = config.tail_capacity_bytes;
        stream_config_a.muxer_reorder_safety_cts = config.reorder_safety_cts;
        stream_config_b.muxer_reorder_safety_cts = config.reorder_safety_cts;
        session = std::make_unique<SynchronizedRecordingSession>(config, std::move(stream_config_a),
                                                                  std::move(stream_config_b), std::move(sink_a),
                                                                  std::move(sink_b));
        const uint64_t requested_start_cts = obs_get_video_frame_time();
        session->Start(requested_start_cts);
        context_a = {session.get(), RecordingStream::A, "A"};
        context_b = {session.get(), RecordingStream::B, "B"};
        obs_output_add_packet_callback(output_a, OnRecordingPacket, &context_a);
        obs_output_add_packet_callback(output_b, OnRecordingPacket, &context_b);
        group = obs_encoder_group_create();
        if (group && obs_encoder_set_group(encoder_a, group) && obs_encoder_set_group(encoder_b, group)) {
            started_a = obs_output_start(output_a);
            if (started_a) {
                started_b = obs_output_start(output_b);
            }
        }

        if (started_a && started_b) {
            blog(LOG_INFO, "[sync-recording] started encoder=%s outputs=A,B duration_seconds=%u", encoder_id,
                 duration_seconds);
            const auto warmup_deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(warmup_milliseconds);
            while (!ShutdownRequested(shutdown_requested) && std::chrono::steady_clock::now() < warmup_deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            session->PollStart(obs_get_video_frame_time());
            const auto start_time = std::chrono::steady_clock::now();
            while (!ShutdownRequested(shutdown_requested) &&
                   (session->state() == SynchronizedRecordingState::Starting ||
                   std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time).count() <
                       static_cast<int64_t>(duration_seconds))) {
                if (session->state() == SynchronizedRecordingState::Starting &&
                    !session->PollStart(obs_get_video_frame_time())) {
                    break;
                }
                if (session->state() == SynchronizedRecordingState::Failed) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (session->state() == SynchronizedRecordingState::Running) {
                const uint64_t requested_stop_cts = obs_get_video_frame_time();
                session->RequestStop(requested_stop_cts);
            }
        }
    }

    if (!started_a) LogOutputFailure("start", "A", output_a);
    if (started_a && !started_b) LogOutputFailure("start", "B", output_b);
    if (started_a || started_b) {
        obs_output_stop(output_a);
        obs_output_stop(output_b);
        blog(LOG_INFO, "[sync-recording] outputs-stop-requested encoder=%s", encoder_id);
        WaitForOutputInactive(output_a);
        WaitForOutputInactive(output_b);
        blog(LOG_INFO, "[sync-recording] outputs-inactive encoder=%s active_a=%s active_b=%s", encoder_id,
             output_a && obs_output_active(output_a) ? "true" : "false",
             output_b && obs_output_active(output_b) ? "true" : "false");
    }

    if (output_a) obs_output_remove_packet_callback(output_a, OnRecordingPacket, &context_a);
    if (output_b) obs_output_remove_packet_callback(output_b, OnRecordingPacket, &context_b);
    if (session && session->state() == SynchronizedRecordingState::Draining) {
        session->CompleteDrain();
    } else if (session && session->state() != SynchronizedRecordingState::Stopped &&
               session->state() != SynchronizedRecordingState::Failed) {
        session->Abort();
    }
    if (session) {
        const SynchronizedRecordingMetrics metrics = session->metrics();
        const MkvWriteResult& write_a = sink_a_diagnostics->result();
        const MkvWriteResult& write_b = sink_b_diagnostics->result();
        blog(session->state() == SynchronizedRecordingState::Stopped ? LOG_INFO : LOG_ERROR,
             "[sync-recording] result encoder=%s state=%s failure=%s common_start_cts=%llu common_end_cts=%llu "
             "pre_roll_packets_a=%llu pre_roll_packets_b=%llu pre_roll_bytes_a=%llu pre_roll_bytes_b=%llu "
             "tail_packets_a=%llu tail_packets_b=%llu tail_bytes_a=%llu tail_bytes_b=%llu peak_retained_bytes=%llu "
             "peak_tail_bytes_a=%llu peak_tail_bytes_b=%llu committed_watermark_cts=%llu "
             "muxed_packets_a=%llu muxed_packets_b=%llu muxed_bytes_a=%llu muxed_bytes_b=%llu "
             "muxed_first_cts_a=%llu muxed_first_cts_b=%llu muxed_last_cts_a=%llu muxed_last_cts_b=%llu "
             "finalize_ms_a=%llu finalize_ms_b=%llu",
             encoder_id, SynchronizedRecordingStateName(session->state()),
             SynchronizedRecordingFailureName(session->failure()),
             static_cast<unsigned long long>(metrics.common_start_cts),
             static_cast<unsigned long long>(metrics.common_end_cts),
             static_cast<unsigned long long>(metrics.pre_roll_packet_count_a),
             static_cast<unsigned long long>(metrics.pre_roll_packet_count_b),
             static_cast<unsigned long long>(metrics.pre_roll_bytes_a),
             static_cast<unsigned long long>(metrics.pre_roll_bytes_b),
             static_cast<unsigned long long>(metrics.tail_packet_count_a),
             static_cast<unsigned long long>(metrics.tail_packet_count_b),
             static_cast<unsigned long long>(metrics.tail_bytes_a), static_cast<unsigned long long>(metrics.tail_bytes_b),
             static_cast<unsigned long long>(metrics.peak_retained_bytes),
             static_cast<unsigned long long>(metrics.peak_tail_bytes_a),
             static_cast<unsigned long long>(metrics.peak_tail_bytes_b),
             static_cast<unsigned long long>(metrics.committed_watermark_cts),
             static_cast<unsigned long long>(write_a.packet_count),
             static_cast<unsigned long long>(write_b.packet_count),
             static_cast<unsigned long long>(write_a.bytes), static_cast<unsigned long long>(write_b.bytes),
             static_cast<unsigned long long>(write_a.first_source_cts),
             static_cast<unsigned long long>(write_b.first_source_cts),
             static_cast<unsigned long long>(write_a.last_source_cts),
             static_cast<unsigned long long>(write_b.last_source_cts),
             static_cast<unsigned long long>(write_a.finalization_time_ms),
             static_cast<unsigned long long>(write_b.finalization_time_ms));
        if (session->state() != SynchronizedRecordingState::Stopped) {
            blog(LOG_ERROR, "[sync-recording] mux-diagnostics encoder=%s sink_a=%s sink_b=%s", encoder_id,
                 sink_a_diagnostics ? sink_a_diagnostics->error().c_str() : "unavailable",
                 sink_b_diagnostics ? sink_b_diagnostics->error().c_str() : "unavailable");
        }
        blog(LOG_INFO, "[sync-recording] result-logged encoder=%s", encoder_id);
    }

    Release(output_a, output_b, encoder_a, encoder_b, audio_encoder_a, audio_encoder_b, group);
}

} // namespace obs_sync_replay
