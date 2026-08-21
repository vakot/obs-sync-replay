#pragma once

#include <cstdint>

struct obs_scene;
typedef struct obs_scene obs_scene_t;

namespace obs_sync_replay {

inline constexpr char kResearchSceneAName[] = "Sync Research Scene A";
inline constexpr char kResearchSceneBName[] = "Sync Research Scene B";

// Creates the minimum synthetic environment required by the stock-OBS research
// run. Setup must execute after the frontend has activated its clean collection.
class DeterministicTestEnvironment final {
public:
    DeterministicTestEnvironment() = default;
    ~DeterministicTestEnvironment();

    DeterministicTestEnvironment(const DeterministicTestEnvironment &) = delete;
    DeterministicTestEnvironment &operator=(const DeterministicTestEnvironment &) = delete;

    bool Setup();

private:
    bool VerifyVideoConfiguration() const;
    bool VerifyCleanSourceNamespace() const;
    bool CreateScene(const char *scene_name, const char *source_name, uint32_t color, obs_scene_t **scene_out);

    obs_scene_t *scene_a_ = nullptr;
    obs_scene_t *scene_b_ = nullptr;
};

} // namespace obs_sync_replay
