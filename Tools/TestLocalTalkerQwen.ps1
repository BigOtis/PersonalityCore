param(
  [string]$ProjectRoot = $(Resolve-Path "."),
  [string]$PythonExe = "python",
  [string]$QwenModel = "Qwen/Qwen3-TTS-12Hz-0.6B-CustomVoice",
  [string]$QwenTokenizer = "Qwen/Qwen3-TTS-Tokenizer-12Hz",
  [string]$QwenDevice = "cuda:0",
  [string]$QwenDType = "bfloat16",
  [switch]$SkipWorkerHealth,
  [switch]$SkipSynthesis,
  [string]$SynthText = "LocalTalker Qwen synthesis test line.",
  [string]$SynthSpeaker = "vivian"
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

$plugin = Join-Path $ProjectRoot "Plugins\\LocalTalker"
if (!(Test-Path $plugin)) { Fail "Plugin not found: $plugin" }

$llamaDir  = Join-Path $plugin "ThirdParty\\llama\\Win64\\Release"
$modelsDir = Join-Path $plugin "Resources\\Models"
$qwenDir   = Join-Path $plugin "Resources\\Qwen"

$libllama  = Join-Path $llamaDir "libllama.dll"
$ggmlBase  = Join-Path $llamaDir "ggml-base.dll"
$ggmlCpu   = Join-Path $llamaDir "ggml-cpu.dll"
$ggmlDll   = Join-Path $llamaDir "ggml.dll"
$model     = Join-Path $modelsDir "Llama-3.2-3B-Instruct-Q6_K_L.gguf"
$worker    = Join-Path $qwenDir "qwen_tts_worker.py"

Write-Host "=== LocalTalker Qwen install test (Win64) ==="
Write-Host "ProjectRoot: $ProjectRoot"
Write-Host ""

foreach ($f in @($libllama,$ggmlBase,$ggmlCpu,$ggmlDll,$worker)) {
  if (!(Test-Path $f)) { Fail "Missing required file: $f" }
  $len = (Get-Item $f).Length
  if ($len -le 0) { Fail "Empty file: $f" }
}
Ok "Required llama/Qwen worker files exist"

if (Test-Path $model) {
  Ok "Found default GGUF model: $model"
} else {
  Write-Host "WARN: Default GGUF model missing: $model" -ForegroundColor Yellow
}

$pythonCmd = Get-Command $PythonExe -ErrorAction SilentlyContinue
if (!$pythonCmd) {
  Fail "Python executable not found: $PythonExe"
}
Ok "Python found: $($pythonCmd.Source)"

if ($SkipWorkerHealth) {
  Write-Host "Skipping Qwen worker health check (-SkipWorkerHealth)."
  Ok "LocalTalker Qwen test completed"
  exit 0
}

# Start worker and send health + optional synth + shutdown over stdin.
$tmpIn = Join-Path $env:TEMP ("localtalker_qwen_req_{0}.txt" -f ([DateTimeOffset]::Now.ToUnixTimeSeconds()))
$outDir = Join-Path $ProjectRoot "Saved\LocalTalkerTest\qwen"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$outWav = Join-Path $outDir ("qwen_test_{0}.wav" -f ([DateTimeOffset]::Now.ToUnixTimeSeconds()))

$reqLines = @()
$reqLines += (@{ cmd = "health" } | ConvertTo-Json -Compress)
if (-not $SkipSynthesis) {
  $reqLines += (@{
      cmd = "synthesize"
      text = $SynthText
      language = "Auto"
      speaker = $SynthSpeaker
      output_wav = $outWav
      non_streaming_mode = $true
    } | ConvertTo-Json -Compress)
}
$reqLines += (@{ cmd = "shutdown" } | ConvertTo-Json -Compress)

[System.IO.File]::WriteAllText(
  $tmpIn,
  ($reqLines -join "`n"),
  (New-Object System.Text.UTF8Encoding($false))
)

$cmd = "type `"$tmpIn`" | `"$PythonExe`" `"$worker`" --model `"$QwenModel`" --tokenizer `"$QwenTokenizer`" --device `"$QwenDevice`" --dtype `"$QwenDType`" --no-flash-attn"
$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
  $out = (cmd.exe /c $cmd) 2>&1 | Out-String
} finally {
  $ErrorActionPreference = $prevEap
}

Remove-Item $tmpIn -Force -ErrorAction SilentlyContinue

$jsonLines = @()
foreach ($line in ($out -split "`r?`n")) {
  $trim = $line.Trim()
  if ($trim.StartsWith("{") -and $trim.EndsWith("}")) {
    try {
      $obj = $trim | ConvertFrom-Json -ErrorAction Stop
      if ($obj) { $jsonLines += $obj }
    } catch {
      # Ignore non-JSON/noisy lines.
    }
  }
}

if ($jsonLines.Count -lt 1) {
  Fail "No JSON responses received from Qwen worker. Output:`n$out"
}

if (-not $jsonLines[0].ok) {
  Fail "Qwen worker health check failed. Response: $($jsonLines[0] | ConvertTo-Json -Compress)`nOutput:`n$out"
}
Ok "Qwen worker health check passed"

if (-not $SkipSynthesis) {
  if ($jsonLines.Count -lt 2) {
    Fail "Missing synth response from Qwen worker. Output:`n$out"
  }

  $synth = $jsonLines[1]
  if (-not $synth.ok) {
    Fail "Qwen worker synth failed. Response: $($synth | ConvertTo-Json -Compress)`nOutput:`n$out"
  }

  $wavPath = [string]$synth.wav_path
  if ([string]::IsNullOrWhiteSpace($wavPath)) {
    # Fallback to requested path if worker omitted wav_path.
    $wavPath = $outWav
  }
  if (!(Test-Path $wavPath)) {
    Fail "Qwen synth reported success but WAV not found: $wavPath"
  }
  $wavSize = (Get-Item $wavPath).Length
  if ($wavSize -le 44) {
    Fail "Generated WAV is too small ($wavSize bytes): $wavPath"
  }

  Ok "Qwen synthesis generated WAV: $wavPath ($wavSize bytes)"
}

Ok "LocalTalker Qwen test completed"
