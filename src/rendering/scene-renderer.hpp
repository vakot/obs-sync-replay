#pragma once

#include "timeline/master-frame.hpp"

#include <cstdint>
#include <string>

struct gs_texture_render;
typedef struct gs_texture_render gs_texrender_t;
struct gs_texture;
typedef struct gs_texture gs_texture_t;

namespace obs_sync_replay {

enum class OutputSlot : uint8_t {
    A,
    B,
};

enum class SceneRenderStatus : uint8_t {
    Rendered,
    MissingScene,
    NotAScene,
    InvalidDimensions,
    RenderTargetUnavailable,
};

// A texture is owned by its SceneRenderer and remains valid until that renderer
// renders its next frame or is destroyed. The MasterFrame itself is immutable.
struct SceneRenderResult final {
    MasterFrame master_frame;
    OutputSlot output;
    SceneRenderStatus status;
    std::string scene_name;
    uint32_t width;
    uint32_t height;
    gs_texture_t *texture;
};

// Renders one configured scene into one independently owned off-screen target.
// Render must execute with the libobs graphics context entered.
class SceneRenderer final {
public:
    SceneRenderer(OutputSlot output, std::string scene_name);
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer &) = delete;
    SceneRenderer &operator=(const SceneRenderer &) = delete;

    SceneRenderResult Render(const MasterFrame &master_frame);
    void DestroyRenderTarget();

private:
    OutputSlot output_;
    std::string scene_name_;
    gs_texrender_t *render_target_ = nullptr;
};

const char *OutputSlotName(OutputSlot output) noexcept;
const char *SceneRenderStatusName(SceneRenderStatus status) noexcept;

} // namespace obs_sync_replay
