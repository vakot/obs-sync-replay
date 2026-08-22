#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace obs_sync_replay {

enum class StreamKind : uint8_t {
    Master,
    Scene,
};

struct StreamIdentity final {
    StreamKind kind = StreamKind::Master;
    std::string key = "master";

    static StreamIdentity Master();
    static StreamIdentity Scene(std::string uuid);

    bool operator==(const StreamIdentity& other) const noexcept;
    bool operator!=(const StreamIdentity& other) const noexcept {
        return !(*this == other);
    }
};

struct DiscoveredScene final {
    std::string uuid;
    std::string display_name;
};

struct SceneTopologyEntry final {
    StreamIdentity identity;
    std::string display_name;
    size_t collection_order = 0;
    bool recording_enabled = true;
    bool replay_enabled = true;
};

struct SceneTopologySnapshot final {
    std::vector<SceneTopologyEntry> streams;
    uint64_t generation = 0;
};

enum class TopologyUpdateResult : uint8_t {
    Unchanged,
    Applied,
    Staged,
};

class SceneTopologyModel final {
  public:
    SceneTopologyModel();

    TopologyUpdateResult ApplyDiscovery(const std::vector<DiscoveredScene>& scenes,
                                        bool capture_epoch_active);
    void BeginCaptureEpoch() noexcept;
    std::optional<SceneTopologySnapshot> EndCaptureEpoch() noexcept;

    const SceneTopologySnapshot& current() const noexcept;
    const SceneTopologySnapshot& active_epoch() const noexcept;
    bool capture_epoch_active() const noexcept;
    bool has_pending() const noexcept;

  private:
    static SceneTopologySnapshot BuildSnapshot(const std::vector<DiscoveredScene>& scenes, uint64_t generation);
    static bool SameTopology(const SceneTopologySnapshot& left, const SceneTopologySnapshot& right) noexcept;
    static void UpdateDisplayNames(SceneTopologySnapshot& target, const SceneTopologySnapshot& discovered);

    SceneTopologySnapshot current_;
    SceneTopologySnapshot active_epoch_;
    std::optional<SceneTopologySnapshot> pending_;
    bool capture_epoch_active_ = false;
};

const char* StreamKindName(StreamKind kind) noexcept;
std::string StreamIdentityLabel(const StreamIdentity& identity);

} // namespace obs_sync_replay
