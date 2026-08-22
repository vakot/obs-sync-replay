#include "control/plugin-capture-runtime.hpp"

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

uint64_t WallClockNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

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

struct CallbackContext final {
    SynchronizedCaptureSession *capture = nullptr;
    CaptureStreamId stream_id = 0;
    const char *stream_name = nullptr;
    uint64_t packet_count = 0;
    uint64_t first_source_cts = 0;
    uint64_t last_source_cts = 0;
};

void OnOutputPacket(obs_output_t *, struct encoder_packet *packet, struct encoder_packet_time *packet_time,
                    void *param) {
    auto *context = static_cast<CallbackContext *>(param);
    if (!context || !context->capture || !packet || packet->type != OBS_ENCODER_VIDEO) {
        return;
    }
    if (packet->size == 0 || !packet_time || packet->timebase_num <= 0 || packet->timebase_den <= 0) {
        blog(LOG_ERROR, "[plugin-control] packet-rejected stream=%s invariant=source-cts-and-packet-timing-required",
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
    const uint64_t source_cts = copy.source_cts;
    if (!context->capture->Ingest(context->stream_id, std::move(copy), std::move(extra_data))) {
        blog(LOG_ERROR, "[plugin-control] packet-rejected stream=%s invariant=capture-session-accepted-packet-required",
             context->stream_name);
        return;
    }
    ++context->packet_count;
    context->last_source_cts = source_cts;
    if (context->packet_count == 1) {
        context->first_source_cts = source_cts;
        blog(LOG_INFO, "[plugin-control] first-encoded-packet stream=%s source_cts=%llu capture_stream_ready=true",
             context->stream_name, static_cast<unsigned long long>(source_cts));
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
    bool grouped = false;
    bool release_pending = false;
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
    blog(LOG_ERROR, "[plugin-control] output-failure stage=%s stream=%s error=%s", stage, StreamName(stream.id),
         stream.output && obs_output_get_last_error(stream.output) ? obs_output_get_last_error(stream.output) : "none");
}

void WaitForOutputInactive(obs_output_t *output) {
    for (uint32_t attempt = 0; output && obs_output_active(output) && attempt < 1000; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void ReleaseStream(StreamResources &stream) {
    if (stream.video_encoder && stream.grouped) {
        if (!obs_encoder_set_group(stream.video_encoder, nullptr)) {
            blog(LOG_ERROR, "[plugin-control] encoder-group-detach-failed stream=%s", StreamName(stream.id));
            return;
        } else {
            blog(LOG_INFO, "[plugin-control] encoder-group-detached stream=%s", StreamName(stream.id));
        }
        stream.grouped = false;
    }
    if (stream.output && stream.callback.capture) {
        obs_output_remove_packet_callback(stream.output, OnOutputPacket, &stream.callback);
        blog(LOG_INFO,
             "[plugin-control] packet-stream-stopped stream=%s encoded_packets=%llu first_source_cts=%llu "
             "last_source_cts=%llu",
             StreamName(stream.id), static_cast<unsigned long long>(stream.callback.packet_count),
             static_cast<unsigned long long>(stream.callback.first_source_cts),
             static_cast<unsigned long long>(stream.callback.last_source_cts));
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

struct PluginCaptureRuntime::State final {
    obs_view_t *view_a = nullptr;
    obs_view_t *view_b = nullptr;
    video_t *video_a = nullptr;
    video_t *video_b = nullptr;
    obs_source_t *scene_a = nullptr;
    obs_source_t *scene_b = nullptr;
};

class PluginEncoderController final : public EncoderController {
  public:
    PluginEncoderController(const char *encoder_id, obs_encoder_group_t *group, SynchronizedCaptureSession& capture,
                            const std::array<video_t *, 3>& scene_videos, std::array<StreamResources, 3>& resources)
        : encoder_id_(encoder_id), group_(group), capture_(capture), scene_videos_(scene_videos), resources_(resources) {}

    ~PluginEncoderController() override {
        for (StreamResources& resource : resources_) {
            ReleaseStream(resource);
        }
    }

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
        resource.started = false;
        resource.release_pending = true;
        if (std::none_of(resources_.begin(), resources_.end(),
                         [](const StreamResources& candidate) { return candidate.started; })) {
            // OBS only permits detaching a group member after every member has
            // stopped. Defer destruction until this barrier so the group never
            // retains stale encoder references across capture epochs.
            for (StreamResources& candidate : resources_) {
                if (candidate.video_encoder && candidate.grouped) {
                    if (!obs_encoder_set_group(candidate.video_encoder, nullptr)) {
                        blog(LOG_ERROR, "[plugin-control] encoder-group-detach-failed stream=%s",
                             StreamName(candidate.id));
                    } else {
                        candidate.grouped = false;
                        blog(LOG_INFO, "[plugin-control] encoder-group-detached stream=%s",
                             StreamName(candidate.id));
                    }
                }
            }
            for (StreamResources& candidate : resources_) {
                if (candidate.release_pending && !candidate.grouped) {
                    ReleaseStream(candidate);
                    candidate.release_pending = false;
                }
            }
        }
        blog(LOG_INFO, "[plugin-control] encoder-release stream=%s family=%s", stream.name.c_str(), encoder_id_);
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
        if (group_) {
            resource.grouped = obs_encoder_set_group(resource.video_encoder, group_);
            if (!resource.grouped) {
                blog(LOG_ERROR, "[plugin-control] encoder-group-join-failed stream=%s reason=group-already-started",
                     stream.name.c_str());
                ReleaseStream(resource);
                return false;
            }
        }
        resource.id = stream.identity == StreamIdentity::Master
                          ? StreamId::Master
                          : stream.identity == StreamIdentity::SceneA ? StreamId::SceneA : StreamId::SceneB;
        resource.capture_id = stream_id;
        blog(LOG_INFO, "[plugin-control] encoder-create stream=%s family=%s", stream.name.c_str(), encoder_id_);
        return true;
    }
};

SynchronizedCaptureConfig RuntimeCaptureConfig() {
    SynchronizedCaptureConfig config;
    config.ring_capacity_bytes = 30 * 1024 * 1024;
    return config;
}

struct PluginCaptureRuntime::ControlState final {
    ControlState(const char* encoder_id, const std::array<video_t *, 3>& scene_videos)
        : capture(RuntimeCaptureConfig()), scene_videos(scene_videos) {
        configuration.replay_duration_ns = 8'000'000'000ULL;
        configuration.ring_capacity_bytes = 30 * 1024 * 1024;
        configuration.streams = {
            {StreamIdentity::Master, "master", StreamParticipationMode::Both, StreamConfig(nullptr)},
            {StreamIdentity::SceneA, "scene_a", StreamParticipationMode::Both, StreamConfig(nullptr)},
            {StreamIdentity::SceneB, "scene_b", StreamParticipationMode::Both, StreamConfig(nullptr)},
        };
        group = obs_encoder_group_create();
        if (!group) {
            return;
        }
        controller = std::make_unique<PluginEncoderController>(encoder_id, group, capture, scene_videos, resources);
        control = std::make_unique<CaptureControlEngine>(
            configuration, capture, *controller,
            [](const EncoderLifecycleDiagnostic& diagnostic) {
                const char *event = diagnostic.event == EncoderLifecycleEvent::Activated
                                        ? "activate"
                                        : diagnostic.event == EncoderLifecycleEvent::Retained
                                              ? "retain"
                                              : diagnostic.event == EncoderLifecycleEvent::Released ? "release" : "create";
                blog(LOG_INFO, "[plugin-control] encoder-%s stream_id=%u active_encoder_count=%llu", event,
                     static_cast<unsigned int>(diagnostic.stream_id),
                     static_cast<unsigned long long>(diagnostic.active_encoder_count));
            });
    }

    ~ControlState() {
        if (control) {
            control->Shutdown();
            control.reset();
        }
        controller.reset();
        if (group) {
            obs_encoder_group_destroy(group);
            group = nullptr;
        }
    }

    SynchronizedCaptureSession capture;
    CaptureConfiguration configuration;
    std::array<StreamResources, 3> resources{{
        {StreamId::Master, 0, nullptr, nullptr, nullptr, {}, false, false, false},
        {StreamId::SceneA, 0, nullptr, nullptr, nullptr, {}, false, false, false},
        {StreamId::SceneB, 0, nullptr, nullptr, nullptr, {}, false, false, false},
    }};
    obs_encoder_group_t *group = nullptr;
    std::array<video_t *, 3> scene_videos{};
    std::unique_ptr<PluginEncoderController> controller;
    std::unique_ptr<CaptureControlEngine> control;
};

const char* SelectPluginEncoder() {
    const std::string requested = ReadEnvironmentString("OBS_SYNC_REPLAY_PLUGIN_ENCODER", "nvenc");
    if (requested == "x264") {
        return kX264EncoderId;
    }
    if (obs_encoder_load_state(kNvencEncoderId) == OBS_MODULE_ENABLED) {
        return kNvencEncoderId;
    }
    blog(LOG_WARNING, "[plugin-control] encoder-fallback requested=nvenc fallback=obs_x264");
    return kX264EncoderId;
}

PluginCaptureRuntime::PluginCaptureRuntime(std::string scene_a_name, std::string scene_b_name)
    : scene_a_name_(std::move(scene_a_name)), scene_b_name_(std::move(scene_b_name)), state_(std::make_unique<State>()) {}

PluginCaptureRuntime::~PluginCaptureRuntime() {
    Stop();
}

bool PluginCaptureRuntime::Initialize() {
    if (control_state_) {
        blog(LOG_WARNING, "[plugin-control] initialize-ignored reason=already-initialized");
        return false;
    }

    state_->scene_a = obs_get_source_by_name(scene_a_name_.c_str());
    state_->scene_b = obs_get_source_by_name(scene_b_name_.c_str());
    state_->view_a = obs_view_create();
    state_->view_b = obs_view_create();
    if (!state_->scene_a || !state_->scene_b || !state_->view_a || !state_->view_b) {
        blog(LOG_ERROR, "[plugin-control] setup-failed reason=scene-or-view-create");
        Stop();
        return false;
    }
    obs_view_set_source(state_->view_a, 0, state_->scene_a);
    obs_view_set_source(state_->view_b, 0, state_->scene_b);
    state_->video_a = obs_view_add(state_->view_a);
    state_->video_b = obs_view_add(state_->view_b);
    if (!state_->video_a || !state_->video_b) {
        blog(LOG_ERROR, "[plugin-control] setup-failed reason=obs_view_add");
        Stop();
        return false;
    }

    const std::array<video_t *, 3> scene_videos{{obs_get_video(), state_->video_a, state_->video_b}};
    control_state_ = std::make_unique<ControlState>(SelectPluginEncoder(), scene_videos);
    if (!control_state_->group || !control_state_->control || !control_state_->control->Initialize()) {
        blog(LOG_ERROR, "[plugin-control] setup-failed reason=control-engine-initialize");
        Stop();
        return false;
    }

    blog(LOG_INFO, "[plugin-control] initialized idle=true active_encoder_count=0");
    return true;
}

void PluginCaptureRuntime::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (control_state_) {
        control_state_->control->Shutdown();
        control_state_.reset();
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

bool PluginCaptureRuntime::initialized() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return control_state_ && control_state_->control;
}

ControlCommandResult PluginCaptureRuntime::Failed(const char *reason) const {
    return {ControlCommandStatus::Failed, reason};
}

std::vector<std::filesystem::path> PluginCaptureRuntime::OutputPaths(const CaptureConsumer consumer,
                                                                      const char *stem) {
    std::vector<std::filesystem::path> paths;
    if (!control_state_ || !control_state_->control) {
        return paths;
    }
    const std::filesystem::path directory = std::filesystem::current_path() / "obs-sync-replay-output";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        blog(LOG_ERROR, "[plugin-control] output-directory-failed path=%s error=%s", directory.string().c_str(),
             error.message().c_str());
        return paths;
    }
    for (const ConfiguredStream& stream : control_state_->configuration.streams) {
        if (StreamParticipates(stream.mode, consumer)) {
            paths.push_back(directory / (std::string(stem) + "-" + stream.name + ".mkv"));
        }
    }
    return paths;
}

ControlCommandResult PluginCaptureRuntime::StartRecording() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!control_state_ || !control_state_->control) {
        return Failed("runtime-not-initialized");
    }
    const std::string stem = "recording-" + std::to_string(++recording_number_) + "-" + std::to_string(WallClockNs());
    const ControlCommandResult result =
        control_state_->control->StartRecording(OutputPaths(CaptureConsumer::Recording, stem.c_str()));
    if (result.ok() && control_state_->control->recording_state() == RecordingConsumerState::Running) {
        blog(LOG_INFO, "[plugin-control] recording-consumer-attached streams=3 shared_capture_running=true");
    }
    return result;
}

ControlCommandResult PluginCaptureRuntime::ToggleRecording() {
    const RecordingConsumerState state = recording_state();
    if (state == RecordingConsumerState::Off) {
        return StartRecording();
    }
    if (state == RecordingConsumerState::Running) {
        return StopRecording();
    }
    return {ControlCommandStatus::InvalidState, "recording-transition-active"};
}

ControlCommandResult PluginCaptureRuntime::StopRecording() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!control_state_ || !control_state_->control) {
        return Failed("runtime-not-initialized");
    }
    const ControlCommandResult result = control_state_->control->StopRecording();
    if (const auto recording = control_state_->control->recording_result()) {
        blog(recording->success ? LOG_INFO : LOG_ERROR,
             "[plugin-control] recording-finalized success=%s packets_muxed=%llu first_source_cts=%llu "
             "last_source_cts=%llu streams=%llu error=%s",
             recording->success ? "true" : "false", static_cast<unsigned long long>(recording->packet_count),
             static_cast<unsigned long long>(recording->range.start_cts),
             static_cast<unsigned long long>(recording->range.end_cts),
             static_cast<unsigned long long>(recording->streams.size()), recording->error.c_str());
    }
    return result;
}

ControlCommandResult PluginCaptureRuntime::StartReplay() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!control_state_ || !control_state_->control) {
        return Failed("runtime-not-initialized");
    }
    const ControlCommandResult result = control_state_->control->StartReplay();
    if (result.ok() && control_state_->control->replay_state() == ReplayConsumerState::Running) {
        blog(LOG_INFO, "[plugin-control] replay-ring-attached streams=3 shared_capture_running=true retention=true");
    }
    return result;
}

ControlCommandResult PluginCaptureRuntime::ToggleReplay() {
    const ReplayConsumerState state = replay_state();
    if (state == ReplayConsumerState::Off) {
        return StartReplay();
    }
    if (state == ReplayConsumerState::Running) {
        return StopReplay();
    }
    return {ControlCommandStatus::InvalidState, "replay-transition-active"};
}

ControlCommandResult PluginCaptureRuntime::SaveReplay() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!control_state_ || !control_state_->control) {
        return Failed("runtime-not-initialized");
    }
    const std::string stem = "replay-" + std::to_string(++replay_number_) + "-" + std::to_string(WallClockNs());
    const ControlCommandResult result =
        control_state_->control->SaveReplay(OutputPaths(CaptureConsumer::Replay, stem.c_str()));
    if (result.ok()) {
        ++replay_save_generation_;
        blog(LOG_INFO, "[plugin-control] replay-history-snapshot requested=true async_mux=true");
    }
    return result;
}

ControlCommandResult PluginCaptureRuntime::StopReplay() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!control_state_ || !control_state_->control) {
        return Failed("runtime-not-initialized");
    }
    return control_state_->control->StopReplay();
}

void PluginCaptureRuntime::PollReplaySave() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (control_state_ && control_state_->control) {
        control_state_->control->PollReplaySave();
        if (const auto replay = control_state_->control->replay_result();
            replay && replay_result_logged_generation_ < replay_save_generation_) {
            blog(replay->success ? LOG_INFO : LOG_ERROR,
                 "[plugin-control] replay-finalized success=%s payload_bytes=%llu first_source_cts=%llu "
                 "last_source_cts=%llu streams=%llu error=%s",
                 replay->success ? "true" : "false",
                 static_cast<unsigned long long>(replay->snapshot_payload_bytes),
                 static_cast<unsigned long long>(replay->range.start_cts),
                 static_cast<unsigned long long>(replay->range.end_cts),
                 static_cast<unsigned long long>(replay->streams.size()), replay->error.c_str());
            replay_result_logged_generation_ = replay_save_generation_;
        }
    }
}

CaptureInfrastructureState PluginCaptureRuntime::capture_state() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return control_state_ && control_state_->control ? control_state_->control->capture_state()
                                                     : CaptureInfrastructureState::Idle;
}

RecordingConsumerState PluginCaptureRuntime::recording_state() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return control_state_ && control_state_->control ? control_state_->control->recording_state()
                                                      : RecordingConsumerState::Off;
}

ReplayConsumerState PluginCaptureRuntime::replay_state() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return control_state_ && control_state_->control ? control_state_->control->replay_state()
                                                      : ReplayConsumerState::Off;
}

size_t PluginCaptureRuntime::active_encoder_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return control_state_ && control_state_->control ? control_state_->control->active_encoder_count() : 0;
}

} // namespace obs_sync_replay
