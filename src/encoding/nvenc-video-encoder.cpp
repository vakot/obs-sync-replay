#include "encoding/nvenc-video-encoder.hpp"

#include <graphics/graphics.h>
#include <obs-module.h>

#include <d3d11.h>
#include <ffnvcodec/nvEncodeAPI.h>
#include <windows.h>

#include <cstring>
#include <utility>

namespace obs_sync_replay {

namespace {

constexpr uint32_t kDevelopmentBitrateBitsPerSecond = 16000000;
constexpr uint32_t kDevelopmentGopFrames = 120;
constexpr uint32_t kDevelopmentFrameRate = 60;

using NvEncodeApiCreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

bool NvencSucceeded(const NVENCSTATUS status) noexcept {
    return status == NV_ENC_SUCCESS;
}

} // namespace

struct NvencVideoEncoder::State final {
    HMODULE library = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api{};
    void* session = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

NvencVideoEncoder::NvencVideoEncoder(const OutputSlot output)
    : state_(std::make_unique<State>()), output_(output) {}

NvencVideoEncoder::~NvencVideoEncoder() {
    Shutdown();
}

VideoEncoderSubmitResult NvencVideoEncoder::Submit(const RetainedGpuFrame& frame,
                                                    const int64_t encoder_pts,
                                                    EncodedVideoPacket* const packet) {
    if (!packet || frame.output() != output_ || !frame.texture()) {
        last_error_ = "invalid NVENC submission";
        return VideoEncoderSubmitResult::Failed;
    }
    if (frame.color_format() != static_cast<uint32_t>(GS_RGBA)) {
        last_error_ = "Phase 4 direct NVENC accepts retained GS_RGBA textures only";
        return VideoEncoderSubmitResult::Failed;
    }
    if (!EnsureInitialized(frame) || !Encode(frame, encoder_pts, packet)) {
        return VideoEncoderSubmitResult::Failed;
    }
    return VideoEncoderSubmitResult::Encoded;
}

void NvencVideoEncoder::Shutdown() noexcept {
    if (state_->session && state_->api.nvEncDestroyEncoder) {
        state_->api.nvEncDestroyEncoder(state_->session);
    }
    state_->session = nullptr;
    state_->width = 0;
    state_->height = 0;
    if (state_->library) {
        FreeLibrary(state_->library);
        state_->library = nullptr;
    }
    std::memset(&state_->api, 0, sizeof(state_->api));
}

const std::string& NvencVideoEncoder::last_error() const noexcept {
    return last_error_;
}

bool NvencVideoEncoder::EnsureInitialized(const RetainedGpuFrame& frame) {
    if (state_->session && state_->width == frame.width() && state_->height == frame.height()) {
        return true;
    }

    Shutdown();
    return Initialize(frame.width(), frame.height());
}

bool NvencVideoEncoder::Initialize(const uint32_t width, const uint32_t height) {
    if (!gs_get_context() || width == 0 || height == 0) {
        last_error_ = "NVENC initialization requires an entered graphics context and nonzero dimensions";
        return false;
    }

    state_->library = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!state_->library) {
        last_error_ = "could not load nvEncodeAPI64.dll";
        return false;
    }
    const auto create_instance = reinterpret_cast<NvEncodeApiCreateInstance>(
        GetProcAddress(state_->library, "NvEncodeAPICreateInstance"));
    if (!create_instance) {
        last_error_ = "NvEncodeAPICreateInstance is unavailable";
        Shutdown();
        return false;
    }

    state_->api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS status = create_instance(&state_->api);
    if (!NvencSucceeded(status)) {
        SetError("NvEncodeAPICreateInstance", status);
        Shutdown();
        return false;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open_params{};
    open_params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open_params.device = gs_get_device_obj();
    open_params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open_params.apiVersion = NVENCAPI_VERSION;
    status = state_->api.nvEncOpenEncodeSessionEx(&open_params, &state_->session);
    if (!NvencSucceeded(status)) {
        SetError("nvEncOpenEncodeSessionEx", status);
        Shutdown();
        return false;
    }

    NV_ENC_PRESET_CONFIG preset{};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    status = state_->api.nvEncGetEncodePresetConfigEx(state_->session, NV_ENC_CODEC_H264_GUID,
                                                       NV_ENC_PRESET_P3_GUID,
                                                       NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset);
    if (!NvencSucceeded(status)) {
        SetError("nvEncGetEncodePresetConfigEx", status);
        Shutdown();
        return false;
    }

    // The retained D3D11 texture is RGBA. NVENC accepts this source format
    // through the H.264 4:4:4 profile; normal High would require an NV12
    // conversion path that public libobs does not expose for arbitrary textures.
    preset.presetCfg.profileGUID = NV_ENC_H264_PROFILE_HIGH_444_GUID;
    preset.presetCfg.encodeCodecConfig.h264Config.chromaFormatIDC = 3;
    preset.presetCfg.gopLength = kDevelopmentGopFrames;
    preset.presetCfg.frameIntervalP = 1;
    preset.presetCfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    preset.presetCfg.rcParams.averageBitRate = kDevelopmentBitrateBitsPerSecond;
    preset.presetCfg.rcParams.maxBitRate = kDevelopmentBitrateBitsPerSecond;
    preset.presetCfg.rcParams.vbvBufferSize = kDevelopmentBitrateBitsPerSecond;
    preset.presetCfg.encodeCodecConfig.h264Config.idrPeriod = kDevelopmentGopFrames;
    preset.presetCfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;

    NV_ENC_INITIALIZE_PARAMS initialize{};
    initialize.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initialize.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initialize.presetGUID = NV_ENC_PRESET_P3_GUID;
    initialize.encodeWidth = width;
    initialize.encodeHeight = height;
    initialize.darWidth = width;
    initialize.darHeight = height;
    initialize.frameRateNum = kDevelopmentFrameRate;
    initialize.frameRateDen = 1;
    initialize.enablePTD = 1;
    initialize.enableEncodeAsync = 0;
    initialize.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    initialize.encodeConfig = &preset.presetCfg;
    status = state_->api.nvEncInitializeEncoder(state_->session, &initialize);
    if (!NvencSucceeded(status)) {
        SetError("nvEncInitializeEncoder", status);
        Shutdown();
        return false;
    }

    state_->width = width;
    state_->height = height;
    last_error_.clear();
    return true;
}

bool NvencVideoEncoder::Encode(const RetainedGpuFrame& frame, const int64_t encoder_pts,
                               EncodedVideoPacket* const packet) {
    void* const native_texture = gs_texture_get_obj(frame.texture());
    if (!native_texture) {
        last_error_ = "gs_texture_get_obj returned null";
        return false;
    }

    auto* const device = static_cast<ID3D11Device*>(gs_get_device_obj());
    if (!device) {
        last_error_ = "gs_get_device_obj returned null";
        return false;
    }

    // OBS creates GS_RGBA resources as typeless D3D textures. NVENC rejects
    // registering that resource directly, so copy it to a typed RGBA texture.
    // The event query is an explicit GPU completion fence for the retained
    // source; after it signals, only the NVENC-owned submission texture remains.
    D3D11_TEXTURE2D_DESC input_desc{};
    input_desc.Width = frame.width();
    input_desc.Height = frame.height();
    input_desc.MipLevels = 1;
    input_desc.ArraySize = 1;
    input_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    input_desc.SampleDesc.Count = 1;
    input_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* input_texture = nullptr;
    HRESULT hr = device->CreateTexture2D(&input_desc, nullptr, &input_texture);
    if (FAILED(hr)) {
        last_error_ = "could not create typed D3D11 NVENC input texture";
        return false;
    }
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    D3D11_QUERY_DESC query_desc{};
    query_desc.Query = D3D11_QUERY_EVENT;
    ID3D11Query* copy_complete = nullptr;
    hr = device->CreateQuery(&query_desc, &copy_complete);
    if (FAILED(hr)) {
        context->Release();
        input_texture->Release();
        last_error_ = "could not create D3D11 copy completion query";
        return false;
    }
    context->CopyResource(input_texture, static_cast<ID3D11Resource*>(native_texture));
    context->End(copy_complete);
    context->Flush();
    HRESULT copy_status = S_FALSE;
    while (copy_status == S_FALSE) {
        copy_status = context->GetData(copy_complete, nullptr, 0, 0);
    }
    if (FAILED(copy_status)) {
        copy_complete->Release();
        context->Release();
        input_texture->Release();
        last_error_ = "D3D11 retained-texture copy completion query failed";
        return false;
    }
    copy_complete->Release();
    context->Release();

    NV_ENC_REGISTER_RESOURCE resource{};
    resource.version = NV_ENC_REGISTER_RESOURCE_VER;
    resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    resource.resourceToRegister = input_texture;
    resource.width = frame.width();
    resource.height = frame.height();
    resource.pitch = frame.width();
    resource.bufferFormat = NV_ENC_BUFFER_FORMAT_ABGR;

    NVENCSTATUS status = state_->api.nvEncRegisterResource(state_->session, &resource);
    if (!NvencSucceeded(status)) {
        SetError("nvEncRegisterResource", status);
        input_texture->Release();
        return false;
    }

    NV_ENC_CREATE_BITSTREAM_BUFFER bitstream{};
    bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    status = state_->api.nvEncCreateBitstreamBuffer(state_->session, &bitstream);
    if (!NvencSucceeded(status)) {
        SetError("nvEncCreateBitstreamBuffer", status);
        state_->api.nvEncUnregisterResource(state_->session, resource.registeredResource);
        input_texture->Release();
        return false;
    }

    NV_ENC_MAP_INPUT_RESOURCE mapped{};
    mapped.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapped.registeredResource = resource.registeredResource;
    status = state_->api.nvEncMapInputResource(state_->session, &mapped);
    if (!NvencSucceeded(status)) {
        SetError("nvEncMapInputResource", status);
        state_->api.nvEncDestroyBitstreamBuffer(state_->session, bitstream.bitstreamBuffer);
        state_->api.nvEncUnregisterResource(state_->session, resource.registeredResource);
        input_texture->Release();
        return false;
    }

    NV_ENC_PIC_PARAMS picture{};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picture.inputBuffer = mapped.mappedResource;
    picture.bufferFmt = NV_ENC_BUFFER_FORMAT_ABGR;
    picture.inputWidth = frame.width();
    picture.inputHeight = frame.height();
    picture.inputPitch = frame.width();
    picture.inputTimeStamp = static_cast<uint64_t>(encoder_pts);
    picture.outputBitstream = bitstream.bitstreamBuffer;
    picture.frameIdx = static_cast<uint32_t>(frame.master_frame().frame_id());
    if (frame.master_frame().frame_id() % kDevelopmentGopFrames == 0) {
        picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }

    status = state_->api.nvEncEncodePicture(state_->session, &picture);
    if (!NvencSucceeded(status)) {
        SetError("nvEncEncodePicture", status);
        state_->api.nvEncUnmapInputResource(state_->session, mapped.mappedResource);
        state_->api.nvEncDestroyBitstreamBuffer(state_->session, bitstream.bitstreamBuffer);
        state_->api.nvEncUnregisterResource(state_->session, resource.registeredResource);
        input_texture->Release();
        return false;
    }

    NV_ENC_LOCK_BITSTREAM locked{};
    locked.version = NV_ENC_LOCK_BITSTREAM_VER;
    locked.outputBitstream = bitstream.bitstreamBuffer;
    // doNotWait=false is the completion fence: only after this returns is the
    // mapped retained texture unregistered and eligible for destruction.
    locked.doNotWait = 0;
    status = state_->api.nvEncLockBitstream(state_->session, &locked);
    if (!NvencSucceeded(status)) {
        SetError("nvEncLockBitstream", status);
        state_->api.nvEncUnmapInputResource(state_->session, mapped.mappedResource);
        state_->api.nvEncDestroyBitstreamBuffer(state_->session, bitstream.bitstreamBuffer);
        state_->api.nvEncUnregisterResource(state_->session, resource.registeredResource);
        input_texture->Release();
        return false;
    }

    if (locked.outputTimeStamp != static_cast<uint64_t>(encoder_pts)) {
        last_error_ = "NVENC returned a packet timestamp different from the submitted master PTS";
        state_->api.nvEncUnlockBitstream(state_->session, bitstream.bitstreamBuffer);
        state_->api.nvEncUnmapInputResource(state_->session, mapped.mappedResource);
        state_->api.nvEncDestroyBitstreamBuffer(state_->session, bitstream.bitstreamBuffer);
        state_->api.nvEncUnregisterResource(state_->session, resource.registeredResource);
        return false;
    }

    packet->master_frame = frame.master_frame();
    packet->output = output_;
    packet->pts = encoder_pts;
    packet->dts = encoder_pts;
    packet->keyframe = locked.pictureType == NV_ENC_PIC_TYPE_IDR;
    const auto* data = static_cast<const uint8_t*>(locked.bitstreamBufferPtr);
    packet->bytes.assign(data, data + locked.bitstreamSizeInBytes);

    state_->api.nvEncUnlockBitstream(state_->session, bitstream.bitstreamBuffer);
    state_->api.nvEncUnmapInputResource(state_->session, mapped.mappedResource);
    state_->api.nvEncDestroyBitstreamBuffer(state_->session, bitstream.bitstreamBuffer);
    state_->api.nvEncUnregisterResource(state_->session, resource.registeredResource);
    input_texture->Release();
    return true;
}

void NvencVideoEncoder::SetError(const char* const operation, const int status) noexcept {
    last_error_ = std::string(operation) + " failed with NVENCSTATUS=" + std::to_string(status);
}

} // namespace obs_sync_replay
