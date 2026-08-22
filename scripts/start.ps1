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
$profileName = 'Sync Replay Research'
$profileDirectory = Join-Path $portableRoot "config\obs-studio\basic\profiles\$profileName"
if (-not (Test-Path -LiteralPath $profileDirectory -PathType Container)) {
  throw "Manual-validation profile is not prepared. Run scripts/research.ps1 first."
}

$process = Start-PortableObs -PortableRoot $portableRoot -ProfileName $profileName `
  -Wait:$Wait -SkipUpdateCheck:$SkipUpdateCheck
Write-Host "Manual-validation profile: $profileName"
$process
