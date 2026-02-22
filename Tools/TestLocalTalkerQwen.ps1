param(
  [string]$ProjectRoot = $(Resolve-Path "."),
  [string]$PythonExe = "python",
  [string]$QwenModel = "Qwen/Qwen3-TTS-12Hz-0.6B-CustomVoice",
  [string]$QwenTokenizer = "Qwen/Qwen3-TTS-Tokenizer-12Hz",
  [string]$QwenDevice = "cuda:0",
  [string]$QwenDType = "bfloat16",
  [switch]$SkipWorkerHealth
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

# Start worker and send a health+shutdown command over stdin.
$tmpIn = Join-Path $env:TEMP ("localtalker_qwen_health_{0}.txt" -f ([DateTimeOffset]::Now.ToUnixTimeSeconds()))
$healthLines = @(
  '{"cmd":"health"}',
  '{"cmd":"shutdown"}'
)
Set-Content -Path $tmpIn -Value ($healthLines -join "`n") -Encoding UTF8

$cmd = "Get-Content -Raw '$tmpIn' | & '$PythonExe' '$worker' --model '$QwenModel' --tokenizer '$QwenTokenizer' --device '$QwenDevice' --dtype '$QwenDType' --no-flash-attn"
$out = powershell -NoProfile -Command $cmd 2>&1 | Out-String

Remove-Item $tmpIn -Force -ErrorAction SilentlyContinue

if ($out -notmatch '"ok"\s*:\s*true') {
  Fail "Qwen worker health check failed. Output:`n$out"
}

Ok "Qwen worker health check passed"
Ok "LocalTalker Qwen test completed"
