[CmdletBinding()]
param(
  [string]$ObsDevRoot,
  [switch]$Wait,
  [switch]$SkipUpdateCheck
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'DevEnvironment.ps1')

$portableRoot = Resolve-ObsDevRoot -ConfiguredRoot $ObsDevRoot
$process = Start-PortableObs -PortableRoot $portableRoot `
  -Wait:$Wait -SkipUpdateCheck:$SkipUpdateCheck
Write-Host 'Started existing portable OBS state without selecting or modifying a profile.'
$process
