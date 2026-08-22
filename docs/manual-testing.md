# Manual Testing

From the repository root, run:

```powershell
.\scripts\start-manual-test.ps1 -SkipUpdateCheck
```

The script builds and deploys the Debug plugin, resets only the configured
portable test runtime, and starts OBS with the plugin's two synthetic research
scenes. It never starts stock OBS Replay Buffer and never stops an existing
process. Close the portable OBS window normally before running it again. Use
`-Configuration Release` for a Release build.

In OBS, configure the source-of-truth settings at **Settings -> Output -> Replay
Buffer**, then check that the plugin-owned Replay controls appear only when
**Enable Replay Buffer** is on. Enabling the setting must not start Replay
automatically; use the plugin's **Start Replay Buffer**, **Save Replay**, and
**Stop Replay** controls to exercise the flow. The Recording control should stay
visible when Replay Buffer is disabled.

Portable logs are under the configured OBS root at
`config\obs-studio\logs`. The launcher refuses to reset a running instance;
close it normally before rebuilding so the plugin DLL is not in use.
