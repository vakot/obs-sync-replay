#include "encoding/native-obs-encoder-experiment.hpp"

#include <obs-module.h>

#include <cstdlib>
#include <sstream>
#include <utility>

namespace obs_sync_replay {

namespace {

constexpr char kNullOutputId[] = "null_output";
constexpr char kDefaultEncoderId[] = "obs_x264";
constexpr char kActivationAudioEncoderId[] = "ffmpeg_aac";
constexpr size_t kObservationWindowCapacity = 4096;

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

    std::vector<detail::LogicalVideoSlot> observed_slots;
    const detail::LogicalVideoSlotObservationResult result =
        logical_slot_timeline_.ObserveRenderedFrame(frame, obs_get_frame_interval_ns(),
                                                    observed_slots);
    if (result != detail::LogicalVideoSlotObservationResult::Accepted) {
        const char* const reason =
            result == detail::LogicalVideoSlotObservationResult::InvalidFrameInterval
                ? "invalid frame interval"
            : result == detail::LogicalVideoSlotObservationResult::NonMonotonicRenderedPts
                ? "non-monotonic rendered PTS"
                : "unaligned rendered PTS";
        blog(LOG_ERROR,
             "[native-encoder-experiment] logical-slot expansion rejected rendered_frame_id=%llu "
             "rendered_pts=%llu reason=%s",
             static_cast<unsigned long long>(frame.frame_id()),
             static_cast<unsigned long long>(frame.pts_ns()), reason);
        return;
    }

    for (const detail::LogicalVideoSlot& slot : observed_slots) {
        const CanonicalLogicalSlot canonical{slot.slot_id, slot.pts_ns, slot.rendered_frame_id,
                                             slot.rendered_pts_ns, slot.disposition};
        const auto inserted = logical_slots_by_pts_.emplace(slot.pts_ns, canonical);
        if (!inserted.second) {
            blog(LOG_ERROR,
                 "[native-encoder-experiment] logical-slot duplicate slot_pts=%llu slot_id=%llu",
                 static_cast<unsigned long long>(slot.pts_ns),
                 static_cast<unsigned long long>(slot.slot_id));
            continue;
        }
        if (slot.disposition == detail::LogicalVideoSlotDisposition::Repeated) {
            blog(LOG_WARNING,
                 "[native-encoder-experiment] OBS logical repeated slot slot_id=%llu slot_pts=%llu "
                 "rendered_frame_id=%llu rendered_pts=%llu",
                 static_cast<unsigned long long>(slot.slot_id),
                 static_cast<unsigned long long>(slot.pts_ns),
                 static_cast<unsigned long long>(slot.rendered_frame_id),
                 static_cast<unsigned long long>(slot.rendered_pts_ns));
        }
    }
    TrimObservationWindows();
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

    const auto logical_slot = logical_slots_by_pts_.find(packet_time->cts);
    if (logical_slot == logical_slots_by_pts_.end()) {
        blog(LOG_ERROR,
             "[native-encoder-experiment] invariant=3 output=%s packet_pts=%lld CTS=%llu has no "
             "known OBS logical slot",
             OutputName(output), static_cast<long long>(packet.pts),
             static_cast<unsigned long long>(packet_time->cts));
        return;
    }
    if (logical_slot->second.pts_ns != packet_time->cts) {
        blog(LOG_ERROR,
             "[native-encoder-experiment] invariant=3 output=%s logical_slot_id=%llu "
             "slot_pts=%llu CTS=%llu mismatch",
             OutputName(output), static_cast<unsigned long long>(logical_slot->second.slot_id),
             static_cast<unsigned long long>(logical_slot->second.pts_ns),
             static_cast<unsigned long long>(packet_time->cts));
        return;
    }

    PacketSet& packet_set =
        packet_sets_.try_emplace(logical_slot->second.slot_id, PacketSet{logical_slot->second})
            .first->second;
    const size_t output_index = OutputIndex(output);
    std::vector<PacketObservation>& observations = packet_set.outputs[output_index];
    const PacketObservation& previous = previous_packets_[output_index];
    const PacketObservation observation{packet.pts,          packet.dts,
                                        packet.keyframe,     packet_time->cts,
                                        observations.size(), has_previous_packets_[output_index],
                                        previous.packet_pts, previous.cts};
    observations.push_back(observation);
    previous_packets_[output_index] = observation;
    has_previous_packets_[output_index] = true;

    blog(LOG_DEBUG,
         "[native-encoder-experiment] packet output=%s packet_pts=%lld packet_dts=%lld CTS=%llu "
         "keyframe=%d logical_slot_id=%llu slot_pts=%llu disposition=%s rendered_frame_id=%llu "
         "rendered_pts=%llu observation_index=%llu "
         "previous_packet_pts=%lld previous_cts=%llu has_previous=%d",
         OutputName(output), static_cast<long long>(observation.packet_pts),
         static_cast<long long>(observation.packet_dts),
         static_cast<unsigned long long>(observation.cts), observation.keyframe ? 1 : 0,
         static_cast<unsigned long long>(packet_set.canonical.slot_id),
         static_cast<unsigned long long>(packet_set.canonical.pts_ns),
         packet_set.canonical.disposition == detail::LogicalVideoSlotDisposition::Repeated
             ? "repeated"
             : "rendered",
         static_cast<unsigned long long>(packet_set.canonical.rendered_frame_id),
         static_cast<unsigned long long>(packet_set.canonical.rendered_pts_ns),
         static_cast<unsigned long long>(observation.observation_index),
         static_cast<long long>(observation.previous_packet_pts),
         static_cast<unsigned long long>(observation.previous_cts),
         observation.has_previous_packet ? 1 : 0);

    const std::vector<PacketObservation>& a_packets = packet_set.outputs[0];
    const std::vector<PacketObservation>& b_packets = packet_set.outputs[1];
    if (a_packets.empty() || b_packets.empty()) {
        if (observations.size() > 1) {
            LogPacketSet(packet_set, "multiple_packet_observations_for_cts");
        }
        return;
    }

    if (a_packets.size() != 1 || b_packets.size() != 1) {
        LogPacketSet(packet_set, "packet_set_contains_multiple_observations");
        return;
    }

    const PacketObservation& a = a_packets.front();
    const PacketObservation& b = b_packets.front();
    if (a.packet_pts != b.packet_pts || a.cts != b.cts || a.cts != packet_set.canonical.pts_ns) {
        if (!packet_set.single_packet_validation_failed_logged) {
            blog(LOG_WARNING,
                 "[native-encoder-experiment] research single-packet CTS set needs investigation "
                 "logical_slot_id=%llu slot_pts=%llu a_pts=%lld b_pts=%lld a_cts=%llu b_cts=%llu; "
                 "raw observations retained",
                 static_cast<unsigned long long>(packet_set.canonical.slot_id),
                 static_cast<unsigned long long>(packet_set.canonical.pts_ns),
                 static_cast<long long>(a.packet_pts), static_cast<long long>(b.packet_pts),
                 static_cast<unsigned long long>(a.cts), static_cast<unsigned long long>(b.cts));
            packet_set.single_packet_validation_failed_logged = true;
        }
        LogPacketSet(packet_set, "single_packet_pts_or_cts_mismatch");
        return;
    }

    if (packet_set.single_packet_validation_logged) {
        return;
    }

    const bool sampled =
        packet_set.canonical.slot_id < 3 || packet_set.canonical.slot_id % 300 == 0 ||
        packet_set.canonical.disposition == detail::LogicalVideoSlotDisposition::Repeated;
    blog(sampled ? LOG_INFO : LOG_DEBUG,
         "[native-encoder-experiment] logical_slot_id=%llu slot_pts=%llu disposition=%s "
         "rendered_frame_id=%llu rendered_pts=%llu encoder_pts=%lld a_dts=%lld b_dts=%lld "
         "a_keyframe=%d b_keyframe=%d single_packet_validation=ok",
         static_cast<unsigned long long>(packet_set.canonical.slot_id),
         static_cast<unsigned long long>(packet_set.canonical.pts_ns),
         packet_set.canonical.disposition == detail::LogicalVideoSlotDisposition::Repeated
             ? "repeated"
             : "rendered",
         static_cast<unsigned long long>(packet_set.canonical.rendered_frame_id),
         static_cast<unsigned long long>(packet_set.canonical.rendered_pts_ns),
         static_cast<long long>(a.packet_pts), static_cast<long long>(a.packet_dts),
         static_cast<long long>(b.packet_dts), a.keyframe ? 1 : 0, b.keyframe ? 1 : 0);
    packet_set.single_packet_validation_logged = true;
}

void NativeObsEncoderExperiment::LogPacketSet(const PacketSet& packet_set,
                                              const char* const reason) const {
    std::ostringstream message;
    message << "[native-encoder-experiment] research packet-set reason=" << reason
            << " logical_slot_id=" << packet_set.canonical.slot_id
            << " slot_pts=" << packet_set.canonical.pts_ns << " disposition="
            << (packet_set.canonical.disposition == detail::LogicalVideoSlotDisposition::Repeated
                    ? "repeated"
                    : "rendered")
            << " rendered_frame_id=" << packet_set.canonical.rendered_frame_id
            << " rendered_pts=" << packet_set.canonical.rendered_pts_ns;

    for (size_t output_index = 0; output_index < packet_set.outputs.size(); ++output_index) {
        message << " output_" << OutputName(static_cast<Output>(output_index)) << "=[";
        const std::vector<PacketObservation>& observations = packet_set.outputs[output_index];
        for (size_t index = 0; index < observations.size(); ++index) {
            const PacketObservation& observation = observations[index];
            if (index != 0) {
                message << ',';
            }
            message << "{index=" << observation.observation_index
                    << " pts=" << observation.packet_pts << " dts=" << observation.packet_dts
                    << " cts=" << observation.cts << " keyframe=" << (observation.keyframe ? 1 : 0)
                    << '}';
        }
        message << ']';
    }
    blog(LOG_WARNING, "%s", message.str().c_str());
}

void NativeObsEncoderExperiment::TrimObservationWindows() {
    while (logical_slots_by_pts_.size() > kObservationWindowCapacity) {
        logical_slots_by_pts_.erase(logical_slots_by_pts_.begin());
    }
    while (packet_sets_.size() > kObservationWindowCapacity) {
        packet_sets_.erase(packet_sets_.begin());
    }
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

    if (was_running) {
        blog(LOG_INFO, "[native-encoder-experiment] stop requested");
    }

    for (size_t index = 0; index < outputs_.size(); ++index) {
        if (outputs_[index]) {
            const char* const output_name = OutputName(static_cast<Output>(index));
            blog(LOG_INFO,
                 "[native-encoder-experiment] packet callback removal requested output=%s",
                 output_name);
            obs_output_remove_packet_callback(outputs_[index], PacketCallback,
                                              &output_states_[index]);
            blog(LOG_INFO, "[native-encoder-experiment] packet callback removed output=%s",
                 output_name);
        }
    }

    // Both grouped encoders receive their stop request before either output is
    // released. Releasing the first output first can wait on the second group's
    // active encoder during its capture-thread shutdown.
    for (size_t index = 0; index < outputs_.size(); ++index) {
        if (outputs_[index]) {
            const char* const output_name = OutputName(static_cast<Output>(index));
            blog(LOG_INFO, "[native-encoder-experiment] output stop requested output=%s",
                 output_name);
            obs_output_stop(outputs_[index]);
        }
    }

    for (size_t index = 0; index < outputs_.size(); ++index) {
        if (outputs_[index]) {
            const char* const output_name = OutputName(static_cast<Output>(index));

            // obs_output_release joins each capture thread only after both
            // grouped outputs have been told to stop. No encoder, audio, or view
            // resource is released first.
            obs_output_release(outputs_[index]);
            outputs_[index] = nullptr;
            blog(LOG_INFO, "[native-encoder-experiment] output stop complete output=%s",
                 output_name);
        }
    }
    if (encoder_group_) {
        blog(LOG_INFO, "[native-encoder-experiment] encoder group destroy requested");
        obs_encoder_group_destroy(encoder_group_);
        encoder_group_ = nullptr;
        blog(LOG_INFO, "[native-encoder-experiment] encoder group destroy complete");
    }
    for (size_t index = 0; index < encoders_.size(); ++index) {
        obs_encoder_t*& encoder = encoders_[index];
        if (encoder) {
            blog(LOG_INFO, "[native-encoder-experiment] video encoder release requested output=%s",
                 OutputName(static_cast<Output>(index)));
            obs_encoder_release(encoder);
            encoder = nullptr;
            blog(LOG_INFO, "[native-encoder-experiment] video encoder release complete output=%s",
                 OutputName(static_cast<Output>(index)));
        }
    }
    for (size_t index = 0; index < activation_audio_encoders_.size(); ++index) {
        obs_encoder_t*& encoder = activation_audio_encoders_[index];
        if (encoder) {
            blog(LOG_INFO,
                 "[native-encoder-experiment] activation audio encoder release requested "
                 "output=%s",
                 OutputName(static_cast<Output>(index)));
            obs_encoder_release(encoder);
            encoder = nullptr;
            blog(LOG_INFO,
                 "[native-encoder-experiment] activation audio encoder release complete "
                 "output=%s",
                 OutputName(static_cast<Output>(index)));
        }
    }
    for (size_t index = 0; index < views_.size(); ++index) {
        obs_view_t*& view = views_[index];
        if (view) {
            blog(LOG_INFO, "[native-encoder-experiment] view remove requested output=%s",
                 OutputName(static_cast<Output>(index)));
            obs_view_remove(view);
            blog(LOG_INFO, "[native-encoder-experiment] view remove complete output=%s",
                 OutputName(static_cast<Output>(index)));
            blog(LOG_INFO, "[native-encoder-experiment] view destroy requested output=%s",
                 OutputName(static_cast<Output>(index)));
            obs_view_destroy(view);
            view = nullptr;
            blog(LOG_INFO, "[native-encoder-experiment] view destroy complete output=%s",
                 OutputName(static_cast<Output>(index)));
        }
    }
    videos_.fill(nullptr);

    std::lock_guard<std::mutex> lock(observations_mutex_);
    logical_slot_timeline_.Reset();
    logical_slots_by_pts_.clear();
    packet_sets_.clear();
    previous_packets_.fill({});
    has_previous_packets_.fill(false);

    if (was_running) {
        blog(LOG_INFO, "[native-encoder-experiment] stop complete");
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
