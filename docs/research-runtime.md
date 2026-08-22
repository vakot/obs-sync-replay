# Stock OBS Research Runtime Contract

This branch's stock-OBS experiment starts from a clean `obs-dev` portable runtime.
The reproducible entry point is:

```powershell
.\scripts\build.ps1
.\scripts\research.ps1
```

Use `-SkipUpdateCheck` when the clean runtime should pass OBS's
`--disable-updater` startup option.

The product runtime installs plugin-owned Recording and Replay buttons into the
native OBS controls area and registers frontend hotkeys after loading, with no
automatic Recording, Replay, or encoder activation. The old scripted control
harness is no longer part of the product module; control behavior is validated by
the focused control/UI-state tests and by manual plugin interaction when desktop
automation is available.

`research.ps1` refuses to modify a running portable OBS process. It clears
the runtime's `config` directory while preserving the deployed plugin data and locale
files, then creates one
temporary profile named `Sync Replay Research`. The profile is not an input to the
experiment and is regenerated on every run.

The only profile values written before OBS startup are:

```ini
[Video]
BaseCX=1920
BaseCY=1080
OutputCX=1920
OutputCY=1080
FPSType=0
FPSCommon=60
ScaleType=bicubic
ColorFormat=NV12
ColorSpace=709
ColorRange=Partial
```

The clean `user.ini` selects this profile and contains only `General/FirstRun=true`
besides the profile selector. This suppresses first-run sources. No scene collection
JSON, recording output, replay buffer, encoder, or plugin-specific state is supplied.
Stock OBS creates its empty collection and initializes the root video pipeline from
this profile.

The plugin does not create a deterministic test scene. After the collection is
active, create at least four ordinary scenes in OBS for a topology acceptance run,
open a prepared collection, or use the research-only
`scripts/prepare-obs-topology-fixture.ps1` while OBS is closed. The plugin discovers all real scenes with public
`obs_enum_scenes()`, obtains each public source UUID with
`obs_source_get_uuid()`, retains the source reference, and preserves callback order.
Master is added as the explicit first stream.

The plugin records every topology entry and epoch transition in the OBS log. Use
the sequence in [`scene-topology.md`](scene-topology.md) to verify idle discovery,
rename continuity, active add/remove staging, and pending apply after both
consumers stop. A collection cleanup is an explicit coordinated shutdown boundary;
the following collection-changed event starts a new idle runtime.

The clean-runtime preflight is necessary for video settings because OBS initializes
the root video output before third-party module callbacks and `obs_reset_video()`
rejects changes while that output is active. This is a generated setup step, not a
manual OBS configuration step.
