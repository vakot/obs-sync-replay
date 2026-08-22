#include "config/audio-encoder-resolution.hpp"

#include <algorithm>
#include <array>

namespace obs_sync_replay {

namespace {

const RegisteredAudioEncoder* FindId(const std::string& id,
                                     const std::vector<RegisteredAudioEncoder>& registered) {
    const auto it = std::find_if(registered.begin(), registered.end(), [&id](const auto& encoder) {
        return encoder.id == id;
    });
    return it == registered.end() ? nullptr : &*it;
}

const RegisteredAudioEncoder* FindCodec(const std::string& codec,
                                        const std::vector<RegisteredAudioEncoder>& registered) {
    const auto it = std::find_if(registered.begin(), registered.end(), [&codec](const auto& encoder) {
        return encoder.codec == codec;
    });
    return it == registered.end() ? nullptr : &*it;
}

const RegisteredAudioEncoder* FindPreferredCodec(const std::string& codec,
                                                 const std::vector<RegisteredAudioEncoder>& registered) {
    // OBS frontend/utility/audio-encoders.cpp seeds the Simple AAC map from
    // ffmpeg_aac, then lets libfdk_aac and CoreAudio_AAC override matching
    // bitrates. The preference list keeps that platform behavior when the
    // profile contains the Simple codec alias rather than a native ID.
    static constexpr std::array<const char*, 3> kAacPreference = {
        "CoreAudio_AAC", "libfdk_aac", "ffmpeg_aac"};
    static constexpr std::array<const char*, 1> kOpusPreference = {"ffmpeg_opus"};

    if (codec == "aac") {
        for (const char* preferred_id : kAacPreference) {
            const RegisteredAudioEncoder* encoder = FindId(preferred_id, registered);
            if (encoder && encoder->codec == codec) {
                return encoder;
            }
        }
    } else if (codec == "opus") {
        for (const char* preferred_id : kOpusPreference) {
            const RegisteredAudioEncoder* encoder = FindId(preferred_id, registered);
            if (encoder && encoder->codec == codec) {
                return encoder;
            }
        }
    }
    return FindCodec(codec, registered);
}

} // namespace

std::optional<std::string> ResolveAudioEncoderId(
    const std::string& configured, const std::vector<RegisteredAudioEncoder>& registered) {
    if (configured.empty()) {
        return std::nullopt;
    }

    if (const RegisteredAudioEncoder* native = FindId(configured, registered)) {
        return native->id;
    }

    if (const RegisteredAudioEncoder* alias = FindPreferredCodec(configured, registered)) {
        return alias->id;
    }

    return std::nullopt;
}

} // namespace obs_sync_replay
