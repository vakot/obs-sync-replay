[CmdletBinding()]
param(
  [string]$ObsDevRoot,
  [switch]$Wait,
  [switch]$SkipUpdateCheck
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'DevEnvironment.ps1')

& (Join-Path $PSScriptRoot 'prepare-obs-research.ps1') -ObsDevRoot $ObsDevRoot
$portableRoot = Resolve-ObsDevRoot -ConfiguredRoot $ObsDevRoot
$obsExecutable = Join-Path $portableRoot 'bin\64bit\obs64.exe'
$workingDirectory = Split-Path -Parent $obsExecutable
$profileName = 'Sync Replay Research'

$arguments = @('--portable', '--multi', '--profile', $profileName)
if ($SkipUpdateCheck) {
  $arguments += '--disable-updater'
}

$process = Start-Process -FilePath $obsExecutable -ArgumentList $arguments `
  -WorkingDirectory $workingDirectory -PassThru
Write-Host "Started clean stock OBS research runtime PID $($process.Id): $obsExecutable"
Write-Host "Expected runtime profile: $profileName"

if ($Wait) {
  $process.WaitForExit()
  Write-Host "Clean stock OBS research runtime PID $($process.Id) exited with code $($process.ExitCode)"
}

$process
