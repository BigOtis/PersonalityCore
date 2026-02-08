param(
  [string]$ProjectRoot = $(Resolve-Path (Join-Path $PSScriptRoot "..")),
  [string]$EngineRoot = "C:\Program Files\Epic Games\UE_5.7",
  [string]$PackageDir = "Packaged\LocalTalker_Build",
  [string]$HostProjectDir = "Saved\PackagedPluginHostProject",
  [string]$TestFilter = "Plugins.LocalTalker",
  [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

function Fail($msg) {
  Write-Host ""
  Write-Host "FAIL: $msg" -ForegroundColor Red
  exit 1
}

function Ok($msg) {
  Write-Host "OK: $msg" -ForegroundColor Green
}

$projectRootAbs = (Resolve-Path $ProjectRoot).Path
$packageAbs = Join-Path $projectRootAbs $PackageDir
$hostProjectRoot = Join-Path $projectRootAbs $HostProjectDir
$hostProjectPluginsDir = Join-Path $hostProjectRoot "Plugins"
$hostPluginDir = Join-Path $hostProjectPluginsDir "LocalTalker"

$pluginUPlugin = Join-Path $projectRootAbs "Plugins\LocalTalker\LocalTalker.uplugin"
$uat = Join-Path $EngineRoot "Engine\Build\BatchFiles\RunUAT.bat"
$editorCmd = Join-Path $EngineRoot "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"

if (!(Test-Path $pluginUPlugin)) { Fail "Plugin descriptor not found: $pluginUPlugin" }
if (!(Test-Path $uat)) { Fail "RunUAT not found: $uat" }
if (!(Test-Path $editorCmd)) { Fail "UnrealEditor-Cmd not found: $editorCmd" }

Write-Host "=== LocalTalker packaged test runner ==="
Write-Host "ProjectRoot : $projectRootAbs"
Write-Host "EngineRoot  : $EngineRoot"
Write-Host "PackageDir  : $packageAbs"
Write-Host "HostProject : $hostProjectRoot"
Write-Host "TestFilter  : $TestFilter"
Write-Host ""

if (!$SkipBuild) {
  Write-Host "[1/2] Building packaged plugin..."
  & $uat BuildPlugin `
    -Plugin="$pluginUPlugin" `
    -Package="$packageAbs" `
    -TargetPlatforms=Win64
  if ($LASTEXITCODE -ne 0) { Fail "BuildPlugin failed with exit code $LASTEXITCODE" }
  Ok "BuildPlugin succeeded"
}
else {
  Write-Host "[1/2] Skipping BuildPlugin as requested"
}

# BuildPlugin only outputs the plugin package. Create a stable throwaway host project
# that consumes that compiled package and run automation there.
if (Test-Path $hostPluginDir) {
  Remove-Item $hostPluginDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $hostProjectPluginsDir | Out-Null
Copy-Item -Path $packageAbs -Destination $hostPluginDir -Recurse -Force

if (!(Test-Path (Join-Path $hostPluginDir "LocalTalker.uplugin"))) {
  Fail "Packaged plugin copy is invalid: missing LocalTalker.uplugin in $hostPluginDir"
}

if (!(Test-Path $hostProjectRoot)) {
  New-Item -ItemType Directory -Path $hostProjectRoot -Force | Out-Null
}

$hostProject = Join-Path $hostProjectRoot "HostProject.uproject"
if (!(Test-Path $hostProject)) {
  $uprojectJson = @'
{
  "FileVersion": 3,
  "EngineAssociation": "5.7",
  "Category": "",
  "Description": "Temporary host project for packaged LocalTalker automation tests.",
  "Plugins": [
    {
      "Name": "LocalTalker",
      "Enabled": true
    }
  ]
}
'@
  Set-Content -Path $hostProject -Value $uprojectJson -Encoding UTF8
}

Write-Host "[2/2] Running automation tests against packaged HostProject..."
$execCmds = "Automation RunTests $TestFilter;Quit"
& $editorCmd `
  "$hostProject" `
  -NullRHI -Unattended -NoSplash -NoPause -NoSound `
  "-ExecCmds=$execCmds"

$exitCode = $LASTEXITCODE
if ($exitCode -ne 0) {
  $hostLog = Join-Path $packageAbs "HostProject\Saved\Logs\HostProject.log"
  if (Test-Path $hostLog) {
    Write-Host ""
    Write-Host "Last HostProject.log lines:" -ForegroundColor Yellow
    Get-Content $hostLog -Tail 80
  }
  Fail "Automation test run failed with exit code $exitCode"
}

Ok "Automation test run succeeded for filter '$TestFilter'"
