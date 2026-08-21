#include "experiment/three-stream-capture-poc.hpp"

#include "muxing/mkv-packet-writer.hpp"
#include "sync/common-packet-range.hpp"

#include <obs.h>
#include <obs-encoder.h>
#include <obs-frontend-api.h>
#include <obs-module.h>

extern "C" {
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iterator>
#include <map>
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
    uint32_t transition_duration_ms = kDefaultTransitionDurationMilliseconds;
    uint32_t transition_period_seconds = kDefaultTransitionPeriodSeconds;
};

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
    config.transition_duration_ms = ReadEnvironmentUint("OBS_SYNC_REPLAY_THREE_STREAM_TRANSITION_MS",
                                                        kDefaultTransitionDurationMilliseconds);
    config.transition_period_seconds = ReadEnvironmentUint("OBS_SYNC_REPLAY_THREE_STREAM_TRANSITION_PERIOD_SECONDS",
                                                           kDefaultTransitionPeriodSeconds);
    return config;
}

uint64_t WallClockNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct CapturedPacket final {
    EncodedPacket packet;
};

class BoundedPacketHistory final {
public:
    BoundedPacketHistory(const char *stream_name, const size_t capacity_packets)
        : stream_name_(stream_name), capacity_packets_(capacity_packets) {}

    void Observe(struct encoder_packet *packet, struct encoder_packet_time *packet_time) {
        if (!packet || packet->type != OBS_ENCODER_VIDEO) {
            return;
        }
        ++packet_count_;
        if (packet->size == 0 || !packet_time ||
            packet->timebase_num <= 0 || packet->timebase_den <= 0) {
            ++rejected_packet_count_;
            if (rejected_packet_count_ <= 3) {
                blog(LOG_ERROR,
                     "[three-stream-poc] packet-rejected stream=%s invariant=video-packet-and-source-cts-required",
                     stream_name_);
            }
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

        if (extra_data_.empty() && packet->encoder) {
            uint8_t *extra_data = nullptr;
            size_t extra_size = 0;
            if (obs_encoder_get_extra_data(packet->encoder, &extra_data, &extra_size) && extra_data && extra_size > 0) {
                extra_data_.assign(extra_data, extra_data + extra_size);
            }
        }

        if (history_.empty()) {
            first_source_cts_ = copy.source_cts;
        }
        if (cts_index_.find(copy.source_cts) != cts_index_.end()) {
            ++duplicate_cts_count_;
            return;
        }

        bytes_ += copy.payload.size();
        history_.push_back(CapturedPacket{std::move(copy)});
        cts_index_[history_.back().packet.source_cts] = true;
        peak_bytes_ = std::max(peak_bytes_, bytes_);
        while (history_.size() > capacity_packets_) {
            EvictFront();
        }
    }

    std::map<uint64_t, EncodedPacket> SnapshotIndex() const {
        std::map<uint64_t, EncodedPacket> result;
        for (const CapturedPacket &captured : history_) {
            result.emplace(captured.packet.source_cts, captured.packet);
        }
        return result;
    }

    uint64_t packet_count() const noexcept { return packet_count_; }
    uint64_t rejected_packet_count() const noexcept { return rejected_packet_count_; }
    uint64_t duplicate_cts_count() const noexcept { return duplicate_cts_count_; }
    size_t retained_packet_count() const noexcept { return history_.size(); }
    size_t retained_bytes() const noexcept { return bytes_; }
    size_t peak_bytes() const noexcept { return peak_bytes_; }
    uint64_t first_source_cts() const noexcept { return first_source_cts_; }
    const std::vector<uint8_t> &extra_data() const noexcept { return extra_data_; }

private:
    using History = std::deque<CapturedPacket>;
    void EvictFront() {
        if (history_.empty()) {
            return;
        }
        const uint64_t cts = history_.front().packet.source_cts;
        bytes_ -= history_.front().packet.payload.size();
        cts_index_.erase(cts);
        history_.pop_front();
    }

    const char *stream_name_ = nullptr;
    size_t capacity_packets_ = 0;
    History history_;
    std::map<uint64_t, bool> cts_index_;
    uint64_t packet_count_ = 0;
    uint64_t rejected_packet_count_ = 0;
    uint64_t duplicate_cts_count_ = 0;
    uint64_t first_source_cts_ = 0;
    std::vector<uint8_t> extra_data_;
    size_t bytes_ = 0;
    size_t peak_bytes_ = 0;
};

struct CallbackContext final {
    BoundedPacketHistory *history = nullptr;
};

void OnOutputPacket(obs_output_t *, struct encoder_packet *packet, struct encoder_packet_time *packet_time,
                    void *param) {
    auto *context = static_cast<CallbackContext *>(param);
    if (context && context->history) {
        context->history->Observe(packet, packet_time);
    }
}

struct StreamResources final {
    StreamId id = StreamId::Master;
    obs_encoder_t *video_encoder = nullptr;
    obs_encoder_t *audio_encoder = nullptr;
    obs_output_t *output = nullptr;
    std::unique_ptr<BoundedPacketHistory> history;
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
    if (stream.output && stream.callback.history) {
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

PacketStreamConfig StreamConfig(obs_encoder_t *encoder, const BoundedPacketHistory &history) {
    PacketStreamConfig config;
    config.width = encoder && obs_encoder_get_width(encoder) ? obs_encoder_get_width(encoder) : kFallbackWidth;
    config.height = encoder && obs_encoder_get_height(encoder) ? obs_encoder_get_height(encoder) : kFallbackHeight;
    const std::map<uint64_t, EncodedPacket> index = history.SnapshotIndex();
    if (!index.empty()) {
        config.timebase_num = index.begin()->second.timebase_num;
        config.timebase_den = index.begin()->second.timebase_den;
    }
    uint8_t *extra_data = nullptr;
    size_t extra_size = 0;
    if (encoder && obs_encoder_get_extra_data(encoder, &extra_data, &extra_size) && extra_data && extra_size > 0) {
        config.extra_data.assign(extra_data, extra_data + extra_size);
    }
    if (config.extra_data.empty()) {
        config.extra_data = history.extra_data();
    }
    return config;
}

bool HasKeyframeAt(const std::map<uint64_t, EncodedPacket> &index, const uint64_t cts) {
    const auto it = index.find(cts);
    return it != index.end() && it->second.keyframe;
}

struct CommonRange final {
    uint64_t start_cts = 0;
    uint64_t end_cts = 0;
    uint64_t missing_packets = 0;
};

bool ContainsAll(const std::array<std::map<uint64_t, EncodedPacket>, 3> &indexes, const uint64_t cts) {
    return std::all_of(indexes.begin(), indexes.end(), [cts](const auto &index) { return index.find(cts) != index.end(); });
}

std::optional<CommonRange> SelectCommonRange(const std::array<std::map<uint64_t, EncodedPacket>, 3> &indexes,
                                             const uint32_t output_seconds, const uint64_t frame_interval_ns,
                                             uint64_t *missing_packets) {
    if (indexes[0].empty() || indexes[1].empty() || indexes[2].empty()) {
        return std::nullopt;
    }

    const uint64_t latest_first = std::max({indexes[0].begin()->first, indexes[1].begin()->first, indexes[2].begin()->first});
    const uint64_t earliest_last = std::min({indexes[0].rbegin()->first, indexes[1].rbegin()->first, indexes[2].rbegin()->first});
    if (latest_first >= earliest_last) {
        return std::nullopt;
    }

    std::vector<uint64_t> common_keyframes;
    for (const auto &[cts, packet] : indexes[0]) {
        if (cts < latest_first || cts >= earliest_last || !packet.keyframe || !ContainsAll(indexes, cts)) {
            continue;
        }
        if (HasKeyframeAt(indexes[1], cts) && HasKeyframeAt(indexes[2], cts)) {
            common_keyframes.push_back(cts);
        }
    }
    if (common_keyframes.empty()) {
        return std::nullopt;
    }

    const uint64_t desired_duration = static_cast<uint64_t>(output_seconds) * 1'000'000'000ULL;
    const uint64_t target_start = earliest_last > desired_duration ? earliest_last - desired_duration : latest_first;
    uint64_t start_cts = common_keyframes.front();
    for (const uint64_t keyframe_cts : common_keyframes) {
        if (keyframe_cts <= target_start) {
            start_cts = keyframe_cts;
        }
    }

    uint64_t end_cts = start_cts;
    uint64_t missing = 0;
    bool started = false;
    for (const auto &[cts, packet] : indexes[0]) {
        (void)packet;
        if (cts < start_cts || cts > earliest_last) {
            continue;
        }
        if (!started) {
            if (cts != start_cts) {
                continue;
            }
            started = true;
        }
        if (!ContainsAll(indexes, cts)) {
            ++missing;
            break;
        }
        end_cts = cts;
    }
    if (!started || end_cts <= start_cts || (frame_interval_ns != 0 && end_cts - start_cts < frame_interval_ns)) {
        return std::nullopt;
    }
    if (missing_packets) {
        *missing_packets = missing;
    }
    return CommonRange{start_cts, end_cts, missing};
}

std::vector<EncodedPacket> PacketsInRange(const std::map<uint64_t, EncodedPacket> &index,
                                          const CommonRange &range, const uint64_t frame_interval_ns) {
    std::vector<EncodedPacket> packets;
    for (auto it = index.lower_bound(range.start_cts); it != index.end() && it->first <= range.end_cts; ++it) {
        packets.push_back(it->second);
    }
    packets = SortForDecodeOrder(std::move(packets));
    if (packets.empty() || packets.front().timebase_num <= 0 || packets.front().timebase_den <= 0 ||
        frame_interval_ns == 0) {
        return packets;
    }

    const AVRational source_timebase{1, 1'000'000'000};
    const AVRational packet_timebase{packets.front().timebase_num, packets.front().timebase_den};
    const int64_t dts_origin = packets.front().dts;
    for (EncodedPacket &packet : packets) {
        const uint64_t relative_cts = packet.source_cts - range.start_cts;
        packet.pts = av_rescale_q(static_cast<int64_t>(relative_cts), source_timebase, packet_timebase);
        packet.dts -= dts_origin;
    }
    return packets;
}

bool WriteRange(const std::filesystem::path &path, const StreamResources &stream,
                const std::map<uint64_t, EncodedPacket> &index, const CommonRange &range, MkvWriteResult *result) {
    PacketStreamConfig config = StreamConfig(stream.video_encoder, *stream.history);
    if (config.timebase_num <= 0 || config.timebase_den <= 0 || config.extra_data.empty()) {
        blog(LOG_ERROR, "[three-stream-poc] mux-failed stream=%s reason=missing-stream-config", StreamName(stream.id));
        return false;
    }

    MkvPacketWriter writer;
    if (!writer.Open(path.string(), config)) {
        blog(LOG_ERROR, "[three-stream-poc] mux-failed stream=%s path=%s reason=%s", StreamName(stream.id),
             path.string().c_str(), writer.error().c_str());
        return false;
    }
    const std::vector<EncodedPacket> packets =
        PacketsInRange(index, range, obs_get_frame_interval_ns() == 0 ? 16'666'667 : obs_get_frame_interval_ns());
    for (const EncodedPacket &packet : packets) {
        if (!writer.Write(packet)) {
            blog(LOG_ERROR, "[three-stream-poc] mux-failed stream=%s path=%s reason=%s", StreamName(stream.id),
                 path.string().c_str(), writer.error().c_str());
            writer.Abort();
            return false;
        }
    }
    *result = writer.Finalize();
    if (!result->success || result->first_source_cts != range.start_cts || result->last_source_cts != range.end_cts) {
        blog(LOG_ERROR,
             "[three-stream-poc] mux-result-invalid stream=%s path=%s success=%s first_cts=%llu last_cts=%llu "
             "expected_start=%llu expected_end=%llu error=%s",
             StreamName(stream.id), path.string().c_str(), result->success ? "true" : "false",
             static_cast<unsigned long long>(result->first_source_cts), static_cast<unsigned long long>(result->last_source_cts),
             static_cast<unsigned long long>(range.start_cts), static_cast<unsigned long long>(range.end_cts),
             result->error.c_str());
        return false;
    }
    return true;
}

void LogHistory(const StreamResources &stream) {
    const BoundedPacketHistory &history = *stream.history;
    blog(LOG_INFO,
         "[three-stream-poc] history stream=%s packets_seen=%llu packets_retained=%llu rejected=%llu "
         "duplicate_cts=%llu first_cts=%llu retained_bytes=%llu peak_bytes=%llu extra_data_bytes=%llu",
         StreamName(stream.id), static_cast<unsigned long long>(history.packet_count()),
         static_cast<unsigned long long>(history.retained_packet_count()),
         static_cast<unsigned long long>(history.rejected_packet_count()),
         static_cast<unsigned long long>(history.duplicate_cts_count()),
         static_cast<unsigned long long>(history.first_source_cts()),
         static_cast<unsigned long long>(history.retained_bytes()), static_cast<unsigned long long>(history.peak_bytes()),
         static_cast<unsigned long long>(history.extra_data().size()));
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
    const uint64_t frame_interval_ns = obs_get_frame_interval_ns();
    const size_t ring_capacity_packets = static_cast<size_t>(
        std::max<uint64_t>(120, (static_cast<uint64_t>(config.ring_seconds) * 1'000'000'000ULL) /
                                   (frame_interval_ns == 0 ? 16'666'667ULL : frame_interval_ns)));

    blog(LOG_INFO,
         "[three-stream-poc] begin topology=program-obs_get_video-plus-two-obs_view streams=master,scene_a,scene_b "
         "encoder_ids=%s,%s duration_seconds=%u warmup_ms=%u ring_capacity_packets=%llu video=%s",
         kX264EncoderId, kNvencEncoderId, config.long_run_seconds, config.warmup_milliseconds,
         static_cast<unsigned long long>(ring_capacity_packets), video_info ? "1920x1080@60" : "unknown");

    for (const char *encoder_id : {kX264EncoderId, kNvencEncoderId}) {
        if (stop_requested_) {
            break;
        }
        if (obs_encoder_load_state(encoder_id) != OBS_MODULE_ENABLED) {
            blog(LOG_WARNING, "[three-stream-poc] encoder-skipped id=%s reason=stock-module-not-loaded", encoder_id);
            continue;
        }

        std::array<StreamResources, 3> streams{{
            {StreamId::Master, nullptr, nullptr, nullptr,
             std::make_unique<BoundedPacketHistory>(StreamName(StreamId::Master), ring_capacity_packets), {}, false},
            {StreamId::SceneA, nullptr, nullptr, nullptr,
             std::make_unique<BoundedPacketHistory>(StreamName(StreamId::SceneA), ring_capacity_packets), {}, false},
            {StreamId::SceneB, nullptr, nullptr, nullptr,
             std::make_unique<BoundedPacketHistory>(StreamName(StreamId::SceneB), ring_capacity_packets), {}, false},
        }};
        for (StreamResources &stream : streams) {
            obs_data_t *settings = CreateVideoSettings(encoder_id);
            obs_data_t *audio_settings = obs_encoder_defaults(kAudioEncoderId);
            stream.video_encoder = obs_video_encoder_create(encoder_id, StreamName(stream.id), settings, nullptr);
            stream.audio_encoder = obs_audio_encoder_create(kAudioEncoderId, StreamName(stream.id), audio_settings, 0,
                                                            nullptr);
            if (settings) {
                obs_data_release(settings);
            }
            if (audio_settings) {
                obs_data_release(audio_settings);
            }
            stream.output = obs_output_create(kNullOutputId, StreamName(stream.id), nullptr, nullptr);
            stream.callback.history = stream.history.get();
        }

        bool ready = true;
        for (const StreamResources &stream : streams) {
            ready = ready && stream.video_encoder && stream.audio_encoder && stream.output;
        }
        if (ready) {
            obs_encoder_set_video(streams[0].video_encoder, obs_get_video());
            obs_encoder_set_video(streams[1].video_encoder, state_->video_a);
            obs_encoder_set_video(streams[2].video_encoder, state_->video_b);
            for (StreamResources &stream : streams) {
                obs_encoder_set_audio(stream.audio_encoder, obs_get_audio());
                obs_output_set_video_encoder(stream.output, stream.video_encoder);
                obs_output_set_audio_encoder(stream.output, stream.audio_encoder, 0);
                obs_output_add_packet_callback(stream.output, OnOutputPacket, &stream.callback);
            }
        }

        obs_encoder_group_t *group = ready ? obs_encoder_group_create() : nullptr;
        if (group) {
            for (StreamResources &stream : streams) {
                ready = ready && obs_encoder_set_group(stream.video_encoder, group);
            }
        }

        if (!ready) {
            blog(LOG_ERROR, "[three-stream-poc] setup-failed encoder=%s reason=resource-or-group-create", encoder_id);
            if (group) {
                obs_encoder_group_destroy(group);
            }
            for (StreamResources &stream : streams) {
                ReleaseStream(stream);
            }
            continue;
        }

        EnsureFadeTransition();
        obs_frontend_set_transition_duration(static_cast<int>(config.transition_duration_ms));
        QueueProgramScene(state_->scene_a);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        blog(LOG_INFO,
             "[three-stream-poc] start encoder=%s master_source=obs_get_video program_transition=true "
             "video_encoder_count=3 audio_harness_encoder_count=3 one_packet_callback_per_stream=true",
             encoder_id);
        for (StreamResources &stream : streams) {
            stream.started = obs_output_start(stream.output);
            if (!stream.started) {
                LogOutputFailure("start", stream);
            }
        }

        if (std::all_of(streams.begin(), streams.end(), [](const StreamResources &stream) { return stream.started; })) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config.warmup_milliseconds));
            const auto start_time = std::chrono::steady_clock::now();
            uint32_t last_transition_second = 0;
            bool target_b = true;
            while (!stop_requested_) {
                const uint32_t elapsed = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time).count());
                if (elapsed >= config.long_run_seconds) {
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
        }

        for (auto it = streams.rbegin(); it != streams.rend(); ++it) {
            if (it->started) {
                obs_output_stop(it->output);
            }
        }
        for (StreamResources &stream : streams) {
            WaitForOutputInactive(stream.output);
        }
        if (group) {
            obs_encoder_group_destroy(group);
        }

        for (const StreamResources &stream : streams) {
            LogHistory(stream);
        }

        std::array<std::map<uint64_t, EncodedPacket>, 3> indexes{
            streams[0].history->SnapshotIndex(), streams[1].history->SnapshotIndex(), streams[2].history->SnapshotIndex()};
        uint64_t missing_packets = 0;
        const std::optional<CommonRange> range =
            SelectCommonRange(indexes, config.output_seconds, frame_interval_ns, &missing_packets);
        if (!range) {
            blog(LOG_ERROR, "[three-stream-poc] range-failed encoder=%s reason=no-common-keyframe-aligned-range", encoder_id);
        } else {
            const std::filesystem::path directory = std::filesystem::current_path() / "three-stream-poc";
            std::error_code directory_error;
            std::filesystem::create_directories(directory, directory_error);
            const std::string stem = std::string("three-stream-") + encoder_id + "-" + std::to_string(WallClockNs());
            const std::array<std::filesystem::path, 3> paths{{
                directory / (stem + "-master.mkv"),
                directory / (stem + "-scene-a.mkv"),
                directory / (stem + "-scene-b.mkv"),
            }};
            std::array<MkvWriteResult, 3> results;
            bool mux_ok = !directory_error;
            for (size_t index = 0; index < streams.size() && mux_ok; ++index) {
                mux_ok = WriteRange(paths[index], streams[index], indexes[index], *range, &results[index]);
            }
            blog(mux_ok ? LOG_INFO : LOG_ERROR,
                 "[three-stream-poc] result encoder=%s range_start_cts=%llu range_end_cts=%llu missing_packets=%llu "
                 "common_start_keyframe=true mux_ok=%s path_master=%s path_scene_a=%s path_scene_b=%s "
                 "first_cts_m=%llu first_cts_a=%llu first_cts_b=%llu last_cts_m=%llu last_cts_a=%llu last_cts_b=%llu",
                 encoder_id, static_cast<unsigned long long>(range->start_cts),
                 static_cast<unsigned long long>(range->end_cts), static_cast<unsigned long long>(missing_packets),
                 mux_ok ? "true" : "false", paths[0].string().c_str(), paths[1].string().c_str(), paths[2].string().c_str(),
                 static_cast<unsigned long long>(results[0].first_source_cts),
                 static_cast<unsigned long long>(results[1].first_source_cts),
                 static_cast<unsigned long long>(results[2].first_source_cts),
                 static_cast<unsigned long long>(results[0].last_source_cts),
                 static_cast<unsigned long long>(results[1].last_source_cts),
                 static_cast<unsigned long long>(results[2].last_source_cts));
        }

        for (StreamResources &stream : streams) {
            ReleaseStream(stream);
        }
    }

    blog(LOG_INFO,
         "[three-stream-poc] complete invariant=common-source-cts-across-master-scene-a-scene-b "
         "capture_layer=bounded-packet-histories consumers=packet-only-mkv-poc replay_consumer=not-implemented");
}

} // namespace obs_sync_replay
