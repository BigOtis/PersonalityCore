param(
  [string]$ProjectRoot = $(Resolve-Path "."),
  [string]$LlamaCppRepo = "https://github.com/ggml-org/llama.cpp",
  [string]$LlamaCppLocalPath = "",
  [string]$PiperReleaseZipUrl = "",
  [string]$DefaultGgufUrl = "",
  [string]$DefaultVoiceOnnxUrl = "",
  [string]$DefaultVoiceJsonUrl = ""
)

$ErrorActionPreference = "Stop"

function Ensure-Dir([string]$p) { if (!(Test-Path $p)) { New-Item -ItemType Directory -Path $p | Out-Null } }
function Download-If([string]$url, [string]$outFile) {
  if ([string]::IsNullOrWhiteSpace($url)) { return $false }
  Write-Host "Downloading $url -> $outFile"
  Invoke-WebRequest -Uri $url -OutFile $outFile
  return $true
}

$plugin = Join-Path $ProjectRoot "Plugins\\LocalTalker"
if (!(Test-Path $plugin)) { throw "LocalTalker plugin not found at: $plugin" }

# Expected bundle locations
$llamaInclude = Join-Path $plugin "ThirdParty\\llama\\include"
$llamaBinDir  = Join-Path $plugin "ThirdParty\\llama\\Win64\\Release"
$piperBinDir  = Join-Path $plugin "ThirdParty\\piper\\Win64\\Release"
$modelsDir    = Join-Path $plugin "Resources\\Models"
$voicesDir    = Join-Path $plugin "Resources\\Voices"
$noticesDir   = Join-Path $plugin "Resources\\ThirdPartyNotices"

Ensure-Dir $llamaInclude
Ensure-Dir $llamaBinDir
Ensure-Dir $piperBinDir
Ensure-Dir $modelsDir
Ensure-Dir $voicesDir
Ensure-Dir $noticesDir

Write-Host ""
Write-Host "=== LocalTalker bundle prep ==="
Write-Host "Plugin: $plugin"
Write-Host ""

#
# 1) llama.cpp headers (pinned by your own workflow)
#
Write-Host "Step 1: llama.cpp headers"
Write-Host " - LocalTalker vendors llama/ggml headers in ThirdParty/llama/include/"
Write-Host " - If you want to update llama.cpp, re-vendor headers from a pinned llama.cpp commit."
Write-Host ""

#
# 2) Build/Copy libllama.dll (Win64)
#
Write-Host "Step 2: libllama.dll (Win64)"
Write-Host " - Expecting: $llamaBinDir\\libllama.dll"
Write-Host " - This script can optionally build it if CMake is installed and a llama.cpp path is provided."
Write-Host "   If CMake is NOT installed, install it (or use Visual Studio's CMake) and rerun."

function Find-CMake {
  $cmd = Get-Command cmake -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  return ""
}

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
    Write-Host "   Provide -LlamaCppLocalPath, or clone to ThirdPartySrc\\llama.cpp"
  } else {
    $buildDir = Join-Path $llamaSrc "build-win64"
    Ensure-Dir $buildDir

    Write-Host " - Building llama.cpp from: $llamaSrc"
    Push-Location $buildDir
    & $cmake $llamaSrc -DBUILD_SHARED_LIBS=ON | Out-Host
    & $cmake --build . --config Release | Out-Host
    Pop-Location

    # Common output locations vary; try a few.
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
      Write-Host " - Build completed but libllama.dll wasn't found in expected locations."
      Write-Host "   Search in: $buildDir"
    }
  }
}
Write-Host ""

#
# 3) Piper binary (Win64)
#
Write-Host "Step 3: Piper (Win64)"
$piperZip = Join-Path $ProjectRoot "Saved\\LocalTalker\\piper.zip"
Ensure-Dir (Split-Path $piperZip)
if (Download-If $PiperReleaseZipUrl $piperZip) {
  Write-Host "Extracting Piper zip..."
  Expand-Archive -Path $piperZip -DestinationPath (Join-Path $ProjectRoot "Saved\\LocalTalker\\piper_extracted") -Force
  Write-Host "Copy piper.exe (and any required DLLs) into:"
  Write-Host " - $piperBinDir"
} else {
  Write-Host " - Skipped (no -PiperReleaseZipUrl provided)."
}
Write-Host ""

#
# 4) Default bundled model/voice
#
Write-Host "Step 4: Default model + voice"
Download-If $DefaultGgufUrl (Join-Path $modelsDir "tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf") | Out-Null
Download-If $DefaultVoiceOnnxUrl (Join-Path $voicesDir "en_US-lessac-small.onnx") | Out-Null
Download-If $DefaultVoiceJsonUrl (Join-Path $voicesDir "en_US-lessac-small.onnx.json") | Out-Null
Write-Host ""

Write-Host "Done."
Write-Host "Next: open UE, ensure LocalTalker is enabled, and call SendPromptAndSpeakStreamingInProc()."


