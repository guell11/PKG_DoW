param([switch]$Quiet)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Target = Join-Path $Root 'engines\ps5'
$Work = Join-Path $Root 'runtime\engine_repair'
$BackupRoot = Join-Path $Root 'engines\backup'
New-Item -ItemType Directory -Force -Path $Target,$Work,$BackupRoot | Out-Null
function Log([string]$m) { if(-not $Quiet){ Write-Host "[ENGINE-REPAIR] $m" } }

try {
    $headers = @{ 'User-Agent' = 'PKG_DoW-HyperCore/2.4.1'; 'Accept'='application/vnd.github+json' }
    $release = Invoke-RestMethod -UseBasicParsing -Headers $headers -Uri 'https://api.github.com/repos/KytyPS5/KytyPS5/releases/latest'
    $assets = @($release.assets)
    $asset = $assets | Where-Object { $_.name -match '(?i)(windows|win64|x64)' -and $_.name -match '(?i)\.zip$' } | Select-Object -First 1
    if(-not $asset){ $asset = $assets | Where-Object { $_.name -match '(?i)\.zip$' } | Select-Object -First 1 }
    if(-not $asset){ throw 'Nenhum asset ZIP Windows encontrado na release atual.' }

    Log "Fallback oficial selecionado: $($asset.name) / tag $($release.tag_name)"
    $zip = Join-Path $Work $asset.name
    Remove-Item -Force $zip -ErrorAction SilentlyContinue
    Invoke-WebRequest -UseBasicParsing -Headers @{ 'User-Agent'='PKG_DoW-HyperCore/2.4.1' } -Uri $asset.browser_download_url -OutFile $zip

    $extract = Join-Path $Work 'extract'
    Remove-Item -Force -Recurse $extract -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $extract | Out-Null
    Expand-Archive -LiteralPath $zip -DestinationPath $extract -Force
    $exe = Get-ChildItem -Path $extract -Filter 'kyty_emulator.exe' -File -Recurse | Select-Object -First 1
    if(-not $exe){ throw 'O asset foi baixado mas não contém kyty_emulator.exe.' }

    # Release archives are expected to contain a staged install tree. Keep adjacent DLLs/plugins.
    $src = $exe.Directory.FullName
    $probe = Get-ChildItem -Path $src -File -ErrorAction SilentlyContinue
    Log "Install tree detectado em: $src"

    if(Test-Path (Join-Path $Target 'kyty_emulator.exe')){
        $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
        $backup = Join-Path $BackupRoot ("ps5_bad_" + $stamp)
        New-Item -ItemType Directory -Force -Path $backup | Out-Null
        Copy-Item -Force -Recurse (Join-Path $Target '*') $backup -ErrorAction SilentlyContinue
        Log "Core anterior preservado em: $backup"
    }
    Remove-Item -Force -Recurse (Join-Path $Target '*') -ErrorAction SilentlyContinue
    Copy-Item -Force -Recurse (Join-Path $src '*') $Target
    Set-Content -Encoding UTF8 -Path (Join-Path $Target 'FALLBACK_SOURCE.txt') -Value @(
      "source=KytyPS5 official GitHub release",
      "tag=$($release.tag_name)",
      "asset=$($asset.name)",
      "downloaded=$(Get-Date -Format o)",
      "reason=automatic recovery after Windows loader failure"
    )
    Log 'Fallback oficial instalado. O fonte HyperCore permanece no projeto; este binário serve para recuperar o runtime enquanto o fork local não estiver compilado.'
    exit 0
} catch {
    Log "Falha no fallback automático: $($_.Exception.Message)"
    exit 5
}
