#include "topology/scene-topology.hpp"

#include <algorithm>
#include <utility>

namespace obs_sync_replay {

StreamIdentity StreamIdentity::Master() {
    return {StreamKind::Master, "master"};
}

StreamIdentity StreamIdentity::Scene(std::string uuid) {
    return {StreamKind::Scene, std::move(uuid)};
}

bool StreamIdentity::operator==(const StreamIdentity& other) const noexcept {
    return kind == other.kind && key == other.key;
}

SceneTopologyModel::SceneTopologyModel() : current_(BuildSnapshot({}, 0)), active_epoch_(current_) {}

SceneTopologySnapshot SceneTopologyModel::BuildSnapshot(const std::vector<DiscoveredScene>& scenes,
                                                         const uint64_t generation) {
    SceneTopologySnapshot snapshot;
    snapshot.generation = generation;
    snapshot.streams.push_back({StreamIdentity::Master(), "Master/Program", 0, true, true});
    for (size_t index = 0; index < scenes.size(); ++index) {
        if (scenes[index].uuid.empty() ||
            std::any_of(snapshot.streams.begin(), snapshot.streams.end(), [&scenes, index](const auto& entry) {
                return entry.identity.kind == StreamKind::Scene && entry.identity.key == scenes[index].uuid;
            })) {
            continue;
        }
        snapshot.streams.push_back(
            {StreamIdentity::Scene(scenes[index].uuid), scenes[index].display_name, index + 1, true, true});
    }
    return snapshot;
}

bool SceneTopologyModel::SameTopology(const SceneTopologySnapshot& left,
                                       const SceneTopologySnapshot& right) noexcept {
    if (left.streams.size() != right.streams.size()) {
        return false;
    }
    for (size_t index = 0; index < left.streams.size(); ++index) {
        const SceneTopologyEntry& a = left.streams[index];
        const SceneTopologyEntry& b = right.streams[index];
        if (a.identity != b.identity || a.collection_order != b.collection_order ||
            a.recording_enabled != b.recording_enabled || a.replay_enabled != b.replay_enabled) {
            return false;
        }
    }
    return true;
}

void SceneTopologyModel::UpdateDisplayNames(SceneTopologySnapshot& target,
                                             const SceneTopologySnapshot& discovered) {
    for (SceneTopologyEntry& entry : target.streams) {
        const auto it = std::find_if(discovered.streams.begin(), discovered.streams.end(),
                                     [&entry](const SceneTopologyEntry& candidate) {
                                         return candidate.identity == entry.identity;
                                     });
        if (it != discovered.streams.end()) {
            entry.display_name = it->display_name;
        }
    }
}

TopologyUpdateResult SceneTopologyModel::ApplyDiscovery(const std::vector<DiscoveredScene>& scenes,
                                                         const bool capture_epoch_active) {
    const uint64_t next_generation = current_.generation + 1;
    const SceneTopologySnapshot discovered = BuildSnapshot(scenes, next_generation);
    const bool same = SameTopology(current_, discovered) &&
                      std::equal(current_.streams.begin(), current_.streams.end(), discovered.streams.begin(),
                                 [](const auto& left, const auto& right) {
                                     return left.display_name == right.display_name;
                                 });
    if (same) {
        return TopologyUpdateResult::Unchanged;
    }

    if (!capture_epoch_active) {
        current_ = discovered;
        pending_.reset();
        return TopologyUpdateResult::Applied;
    }

    // Renames are metadata-only during an epoch. Add/remove/order changes are
    // staged so the common synchronized participant set remains immutable.
    UpdateDisplayNames(current_, discovered);
    pending_ = discovered;
    return TopologyUpdateResult::Staged;
}

void SceneTopologyModel::BeginCaptureEpoch() noexcept {
    if (!capture_epoch_active_) {
        active_epoch_ = current_;
        capture_epoch_active_ = true;
    }
}

std::optional<SceneTopologySnapshot> SceneTopologyModel::EndCaptureEpoch() noexcept {
    if (!capture_epoch_active_) {
        return std::nullopt;
    }
    capture_epoch_active_ = false;
    if (!pending_) {
        return std::nullopt;
    }
    current_ = std::move(*pending_);
    pending_.reset();
    active_epoch_ = current_;
    return current_;
}

const SceneTopologySnapshot& SceneTopologyModel::current() const noexcept {
    return current_;
}

const SceneTopologySnapshot& SceneTopologyModel::active_epoch() const noexcept {
    return active_epoch_;
}

bool SceneTopologyModel::capture_epoch_active() const noexcept {
    return capture_epoch_active_;
}

bool SceneTopologyModel::has_pending() const noexcept {
    return pending_.has_value();
}

const char* StreamKindName(const StreamKind kind) noexcept {
    return kind == StreamKind::Master ? "master" : "scene";
}

std::string StreamIdentityLabel(const StreamIdentity& identity) {
    return identity.kind == StreamKind::Master ? "master" : "scene:" + identity.key;
}

} // namespace obs_sync_replay
