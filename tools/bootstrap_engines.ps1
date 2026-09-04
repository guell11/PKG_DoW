param([switch]$Quiet,[switch]$InstallPS4)
$ErrorActionPreference = 'SilentlyContinue'
$Root = Split-Path -Parent $PSScriptRoot
$Ps5Dir = Join-Path $Root 'engines\ps5'
$Ps4Dir = Join-Path $Root 'engines\ps4'
New-Item -ItemType Directory -Force -Path $Ps5Dir,$Ps4Dir | Out-Null

if($InstallPS4){
    & (Join-Path $PSScriptRoot 'install_ps4_runtime.ps1')
}

function Log([string]$m) { if(-not $Quiet){ Write-Host "[ENGINES] $m" } }

function Sync-InstallTree([string[]]$Candidates,[string]$TargetDir,[string]$ExeName) {
    foreach($dir in $Candidates){
        $exe = Join-Path $dir $ExeName
        if(Test-Path $exe -PathType Leaf){
            Log "sincronizando install tree: $dir -> $TargetDir"
            New-Item -ItemType Directory -Force -Path $TargetDir | Out-Null
            Copy-Item -Force -Recurse (Join-Path $dir '*') $TargetDir
            return (Join-Path $TargetDir $ExeName)
        }
    }
    return $null
}

function Install-FirstFile([string[]]$Candidates,[string]$Target) {
    if(Test-Path $Target -PathType Leaf){ return $Target }
    foreach($c in $Candidates){
        if(Test-Path $c -PathType Leaf){
            $resolved=(Resolve-Path $c).Path
            $targetResolved=[System.IO.Path]::GetFullPath($Target)
            if($resolved -ne $targetResolved){
                Log "AVISO: só encontrei executável isolado; dependências podem faltar: $resolved"
                Copy-Item -Force $resolved $Target
            }
            return $Target
        }
    }
    return $null
}

# Prefer a complete CMake install prefix because it carries every runtime file staged by the build.
$ps5 = Sync-InstallTree @(
    (Join-Path $Root '_Build\windows\install'),
    (Join-Path $Root '_Build\windows\install\bin')
) $Ps5Dir 'kyty_emulator.exe'

if(-not $ps5){
    $ps5 = Install-FirstFile @(
        (Join-Path $Root 'engines\ps5\kyty_emulator.exe'),
        (Join-Path $Root '_Build\windows\kyty_emulator.exe'),
        (Join-Path $Root 'bin\kyty_emulator.exe'),
        (Join-Path $Root 'kyty_emulator.exe')
    ) (Join-Path $Ps5Dir 'kyty_emulator.exe')
}

$ps4 = Install-FirstFile @(
    (Join-Path $Root 'engines\ps4\core-0.18.0\shadPS4.exe'),
    (Join-Path $Root 'engines\ps4\shadPS4.exe'),
    (Join-Path $Root 'engines\shadps4\shadPS4.exe'),
    (Join-Path $Root 'shadPS4.exe')
) (Join-Path $Ps4Dir 'shadPS4.exe')

if($ps5){ Log "PS5 core localizado: $ps5" } else { Log 'PS5 core ainda nao foi compilado; esperado em engines\ps5\kyty_emulator.exe' }
if($ps4){ Log "PS4 core localizado: $ps4" } else { Log 'PS4 core ausente; esperado em engines\ps4\shadPS4.exe' }
exit 0
