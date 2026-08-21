#include "experiment/three-stream-capture-poc.hpp"

#include "capture/synchronized-capture-session.hpp"
#include "control/capture-control.hpp"
#include "recording/synchronized-recording-consumer.hpp"
#include "replay/synchronized-replay-consumer.hpp"

#include <obs.h>
#include <obs-encoder.h>
#include <obs-frontend-api.h>
#include <obs-module.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace obs_sync_replay {

namespace {

constexpr char kNullOutputId[] = "null_output";
constexpr char kX264EncoderId[] = "obs_x264";
constexpr char kNvencEncoderId[] = "obs_nvenc_h264_tex";
constexpr char kAudioEncoderId[] = "ffmpeg_aac";
constexpr char kFadeTransitionId[] = "fade_transition";
constexpr uint32_t kDefaultLongRunSeconds = 180;
constexpr uint32_t kDefaultWarmupMilliseconds = 2000;
constexpr uint32_t kDefaultOutputSeconds = 8;
constexpr uint32_t kDefaultRingSeconds = 60;
constexpr uint32_t kDefaultTransitionDurationMilliseconds = 750;
constexpr uint32_t kDefaultTransitionPeriodSeconds = 5;
constexpr uint32_t kFallbackWidth = 1920;
constexpr uint32_t kFallbackHeight = 1080;

enum class StreamId : uint8_t {
    Master,
    SceneA,
    SceneB,
};

const char *StreamName(const StreamId stream) {
    switch (stream) {
    case StreamId::Master:
        return "master";
    case StreamId::SceneA:
        return "scene_a";
    case StreamId::SceneB:
        return "scene_b";
    }
    return "unknown";
}

struct PocConfig final {
    uint32_t long_run_seconds = kDefaultLongRunSeconds;
    uint32_t warmup_milliseconds = kDefaultWarmupMilliseconds;
    uint32_t output_seconds = kDefaultOutputSeconds;
    uint32_t ring_seconds = kDefaultRingSeconds;
    uint32_t test_save_delay_ms = 0;
    uint32_t transition_duration_ms = kDefaultTransitionDurationMilliseconds;
    uint32_t transition_period_seconds = kDefaultTransitionPeriodSeconds;
    std::string sequence = "C";
    std::array<StreamParticipationMode, 3> modes{
        StreamParticipationMode::Both, StreamParticipationMode::Both, StreamParticipationMode::Both};
};

std::string ReadEnvironmentString(const char *name, std::string fallback) {
    char *raw_value = nullptr;
    size_t raw_value_length = 0;
    if (_dupenv_s(&raw_value, &raw_value_length, name) != 0 || !raw_value) {
        return fallback;
    }
    (void)raw_value_length;
    std::string value(raw_value);
    std::free(raw_value);
    return value.empty() ? std::move(fallback) : value;
}

StreamParticipationMode ParseMode(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (value == "disabled") {
        return StreamParticipationMode::Disabled;
    }
    if (value == "recording") {
        return StreamParticipationMode::Recording;
    }
    if (value == "replay") {
        return StreamParticipationMode::Replay;
    }
    if (value != "both") {
        blog(LOG_WARNING, "[phase7] invalid-mode value=%s fallback=both", value.c_str());
    }
    return StreamParticipationMode::Both;
}

std::array<StreamParticipationMode, 3> ReadModes() {
    const std::string raw = ReadEnvironmentString("OBS_SYNC_REPLAY_THREE_STREAM_MODES", "both,both,both");
    std::array<StreamParticipationMode, 3> modes{
        StreamParticipationMode::Both, StreamParticipationMode::Both, StreamParticipationMode::Both};
    size_t start = 0;
    for (size_t index = 0; index < modes.size() && start <= raw.size(); ++index) {
        const size_t end = raw.find(',', start);
        modes[index] = ParseMode(raw.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return modes;
}

uint32_t ReadEnvironmentUint(const char *name, const uint32_t fallback) {
    char *raw_value = nullptr;
    size_t raw_value_length = 0;
    if (_dupenv_s(&raw_value, &raw_value_length, name) != 0 || !raw_value) {
        return fallback;
    }
    (void)raw_value_length;

    const std::string value(raw_value);
    std::free(raw_value);
    if (value.empty()) {
        return fallback;
    }

    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) {
        blog(LOG_WARNING, "[three-stream-poc] invalid-environment-value name=%s value=%s fallback=%u", name,
             value.c_str(), fallback);
        return fallback;
    }
    return static_cast<uint32_t>(parsed);
}

PocConfig ReadConfig() {
    PocConfig config;
    config.long_run_seconds =
        ReadEnvironmentUint("OBS_SYNC_REPLAY_THREE_STREAM_SECONDS", kDefaultLongRunSeconds);
    config.warmup_milliseconds =
        ReadEnvironmentUint("OBS_SYNC_REPLAY_THREE_STREAM_WARMUP_MS", kDefaultWarmupMilliseconds);
    config.output_seconds =
        ReadEnvironmentUint("OBS_SYNC_REPLAY_THREE_STREAM_OUTPUT_SECONDS", kDefaultOutputSeconds);
    config.ring_seconds =
        ReadEnvironmentUint("OBS_SYNC_REPLAY_THREE_STREAM_RING_SECONDS", kDefaultRingSeconds);
    config.test_save_delay_ms = ReadEnvironmentUint("OBS_SYNC_REPLAY_THREE_STREAM_SAVE_DELAY_MS", 0);
    config.transition_duration_ms = ReadEnvironmentUint("OBS_SYNC_REPLAY_THREE_STREAM_TRANSITION_MS",
                                                        kDefaultTransitionDurationMilliseconds);
    config.transition_period_seconds = ReadEnvironmentUint("OBS_SYNC_REPLAY_THREE_STREAM_TRANSITION_PERIOD_SECONDS",
                                                           kDefaultTransitionPeriodSeconds);
    config.sequence = ReadEnvironmentString("OBS_SYNC_REPLAY_THREE_STREAM_SEQUENCE", "C");
    config.modes = ReadModes();
    return config;
}

uint64_t WallClockNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct CallbackContext final {
    SynchronizedCaptureSession *capture = nullptr;
    CaptureStreamId stream_id = 0;
    const char *stream_name = nullptr;
};

void OnOutputPacket(obs_output_t *, struct encoder_packet *packet, struct encoder_packet_time *packet_time,
                    void *param) {
    auto *context = static_cast<CallbackContext *>(param);
    if (!context || !context->capture || !packet || packet->type != OBS_ENCODER_VIDEO) {
        return;
    }
    if (packet->size == 0 || !packet_time || packet->timebase_num <= 0 || packet->timebase_den <= 0) {
        blog(LOG_ERROR, "[three-stream-poc] packet-rejected stream=%s invariant=source-cts-and-packet-timing-required",
             context->stream_name);
        return;
    }
    EncodedPacket copy;
    copy.source_cts = packet_time->cts;
    copy.pts = packet->pts;
    copy.dts = packet->dts;
    copy.timebase_num = packet->timebase_num;
    copy.timebase_den = packet->timebase_den;
    copy.keyframe = packet->keyframe;
    copy.payload.assign(packet->data, packet->data + packet->size);
    std::vector<uint8_t> extra_data;
    if (packet->encoder) {
        uint8_t *extra = nullptr;
        size_t extra_size = 0;
        if (obs_encoder_get_extra_data(packet->encoder, &extra, &extra_size) && extra && extra_size > 0) {
            extra_data.assign(extra, extra + extra_size);
        }
    }
    if (!context->capture->Ingest(context->stream_id, std::move(copy), std::move(extra_data))) {
        blog(LOG_ERROR, "[three-stream-poc] packet-rejected stream=%s invariant=capture-session-accepted-packet-required",
             context->stream_name);
    }
}

struct StreamResources final {
    StreamId id = StreamId::Master;
    CaptureStreamId capture_id = 0;
    obs_encoder_t *video_encoder = nullptr;
    obs_encoder_t *audio_encoder = nullptr;
    obs_output_t *output = nullptr;
    CallbackContext callback;
    bool started = false;
};

obs_data_t *CreateVideoSettings(const char *encoder_id) {
    obs_data_t *settings = obs_encoder_defaults(encoder_id);
    if (!settings) {
        return nullptr;
    }
    obs_data_set_int(settings, "bitrate", 4000);
    obs_data_set_int(settings, "max_bitrate", 4000);
    obs_data_set_int(settings, "keyint_sec", 1);
    obs_data_set_bool(settings, "repeat_headers", false);
    // The POC maps presentation time from the common source CTS. Disable codec
    // reordering so the disposable packet-only mux has one monotonic decode
    // order for both x264 and NVENC; encoder completion is never temporal ID.
    obs_data_set_int(settings, "bf", 0);
    if (std::string(encoder_id) == kX264EncoderId) {
        obs_data_set_string(settings, "rate_control", "CBR");
        obs_data_set_string(settings, "preset", "ultrafast");
        obs_data_set_string(settings, "profile", "high");
    } else {
        obs_data_set_string(settings, "rate_control", "cbr");
        obs_data_set_string(settings, "preset", "p1");
        obs_data_set_string(settings, "profile", "high");
    }
    return settings;
}

void LogOutputFailure(const char *stage, const StreamResources &stream) {
    blog(LOG_ERROR, "[three-stream-poc] output-failure stage=%s stream=%s error=%s", stage, StreamName(stream.id),
         stream.output && obs_output_get_last_error(stream.output) ? obs_output_get_last_error(stream.output) : "none");
}

void WaitForOutputInactive(obs_output_t *output) {
    for (uint32_t attempt = 0; output && obs_output_active(output) && attempt < 1000; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void ReleaseStream(StreamResources &stream) {
    if (stream.output && stream.callback.capture) {
        obs_output_remove_packet_callback(stream.output, OnOutputPacket, &stream.callback);
    }
    if (stream.output) {
        obs_output_release(stream.output);
        stream.output = nullptr;
    }
    if (stream.video_encoder) {
        obs_encoder_release(stream.video_encoder);
        stream.video_encoder = nullptr;
    }
    if (stream.audio_encoder) {
        obs_encoder_release(stream.audio_encoder);
        stream.audio_encoder = nullptr;
    }
}

void SwitchProgramSceneTask(void *param) {
    auto *scene = static_cast<obs_source_t *>(param);
    if (!scene) {
        return;
    }
    obs_frontend_set_current_scene(scene);
    blog(LOG_INFO, "[three-stream-poc] program-transition-request target=%s", obs_source_get_name(scene));
    obs_source_release(scene);
}

bool QueueProgramScene(obs_source_t *scene) {
    obs_source_t *reference = obs_source_get_ref(scene);
    if (!reference) {
        return false;
    }
    obs_queue_task(OBS_TASK_UI, SwitchProgramSceneTask, reference, true);
    return true;
}

void EnsureFadeTransition() {
    obs_source_t *current = obs_frontend_get_current_transition();
    if (current) {
        blog(LOG_INFO, "[three-stream-poc] program-transition-configured type=%s name=%s", obs_source_get_id(current),
             obs_source_get_name(current));
        obs_source_release(current);
        return;
    }

    obs_data_t *settings = obs_data_create();
    obs_source_t *fade = obs_source_create(kFadeTransitionId, "Sync Research Fade", settings, nullptr);
    if (settings) {
        obs_data_release(settings);
    }
    if (!fade) {
        blog(LOG_WARNING, "[three-stream-poc] program-transition-unavailable type=%s", kFadeTransitionId);
        return;
    }
    obs_frontend_set_current_transition(fade);
    blog(LOG_INFO, "[three-stream-poc] program-transition-configured type=%s name=%s", obs_source_get_id(fade),
         obs_source_get_name(fade));
    obs_source_release(fade);
}

PacketStreamConfig StreamConfig(obs_encoder_t *encoder) {
    PacketStreamConfig config;
    config.width = encoder && obs_encoder_get_width(encoder) ? obs_encoder_get_width(encoder) : kFallbackWidth;
    config.height = encoder && obs_encoder_get_height(encoder) ? obs_encoder_get_height(encoder) : kFallbackHeight;
    config.timebase_num = 1;
    config.timebase_den = 60000;
    uint8_t *extra_data = nullptr;
    size_t extra_size = 0;
    if (encoder && obs_encoder_get_extra_data(encoder, &extra_data, &extra_size) && extra_data && extra_size > 0) {
        config.extra_data.assign(extra_data, extra_data + extra_size);
    }
    return config;
}

} // namespace

struct ThreeStreamCapturePoc::State final {
    obs_view_t *view_a = nullptr;
    obs_view_t *view_b = nullptr;
    video_t *video_a = nullptr;
    video_t *video_b = nullptr;
    obs_source_t *scene_a = nullptr;
    obs_source_t *scene_b = nullptr;
};

class PocEncoderController final : public EncoderController {
  public:
    PocEncoderController(const char *encoder_id, obs_encoder_group_t *group, SynchronizedCaptureSession& capture,
                         const std::array<video_t *, 3>& scene_videos, std::array<StreamResources, 3>& resources)
        : encoder_id_(encoder_id), group_(group), capture_(capture), scene_videos_(scene_videos), resources_(resources) {}

    bool EnsureCreated(const CaptureStreamId stream_id, const ConfiguredStream& stream) override {
        if (!Resource(stream.identity).video_encoder && !CreateResource(stream_id, stream)) {
            return false;
        }
        return true;
    }

    bool Activate(const CaptureStreamId stream_id, const ConfiguredStream& stream) override {
        StreamResources& resource = Resource(stream.identity);
        if (resource.started) {
            return true;
        }
        if (!resource.output || resource.capture_id != stream_id) {
            return false;
        }
        resource.started = obs_output_start(resource.output);
        if (!resource.started) {
            LogOutputFailure("start", resource);
            ReleaseStream(resource);
            return false;
        }
        return true;
    }

    void Release(const CaptureStreamId stream_id, const ConfiguredStream& stream) noexcept override {
        StreamResources& resource = Resource(stream.identity);
        (void)stream_id;
        if (resource.output && resource.started) {
            obs_output_stop(resource.output);
            WaitForOutputInactive(resource.output);
        }
        ReleaseStream(resource);
        resource.started = false;
        blog(LOG_INFO, "[phase7] encoder-release stream=%s family=%s", stream.name.c_str(), encoder_id_);
    }

    bool IsActive(const CaptureStreamId stream_id) const noexcept override {
        return std::any_of(resources_.begin(), resources_.end(), [stream_id](const StreamResources& resource) {
            return resource.capture_id == stream_id && resource.started;
        });
    }

    size_t active_count() const noexcept override {
        return static_cast<size_t>(std::count_if(resources_.begin(), resources_.end(),
                                                 [](const StreamResources& resource) { return resource.started; }));
    }

  private:
    StreamResources& Resource(const StreamIdentity identity) noexcept {
        switch (identity) {
        case StreamIdentity::Master:
            return resources_[0];
        case StreamIdentity::SceneA:
            return resources_[1];
        case StreamIdentity::SceneB:
            return resources_[2];
        }
        return resources_[0];
    }

    const char *encoder_id_;
    obs_encoder_group_t *group_;
    SynchronizedCaptureSession& capture_;
    std::array<video_t *, 3> scene_videos_;
    std::array<StreamResources, 3>& resources_;

    bool CreateResource(const CaptureStreamId stream_id, const ConfiguredStream& stream) {
        StreamResources& resource = Resource(stream.identity);
        obs_data_t *settings = CreateVideoSettings(encoder_id_);
        obs_data_t *audio_settings = obs_encoder_defaults(kAudioEncoderId);
        resource.video_encoder = obs_video_encoder_create(encoder_id_, stream.name.c_str(), settings, nullptr);
        resource.audio_encoder = obs_audio_encoder_create(kAudioEncoderId, stream.name.c_str(), audio_settings, 0,
                                                          nullptr);
        resource.output = obs_output_create(kNullOutputId, stream.name.c_str(), nullptr, nullptr);
        if (settings) {
            obs_data_release(settings);
        }
        if (audio_settings) {
            obs_data_release(audio_settings);
        }
        if (!resource.video_encoder || !resource.audio_encoder || !resource.output) {
            ReleaseStream(resource);
            return false;
        }

        video_t *video = nullptr;
        switch (stream.identity) {
        case StreamIdentity::Master:
            video = obs_get_video();
            break;
        case StreamIdentity::SceneA:
            video = scene_videos_[1];
            break;
        case StreamIdentity::SceneB:
            video = scene_videos_[2];
            break;
        }
        obs_encoder_set_video(resource.video_encoder, video);
        obs_encoder_set_audio(resource.audio_encoder, obs_get_audio());
        obs_output_set_video_encoder(resource.output, resource.video_encoder);
        obs_output_set_audio_encoder(resource.output, resource.audio_encoder, 0);
        resource.callback = {&capture_, stream_id, stream.name.c_str()};
        obs_output_add_packet_callback(resource.output, OnOutputPacket, &resource.callback);
        if (group_ && !obs_encoder_set_group(resource.video_encoder, group_)) {
            blog(LOG_WARNING, "[phase7] encoder-group-join-skipped stream=%s reason=group-already-started",
                 stream.name.c_str());
        }
        resource.id = stream.identity == StreamIdentity::Master
                          ? StreamId::Master
                          : stream.identity == StreamIdentity::SceneA ? StreamId::SceneA : StreamId::SceneB;
        resource.capture_id = stream_id;
        blog(LOG_INFO, "[phase7] encoder-create stream=%s family=%s", stream.name.c_str(), encoder_id_);
        return true;
    }
};

ThreeStreamCapturePoc::ThreeStreamCapturePoc(std::string scene_a_name, std::string scene_b_name)
    : scene_a_name_(std::move(scene_a_name)), scene_b_name_(std::move(scene_b_name)), state_(std::make_unique<State>()) {}

ThreeStreamCapturePoc::~ThreeStreamCapturePoc() {
    Stop();
}

bool ThreeStreamCapturePoc::Start() {
    if (worker_.joinable()) {
        blog(LOG_WARNING, "[three-stream-poc] start-ignored reason=already-running");
        return false;
    }

    state_->scene_a = obs_get_source_by_name(scene_a_name_.c_str());
    state_->scene_b = obs_get_source_by_name(scene_b_name_.c_str());
    state_->view_a = obs_view_create();
    state_->view_b = obs_view_create();
    if (!state_->scene_a || !state_->scene_b || !state_->view_a || !state_->view_b) {
        blog(LOG_ERROR, "[three-stream-poc] setup-failed reason=scene-or-view-create");
        Stop();
        return false;
    }
    obs_view_set_source(state_->view_a, 0, state_->scene_a);
    obs_view_set_source(state_->view_b, 0, state_->scene_b);
    state_->video_a = obs_view_add(state_->view_a);
    state_->video_b = obs_view_add(state_->view_b);
    if (!state_->video_a || !state_->video_b) {
        blog(LOG_ERROR, "[three-stream-poc] setup-failed reason=obs_view_add");
        Stop();
        return false;
    }

    stop_requested_ = false;
    worker_ = std::thread(&ThreeStreamCapturePoc::Run, this);
    return true;
}

void ThreeStreamCapturePoc::Stop() {
    stop_requested_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }
    if (!state_) {
        return;
    }
    if (state_->view_a) {
        obs_view_remove(state_->view_a);
        obs_view_destroy(state_->view_a);
        state_->view_a = nullptr;
    }
    if (state_->view_b) {
        obs_view_remove(state_->view_b);
        obs_view_destroy(state_->view_b);
        state_->view_b = nullptr;
    }
    state_->video_a = nullptr;
    state_->video_b = nullptr;
    if (state_->scene_a) {
        obs_source_release(state_->scene_a);
        state_->scene_a = nullptr;
    }
    if (state_->scene_b) {
        obs_source_release(state_->scene_b);
        state_->scene_b = nullptr;
    }
}

void ThreeStreamCapturePoc::Run() {
    const PocConfig config = ReadConfig();
    const obs_video_info *video_info = nullptr;
    obs_video_info observed_video_info{};
    if (obs_get_video_info(&observed_video_info)) {
        video_info = &observed_video_info;
    }
    SynchronizedCaptureConfig capture_config;
    capture_config.ring_capacity_bytes = std::max<size_t>(1 * 1024 * 1024,
                                                          static_cast<size_t>(config.ring_seconds) * 500'000);
    blog(LOG_INFO,
         "[phase7] begin sequence=%s modes=%s,%s,%s duration_seconds=%u ring_capacity_bytes=%llu video=%s",
         config.sequence.c_str(), StreamParticipationModeName(config.modes[0]),
         StreamParticipationModeName(config.modes[1]), StreamParticipationModeName(config.modes[2]),
         config.long_run_seconds, static_cast<unsigned long long>(capture_config.ring_capacity_bytes),
         video_info ? "1920x1080@60" : "unknown");

    for (const char *encoder_id : {kX264EncoderId, kNvencEncoderId}) {
        if (stop_requested_) {
            break;
        }
        if (obs_encoder_load_state(encoder_id) != OBS_MODULE_ENABLED) {
            blog(LOG_WARNING, "[phase7] encoder-skipped id=%s reason=stock-module-not-loaded", encoder_id);
            continue;
        }

        std::array<StreamResources, 3> streams{{
            {StreamId::Master, 0, nullptr, nullptr, nullptr, {}, false},
            {StreamId::SceneA, 0, nullptr, nullptr, nullptr, {}, false},
            {StreamId::SceneB, 0, nullptr, nullptr, nullptr, {}, false},
        }};
        SynchronizedCaptureSession capture(capture_config);
        CaptureConfiguration control_config;
        control_config.replay_duration_ns = static_cast<uint64_t>(config.output_seconds) * 1'000'000'000ULL;
        control_config.ring_capacity_bytes = capture_config.ring_capacity_bytes;
        control_config.streams = {
            {StreamIdentity::Master, "master", config.modes[0], StreamConfig(nullptr)},
            {StreamIdentity::SceneA, "scene_a", config.modes[1], StreamConfig(nullptr)},
            {StreamIdentity::SceneB, "scene_b", config.modes[2], StreamConfig(nullptr)},
        };
        const auto group = obs_encoder_group_create();
        const std::array<video_t *, 3> scene_videos{{obs_get_video(), state_->video_a, state_->video_b}};
        PocEncoderController encoder_controller(encoder_id, group, capture, scene_videos, streams);
        auto diagnostic_sink = [encoder_id](const EncoderLifecycleDiagnostic& diagnostic) {
            const char *event = diagnostic.event == EncoderLifecycleEvent::Activated
                                    ? "activate"
                                    : diagnostic.event == EncoderLifecycleEvent::Retained
                                          ? "retain"
                                          : diagnostic.event == EncoderLifecycleEvent::Released ? "release" : "create";
            blog(LOG_INFO, "[phase7] encoder-%s stream_id=%u active_encoder_count=%llu family=%s", event,
                 static_cast<unsigned int>(diagnostic.stream_id),
                 static_cast<unsigned long long>(diagnostic.active_encoder_count), encoder_id);
        };
        CaptureControlEngine control(control_config, capture, encoder_controller, std::move(diagnostic_sink),
                                     config.test_save_delay_ms);
        bool ready = group && control.Initialize();
        const std::filesystem::path directory = std::filesystem::current_path() / "three-stream-poc";
        std::error_code directory_error;
        std::filesystem::create_directories(directory, directory_error);
        if (!ready || directory_error) {
            blog(LOG_ERROR, "[phase7] setup-failed encoder=%s reason=control-or-output-directory", encoder_id);
            control.Shutdown();
            if (group) {
                obs_encoder_group_destroy(group);
            }
            continue;
        }

        auto PathsFor = [&](const CaptureConsumer consumer, const std::string& stem) {
            std::vector<std::filesystem::path> paths;
            for (const ConfiguredStream& stream : control_config.streams) {
                if (StreamParticipates(stream.mode, consumer)) {
                    paths.push_back(directory / (stem + "-" + stream.name + ".mkv"));
                }
            }
            return paths;
        };
        auto LogCommand = [&](const char *name, const ControlCommandResult& result) {
            blog(result.ok() ? LOG_INFO : LOG_ERROR, "[phase7] command=%s status=%s reason=%s active_encoder_count=%llu",
                 name, ControlCommandStatusName(result.status), result.reason.c_str(),
                 static_cast<unsigned long long>(control.active_encoder_count()));
        };
        uint32_t save_number = 0;
        auto SaveReplay = [&]() {
            const std::string stem = std::string("phase7-") + encoder_id + "-save-" + std::to_string(++save_number) +
                                     "-" + std::to_string(WallClockNs());
            const ControlCommandResult request = control.SaveReplay(PathsFor(CaptureConsumer::Replay, stem));
            LogCommand("save-replay", request);
            if (!request.ok()) {
                return;
            }
            control.WaitForReplaySave();
            if (const auto result = control.replay_result()) {
                blog(result->success ? LOG_INFO : LOG_ERROR,
                     "[phase7] replay-save-result encoder=%s number=%u success=%s range_start_cts=%llu "
                     "range_end_cts=%llu snapshot_payload_bytes=%llu mux_wall_ms=%llu error=%s",
                     encoder_id, save_number, result->success ? "true" : "false",
                     static_cast<unsigned long long>(result->range.start_cts),
                     static_cast<unsigned long long>(result->range.end_cts),
                     static_cast<unsigned long long>(result->snapshot_payload_bytes),
                     static_cast<unsigned long long>(result->wall_time_ms), result->error.c_str());
            }
        };
        auto WaitWithTransitions = [&](const uint32_t milliseconds) {
            const auto start = std::chrono::steady_clock::now();
            uint32_t last_transition_second = 0;
            bool target_b = true;
            while (!stop_requested_) {
                const uint32_t elapsed = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start).count());
                if (elapsed * 1000U >= milliseconds) {
                    break;
                }
                if (elapsed >= config.transition_period_seconds &&
                    elapsed / config.transition_period_seconds > last_transition_second / config.transition_period_seconds) {
                    last_transition_second = elapsed;
                    QueueProgramScene(target_b ? state_->scene_b : state_->scene_a);
                    target_b = !target_b;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        };
        const std::string recording_stem = std::string("phase7-") + encoder_id + "-recording-" +
                                           std::to_string(WallClockNs());
        const auto recording_paths = PathsFor(CaptureConsumer::Recording, recording_stem);
        EnsureFadeTransition();
        obs_frontend_set_transition_duration(static_cast<int>(config.transition_duration_ms));
        QueueProgramScene(state_->scene_a);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        std::string sequence = config.sequence;
        std::transform(sequence.begin(), sequence.end(), sequence.begin(),
                       [](const unsigned char character) { return static_cast<char>(std::toupper(character)); });
        if (sequence == "A") {
            LogCommand("start-replay", control.StartReplay());
            WaitWithTransitions(config.warmup_milliseconds + config.output_seconds * 1000U);
            SaveReplay();
            LogCommand("stop-replay", control.StopReplay());
        } else if (sequence == "B") {
            LogCommand("start-recording", control.StartRecording(recording_paths));
            WaitWithTransitions(config.long_run_seconds * 1000U);
            LogCommand("stop-recording", control.StopRecording());
        } else if (sequence == "D") {
            LogCommand("start-recording", control.StartRecording(recording_paths));
            WaitWithTransitions(config.warmup_milliseconds);
            LogCommand("start-replay", control.StartReplay());
            WaitWithTransitions(config.output_seconds * 1000U);
            LogCommand("stop-replay", control.StopReplay());
            WaitWithTransitions(config.warmup_milliseconds);
            LogCommand("stop-recording", control.StopRecording());
        } else {
            LogCommand("start-replay", control.StartReplay());
            WaitWithTransitions(config.warmup_milliseconds);
            LogCommand("start-recording", control.StartRecording(recording_paths));
            WaitWithTransitions(std::max<uint32_t>(config.output_seconds * 1000U, config.warmup_milliseconds));
            SaveReplay();
            if (sequence != "A") {
                LogCommand("stop-recording", control.StopRecording());
                WaitWithTransitions(config.warmup_milliseconds);
                SaveReplay();
            }
            LogCommand("stop-replay", control.StopReplay());
        }

        const auto recording_result = control.recording_result();
        const SynchronizedCaptureMetrics metrics = capture.metrics();
        blog(LOG_INFO,
             "[phase7] capture-result encoder=%s stream_count=%llu recording_packets=%llu retained_bytes=%llu "
             "peak_retained_bytes=%llu evicted_packets=%llu replay_saves=%u recording_success=%s "
             "recording_error=%s",
             encoder_id, static_cast<unsigned long long>(metrics.stream_count),
             recording_result ? static_cast<unsigned long long>(recording_result->packet_count) : 0ULL,
             static_cast<unsigned long long>(metrics.retained_bytes),
             static_cast<unsigned long long>(metrics.peak_retained_bytes),
             static_cast<unsigned long long>(metrics.evicted_packet_count), save_number,
             recording_result && recording_result->success ? "true" : "false",
             recording_result ? recording_result->error.c_str() : "recording-result-missing");
        control.Shutdown();
        if (group) {
            obs_encoder_group_destroy(group);
        }
        for (StreamResources& stream : streams) {
            ReleaseStream(stream);
        }
    }

    blog(LOG_INFO,
         "[phase7] complete invariant=common-source-cts-across-selected-streams "
         "capture_layer=n-stream-shared-session encoder_ownership=aggregate-consumer-demand");
}

} // namespace obs_sync_replay
