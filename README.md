# OBS Synchronized Multi-Scene Replay

This repository is preparing a Windows-first OBS Studio plugin that saves the
Master/Program output and every top-level OBS scene as separate MKV replay files on
one shared video timeline.
Frame-perfect pairing is the product: two independently started replay outputs are
not an acceptable architecture.

The supported platform is Windows x64 with OBS Studio 32.2.1. Start development
with the copyable configure, build, portable deployment, and debugging workflow in
[`docs/building.md`](docs/building.md).

## Project Documents

- [`AGENTS.md`](AGENTS.md): repository-wide implementation rules and definition of
  done;
- [`docs/architecture.md`](docs/architecture.md): timing model, conceptual modules,
  and synchronization boundaries;
- [`docs/testing.md`](docs/testing.md): required synchronization evidence and stress
  coverage;
- [`docs/building.md`](docs/building.md): Windows toolchain, build presets, portable
  OBS deployment, runtime validation, and debugging;
- [`docs/research-runtime.md`](docs/research-runtime.md): clean stock-OBS research
  runtime and real-scene topology acceptance workflow;
- [`docs/scene-topology.md`](docs/scene-topology.md): public OBS identity, ordering,
  ownership, and active-epoch lifecycle;
- [`docs/replay-configuration.md`](docs/replay-configuration.md): OBS Replay Buffer
  profile keys, availability policy, refresh lifecycle, and shared memory bound;
- [`docs/frontend-event-compatibility.md`](docs/frontend-event-compatibility.md):
  OBS frontend event, state-query, script, and output/path compatibility audit;
- [`docs/git-workflow.md`](docs/git-workflow.md): mandatory branch, commit, pull
  request, and squash-merge conventions.

## Central Constraint

```text
ONE MASTER VIDEO TIMELINE
|-- Master/Program -> separate program output frame N
`-- Scene 1..N -> separate scene outputs frame N
```

Every corresponding frame must carry the same master-frame identity and PTS. Replay
boundaries are chosen once for the complete Master + N stream set. See the
architecture and testing documents for the synchronization contract and acceptance
evidence.
