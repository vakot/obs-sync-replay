#include "control/plugin-capture-runtime.hpp"

#include "capture/synchronized-capture-session.hpp"
#include "config/obs-audio-configuration.hpp"
#include "config/obs-replay-configuration.hpp"
#include "control/capture-control.hpp"
#include "control/obs-audio-packet-output.hpp"
#include "plugin/plugin-log.hpp"
#include "recording/synchronized-recording-consumer.hpp"
#include "replay/synchronized-replay-consumer.hpp"
#include "topology/obs-scene-topology.hpp"

#include <obs.h>
#include <obs-encoder.h>
#include <obs-frontend-api.h>
#include <obs-module.h>

#include <algorithm>
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

constexpr char kX264EncoderId[] = "obs_x264";
constexpr char kNvencEncoderId[] = "obs_nvenc_h264_tex";
constexpr uint32_t kFallbackWidth = 1920;
constexpr uint32_t kFallbackHeight = 1080;

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
    AudioTrackId audio_track_id = 0;
    bool shared_audio = false;
    uint64_t audio_duration_cts = 0;
};

void OnOutputPacket(obs_output_t *, struct encoder_packet *packet, struct encoder_packet_time *packet_time,
                    void *param) {
    auto *context = static_cast<CallbackContext *>(param);
    if (!context || !context->capture || !packet || !packet_time ||
        (context->shared_audio ? packet->type != OBS_ENCODER_AUDIO : packet->type != OBS_ENCODER_VIDEO)) {
        return;
    }
    if (packet->size == 0 || !packet_time || packet->timebase_num <= 0 || packet->timebase_den <= 0) {
        OBS_SYNC_REPLAY_LOG(LOG_ERROR, "control", "packet-rejected stream=%s invariant=source-cts-and-packet-timing-required",
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
    copy.kind = packet->type == OBS_ENCODER_AUDIO ? EncodedPacketKind::Audio : EncodedPacketKind::Video;
    copy.audio_track_index = context->audio_track_id;
    copy.duration_cts = context->audio_duration_cts;
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
    const bool accepted = copy.kind == EncodedPacketKind::Audio
                              ? context->capture->IngestAudio(context->audio_track_id, std::move(copy),
                                                              std::move(extra_data))
                              : context->capture->Ingest(context->stream_id, std::move(copy), std::move(extra_data));
    if (!accepted) {
        OBS_SYNC_REPLAY_LOG(LOG_ERROR, "control", "packet-rejected stream=%s invariant=capture-session-accepted-packet-required",
             context->stream_name);
        return;
    }
    ++context->packet_count;
    context->last_source_cts = source_cts;
    if (context->packet_count == 1) {
        context->first_source_cts = source_cts;
        OBS_SYNC_REPLAY_LOG(LOG_INFO, "control", "first-encoded-packet stream=%s source_cts=%llu capture_stream_ready=true",
             context->stream_name, static_cast<unsigned long long>(source_cts));
    }
}

struct StreamResources final {
    StreamIdentity identity = StreamIdentity::Master();
    CaptureStreamId capture_id = 0;
    video_t *video = nullptr;
    obs_encoder_t *video_encoder = nullptr;
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
    OBS_SYNC_REPLAY_LOG(LOG_ERROR, "control", "output-failure stage=%s identity=%s stream_id=%u error=%s", stage,
         StreamIdentityLabel(stream.identity).c_str(),
         static_cast<unsigned int>(stream.capture_id),
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
            OBS_SYNC_REPLAY_LOG(LOG_ERROR, "control", "encoder-group-detach-failed identity=%s",
                 StreamIdentityLabel(stream.identity).c_str());
            return;
        } else {
            OBS_SYNC_REPLAY_LOG(LOG_INFO, "control", "encoder-group-detached identity=%s",
                 StreamIdentityLabel(stream.identity).c_str());
        }
        stream.grouped = false;
    }
    if (stream.output && stream.callback.capture) {
        obs_output_remove_packet_callback(stream.output, OnOutputPacket, &stream.callback);
        OBS_SYNC_REPLAY_LOG(LOG_INFO, "control",
             "packet-stream-stopped stream=%s encoded_packets=%llu first_source_cts=%llu "
             "last_source_cts=%llu",
             StreamIdentityLabel(stream.identity).c_str(), static_cast<unsigned long long>(stream.callback.packet_count),
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
}

std::string FileNameComponent(std::string value) {
    for (char& character : value) {
        const bool invalid = character == '<' || character == '>' || character == ':' || character == '"' ||
                             character == '/' || character == '\\' || character == '|' || character == '?' ||
                             character == '*';
        if (invalid) {
            character = '_';
        }
    }
    return value.empty() ? "stream" : std::move(value);
}

PacketStreamConfig StreamConfig(video_t *video) {
    PacketStreamConfig config;
    const video_output_info *info = video ? video_output_get_info(video) : nullptr;
    config.width = info && info->width ? info->width : kFallbackWidth;
    config.height = info && info->height ? info->height : kFallbackHeight;
    config.timebase_num = 1;
    config.timebase_den = 60000;
    return config;
}

} // namespace

struct PluginCaptureRuntime::State final {
    struct SceneTarget final {
        SceneTopologyEntry topology;
        obs_source_t *source = nullptr;
        obs_view_t *view = nullptr;
        video_t *video = nullptr;
    };

    std::vector<SceneTarget> scenes;
    std::vector<DiscoveredObsScene> pending_scenes;
};

class PluginEncoderController final : public EncoderController {
  public:
    PluginEncoderController(const char *encoder_id, obs_encoder_group_t *group, SynchronizedCaptureSession& capture,
                            std::vector<video_t*> capture_videos, std::vector<StreamResources>& resources,
                            const ObsAudioConfiguration& audio_configuration)
        : encoder_id_(encoder_id), group_(group), capture_(capture), capture_videos_(std::move(capture_videos)),
          resources_(resources), audio_configuration_(audio_configuration) {}

    ~PluginEncoderController() override {
        for (StreamResources& resource : resources_) {
            ReleaseStream(resource);
        }
        StopSharedAudio();
        ReleaseSharedAudio();
    }

    bool EnsureCreated(const CaptureStreamId stream_id, const ConfiguredStream& stream) override {
        if (stream_id >= resources_.size() || !Resource(stream_id).video_encoder && !CreateResource(stream_id, stream)) {
            return false;
        }
        return true;
    }

    bool Activate(const CaptureStreamId stream_id, const ConfiguredStream& stream) override {
        (void)stream;
        if (stream_id >= resources_.size()) {
            return false;
        }
        StreamResources& resource = Resource(stream_id);
        if (resource.started) {
            return true;
        }
        if (!resource.output || resource.capture_id != stream_id) {
            return false;
        }
        if (stream_id == 0) {
            if (!audio_started_ && !StartSharedAudio()) {
                return false;
            }
            resource.started = true;
            OBS_SYNC_REPLAY_LOG(LOG_INFO, "control", "output-started identity=%s via=shared-av-output",
                                StreamIdentityLabel(stream.identity).c_str());
            return true;
        }
        if (!audio_started_ && !StartSharedAudio()) {
            return false;
        }
        const bool can_begin = obs_output_can_begin_data_capture(resource.output, 0);
        OBS_SYNC_REPLAY_LOG(LOG_INFO, "control",
                            "output-start-attempt identity=%s can_begin=%s active=%s video_encoder=%s video_media=%s",
                            StreamIdentityLabel(stream.identity).c_str(), can_begin ? "true" : "false",
                            obs_output_active(resource.output) ? "true" : "false",
                            obs_output_get_video_encoder(resource.output) ? "present" : "missing",
                            resource.video_encoder && obs_encoder_video(resource.video_encoder) ? "present" : "missing");
        resource.started = obs_output_start(resource.output);
        if (!resource.started) {
            LogOutputFailure("start", resource);
            OBS_SYNC_REPLAY_LOG(LOG_ERROR, "control", "output-start-encoder-state identity=%s active=%s error=%s",
                                StreamIdentityLabel(stream.identity).c_str(),
                                resource.video_encoder && obs_encoder_active(resource.video_encoder) ? "true" : "false",
                                resource.video_encoder && obs_encoder_get_last_error(resource.video_encoder)
                                    ? obs_encoder_get_last_error(resource.video_encoder)
                                    : "none");
            if (std::none_of(resources_.begin(), resources_.end(),
                             [](const StreamResources& candidate) { return candidate.started; })) {
                StopSharedAudio();
            }
            ReleaseStream(resource);
            return false;
        }
        return true;
    }

    void Release(const CaptureStreamId stream_id, const ConfiguredStream& stream) noexcept override {
        if (stream_id >= resources_.size()) {
            return;
        }
        StreamResources& resource = Resource(stream_id);
        if (resource.output && resource.started) {
            obs_output_stop(resource.output);
            WaitForOutputInactive(resource.output);
        }
        resource.started = false;
        resource.release_pending = true;
        if (std::none_of(resources_.begin(), resources_.end(),
                         [](const StreamResources& candidate) { return candidate.started; })) {
            StopSharedAudio();
            // OBS only permits detaching a group member after every member has
            // stopped. Defer destruction until this barrier so the group never
            // retains stale encoder references across capture epochs.
            for (StreamResources& candidate : resources_) {
                if (candidate.video_encoder && candidate.grouped) {
                    if (!obs_encoder_set_group(candidate.video_encoder, nullptr)) {
                        OBS_SYNC_REPLAY_LOG(LOG_ERROR, "control", "encoder-group-detach-failed identity=%s",
                             StreamIdentityLabel(candidate.identity).c_str());
                    } else {
                        candidate.grouped = false;
                        OBS_SYNC_REPLAY_LOG(LOG_INFO, "control", "encoder-group-detached identity=%s",
                             StreamIdentityLabel(candidate.identity).c_str());
                    }
                }
            }
            for (StreamResources& candidate : resources_) {
                if (candidate.release_pending && !candidate.grouped) {
                    ReleaseStream(candidate);
                    candidate.release_pending = false;
                }
            }
            ReleaseSharedAudio();
        }
        OBS_SYNC_REPLAY_LOG(LOG_INFO, "control", "encoder-release stream=%s family=%s", stream.name.c_str(), encoder_id_);
    }

    bool IsActive(const CaptureStreamId stream_id) const noexcept override {
        return std::any_of(resources_.begin(), resources_.end(), [stream_id](const StreamResources& resource) {
            return resource.capture_id == stream_id && resource.started;
        });
    }

    size_t active_count() const noexcept override {
        const size_t video_count = static_cast<size_t>(std::count_if(
            resources_.begin(), resources_.end(), [](const StreamResources& resource) { return resource.started; }));
        return video_count + (audio_started_ ? audio_encoders_.size() : 0);
    }

  private:
    StreamResources& Resource(const CaptureStreamId stream_id) noexcept {
        return resources_[stream_id];
    }

    const char *encoder_id_;
    obs_encoder_group_t *group_;
    SynchronizedCaptureSession& capture_;
    std::vector<video_t*> capture_videos_;
    std::vector<StreamResources>& resources_;
    const ObsAudioConfiguration& audio_configuration_;
    obs_output_t* audio_output_ = nullptr;
    std::vector<obs_encoder_t*> audio_encoders_;
    std::vector<CallbackContext> audio_callbacks_;
    bool audio_started_ = false;
    bool master_callback_attached_ = false;

    bool CreateSharedAudio() {
        if (audio_output_ || audio_configuration_.recording_tracks.empty()) {
            return audio_output_ != nullptr;
        }
        audio_output_ = obs_output_create(kPacketOutputId, "obs-sync-replay-audio", nullptr, nullptr);
        if (!audio_output_) {
            OBS_SYNC_REPLAY_LOG(LOG_ERROR, "audio", "output-create-failed type=%s reason=obs-output-create",
                                kPacketOutputId);
            return false;
        }
        audio_encoders_.reserve(audio_configuration_.recording_tracks.size());
        audio_callbacks_.reserve(audio_configuration_.recording_tracks.size());
        for (size_t index = 0; index < audio_configuration_.recording_tracks.size(); ++index) {
            const AudioStreamConfig& config = audio_configuration_.recording_tracks[index];
            obs_data_t* settings = obs_encoder_defaults(config.encoder_id.c_str());
            if (settings) {
                obs_data_set_int(settings, "bitrate", config.bitrate_kbps);
            }
            obs_encoder_t* encoder = obs_audio_encoder_create(config.encoder_id.c_str(),
                                                               ("obs-sync-replay-audio-" + std::to_string(index)).c_str(),
                                                               settings, config.mixer_index, nullptr);
            if (settings) {
                obs_data_release(settings);
            }
            if (!encoder) {
                OBS_SYNC_REPLAY_LOG(LOG_ERROR, "audio",
                                    "encoder-create-failed configured=%s resolved=%s track=%zu mixer=%zu",
                                    config.encoder_id.c_str(), config.encoder_id.c_str(), index,
                                    config.mixer_index);
                ReleaseSharedAudio();
                return false;
            }
            obs_encoder_set_audio(encoder, obs_get_audio());
            obs_output_set_audio_encoder(audio_output_, encoder, index);
            audio_encoders_.push_back(encoder);
            const char* codec = obs_get_encoder_codec(config.encoder_id.c_str());
            const uint64_t samples_per_packet = codec && std::strcmp(codec, "opus") == 0 ? 960 : 1024;
            audio_callbacks_.push_back(CallbackContext{&capture_, 0, "shared-audio", 0, 0, 0,
                                                        static_cast<AudioTrackId>(index), true,
                                                        (1'000'000'000ULL * samples_per_packet) / config.sample_rate});
            obs_output_add_packet_callback(audio_output_, OnOutputPacket, &audio_callbacks_.back());
        }
        return true;
    }

    bool StartSharedAudio() {
        if (audio_started_) {
            return true;
        }
        if (!CreateSharedAudio()) {
            return false;
        }
        audio_started_ = obs_output_start(audio_output_);
        if (!audio_started_) {
            const char* error = obs_output_get_last_error(audio_output_);
            OBS_SYNC_REPLAY_LOG(LOG_ERROR, "audio", "output-start-failed type=%s error=%s",
                                kPacketOutputId, error ? error : "unspecified");
            for (size_t index = 0; index < audio_encoders_.size(); ++index) {
                const char* encoder_error = obs_encoder_get_last_error(audio_encoders_[index]);
                OBS_SYNC_REPLAY_LOG(LOG_ERROR, "audio", "encoder-start-failed track=%zu id=%s error=%s",
                                    index, audio_configuration_.recording_tracks[index].encoder_id.c_str(),
                                    encoder_error ? encoder_error : "unspecified");
            }
            ReleaseSharedAudio();
        }
        return audio_started_;
    }

    void StopSharedAudio() noexcept {
        if (!audio_output_ || !audio_started_) {
            return;
        }
        obs_output_stop(audio_output_);
        WaitForOutputInactive(audio_output_);
        audio_started_ = false;
    }

    void ReleaseSharedAudio() noexcept {
        if (audio_output_) {
            if (master_callback_attached_ && !resources_.empty()) {
                obs_output_remove_packet_callback(audio_output_, OnOutputPacket, &resources_[0].callback);
                obs_output_set_video_encoder(audio_output_, nullptr);
                master_callback_attached_ = false;
            }
            for (size_t index = 0; index < audio_callbacks_.size(); ++index) {
                obs_output_remove_packet_callback(audio_output_, OnOutputPacket, &audio_callbacks_[index]);
            }
            obs_output_release(audio_output_);
            audio_output_ = nullptr;
        }
        for (obs_encoder_t* encoder : audio_encoders_) {
            if (encoder) {
                obs_encoder_release(encoder);
            }
        }
        audio_encoders_.clear();
        audio_callbacks_.clear();
        audio_started_ = false;
    }

    bool CreateResource(const CaptureStreamId stream_id, const ConfiguredStream& stream) {
        if (stream_id >= resources_.size() || stream_id >= capture_videos_.size()) {
            OBS_SYNC_REPLAY_LOG(LOG_ERROR, "capture",
                                "failed stage=stream-prepare identity=%s stream_id=%u reason=resource-index-out-of-range "
                                "resources=%zu capture_videos=%zu invariant=one-resource-per-active-stream",
                                StreamIdentityLabel(stream.identity).c_str(), static_cast<unsigned int>(stream_id),
                                resources_.size(), capture_videos_.size());
            return false;
        }
        StreamResources& resource = Resource(stream_id);
        if (!CreateSharedAudio()) {
            OBS_SYNC_REPLAY_LOG(LOG_ERROR, "audio",
                                "encoder-create-failed reason=prepare-transaction track-count=%zu",
                                audio_configuration_.recording_tracks.size());
            return false;
        }
        obs_data_t *settings = CreateVideoSettings(encoder_id_);
        resource.video_encoder = obs_video_encoder_create(encoder_id_, stream.name.c_str(), settings, nullptr);
        resource.output = obs_output_create(kPacketOutputId, stream.name.c_str(), nullptr, nullptr);
        if (settings) {
            obs_data_release(settings);
        }
        if (!resource.video_encoder || !resource.output) {
            OBS_SYNC_REPLAY_LOG(LOG_ERROR, "control",
                 "encoder-create-failed identity=%s stream_id=%u video=%s audio=%s output=%s "
                 "invariant=all-stream-resources-required",
                 StreamIdentityLabel(stream.identity).c_str(), static_cast<unsigned int>(stream_id),
                 resource.video_encoder ? "created" : "missing", audio_output_ ? "created" : "missing",
                 resource.output ? "created" : "missing");
            ReleaseStream(resource);
            if (std::none_of(resources_.begin(), resources_.end(),
                             [](const StreamResources& candidate) { return candidate.started; })) {
                ReleaseSharedAudio();
            }
            return false;
        }

        resource.video = capture_videos_[stream_id];
        resource.identity = stream.identity;
        obs_encoder_set_video(resource.video_encoder, resource.video);
        obs_output_set_video_encoder(resource.output, resource.video_encoder);
        resource.callback = {&capture_, stream_id, stream.name.c_str()};
        if (stream_id == 0) {
            obs_output_set_video_encoder(audio_output_, resource.video_encoder);
            obs_output_add_packet_callback(audio_output_, OnOutputPacket, &resource.callback);
            master_callback_attached_ = true;
        } else {
            for (size_t index = 0; index < audio_encoders_.size(); ++index) {
                obs_output_set_audio_encoder(resource.output, audio_encoders_[index], index);
            }
            obs_output_add_packet_callback(resource.output, OnOutputPacket, &resource.callback);
        }
        if (group_) {
            resource.grouped = obs_encoder_set_group(resource.video_encoder, group_);
            if (!resource.grouped) {
                OBS_SYNC_REPLAY_LOG(LOG_ERROR, "control", "encoder-group-join-failed stream=%s reason=group-already-started",
                     stream.name.c_str());
                ReleaseStream(resource);
                return false;
            }
        }
        resource.capture_id = stream_id;
        OBS_SYNC_REPLAY_LOG(LOG_INFO, "control", "encoder-create stream=%s family=%s", stream.name.c_str(), encoder_id_);
        return true;
    }
};

SynchronizedCaptureConfig RuntimeCaptureConfig(const ReplayConfiguration& replay_configuration) {
    SynchronizedCaptureConfig config;
    config.ring_capacity_bytes = replay_configuration.memory_budget_bytes;
    return config;
}

struct PluginCaptureRuntime::ControlState final {
    ControlState(const char* encoder_id, std::vector<ConfiguredStream> configured_streams,
                 std::vector<video_t*> capture_videos, const ReplayConfiguration& replay_configuration,
                 ObsAudioConfiguration audio_configuration)
        : capture(RuntimeCaptureConfig(replay_configuration)), capture_videos(std::move(capture_videos)) {
        configuration.replay = replay_configuration;
        configuration.streams = std::move(configured_streams);
        audio = std::move(audio_configuration);
        group = obs_encoder_group_create();
        if (!group) {
            return;
        }
        resources.resize(this->capture_videos.size());
        controller = std::make_unique<PluginEncoderController>(encoder_id, group, capture,
                                                                this->capture_videos, resources, audio);
        control = std::make_unique<CaptureControlEngine>(
            configuration, capture, *controller,
            [](const EncoderLifecycleDiagnostic& diagnostic) {
                const char *event = diagnostic.event == EncoderLifecycleEvent::Activated
                                        ? "activate"
                                        : diagnostic.event == EncoderLifecycleEvent::Retained
                                              ? "retain"
                                              : diagnostic.event == EncoderLifecycleEvent::Released ? "release" : "create";
                OBS_SYNC_REPLAY_LOG(LOG_INFO, "control", "encoder-%s stream_id=%u active_encoder_count=%llu", event,
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
    std::vector<StreamResources> resources;
    obs_encoder_group_t *group = nullptr;
    std::vector<video_t*> capture_videos;
    ObsAudioConfiguration audio;
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
    OBS_SYNC_REPLAY_LOG(LOG_WARNING, "control", "encoder-fallback requested=nvenc fallback=obs_x264");
    return kX264EncoderId;
}

PluginCaptureRuntime::PluginCaptureRuntime()
    : replay_configuration_(ReadObsReplayConfiguration()), state_(std::make_unique<State>()) {}

PluginCaptureRuntime::~PluginCaptureRuntime() {
    Stop();
}

bool PluginCaptureRuntime::Initialize() {
    if (control_state_) {
        OBS_SYNC_REPLAY_LOG(LOG_WARNING, "control", "initialize-ignored reason=already-initialized");
        return false;
    }

    std::vector<DiscoveredObsScene> discovered = DiscoverObsScenes();
    std::vector<DiscoveredScene> metadata;
    metadata.reserve(discovered.size());
    for (const DiscoveredObsScene& scene : discovered) {
        metadata.push_back(scene.scene);
    }
    if (topology_model_.ApplyDiscovery(metadata, false) == TopologyUpdateResult::Unchanged) {
        // The initial model always contains Master, but keep this path explicit
        // so a future persisted topology cannot silently bypass construction.
        OBS_SYNC_REPLAY_LOG(LOG_INFO, "topology", "initial-discovery unchanged");
    }
    state_->pending_scenes.clear();
    if (!InstallSceneTargets(std::move(discovered))) {
        OBS_SYNC_REPLAY_LOG(LOG_ERROR, "control", "setup-failed reason=scene-topology-target-create");
        Stop();
        return false;
    }
    if (!BuildControlState()) {
        OBS_SYNC_REPLAY_LOG(LOG_ERROR, "control", "setup-failed reason=control-engine-initialize");
        Stop();
        return false;
    }

    OBS_SYNC_REPLAY_LOG(LOG_INFO, "config",
         "source=obs-profile replay enabled=%s duration_ns=%llu memory_budget_bytes=%zu "
         "memory_limit_configured=%s status=applied reason=initial-profile-read",
         replay_configuration_.enabled ? "true" : "false",
         static_cast<unsigned long long>(replay_configuration_.target_duration_ns),
         replay_configuration_.memory_budget_bytes, replay_configuration_.memory_limit_configured ? "true" : "false");
    OBS_SYNC_REPLAY_LOG(LOG_INFO, "control", "initialized idle=true active_encoder_count=0");
    LogTopology("initial-discovery");
    return true;
}

bool PluginCaptureRuntime::InstallSceneTargets(std::vector<DiscoveredObsScene> discovered) {
    ResetSceneTargets();
    state_->scenes.reserve(discovered.size());
    for (DiscoveredObsScene& discovered_scene : discovered) {
        const auto& entries = topology_model_.current().streams;
        const auto entry = std::find_if(entries.begin(), entries.end(), [&discovered_scene](const auto& candidate) {
            return candidate.identity.kind == StreamKind::Scene &&
                   candidate.identity.key == discovered_scene.scene.uuid;
        });
        if (entry == entries.end()) {
            continue;
        }
        if (std::any_of(state_->scenes.begin(), state_->scenes.end(), [&entry](const auto& target) {
                return target.topology.identity == entry->identity;
            })) {
            OBS_SYNC_REPLAY_LOG(LOG_WARNING, "topology", "scene-skipped identity=%s reason=duplicate-discovery",
                 entry->identity.key.c_str());
            continue;
        }
        State::SceneTarget target;
        target.topology = *entry;
        target.source = std::exchange(discovered_scene.source, nullptr);
        target.view = obs_view_create();
        if (!target.source || !target.view) {
            if (target.source) {
                obs_source_release(target.source);
            }
            if (target.view) {
                obs_view_destroy(target.view);
            }
            ResetSceneTargets();
            return false;
        }
        obs_view_set_source(target.view, 0, target.source);
        target.video = obs_view_add(target.view);
        if (!target.video) {
            obs_view_remove(target.view);
            obs_view_destroy(target.view);
            obs_source_release(target.source);
            ResetSceneTargets();
            return false;
        }
        state_->scenes.push_back(std::move(target));
    }
    return true;
}

void PluginCaptureRuntime::ResetSceneTargets() noexcept {
    if (!state_) {
        return;
    }
    for (State::SceneTarget& target : state_->scenes) {
        if (target.view) {
            obs_view_remove(target.view);
            obs_view_destroy(target.view);
            target.view = nullptr;
        }
        target.video = nullptr;
        if (target.source) {
            obs_source_release(target.source);
            target.source = nullptr;
        }
    }
    state_->scenes.clear();
}

bool PluginCaptureRuntime::BuildControlState() {
    std::vector<ConfiguredStream> configured_streams;
    std::vector<video_t*> capture_videos;
    const ObsAudioConfiguration audio_configuration = ReadObsAudioConfiguration();
    if (!audio_configuration.valid) {
        OBS_SYNC_REPLAY_LOG(LOG_ERROR, "audio", "configuration-invalid reason=%s",
                            audio_configuration.error.c_str());
        return false;
    }
    for (const SceneTopologyEntry& entry : topology_model_.current().streams) {
        const StreamParticipationMode mode = entry.recording_enabled && entry.replay_enabled
                                                 ? StreamParticipationMode::Both
                                                 : entry.recording_enabled ? StreamParticipationMode::Recording
                                                                           : entry.replay_enabled ? StreamParticipationMode::Replay
                                                                                                  : StreamParticipationMode::Disabled;
        if (entry.identity.kind == StreamKind::Master) {
            PacketStreamConfig config = StreamConfig(obs_get_video());
            config.audio_streams = audio_configuration.recording_tracks;
            configured_streams.push_back({entry.identity, "master", mode, std::move(config)});
            if (mode != StreamParticipationMode::Disabled) {
                capture_videos.push_back(obs_get_video());
            }
            continue;
        }
        const auto scene = std::find_if(state_->scenes.begin(), state_->scenes.end(), [&entry](const auto& target) {
            return target.topology.identity == entry.identity;
        });
        if (scene == state_->scenes.end() || !scene->video) {
            OBS_SYNC_REPLAY_LOG(LOG_ERROR, "topology", "scene-missing identity=%s name=%s reason=target-not-created",
                 entry.identity.key.c_str(), entry.display_name.c_str());
            return false;
        }
        PacketStreamConfig config = StreamConfig(scene->video);
        config.audio_streams = audio_configuration.recording_tracks;
        configured_streams.push_back({entry.identity, entry.display_name, mode, std::move(config)});
        if (mode != StreamParticipationMode::Disabled) {
            capture_videos.push_back(scene->video);
        }
    }
    control_state_ = std::make_unique<ControlState>(SelectPluginEncoder(), std::move(configured_streams),
                                                    std::move(capture_videos), replay_configuration_,
                                                    audio_configuration);
    if (!control_state_->group || !control_state_->control || !control_state_->control->Initialize()) {
        control_state_.reset();
        return false;
    }
    return true;
}

void PluginCaptureRuntime::FinishCaptureEpochIfIdle() {
    if (!control_state_ || !control_state_->control ||
        control_state_->control->recording_state() != RecordingConsumerState::Off ||
        control_state_->control->replay_state() != ReplayConsumerState::Off ||
        !topology_model_.capture_epoch_active()) {
        return;
    }
    const bool had_pending_topology = topology_model_.has_pending();
    const std::optional<SceneTopologySnapshot> applied = topology_model_.EndCaptureEpoch();
    if (!applied) {
        return;
    }
    LogTopology("capture-epoch-end");
    if (!had_pending_topology) {
        return;
    }
    std::vector<DiscoveredObsScene> pending = std::move(state_->pending_scenes);
    state_->pending_scenes.clear();
    control_state_->control->Shutdown();
    control_state_.reset();
    if (!InstallSceneTargets(std::move(pending)) || !BuildControlState()) {
        OBS_SYNC_REPLAY_LOG(LOG_ERROR, "topology", "pending-apply-failed invariant=active-epoch-ended");
    } else {
        LogTopology("pending-applied");
    }
}

void PluginCaptureRuntime::LogTopology(const char* event) const {
    const SceneTopologySnapshot& topology = topology_model_.capture_epoch_active() ? topology_model_.active_epoch()
                                                                                     : topology_model_.current();
    const size_t top_level_scene_count = topology.streams.empty() ? 0 : topology.streams.size() - 1;
    OBS_SYNC_REPLAY_LOG(LOG_INFO, "topology",
         "event=%s generation=%llu top_level_scene_count=%zu stream_count=%zu epoch_active=%s pending=%s",
         event, static_cast<unsigned long long>(topology.generation), top_level_scene_count, topology.streams.size(),
         topology_model_.capture_epoch_active() ? "true" : "false", topology_model_.has_pending() ? "true" : "false");
    for (const SceneTopologyEntry& entry : topology.streams) {
        OBS_SYNC_REPLAY_LOG(LOG_INFO, "topology", "stream kind=%s identity=%s name=%s order=%zu recording=%s replay=%s",
             StreamKindName(entry.identity.kind), entry.identity.key.c_str(), entry.display_name.c_str(),
             entry.collection_order, entry.recording_enabled ? "true" : "false", entry.replay_enabled ? "true" : "false");
    }
}

void PluginCaptureRuntime::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (control_state_) {
        control_state_->control->Shutdown();
        control_state_.reset();
    }
    ResetSceneTargets();
    state_->pending_scenes.clear();
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
        OBS_SYNC_REPLAY_LOG(LOG_ERROR, "control", "output-directory-failed path=%s error=%s", directory.string().c_str(),
             error.message().c_str());
        return paths;
    }
    for (const ConfiguredStream& stream : control_state_->configuration.streams) {
        if (StreamParticipates(stream.mode, consumer)) {
            paths.push_back(directory / (std::string(stem) + "-" + FileNameComponent(stream.name) + ".mkv"));
        }
    }
    return paths;
}

ControlCommandResult PluginCaptureRuntime::StartRecording() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!control_state_ || !control_state_->control) {
        return Failed("runtime-not-initialized");
    }
    const bool epoch_was_active = topology_model_.capture_epoch_active();
    topology_model_.BeginCaptureEpoch();
    if (!epoch_was_active) {
        LogTopology("capture-epoch-begin");
    }
    const std::string stem = "recording-" + std::to_string(++recording_number_) + "-" + std::to_string(WallClockNs());
    const ControlCommandResult result =
        control_state_->control->StartRecording(OutputPaths(CaptureConsumer::Recording, stem.c_str()));
    if (result.ok() && control_state_->control->recording_state() == RecordingConsumerState::Running) {
        OBS_SYNC_REPLAY_LOG(LOG_INFO, "control", "recording-consumer-attached streams=%zu shared_capture_running=true",
                            control_state_->capture.stream_count());
    }
    if (!result.ok() && control_state_->control->replay_state() == ReplayConsumerState::Off) {
        (void)topology_model_.EndCaptureEpoch();
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
        OBS_SYNC_REPLAY_LOG(recording->success ? LOG_INFO : LOG_ERROR, "control",
             "recording-finalized success=%s packets_muxed=%llu first_source_cts=%llu "
             "last_source_cts=%llu streams=%llu error=%s",
             recording->success ? "true" : "false", static_cast<unsigned long long>(recording->packet_count),
             static_cast<unsigned long long>(recording->range.start_cts),
             static_cast<unsigned long long>(recording->range.end_cts),
             static_cast<unsigned long long>(recording->streams.size()), recording->error.c_str());
    }
    FinishCaptureEpochIfIdle();
    return result;
}

ControlCommandResult PluginCaptureRuntime::StartReplay() {
    (void)RefreshReplayConfiguration();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!control_state_ || !control_state_->control) {
        return Failed("runtime-not-initialized");
    }
    const bool epoch_was_active = topology_model_.capture_epoch_active();
    topology_model_.BeginCaptureEpoch();
    if (!epoch_was_active) {
        LogTopology("capture-epoch-begin");
    }
    const ControlCommandResult result = control_state_->control->StartReplay();
    if (result.ok() && control_state_->control->replay_state() == ReplayConsumerState::Running) {
        OBS_SYNC_REPLAY_LOG(LOG_INFO, "control", "replay-ring-attached streams=%zu shared_capture_running=true retention=true",
                            control_state_->capture.stream_count());
    }
    if (!result.ok() && control_state_->control->recording_state() == RecordingConsumerState::Off) {
        (void)topology_model_.EndCaptureEpoch();
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
    (void)RefreshReplayConfiguration();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!control_state_ || !control_state_->control) {
        return Failed("runtime-not-initialized");
    }
    const std::string stem = "replay-" + std::to_string(++replay_number_) + "-" + std::to_string(WallClockNs());
    const ControlCommandResult result =
        control_state_->control->SaveReplay(OutputPaths(CaptureConsumer::Replay, stem.c_str()));
    if (result.ok()) {
        ++replay_save_generation_;
        OBS_SYNC_REPLAY_LOG(LOG_INFO, "control", "replay-history-snapshot requested=true async_mux=true");
    }
    return result;
}

ControlCommandResult PluginCaptureRuntime::StopReplay() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!control_state_ || !control_state_->control) {
        return Failed("runtime-not-initialized");
    }
    const ControlCommandResult result = control_state_->control->StopReplay();
    FinishCaptureEpochIfIdle();
    return result;
}

ControlCommandResult PluginCaptureRuntime::ApplyReplayConfiguration(ReplayConfiguration configuration) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (replay_configuration_.enabled == configuration.enabled &&
        replay_configuration_.target_duration_ns == configuration.target_duration_ns &&
        replay_configuration_.memory_budget_bytes == configuration.memory_budget_bytes &&
        replay_configuration_.memory_limit_configured == configuration.memory_limit_configured) {
        return {ControlCommandStatus::NoOp, "replay-config-unchanged"};
    }
    replay_configuration_ = configuration;
    if (!control_state_ || !control_state_->control) {
        return {ControlCommandStatus::Succeeded, "replay-config-stored"};
    }
    const ControlCommandResult result = control_state_->control->ApplyReplayConfiguration(configuration);
    OBS_SYNC_REPLAY_LOG(result.ok() ? LOG_INFO : LOG_ERROR, "config",
         "replay enabled=%s duration_ns=%llu memory_budget_bytes=%zu memory_limit_configured=%s "
         "status=%s reason=%s",
         configuration.enabled ? "true" : "false",
         static_cast<unsigned long long>(configuration.target_duration_ns), configuration.memory_budget_bytes,
         configuration.memory_limit_configured ? "true" : "false", ControlCommandStatusName(result.status),
         result.reason.c_str());
    FinishCaptureEpochIfIdle();
    return result;
}

ControlCommandResult PluginCaptureRuntime::RefreshReplayConfiguration() {
    return ApplyReplayConfiguration(ReadObsReplayConfiguration());
}

ControlCommandResult PluginCaptureRuntime::RefreshSceneTopology() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!control_state_ || !control_state_->control) {
        return Failed("runtime-not-initialized");
    }
    std::vector<DiscoveredObsScene> discovered = DiscoverObsScenes();
    std::vector<DiscoveredScene> metadata;
    metadata.reserve(discovered.size());
    for (const DiscoveredObsScene& scene : discovered) {
        metadata.push_back(scene.scene);
    }
    const bool epoch_active = topology_model_.capture_epoch_active();
    const TopologyUpdateResult update = topology_model_.ApplyDiscovery(metadata, epoch_active);
    if (update == TopologyUpdateResult::Unchanged) {
        return {ControlCommandStatus::NoOp, "scene-topology-unchanged"};
    }
    if (epoch_active) {
        state_->pending_scenes = std::move(discovered);
        LogTopology("discovery-staged");
        return {ControlCommandStatus::Succeeded, "scene-topology-staged"};
    }

    control_state_->control->Shutdown();
    control_state_.reset();
    if (!InstallSceneTargets(std::move(discovered)) || !BuildControlState()) {
        OBS_SYNC_REPLAY_LOG(LOG_ERROR, "topology", "discovery-apply-failed invariant=idle-topology-rebuild");
        return Failed("scene-topology-rebuild");
    }
    LogTopology("discovery-applied");
    return {ControlCommandStatus::Succeeded, "scene-topology-applied"};
}

void PluginCaptureRuntime::PollReplaySave() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (control_state_ && control_state_->control) {
        control_state_->control->PollReplaySave();
        if (const auto replay = control_state_->control->replay_result();
            replay && replay_result_logged_generation_ < replay_save_generation_) {
            OBS_SYNC_REPLAY_LOG(replay->success ? LOG_INFO : LOG_ERROR, "control",
                 "replay-finalized success=%s payload_bytes=%llu first_source_cts=%llu "
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

bool PluginCaptureRuntime::replay_available() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return control_state_ && control_state_->control ? control_state_->control->replay_available()
                                                      : replay_configuration_.enabled;
}

} // namespace obs_sync_replay
