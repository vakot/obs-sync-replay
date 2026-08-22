#include "config/obs-audio-configuration.hpp"
#include "config/audio-encoder-resolution.hpp"
#include "plugin/plugin-log.hpp"

#include <obs-frontend-api.h>
#include <obs.h>
#include <obs-data.h>
#include <util/config-file.h>
#include <media-io/audio-io.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace obs_sync_replay {

namespace {

constexpr size_t kMaxAudioMixes = 6;

uint32_t AudioChannels() noexcept {
    obs_audio_info info{};
    if (!obs_get_audio_info(&info)) {
        return 2;
    }
    const uint32_t channels = get_audio_channels(info.speakers);
    return channels == 0 ? 2 : channels;
}

uint64_t ReadTrackMask(config_t* profile, const char* section, const char* mask_key, const char* fallback_key) {
    const uint64_t mask = static_cast<uint64_t>(config_get_uint(profile, section, mask_key));
    if (mask != 0 || !fallback_key) {
        return mask;
    }
    const int fallback = static_cast<int>(config_get_int(profile, section, fallback_key));
    return fallback > 0 && fallback <= static_cast<int>(kMaxAudioMixes) ? (uint64_t{1} << (fallback - 1)) : 0;
}

uint32_t ReadBitrate(config_t* profile, const char* section, const char* key, uint32_t fallback) {
    const int value = static_cast<int>(config_get_int(profile, section, key));
    return value > 0 ? static_cast<uint32_t>(value) : fallback;
}

std::vector<RegisteredAudioEncoder> RegisteredAudioEncoders() {
    std::vector<RegisteredAudioEncoder> result;
    const char* id = nullptr;
    for (size_t index = 0; obs_enum_encoder_types(index, &id); ++index) {
        if (!id || obs_get_encoder_type(id) != OBS_ENCODER_AUDIO) {
            continue;
        }
        const char* codec = obs_get_encoder_codec(id);
        if (codec && *codec) {
            result.push_back({id, codec});
        }
    }
    return result;
}

} // namespace

ObsAudioConfiguration ReadObsAudioConfiguration() noexcept {
    ObsAudioConfiguration result;
    result.sample_rate = 48'000;
    result.channels = AudioChannels();
    config_t* profile = obs_frontend_get_profile_config();
    bool advanced = false;
    const char* encoder_id = "aac";
    const char* quality = nullptr;
    const char* format = nullptr;
    uint64_t mask = 1;
    if (profile) {
        const char* mode = config_get_string(profile, "Output", "Mode");
        advanced = mode && std::strcmp(mode, "Advanced") == 0;
        const char* section = advanced ? "AdvOut" : "SimpleOutput";
        encoder_id = config_get_string(profile, section, "RecAudioEncoder");
        quality = config_get_string(profile, "SimpleOutput", "RecQuality");
        format = config_get_string(profile, section, "RecFormat2");
        mask = advanced ? ReadTrackMask(profile, section, "RecTracks", "TrackIndex")
                        : ReadTrackMask(profile, section, "RecTracks", nullptr);
    }
    const bool single_track_fallback = !advanced && quality && std::strcmp(quality, "Stream") == 0;
    if (single_track_fallback) {
        mask = 1;
    }
    // Stock FLV recording selects one enabled track. The plugin's MKV output
    // remains multi-track for other formats, matching the configured MKV
    // Recording path rather than silently collapsing the normal case.
    const bool flv = format && (_stricmp(format, "flv") == 0);
    if (flv && mask != 0) {
        uint64_t selected = 0;
        for (size_t mixer = 0; mixer < kMaxAudioMixes; ++mixer) {
            if (mask & (uint64_t{1} << mixer)) {
                selected = uint64_t{1} << mixer;
            }
        }
        mask = selected;
    }
    if (!encoder_id || !*encoder_id || std::strcmp(encoder_id, "none") == 0) {
        encoder_id = profile && advanced ? config_get_string(profile, "AdvOut", "AudioEncoder") : "aac";
    }
    if (!encoder_id || !*encoder_id || std::strcmp(encoder_id, "none") == 0) {
        encoder_id = "aac";
    }

    const std::string configured_encoder = encoder_id;
    const std::vector<RegisteredAudioEncoder> registered = RegisteredAudioEncoders();
    const std::optional<std::string> resolved_encoder = ResolveAudioEncoderId(configured_encoder, registered);
    if (!resolved_encoder) {
        result.valid = false;
        result.error = "audio-encoder-unavailable";
        OBS_SYNC_REPLAY_LOG(LOG_ERROR, "audio",
                            "encoder-resolve-failed configured=%s track=all reason=not-registered",
                            configured_encoder.c_str());
        return result;
    }

    for (size_t mixer = 0; mixer < kMaxAudioMixes; ++mixer) {
        if ((mask & (uint64_t{1} << mixer)) == 0) {
            continue;
        }
        const uint32_t bitrate = advanced
                                     ? ReadBitrate(profile, "AdvOut",
                                                   (std::string("Track") + std::to_string(mixer + 1) + "Bitrate").c_str(),
                                                   160)
                                     : profile ? ReadBitrate(profile, "SimpleOutput", "ABitrate", 160) : 160;
        OBS_SYNC_REPLAY_LOG(LOG_INFO, "audio", "encoder-resolve configured=%s resolved=%s mixer=%zu",
                            configured_encoder.c_str(), resolved_encoder->c_str(), mixer);
        result.recording_tracks.push_back(
            {mixer, *resolved_encoder, result.sample_rate, result.channels, bitrate, {}});
    }
    if (result.recording_tracks.empty()) {
        OBS_SYNC_REPLAY_LOG(LOG_INFO, "audio", "encoder-resolve configured=%s resolved=%s mixer=0",
                            configured_encoder.c_str(), resolved_encoder->c_str());
        result.recording_tracks.push_back({0, *resolved_encoder, result.sample_rate, result.channels, 160, {}});
    }
    return result;
}

} // namespace obs_sync_replay
