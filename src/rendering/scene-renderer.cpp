#include "rendering/scene-renderer.hpp"

#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <obs-module.h>

#include <memory>
#include <utility>

namespace obs_sync_replay {

namespace {

struct SourceReleaser final {
    void operator()(obs_source_t *source) const noexcept {
        if (source) {
            obs_source_release(source);
        }
    }
};

using SourceReference = std::unique_ptr<obs_source_t, SourceReleaser>;

} // namespace

SceneRenderer::SceneRenderer(const OutputSlot output, std::string scene_name)
    : output_(output), scene_name_(std::move(scene_name)) {}

SceneRenderer::~SceneRenderer() {
    DestroyRenderTarget();
}

SceneRenderResult SceneRenderer::Render(const MasterFrame &master_frame) {
    SceneRenderResult result{master_frame, output_, SceneRenderStatus::MissingScene, scene_name_, 0, 0, 0, 0, nullptr};
    SourceReference scene_source(obs_get_source_by_name(scene_name_.c_str()));
    if (!scene_source) {
        return result;
    }

    if (!obs_scene_from_source(scene_source.get())) {
        result.status = SceneRenderStatus::NotAScene;
        return result;
    }

    result.width = obs_source_get_width(scene_source.get());
    result.height = obs_source_get_height(scene_source.get());
    if (result.width == 0 || result.height == 0) {
        result.status = SceneRenderStatus::InvalidDimensions;
        return result;
    }

    const gs_color_space color_space = obs_source_get_color_space(scene_source.get(), 0, nullptr);
    const gs_color_format color_format = gs_get_format_from_space(color_space);
    result.color_space = static_cast<uint32_t>(color_space);
    result.color_format = static_cast<uint32_t>(color_format);
    if (render_target_ && gs_texrender_get_format(render_target_) != color_format) {
        gs_texrender_destroy(render_target_);
        render_target_ = nullptr;
    }
    if (!render_target_) {
        render_target_ = gs_texrender_create(color_format, GS_ZS_NONE);
    }
    if (!render_target_) {
        result.status = SceneRenderStatus::RenderTargetCreationFailed;
        return result;
    }

    // libobs marks a texrender as rendered in gs_texrender_end. Reset it before
    // its next master-frame render; this does not assign temporal identity.
    gs_texrender_reset(render_target_);
    if (!gs_texrender_begin_with_color_space(render_target_, result.width, result.height, color_space)) {
        result.status = SceneRenderStatus::RenderTargetBeginFailed;
        return result;
    }

    vec4 clear_color;
    vec4_zero(&clear_color);
    gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0F, 0);

    gs_viewport_push();
    gs_projection_push();
    gs_ortho(0.0F, static_cast<float>(result.width), 0.0F, static_cast<float>(result.height), -100.0F, 100.0F);
    gs_set_viewport(0, 0, result.width, result.height);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

    obs_source_inc_showing(scene_source.get());
    obs_source_video_render(scene_source.get());
    obs_source_dec_showing(scene_source.get());

    gs_blend_state_pop();
    gs_projection_pop();
    gs_viewport_pop();
    gs_texrender_end(render_target_);

    result.status = SceneRenderStatus::Rendered;
    result.texture = gs_texrender_get_texture(render_target_);
    return result;
}

void SceneRenderer::DestroyRenderTarget() {
    if (render_target_) {
        gs_texrender_destroy(render_target_);
        render_target_ = nullptr;
    }
}

const char *OutputSlotName(const OutputSlot output) noexcept {
    return output == OutputSlot::A ? "A" : "B";
}

const char *SceneRenderStatusName(const SceneRenderStatus status) noexcept {
    switch (status) {
    case SceneRenderStatus::Rendered:
        return "rendered";
    case SceneRenderStatus::MissingScene:
        return "missing-scene";
    case SceneRenderStatus::NotAScene:
        return "not-a-scene";
    case SceneRenderStatus::InvalidDimensions:
        return "invalid-dimensions";
    case SceneRenderStatus::RenderTargetCreationFailed:
        return "render-target-creation-failed";
    case SceneRenderStatus::RenderTargetBeginFailed:
        return "render-target-begin-failed";
    }

    return "unknown";
}

} // namespace obs_sync_replay
