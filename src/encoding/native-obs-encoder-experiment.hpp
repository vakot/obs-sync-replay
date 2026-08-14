#pragma once

#include "timeline/master-frame.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

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

    // Called by the existing MasterFrameCoordinator on the OBS graphics thread
    // while the explicitly activated experiment is running. Packet callbacks use
    // this table to prove CTS-to-master association.
    void ObserveMasterFrame(const MasterFrame& frame);

    enum class Output : uint8_t { A = 0, B = 1 };

  private:
    struct OutputState final {
        NativeObsEncoderExperiment* experiment = nullptr;
        Output output = Output::A;
    };

    struct PacketObservation final {
        bool seen = false;
        int64_t packet_pts = 0;
        int64_t packet_dts = 0;
        bool keyframe = false;
        uint64_t cts = 0;
    };

    struct PacketPair final {
        std::array<PacketObservation, 2> outputs{};
    };

    struct CanonicalFrame final {
        MasterFrameId frame_id = 0;
        MasterFramePts pts_ns = 0;
    };

    static void PacketCallback(obs_output_t* output, struct encoder_packet* packet,
                               struct encoder_packet_time* packet_time, void* parameter);
    void ObservePacket(Output output, const struct encoder_packet& packet,
                       const struct encoder_packet_time* packet_time);

    bool CreateView(Output output, const std::string& scene_name);
    bool CreateEncoderAndOutput(Output output);
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
    std::unordered_map<MasterFramePts, CanonicalFrame> master_frames_by_pts_;
    std::map<MasterFrameId, PacketPair> packet_pairs_;
    bool running_ = false;
};

} // namespace obs_sync_replay
