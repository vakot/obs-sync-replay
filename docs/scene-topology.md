# Scene Identity and Stream Topology

The product capture runtime derives its active stream set from the current OBS
scene collection. It does not create, select, or assume scenes named Scene A or
Scene B. The stream set is always:

```text
Master/Program
then every eligible real scene in obs_enum_scenes() collection order
```

## Identity

Master has the explicit identity `master` and is independent of OBS's current
program-scene selection. A real scene uses the public libobs source UUID returned
by `obs_source_get_uuid()`. The UUID is the internal key; the OBS source name is
display metadata only. Discovery also retains the source reference and records
the source's current display name.

OBS persists a scene source UUID in its scene-collection JSON. Therefore a rename,
scene-collection reload, or OBS restart preserves identity when the same scene
object is retained. Deleting a scene and creating a replacement produces a new
identity. A UUID is not a cross-collection identity contract: switching to another
collection can legitimately replace the complete topology.

`obs_enum_scenes()` is the public enumeration API used for production discovery.
Its callback order is preserved after Master, giving deterministic stream and
encoder ordering without depending on Qt widgets. Invalid entries without a
public UUID or name are rejected and logged.

Each topology entry carries stable identity and kind (`Master` or `Scene`), current
display name and collection order, recording and replay participation flags, and
an owned OBS source reference for scene rendering. Per-scene persistence and user
configuration are intentionally not implemented in this phase. The identity is
already the key on which those settings can be added without making display names
authoritative.

## Topology lifecycle

While idle, discovery changes replace the current topology. The runtime rebuilds
scene views and control resources, but does not start encoders merely because the
topology changed.

When Recording or Replay first activates, the runtime takes one immutable topology
snapshot for the capture epoch. Both consumers share that participant set. A rename
during an active epoch updates current metadata but does not restart an encoder or
change the active output participant; the active epoch retains its capture-time
metadata. Add, remove, or reorder changes are staged and become authoritative only
after both consumers are off.

For an active scene that is removed from the collection, the runtime retains its
owned source reference and finishes the current epoch with the original participant
set. The staged topology is installed after the epoch ends. If OBS destroys the
collection itself, the public `SCENE_COLLECTION_CLEANUP` lifecycle boundary causes
an explicit coordinated plugin shutdown before OBS destroys the owned resources;
the later collection-changed event starts a fresh idle runtime.

Every discovery, epoch snapshot, staged update, pending apply, and failure is
reported under `[topology]` with identity, display name, order, participation, and
epoch state. A failed rebuild is explicit; the runtime never silently substitutes
another scene or shifts a later frame into a removed scene's temporal slot.

## Runtime acceptance fixture

The clean portable research launcher intentionally creates no scenes. Before a
topology acceptance run, create at least four ordinary OBS scenes in the active
collection (or use the research-only `scripts/prepare-obs-topology-fixture.ps1`),
then inspect the plugin log for:

1. deterministic `Master/Program` plus all four scene UUIDs in collection order;
2. one unchanged topology after repeated idle polling;
3. exactly that stream set in the `capture-epoch-begin` snapshot;
4. rename continuity with the same UUID and no encoder restart;
5. an added scene staged during capture and applied after both consumers stop;
6. a removed active scene retained through the epoch and absent after pending apply.
