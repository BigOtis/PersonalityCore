param([switch]$SkipBuild, [string]$Python)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location -LiteralPath $projectRoot
if (!$Python) { $Python = Join-Path $projectRoot '.venv/Scripts/python.exe' }
if (!(Test-Path 'app/node_modules/electron/dist/electron.exe')) {
    & 'app/node_modules/node/bin/node.exe' 'app/node_modules/electron/install.js'
    if ($LASTEXITCODE) { throw 'Electron binary installation failed.' }
}
if (!$SkipBuild) {
    Push-Location app
    try { npm run build; if ($LASTEXITCODE) { throw 'UI build failed.' } } finally { Pop-Location }
    New-Item -ItemType Directory -Path 'build' -Force | Out-Null
    $buildProcess = Start-Process -FilePath $Python -ArgumentList '-m','PyInstaller','--noconfirm','scripts/runtime.spec' -WorkingDirectory $projectRoot -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput 'build/package.stdout.log' -RedirectStandardError 'build/package.stderr.log'
    if ($buildProcess.ExitCode) { Get-Content 'build/package.stderr.log' -Tail 30; throw 'Runtime packaging failed. Install .[packaging] first.' }
}
$releaseRoot = Join-Path $projectRoot ('app\release\PersonalityCore-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $releaseRoot -Force | Out-Null
Copy-Item -Path 'app\node_modules\electron\dist\*' -Destination $releaseRoot -Recurse
Rename-Item -LiteralPath (Join-Path $releaseRoot 'electron.exe') -NewName 'PersonalityCore.exe'
$resources = Join-Path $releaseRoot 'resources'
$appTarget = Join-Path $resources 'app'
New-Item -ItemType Directory -Path $appTarget -Force | Out-Null
Copy-Item -LiteralPath 'app\electron' -Destination $appTarget -Recurse
@{name='localtalker'; productName='PersonalityCore'; version='0.2.0'; main='electron/main.cjs'} | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $appTarget 'package.json')
Copy-Item -LiteralPath 'dist\localtalker-runtime' -Destination (Join-Path $resources 'runtime') -Recurse
Copy-Item -LiteralPath 'README.md' -Destination $releaseRoot
@'
PERSONALITYCORE — CHARACTER STUDIO

Extract the entire folder and run PersonalityCore.exe.
Python and Node are included; no development setup is required.

Choose a local model in Inspect > setup. Model weights and inference servers
are separate downloads. Speech models download on first use.
Type a message or hold Mic / Space to speak. Escape / Interrupt stops a reply.
Use New group chat to try multiple characters.

Settings and conversations: %LOCALAPPDATA%\LocalTalker
Source, UE5 plugin, and engine guides: https://github.com/BigOtis/PersonalityCore
This unsigned early demo includes no game project or game assets.
'@ | Set-Content -Encoding UTF8 (Join-Path $releaseRoot 'START-HERE.txt')
Copy-Item -LiteralPath 'LICENSE' -Destination (Join-Path $releaseRoot 'PersonalityCore-LICENSE.txt')
& $Python "$PSScriptRoot/package_notices.py" $releaseRoot
if ($LASTEXITCODE) { throw 'Third-party notice collection failed.' }
& $Python "$PSScriptRoot/brand_executable.py" (Join-Path $releaseRoot 'PersonalityCore.exe')
if ($LASTEXITCODE) { throw 'Executable branding failed.' }
Write-Host "Portable Windows application: $releaseRoot\PersonalityCore.exe"
