$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not (Test-Path .\.venv\Scripts\python.exe)) {
  py -3.11 -m venv .venv
}
$python = Join-Path $root ".venv\Scripts\python.exe"
Start-Process -FilePath $python -ArgumentList "-m","localtalker","serve" -WorkingDirectory $root
Set-Location (Join-Path $root "app")
if (-not (Test-Path node_modules)) { npm install }
npm run dev
