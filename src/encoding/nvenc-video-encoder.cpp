#include "encoding/nvenc-video-encoder.hpp"

#include <graphics/graphics.h>

#include <d3d11.h>
#include <ffnvcodec/nvEncodeAPI.h>
#include <windows.h>

#include <array>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace obs_sync_replay {
namespace {
constexpr uint32_t kDevelopmentBitrateBitsPerSecond = 16000000;
constexpr uint32_t kDevelopmentGopFrames = 120;
constexpr uint32_t kDevelopmentFrameRate = 60;
constexpr size_t kInFlightSlots = 6;
using NvEncodeApiCreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
bool NvencSucceeded(const NVENCSTATUS status) noexcept { return status == NV_ENC_SUCCESS; }
} // namespace

struct NvencVideoEncoder::State final {
    struct Slot final {
        ID3D11Texture2D* input_texture = nullptr;
        NV_ENC_REGISTERED_PTR registered_resource = nullptr;
        NV_ENC_INPUT_PTR mapped_resource = nullptr;
        NV_ENC_OUTPUT_PTR bitstream = nullptr;
        HANDLE completion_event = nullptr;
        std::optional<MasterFrame> master_frame;
        int64_t encoder_pts = 0;
        bool in_flight = false;
    };
    HMODULE library = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api{};
    void* session = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    std::array<Slot, kInFlightSlots> slots{};
    std::mutex slots_mutex;
    std::condition_variable submitted;
    std::deque<size_t> submission_order;
    bool stopping = false;
    std::thread completion_thread;
};

NvencVideoEncoder::NvencVideoEncoder(const OutputSlot output, PacketCallback packet_callback,
                                     FailureCallback failure_callback)
    : state_(std::make_unique<State>()), output_(output), packet_callback_(std::move(packet_callback)),
      failure_callback_(std::move(failure_callback)) {}
NvencVideoEncoder::~NvencVideoEncoder() { Shutdown(); }

VideoEncoderSubmitResult NvencVideoEncoder::Prepare(const RetainedGpuFrame& frame) {
    if (frame.output() != output_ || !frame.texture()) { last_error_ = "invalid NVENC submission"; return VideoEncoderSubmitResult::Failed; }
    if (frame.color_format() != static_cast<uint32_t>(GS_RGBA)) { last_error_ = "Phase 4 direct NVENC accepts retained GS_RGBA textures only"; return VideoEncoderSubmitResult::Failed; }
    if (!EnsureInitialized(frame)) return VideoEncoderSubmitResult::Failed;
    std::lock_guard<std::mutex> lock(state_->slots_mutex);
    for (const State::Slot& slot : state_->slots) if (!slot.in_flight) return VideoEncoderSubmitResult::Submitted;
    return VideoEncoderSubmitResult::Capacity;
}

VideoEncoderSubmitResult NvencVideoEncoder::Submit(const RetainedGpuFrame& frame, const int64_t encoder_pts) {
    return state_->session && SubmitAsync(frame, encoder_pts) ? VideoEncoderSubmitResult::Submitted : VideoEncoderSubmitResult::Failed;
}

void NvencVideoEncoder::Shutdown() noexcept {
    if (state_->completion_thread.joinable()) {
        { std::lock_guard<std::mutex> lock(state_->slots_mutex); state_->stopping = true; }
        state_->submitted.notify_all();
        state_->completion_thread.join();
    }
    ReleaseReusableResources();
    if (state_->session && state_->api.nvEncDestroyEncoder) state_->api.nvEncDestroyEncoder(state_->session);
    state_->session = nullptr; state_->width = 0; state_->height = 0;
    if (state_->library) { FreeLibrary(state_->library); state_->library = nullptr; }
    std::memset(&state_->api, 0, sizeof(state_->api));
}
const std::string& NvencVideoEncoder::last_error() const noexcept { return last_error_; }

bool NvencVideoEncoder::EnsureInitialized(const RetainedGpuFrame& frame) {
    if (!state_->session) return Initialize(frame.width(), frame.height());
    if (state_->width == frame.width() && state_->height == frame.height()) return true;
    last_error_ = "NVENC resolution changed while asynchronous frames are active";
    return false;
}

bool NvencVideoEncoder::Initialize(const uint32_t width, const uint32_t height) {
    if (!gs_get_context() || width == 0 || height == 0) { last_error_ = "NVENC initialization requires an entered graphics context and nonzero dimensions"; return false; }
    state_->library = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!state_->library) { last_error_ = "could not load nvEncodeAPI64.dll"; return false; }
    const auto create_instance = reinterpret_cast<NvEncodeApiCreateInstance>(GetProcAddress(state_->library, "NvEncodeAPICreateInstance"));
    if (!create_instance) { last_error_ = "NvEncodeAPICreateInstance is unavailable"; Shutdown(); return false; }
    state_->api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS status = create_instance(&state_->api);
    if (!NvencSucceeded(status)) { SetError("NvEncodeAPICreateInstance", status); Shutdown(); return false; }
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open_params{};
    open_params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open_params.device = gs_get_device_obj(); open_params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX; open_params.apiVersion = NVENCAPI_VERSION;
    status = state_->api.nvEncOpenEncodeSessionEx(&open_params, &state_->session);
    if (!NvencSucceeded(status)) { SetError("nvEncOpenEncodeSessionEx", status); Shutdown(); return false; }
    NV_ENC_PRESET_CONFIG preset{}; preset.version = NV_ENC_PRESET_CONFIG_VER; preset.presetCfg.version = NV_ENC_CONFIG_VER;
    status = state_->api.nvEncGetEncodePresetConfigEx(state_->session, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P3_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset);
    if (!NvencSucceeded(status)) { SetError("nvEncGetEncodePresetConfigEx", status); Shutdown(); return false; }
    preset.presetCfg.profileGUID = NV_ENC_H264_PROFILE_HIGH_444_GUID;
    preset.presetCfg.encodeCodecConfig.h264Config.chromaFormatIDC = 3;
    preset.presetCfg.gopLength = kDevelopmentGopFrames; preset.presetCfg.frameIntervalP = 1;
    preset.presetCfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    preset.presetCfg.rcParams.averageBitRate = kDevelopmentBitrateBitsPerSecond;
    preset.presetCfg.rcParams.maxBitRate = kDevelopmentBitrateBitsPerSecond;
    preset.presetCfg.rcParams.vbvBufferSize = kDevelopmentBitrateBitsPerSecond;
    preset.presetCfg.encodeCodecConfig.h264Config.idrPeriod = kDevelopmentGopFrames;
    preset.presetCfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    NV_ENC_INITIALIZE_PARAMS initialize{};
    initialize.version = NV_ENC_INITIALIZE_PARAMS_VER; initialize.encodeGUID = NV_ENC_CODEC_H264_GUID; initialize.presetGUID = NV_ENC_PRESET_P3_GUID;
    initialize.encodeWidth = width; initialize.encodeHeight = height; initialize.darWidth = width; initialize.darHeight = height;
    initialize.frameRateNum = kDevelopmentFrameRate; initialize.frameRateDen = 1; initialize.enablePTD = 1; initialize.enableEncodeAsync = 1;
    initialize.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY; initialize.encodeConfig = &preset.presetCfg;
    status = state_->api.nvEncInitializeEncoder(state_->session, &initialize);
    if (!NvencSucceeded(status)) { SetError("nvEncInitializeEncoder", status); Shutdown(); return false; }
    if (!CreateReusableResources(width, height)) { Shutdown(); return false; }
    state_->width = width; state_->height = height; state_->stopping = false;
    state_->completion_thread = std::thread(&NvencVideoEncoder::CompletionThread, this);
    last_error_.clear(); return true;
}

bool NvencVideoEncoder::CreateReusableResources(const uint32_t width, const uint32_t height) {
    auto* const device = static_cast<ID3D11Device*>(gs_get_device_obj());
    if (!device) { last_error_ = "gs_get_device_obj returned null"; return false; }
    D3D11_TEXTURE2D_DESC input_desc{}; input_desc.Width = width; input_desc.Height = height; input_desc.MipLevels = 1; input_desc.ArraySize = 1;
    input_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; input_desc.SampleDesc.Count = 1; input_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    for (State::Slot& slot : state_->slots) {
        if (FAILED(device->CreateTexture2D(&input_desc, nullptr, &slot.input_texture))) { last_error_ = "could not create reusable typed D3D11 NVENC input texture"; return false; }
        NV_ENC_REGISTER_RESOURCE resource{}; resource.version = NV_ENC_REGISTER_RESOURCE_VER; resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        resource.resourceToRegister = slot.input_texture; resource.width = width; resource.height = height; resource.pitch = width; resource.bufferFormat = NV_ENC_BUFFER_FORMAT_ABGR;
        NVENCSTATUS status = state_->api.nvEncRegisterResource(state_->session, &resource);
        if (!NvencSucceeded(status)) { SetError("nvEncRegisterResource", status); return false; }
        slot.registered_resource = resource.registeredResource;
        NV_ENC_CREATE_BITSTREAM_BUFFER bitstream{}; bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        status = state_->api.nvEncCreateBitstreamBuffer(state_->session, &bitstream);
        if (!NvencSucceeded(status)) { SetError("nvEncCreateBitstreamBuffer", status); return false; }
        slot.bitstream = bitstream.bitstreamBuffer;
        slot.completion_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!slot.completion_event) { last_error_ = "could not create NVENC completion event"; return false; }
        NV_ENC_EVENT_PARAMS event{}; event.version = NV_ENC_EVENT_PARAMS_VER; event.completionEvent = slot.completion_event;
        status = state_->api.nvEncRegisterAsyncEvent(state_->session, &event);
        if (!NvencSucceeded(status)) { SetError("nvEncRegisterAsyncEvent", status); return false; }
    }
    return true;
}

bool NvencVideoEncoder::SubmitAsync(const RetainedGpuFrame& frame, const int64_t encoder_pts) {
    void* const native_texture = gs_texture_get_obj(frame.texture()); auto* const device = static_cast<ID3D11Device*>(gs_get_device_obj());
    if (!native_texture || !device) { last_error_ = "OBS D3D11 source is unavailable"; return false; }
    size_t slot_index = kInFlightSlots;
    { std::lock_guard<std::mutex> lock(state_->slots_mutex); for (size_t i = 0; i < state_->slots.size(); ++i) if (!state_->slots[i].in_flight) { auto& slot = state_->slots[i]; slot.in_flight = true; slot.master_frame = frame.master_frame(); slot.encoder_pts = encoder_pts; slot_index = i; break; } }
    if (slot_index == kInFlightSlots) { last_error_ = "NVENC asynchronous in-flight ring is full"; return false; }
    State::Slot& slot = state_->slots[slot_index]; ResetEvent(slot.completion_event);
    ID3D11DeviceContext* context = nullptr; device->GetImmediateContext(&context);
    // Graphics-thread handoff only: command ordering retains the source through the copy; no Flush, query, or GPU wait occurs here.
    context->CopyResource(slot.input_texture, static_cast<ID3D11Resource*>(native_texture)); context->Release();
    NV_ENC_MAP_INPUT_RESOURCE mapped{}; mapped.version = NV_ENC_MAP_INPUT_RESOURCE_VER; mapped.registeredResource = slot.registered_resource;
    NVENCSTATUS status = state_->api.nvEncMapInputResource(state_->session, &mapped);
    if (!NvencSucceeded(status)) { SetError("nvEncMapInputResource", status); std::lock_guard<std::mutex> lock(state_->slots_mutex); slot.in_flight = false; slot.master_frame.reset(); return false; }
    slot.mapped_resource = mapped.mappedResource;
    NV_ENC_PIC_PARAMS picture{}; picture.version = NV_ENC_PIC_PARAMS_VER; picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picture.inputBuffer = slot.mapped_resource; picture.bufferFmt = NV_ENC_BUFFER_FORMAT_ABGR; picture.inputWidth = frame.width(); picture.inputHeight = frame.height(); picture.inputPitch = frame.width();
    picture.inputTimeStamp = static_cast<uint64_t>(encoder_pts); picture.outputBitstream = slot.bitstream; picture.completionEvent = slot.completion_event; picture.frameIdx = static_cast<uint32_t>(frame.master_frame().frame_id());
    if (frame.master_frame().frame_id() % kDevelopmentGopFrames == 0) picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    status = state_->api.nvEncEncodePicture(state_->session, &picture);
    if (!NvencSucceeded(status)) { SetError("nvEncEncodePicture", status); state_->api.nvEncUnmapInputResource(state_->session, slot.mapped_resource); slot.mapped_resource = nullptr; std::lock_guard<std::mutex> lock(state_->slots_mutex); slot.in_flight = false; slot.master_frame.reset(); return false; }
    { std::lock_guard<std::mutex> lock(state_->slots_mutex); state_->submission_order.push_back(slot_index); }
    state_->submitted.notify_one(); return true;
}

void NvencVideoEncoder::CompletionThread() noexcept {
    for (;;) {
        std::unique_lock<std::mutex> lock(state_->slots_mutex);
        state_->submitted.wait(lock, [this] { return state_->stopping || !state_->submission_order.empty(); });
        if (state_->submission_order.empty()) return;
        const HANDLE completion_event = state_->slots[state_->submission_order.front()].completion_event;
        lock.unlock();
        // NVENC requires output events within one session to be processed in submission order. This wait never runs on the OBS tick.
        WaitForSingleObject(completion_event, INFINITE);
        CompleteNextSubmission();
    }
}

void NvencVideoEncoder::CompleteNextSubmission() noexcept {
    size_t slot_index = 0; MasterFrame master_frame = [&] { std::lock_guard<std::mutex> lock(state_->slots_mutex); slot_index = state_->submission_order.front(); return *state_->slots[slot_index].master_frame; }();
    State::Slot& slot = state_->slots[slot_index]; NV_ENC_LOCK_BITSTREAM locked{}; locked.version = NV_ENC_LOCK_BITSTREAM_VER; locked.outputBitstream = slot.bitstream; locked.doNotWait = 1;
    const NVENCSTATUS status = state_->api.nvEncLockBitstream(state_->session, &locked);
    if (!NvencSucceeded(status) || locked.outputTimeStamp != static_cast<uint64_t>(slot.encoder_pts)) {
        const std::string detail = !NvencSucceeded(status) ? "nvEncLockBitstream failed with NVENCSTATUS=" + std::to_string(status) : "NVENC returned a packet timestamp different from submitted master PTS";
        if (NvencSucceeded(status)) state_->api.nvEncUnlockBitstream(state_->session, slot.bitstream);
        if (slot.mapped_resource) state_->api.nvEncUnmapInputResource(state_->session, slot.mapped_resource);
        { std::lock_guard<std::mutex> lock(state_->slots_mutex); slot.mapped_resource = nullptr; slot.in_flight = false; slot.master_frame.reset(); state_->submission_order.pop_front(); }
        NotifyFailure(master_frame, detail); return;
    }
    EncodedVideoPacket packet{master_frame, output_, slot.encoder_pts, slot.encoder_pts, locked.pictureType == NV_ENC_PIC_TYPE_IDR, {}};
    const auto* data = static_cast<const uint8_t*>(locked.bitstreamBufferPtr); packet.bytes.assign(data, data + locked.bitstreamSizeInBytes);
    state_->api.nvEncUnlockBitstream(state_->session, slot.bitstream); state_->api.nvEncUnmapInputResource(state_->session, slot.mapped_resource);
    { std::lock_guard<std::mutex> lock(state_->slots_mutex); slot.mapped_resource = nullptr; slot.in_flight = false; slot.master_frame.reset(); state_->submission_order.pop_front(); }
    if (packet_callback_) packet_callback_(std::move(packet));
}

void NvencVideoEncoder::ReleaseReusableResources() noexcept {
    for (State::Slot& slot : state_->slots) {
        if (slot.mapped_resource && state_->session && state_->api.nvEncUnmapInputResource) state_->api.nvEncUnmapInputResource(state_->session, slot.mapped_resource);
        slot.mapped_resource = nullptr;
        if (slot.completion_event && state_->session && state_->api.nvEncUnregisterAsyncEvent) { NV_ENC_EVENT_PARAMS event{}; event.version = NV_ENC_EVENT_PARAMS_VER; event.completionEvent = slot.completion_event; state_->api.nvEncUnregisterAsyncEvent(state_->session, &event); }
        if (slot.completion_event) { CloseHandle(slot.completion_event); slot.completion_event = nullptr; }
        if (slot.bitstream && state_->session && state_->api.nvEncDestroyBitstreamBuffer) state_->api.nvEncDestroyBitstreamBuffer(state_->session, slot.bitstream);
        slot.bitstream = nullptr;
        if (slot.registered_resource && state_->session && state_->api.nvEncUnregisterResource) state_->api.nvEncUnregisterResource(state_->session, slot.registered_resource);
        slot.registered_resource = nullptr;
        if (slot.input_texture) { slot.input_texture->Release(); slot.input_texture = nullptr; }
        slot.master_frame.reset(); slot.in_flight = false;
    }
    std::lock_guard<std::mutex> lock(state_->slots_mutex); state_->submission_order.clear(); state_->stopping = false;
}
void NvencVideoEncoder::NotifyFailure(const MasterFrame& master_frame, const std::string& detail) noexcept { if (failure_callback_) failure_callback_(master_frame, detail); }
void NvencVideoEncoder::SetError(const char* operation, const int status) noexcept { last_error_ = std::string(operation) + " failed with NVENCSTATUS=" + std::to_string(status); }
} // namespace obs_sync_replay
