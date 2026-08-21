#include "bootstrap/deterministic-test-environment.hpp"

#include <obs-module.h>

#include <cstdint>
#include <string>

namespace obs_sync_replay {

namespace {

constexpr uint32_t kExpectedWidth = 1920;
constexpr uint32_t kExpectedHeight = 1080;
constexpr uint32_t kExpectedFpsNumerator = 60;
constexpr uint32_t kExpectedFpsDenominator = 1;

constexpr char kColorSourceType[] = "color_source";
constexpr char kColorSourceAName[] = "Sync Research Synthetic A";
constexpr char kColorSourceBName[] = "Sync Research Synthetic B";

struct SourceCounts final {
    uint32_t inputs = 0;
    uint32_t scenes = 0;
    std::string sole_scene_name;
};

bool CountSceneItems(obs_scene_t *, obs_sceneitem_t *, void *param) {
    auto *count = static_cast<uint32_t *>(param);
    ++(*count);
    return true;
}

bool CountInput(void *param, obs_source_t *source) {
    auto *counts = static_cast<SourceCounts *>(param);
    ++counts->inputs;
    blog(LOG_INFO, "[sync-bootstrap] initial-input name=%s id=%s", obs_source_get_name(source),
         obs_source_get_id(source));
    return true;
}

bool CountScene(void *param, obs_source_t *source) {
    auto *counts = static_cast<SourceCounts *>(param);
    ++counts->scenes;
    if (counts->scenes == 1) {
        counts->sole_scene_name = obs_source_get_name(source);
    }
    blog(LOG_INFO, "[sync-bootstrap] initial-scene name=%s id=%s", obs_source_get_name(source),
         obs_source_get_id(source));
    return true;
}

bool RemoveStockEmptyPlaceholder(const std::string &scene_name) {
    obs_source_t *scene_source = obs_get_source_by_name(scene_name.c_str());
    if (!scene_source) {
        blog(LOG_ERROR, "[sync-bootstrap] stock-placeholder failed name=%s reason=source-not-found", scene_name.c_str());
        return false;
    }

    obs_scene_t *scene = obs_scene_from_source(scene_source);
    uint32_t item_count = 0;
    if (scene) {
        obs_scene_enum_items(scene, CountSceneItems, &item_count);
    }
    blog(LOG_INFO, "[sync-bootstrap] stock-placeholder-check name=%s items=%u", scene_name.c_str(), item_count);

    if (!scene || item_count != 0) {
        blog(LOG_ERROR,
             "[sync-bootstrap] stock-placeholder failed invariant=clean-runtime expected-one-empty-stock-scene");
        obs_source_release(scene_source);
        return false;
    }

    blog(LOG_INFO, "[sync-bootstrap] stock-placeholder remove begin name=%s", scene_name.c_str());
    obs_source_remove(scene_source);
    obs_source_release(scene_source);
    blog(LOG_INFO, "[sync-bootstrap] stock-placeholder remove complete name=%s", scene_name.c_str());
    return true;
}

void RemoveScene(obs_scene_t *scene) {
    if (!scene) {
        return;
    }

    obs_source_t *scene_source = obs_scene_get_source(scene);
    if (scene_source) {
        obs_source_remove(scene_source);
    }
    obs_scene_release(scene);
}

} // namespace

DeterministicTestEnvironment::~DeterministicTestEnvironment() {
    RemoveScene(scene_b_);
    RemoveScene(scene_a_);
    scene_b_ = nullptr;
    scene_a_ = nullptr;
}

bool DeterministicTestEnvironment::Setup() {
    blog(LOG_INFO,
         "[sync-bootstrap] begin clean_runtime=true expected_environment=two-scenes synthetic-color-sources");

    if (!VerifyVideoConfiguration() || !VerifyCleanSourceNamespace()) {
        blog(LOG_ERROR, "[sync-bootstrap] failed invariant=clean-runtime environment_not_constructed");
        return false;
    }

    if (!CreateScene(kResearchSceneAName, kColorSourceAName, 0xE83E3EFF, &scene_a_)) {
        return false;
    }
    if (!CreateScene(kResearchSceneBName, kColorSourceBName, 0x3E6FE8FF, &scene_b_)) {
        return false;
    }

    blog(LOG_INFO,
         "[sync-bootstrap] complete scene_a=%s scene_b=%s source_type=%s source_dimensions=%ux%u",
         kResearchSceneAName, kResearchSceneBName, kColorSourceType, kExpectedWidth, kExpectedHeight);
    return true;
}

bool DeterministicTestEnvironment::VerifyVideoConfiguration() const {
    obs_video_info video_info{};
    if (!obs_get_video_info(&video_info)) {
        blog(LOG_ERROR, "[sync-bootstrap] video-check failed reason=no-video-info");
        return false;
    }

    blog(LOG_INFO,
         "[sync-bootstrap] video-check observed base=%ux%u output=%ux%u fps=%u/%u format=%d",
         video_info.base_width, video_info.base_height, video_info.output_width, video_info.output_height,
         video_info.fps_num, video_info.fps_den, static_cast<int>(video_info.output_format));

    const bool valid = video_info.base_width == kExpectedWidth && video_info.base_height == kExpectedHeight &&
                       video_info.output_width == kExpectedWidth && video_info.output_height == kExpectedHeight &&
                       video_info.fps_num == kExpectedFpsNumerator && video_info.fps_den == kExpectedFpsDenominator;
    if (!valid) {
        blog(LOG_ERROR,
             "[sync-bootstrap] video-check failed invariant=clean-runtime expected=base/output %ux%u fps=%u/%u",
             kExpectedWidth, kExpectedHeight, kExpectedFpsNumerator, kExpectedFpsDenominator);
    }
    return valid;
}

bool DeterministicTestEnvironment::VerifyCleanSourceNamespace() const {
    SourceCounts counts;
    obs_enum_sources(CountInput, &counts);
    obs_enum_scenes(CountScene, &counts);

    blog(LOG_INFO, "[sync-bootstrap] initial-source-check inputs=%u scenes=%u", counts.inputs, counts.scenes);

    obs_source_t *scene_a = obs_get_source_by_name(kResearchSceneAName);
    obs_source_t *scene_b = obs_get_source_by_name(kResearchSceneBName);
    const bool names_available = scene_a == nullptr && scene_b == nullptr;
    if (scene_a) {
        obs_source_release(scene_a);
    }
    if (scene_b) {
        obs_source_release(scene_b);
    }
    if (!names_available) {
        blog(LOG_ERROR,
             "[sync-bootstrap] initial-source-check failed invariant=clean-runtime required-scene-name-collision");
        return false;
    }

    if (counts.inputs != 0 || counts.scenes > 1) {
        blog(LOG_ERROR,
             "[sync-bootstrap] initial-source-check failed invariant=clean-runtime expected-zero-inputs-and-at-most-one-stock-placeholder");
        return false;
    }

    if (counts.scenes == 1 && !RemoveStockEmptyPlaceholder(counts.sole_scene_name)) {
        return false;
    }

    SourceCounts remaining;
    obs_enum_sources(CountInput, &remaining);
    obs_enum_scenes(CountScene, &remaining);
    blog(LOG_INFO, "[sync-bootstrap] clean-source-check inputs=%u scenes=%u", remaining.inputs,
         remaining.scenes);
    if (remaining.inputs != 0 || remaining.scenes != 0) {
        blog(LOG_ERROR,
             "[sync-bootstrap] clean-source-check failed invariant=clean-runtime expected-zero-after-placeholder-cleanup");
        return false;
    }
    return true;
}

bool DeterministicTestEnvironment::CreateScene(const char *scene_name, const char *source_name, const uint32_t color,
                                               obs_scene_t **scene_out) {
    blog(LOG_INFO, "[sync-bootstrap] create-scene begin name=%s", scene_name);
    obs_scene_t *scene = obs_scene_create(scene_name);
    if (!scene) {
        blog(LOG_ERROR, "[sync-bootstrap] create-scene failed name=%s reason=obs_scene_create", scene_name);
        return false;
    }
    blog(LOG_INFO, "[sync-bootstrap] create-scene object-created name=%s", scene_name);

    obs_data_t *settings = obs_data_create();
    if (!settings) {
        blog(LOG_ERROR, "[sync-bootstrap] create-source failed scene=%s reason=obs_data_create", scene_name);
        obs_scene_release(scene);
        return false;
    }
    obs_data_set_int(settings, "color", color);
    obs_data_set_int(settings, "width", kExpectedWidth);
    obs_data_set_int(settings, "height", kExpectedHeight);

    const char *source_type = obs_get_latest_input_type_id(kColorSourceType);
    blog(LOG_INFO, "[sync-bootstrap] create-source begin scene=%s source=%s type=%s", scene_name, source_name,
         source_type ? source_type : "missing");
    obs_source_t *source = source_type ? obs_source_create(source_type, source_name, settings, nullptr) : nullptr;
    obs_data_release(settings);
    blog(LOG_INFO, "[sync-bootstrap] create-source returned scene=%s source=%s success=%s", scene_name, source_name,
         source ? "true" : "false");
    if (!source) {
        blog(LOG_ERROR,
             "[sync-bootstrap] create-source failed scene=%s source=%s type=%s reason=obs_source_create",
             scene_name, source_name, source_type ? source_type : "missing");
        obs_source_remove(obs_scene_get_source(scene));
        obs_scene_release(scene);
        return false;
    }

    blog(LOG_INFO, "[sync-bootstrap] create-source success scene=%s source=%s type=%s color=0x%08x dimensions=%ux%u",
         scene_name, source_name, source_type, color, kExpectedWidth, kExpectedHeight);
    obs_sceneitem_t *item = obs_scene_add(scene, source);
    obs_source_release(source);
    if (!item) {
        blog(LOG_ERROR, "[sync-bootstrap] add-source failed scene=%s source=%s reason=obs_scene_add", scene_name,
             source_name);
        obs_source_remove(obs_scene_get_source(scene));
        obs_scene_release(scene);
        return false;
    }

    *scene_out = scene;
    blog(LOG_INFO, "[sync-bootstrap] create-scene complete name=%s source=%s", scene_name, source_name);
    return true;
}

} // namespace obs_sync_replay
