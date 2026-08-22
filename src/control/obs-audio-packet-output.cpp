#include "control/obs-audio-packet-output.hpp"

#include <obs-module.h>

namespace obs_sync_replay {

namespace {

struct AudioPacketOutputContext final {
    obs_output_t *output = nullptr;
};

const char *GetName(void *) {
    return "OBS Sync Replay Packet Output";
}

void *Create(obs_data_t *, obs_output_t *output) {
    return new AudioPacketOutputContext{output};
}

void Destroy(void *data) {
    delete static_cast<AudioPacketOutputContext *>(data);
}

bool Start(void *data) {
    auto *context = static_cast<AudioPacketOutputContext *>(data);
    if (!context || !obs_output_can_begin_data_capture(context->output, 0) ||
        !obs_output_initialize_encoders(context->output, 0)) {
        return false;
    }
    return obs_output_begin_data_capture(context->output, 0);
}

void Stop(void *data, uint64_t) {
    auto *context = static_cast<AudioPacketOutputContext *>(data);
    if (context) {
        obs_output_end_data_capture(context->output);
    }
}

void EncodedPacket(void *, struct encoder_packet *) {}

obs_output_info MakePacketOutputInfo() {
    obs_output_info info{};
    info.id = kPacketOutputId;
    info.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_MULTI_TRACK_AV;
    info.get_name = GetName;
    info.create = Create;
    info.destroy = Destroy;
    info.start = Start;
    info.stop = Stop;
    info.encoded_packet = EncodedPacket;
    return info;
}

} // namespace

void RegisterPacketOutputs() {
    static const obs_output_info kPacketOutputInfo = MakePacketOutputInfo();
    obs_register_output(&kPacketOutputInfo);
}

} // namespace obs_sync_replay
