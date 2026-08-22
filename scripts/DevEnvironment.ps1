Set-StrictMode -Version Latest

function Resolve-CMakeExecutable {
  $cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
  if ($cmakeCommand) {
    return $cmakeCommand.Source
  }

  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
    $visualStudioRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
    if (-not [string]::IsNullOrWhiteSpace($visualStudioRoot)) {
      $bundledCMake = Join-Path $visualStudioRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
      if (Test-Path -LiteralPath $bundledCMake -PathType Leaf) {
        return $bundledCMake
      }
    }
  }

  throw 'CMake 3.28 or newer was not found on PATH or in Visual Studio.'
}

function Resolve-ObsDevRoot {
  param([string]$ConfiguredRoot)

  $candidate = $ConfiguredRoot
  if ([string]::IsNullOrWhiteSpace($candidate)) {
    $candidate = $env:OBS_DEV_ROOT
  }

  if ([string]::IsNullOrWhiteSpace($candidate)) {
    $localConfig = Join-Path $PSScriptRoot 'dev-config.ps1'
    if (Test-Path -LiteralPath $localConfig -PathType Leaf) {
      . $localConfig
      if (Get-Variable -Name ObsDevRoot -ErrorAction SilentlyContinue) {
        $candidate = $ObsDevRoot
      }
    }
  }

  if ([string]::IsNullOrWhiteSpace($candidate)) {
    throw 'Portable OBS is not configured. Set OBS_DEV_ROOT or copy scripts/dev-config.example.ps1 to scripts/dev-config.ps1.'
  }

  $resolvedRoot = (Resolve-Path -LiteralPath $candidate -ErrorAction Stop).Path
  $obsExecutable = Join-Path $resolvedRoot 'bin\64bit\obs64.exe'
  if (-not (Test-Path -LiteralPath $obsExecutable -PathType Leaf)) {
    throw "Configured OBS directory is invalid; missing $obsExecutable"
  }

  $portableMarker = Join-Path $resolvedRoot 'portable_mode'
  $portableTextMarker = Join-Path $resolvedRoot 'portable_mode.txt'
  if (-not (Test-Path -LiteralPath $portableMarker) -and -not (Test-Path -LiteralPath $portableTextMarker)) {
    throw "Configured OBS directory is not a portable instance; missing portable_mode marker in $resolvedRoot"
  }

  return $resolvedRoot
}

function Assert-PortableObsStopped {
  param([Parameter(Mandatory)][string]$ObsExecutable)

  $expectedPath = [System.IO.Path]::GetFullPath($ObsExecutable).TrimEnd('\')
  Get-CimInstance Win32_Process -Filter "Name = 'obs64.exe'" | ForEach-Object {
    if ([string]::IsNullOrWhiteSpace($_.ExecutablePath)) {
      return
    }

    $processPath = [System.IO.Path]::GetFullPath($_.ExecutablePath).TrimEnd('\')
    if ($processPath -ieq $expectedPath) {
      throw "Portable OBS is already running (PID $($_.ProcessId)). Close that instance normally first."
    }
  }
}

function Start-PortableObs {
  param(
    [Parameter(Mandatory)][string]$PortableRoot,
    [string]$ProfileName,
    [switch]$Wait,
    [switch]$SkipUpdateCheck
  )

  $obsExecutable = Join-Path $PortableRoot 'bin\64bit\obs64.exe'
  Assert-PortableObsStopped -ObsExecutable $obsExecutable

  $workingDirectory = Split-Path -Parent $obsExecutable
  $arguments = @('--portable', '--multi')
  if (-not [string]::IsNullOrWhiteSpace($ProfileName)) {
    $arguments += @('--profile', $ProfileName)
  }
  if ($SkipUpdateCheck) {
    $arguments += '--disable-updater'
  }

  $process = Start-Process -FilePath $obsExecutable -ArgumentList $arguments `
    -WorkingDirectory $workingDirectory -PassThru
  Write-Host "Started portable OBS PID $($process.Id): $obsExecutable"

  if ($Wait) {
    $process.WaitForExit()
    Write-Host "Portable OBS PID $($process.Id) exited with code $($process.ExitCode)"
  }

  return $process
}
