#include "topology/scene-topology.hpp"

#include <cstdlib>
#include <initializer_list>
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

std::vector<DiscoveredScene> Scenes(std::initializer_list<DiscoveredScene> scenes) {
    return std::vector<DiscoveredScene>(scenes);
}

void TestMasterAndDeterministicOrder() {
    SceneTopologyModel model;
    Require(model.current().streams.size() == 1, "empty discovery must retain the master stream");
    Require(model.current().streams.front().identity == StreamIdentity::Master(), "master identity must be explicit");
    Require(model.current().streams.front().display_name == "Master/Program", "master display name must be stable");

    const auto result = model.ApplyDiscovery(
        Scenes({{"uuid-b", "Second"}, {"uuid-a", "First"}, {"", "Invalid"}, {"uuid-b", "Duplicate"}}), false);
    Require(result == TopologyUpdateResult::Applied, "new scene discovery must apply while idle");
    const auto& streams = model.current().streams;
    Require(streams.size() == 3, "master plus unique real scenes must be active");
    Require(streams[1].identity == StreamIdentity::Scene("uuid-b"), "first scene must preserve collection order");
    Require(streams[2].identity == StreamIdentity::Scene("uuid-a"), "second scene must preserve collection order");
    Require(streams[1].collection_order == 1 && streams[2].collection_order == 2,
            "scene collection order must be deterministic");
}

void TestRenameAddRemoveWhileIdle() {
    SceneTopologyModel model;
    Require(model.ApplyDiscovery(Scenes({{"uuid-a", "Before"}}), false) == TopologyUpdateResult::Applied,
            "initial scene discovery must apply");
    Require(model.ApplyDiscovery(Scenes({{"uuid-a", "After"}}), false) == TopologyUpdateResult::Applied,
            "rename must update idle topology");
    Require(model.current().streams[1].identity == StreamIdentity::Scene("uuid-a"),
            "rename must preserve scene identity");
    Require(model.current().streams[1].display_name == "After", "rename must update display metadata");

    Require(model.ApplyDiscovery(Scenes({{"uuid-a", "After"}, {"uuid-b", "Added"}}), false) ==
                TopologyUpdateResult::Applied,
            "scene add must apply while idle");
    Require(model.current().streams.size() == 3, "idle add must create a new participant");
    Require(model.ApplyDiscovery(Scenes({{"uuid-b", "Added"}}), false) == TopologyUpdateResult::Applied,
            "scene remove must apply while idle");
    Require(model.current().streams.size() == 2 &&
                model.current().streams[1].identity == StreamIdentity::Scene("uuid-b"),
            "idle remove must remove only the absent scene");
}

void TestImmutableActiveEpochAndPendingTopology() {
    SceneTopologyModel model;
    Require(model.ApplyDiscovery(Scenes({{"uuid-a", "Alpha"}, {"uuid-b", "Beta"}}), false) ==
                TopologyUpdateResult::Applied,
            "active epoch test setup must apply");
    model.BeginCaptureEpoch();
    const SceneTopologySnapshot epoch = model.active_epoch();

    Require(model.ApplyDiscovery(Scenes({{"uuid-a", "Renamed"}, {"uuid-c", "Added"}}), true) ==
                TopologyUpdateResult::Staged,
            "active add/remove must stage a replacement topology");
    Require(model.has_pending(), "active topology change must remain pending");
    Require(model.active_epoch().streams.size() == epoch.streams.size() &&
                model.active_epoch().streams[1].identity == StreamIdentity::Scene("uuid-a") &&
                model.active_epoch().streams[2].identity == StreamIdentity::Scene("uuid-b"),
            "active epoch participants must remain immutable");
    Require(model.current().streams[1].display_name == "Renamed",
            "active rename must update current metadata without changing participants");
    Require(model.active_epoch().streams[1].display_name == "Alpha",
            "active epoch output metadata must retain its immutable snapshot");

    const auto applied = model.EndCaptureEpoch();
    Require(applied.has_value(), "ending an epoch with pending topology must apply it");
    Require(!model.capture_epoch_active() && !model.has_pending(), "epoch end must clear active and pending state");
    Require(model.current().streams.size() == 3 &&
                model.current().streams[2].identity == StreamIdentity::Scene("uuid-c"),
            "pending topology must become authoritative only after the epoch ends");
}

void TestIdentityCanKeyFutureParticipation() {
    SceneTopologyEntry entry;
    entry.identity = StreamIdentity::Scene("persistent-scene-uuid");
    entry.display_name = "Renamable Scene";
    entry.recording_enabled = false;
    entry.replay_enabled = true;
    Require(entry.identity == StreamIdentity::Scene("persistent-scene-uuid"),
            "participation metadata must be associated with stable scene identity");
    Require(!entry.recording_enabled && entry.replay_enabled,
            "future per-scene participation flags must not be encoded in display names");
}

} // namespace

int main() {
    TestMasterAndDeterministicOrder();
    TestRenameAddRemoveWhileIdle();
    TestImmutableActiveEpochAndPendingTopology();
    TestIdentityCanKeyFutureParticipation();
    return EXIT_SUCCESS;
}
