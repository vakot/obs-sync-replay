#include "experiment/stock-encoder-timeline-probe.hpp"

#include "recording/obs-synchronized-recording.hpp"
#include "plugin/plugin-log.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <map>
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
constexpr uint32_t kDefaultBoundaryCycles = 100;
constexpr uint32_t kDefaultLongRunSeconds = 180;
constexpr uint32_t kDefaultCycleWarmupMilliseconds = 300;

enum class ActivationStrategy {
    Sequential,
    Prepared,
    Grouped,
};

const char* StrategyName(const ActivationStrategy strategy) {
    switch (strategy) {
    case ActivationStrategy::Sequential:
        return "sequential";
    case ActivationStrategy::Prepared:
        return "prepared";
    case ActivationStrategy::Grouped:
        return "grouped";
    }
    return "unknown";
}

struct PacketObservation final {
    int64_t pts = 0;
    int64_t dts = 0;
    uint64_t cts = 0;
    uint64_t fer = 0;
    uint64_t ferc = 0;
    uint64_t pir = 0;
    uint64_t callback_wall_ns = 0;
    uint64_t callback_root_pts = 0;
    uint32_t callback_root_frame_count = 0;
    uint32_t callback_root_lagged_frames = 0;
    bool keyframe = false;
};

struct OutputCapture final {
    const char* output = nullptr;
    uint64_t packet_count = 0;
    uint64_t missing_packet_time_count = 0;
    uint64_t first_packet_wall_ns = 0;
    std::vector<PacketObservation> packets;
    std::map<int64_t, uint64_t> cts_by_pts;
    std::map<uint64_t, PacketObservation> packets_by_source_cts;
    uint64_t start_call_wall_ns = 0;
    uint64_t start_call_root_pts = 0;
    uint32_t start_call_root_frame_count = 0;
    uint32_t start_call_root_lagged_frames = 0;
    uint64_t stop_call_wall_ns = 0;
    uint64_t stop_call_root_pts = 0;
    uint32_t stop_call_root_frame_count = 0;
    uint32_t stop_call_root_lagged_frames = 0;
};

struct ExperimentResult final {
    bool started_a = false;
    bool started_b = false;
    bool stopped_cleanly = false;
    bool packet_timing_available = true;
    bool first_observed_cts_equal = false;
    bool packet_pts_cts_mapping_equal = false;
    uint64_t first_observed_cts_a = 0;
    uint64_t first_observed_cts_b = 0;
    uint64_t packet_count_a = 0;
    uint64_t packet_count_b = 0;
    uint64_t packet_mapping_mismatches = 0;
    uint64_t source_cts_mismatches = 0;
    uint64_t source_pts_gaps = 0;
};

struct RootEpochObservation final {
    uint64_t source_cts = 0;
    uint32_t root_frame_count = 0;
};

struct RootEpochCapture final {
    uint32_t tick_stride = 30;
    uint32_t tick_count = 0;
    std::vector<RootEpochObservation> observations;
};

struct ProbeConfig final {
    uint32_t boundary_cycles = kDefaultBoundaryCycles;
    uint32_t long_run_seconds = kDefaultLongRunSeconds;
    uint32_t cycle_warmup_ms = kDefaultCycleWarmupMilliseconds;
    uint32_t epoch_tick_stride = 30;
    uint32_t poc_duration_seconds = 5;
    uint32_t poc_warmup_ms = 2000;
};

uint32_t ReadEnvironmentUint(const char* name, const uint32_t fallback) {
    char* raw_value = nullptr;
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

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) {
        OBS_SYNC_REPLAY_LOG(LOG_WARNING, "experiment", "invalid environment value name=%s value=%s fallback=%u", name, value.c_str(),
             fallback);
        return fallback;
    }
    return static_cast<uint32_t>(parsed);
}

ProbeConfig ReadConfig() {
    ProbeConfig config;
    config.boundary_cycles =
        ReadEnvironmentUint("OBS_SYNC_REPLAY_PROBE_CYCLES", kDefaultBoundaryCycles);
    config.long_run_seconds =
        ReadEnvironmentUint("OBS_SYNC_REPLAY_PROBE_LONG_RUN_SECONDS", kDefaultLongRunSeconds);
    config.cycle_warmup_ms =
        ReadEnvironmentUint("OBS_SYNC_REPLAY_PROBE_CYCLE_WARMUP_MS", kDefaultCycleWarmupMilliseconds);
    config.epoch_tick_stride =
        ReadEnvironmentUint("OBS_SYNC_REPLAY_PROBE_EPOCH_TICK_STRIDE", 30);
    config.poc_duration_seconds =
        ReadEnvironmentUint("OBS_SYNC_REPLAY_PROBE_POC_SECONDS", 5);
    config.poc_warmup_ms =
        ReadEnvironmentUint("OBS_SYNC_REPLAY_PROBE_POC_WARMUP_MS", 2000);
    return config;
}

uint64_t WallClockNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void OnOutputPacket(obs_output_t*, struct encoder_packet* packet, struct encoder_packet_time* packet_time,
                    void* param) {
    auto* capture = static_cast<OutputCapture*>(param);
    if (!capture || !packet || packet->type != OBS_ENCODER_VIDEO) {
        return;
    }

    ++capture->packet_count;
    if (!packet || !packet_time) {
        ++capture->missing_packet_time_count;
        OBS_SYNC_REPLAY_LOG(LOG_WARNING, "experiment", "packet-observation-missing-timing output=%s packet=%llu",
             capture->output ? capture->output : "unknown",
             static_cast<unsigned long long>(capture->packet_count));
        return;
    }

    const PacketObservation observation{packet->pts,
                                        packet->dts,
                                        packet_time->cts,
                                        packet_time->fer,
                                        packet_time->ferc,
                                        packet_time->pir,
                                        WallClockNs(),
                                        obs_get_video_frame_time(),
                                        obs_get_total_frames(),
                                        obs_get_lagged_frames(),
                                        packet->keyframe};
    capture->packets.push_back(observation);
    capture->cts_by_pts[observation.pts] = observation.cts;
    capture->packets_by_source_cts[observation.cts] = observation;
    if (capture->first_packet_wall_ns == 0) {
        capture->first_packet_wall_ns = WallClockNs();
    }

    const size_t sample_index = capture->packets.size() - 1;
    if (sample_index < 3 || sample_index % 120 == 0) {
        OBS_SYNC_REPLAY_LOG(LOG_INFO, "experiment",
             "packet-sample output=%s packet=%llu local_pts=%lld local_dts=%lld "
             "packet_cts=%llu encoder_fer=%llu encoder_ferc=%llu encoder_pir=%llu "
             "callback_root_pts=%llu callback_root_frame_count=%u callback_root_lagged_frames=%u keyframe=%s",
             capture->output ? capture->output : "unknown",
             static_cast<unsigned long long>(capture->packets.size()), static_cast<long long>(observation.pts),
             static_cast<long long>(observation.dts), static_cast<unsigned long long>(observation.cts),
             static_cast<unsigned long long>(observation.fer), static_cast<unsigned long long>(observation.ferc),
             static_cast<unsigned long long>(observation.pir),
             static_cast<unsigned long long>(observation.callback_root_pts), observation.callback_root_frame_count,
             observation.callback_root_lagged_frames, observation.keyframe ? "true" : "false");
    }
}

void OnRootTick(void* param, float) {
    auto* capture = static_cast<RootEpochCapture*>(param);
    if (!capture) {
        return;
    }

    ++capture->tick_count;
    if (capture->tick_count % capture->tick_stride != 0) {
        return;
    }

    capture->observations.push_back(
        RootEpochObservation{obs_get_video_frame_time(), obs_get_total_frames()});
}

void CaptureRootSample(OutputCapture* capture, const bool is_start) {
    if (!capture) {
        return;
    }

    const uint64_t wall_ns = WallClockNs();
    const uint64_t root_pts = obs_get_video_frame_time();
    const uint32_t root_frame_count = obs_get_total_frames();
    const uint32_t root_lagged_frames = obs_get_lagged_frames();
    if (is_start) {
        capture->start_call_wall_ns = wall_ns;
        capture->start_call_root_pts = root_pts;
        capture->start_call_root_frame_count = root_frame_count;
        capture->start_call_root_lagged_frames = root_lagged_frames;
    } else {
        capture->stop_call_wall_ns = wall_ns;
        capture->stop_call_root_pts = root_pts;
        capture->stop_call_root_frame_count = root_frame_count;
        capture->stop_call_root_lagged_frames = root_lagged_frames;
    }
}

uint64_t CountSourcePtsGaps(const std::map<int64_t, uint64_t>& cts_by_pts) {
    if (cts_by_pts.size() < 2) {
        return 0;
    }

    uint64_t gaps = 0;
    auto previous = cts_by_pts.begin();
    for (auto current = std::next(previous); current != cts_by_pts.end(); ++current) {
        if (current->first > previous->first + 1) {
            gaps += static_cast<uint64_t>(current->first - previous->first - 1);
        }
        previous = current;
    }
    return gaps;
}

void SwitchCurrentSceneTask(void* param) {
    auto* scene = static_cast<obs_source_t*>(param);
    if (!scene) {
        return;
    }
    obs_frontend_set_current_scene(scene);
    OBS_SYNC_REPLAY_LOG(LOG_INFO, "experiment", "program-scene-switch scene=%s plugin_views_unchanged=true",
         obs_source_get_name(scene));
    obs_source_release(scene);
}

bool SwitchProgramScene(obs_source_t* scene) {
    obs_source_t* reference = obs_source_get_ref(scene);
    if (!reference) {
        return false;
    }
    obs_queue_task(OBS_TASK_UI, SwitchCurrentSceneTask, reference, true);
    return true;
}

void LogOutputFailure(const char* stage, const char* output_name, obs_output_t* output) {
    OBS_SYNC_REPLAY_LOG(LOG_ERROR, "experiment", "output-failure stage=%s output=%s error=%s", stage,
         output_name ? output_name : "unknown",
         output ? (obs_output_get_last_error(output) ? obs_output_get_last_error(output) : "none") : "null-output");
}

void WaitForOutputInactive(obs_output_t* output) {
    if (!output) {
        return;
    }
    for (uint32_t attempt = 0; attempt < 500 && obs_output_active(output); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

ExperimentResult CompareCaptures(const OutputCapture& capture_a, const OutputCapture& capture_b) {
    ExperimentResult result;
    result.packet_timing_available = capture_a.missing_packet_time_count == 0 &&
                                     capture_b.missing_packet_time_count == 0 && !capture_a.packets.empty() &&
                                     !capture_b.packets.empty();
    result.packet_count_a = capture_a.packet_count;
    result.packet_count_b = capture_b.packet_count;
    result.source_pts_gaps = CountSourcePtsGaps(capture_a.cts_by_pts) + CountSourcePtsGaps(capture_b.cts_by_pts);

    for (const auto& [source_cts, packet_a] : capture_a.packets_by_source_cts) {
        const auto it = capture_b.packets_by_source_cts.find(source_cts);
        if (it == capture_b.packets_by_source_cts.end()) {
            ++result.source_cts_mismatches;
        }
    }
    for (const auto& [source_cts, packet_b] : capture_b.packets_by_source_cts) {
        if (capture_a.packets_by_source_cts.find(source_cts) == capture_a.packets_by_source_cts.end()) {
            ++result.source_cts_mismatches;
        }
    }

    if (!capture_a.cts_by_pts.empty() && !capture_b.cts_by_pts.empty()) {
        result.first_observed_cts_a = capture_a.cts_by_pts.begin()->second;
        result.first_observed_cts_b = capture_b.cts_by_pts.begin()->second;
        result.first_observed_cts_equal = result.first_observed_cts_a == result.first_observed_cts_b;
    }

    for (const auto& [pts, cts_a] : capture_a.cts_by_pts) {
        const auto it = capture_b.cts_by_pts.find(pts);
        if (it == capture_b.cts_by_pts.end() || it->second != cts_a) {
            ++result.packet_mapping_mismatches;
        }
    }
    for (const auto& [pts, cts_b] : capture_b.cts_by_pts) {
        if (capture_a.cts_by_pts.find(pts) == capture_a.cts_by_pts.end()) {
            ++result.packet_mapping_mismatches;
        }
    }
    result.packet_pts_cts_mapping_equal = result.packet_mapping_mismatches == 0 && result.packet_timing_available;
    return result;
}

struct EpochSummary final {
    uint32_t attempts = 0;
    uint32_t usable_attempts = 0;
    uint32_t first_source_cts_equal = 0;
    uint32_t first_source_cts_exactly_at_epoch = 0;
    uint32_t common_keyframe_cts = 0;
};

const PacketObservation* FindFirstPacketAtOrAfter(const OutputCapture& capture, const uint64_t source_cts,
                                                   const bool keyframe_only) {
    for (auto it = capture.packets_by_source_cts.lower_bound(source_cts); it != capture.packets_by_source_cts.end();
         ++it) {
        if (!keyframe_only || it->second.keyframe) {
            return &it->second;
        }
    }
    return nullptr;
}

EpochSummary AnalyzeRootEpochs(const char* encoder_id, const RootEpochCapture& epochs,
                               const OutputCapture& capture_a, const OutputCapture& capture_b) {
    EpochSummary summary;
    summary.attempts = static_cast<uint32_t>(epochs.observations.size());
    for (uint32_t i = 0; i < summary.attempts; ++i) {
        const auto& epoch = epochs.observations[i];
        const PacketObservation* first_a = FindFirstPacketAtOrAfter(capture_a, epoch.source_cts, false);
        const PacketObservation* first_b = FindFirstPacketAtOrAfter(capture_b, epoch.source_cts, false);
        const PacketObservation* key_a = FindFirstPacketAtOrAfter(capture_a, epoch.source_cts, true);
        const PacketObservation* key_b = FindFirstPacketAtOrAfter(capture_b, epoch.source_cts, true);
        if (!first_a || !first_b) {
            continue;
        }

        ++summary.usable_attempts;
        const bool first_equal = first_a->cts == first_b->cts;
        if (first_equal) {
            ++summary.first_source_cts_equal;
        }
        if (first_equal && first_a->cts == epoch.source_cts) {
            ++summary.first_source_cts_exactly_at_epoch;
        }
        if (key_a && key_b && key_a->cts == key_b->cts) {
            ++summary.common_keyframe_cts;
        }

        if (i < 3 || i % 25 == 0) {
            OBS_SYNC_REPLAY_LOG(LOG_INFO, "experiment",
                 "epoch-sample encoder=%s attempt=%u root_epoch_cts=%llu root_frame_count=%u "
                 "first_packet_cts_a=%llu first_packet_cts_b=%llu first_packet_cts_equal=%s "
                 "first_packet_keyframe_a=%s first_packet_keyframe_b=%s common_keyframe_cts=%llu",
                 encoder_id, i + 1, static_cast<unsigned long long>(epoch.source_cts), epoch.root_frame_count,
                 static_cast<unsigned long long>(first_a->cts), static_cast<unsigned long long>(first_b->cts),
                 first_equal ? "true" : "false", first_a->keyframe ? "true" : "false",
                 first_b->keyframe ? "true" : "false",
                 key_a && key_b && key_a->cts == key_b->cts ? static_cast<unsigned long long>(key_a->cts) : 0ULL);
        }
    }

    OBS_SYNC_REPLAY_LOG(LOG_INFO, "experiment",
         "epoch-result encoder=%s attempts=%u usable_attempts=%u first_source_cts_equal=%u "
         "first_source_cts_exactly_at_epoch=%u common_keyframe_cts=%u "
         "root_epoch_identity=public-obs_add_tick_callback-plus-encoder_packet_time_cts "
         "forced_keyframe_api=unavailable-public-stock-api",
         encoder_id, summary.attempts, summary.usable_attempts, summary.first_source_cts_equal,
         summary.first_source_cts_exactly_at_epoch, summary.common_keyframe_cts);
    return summary;
}

} // namespace

struct StockEncoderTimelineProbe::State final {
    obs_view_t* view_a = nullptr;
    obs_view_t* view_b = nullptr;
    video_t* video_a = nullptr;
    video_t* video_b = nullptr;
    obs_source_t* scene_a = nullptr;
    obs_source_t* scene_b = nullptr;
    uint64_t observed_master_frame_id = 0;
};

StockEncoderTimelineProbe::StockEncoderTimelineProbe(std::string scene_a_name, std::string scene_b_name)
    : scene_a_name_(std::move(scene_a_name)), scene_b_name_(std::move(scene_b_name)), state_(std::make_unique<State>()) {}

StockEncoderTimelineProbe::~StockEncoderTimelineProbe() {
    Stop();
}

bool StockEncoderTimelineProbe::Start() {
    if (worker_.joinable()) {
        OBS_SYNC_REPLAY_LOG(LOG_WARNING, "experiment", "start ignored reason=already-running");
        return false;
    }

    state_->scene_a = obs_get_source_by_name(scene_a_name_.c_str());
    state_->scene_b = obs_get_source_by_name(scene_b_name_.c_str());
    if (!state_->scene_a || !state_->scene_b) {
        OBS_SYNC_REPLAY_LOG(LOG_ERROR, "experiment", "setup-failed reason=scene-not-found scene_a=%s scene_b=%s",
             scene_a_name_.c_str(), scene_b_name_.c_str());
        return false;
    }

    state_->view_a = obs_view_create();
    state_->view_b = obs_view_create();
    if (!state_->view_a || !state_->view_b) {
        OBS_SYNC_REPLAY_LOG(LOG_ERROR, "experiment", "setup-failed reason=obs_view_create");
        Stop();
        return false;
    }

    obs_view_set_source(state_->view_a, 0, state_->scene_a);
    obs_view_set_source(state_->view_b, 0, state_->scene_b);
    state_->video_a = obs_view_add(state_->view_a);
    state_->video_b = obs_view_add(state_->view_b);
    if (!state_->video_a || !state_->video_b) {
        OBS_SYNC_REPLAY_LOG(LOG_ERROR, "experiment", "setup-failed reason=obs_view_add");
        Stop();
        return false;
    }

    OBS_SYNC_REPLAY_LOG(LOG_INFO, "experiment",
         "setup-complete view_a=SceneA view_b=SceneB topology=two-views-two-video_t-two-encoders "
         "activation=stock-null-output packet_identity=public-encoder-packet-time-only "
         "source_frame_id_observable=false source_cts_identity_observable=true patched_association_api_used=false");
    stop_requested_ = false;
    worker_ = std::thread(&StockEncoderTimelineProbe::Run, this);
    return true;
}

void StockEncoderTimelineProbe::Stop() {
    stop_requested_ = true;
    OBS_SYNC_REPLAY_LOG(LOG_INFO, "shutdown", "probe-stop begin worker_joinable=%s", worker_.joinable() ? "true" : "false");
    if (worker_.joinable()) {
        worker_.join();
        OBS_SYNC_REPLAY_LOG(LOG_INFO, "shutdown", "probe-stop worker-joined");
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
    OBS_SYNC_REPLAY_LOG(LOG_INFO, "shutdown", "probe-stop views-released");
}

void StockEncoderTimelineProbe::Run() {
    const ProbeConfig config = ReadConfig();
    OBS_SYNC_REPLAY_LOG(LOG_INFO, "experiment",
         "begin clean_runtime=true encoder_ids=%s,%s boundary_cycles=%u long_run_seconds=%u "
         "cycle_warmup_ms=%u poc_seconds=%u poc_warmup_ms=%u source_frame_id_identity=unobservable "
         "source_cts_identity=encoder_packet_time_cts "
         "lag_injector=unavailable",
         kX264EncoderId, kNvencEncoderId, config.boundary_cycles, config.long_run_seconds, config.cycle_warmup_ms,
         config.poc_duration_seconds, config.poc_warmup_ms);
    OBS_SYNC_REPLAY_LOG(LOG_INFO, "experiment", "output-harness id=%s purpose=satisfy-stock-null-output-audio-flag "
                  "video-observations=filtered-to-OBS_ENCODER_VIDEO files_or_muxers=false",
         kAudioEncoderId);

    const char* encoder_ids[] = {kX264EncoderId, kNvencEncoderId};
    for (const char* encoder_id : encoder_ids) {
        if (stop_requested_) {
            break;
        }
        const enum obs_module_load_state load_state = obs_encoder_load_state(encoder_id);
        const char* display_name = obs_encoder_get_display_name(encoder_id);
        OBS_SYNC_REPLAY_LOG(LOG_INFO, "experiment", "encoder-availability id=%s load_state=%d display_name=%s", encoder_id,
             static_cast<int>(load_state), display_name ? display_name : "unknown");
        if (load_state != OBS_MODULE_ENABLED) {
            OBS_SYNC_REPLAY_LOG(LOG_WARNING, "experiment", "encoder-skipped id=%s reason=stock-module-not-loaded", encoder_id);
            continue;
        }

        for (uint32_t cycle = 0; cycle < config.boundary_cycles && !stop_requested_; ++cycle) {
            const auto strategy = static_cast<ActivationStrategy>(cycle % 3);
            const char* mode = StrategyName(strategy);
            const char* output_name_a = "stock-probe-A";
            const char* output_name_b = "stock-probe-B";
            auto* settings_a = obs_encoder_defaults(encoder_id);
            auto* settings_b = obs_encoder_defaults(encoder_id);
            auto* audio_settings_a = obs_encoder_defaults(kAudioEncoderId);
            auto* audio_settings_b = obs_encoder_defaults(kAudioEncoderId);
            obs_encoder_t* encoder_a = obs_video_encoder_create(encoder_id, "Stock Probe Encoder A", settings_a, nullptr);
            obs_encoder_t* encoder_b = obs_video_encoder_create(encoder_id, "Stock Probe Encoder B", settings_b, nullptr);
            obs_encoder_t* audio_encoder_a = obs_audio_encoder_create(kAudioEncoderId, "Stock Probe Audio A",
                                                                        audio_settings_a, 0, nullptr);
            obs_encoder_t* audio_encoder_b = obs_audio_encoder_create(kAudioEncoderId, "Stock Probe Audio B",
                                                                        audio_settings_b, 0, nullptr);
            if (settings_a) obs_data_release(settings_a);
            if (settings_b) obs_data_release(settings_b);
            if (audio_settings_a) obs_data_release(audio_settings_a);
            if (audio_settings_b) obs_data_release(audio_settings_b);
            obs_output_t* output_a = obs_output_create(kNullOutputId, "Stock Probe Output A", nullptr, nullptr);
            obs_output_t* output_b = obs_output_create(kNullOutputId, "Stock Probe Output B", nullptr, nullptr);
            OutputCapture capture_a{output_name_a};
            OutputCapture capture_b{output_name_b};
            obs_encoder_group_t* group = nullptr;
            bool ready = encoder_a && encoder_b && audio_encoder_a && audio_encoder_b && output_a && output_b;

            if (ready) {
                obs_encoder_set_video(encoder_a, state_->video_a);
                obs_encoder_set_video(encoder_b, state_->video_b);
                obs_encoder_set_audio(audio_encoder_a, obs_get_audio());
                obs_encoder_set_audio(audio_encoder_b, obs_get_audio());
                obs_output_set_video_encoder(output_a, encoder_a);
                obs_output_set_video_encoder(output_b, encoder_b);
                obs_output_set_audio_encoder(output_a, audio_encoder_a, 0);
                obs_output_set_audio_encoder(output_b, audio_encoder_b, 0);
                obs_output_add_packet_callback(output_a, OnOutputPacket, &capture_a);
                obs_output_add_packet_callback(output_b, OnOutputPacket, &capture_b);
                if (strategy == ActivationStrategy::Grouped) {
                    group = obs_encoder_group_create();
                    ready = group && obs_encoder_set_group(encoder_a, group) && obs_encoder_set_group(encoder_b, group);
                }
                if (ready && strategy == ActivationStrategy::Prepared) {
                    ready = obs_output_initialize_encoders(output_a, 0) && obs_output_initialize_encoders(output_b, 0);
                }
            }

            OBS_SYNC_REPLAY_LOG(LOG_INFO, "experiment",
             "activation-begin encoder=%s strategy=%s cycle=%u canonical_obs_pts=%llu "
                 "obs_root_frame_count=%u obs_lagged_frames=%u group=%s",
                 encoder_id, mode, cycle + 1, static_cast<unsigned long long>(obs_get_video_frame_time()),
                 obs_get_total_frames(), obs_get_lagged_frames(), group ? "true" : "false");

            if (ready) {
                capture_a.output = output_name_a;
                capture_b.output = output_name_b;
                capture_a.packets.reserve(32);
                capture_b.packets.reserve(32);
                capture_a.cts_by_pts.clear();
                capture_b.cts_by_pts.clear();
                CaptureRootSample(&capture_a, true);
                const bool started_a = obs_output_start(output_a);
                bool started_b = false;
                if (started_a) {
                    CaptureRootSample(&capture_b, true);
                    started_b = obs_output_start(output_b);
                }
                if (!started_a) LogOutputFailure("start", output_name_a, output_a);
                if (started_a && !started_b) LogOutputFailure("start", output_name_b, output_b);
                std::this_thread::sleep_for(std::chrono::milliseconds(config.cycle_warmup_ms));
                const bool stop_b_first = (cycle % 2) != 0;
                if (stop_b_first) {
                    if (started_b) {
                        CaptureRootSample(&capture_b, false);
                        obs_output_stop(output_b);
                    }
                    if (started_a) {
                        CaptureRootSample(&capture_a, false);
                        obs_output_stop(output_a);
                    }
                } else {
                    if (started_a) {
                        CaptureRootSample(&capture_a, false);
                        obs_output_stop(output_a);
                    }
                    if (started_b) {
                        CaptureRootSample(&capture_b, false);
                        obs_output_stop(output_b);
                    }
                }
                WaitForOutputInactive(output_a);
                WaitForOutputInactive(output_b);
                const ExperimentResult result = CompareCaptures(capture_a, capture_b);
                OBS_SYNC_REPLAY_LOG(result.packet_pts_cts_mapping_equal ? LOG_INFO : LOG_WARNING, "experiment",
                     "activation-result encoder=%s strategy=%s cycle=%u started_a=%s started_b=%s "
                     "first_observed_packet_cts_a=%llu first_observed_packet_cts_b=%llu first_cts_equal=%s "
                     "packet_count_a=%llu packet_count_b=%llu packet_pts_cts_mismatches=%llu "
                     "packet_cts_mapping_equal=%s packet_timing_available=%s source_cts_mismatches=%llu "
                     "source_cts_mapping_equal=%s source_pts_gaps=%llu "
                     "source_frame_id_identity_proven=false source_cts_identity_observed=true stop_order=%s "
                     "start_root_frame_count_a=%u start_root_frame_count_b=%u "
                     "stop_root_frame_count_a=%u stop_root_frame_count_b=%u "
                     "last_packet_pts_a=%lld last_packet_pts_b=%lld",
                     encoder_id, mode, cycle + 1, started_a ? "true" : "false", started_b ? "true" : "false",
                     static_cast<unsigned long long>(result.first_observed_cts_a),
                     static_cast<unsigned long long>(result.first_observed_cts_b),
                     result.first_observed_cts_equal ? "true" : "false",
                     static_cast<unsigned long long>(result.packet_count_a),
                     static_cast<unsigned long long>(result.packet_count_b),
                     static_cast<unsigned long long>(result.packet_mapping_mismatches),
                     result.packet_pts_cts_mapping_equal ? "true" : "false",
                     result.packet_timing_available ? "true" : "false",
                     static_cast<unsigned long long>(result.source_cts_mismatches),
                     result.source_cts_mismatches == 0 ? "true" : "false",
                     static_cast<unsigned long long>(result.source_pts_gaps), stop_b_first ? "B_then_A" : "A_then_B",
                     capture_a.start_call_root_frame_count, capture_b.start_call_root_frame_count,
                     capture_a.stop_call_root_frame_count, capture_b.stop_call_root_frame_count,
                     capture_a.packets.empty() ? -1LL : static_cast<long long>(capture_a.packets.back().pts),
                     capture_b.packets.empty() ? -1LL : static_cast<long long>(capture_b.packets.back().pts));
            } else {
                OBS_SYNC_REPLAY_LOG(LOG_ERROR, "experiment", "activation-result encoder=%s strategy=%s cycle=%u status=not-ready",
                     encoder_id, mode, cycle + 1);
            }

            if (output_a) obs_output_remove_packet_callback(output_a, OnOutputPacket, &capture_a);
            if (output_b) obs_output_remove_packet_callback(output_b, OnOutputPacket, &capture_b);
            if (group) obs_encoder_group_destroy(group);
            if (output_a) obs_output_release(output_a);
            if (output_b) obs_output_release(output_b);
            if (encoder_a) obs_encoder_release(encoder_a);
            if (encoder_b) obs_encoder_release(encoder_b);
            if (audio_encoder_a) obs_encoder_release(audio_encoder_a);
            if (audio_encoder_b) obs_encoder_release(audio_encoder_b);
        }

        if (stop_requested_) {
            break;
        }
        if (config.long_run_seconds == 0) {
            RunSynchronizedRecording(encoder_id, state_->video_a, state_->video_b, config.poc_duration_seconds,
                                     config.poc_warmup_ms, &stop_requested_);
            continue;
        }

        auto* settings_a = obs_encoder_defaults(encoder_id);
        auto* settings_b = obs_encoder_defaults(encoder_id);
        auto* audio_settings_a = obs_encoder_defaults(kAudioEncoderId);
        auto* audio_settings_b = obs_encoder_defaults(kAudioEncoderId);
        obs_encoder_t* encoder_a = obs_video_encoder_create(encoder_id, "Stock Probe Long Encoder A", settings_a, nullptr);
        obs_encoder_t* encoder_b = obs_video_encoder_create(encoder_id, "Stock Probe Long Encoder B", settings_b, nullptr);
        obs_encoder_t* audio_encoder_a = obs_audio_encoder_create(kAudioEncoderId, "Stock Probe Long Audio A",
                                                                   audio_settings_a, 0, nullptr);
        obs_encoder_t* audio_encoder_b = obs_audio_encoder_create(kAudioEncoderId, "Stock Probe Long Audio B",
                                                                   audio_settings_b, 0, nullptr);
        if (settings_a) obs_data_release(settings_a);
        if (settings_b) obs_data_release(settings_b);
        if (audio_settings_a) obs_data_release(audio_settings_a);
        if (audio_settings_b) obs_data_release(audio_settings_b);
        obs_output_t* output_a = obs_output_create(kNullOutputId, "Stock Probe Long Output A", nullptr, nullptr);
        obs_output_t* output_b = obs_output_create(kNullOutputId, "Stock Probe Long Output B", nullptr, nullptr);
        OutputCapture capture_a{"long-A"};
        OutputCapture capture_b{"long-B"};
        RootEpochCapture epoch_capture;
        epoch_capture.tick_stride = config.epoch_tick_stride;
        obs_encoder_group_t* group = nullptr;
        bool ready = encoder_a && encoder_b && audio_encoder_a && audio_encoder_b && output_a && output_b;
        if (ready) {
            obs_encoder_set_video(encoder_a, state_->video_a);
            obs_encoder_set_video(encoder_b, state_->video_b);
            obs_encoder_set_audio(audio_encoder_a, obs_get_audio());
            obs_encoder_set_audio(audio_encoder_b, obs_get_audio());
            obs_output_set_video_encoder(output_a, encoder_a);
            obs_output_set_video_encoder(output_b, encoder_b);
            obs_output_set_audio_encoder(output_a, audio_encoder_a, 0);
            obs_output_set_audio_encoder(output_b, audio_encoder_b, 0);
            obs_output_add_packet_callback(output_a, OnOutputPacket, &capture_a);
            obs_output_add_packet_callback(output_b, OnOutputPacket, &capture_b);
            group = obs_encoder_group_create();
            ready = group && obs_encoder_set_group(encoder_a, group) && obs_encoder_set_group(encoder_b, group);
        }

        OBS_SYNC_REPLAY_LOG(LOG_INFO, "experiment",
             "long-run-begin encoder=%s seconds=%u strategy=grouped program_scene_switch=true "
             "canonical_obs_pts=%llu obs_root_frame_count=%u obs_lagged_frames=%u epoch_tick_stride=%u "
             "forced_keyframe_api=unavailable-public-stock-api",
             encoder_id, config.long_run_seconds, static_cast<unsigned long long>(obs_get_video_frame_time()),
             obs_get_total_frames(), obs_get_lagged_frames(), config.epoch_tick_stride);
        bool started_a = false;
        bool started_b = false;
        if (ready) {
            CaptureRootSample(&capture_a, true);
            started_a = obs_output_start(output_a);
            if (started_a) {
                CaptureRootSample(&capture_b, true);
                started_b = obs_output_start(output_b);
            }
        }
        if (started_a && started_b) {
            obs_add_tick_callback(OnRootTick, &epoch_capture);
            obs_source_t* scene_a = state_->scene_a;
            obs_source_t* scene_b = state_->scene_b;
            const auto start = std::chrono::steady_clock::now();
            uint32_t last_scene_switch_second = 0;
            while (!stop_requested_) {
                const uint32_t elapsed = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count());
                if (elapsed >= config.long_run_seconds) break;
                if (elapsed / 5 > last_scene_switch_second / 5) {
                    last_scene_switch_second = elapsed;
                    SwitchProgramScene((elapsed / 5) % 2 == 0 ? scene_a : scene_b);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            obs_remove_tick_callback(OnRootTick, &epoch_capture);
            CaptureRootSample(&capture_a, false);
            CaptureRootSample(&capture_b, false);
            obs_output_stop(output_a);
            obs_output_stop(output_b);
            WaitForOutputInactive(output_a);
            WaitForOutputInactive(output_b);
            const ExperimentResult result = CompareCaptures(capture_a, capture_b);
            AnalyzeRootEpochs(encoder_id, epoch_capture, capture_a, capture_b);
            OBS_SYNC_REPLAY_LOG(result.packet_pts_cts_mapping_equal ? LOG_INFO : LOG_WARNING, "experiment",
                 "long-run-result encoder=%s duration_seconds=%u packet_count_a=%llu packet_count_b=%llu "
                 "first_observed_packet_cts_a=%llu first_observed_packet_cts_b=%llu packet_pts_cts_mismatches=%llu "
                 "packet_cts_mapping_equal=%s source_cts_mismatches=%llu source_cts_mapping_equal=%s "
                 "source_pts_gaps=%llu obs_lagged_frames=%u "
                 "source_frame_id_identity_proven=false source_cts_identity_observed=true "
                 "start_root_frame_count_a=%u start_root_frame_count_b=%u "
                 "stop_root_frame_count_a=%u stop_root_frame_count_b=%u "
                 "last_packet_pts_a=%lld last_packet_pts_b=%lld",
                 encoder_id, config.long_run_seconds, static_cast<unsigned long long>(result.packet_count_a),
                 static_cast<unsigned long long>(result.packet_count_b),
                 static_cast<unsigned long long>(result.first_observed_cts_a),
                 static_cast<unsigned long long>(result.first_observed_cts_b),
                 static_cast<unsigned long long>(result.packet_mapping_mismatches),
                 result.packet_pts_cts_mapping_equal ? "true" : "false",
                 static_cast<unsigned long long>(result.source_cts_mismatches),
                 result.source_cts_mismatches == 0 ? "true" : "false",
                 static_cast<unsigned long long>(result.source_pts_gaps), obs_get_lagged_frames(),
                 capture_a.start_call_root_frame_count, capture_b.start_call_root_frame_count,
                 capture_a.stop_call_root_frame_count, capture_b.stop_call_root_frame_count,
                 capture_a.packets.empty() ? -1LL : static_cast<long long>(capture_a.packets.back().pts),
                 capture_b.packets.empty() ? -1LL : static_cast<long long>(capture_b.packets.back().pts));
        } else {
            if (!started_a) {
                LogOutputFailure("long-run-start", "long-A", output_a);
            }
            if (started_a && !started_b) {
                LogOutputFailure("long-run-start", "long-B", output_b);
            }
            if (started_a) {
                obs_output_stop(output_a);
            }
            if (started_b) {
                obs_output_stop(output_b);
            }
            WaitForOutputInactive(output_a);
            WaitForOutputInactive(output_b);
        }

        if (output_a) obs_output_remove_packet_callback(output_a, OnOutputPacket, &capture_a);
        if (output_b) obs_output_remove_packet_callback(output_b, OnOutputPacket, &capture_b);
        if (group) obs_encoder_group_destroy(group);
        if (output_a) obs_output_release(output_a);
        if (output_b) obs_output_release(output_b);
        if (encoder_a) obs_encoder_release(encoder_a);
        if (encoder_b) obs_encoder_release(encoder_b);
        if (audio_encoder_a) obs_encoder_release(audio_encoder_a);
        if (audio_encoder_b) obs_encoder_release(audio_encoder_b);

        if (stop_requested_) {
            break;
        }
        RunSynchronizedRecording(encoder_id, state_->video_a, state_->video_b, config.poc_duration_seconds,
                                 config.poc_warmup_ms, &stop_requested_);
    }

    OBS_SYNC_REPLAY_LOG(LOG_INFO, "experiment",
         "complete decision_basis=public_root_tick_plus_encoder_packet_cts "
         "source_frame_id_proof=false source_cts_identity_observed=true");
}

} // namespace obs_sync_replay
