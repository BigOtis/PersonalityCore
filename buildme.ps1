param(
    [string]$ProjectRoot = $PSScriptRoot,
    [string]$ProjectFile = "AutoChat.uproject",
    [string]$Platform = "Win64",
    [string]$Configuration = "Development",
    [switch]$SkipKill
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Test-EngineRoot([string]$Root) {
    if ([string]::IsNullOrWhiteSpace($Root)) { return $false }
    $buildBat = Join-Path $Root "Engine\Build\BatchFiles\Build.bat"
    return (Test-Path $buildBat)
}

function Resolve-EngineRoot([string]$EngineAssociation) {
    if ($env:UE_ENGINE_ROOT -and (Test-EngineRoot $env:UE_ENGINE_ROOT)) {
        return $env:UE_ENGINE_ROOT
    }

    # Registry lookup for source/custom engine associations (GUID-like values).
    $buildsReg = "HKCU:\Software\Epic Games\Unreal Engine\Builds"
    if (Test-Path $buildsReg) {
        $reg = Get-ItemProperty -Path $buildsReg
        foreach ($p in $reg.PSObject.Properties) {
            if ($p.Name -like "PS*") { continue }
            if ($EngineAssociation -and $p.Name -eq $EngineAssociation -and (Test-EngineRoot ([string]$p.Value))) {
                return [string]$p.Value
            }
        }
    }

    $candidates = @()
    if ($EngineAssociation) {
        $candidates += (Join-Path ${env:ProgramFiles} "Epic Games\UE_$EngineAssociation")
        $candidates += (Join-Path ${env:ProgramFiles(x86)} "Epic Games\UE_$EngineAssociation")
    }

    foreach ($c in $candidates) {
        if (Test-EngineRoot $c) {
            return $c
        }
    }

    # Fallback: latest launcher install under Program Files.
    $launcherRoot = Join-Path ${env:ProgramFiles} "Epic Games"
    if (Test-Path $launcherRoot) {
        $installs = Get-ChildItem -Path $launcherRoot -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like "UE_*" } |
            Sort-Object Name -Descending
        foreach ($i in $installs) {
            if (Test-EngineRoot $i.FullName) {
                return $i.FullName
            }
        }
    }

    return $null
}

function Stop-UnrealEditors() {
    $names = @("UnrealEditor", "UE5Editor", "UE4Editor", "UnrealEditor-Cmd")
    $procs = Get-Process -Name $names -ErrorAction SilentlyContinue
    if ($null -eq $procs -or $procs.Count -eq 0) {
        Write-Host "[buildme] No running Unreal editor process found."
        return
    }

    $toStop = $procs | Select-Object -ExpandProperty ProcessName -Unique
    Write-Host "[buildme] Stopping Unreal process(es): $($toStop -join ', ')"
    $procs | Stop-Process -Force

    for ($i = 0; $i -lt 50; $i++) {
        Start-Sleep -Milliseconds 200
        $left = Get-Process -Name $names -ErrorAction SilentlyContinue
        if (!$left) { break }
    }
}

$uprojectPath = Join-Path $ProjectRoot $ProjectFile
if (!(Test-Path $uprojectPath)) {
    throw "Project file not found: $uprojectPath"
}

$uproject = Get-Content -Raw -Path $uprojectPath | ConvertFrom-Json
$engineAssociation = [string]$uproject.EngineAssociation
$engineRoot = Resolve-EngineRoot $engineAssociation
if ([string]::IsNullOrWhiteSpace($engineRoot)) {
    throw "Could not resolve Unreal Engine root for EngineAssociation '$engineAssociation'. Set UE_ENGINE_ROOT env var."
}

$buildBat = Join-Path $engineRoot "Engine\Build\BatchFiles\Build.bat"
$editorExe = Join-Path $engineRoot "Engine\Binaries\Win64\UnrealEditor.exe"
if (!(Test-Path $buildBat)) { throw "Build.bat not found: $buildBat" }
if (!(Test-Path $editorExe)) { throw "UnrealEditor.exe not found: $editorExe" }

$projectName = [System.IO.Path]::GetFileNameWithoutExtension($uprojectPath)
$target = "$projectName`Editor"

Write-Host "[buildme] Project: $uprojectPath"
Write-Host "[buildme] Engine:  $engineRoot"
Write-Host "[buildme] Target:  $target $Platform $Configuration"

if (-not $SkipKill) {
    Stop-UnrealEditors
}

& $buildBat $target $Platform $Configuration "-Project=$uprojectPath" -WaitMutex
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host "[buildme] Build succeeded. Launching editor..."
Start-Process -FilePath $editorExe -ArgumentList "`"$uprojectPath`""

