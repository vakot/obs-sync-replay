[CmdletBinding()]
param(
  [string]$ObsDevRoot,
  [switch]$Wait
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'DevEnvironment.ps1')

$portableRoot = Resolve-ObsDevRoot -ConfiguredRoot $ObsDevRoot
$obsExecutable = Join-Path $portableRoot 'bin\64bit\obs64.exe'
$workingDirectory = Split-Path -Parent $obsExecutable

$process = Start-Process -FilePath $obsExecutable -ArgumentList @('--portable', '--multi') -WorkingDirectory $workingDirectory -PassThru
Write-Host "Started portable OBS PID $($process.Id): $obsExecutable"

if ($Wait) {
  $process.WaitForExit()
  Write-Host "Portable OBS PID $($process.Id) exited with code $($process.ExitCode)"
}

$process
