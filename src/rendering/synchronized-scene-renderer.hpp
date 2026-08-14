#pragma once

#include "pipeline/synchronized-frame-pipeline.hpp"
#include "rendering/scene-render-pair-tracker.hpp"

#include <array>
#include <optional>
#include <string>

namespace obs_sync_replay {

// Consumes only frames issued by MasterFrameCoordinator. It performs both
// graphics renders synchronously after entering libobs's graphics context.
class SynchronizedSceneRenderer final {
  public:
    SynchronizedSceneRenderer(std::string scene_a_name, std::string scene_b_name);
    ~SynchronizedSceneRenderer();

    SynchronizedSceneRenderer(const SynchronizedSceneRenderer&) = delete;
    SynchronizedSceneRenderer& operator=(const SynchronizedSceneRenderer&) = delete;

    void Render(const MasterFrame& master_frame);
    void Stop();

  private:
    void RecordAndLog(const SceneRenderResult& result);
    void CaptureAndLog(const SceneRenderResult& output_a, const SceneRenderResult& output_b);

    SceneRenderer scene_a_renderer_;
    SceneRenderer scene_b_renderer_;
    SceneRenderPairTracker pair_tracker_;
    SynchronizedFramePipeline pipeline_{4};
    std::array<std::optional<SceneRenderStatus>, 2> last_reported_status_;
    std::optional<SynchronizedFramePipelineResult> last_reported_pipeline_result_;
    bool stopped_ = false;
};

} // namespace obs_sync_replay
