[CmdletBinding()]
param(
  [string]$ObsDevRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'DevEnvironment.ps1')

$portableRoot = Resolve-ObsDevRoot -ConfiguredRoot $ObsDevRoot
$obsExecutable = [System.IO.Path]::GetFullPath((Join-Path $portableRoot 'bin\64bit\obs64.exe'))
Get-CimInstance Win32_Process -Filter "Name = 'obs64.exe'" | ForEach-Object {
  if ($_.ExecutablePath -and ([System.IO.Path]::GetFullPath($_.ExecutablePath) -ieq $obsExecutable)) {
    throw "Close the configured portable OBS process before changing its scene fixture (PID $($_.ProcessId))."
  }
}

$sceneDirectory = Join-Path $portableRoot 'config\obs-studio\basic\scenes'
$scenePath = Get-ChildItem -LiteralPath $sceneDirectory -Filter '*.json' -File |
  Sort-Object LastWriteTime -Descending |
  Select-Object -First 1 -ExpandProperty FullName
if (-not $scenePath) {
  throw "No scene collection JSON found under $sceneDirectory. Start the portable runtime once first."
}

$document = Get-Content -LiteralPath $scenePath -Raw | ConvertFrom-Json
$sceneSources = @($document.sources | Where-Object { $_.id -eq 'scene' })
if ($sceneSources.Count -eq 0) {
  throw "Scene collection contains no real scene sources: $scenePath"
}

$fixtureScenes = [System.Collections.Generic.List[object]]::new()
foreach ($scene in $sceneSources) {
  $fixtureScenes.Add($scene)
}
while ($fixtureScenes.Count -lt 4) {
  $clone = ($sceneSources[($fixtureScenes.Count - 1) % $sceneSources.Count] | ConvertTo-Json -Depth 100 |
    ConvertFrom-Json)
  $clone.name = "Topology Fixture Scene $($fixtureScenes.Count + 1)"
  $clone.uuid = ([guid]::NewGuid()).ToString()
  $fixtureScenes.Add($clone)
}

$document.scene_order = @($fixtureScenes | ForEach-Object { [pscustomobject]@{ name = $_.name } })
$document.current_scene = $fixtureScenes[0].name
$document.current_program_scene = $fixtureScenes[0].name
$document.sources = @($document.sources) + @($fixtureScenes | Select-Object -Skip $sceneSources.Count)

$backupPath = "$scenePath.topology-backup.json"
Copy-Item -LiteralPath $scenePath -Destination $backupPath -Force
$json = $document | ConvertTo-Json -Depth 100
[System.IO.File]::WriteAllText($scenePath, $json, [System.Text.UTF8Encoding]::new($false))

Write-Host "Prepared four-scene topology fixture: $scenePath"
Write-Host "Backup: $backupPath"
$fixtureScenes | ForEach-Object { Write-Host "Scene: $($_.name) uuid=$($_.uuid)" }
