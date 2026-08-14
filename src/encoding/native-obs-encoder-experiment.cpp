#include "encoding/native-obs-encoder-experiment.hpp"

#include <obs-module.h>

#include <cstdlib>
#include <utility>

namespace obs_sync_replay {

namespace {

constexpr char kNullOutputId[] = "null_output";
constexpr char kDefaultEncoderId[] = "obs_x264";
constexpr char kActivationAudioEncoderId[] = "ffmpeg_aac";

size_t OutputIndex(const NativeObsEncoderExperiment::Output output) {
    return output == NativeObsEncoderExperiment::Output::A ? 0 : 1;
}

} // namespace

NativeObsEncoderExperiment::NativeObsEncoderExperiment(std::string scene_a_name,
                                                       std::string scene_b_name)
    : scene_a_name_(std::move(scene_a_name)), scene_b_name_(std::move(scene_b_name)) {
    output_states_[0] = {this, Output::A};
    output_states_[1] = {this, Output::B};
}

NativeObsEncoderExperiment::~NativeObsEncoderExperiment() {
    Stop();
}

bool NativeObsEncoderExperiment::Start() {
    if (running_) {
        blog(LOG_WARNING, "[native-encoder-experiment] start ignored: already running");
        return true;
    }

    encoder_id_ = EncoderId();
    blog(LOG_INFO, "[native-encoder-experiment] explicitly activated encoder_id=%s",
         encoder_id_.c_str());
    if (!CreateView(Output::A, scene_a_name_) || !CreateView(Output::B, scene_b_name_)) {
        ReleaseResources();
        return false;
    }

    encoder_group_ = obs_encoder_group_create();
    if (!encoder_group_) {
        blog(LOG_ERROR, "[native-encoder-experiment] failed to create public encoder group");
        ReleaseResources();
        return false;
    }

    if (!CreateEncoderAndOutput(Output::A) || !CreateEncoderAndOutput(Output::B)) {
        ReleaseResources();
        return false;
    }

    if (!obs_encoder_set_group(encoders_[0], encoder_group_) ||
        !obs_encoder_set_group(encoders_[1], encoder_group_)) {
        blog(LOG_ERROR, "[native-encoder-experiment] failed to group native encoders");
        ReleaseResources();
        return false;
    }

    running_ = true;
    if (!obs_output_start(outputs_[0]) || !obs_output_start(outputs_[1])) {
        blog(LOG_ERROR,
             "[native-encoder-experiment] failed to start output encoder_id=%s output_a_error=%s "
             "output_b_error=%s",
             encoder_id_.c_str(), obs_output_get_last_error(outputs_[0]),
             obs_output_get_last_error(outputs_[1]));
        ReleaseResources();
        return false;
    }

    blog(LOG_INFO,
         "[native-encoder-experiment] started encoder_id=%s outputs=null_output views=2 "
         "invariant=2,3,4 CTS mapping required",
         encoder_id_.c_str());
    return true;
}

void NativeObsEncoderExperiment::Stop() {
    ReleaseResources();
}

void NativeObsEncoderExperiment::ObserveMasterFrame(const MasterFrame& frame) {
    std::lock_guard<std::mutex> lock(observations_mutex_);
    if (!running_) {
        return;
    }

    const auto inserted = master_frames_by_pts_.emplace(
        frame.pts_ns(), CanonicalFrame{frame.frame_id(), frame.pts_ns()});
    if (!inserted.second) {
        blog(LOG_ERROR,
             "[native-encoder-experiment] invariant=3 duplicate master PTS master_frame_id=%llu "
             "master_pts=%llu",
             static_cast<unsigned long long>(frame.frame_id()),
             static_cast<unsigned long long>(frame.pts_ns()));
    }
}

void NativeObsEncoderExperiment::PacketCallback(obs_output_t*, struct encoder_packet* packet,
                                                struct encoder_packet_time* packet_time,
                                                void* parameter) {
    auto* state = static_cast<OutputState*>(parameter);
    if (!state || !state->experiment || !packet || packet->type != OBS_ENCODER_VIDEO) {
        return;
    }

    state->experiment->ObservePacket(state->output, *packet, packet_time);
}

void NativeObsEncoderExperiment::ObservePacket(
    const Output output, const struct encoder_packet& packet,
    const struct encoder_packet_time* const packet_time) {
    std::lock_guard<std::mutex> lock(observations_mutex_);
    if (!running_) {
        return;
    }
    if (!packet_time) {
        blog(LOG_ERROR,
             "[native-encoder-experiment] invariant=8 output=%s packet_pts=%lld missing packet "
             "timing",
             OutputName(output), static_cast<long long>(packet.pts));
        return;
    }
    if (packet.pts != packet_time->pts) {
        blog(LOG_ERROR,
             "[native-encoder-experiment] invariant=8 output=%s packet_pts=%lld timing_pts=%lld "
             "CTS=%llu mismatch",
             OutputName(output), static_cast<long long>(packet.pts),
             static_cast<long long>(packet_time->pts),
             static_cast<unsigned long long>(packet_time->cts));
        return;
    }

    const auto master = master_frames_by_pts_.find(packet_time->cts);
    if (master == master_frames_by_pts_.end()) {
        blog(LOG_ERROR,
             "[native-encoder-experiment] invariant=3 output=%s packet_pts=%lld CTS=%llu has no "
             "accepted master frame",
             OutputName(output), static_cast<long long>(packet.pts),
             static_cast<unsigned long long>(packet_time->cts));
        return;
    }
    if (master->second.pts_ns != packet_time->cts) {
        blog(LOG_ERROR,
             "[native-encoder-experiment] invariant=3 output=%s master_frame_id=%llu "
             "master_pts=%llu CTS=%llu mismatch",
             OutputName(output), static_cast<unsigned long long>(master->second.frame_id),
             static_cast<unsigned long long>(master->second.pts_ns),
             static_cast<unsigned long long>(packet_time->cts));
        return;
    }

    PacketPair& pair = packet_pairs_[master->second.frame_id];
    PacketObservation& observation = pair.outputs[OutputIndex(output)];
    if (observation.seen) {
        blog(LOG_ERROR,
             "[native-encoder-experiment] invariant=8 output=%s master_frame_id=%llu duplicate "
             "packet observation",
             OutputName(output), static_cast<unsigned long long>(master->second.frame_id));
        return;
    }

    observation = {true, packet.pts, packet.dts, packet.keyframe, packet_time->cts};
    const PacketObservation& a = pair.outputs[0];
    const PacketObservation& b = pair.outputs[1];
    if (!a.seen || !b.seen) {
        return;
    }

    if (a.packet_pts != b.packet_pts || a.cts != b.cts || a.cts != master->second.pts_ns) {
        blog(LOG_ERROR,
             "[native-encoder-experiment] invariant=4 failed master_frame_id=%llu master_pts=%llu "
             "a_pts=%lld b_pts=%lld a_cts=%llu b_cts=%llu",
             static_cast<unsigned long long>(master->second.frame_id),
             static_cast<unsigned long long>(master->second.pts_ns),
             static_cast<long long>(a.packet_pts), static_cast<long long>(b.packet_pts),
             static_cast<unsigned long long>(a.cts), static_cast<unsigned long long>(b.cts));
        return;
    }

    const bool sampled = master->second.frame_id < 3 || master->second.frame_id % 300 == 0;
    blog(sampled ? LOG_INFO : LOG_DEBUG,
         "[native-encoder-experiment] master_frame_id=%llu master_pts=%llu encoder_pts=%lld "
         "a_dts=%lld b_dts=%lld a_keyframe=%d b_keyframe=%d validation=ok",
         static_cast<unsigned long long>(master->second.frame_id),
         static_cast<unsigned long long>(master->second.pts_ns),
         static_cast<long long>(a.packet_pts), static_cast<long long>(a.packet_dts),
         static_cast<long long>(b.packet_dts), a.keyframe ? 1 : 0, b.keyframe ? 1 : 0);
    packet_pairs_.erase(master->second.frame_id);
}

bool NativeObsEncoderExperiment::CreateView(const Output output, const std::string& scene_name) {
    const size_t index = OutputIndex(output);
    views_[index] = obs_view_create();
    if (!views_[index]) {
        blog(LOG_ERROR, "[native-encoder-experiment] failed to create view output=%s",
             OutputName(output));
        return false;
    }

    obs_source_t* scene = obs_get_source_by_name(scene_name.c_str());
    if (!scene || !obs_scene_from_source(scene)) {
        blog(LOG_ERROR,
             "[native-encoder-experiment] configured scene unavailable output=%s scene=%s",
             OutputName(output), scene_name.c_str());
        obs_source_release(scene);
        return false;
    }

    // The view retains its own source reference. This makes Program switching
    // irrelevant to either independent experimental output.
    obs_view_set_source(views_[index], 0, scene);
    obs_source_release(scene);

    videos_[index] = obs_view_add(views_[index]);
    if (!videos_[index]) {
        blog(LOG_ERROR,
             "[native-encoder-experiment] failed to add view to OBS video loop output=%s",
             OutputName(output));
        return false;
    }
    return true;
}

bool NativeObsEncoderExperiment::CreateEncoderAndOutput(const Output output) {
    const size_t index = OutputIndex(output);
    const char* const name =
        output == Output::A ? "sync_replay_native_encoder_a" : "sync_replay_native_encoder_b";
    encoders_[index] = obs_video_encoder_create(encoder_id_.c_str(), name, nullptr, nullptr);
    if (!encoders_[index]) {
        blog(LOG_ERROR, "[native-encoder-experiment] unavailable encoder output=%s encoder_id=%s",
             OutputName(output), encoder_id_.c_str());
        return false;
    }
    obs_encoder_set_video(encoders_[index], videos_[index]);

    // OBS's shipped null_output is AV-only. This stock encoder exists only to
    // activate that public output lifecycle; the experiment never retains,
    // inspects, or writes audio.
    const char* const audio_name = output == Output::A ? "sync_replay_native_activation_audio_a"
                                                       : "sync_replay_native_activation_audio_b";
    activation_audio_encoders_[index] =
        obs_audio_encoder_create(kActivationAudioEncoderId, audio_name, nullptr, 0, nullptr);
    if (!activation_audio_encoders_[index]) {
        blog(LOG_ERROR,
             "[native-encoder-experiment] required activation audio encoder is unavailable "
             "output=%s",
             OutputName(output));
        return false;
    }
    obs_encoder_set_audio(activation_audio_encoders_[index], obs_get_audio());

    const char* const output_name =
        output == Output::A ? "sync_replay_native_output_a" : "sync_replay_native_output_b";
    outputs_[index] = obs_output_create(kNullOutputId, output_name, nullptr, nullptr);
    if (!outputs_[index]) {
        blog(LOG_ERROR, "[native-encoder-experiment] required null_output is unavailable output=%s",
             OutputName(output));
        return false;
    }
    obs_output_set_video_encoder(outputs_[index], encoders_[index]);
    obs_output_set_audio_encoder(outputs_[index], activation_audio_encoders_[index], 0);
    obs_output_add_packet_callback(outputs_[index], PacketCallback, &output_states_[index]);
    return true;
}

void NativeObsEncoderExperiment::ReleaseResources() {
    bool was_running = false;
    {
        std::lock_guard<std::mutex> lock(observations_mutex_);
        was_running = running_;
        running_ = false;
    }

    for (size_t index = 0; index < outputs_.size(); ++index) {
        if (outputs_[index]) {
            obs_output_remove_packet_callback(outputs_[index], PacketCallback,
                                              &output_states_[index]);
            obs_output_stop(outputs_[index]);
            obs_output_release(outputs_[index]);
            outputs_[index] = nullptr;
        }
    }
    if (encoder_group_) {
        obs_encoder_group_destroy(encoder_group_);
        encoder_group_ = nullptr;
    }
    for (obs_encoder_t*& encoder : encoders_) {
        if (encoder) {
            obs_encoder_release(encoder);
            encoder = nullptr;
        }
    }
    for (obs_encoder_t*& encoder : activation_audio_encoders_) {
        if (encoder) {
            obs_encoder_release(encoder);
            encoder = nullptr;
        }
    }
    for (obs_view_t*& view : views_) {
        if (view) {
            obs_view_remove(view);
            obs_view_destroy(view);
            view = nullptr;
        }
    }
    videos_.fill(nullptr);

    std::lock_guard<std::mutex> lock(observations_mutex_);
    master_frames_by_pts_.clear();
    packet_pairs_.clear();

    if (was_running) {
        blog(LOG_INFO, "[native-encoder-experiment] stopped");
    }
}

std::string NativeObsEncoderExperiment::EncoderId() const {
    char* requested = nullptr;
    size_t requested_length = 0;
    if (_dupenv_s(&requested, &requested_length, "OBS_SYNC_REPLAY_EXPERIMENT_ENCODER_ID") == 0 &&
        requested_length > 1) {
        std::string encoder_id(requested);
        std::free(requested);
        return encoder_id;
    }
    std::free(requested);
    return kDefaultEncoderId;
}

const char* NativeObsEncoderExperiment::OutputName(const Output output) noexcept {
    return output == Output::A ? "A" : "B";
}

} // namespace obs_sync_replay
