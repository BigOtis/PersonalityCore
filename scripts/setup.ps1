param([switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location -LiteralPath $projectRoot
if (!(Test-Path '.venv\Scripts\python.exe')) { py -3.11 -m venv .venv; if ($LASTEXITCODE) { throw 'Python 3.11 is required.' } }
& '.\.venv\Scripts\python.exe' -m pip install -e '.[dev]'
if ($LASTEXITCODE) { throw 'Python dependency installation failed.' }
Push-Location app
try {
    npm ci
    if ($LASTEXITCODE) { throw 'Node dependency installation failed. Use Node.js 22.12 or newer.' }
    & '.\node_modules\node\bin\node.exe' '.\node_modules\electron\install.js'
    if ($LASTEXITCODE) { throw 'Electron binary installation failed.' }
    npm run build
    if ($LASTEXITCODE) { throw 'Frontend build failed.' }
} finally { Pop-Location }
if (!$SkipTests) { & '.\.venv\Scripts\python.exe' -m pytest -q; if ($LASTEXITCODE) { throw 'Tests failed.' } }
Write-Host 'Ready. Double-click LocalTalker.cmd to launch the studio.'
