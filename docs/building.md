# Build and Development Environment

OBS Sync Replay is a Windows x64 native OBS module built with CMake and MSVC. The
product runtime discovers the active OBS scene collection through public libobs
APIs and owns the synchronized Recording/Replay controls. Capture output and
synchronization remain validated through the shared control engine and portable
runtime workflow.

## Pinned OBS Development Baseline

The development runtime and SDK baseline is OBS Studio 32.2.1. CMake downloads the
official `obs-studio` 32.2.1 source tag plus the official 2026-07-15 x64 `obs-deps`
bundle, verifies the pinned downloads, and builds the `libobs` development SDK. OBS's
Development install also requires its small `obs-frontend-api` target, but this plugin
links only `OBS::libobs`. The resulting local SDK lives under ignored `.deps/` and is
not committed.

This follows the official OBS plugin-template model of building against a pinned OBS
source tree and official prebuilt dependencies. It does not link against DLLs copied
from an installed OBS instance, and it does not require the portable instance to
contain headers or import libraries. The SDK version deliberately matches the local
runtime because OBS modules export their build-time OBS API version and incompatible
modules are rejected at load time.

References:

- [official OBS plugin template](https://github.com/obsproject/obs-plugintemplate);
- [OBS 32.2.1 module API](https://docs.obsproject.com/reference-modules);
- [OBS portable mode](https://obsproject.com/kb/portable-mode);
- [OBS Windows module search layout](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-windows.c).

## Prerequisites

- 64-bit Windows 11;
- Visual Studio 2022 17.14 or Build Tools with Desktop development with C++;
- a Windows 11 SDK (10.0.26100 is the validated local SDK);
- CMake 3.28 or newer on `PATH` or the CMake component bundled with Visual Studio;
- Git and PowerShell 7 or Windows PowerShell 5.1;
- network access during the first configure so pinned OBS inputs can be downloaded.

The committed generator is `Visual Studio 17 2022`, matching the current official
OBS plugin template. C++17 matches the public language level declared by libobs 32.2.1.

## First-Time Local Setup

The portable OBS path is machine-specific and must never be added to a committed
preset or script. Configure it with either the `OBS_DEV_ROOT` environment variable or
an ignored local file:

```powershell
Copy-Item scripts/dev-config.example.ps1 scripts/dev-config.ps1
notepad scripts/dev-config.ps1
```

The local file contains:

```powershell
$ObsDevRoot = 'C:\path\to\portable-obs'
```

The configured root must contain `bin\64bit\obs64.exe` and a `portable_mode` or
`portable_mode.txt` marker. Deployment and launch scripts reject a directory without
those markers, which prevents accidental use of a normal system installation.

## Configure, Build, and Test

Run these commands from a PowerShell session where `cmake` is on `PATH`:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

For Release:

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

The first configure downloads and prepares the pinned SDK in `.deps/`; later
configures reuse it. Generated project files and binaries are placed beneath
`build/windows-debug` and `build/windows-release`.

## Normal development and manual testing

Build, deploy, and launch the existing portable OBS state:

```powershell
.\scripts\build.ps1
.\scripts\start.ps1
```

`start.ps1` resolves the configured portable root, validates that the matching OBS
process is stopped, and launches OBS with `--portable --multi`. It does not select a
profile or write `user.ini`, `basic.ini`, settings, scene collections, or research
scenes. Use `-Wait` to wait for OBS exit and `-SkipUpdateCheck` to pass OBS's updater
option.

Prepare an arbitrary collection once in OBS, close it normally, then use the two
commands above. Verify that the selected profile and collection are preserved, the
plugin discovers Master plus every top-level scene, the UI appears, and Recording
and Replay remain idle with zero plugin encoders until explicitly started.

## Destructive research runtime

Use `research.ps1` only when a reproducible stock-OBS encoder experiment requires a
reset portable runtime:

```powershell
.\scripts\research.ps1 -SkipUpdateCheck
```

The deployment layout matches OBS's Windows module paths:

```text
<portable-root>\obs-plugins\64bit\obs-sync-replay.dll
<portable-root>\data\obs-plugins\obs-sync-replay\locale\en-US.ini
```

The build script copies only the plugin DLL and its data files. It reports every
destination and fails when the artifact, portable executable, or portable marker is
missing.

The research launcher refuses to reset a running portable OBS process, removes only
the configured runtime's `config` directory, writes the documented research profile,
and may create research-only fixtures through separate tooling. It is intentionally
destructive and must never be used for normal product startup.

The generated profile contains exactly these video values:

```text
BaseCX=1920       BaseCY=1080
OutputCX=1920     OutputCY=1080
FPSType=0        FPSCommon=60
ScaleType=bicubic ColorFormat=NV12
ColorSpace=709   ColorRange=Partial
```

The clean-runtime `user.ini` selects the generated profile and sets only
`General/FirstRun=true` to prevent first-run sources. OBS's normal startup then
activates its empty scene collection and initializes the video pipeline from the
generated profile. The plugin waits for the frontend finished-loading event and
discovers whatever real top-level scenes are present through `obs_enum_scenes()`. It
does not create test scenes or use scene names as identity.

For topology acceptance, create at least four ordinary scenes in the portable
collection after startup, or open a prepared collection, and use the
`[obs-sync-replay] topology:`
log records described in [`scene-topology.md`](scene-topology.md). The clean
launcher and plugin log every reset, profile creation, video check, discovery,
epoch snapshot, staged update, and coordinated shutdown. A run is invalid if
topology discovery fails, a source has no public UUID, the active epoch changes
participant membership, or a topology update starts an encoder while idle.

For a complete Debug iteration:

```powershell
.\scripts\build.ps1
.\scripts\research.ps1
```

The build script discovers the CMake executable bundled with Visual Studio when
`cmake` is not on `PATH`.

## Fast Iteration Loop

After first-time setup:

```text
edit
-> close only the portable OBS process
-> .\scripts\build.ps1
-> .\scripts\start.ps1 -SkipUpdateCheck
-> inspect the portable OBS log
```

The start workflow refuses to modify a running instance. Close the specific
portable instance normally before rebuilding or rerunning it; never terminate
every `obs64.exe` process.

Portable logs are under:

```text
<portable-root>\config\obs-studio\logs
```

A successful load contains:

```text
[obs-sync-replay] plugin loaded (version 0.1.0)
```

Normal shutdown contains the matching `plugin unloaded` entry.

## Visual Studio Debugging

1. Run `scripts/build.ps1`, then `scripts/start.ps1`; note the exact PID printed by the script.
2. In Visual Studio, select **Debug > Attach to Process**.
3. Select that PID and use the Native code debugger.
4. Add `build\windows-debug\Debug` to the symbol locations if the PDB is not found
   automatically.

This attaches to the portable process without committing a machine-specific Visual
Studio user file. The launcher uses the configured executable directly and passes
`--portable --multi`; it never launches the user's normal OBS installation.

## Formatting and Static Analysis

Repository rules are in `.clang-format`, `.clang-tidy`, and `.editorconfig`.

```powershell
clang-format --dry-run --Werror src/plugin/plugin-main.cpp
clang-format -i src/plugin/plugin-main.cpp
cmake --preset windows-debug -DCMAKE_CXX_CLANG_TIDY=clang-tidy
cmake --build --preset windows-debug
```

The baseline checks are deliberately conservative. CI enables compiler warnings as
errors for plugin code but does not treat warnings from the separately built OBS SDK
as plugin failures.

## CI Scope

`.github/workflows/ci.yml` runs the Release configure, build, and CTest preset on
`windows-2022` for pull requests to `master`. The current CTest smoke check verifies
that the expected plugin artifact exists and is non-empty. NVENC, runtime OBS, and
synchronization tests require suitable Windows/GPU environments and are not claimed
by this bootstrap CI job.
