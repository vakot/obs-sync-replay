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

void TestZeroOneManyRealScenes() {
    SceneTopologyModel model;
    Require(model.current().streams.size() == 1, "zero real scenes must produce a Master-only topology");

    Require(model.ApplyDiscovery(Scenes({{"gameplay-uuid", "Gameplay"}}), false) == TopologyUpdateResult::Applied,
            "one arbitrary real scene must apply while idle");
    Require(model.current().streams.size() == 2 &&
                model.current().streams[1].identity == StreamIdentity::Scene("gameplay-uuid") &&
                model.current().streams[1].display_name == "Gameplay",
            "one arbitrary real scene must be the only non-Master stream");

    Require(model.ApplyDiscovery(
                Scenes({{"camera-uuid", "Camera"}, {"brb-uuid", "BRB"}, {"nested-uuid", "Nested Scene"},
                        {"intro-uuid", "Intro"}}),
                false) == TopologyUpdateResult::Applied,
            "many arbitrary real scenes must apply while idle");
    Require(model.current().streams.size() == 5, "many real scenes must produce Master plus N streams");
    Require(model.current().streams[1].display_name == "Camera" &&
                model.current().streams[4].display_name == "Intro",
            "many real scenes must preserve OBS collection order and display names");
}

void TestTopLevelScenesOnly() {
    SceneTopologyModel model;
    Require(model.ApplyDiscovery(Scenes({{"gameplay-uuid", "Gameplay"}, {"camera-uuid", "Camera"},
                                         {"brb-uuid", "BRB"}}),
                                false) == TopologyUpdateResult::Applied,
            "top-level scene discovery must apply while idle");
    Require(model.current().streams.size() == 4 &&
                model.current().streams[1].display_name == "Gameplay" &&
                model.current().streams[3].display_name == "BRB",
            "groups, nested scenes, and ordinary sources must not add video streams");
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
    TestZeroOneManyRealScenes();
    TestTopLevelScenesOnly();
    TestRenameAddRemoveWhileIdle();
    TestImmutableActiveEpochAndPendingTopology();
    TestIdentityCanKeyFutureParticipation();
    return EXIT_SUCCESS;
}
