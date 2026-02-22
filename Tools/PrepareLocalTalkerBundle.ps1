param(
  [string]$ProjectRoot = $(Resolve-Path "."),
  [string]$LlamaCppLocalPath = "",
  [string]$DefaultGgufUrl = "",
  [string]$PythonExe = "python"
)

$ErrorActionPreference = "Stop"

function Ensure-Dir([string]$p) { if (!(Test-Path $p)) { New-Item -ItemType Directory -Path $p | Out-Null } }
function Download-If([string]$url, [string]$outFile) {
  if ([string]::IsNullOrWhiteSpace($url)) { return $false }
  Write-Host "Downloading $url -> $outFile"
  Invoke-WebRequest -Uri $url -OutFile $outFile
  return $true
}

function Find-CMake {
  $cmd = Get-Command cmake -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  return ""
}

$plugin = Join-Path $ProjectRoot "Plugins\\LocalTalker"
if (!(Test-Path $plugin)) { throw "LocalTalker plugin not found at: $plugin" }

$llamaInclude = Join-Path $plugin "ThirdParty\\llama\\include"
$llamaBinDir  = Join-Path $plugin "ThirdParty\\llama\\Win64\\Release"
$modelsDir    = Join-Path $plugin "Resources\\Models"
$voicesDir    = Join-Path $plugin "Resources\\Voices"
$qwenDir      = Join-Path $plugin "Resources\\Qwen"

Ensure-Dir $llamaInclude
Ensure-Dir $llamaBinDir
Ensure-Dir $modelsDir
Ensure-Dir $voicesDir
Ensure-Dir $qwenDir

Write-Host ""
Write-Host "=== LocalTalker bundle prep (llama + Qwen worker) ==="
Write-Host "Plugin: $plugin"
Write-Host ""

Write-Host "Step 1: llama.cpp headers"
Write-Host " - LocalTalker vendors llama/ggml headers in ThirdParty/llama/include/."
Write-Host ""

Write-Host "Step 2: libllama.dll (Win64)"
Write-Host " - Expecting: $llamaBinDir\\libllama.dll"
$cmake = Find-CMake
if ([string]::IsNullOrWhiteSpace($cmake)) {
  Write-Host " - CMake not found on PATH. Skipping llama.cpp build."
} else {
  $llamaSrc = $LlamaCppLocalPath
  if ([string]::IsNullOrWhiteSpace($llamaSrc)) {
    $llamaSrc = Join-Path $ProjectRoot "ThirdPartySrc\\llama.cpp"
  }

  if (!(Test-Path $llamaSrc)) {
    Write-Host " - llama.cpp source not found at: $llamaSrc"
    Write-Host "   Provide -LlamaCppLocalPath or clone to ThirdPartySrc\\llama.cpp"
  } else {
    $buildDir = Join-Path $llamaSrc "build-win64"
    Ensure-Dir $buildDir

    Write-Host " - Building llama.cpp from: $llamaSrc"
    Push-Location $buildDir
    & $cmake $llamaSrc -DBUILD_SHARED_LIBS=ON | Out-Host
    & $cmake --build . --config Release | Out-Host
    Pop-Location

    $candidates = @(
      Join-Path $buildDir "Release\\libllama.dll",
      Join-Path $buildDir "bin\\Release\\libllama.dll",
      Join-Path $buildDir "bin\\Release\\llama.dll",
      Join-Path $buildDir "Release\\llama.dll"
    )

    $found = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($found) {
      Copy-Item $found (Join-Path $llamaBinDir "libllama.dll") -Force
      Write-Host " - Copied: $found -> $llamaBinDir\\libllama.dll"
    } else {
      Write-Host " - Build completed but libllama.dll was not found in expected locations."
    }
  }
}
Write-Host ""

Write-Host "Step 3: Default GGUF (optional)"
Download-If $DefaultGgufUrl (Join-Path $modelsDir "Llama-3.2-3B-Instruct-Q6_K_L.gguf") | Out-Null
Write-Host ""

Write-Host "Step 4: Qwen worker dependencies"
$req = Join-Path $qwenDir "requirements-qwen-tts.txt"
if (Test-Path $req) {
  Write-Host " - Installing Python deps with: $PythonExe -m pip install -r $req"
  & $PythonExe -m pip install -r $req | Out-Host
} else {
  Write-Host " - Missing requirements file: $req"
}
Write-Host ""

Write-Host "Done."
Write-Host "Next: configure Project Settings -> LocalTalker -> DefaultPaths for Qwen model/tokenizer and run in-editor."
