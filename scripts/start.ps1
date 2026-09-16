$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location -LiteralPath $projectRoot
if (!(Test-Path '.venv\Scripts\python.exe') -or !(Test-Path 'app\node_modules\electron')) {
    & "$PSScriptRoot\setup.ps1"
}
if (!(Test-Path 'app\dist\index.html')) {
    Push-Location app
    try { npm run build; if ($LASTEXITCODE) { throw 'Frontend build failed.' } } finally { Pop-Location }
}
$nodeCommand = if (Test-Path 'app\node_modules\node\bin\node.exe') { (Resolve-Path 'app\node_modules\node\bin\node.exe').Path } else { 'node' }
Remove-Item Env:ELECTRON_RUN_AS_NODE -ErrorAction SilentlyContinue
Push-Location app
try { & $nodeCommand 'node_modules\electron\cli.js' '.'; if ($LASTEXITCODE) { throw 'LocalTalker exited unexpectedly.' } } finally { Pop-Location }
