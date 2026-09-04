param([switch]$WithPS4, [switch]$WithPS5, [switch]$Launch, [switch]$Force)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$RuntimeRoot = Join-Path $ProjectRoot '.runtime'
$VenvRoot = Join-Path $RuntimeRoot 'venv'
$PythonExe = Join-Path $VenvRoot 'Scripts\python.exe'

function Step([string]$Text) { Write-Host "`n  $Text" -ForegroundColor Cyan }
function Success([string]$Text) { Write-Host "  [OK] $Text" -ForegroundColor Green }
function Find-Python {
    foreach ($candidate in @('py.exe', 'python.exe')) {
        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if (-not $command) { continue }
        $args = if ($candidate -eq 'py.exe') { @('-3') } else { @() }
        & $command.Source @args -c 'import sys; raise SystemExit(0 if sys.version_info >= (3,10) else 1)' 2>$null
        if ($LASTEXITCODE -eq 0) { return @{ Exe = $command.Source; Args = $args } }
    }
    return $null
}

Clear-Host
Write-Host '  ==========================================' -ForegroundColor Magenta
Write-Host '              PKG_DoW SETUP' -ForegroundColor White
Write-Host '  ==========================================' -ForegroundColor Magenta
Write-Host '  Instalacao local. Sem jogos, firmware ou chaves.' -ForegroundColor DarkGray

New-Item -ItemType Directory -Force -Path $RuntimeRoot | Out-Null
$Python = Find-Python
if (-not $Python) {
    Step 'Python 3.10+ ausente'
    $Winget = Get-Command winget.exe -ErrorAction SilentlyContinue
    if (-not $Winget) { throw 'Instale Python 3.10+ e execute Setup.cmd novamente.' }
    Write-Host '  Baixando Python oficial via winget...'
    & $Winget.Source install --id Python.Python.3.12 --exact --accept-package-agreements --accept-source-agreements
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao instalar Python.' }
    $Python = Find-Python
    if (-not $Python) { throw 'Python instalado. Reabra Setup.cmd para continuar.' }
}
Success 'Python encontrado'

if ($Force -and (Test-Path -LiteralPath $VenvRoot)) { Remove-Item -LiteralPath $VenvRoot -Recurse -Force }
if (-not (Test-Path -LiteralPath $PythonExe -PathType Leaf)) {
    Step 'Criando ambiente isolado'
    & $Python.Exe @($Python.Args) -m venv $VenvRoot
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao criar ambiente Python.' }
}

Step 'Baixando interface e bibliotecas'
& $PythonExe -m pip install --disable-pip-version-check --upgrade pip
if ($LASTEXITCODE -ne 0) { throw 'Falha ao preparar pip.' }
& $PythonExe -m pip install --disable-pip-version-check -r (Join-Path $ProjectRoot 'requirements-native.txt')
if ($LASTEXITCODE -ne 0) { throw 'Falha ao instalar dependencias.' }
Success 'Interface pronta'

if (-not $PSBoundParameters.ContainsKey('WithPS4') -and -not $PSBoundParameters.ContainsKey('WithPS5')) {
    $answer = Read-Host '`n  Baixar core PS4 agora? [S/n]'
    $WithPS4 = ($answer -notmatch '^(n|nao|não)$')
    $answer = Read-Host '  Baixar core PS5 agora? [s/N]'
    $WithPS5 = ($answer -match '^(s|sim|y|yes)$')
}
if ($WithPS4) {
    Step 'Baixando cores PS4 oficiais'
    & (Join-Path $PSScriptRoot 'install_ps4_runtime.ps1') -Force:$Force
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao instalar core PS4.' }
}
if ($WithPS5) {
    Step 'Baixando core PS5 da release oficial'
    & (Join-Path $PSScriptRoot 'repair_ps5_engine.ps1') -Quiet
    if ($LASTEXITCODE -ne 0) { throw 'Release PS5 compativel nao encontrada.' }
}
New-Item -ItemType Directory -Force -Path (Join-Path $ProjectRoot 'logs'),(Join-Path $ProjectRoot 'userdata') | Out-Null
Success 'Instalacao concluida'
Write-Host "`n  Execute 1.bat para abrir." -ForegroundColor Yellow
if ($Launch) { & (Join-Path $ProjectRoot '1.bat') }
