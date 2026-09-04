param([switch]$Force)

$ErrorActionPreference = 'Stop'
$WorkspaceRoot = Split-Path -Parent $PSScriptRoot
$Ps4Root = Join-Path $WorkspaceRoot 'engines\ps4'
$CoreDir = Join-Path $Ps4Root 'core-0.18.0'
$InstallerDir = Join-Path $Ps4Root 'pkg-installer-0.7.0'
$CoreExe = Join-Path $CoreDir 'shadPS4.exe'
$InstallerExe = Join-Path $InstallerDir 'shadPS4.exe'

$CoreUrl = 'https://github.com/shadps4-emu/shadPS4/releases/download/v.0.18.0/shadps4-win64-sdl-0.18.0.zip'
$InstallerUrl = 'https://github.com/shadps4-emu/shadPS4/releases/download/v.0.7.0/shadps4-win64-qt-0.7.0.zip'
$CoreSha256 = 'ea273c6a2b296d4cee2e3dc45414fa9077f5fb43679784d1c43eee97a484fcc8'
$InstallerSha256 = 'e4176f6e5181db581cf7a1c714400d6cb38f2b92e80094e296ff7b04722290a6'

function Install-OfficialZip([string]$Url, [string]$Sha256, [string]$Destination, [string]$ExpectedExe) {
    if ((Test-Path -LiteralPath $ExpectedExe -PathType Leaf) -and -not $Force) {
        Write-Host "[PS4] pronto: $ExpectedExe"
        return
    }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    $ZipPath = Join-Path $env:TEMP ("pkg-dow-" + [guid]::NewGuid().ToString('N') + '.zip')
    try {
        Write-Host "[PS4] baixando: $Url"
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $ZipPath
        $Actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $ZipPath).Hash.ToLowerInvariant()
        if ($Actual -ne $Sha256) {
            throw "SHA256 invalido: esperado=$Sha256 obtido=$Actual"
        }
        Expand-Archive -LiteralPath $ZipPath -DestinationPath $Destination -Force
        if (-not (Test-Path -LiteralPath $ExpectedExe -PathType Leaf)) {
            throw "Executavel ausente apos extracao: $ExpectedExe"
        }
    } finally {
        Remove-Item -LiteralPath $ZipPath -Force -ErrorAction SilentlyContinue
    }
}

Install-OfficialZip $CoreUrl $CoreSha256 $CoreDir $CoreExe
Install-OfficialZip $InstallerUrl $InstallerSha256 $InstallerDir $InstallerExe
New-Item -ItemType Directory -Force -Path (Join-Path $CoreDir 'user'),(Join-Path $InstallerDir 'user'),(Join-Path $WorkspaceRoot 'userdata\ps4-games') | Out-Null

Write-Host '[PS4] core 0.18.0: pronto'
Write-Host '[PS4] instalador PKG 0.7.0: pronto'

