param(
    [string]$OutputDir = $(Join-Path $PSScriptRoot "..\\Plugins\\LocalTalker\\Resources\\Voices")
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

$voices = @(
    @{ Id = "de_DE-thorsten-high"; RelPath = "de/de_DE/thorsten/high/de_DE-thorsten-high" },
    @{ Id = "en_US-lessac-high";  RelPath = "en/en_US/lessac/high/en_US-lessac-high"  },
    @{ Id = "en_US-libritts-high"; RelPath = "en/en_US/libritts/high/en_US-libritts-high" },
    @{ Id = "en_US-ljspeech-high"; RelPath = "en/en_US/ljspeech/high/en_US-ljspeech-high" },
    @{ Id = "en_US-ryan-high";    RelPath = "en/en_US/ryan/high/en_US-ryan-high"    },
    @{ Id = "en_GB-cori-high";    RelPath = "en/en_GB/cori/high/en_GB-cori-high"    },
    @{ Id = "es_AR-daniela-high"; RelPath = "es/es_AR/daniela/high/es_AR-daniela-high" },
    @{ Id = "es_MX-claude-high";  RelPath = "es/es_MX/claude/high/es_MX-claude-high"  },
    @{ Id = "kk_KZ-issai-high";   RelPath = "kk/kk_KZ/issai/high/kk_KZ-issai-high"   }
)

$base = "https://huggingface.co/rhasspy/piper-voices/resolve/main"
$curl = "curl.exe"
$extensions = @(".onnx", ".onnx.json")

foreach ($voice in $voices) {
    $allPresent = $true
    foreach ($ext in $extensions) {
        $url = "$base/$($voice.RelPath)$ext"
        $fileName = [System.IO.Path]::GetFileName($url)
        $outPath = Join-Path $OutputDir $fileName
        if (-not (Test-Path -LiteralPath $outPath)) {
            $allPresent = $false
            break
        }
    }

    if ($allPresent) {
        Write-Host "Skipping $($voice.Id) (already downloaded)"
        continue
    }

    foreach ($ext in $extensions) {
        $url = "$base/$($voice.RelPath)$ext"
        $fileName = [System.IO.Path]::GetFileName($url)
        $outPath = Join-Path $OutputDir $fileName
        if (Test-Path -LiteralPath $outPath) {
            Write-Host "Skipping $fileName (already exists)"
            continue
        }
        Write-Host "Downloading $url -> $outPath"
        & $curl -L --fail --retry 3 --retry-delay 1 -o $outPath $url
        if ($LASTEXITCODE -ne 0) {
            throw "curl failed for $url"
        }
    }
}

Write-Host "Done. Voices downloaded to: $OutputDir"
