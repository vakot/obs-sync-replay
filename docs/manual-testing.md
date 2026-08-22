# Manual Testing

From the repository root, build and deploy the current plugin, then create the
clean manual-validation profile:

```powershell
.\scripts\build.ps1
.\scripts\research.ps1 -SkipUpdateCheck
```

`build.ps1` rebuilds and deploys the selected configuration. `research.ps1`
resets only the configured portable test runtime and starts OBS with the
plugin's two synthetic research scenes. It never starts stock OBS Replay Buffer.
Use `-Configuration Release` on `build.ps1` for a Release build.

After the research profile has been created, use this to start it again without
resetting manual settings:

```powershell
.\scripts\start.ps1 -SkipUpdateCheck
```

In OBS, configure the source-of-truth settings at **Settings -> Output -> Replay
Buffer**, then check that the plugin-owned Replay controls appear only when
**Enable Replay Buffer** is on. Enabling the setting must not start Replay
automatically; use the plugin's **Start Replay Buffer**, **Save Replay**, and
**Stop Replay** controls to exercise the flow. The Recording control should stay
visible when Replay Buffer is disabled.

The plugin-owned Recording and Replay toggle buttons show a small question-mark
help indicator inside their right edge. Hover the indicator in both the inactive
and active states and confirm that the localized tooltip identifies the plugin as
the owner of the synchronized capture action. Clicking elsewhere on the main
button must still toggle the corresponding action; clicking the indicator itself
only exposes help. Save Replay remains a separate compact icon button without
the plugin-owned indicator. Repeat the check at the current Windows display
scaling, including 175%, for clipping or overlap.

Portable logs are under the configured OBS root at
`config\obs-studio\logs`. `research.ps1` refuses to reset a running instance;
close it normally before rebuilding so the plugin DLL is not in use.
