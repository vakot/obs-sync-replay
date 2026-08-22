[CmdletBinding()]
param(
  [ValidateSet('Debug', 'Release')]
  [string]$Configuration = 'Debug',
  [string]$ObsDevRoot,
  [switch]$NoLaunch,
  [switch]$SkipUpdateCheck
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'DevEnvironment.ps1')

$presetName = "windows-$($Configuration.ToLowerInvariant())"
$cmake = Resolve-CMakeExecutable
& $cmake --preset $presetName
if ($LASTEXITCODE -ne 0) {
  throw "CMake configure failed with exit code $LASTEXITCODE"
}

& $cmake --build --preset $presetName
if ($LASTEXITCODE -ne 0) {
  throw "CMake build failed with exit code $LASTEXITCODE"
}

& (Join-Path $PSScriptRoot 'deploy-dev.ps1') -Configuration $Configuration -ObsDevRoot $ObsDevRoot
if (-not $NoLaunch) {
  $launchArguments = @{ ObsDevRoot = $ObsDevRoot }
  if ($SkipUpdateCheck) {
    $launchArguments.SkipUpdateCheck = $true
  }
  & (Join-Path $PSScriptRoot 'run-obs-research.ps1') @launchArguments
}
