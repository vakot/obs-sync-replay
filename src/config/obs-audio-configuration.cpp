#include "config/obs-audio-configuration.hpp"

#include <obs-frontend-api.h>
#include <obs.h>
#include <obs-data.h>
#include <util/config-file.h>
#include <media-io/audio-io.h>

#include <algorithm>
#include <cstring>
#include <string>

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

} // namespace

ObsAudioConfiguration ReadObsAudioConfiguration() noexcept {
    ObsAudioConfiguration result;
    result.sample_rate = 48'000;
    result.channels = AudioChannels();
    config_t* profile = obs_frontend_get_profile_config();
    if (!profile) {
        result.recording_tracks.push_back({0, "aac", result.sample_rate, result.channels, 160, {}});
        return result;
    }

    const char* mode = config_get_string(profile, "Output", "Mode");
    const bool advanced = mode && std::strcmp(mode, "Advanced") == 0;
    const char* section = advanced ? "AdvOut" : "SimpleOutput";
    const char* encoder_key = advanced ? "RecAudioEncoder" : "RecAudioEncoder";
    const char* encoder_id = config_get_string(profile, section, encoder_key);
    const char* quality = config_get_string(profile, "SimpleOutput", "RecQuality");
    const char* format = config_get_string(profile, section, advanced ? "RecFormat2" : "RecFormat2");
    uint64_t mask = advanced ? ReadTrackMask(profile, section, "RecTracks", "TrackIndex")
                             : ReadTrackMask(profile, section, "RecTracks", nullptr);
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
        encoder_id = advanced ? config_get_string(profile, section, "AudioEncoder") : "aac";
    }
    if (!encoder_id || !*encoder_id || std::strcmp(encoder_id, "none") == 0) {
        encoder_id = "aac";
    }

    for (size_t mixer = 0; mixer < kMaxAudioMixes; ++mixer) {
        if ((mask & (uint64_t{1} << mixer)) == 0) {
            continue;
        }
        const uint32_t bitrate = advanced
                                     ? ReadBitrate(profile, section,
                                                   (std::string("Track") + std::to_string(mixer + 1) + "Bitrate").c_str(),
                                                   160)
                                     : ReadBitrate(profile, section, "ABitrate", 160);
        result.recording_tracks.push_back(
            {mixer, encoder_id, result.sample_rate, result.channels, bitrate, {}});
    }
    if (result.recording_tracks.empty()) {
        result.recording_tracks.push_back({0, encoder_id, result.sample_rate, result.channels, 160, {}});
    }
    return result;
}

} // namespace obs_sync_replay
