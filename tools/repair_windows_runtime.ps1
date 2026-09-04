param([switch]$Quiet)
$ErrorActionPreference = 'SilentlyContinue'
$Root = Split-Path -Parent $PSScriptRoot
$Runtime = Join-Path $Root 'runtime\windows'
New-Item -ItemType Directory -Force -Path $Runtime | Out-Null
function Log([string]$m) { if(-not $Quiet){ Write-Host "[RUNTIME] $m" } }

$needVc = -not (Test-Path "$env:WINDIR\System32\vcruntime140.dll") -or -not (Test-Path "$env:WINDIR\System32\msvcp140.dll")
if(-not $needVc){
    Log 'Visual C++ x64 runtime principal parece instalado.'
    exit 0
}

$installer = Join-Path $Runtime 'vc_redist.x64.exe'
$url = 'https://aka.ms/vc14/vc_redist.x64.exe'
Log 'Visual C++ runtime x64 ausente. Baixando redistribuível oficial da Microsoft...'
try {
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $installer
} catch {
    Log "Falha no download: $($_.Exception.Message)"
    exit 2
}
$sig = Get-AuthenticodeSignature $installer
if($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'Microsoft'){
    Log 'Assinatura do VC Redist não foi validada como Microsoft; instalação cancelada.'
    Remove-Item -Force $installer
    exit 3
}
Log 'Instalando VC++ x64. O Windows pode solicitar permissão de administrador.'
try {
    $p = Start-Process -FilePath $installer -ArgumentList '/install','/quiet','/norestart' -Verb RunAs -PassThru -Wait
    Log "VC Redist encerrou com código $($p.ExitCode)."
    exit $p.ExitCode
} catch {
    Log "Falha ao instalar VC Redist: $($_.Exception.Message)"
    exit 4
}
