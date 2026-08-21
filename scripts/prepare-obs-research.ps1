[CmdletBinding()]
param(
  [string]$ObsDevRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'DevEnvironment.ps1')

$portableRoot = Resolve-ObsDevRoot -ConfiguredRoot $ObsDevRoot
$obsExecutable = Join-Path $portableRoot 'bin\64bit\obs64.exe'
$portableRootFull = [System.IO.Path]::GetFullPath($portableRoot).TrimEnd('\')

Get-CimInstance Win32_Process -Filter "Name = 'obs64.exe'" | ForEach-Object {
  if ($_.ExecutablePath) {
    $processPath = [System.IO.Path]::GetFullPath($_.ExecutablePath).TrimEnd('\')
    if ($processPath -ieq $obsExecutable) {
      throw "Cannot reset the research runtime while portable OBS is running (PID $($_.ProcessId)). Close that process normally first."
    }
  }
}

$configDirectory = Join-Path $portableRoot 'config'
if (Test-Path -LiteralPath $configDirectory) {
  Remove-Item -LiteralPath $configDirectory -Recurse -Force
  Write-Host "Reset clean runtime config: $configDirectory"
} else {
  Write-Host "Clean runtime config was already absent: $configDirectory"
}

$pluginStateDirectory = Join-Path $portableRoot 'data\obs-plugins\obs-sync-replay'
if (Test-Path -LiteralPath $pluginStateDirectory) {
  Remove-Item -LiteralPath $pluginStateDirectory -Recurse -Force
  Write-Host "Removed plugin-specific runtime state: $pluginStateDirectory"
} else {
  Write-Host "Plugin-specific runtime state was already absent: $pluginStateDirectory"
}

$profileName = 'Sync Replay Research'
$profileDirectory = Join-Path $portableRoot "config\obs-studio\basic\profiles\$profileName"
New-Item -ItemType Directory -Path $profileDirectory -Force | Out-Null

$userConfig = @"
[General]
FirstRun=true

[Basic]
Profile=$profileName
ProfileDir=$profileName
"@.TrimStart()

$profileConfig = @"
[General]
Name=$profileName

[Video]
BaseCX=1920
BaseCY=1080
OutputCX=1920
OutputCY=1080
FPSType=0
FPSCommon=60
ScaleType=bicubic
ColorFormat=NV12
ColorSpace=709
ColorRange=Partial
"@.TrimStart()

$userConfigPath = Join-Path $portableRoot 'config\obs-studio\user.ini'
$profileConfigPath = Join-Path $profileDirectory 'basic.ini'
[System.IO.File]::WriteAllText($userConfigPath, $userConfig, [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText($profileConfigPath, $profileConfig, [System.Text.UTF8Encoding]::new($false))

Write-Host "Created clean OBS user config: $userConfigPath"
Write-Host "Created research video profile: $profileConfigPath"
Write-Host "Research profile values: base/output=1920x1080 fps=60/1 format=NV12 colorspace=709 range=Partial"
Write-Host "No scene collection, recording output, replay buffer, or plugin state was created by this preflight."
Write-Host "Prepared portable OBS root: $portableRootFull"
