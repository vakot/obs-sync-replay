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
$portableRoot = Resolve-ObsDevRoot -ConfiguredRoot $ObsDevRoot

if ([string]::IsNullOrWhiteSpace($ArtifactPath)) {
  $presetName = "windows-$($Configuration.ToLowerInvariant())"
  $ArtifactPath = Join-Path $repositoryRoot "build\$presetName\$Configuration\obs-sync-replay.dll"
}

if (-not (Test-Path -LiteralPath $ArtifactPath -PathType Leaf)) {
  throw "Plugin build artifact does not exist: $ArtifactPath"
}

$pluginDirectory = Join-Path $portableRoot 'obs-plugins\64bit'
$dataDirectory = Join-Path $portableRoot 'data\obs-plugins\obs-sync-replay'
$sourceDataDirectory = Join-Path $repositoryRoot 'data'

New-Item -ItemType Directory -Path $pluginDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $dataDirectory -Force | Out-Null

$deployedPlugin = Join-Path $pluginDirectory 'obs-sync-replay.dll'
Copy-Item -LiteralPath $ArtifactPath -Destination $deployedPlugin -Force
Copy-Item -Path (Join-Path $sourceDataDirectory '*') -Destination $dataDirectory -Recurse -Force

Write-Host "Deployed plugin: $deployedPlugin"
Write-Host "Deployed data:   $dataDirectory"
Write-Host "Portable OBS:    $portableRoot"
