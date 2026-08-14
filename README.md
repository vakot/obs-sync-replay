# OBS Synchronized Multi-Scene Replay

This repository is preparing a Windows-first OBS Studio plugin that saves two
separately rendered scenes as two MKV replay files on one shared video timeline.
Frame-perfect pairing is the product: two independently started replay outputs are
not an acceptable architecture.

The project now has an initial Windows plugin scaffold. It builds and loads in OBS,
but deliberately implements no rendering, encoding, replay, synchronization, or UI
behavior yet. The only runtime behavior is identifiable load/unload logging.

The supported MVP platform is Windows x64 with OBS Studio 32.2.1. Start development
with the copyable configure, build, portable deployment, and debugging workflow in
[`docs/building.md`](docs/building.md).

## Project Documents

- [`mvp-plan.md`](mvp-plan.md): product scope, phases, and final acceptance model;
- [`AGENTS.md`](AGENTS.md): repository-wide implementation rules and definition of
  done;
- [`docs/architecture.md`](docs/architecture.md): timing model, conceptual modules,
  and synchronization boundaries;
- [`docs/testing.md`](docs/testing.md): required synchronization evidence and stress
  coverage;
- [`docs/building.md`](docs/building.md): Windows toolchain, build presets, portable
  OBS deployment, runtime validation, and debugging;
- [`docs/git-workflow.md`](docs/git-workflow.md): mandatory branch, commit, pull
  request, and squash-merge conventions.

## Central Constraint

```text
ONE MASTER VIDEO TIMELINE
|-- Scene A -> output A frame N
`-- Scene B -> output B frame N
```

Both frames must carry the same master-frame identity and PTS. Replay boundaries are
chosen once for the pair. See `mvp-plan.md` for the complete MVP definition.
