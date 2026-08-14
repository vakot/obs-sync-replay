#pragma once

#include "rendering/scene-renderer.hpp"

#include <optional>

namespace obs_sync_replay {

// OBS-independent pair accounting. It rejects a result unless it belongs to
// the single MasterFrame currently being dispatched and each output is unique.
class SceneRenderPairTracker final {
public:
    bool Begin(const MasterFrame &master_frame) noexcept;
    bool Record(const SceneRenderResult &result) noexcept;
    bool IsComplete() const noexcept;
    void Reset() noexcept;

private:
    std::optional<MasterFrame> active_frame_;
    bool output_a_recorded_ = false;
    bool output_b_recorded_ = false;
};

} // namespace obs_sync_replay
