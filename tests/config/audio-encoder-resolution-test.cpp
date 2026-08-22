#include "config/audio-encoder-resolution.hpp"

#include <cstdlib>
#include <iostream>
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

} // namespace

int main() {
    const std::vector<RegisteredAudioEncoder> registered = {
        {"ffmpeg_aac", "aac"}, {"ffmpeg_opus", "opus"}, {"other_aac", "aac"}};

    Require(ResolveAudioEncoderId("aac", registered) == "ffmpeg_aac", "aac alias must resolve to stock fallback");
    Require(ResolveAudioEncoderId("opus", registered) == "ffmpeg_opus", "opus alias must resolve to native ID");
    Require(ResolveAudioEncoderId("other_aac", registered) == "other_aac", "native ID must remain unchanged");
    Require(!ResolveAudioEncoderId("unavailable", registered), "unavailable encoder must fail explicitly");

    const std::vector<RegisteredAudioEncoder> platform_aac = {
        {"ffmpeg_aac", "aac"}, {"libfdk_aac", "aac"}, {"CoreAudio_AAC", "aac"}};
    Require(ResolveAudioEncoderId("aac", platform_aac) == "CoreAudio_AAC",
            "AAC alias must follow OBS platform preference order");
    return EXIT_SUCCESS;
}
