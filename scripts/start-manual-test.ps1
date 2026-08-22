[CmdletBinding()]
param(
  [ValidateSet('Debug', 'Release')]
  [string]$Configuration = 'Debug',
  [string]$ObsDevRoot,
  [switch]$Wait,
  [switch]$SkipUpdateCheck
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'DevEnvironment.ps1')

$portableRoot = Resolve-ObsDevRoot -ConfiguredRoot $ObsDevRoot
$obsExecutable = [System.IO.Path]::GetFullPath((Join-Path $portableRoot 'bin\64bit\obs64.exe'))

$runningPortableProcesses = @(
  Get-CimInstance Win32_Process -Filter "Name = 'obs64.exe'" |
    Where-Object {
      if ([string]::IsNullOrWhiteSpace($_.ExecutablePath)) {
        return $false
      }

      [System.IO.Path]::GetFullPath($_.ExecutablePath).TrimEnd('\') -ieq $obsExecutable.TrimEnd('\')
    }
)

if ($runningPortableProcesses.Count -gt 0) {
  $pids = ($runningPortableProcesses | ForEach-Object { $_.ProcessId }) -join ', '
  throw "The configured portable OBS instance is already running (PID $pids). Close it normally before starting manual testing."
}

$buildArguments = @{
  Configuration = $Configuration
  ObsDevRoot = $ObsDevRoot
  NoLaunch = $true
}
if ($SkipUpdateCheck) {
  $buildArguments.SkipUpdateCheck = $true
}

Write-Host "Building and deploying $Configuration for manual testing..."
& (Join-Path $PSScriptRoot 'dev.ps1') @buildArguments
if ($LASTEXITCODE -ne 0) {
  throw "Build or deployment failed with exit code $LASTEXITCODE"
}

$launchArguments = @{
  ObsDevRoot = $portableRoot
  Wait = $Wait
}
if ($SkipUpdateCheck) {
  $launchArguments.SkipUpdateCheck = $true
}

& (Join-Path $PSScriptRoot 'run-obs-research.ps1') @launchArguments
Write-Host ''
Write-Host 'Manual testing instance started from a clean portable profile. Configure Replay Buffer in OBS Settings -> Output -> Replay Buffer.'
Write-Host "Portable OBS logs: $(Join-Path $portableRoot 'config\obs-studio\logs')"
