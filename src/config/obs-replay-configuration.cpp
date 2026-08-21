#include "config/obs-replay-configuration.hpp"

#include <obs-data.h>
#include <obs-frontend-api.h>
#include <util/base.h>
#include <util/config-file.h>

#include <cstring>
#include <filesystem>

namespace obs_sync_replay {

namespace {

bool IsBitrateRateControl(const char* rate_control) noexcept {
    return rate_control && (std::strcmp(rate_control, "CBR") == 0 || std::strcmp(rate_control, "VBR") == 0 ||
                            std::strcmp(rate_control, "ABR") == 0 || std::strcmp(rate_control, "cbr") == 0 ||
                            std::strcmp(rate_control, "vbr") == 0 || std::strcmp(rate_control, "abr") == 0);
}

bool ReadAdvancedMemoryLimitApplies(const char* profile_path, const char* record_encoder) {
    const char* settings_name = record_encoder && std::strcmp(record_encoder, "none") == 0 ? "streamEncoder.json"
                                                                                              : "recordEncoder.json";
    if (!profile_path || !*profile_path) {
        return true;
    }

    const std::filesystem::path settings_path = std::filesystem::u8path(profile_path) / settings_name;
    obs_data_t* settings = obs_data_create_from_json_file_safe(settings_path.u8string().c_str(), "bak");
    if (!settings) {
        return true;
    }
    const bool bitrate_rate_control = IsBitrateRateControl(obs_data_get_string(settings, "rate_control"));
    obs_data_release(settings);
    return !bitrate_rate_control;
}

} // namespace

ReplayConfiguration ReadObsReplayConfiguration() noexcept {
    ReplayProfileValues values;
    config_t* profile_config = obs_frontend_get_profile_config();
    if (!profile_config) {
        return MakeReplayConfiguration(values);
    }

    const char* mode = config_get_string(profile_config, "Output", "Mode");
    values.output_mode = mode && std::strcmp(mode, "Advanced") == 0 ? ReplayOutputMode::Advanced
                                                                      : ReplayOutputMode::Simple;

    if (values.output_mode == ReplayOutputMode::Simple) {
        values.replay_enabled = config_get_bool(profile_config, "SimpleOutput", "RecRB");
        values.duration_seconds = static_cast<uint32_t>(config_get_int(profile_config, "SimpleOutput", "RecRBTime"));
        values.memory_megabytes = static_cast<size_t>(config_get_int(profile_config, "SimpleOutput", "RecRBSize"));

        const char* quality = config_get_string(profile_config, "SimpleOutput", "RecQuality");
        const bool stream_quality = quality && std::strcmp(quality, "Stream") == 0;
        const bool lossless = quality && std::strcmp(quality, "Lossless") == 0;
        values.stock_memory_limit_applies = !stream_quality;
        values.stock_backend_available = !lossless;
    } else {
        values.replay_enabled = config_get_bool(profile_config, "AdvOut", "RecRB");
        values.duration_seconds = static_cast<uint32_t>(config_get_int(profile_config, "AdvOut", "RecRBTime"));
        values.memory_megabytes = static_cast<size_t>(config_get_int(profile_config, "AdvOut", "RecRBSize"));

        const char* recording_type = config_get_string(profile_config, "AdvOut", "RecType");
        values.stock_backend_available = !recording_type || std::strcmp(recording_type, "FFmpeg") != 0;
        char* profile_path = obs_frontend_get_current_profile_path();
        values.stock_memory_limit_applies = ReadAdvancedMemoryLimitApplies(
            profile_path, config_get_string(profile_config, "AdvOut", "RecEncoder"));
        if (profile_path) {
            bfree(profile_path);
        }
    }

    return MakeReplayConfiguration(values);
}

} // namespace obs_sync_replay
