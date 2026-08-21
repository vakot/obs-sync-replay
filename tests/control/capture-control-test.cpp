#include "control/capture-control.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace obs_sync_replay;

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

PacketStreamConfig StreamConfig() {
    PacketStreamConfig config;
    config.width = 1920;
    config.height = 1080;
    config.timebase_num = 1;
    config.timebase_den = 60000;
    return config;
}

EncodedPacket Packet(const uint64_t source_cts, const bool keyframe = false) {
    EncodedPacket packet;
    packet.source_cts = source_cts;
    packet.pts = static_cast<int64_t>(source_cts / 1'000'000);
    packet.dts = packet.pts;
    packet.timebase_num = 1;
    packet.timebase_den = 60000;
    packet.keyframe = keyframe;
    packet.payload = {0x00, 0x00, 0x01, static_cast<uint8_t>(keyframe ? 0x65 : 0x41)};
    return packet;
}

CaptureConfiguration Configuration(const StreamParticipationMode master, const StreamParticipationMode scene_a,
                                   const StreamParticipationMode scene_b) {
    CaptureConfiguration configuration;
    configuration.streams = {{StreamIdentity::Master, "master", master, StreamConfig()},
                             {StreamIdentity::SceneA, "scene_a", scene_a, StreamConfig()},
                             {StreamIdentity::SceneB, "scene_b", scene_b, StreamConfig()}};
    return configuration;
}

std::vector<std::filesystem::path> Paths(const char* const stem, const size_t count) {
    std::vector<std::filesystem::path> paths;
    for (size_t index = 0; index < count; ++index) {
        paths.push_back(std::filesystem::temp_directory_path() /
                        (std::string(stem) + "-" + std::to_string(index) + ".mkv"));
    }
    return paths;
}

class FakeEncoderController final : public EncoderController {
  public:
    bool Acquire(const CaptureStreamId stream_id, const ConfiguredStream&) override {
        return active_.insert(stream_id).second;
    }

    void Release(const CaptureStreamId stream_id, const ConfiguredStream&) noexcept override {
        active_.erase(stream_id);
    }

    bool IsActive(const CaptureStreamId stream_id) const noexcept override {
        return active_.find(stream_id) != active_.end();
    }

    size_t active_count() const noexcept override {
        return active_.size();
    }

    std::set<CaptureStreamId> active_;
};

void TestModeSemantics() {
    Require(!StreamParticipates(StreamParticipationMode::Disabled, CaptureConsumer::Recording),
            "disabled must not record");
    Require(!StreamParticipates(StreamParticipationMode::Disabled, CaptureConsumer::Replay),
            "disabled must not replay");
    Require(StreamParticipates(StreamParticipationMode::Recording, CaptureConsumer::Recording),
            "recording mode must record");
    Require(!StreamParticipates(StreamParticipationMode::Recording, CaptureConsumer::Replay),
            "recording mode must not replay");
    Require(StreamNeedsEncoder(StreamParticipationMode::Both, true, true), "both needs one encoder");
    Require(!StreamNeedsEncoder(StreamParticipationMode::Replay, true, false), "replay-only is idle without replay");
    Require(StreamNeedsEncoder(StreamParticipationMode::Replay, true, true), "replay-only needs replay encoder");
}

void TestHandoffAndEncoderOwnership() {
    CaptureConfiguration configuration = Configuration(StreamParticipationMode::Both, StreamParticipationMode::Both,
                                                        StreamParticipationMode::Both);
    SynchronizedCaptureConfig capture_config;
    capture_config.ring_capacity_bytes = 1024 * 1024;
    SynchronizedCaptureSession capture(capture_config);
    FakeEncoderController encoders;
    std::vector<EncoderLifecycleEvent> events;
    CaptureControlEngine engine(configuration, capture, encoders,
                                [&events](const EncoderLifecycleDiagnostic& diagnostic) {
                                    events.push_back(diagnostic.event);
                                });
    Require(engine.Initialize(), "initialize all-both engine");

    Require(engine.StartReplay().ok(), "start replay first");
    Require(engine.active_encoder_count() == 3, "replay-only must use three encoders");
    const size_t replay_epoch = engine.capture_epoch();
    Require(engine.StartRecording(Paths("control-handoff-recording", 3)).ok(), "attach recording to replay");
    Require(engine.active_encoder_count() == 3, "recording handoff must retain three encoders");
    Require(engine.StopRecording().ok(), "stop recording while replay continues");
    Require(engine.active_encoder_count() == 3 && engine.replay_state() == ReplayConsumerState::Running,
            "replay must continue after recording stop");
    Require(engine.StopReplay().ok(), "stop replay");
    Require(engine.active_encoder_count() == 0 && engine.capture_state() == CaptureInfrastructureState::Idle,
            "all consumers off must release encoders and capture");
    Require(engine.StartRecording(Paths("control-inverse-recording", 3)).ok(), "start recording first");
    Require(engine.StartReplay().ok(), "attach replay to recording");
    Require(engine.active_encoder_count() == 3, "inverse handoff must use three encoders");
    Require(engine.StopReplay().ok() && engine.active_encoder_count() == 3,
            "recording must survive replay stop");
    Require(engine.StopRecording().ok() && engine.capture_epoch() == replay_epoch + 1,
            "total idle must create a fresh capture epoch");
    Require(!events.empty(), "encoder lifecycle diagnostics must be emitted");
}

void TestMixedModesAndInvalidCommands() {
    CaptureConfiguration configuration = Configuration(StreamParticipationMode::Both,
                                                        StreamParticipationMode::Recording,
                                                        StreamParticipationMode::Replay);
    SynchronizedCaptureSession capture;
    FakeEncoderController encoders;
    CaptureControlEngine engine(configuration, capture, encoders);
    Require(engine.Initialize(), "initialize mixed engine");
    Require(engine.SaveReplay(Paths("invalid-save", 1)).status == ControlCommandStatus::InvalidState,
            "save while replay is off must be invalid");
    Require(engine.StartRecording(Paths("mixed-recording", 2)).ok(), "start mixed recording");
    Require(engine.active_encoder_count() == 2, "mixed recording must use master and scene A");
    Require(capture.Ingest(0, Packet(100, true)) && capture.Ingest(1, Packet(100, true)),
            "mixed recording packets must be accepted");
    Require(capture.metrics().retained_bytes == 0, "recording-only capture must not retain replay packets");
    Require(engine.StartReplay().ok(), "start mixed replay");
    Require(engine.active_encoder_count() == 3, "mixed replay must add scene B only");
    Require(engine.StartReplay().status == ControlCommandStatus::NoOp, "start replay must be idempotent");
    Require(engine.StopReplay().ok() && engine.active_encoder_count() == 2,
            "mixed replay stop must retain recording encoders");
    Require(engine.StopRecording().ok() && engine.active_encoder_count() == 0,
            "mixed recording stop must release remaining encoders");

    CaptureConfiguration disabled = Configuration(StreamParticipationMode::Both, StreamParticipationMode::Disabled,
                                                  StreamParticipationMode::Replay);
    SynchronizedCaptureSession disabled_capture;
    FakeEncoderController disabled_encoders;
    CaptureControlEngine disabled_engine(disabled, disabled_capture, disabled_encoders);
    Require(disabled_engine.Initialize(), "initialize disabled engine");
    Require(disabled_engine.StartRecording(Paths("disabled-recording", 1)).ok(), "start disabled test recording");
    Require(disabled_engine.active_encoder_count() == 1, "disabled stream must never start an encoder");
    Require(disabled_engine.StopRecording().ok(), "stop disabled test recording");
}

} // namespace

int main() {
    TestModeSemantics();
    TestHandoffAndEncoderOwnership();
    TestMixedModesAndInvalidCommands();
    return EXIT_SUCCESS;
}
