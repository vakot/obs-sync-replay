# Stock OBS Research Runtime Contract

This branch's stock-OBS experiment starts from a clean `obs-dev` portable runtime.
The reproducible entry point is:

```powershell
.\scripts\deploy-dev.ps1 -Configuration Debug
.\scripts\run-obs-research.ps1
```

Use `-SkipUpdateCheck` when the clean runtime should pass OBS's
`--disable-updater` startup option.

The product runtime installs plugin-owned Recording and Replay buttons into the
native OBS controls area and registers frontend hotkeys after loading, with no
automatic Recording, Replay, or encoder activation. The old scripted control
harness is no longer part of the product module; control behavior is validated by
the focused control/UI-state tests and by manual plugin interaction when desktop
automation is available.

`run-obs-research.ps1` refuses to modify a running portable OBS process. It clears
the runtime's `config` directory and the plugin data directory, then creates one
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

After the collection is active, the plugin uses public libobs APIs to create:

```text
Sync Research Scene A
`-- Sync Research Synthetic A (color_source, 1920x1080, opaque red)

Sync Research Scene B
`-- Sync Research Synthetic B (color_source, 1920x1080, opaque blue)
```

Stock OBS creates one empty localized placeholder scene when it initializes a new
scene collection. The plugin records that placeholder and item count, removes it only
when it is the sole empty scene, and then rechecks the namespace before construction.
Any additional scene or any item in the placeholder invalidates the run. The plugin
records the initial source/scene counts and every bootstrap API action in
the OBS log. It creates the plugin-owned control runtime idle only after the
environment has passed the fixed-video and zero-existing-source checks. Bootstrap is registered from
`obs_module_post_load` and runs on `OBS_FRONTEND_EVENT_FINISHED_LOADING`, after OBS
activates its empty collection and on the frontend thread required by stock scene
signals.

The clean-runtime preflight is necessary for video settings because OBS initializes
the root video output before third-party module callbacks and `obs_reset_video()`
rejects changes while that output is active. This is a generated setup step, not a
manual OBS configuration step.
