[CmdletBinding()]
param(
  [ValidateSet('Debug', 'Release')]
  [string]$Configuration = 'Debug',
  [string]$ObsDevRoot,
  [string]$ArtifactPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'DevEnvironment.ps1')

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$presetName = "windows-$($Configuration.ToLowerInvariant())"
$portableRoot = Resolve-ObsDevRoot -ConfiguredRoot $ObsDevRoot
Assert-PortableObsStopped -ObsExecutable (Join-Path $portableRoot 'bin\64bit\obs64.exe')
$cmake = Resolve-CMakeExecutable

& $cmake --preset $presetName
if ($LASTEXITCODE -ne 0) {
  throw "CMake configure failed with exit code $LASTEXITCODE"
}

& $cmake --build --preset $presetName
if ($LASTEXITCODE -ne 0) {
  throw "CMake build failed with exit code $LASTEXITCODE"
}

if ([string]::IsNullOrWhiteSpace($ArtifactPath)) {
  $ArtifactPath = Join-Path $repositoryRoot "build\$presetName\$Configuration\obs-sync-replay.dll"
}

$artifactPath = [System.IO.Path]::GetFullPath($ArtifactPath)
if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
  throw "Plugin build artifact does not exist: $artifactPath"
}

$pluginDirectory = Join-Path $portableRoot 'obs-plugins\64bit'
$dataDirectory = Join-Path $portableRoot 'data\obs-plugins\obs-sync-replay'
$sourceDataDirectory = Join-Path $repositoryRoot 'data'
New-Item -ItemType Directory -Path $pluginDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $dataDirectory -Force | Out-Null

$deployedPlugin = Join-Path $pluginDirectory 'obs-sync-replay.dll'
Copy-Item -LiteralPath $artifactPath -Destination $deployedPlugin -Force
Copy-Item -Path (Join-Path $sourceDataDirectory '*') -Destination $dataDirectory -Recurse -Force

Write-Host "Deployed plugin: $deployedPlugin"
Write-Host "Deployed data:   $dataDirectory"
Write-Host "Portable OBS:    $portableRoot"
Write-Host "Build and deployment complete for $Configuration."
