#pragma once

#include "timeline/logical-video-slot-timeline.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct encoder_packet;
struct encoder_packet_time;
struct obs_encoder;
struct obs_encoder_group;
struct obs_output;
struct obs_view;
struct video_output;

typedef struct obs_encoder obs_encoder_t;
typedef struct obs_encoder_group obs_encoder_group_t;
typedef struct obs_output obs_output_t;
typedef struct obs_view obs_view_t;
typedef struct video_output video_t;

namespace obs_sync_replay {

// Experimental only: proves the public libobs view -> video_t -> output ->
// obs_encoder_t path. It owns no replay state, file output, or muxer.
class NativeObsEncoderExperiment final {
  public:
    NativeObsEncoderExperiment(std::string scene_a_name, std::string scene_b_name);
    ~NativeObsEncoderExperiment();

    NativeObsEncoderExperiment(const NativeObsEncoderExperiment&) = delete;
    NativeObsEncoderExperiment& operator=(const NativeObsEncoderExperiment&) = delete;

    bool Start();
    void Stop();

    // Called for rendered graphics observations. The experiment expands OBS-owned
    // repeated logical slots before joining public packet CTS values.
    void ObserveMasterFrame(const MasterFrame& frame);

    enum class Output : uint8_t { A = 0, B = 1 };

  private:
    struct OutputState final {
        NativeObsEncoderExperiment* experiment = nullptr;
        Output output = Output::A;
    };

    struct PacketObservation final {
        int64_t packet_pts = 0;
        int64_t packet_dts = 0;
        bool keyframe = false;
        uint64_t cts = 0;
        size_t observation_index = 0;
        bool has_previous_packet = false;
        int64_t previous_packet_pts = 0;
        uint64_t previous_cts = 0;
        uint64_t input_id = 0;
        uint64_t association_id = 0;
        bool has_association = false;
    };

    struct CanonicalLogicalSlot final {
        detail::LogicalVideoSlotId slot_id = 0;
        MasterFramePts pts_ns = 0;
        MasterFrameId rendered_frame_id = 0;
        MasterFramePts rendered_pts_ns = 0;
        detail::LogicalVideoSlotDisposition disposition =
            detail::LogicalVideoSlotDisposition::Rendered;
    };

    struct PacketSet final {
        CanonicalLogicalSlot canonical;
        std::array<std::vector<PacketObservation>, 2> outputs{};
        bool single_packet_validation_logged = false;
        bool single_packet_validation_failed_logged = false;
    };

    static void PacketCallback(obs_output_t* output, struct encoder_packet* packet,
                               struct encoder_packet_time* packet_time, void* parameter);
    static bool InputAssociationCallback(obs_encoder_t* encoder, uint64_t input_id,
                                         uint64_t* association_id, void* parameter);
    bool AssociateInput(Output output, uint64_t input_id, uint64_t& association_id);
    void ObservePacket(Output output, const struct encoder_packet& packet,
                       const struct encoder_packet_time* packet_time);

    bool CreateView(Output output, const std::string& scene_name);
    bool CreateEncoderAndOutput(Output output);
    void LogPacketSet(const PacketSet& packet_set, const char* reason) const;
    void TrimObservationWindows();
    void ReleaseResources();
    std::string EncoderId() const;
    static const char* OutputName(Output output) noexcept;

    std::string scene_a_name_;
    std::string scene_b_name_;
    std::string encoder_id_;
    std::array<obs_view_t*, 2> views_{};
    std::array<video_t*, 2> videos_{};
    std::array<obs_encoder_t*, 2> encoders_{};
    std::array<obs_encoder_t*, 2> activation_audio_encoders_{};
    std::array<obs_output_t*, 2> outputs_{};
    std::array<OutputState, 2> output_states_{};
    obs_encoder_group_t* encoder_group_ = nullptr;

    std::mutex observations_mutex_;
    detail::LogicalVideoSlotTimeline logical_slot_timeline_;
    std::map<MasterFramePts, CanonicalLogicalSlot> logical_slots_by_pts_;
    std::map<detail::LogicalVideoSlotId, CanonicalLogicalSlot> logical_slots_by_id_;
    std::map<detail::LogicalVideoSlotId, PacketSet> packet_sets_;
    std::array<PacketObservation, 2> previous_packets_{};
    std::array<bool, 2> has_previous_packets_{};
    bool input_slot_origin_set_ = false;
    uint64_t input_slot_origin_ = 0;
    detail::LogicalVideoSlotId logical_slot_origin_ = 0;
    bool running_ = false;
};

} // namespace obs_sync_replay
